#!/usr/bin/env python3
"""Open a terminal on the desktop over VNC and type at it, as a person at the board would.

    vnc_keys.py <host> STEP...

Each STEP, in order:
    type:TEXT       type TEXT and Enter, then wait for the command to start
    key:KEYSYM      press and release one key (0xFF52 is Up, 0xFFBE F1)

The terminal is closed with `exit` at the end; what the commands did is the caller's to read.
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from rfb import Rfb  # noqa: E402

# Centres measured from the bottom of the screen: the start button (desktop.c start_rect())
# and the menu's "terminal" row (menu_rect(), menu_row_top(1)).
START = (13, 13)
TERMINAL = (68, 126)


def main():
    c = Rfb(sys.argv[1])
    c.connect()
    c.frame(full=True)
    c.click(START[0], c.height - START[1], settle=0.5)
    c.click(TERMINAL[0], c.height - TERMINAL[1], settle=3.0)
    for step in sys.argv[2:]:
        kind, _, arg = step.partition(":")
        if kind == "type":
            c.type_text(arg + "\n")
            time.sleep(2.0)
        elif kind == "key":
            sym = int(arg, 0)
            c.key(sym, True)
            time.sleep(0.05)
            c.key(sym, False)
            time.sleep(1.0)
        else:
            raise SystemExit(f"vnc_keys: no step {step!r}")
    c.type_text("exit\n")
    time.sleep(0.5)
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
