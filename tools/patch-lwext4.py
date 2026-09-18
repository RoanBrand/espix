#!/usr/bin/env python3
"""
Teach the vendored lwext4 core to honour the metadata checksum seed.

A volume made by e2fsprogs 1.47 or later sets INCOMPAT_CSUM_SEED (0x2000),
because mke2fs now enables metadata_csum_seed by default, and every metadata
checksum on such a volume is seeded from the superblock's s_checksum_seed
instead of from the filesystem UUID. The core seeds from the UUID in eight
places, so it refuses the volume at mount -- correctly, since the feature is not
implemented. The effect is that no ext4 volume a current Linux creates can be
read at all. See docs/KNOWN-ISSUES.md.

The change is tools/lwext4-csum-seed.patch, applied to the *core* -- the lwext4
submodule the component carries, not the port around it. Applying the patch file
rather than doing the same surgery in Python, as the other two scripts here do,
is deliberate and is the one place this file differs from them: the change is
ten files and eight functions, so a second copy of it in Python would be one
more thing to keep in step, and this way the applied change and the artefact
that goes upstream are the same bytes.

Idempotent, and it has to be: CMake runs this on every configure because the
component manager re-downloads the component whenever it re-resolves, which
takes the patch with it.

**Temporary by construction.** When this goes upstream -- it belongs to
gkostka/lwext4, and the port would pick it up by bumping its submodule -- delete
this file, tools/lwext4-csum-seed.patch and the execute_process() block in the
top-level CMakeLists.txt.
"""

import re
import subprocess
import sys
from pathlib import Path

# The component revision this patch was written against, and the revision
# main/idf_component.yml pins. check_version() refuses anything else, because a
# patch that no longer matches would otherwise fail as a confusing apply error
# or, worse, apply to the wrong lines.
EXPECTED_REVISION = "d774c178f0211d62d3f0d49e9164aa655573ecfe"
COMPONENT = "esp_lwext4"
PATCH = "lwext4-csum-seed.patch"

# In the core, and the whole of the change: the helper every checksum site now
# asks for the seed from. Its presence is what "already patched" means.
MARKER = "ext4_sb_csum_seed"


def die(msg):
    print(f"patch-lwext4: {msg}", file=sys.stderr)
    sys.exit(1)


def check_revision(root):
    """Refuse to patch a revision this was not written against."""
    lock = root / "dependencies.lock"
    if not lock.exists():
        return  # first configure, before the manager has written one
    text = lock.read_text(encoding="utf-8")

    m = re.search(r"^  esp_lwext4:\n(?:.*?\n)*?^    version: (\S+)$", text, re.M)
    if m and m.group(1).strip("'\"") != EXPECTED_REVISION:
        die(
            f"esp_lwext4 is {m.group(1)}, but this patch was written for\n"
            f"  {EXPECTED_REVISION}.\n"
            f"  Check tools/{PATCH} against the new source -- it may still "
            f"apply -- then update EXPECTED_REVISION, and update the pin in\n"
            f"  main/idf_component.yml to match."
        )


def main():
    root = Path(__file__).resolve().parent.parent
    comp = root / "managed_components" / COMPONENT

    if not comp.is_dir():
        # Nothing downloaded yet. Not an error: CMake reconfigures after the
        # component manager runs, and this will be called again.
        return 0

    check_revision(root)

    core = comp / "lwext4"
    header = core / "include" / "ext4_super.h"

    if not header.is_file():
        die(
            f"{core} has no include/ext4_super.h, so the lwext4 submodule did "
            f"not\n  come with the component. The component manager fetches "
            f"submodules, so a\n  core that is missing means this tree was "
            f"assembled some other way."
        )

    if MARKER in header.read_text(encoding="utf-8"):
        return 0  # already patched; configure runs every build

    patch = Path(__file__).resolve().parent / PATCH
    if not patch.is_file():
        die(f"{patch} is missing; it is the change this applies.")

    # Dry run first. A tree that is half-patched -- an apply interrupted, or a
    # hand edit -- is reported rather than made worse, and the way out is named.
    args = ["patch", "-p1", "-d", str(core), "--forward", "-i", str(patch)]
    check = subprocess.run(args + ["--dry-run"], capture_output=True, text=True)
    if check.returncode != 0:
        die(
            f"the patch does not apply to this tree:\n"
            f"{check.stdout}{check.stderr}"
            f"  Delete managed_components/{COMPONENT} and reconfigure: the "
            f"manager fetches\n  a clean copy, and this runs again on the way "
            f"back up."
        )

    applied = subprocess.run(args, capture_output=True, text=True)
    if applied.returncode != 0:
        die(f"patch failed:\n{applied.stdout}{applied.stderr}")

    print(
        f"patch-lwext4: taught the lwext4 core to honour the metadata checksum "
        f"seed (in managed_components/{COMPONENT}/lwext4)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
