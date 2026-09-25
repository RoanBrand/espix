#!/usr/bin/env python3
"""
Make GMF allocate from PSRAM before internal RAM.

---------------
What this does
---------------
gmf_core's esp_gmf_oal_calloc_inner() calls

    heap_caps_calloc_prefer(n, size, 2,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM)

-- internal first, PSRAM only as the fallback. On a part where Bluetooth
Classic needs the internal heap (Bluedroid's BTU task aborts in fixed_queue_new
when it cannot get a mutex), a GMF pipeline that fills internal RAM starves the
radio: connecting a sink then asserts. Every GMF object -- pools, elements, IO
contexts, payload headers -- comes through here, so flipping the preference is
what moves the pipeline out of the radio's way. PSRAM is slower, and that is
the right trade: the audio paths that matter are the hardware ASRC and the
decoder, not the object headers.

---------------
Why a patch
---------------
There is no Kconfig or runtime setter for the preference (only
esp_gmf_oal_mem_spiram_is_enabled(), a getter). Temporary by construction: the
same change as a one-line git patch, ready to send upstream, and this hook goes
when it lands.
"""

import argparse
import os
import sys

OLD = ("heap_caps_calloc_prefer(n, size, 2, "
       "MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "
       "MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM)")
NEW = ("heap_caps_calloc_prefer(n, size, 2, "
       "MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM, "
       "MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)")

REL = os.path.join("espressif__gmf_core", "oal", "esp_gmf_oal_mem.c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--components-path",
                    default=os.path.join(os.getcwd(), "managed_components"))
    args = ap.parse_args()

    path = os.path.join(args.components_path, REL)
    if not os.path.exists(path):
        sys.stderr.write("patch-gmf: %s not found\n" % path)
        return 1

    with open(path, "r") as f:
        text = f.read()

    if NEW in text:
        print("patch-gmf: already patched")
        return 0
    if OLD not in text:
        sys.stderr.write(
            "patch-gmf: the expected calloc_prefer line is gone; gmf_core moved\n")
        return 1

    with open(path, "w") as f:
        f.write(text.replace(OLD, NEW))
    print("patch-gmf: GMF objects now prefer PSRAM over internal RAM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
