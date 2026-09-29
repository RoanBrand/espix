#!/usr/bin/env python3
"""Capture a SEGGER SystemView trace over JTAG from a PROFILE=sysview build.

OpenOCD's 'esp sysview' command reads the SystemView data the
espressif/esp_sysview encoder has written into the target's apptrace buffer,
over the JTAG TAP. Nothing is halted: the target keeps running, which is what
makes this the one probe that can watch the VNC path, or a Bluetooth stream,
while it is doing work. tools/jtag-profile.py cannot, because it stops the core
to read it.

Prerequisite: the board is running a 'make PROFILE=sysview' build and the
USB-JTAG port is connected. Then:

    tools/sysview.py -s 20            # 20s -> sysview-pro.SVDat, sysview-app.SVDat
    tools/sysview.py -s 20 --mcore    # one file, SEGGER SystemView 3.60+
    tools/sysview.py --open           # launch SEGGER SystemView on the result

Open the file in SEGGER SystemView and replace its
'Description/SYSVIEW_FreeRTOS.txt' with
'$IDF_PATH/tools/esp_app_trace/SYSVIEW_FreeRTOS.txt' -- the one setup step that
is easy to miss and leaves the trace looking empty.

Only OpenOCD v0.12.0-esp32-20260703 knows the esp32s31 target; the IDF-pinned
20260424 does not, so this finds the former by the same rule as
tools/jtag-profile.py -- and this is the newest one, since 20260831 is also
installed.

**This captures nothing on an S31 yet.** Both 20260703 and 20260831 fail with
"Failed to get max trace block size!" / "Failed to init cmd ctx (-4)!": the
apptrace control block comes from the target's semihosting parameter and never
resolves for esp32s31. There is no CONFIG_ for it. The working route is the
USB-Serial/JTAG transport that PROFILE=sysview builds, read directly by SEGGER
SystemView; this tool is here for when OpenOCD gains S31 support. See
docs/PROFILING.md.

Recording does not stop the target -- the trace is buffered on-chip and read
over JTAG while the application runs -- so --seconds is only how long to record,
not a stop condition. The target is resumed again on the way out.
"""
import argparse
import glob
import os
import socket
import subprocess
import sys
import time

ESP = os.path.expanduser("~/.espressif/tools")


def find_openocd():
    """The newest OpenOCD whose scripts carry an esp32s31 target."""
    best = None
    for ocd in sorted(glob.glob(f"{ESP}/openocd-esp32/*/openocd-esp32")):
        if os.path.exists(f"{ocd}/share/openocd/scripts/target/esp32s31.cfg"):
            best = ocd  # sorted, so the last is the newest version
    if best is None:
        sys.exit("no OpenOCD with an esp32s31 target under " + ESP)
    return best


def port_open(port, timeout=0.3):
    with socket.socket() as s:
        s.settimeout(timeout)
        return s.connect_ex(("127.0.0.1", port)) == 0


def telnet(port, command, wait=0.5):
    """Send one command to OpenOCD's telnet console and collect the reply.

    A fresh connection per command: OpenOCD's console is line-oriented, and
    'esp sysview' holds whatever state matters on its own.
    """
    with socket.create_connection(("127.0.0.1", port), 5) as s:
        s.settimeout(wait)
        out = b""
        for _ in range(2):  # drain the banner and prompt
            try:
                out += s.recv(4096)
            except socket.timeout:
                break
        s.sendall(command.encode() + b"\n")
        time.sleep(wait)
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                out += chunk
        except socket.timeout:
            pass
    return out.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--seconds", type=float, default=10.0,
                    help="how long to record (default 10)")
    ap.add_argument("--out", default="sysview",
                    help="output prefix; .SVDat files are written beside it")
    ap.add_argument("--mcore", action="store_true",
                    help="one multi-core file (SEGGER SystemView 3.60+)")
    ap.add_argument("--open", action="store_true",
                    help="open the trace in SEGGER SystemView when done")
    ap.add_argument("--port", type=int, default=4444,
                    help="OpenOCD telnet port (default 4444)")
    args = ap.parse_args()

    ocd = find_openocd()
    log = "/tmp/espix-sysview-openocd.log"
    proc = subprocess.Popen(
        [f"{ocd}/bin/openocd", "-f",
         f"{ocd}/share/openocd/scripts/board/esp32s31-builtin.cfg",
         "-c", "init"],
        stdout=open(log, "w"), stderr=subprocess.STDOUT)
    for _ in range(60):
        if port_open(args.port):
            break
        time.sleep(0.2)
    else:
        proc.kill()
        sys.exit("OpenOCD did not open its telnet port; see " + log)
    print(f"openocd {os.path.basename(os.path.dirname(ocd))}")

    prefix = os.path.abspath(args.out)
    if args.mcore:
        targets = [prefix + ".SVDat"]
        command = f"esp sysview_mcore start file://{targets[0]} 1 -1 -1"
    else:
        targets = [prefix + "-pro.SVDat", prefix + "-app.SVDat"]
        command = (f"esp sysview start file://{targets[0]} "
                   f"file://{targets[1]} 1 -1 -1")

    try:
        # OpenOCD's init halts the target; put it back before tracing, and do not
        # reset -- the application under study is the one already running.
        telnet(args.port, "resume", 0.3)
        reply = telnet(args.port, command, 0.5)
        if "invalid command" in reply or "not supported" in reply:
            print(reply, end="")
            sys.exit("this OpenOCD cannot start SystemView tracing")
        print(f"recording {args.seconds:g}s ...")
        time.sleep(args.seconds)
        telnet(args.port, "esp sysview stop", 1.0)
        # Leaving the target halted is the trap tools/jtag-profile.py documents:
        # the console and the network both stop answering until it is resumed.
        telnet(args.port, "resume", 0.3)
    finally:
        telnet(args.port, "shutdown", 0.3)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    got = []
    for path in targets:
        size = os.path.getsize(path) if os.path.exists(path) else 0
        got.append((path, size))
        print(f"  {size:>8} bytes  {path}")
    if not any(size for _, size in got):
        print("\nno data: is the board running a PROFILE=sysview build?")
        sys.exit(1)

    if args.open:
        if os.path.exists("/Applications/SystemView.app"):
            subprocess.run(["open", "-a", "SystemView", got[0][0]], check=False)
        else:
            print("SEGGER SystemView not installed; open the file above by hand")


if __name__ == "__main__":
    main()
