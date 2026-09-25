#!/usr/bin/env python3
"""Sample every FreeRTOS task's stack over JTAG and fold it for a flamegraph.

The pprof-shaped view for espix, with nothing added to the build: GDB attaches
to OpenOCD's server, dumps each task's backtrace, detaches, and repeats. The
backtraces unwind with DWARF, so CONFIG_ESP_SYSTEM_USE_FRAME_POINTER is not
needed (it costs ~152 kB of code and does not fit here).

    tools/jtag-profile.py -n 40 -i 0.4
    tools/jtag-profile.py --flamegraph          # if flamegraph.pl is on PATH

Writes profile.folded: one line per sample, "a;b;c 1", the standard flamegraph
input, plus a top-functions summary on stdout so it is useful on its own.

Needs the board's USB-JTAG connected, and the OpenOCD that knows esp32s31
(20260703 in the tool tree; the IDF-pinned 20260424 has no S31 target).

A sample halts the target while GDB reads it. The normal path detaches (which
resumes), but killing OpenOCD mid-sample leaves the board frozen -- there is no
console output and no hint why. Resume it with:

    openocd -f .../board/esp32s31-builtin.cfg -c 'init' -c 'reset run' -c shutdown

Note too that only the task(s) on a core are recorded; OpenOCD's S31 RTOS
support lists every task but gives blocked ones a halt-artifact leaf frame, so
their stacks are excluded rather than allowed to swamp the histogram.
"""
import argparse
import glob
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from collections import Counter

ESP = os.path.expanduser("~/.espressif/tools")
FRAME = re.compile(r"^#(\d+)\s+(?:0x[0-9a-f]+ in )?([^ (]+)")


def find_openocd():
    """The newest OpenOCD whose scripts carry an esp32s31 target."""
    best = None
    for ocd in sorted(glob.glob(f"{ESP}/openocd-esp32/*/openocd-esp32")):
        if os.path.exists(f"{ocd}/share/openocd/scripts/target/esp32s31.cfg"):
            best = ocd  # sorted, so the last is the newest version
    if best is None:
        sys.exit("no OpenOCD with an esp32s31 target under " + ESP)
    return best


def find_gdb():
    hits = sorted(glob.glob(
        f"{ESP}/riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb"))
    if not hits:
        sys.exit("no riscv32-esp-elf-gdb under " + ESP)
    return hits[-1]


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


def start_openocd(ocd, port, log):
    cfg = f"{ocd}/share/openocd/scripts/board/esp32s31-builtin.cfg"
    if not os.path.exists(cfg):
        sys.exit("no esp32s31-builtin.cfg under " + ocd)
    # 'init' explicitly: without it the profile/attach happens before the target
    # is examined and the core cannot be identified.
    proc = subprocess.Popen([f"{ocd}/bin/openocd", "-f", cfg, "-c", "init"],
                            stdout=open(log, "w"), stderr=subprocess.STDOUT)
    for _ in range(50):
        if port_open(port):
            return proc
        time.sleep(0.2)
    proc.kill()
    sys.exit("OpenOCD did not open gdb port " + str(port) + "; see " + log)


THREAD_HDR = re.compile(r"Thread (\d+) \(")
RUNNING = re.compile(r"\s*\*?\s*(\d+)\s+Thread .*State: Running")


def sample(gdb, elf, port):
    """One attach -> all backtraces -> detach. Returns the running stacks.

    Only the task(s) actually on a core are recorded. A halted sample cannot
    weight a blocked task, and every blocked task saves the same block frame
    (vPortExitCriticalMultiCore -> vPortClearInterruptMaskFromISR), which would
    otherwise swamp the histogram with a frame that says nothing. Sampling only
    what is running -- once or twice per sample -- still yields each task's
    share across many samples, and leaves the interesting code visible.
    """
    out = subprocess.run(
        [gdb, "-q", "-batch",
         "-ex", f"target remote :{port}",
         "-ex", "info threads",
         "-ex", "thread apply all bt",
         "-ex", "detach",
         elf],
        capture_output=True, text=True, timeout=60).stdout

    running = {m.group(1) for m in (RUNNING.match(l) for l in out.splitlines()) if m}

    stacks, tid, cur = [], None, None
    for line in out.splitlines():
        hm = THREAD_HDR.match(line)
        if hm:
            if tid is not None and cur:
                stacks.append((tid, cur))
            tid, cur = hm.group(1), []
            continue
        m = FRAME.match(line.strip())
        if m and cur is not None:
            cur.append(m.group(2))
    if tid is not None and cur:
        stacks.append((tid, cur))

    if not running:
        return [s for _, s in stacks]
    return [s for t, s in stacks if t in running]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--samples", type=int, default=30)
    ap.add_argument("-i", "--interval", type=float, default=0.4,
                    help="seconds between samples (attach/detach is ~0.5s anyway)")
    ap.add_argument("--elf", default=None)
    ap.add_argument("--out", default="profile.folded")
    ap.add_argument("--port", type=int, default=3333)
    ap.add_argument("--flamegraph", action="store_true",
                    help="render profile.svg too, if flamegraph.pl is available")
    args = ap.parse_args()

    elf = args.elf
    if elf is None:
        # Prefer the current S31 build; build-s31 and others are scratch trees.
        for cand in ("build-esp32s31-maint/espix.elf",
                     "build-esp32s31/espix.elf"):
            if os.path.exists(cand):
                elf = cand
                break
        else:
            hits = sorted(glob.glob("build-*/espix.elf"))
            if not hits:
                sys.exit("no build-*/espix.elf; pass --elf")
            elf = hits[-1]
    gdb, ocd = find_gdb(), find_openocd()
    log = "/tmp/espix-jtag-openocd.log"

    proc = None
    if not port_open(args.port):
        proc = start_openocd(ocd, args.port, log)
    print(f"elf {elf}")
    print(f"openocd {os.path.basename(os.path.dirname(ocd))}, gdb {os.path.basename(os.path.dirname(os.path.dirname(gdb)))}")

    folded = Counter()
    leaves = Counter()
    try:
        for i in range(args.samples):
            for stack in sample(gdb, elf, args.port):
                if not stack:
                    continue
                # GDB prints leaf first; a flamegraph wants root first.
                root_first = list(reversed(stack))[:40]
                folded[";".join(root_first)] += 1
                leaves[root_first[-1]] += 1
            print(f"\r{i + 1}/{args.samples} samples", end="", file=sys.stderr)
            time.sleep(args.interval)
    finally:
        print(file=sys.stderr)
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    with open(args.out, "w") as f:
        for stack, count in folded.most_common():
            f.write(f"{stack} {count}\n")

    total = sum(folded.values())
    print(f"\n{total} stacks in {args.out}\n")
    print("what the samples landed in (leaf frame):")
    for fn, count in leaves.most_common(20):
        print(f"  {count * 100 // max(total, 1):3d}%  {fn}")

    if args.flamegraph:
        fg = shutil.which("flamegraph.pl")
        if not fg:
            print("\nflamegraph.pl not on PATH; " + args.out + " is standard input for it")
        else:
            svg = args.out.rsplit(".", 1)[0] + ".svg"
            with open(svg, "w") as out:
                subprocess.run([fg, args.out], stdout=out, check=True)
            print("wrote " + svg)


if __name__ == "__main__":
    main()
