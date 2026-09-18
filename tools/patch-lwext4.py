#!/usr/bin/env python3
"""
Teach the vendored lwext4 core the two things it needs that upstream lacks.

The changes are patch files, applied to the *core* -- the lwext4 submodule the
component carries, not the port around it. Applying patch files rather than doing
the same surgery in Python, as the other scripts here do, is deliberate and is
where this file differs from them: between them the changes are ten files and eight
functions plus a two-line fix, so a second copy in Python would be one more thing
to keep in step, and this way the applied change and the artefact that goes
upstream are the same bytes.

**lwext4-csum-seed.patch** is why an ext4 volume made by a current Linux could not
be read at all. A volume made by e2fsprogs 1.47 or later sets INCOMPAT_CSUM_SEED
(0x2000), because mke2fs now enables metadata_csum_seed by default, and every
metadata checksum on such a volume is seeded from the superblock's
s_checksum_seed instead of from the filesystem UUID. The core seeded from the UUID
in eight places, so it refused the volume at mount -- correctly, since the feature
was not implemented. See docs/KNOWN-ISSUES.md.

**lwext4-fwrite-error.patch** is the one that matters before anything writes to an
ext volume. `ext4_fwrite()` assigns the result of `ext4_fs_put_inode_ref()` to `r`
at its `Finish` label, discarding the error that sent it there -- so a failed block
write can commit its transaction instead of aborting, and the caller is told the
operation succeeded. That is why the write milestone is read-only until this
lands, and why it is fixed here rather than worked around: see docs/ROADMAP.md.

Idempotent, and it has to be: CMake runs this on every configure because the
component manager re-downloads the component whenever it re-resolves, which takes
the patches with it.

**Temporary by construction.** Both belong to gkostka/lwext4, and the port would
pick them up by bumping its submodule. When they land, delete this file, the two
patch files and the execute_process() block in the top-level CMakeLists.txt.
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

# (patch file, file in the core, a string that only exists once it is applied).
#
# A marker per patch, rather than one test for all of them, and patch(1)'s exit
# status is not trusted for "already applied": asked to reverse a patch that is not
# in the tree, patch *asks* rather than failing -- "Unreversed (or previously
# applied) patch detected! Ignore -R?" -- and answers itself with the default, so it
# returns success in both cases. A marker cannot be fooled that way.
PATCHES = [
    # INCOMPAT_CSUM_SEED, without which no current ext4 volume mounts.
    ("lwext4-csum-seed.patch", "include/ext4_super.h", "ext4_sb_csum_seed"),
    # A failed write must abort its transaction, not commit over the failure.
    ("lwext4-fwrite-error.patch", "src/ext4.c",
     "const int released = ext4_fs_put_inode_ref(&ref);"),
]


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


def patch_command(core, name, *flags):
    """A patch invocation, with the patch file named rather than piped in.

    `--batch` because a build must never wait for an answer: patch asks about
    anything ambiguous, and its default answer is not always the one a build wants.
    """
    return ["patch", "-p1", "-d", str(core), "--batch", *flags, "-i",
            str(Path(__file__).resolve().parent / name)]


def apply(core, name):
    # Dry run first. A tree that is half-patched -- an apply interrupted, or a hand
    # edit -- is reported rather than made worse, and the way out is named.
    check = subprocess.run(patch_command(core, name, "--dry-run"),
                           capture_output=True, text=True)
    if check.returncode != 0:
        die(
            f"{name} does not apply to this tree:\n{check.stdout}{check.stderr}"
            f"  Delete managed_components/{COMPONENT} and reconfigure: the manager"
            f" fetches\n  a clean copy, and this runs again on the way back up."
        )

    done = subprocess.run(patch_command(core, name, "--forward"),
                          capture_output=True, text=True)
    if done.returncode != 0:
        die(f"{name} failed:\n{done.stdout}{done.stderr}")

    print(f"patch-lwext4: applied {name} to the lwext4 core")


def main():
    root = Path(__file__).resolve().parent.parent
    comp = root / "managed_components" / COMPONENT

    if not comp.is_dir():
        # Nothing downloaded yet. Not an error: CMake reconfigures after the
        # component manager runs, and this will be called again.
        return 0

    check_revision(root)

    core = comp / "lwext4"

    if not (core / "src" / "ext4.c").is_file():
        die(
            f"{core} has no src/ext4.c, so the lwext4 submodule did not\n"
            f"  come with the component. The component manager fetches "
            f"submodules, so a\n  core that is missing means this tree was "
            f"assembled some other way."
        )

    for name, where, marker in PATCHES:
        target = core / where
        if not target.is_file():
            die(f"{core} has no {where}; the core is not the tree this expects.")

        if marker in target.read_text(encoding="utf-8"):
            continue  # configure runs every build, so this has to be a no-op
        apply(core, name)

    return 0


if __name__ == "__main__":
    sys.exit(main())
