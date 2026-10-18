#!/usr/bin/env python3
"""
Fetch esp_lwext4 and its lwext4 core at pinned revisions, and teach the core the
two things it needs that upstream lacks.

**Why not the component manager.** It hashes every file of a git dependency,
including the submodule's `lwext4/.git` file -- and that file holds a relative
path to the downloader's own cache, so the hash in dependencies.lock only ever
matched on the machine that wrote it. Fetching by commit id here gives the same
guarantee (git verifies the content) without that. The result goes to the
gitignored external/esp_lwext4, which CMakeLists.txt adds as a component.

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

Runs on every configure. A stamp naming both revisions and both patches makes
that a no-op; when any of them changes, the tree is fetched and patched afresh.

**Temporary by construction.** Both patches belong to gkostka/lwext4, and the
port would pick them up by bumping its submodule. When they land, drop the
patching here and the two patch files.
"""

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

COMPONENT = "esp_lwext4"
PORT_REPO = "https://github.com/huming2207/esp_lwext4.git"
PORT_REVISION = "d774c178f0211d62d3f0d49e9164aa655573ecfe"
# The submodule commit PORT_REVISION records; check it when bumping the port.
CORE_REPO = "https://github.com/gkostka/lwext4.git"
CORE_REVISION = "58bcf89a121b72d4fb66334f1693d3b30e4cb9c5"

STAMP = ".espix-stamp"

# (patch file, file in the core, a string that only exists once it is applied).
PATCHES = [
    # INCOMPAT_CSUM_SEED, without which no current ext4 volume mounts.
    ("lwext4-csum-seed.patch", "include/ext4_super.h", "ext4_sb_csum_seed"),
    # A failed write must abort its transaction, not commit over the failure.
    ("lwext4-fwrite-error.patch", "src/ext4.c",
     "const int released = ext4_fs_put_inode_ref(&ref);"),
]

TOOLS = Path(__file__).resolve().parent


def die(msg):
    print(f"patch-lwext4: {msg}", file=sys.stderr)
    sys.exit(1)


def git(*args, cwd):
    done = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if done.returncode != 0:
        die(f"git {' '.join(args)} failed in {cwd}:\n{done.stdout}{done.stderr}")
    return done.stdout


def fetch(repo, revision, into):
    into.mkdir(parents=True, exist_ok=True)
    git("init", "-q", cwd=into)
    git("fetch", "-q", "--depth", "1", repo, revision, cwd=into)
    git("-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD", cwd=into)
    got = git("rev-parse", "HEAD", cwd=into).strip()
    if got != revision:
        die(f"{repo} gave {got}, not {revision}")


def apply(core, name):
    patch = str(TOOLS / name)
    check = subprocess.run(["git", "apply", "--check", patch], cwd=core,
                           capture_output=True, text=True)
    if check.returncode != 0:
        die(f"{name} does not apply to {CORE_REVISION}:\n{check.stderr}")
    git("apply", patch, cwd=core)
    print(f"patch-lwext4: applied {name} to the lwext4 core")


def stamp_text():
    parts = [PORT_REVISION, CORE_REVISION]
    for name, _, _ in PATCHES:
        parts.append(hashlib.sha256((TOOLS / name).read_bytes()).hexdigest())
    return " ".join(parts) + "\n"


def main():
    root = TOOLS.parent
    dest = root / "external" / COMPONENT
    want = stamp_text()

    try:
        if (dest / STAMP).read_text() == want:
            return 0
    except OSError:
        pass

    # Built beside the destination and swapped in, so an interrupted fetch never
    # leaves a half-patched tree that looks finished.
    work = dest.with_name(COMPONENT + ".tmp")
    shutil.rmtree(work, ignore_errors=True)
    fetch(PORT_REPO, PORT_REVISION, work)
    core = work / "lwext4"
    fetch(CORE_REPO, CORE_REVISION, core)

    for name, where, marker in PATCHES:
        target = core / where
        if not target.is_file():
            die(f"{core} has no {where}; the core is not the tree this expects.")
        if marker not in target.read_text(encoding="utf-8"):
            apply(core, name)

    (work / STAMP).write_text(want)
    shutil.rmtree(dest, ignore_errors=True)
    work.rename(dest)
    print(f"patch-lwext4: {COMPONENT} {PORT_REVISION[:8]} with lwext4 "
          f"{CORE_REVISION[:8]} in external/{COMPONENT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
