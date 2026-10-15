#!/usr/bin/env python3
"""
How long does the S31 run video over VNC, plus the 2D-DMA stress, before something wedges?

    tools/vnc-soak.py                         60 minutes against the board in .espix/hosts
    tools/vnc-soak.py -t 240 --host 1.2.3.4   four hours, an explicit address
    tools/vnc-soak.py --video /mnt/bigfat/x.mjpg

One number, like tools/soak.sh: time to the first failure, or the full run clean.
A failure is any of: the board reset (/proc/uptime went backwards), a core dump
stored, the accelerator retired (`display status`), or a viewer that stopped
getting frames. Capture the console with tools/serlog.sh while this runs -- and
nothing else on the UART.
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "lib"))
from rfb import Rfb  # noqa: E402

TASKBAR_H = 26


def board(host, cmd, timeout=90):
    env = dict(os.environ, ESPIX_HOST=host)
    try:
        r = subprocess.run([os.path.join(ROOT, "tools", "esp.sh"), cmd], env=env,
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout
    except subprocess.TimeoutExpired:
        return ""


def uptime_s(host):
    out = board(host, "cat /proc/uptime")
    try:
        return float(out.split()[0])
    except (IndexError, ValueError):
        return None


def watch(host, seconds):
    """Video over VNC with Tight, Settings opened and closed; updates seen."""
    c = Rfb(host, tight=True, timeout=10.0)
    c.connect()
    c.frame(full=True)
    w, h = c.width, c.height
    bar_y = h - TASKBAR_H
    menu_y = bar_y - (3 + 4 * 20 + 7 + 2 * 20 + 3)
    n = 0
    end = time.time() + seconds
    while time.time() < end:
        c.frame()
        n += 1
        if n % 200 == 0:
            c.click(13, bar_y + 13, settle=0.05)
            c.frame()
            c.click(40, menu_y + 3 + 2 * 20 + 10, settle=0.05)  # settings
            c.frame()
            c.click(116 + 560 - 9, 90 + 9, settle=0.05)          # its close box
        if n % 50 == 0:
            c.pointer((n * 37) % w, (n * 53) % (h - TASKBAR_H))
    c.close()
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("ESPIX_HOST", "192.168.110.203"))
    ap.add_argument("-t", "--minutes", type=float, default=60)
    ap.add_argument("--video", default="/mnt/bigfat/sintel-640x480-24fps.mjpg")
    ap.add_argument("--cycle", type=int, default=60, help="seconds of video per cycle")
    a = ap.parse_args()

    start = time.time()
    last_up = uptime_s(a.host)
    cycles = updates = 0
    why = None

    while time.time() - start < a.minutes * 60:
        board(a.host, "desktop open " + a.video)
        try:
            n = watch(a.host, a.cycle)
        except Exception as e:  # a viewer that cannot connect or stops hearing back
            why = "viewer: %s" % e
            break
        updates += n
        if n < a.cycle * 5:
            why = "viewer got only %d updates in %d s" % (n, a.cycle)
            break

        board(a.host, "display dmatest 20")

        up = uptime_s(a.host)
        status = board(a.host, "coredump; display status")
        if up is None or (last_up is not None and up < last_up):
            why = "the board reset (uptime %s -> %s)" % (last_up, up)
            break
        if "faulting task" in status or "core dump:" in status:
            why = "a core dump is stored"
            break
        if "accelerator: off" in status:
            why = status.strip().splitlines()[-1]
            break
        last_up = up
        cycles += 1
        print("%6.1f min: %d cycles clean, %d updates" %
              ((time.time() - start) / 60, cycles, updates), flush=True)

    mins = (time.time() - start) / 60
    if why:
        print("FAILED after %.1f min (%d clean cycles): %s" % (mins, cycles, why))
        return 1
    print("clean: %.1f min, %d cycles, %d updates" % (mins, cycles, updates))
    return 0


if __name__ == "__main__":
    sys.exit(main())
