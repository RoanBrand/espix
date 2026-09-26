#!/usr/bin/env python3
"""Ask the S31 console a question and get an immediate answer.

    tools/esp-console.py ready            # READY | NO-RESPONSE
    tools/esp-console.py run "uname -a"   # run one line, print the reply
    tools/esp-console.py resume           # JTAG reset run, then wait for READY
    tools/esp-console.py jtag             # cores halted or running (RESETS them)

Why this exists. A serial port has no notion of a peer connecting: open it after
the board has already printed its prompt and there is nothing pending, because
the shell reprints the prompt only when it reads a newline. So every probe sends
a newline first.

The trap this tool exists to avoid: OpenOCD init HALTS the cores, and its
shutdown does not resume them, so a readiness check that consults JTAG with
init/targets/shutdown leaves the board dead and the console silent -- which looks
exactly like a hang. The default check therefore touches the UART only. Only
resume and jtag use OpenOCD, and both end with reset run, the one sequence
observed to leave the board executing.
"""
import glob
import os
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing; run through the IDF python")

ESP = os.path.expanduser("~/.espressif/tools")
PORT = os.environ.get("ESPIX_CONSOLE", "/dev/cu.usbserial-110")


def find_openocd():
    best = None
    for ocd in sorted(glob.glob(ESP + "/openocd-esp32/*/openocd-esp32")):
        if os.path.exists(ocd + "/share/openocd/scripts/target/esp32s31.cfg"):
            best = ocd
    return best


def openocd(last_cmd):
    """Run OpenOCD and always end by leaving the board running."""
    ocd = find_openocd()
    if ocd is None:
        return ""
    cfg = ocd + "/share/openocd/scripts/board/esp32s31-builtin.cfg"
    cmd = [ocd + "/bin/openocd", "-f", cfg, "-c", "init"]
    if last_cmd:
        cmd += ["-c", last_cmd]
    cmd += ["-c", "reset run", "-c", "shutdown"]
    res = subprocess.run(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, timeout=60)
    return res.stdout


def probe(timeout=8.0):
    """Poke the console once a second until the prompt answers.

    One newline is not enough: the prompt is printed when the shell *reads*
    input, so a board that finished booting before we opened the port has
    nothing pending, and a board still booting needs ~11 s. So send a newline
    every second for `timeout` seconds, and treat silence over that whole window
    as a real answer -- no output at all means booting, halted, or starved, and
    the caller decides which with the JTAG state.
    """
    try:
        s = serial.Serial(PORT, 115200, timeout=0.2)
    except Exception as e:
        return None, "cannot open " + PORT + ": " + str(e)

    buf = bytearray()
    t = time.time()
    next_poke = 0.0
    while time.time() - t < timeout:
        if time.time() - t >= next_poke:
            s.write(b"\r\n")
            next_poke += 1.0
        line = s.readline()
        if line:
            buf += line
            if b"root:~#" in buf:
                break
    s.close()
    return b"root:~#" in buf, bytes(buf)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    what = sys.argv[1]

    if what == "resume":
        openocd("")
        # A reset runs the full boot, which takes ~11 s on this board; a shorter
        # window reports a healthy board as dead.
        time.sleep(1)
        ok, _ = probe(16)
        print("READY" if ok else "still no response")
        return

    if what == "jtag":
        out = openocd("targets")
        states = re.findall(r"esp32s31\.hp\.cpu\d\s+\S+\s+\S+\s+\S+\s+(\w+)", out)
        print("cores: " + (", ".join(states) if states else "unknown"))
        print("(this resets the board, so the state shown is from before it)")
        return

    if what == "run":
        if len(sys.argv) < 3:
            sys.exit('usage: esp-console.py run "<command>"')
        cmd = sys.argv[2]
        ok, _ = probe(3)
        if not ok:
            print("no response; try resume")
            return
        s = serial.Serial(PORT, 115200, timeout=0.2)
        s.write((cmd + "\n").encode())
        t = time.time()
        out = bytearray()
        while time.time() - t < 10:
            line = s.readline()
            if line:
                out += line
        s.close()
        sys.stdout.write(out.decode("utf-8", "replace"))
        return

    ok, _ = probe(2)
    print("READY" if ok else "NO-RESPONSE (try resume)")


if __name__ == "__main__":
    main()
