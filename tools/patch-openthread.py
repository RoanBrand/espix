#!/usr/bin/env python3
"""
Put OpenThread's instance storage in PSRAM instead of internal RAM.

Why
---
OpenThread keeps its single ot::Instance in one static buffer,
ot::gInstanceRaw[kInstanceSizeInUint64s], in
components/openthread/openthread/src/core/instance/instance.cpp. That buffer is
the whole stack's state graph -- MLE, IP6, MeshForwarder, NetworkData, SRP,
CoAP, the dataset and the timers -- and on the S31 it measures 143,856 bytes of
internal DRAM .bss. It is the largest single internal allocation espix makes,
and the S31 has 16 MiB of PSRAM beside it.

EXT_RAM_BSS_ATTR (esp_attr.h) places a symbol in .ext_ram.bss, which IDF's
linker script puts in external memory when
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y -- espix already sets it. With
the option off the macro expands to nothing, so this patch is byte-for-byte
inert on a build that does not ask for it.

What it changes
---------------
1. Include esp_attr.h, which brings EXT_RAM_BSS_ATTR and sdkconfig.h.
2. Add the attribute to the gInstanceRaw definition.

Two things to respect, both about *when* the bytes must be reachable rather than
where they live:

  * PSRAM is behind the cache. While the cache is frozen -- esp_partition_mmap()
    and therefore OTA -- the instance is not addressable: the same hazard espix
    records for PSRAM task stacks. Do not run an OTA while OpenThread is using it.
  * The 802.15.4 path must not dereference the instance from an ISR. It posts to
    a queue and the OpenThread task does the work, so this is expected to hold,
    but it is the thing to re-check if radio behaviour changes.

Where this writes
-----------------
instance.cpp is part of the OpenThread copy bundled inside the IDF component,
not a managed component, so this patches the IDF installation in place, as
tools/patch-fatfs.py and tools/patch-libc-offt.py do. The CMake hook re-applies
it on every configure, so an IDF reinstall or git checkout is patched again
rather than silently losing the 144 KB.

Failure policy
--------------
Loud, never silent: a missing file or a moved anchor stops the build with a
message saying which one, and running twice is a no-op rather than a second
edit.
"""

import argparse
import sys
from pathlib import Path

ATTR_MARK = "EXT_RAM_BSS_ATTR"

INSTANCE_REL = "components/openthread/openthread/src/core/instance/instance.cpp"

ANCHOR_INCLUDE = '#include "utils/heap.hpp"'
ANCHOR_RAW = "uint64_t gInstanceRaw[kInstanceSizeInUint64s];"

NEW_INCLUDE = (
    '#include "utils/heap.hpp"\n\n'
    '/*\n'
    ' * EXT_RAM_BSS_ATTR: keep gInstanceRaw out of internal DRAM. See\n'
    ' * tools/patch-openthread.py.\n'
    ' */\n'
    '#include "esp_attr.h"'
)

NEW_RAW = (
    '/*\n'
    ' * The whole OpenThread instance. EXT_RAM_BSS_ATTR is empty unless\n'
    ' * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY, in which case this ~144 KB\n'
    ' * lives in PSRAM rather than internal RAM.\n'
    ' */\n'
    'uint64_t gInstanceRaw[kInstanceSizeInUint64s] EXT_RAM_BSS_ATTR;'
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", required=True, type=Path)
    args = parser.parse_args()

    path = args.idf_path / INSTANCE_REL
    try:
        text = path.read_text()
    except OSError as exc:
        raise SystemExit(f"patch-openthread: cannot read {path}: {exc}")

    if ATTR_MARK in text:
        print(f"patch-openthread: already patched ({path})")
        return 0

    if ANCHOR_INCLUDE not in text:
        raise SystemExit(f"patch-openthread: anchor {ANCHOR_INCLUDE!r} not found in {path}")
    if ANCHOR_RAW not in text:
        raise SystemExit(f"patch-openthread: anchor {ANCHOR_RAW!r} not found in {path}")

    text = text.replace(ANCHOR_INCLUDE, NEW_INCLUDE, 1)
    text = text.replace(ANCHOR_RAW, NEW_RAW, 1)
    try:
        path.write_text(text)
    except OSError as exc:
        raise SystemExit(f"patch-openthread: cannot write {path}: {exc}")

    print(f"patch-openthread: gInstanceRaw -> .ext_ram.bss in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
