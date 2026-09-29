#!/usr/bin/env python3
"""
Stop the PPA blit and scale paths discarding CPU-written pixels.

The fill path flushes its output window with C2M | INVALIDATE -- write back the
dirty cache lines, then invalidate -- which is what a DMA output needs. The SRM
(blit/scale) and blend paths ask for M2C only, and per esp_cache.h that "will by
default do an invalidation": it *discards* dirty lines instead of writing them
back.

The window it discards is the destination's full rows, not just the block: the
length is pic_w * block_h. So any pixel the CPU drew in those rows before the
accelerator ran is silently lost. On the desktop that is every small fill --
the launcher's icon bands (200 px, below PPA_MIN_PIXELS, so drawn in software)
are wiped by the next window blit whose rows they share.

Which is also why a drag is asymmetric. A horizontal move uncovers a strip in
the same rows as the window, so the accelerator invalidates the strip and the
icon stays dark; a vertical move uncovers rows the blit does not touch, so the
icon redraws. Measured, not inferred: a full repaint drew two of the four icon
bands and a partial repaint drew all four, and the terminal window at y40..290
covers exactly the rows that lose theirs.

**Temporary by construction.** tools/esp_driver_ppa-out-sync.patch is the same
change as a plain diff, ready to send to espressif/esp-idf. When it lands,
delete both files and the execute_process() block in the top-level
CMakeLists.txt.
"""

import os
import sys
from pathlib import Path

EXPECTED_IDF = "v6.1"

# The same call in both engines; ppa_fill.c already does this correctly, which
# is the shape of the bug rather than a coincidence.
OLD = ("esp_cache_msync((void *)out_ext_window_aligned, PPA_ALIGN_UP("
       "out_ext_window_len + (out_ext_window - out_ext_window_aligned), "
       "out_buf_alignment), ESP_CACHE_MSYNC_FLAG_DIR_M2C);")
NEW = ("esp_cache_msync((void *)out_ext_window_aligned, PPA_ALIGN_UP("
       "out_ext_window_len + (out_ext_window - out_ext_window_aligned), "
       "out_buf_alignment), ESP_CACHE_MSYNC_FLAG_DIR_C2M | "
       "ESP_CACHE_MSYNC_FLAG_INVALIDATE);")

PATCHES = {
    "components/esp_driver_ppa/src/ppa_srm.c":   (OLD, NEW),
    "components/esp_driver_ppa/src/ppa_blend.c": (OLD, NEW),
}


def die(msg):
    print(f"patch-ppa: {msg}", file=sys.stderr)
    sys.exit(1)


def arg_path(argv, flag):
    if flag in argv:
        i = argv.index(flag)
        if i + 1 < len(argv):
            return Path(argv[i + 1])
    return None


def idf_root(argv):
    given = arg_path(argv, "--idf-path")
    if given is not None:
        return given
    env = os.environ.get("IDF_PATH")
    if not env:
        die("no IDF path: pass --idf-path, or set IDF_PATH")
    return Path(env)


def check_version(idf):
    version = idf / "version.txt"
    if not version.is_file():
        return
    got = version.read_text(encoding="utf-8").strip()
    if got != EXPECTED_IDF:
        die(
            f"ESP-IDF is {got}, but this patch was written against "
            f"{EXPECTED_IDF}.\n"
            f"  Check the anchors in tools/patch-ppa.py against the new "
            f"source, then update EXPECTED_IDF."
        )


def main():
    idf = idf_root(sys.argv)

    for rel, (old, new) in PATCHES.items():
        target = idf / rel
        if not target.is_file():
            die(f"{target} does not exist; is the IDF path right?")

        text = target.read_text(encoding="utf-8")

        # Configure runs this every build, so it has to be a no-op from the
        # second time on.
        if new in text:
            continue

        if text.count(old) != 1:
            die(
                f"expected exactly one anchor in {rel}, found "
                f"{text.count(old)}.\n  The file moved; re-read it before "
                f"changing this script."
            )

        check_version(idf)
        target.write_text(text.replace(old, new), encoding="utf-8")
        print(f"patch-ppa: wrote the output cache back before invalidating in {rel}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
