#!/usr/bin/env python3
"""Two frames of the screen a second apart, then optionally a key from the viewer.

    gfx_probe.py <host> [key]

Prints "moved <pixels that changed> of <pixels>, colours <distinct, sampled>",
which is what says an app is drawing and not just holding the screen.
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from rfb import Rfb, count_differing  # noqa: E402

c = Rfb(sys.argv[1])
c.connect()
a = c.frame(full=True)
time.sleep(1.0)
b = c.frame(full=True)
colours = len({bytes(b[i:i + 3]) for i in range(0, len(b), 4 * 97)})
print("moved %d of %d, colours %d" % (count_differing(a, b), c.width * c.height, colours))
if len(sys.argv) > 2:
    k = sys.argv[2]
    keysym = ord(k) if len(k) == 1 else int(k, 0)
    c.key(keysym, True)
    time.sleep(0.05)
    c.key(keysym, False)
c.close()
