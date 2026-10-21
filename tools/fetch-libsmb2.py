#!/usr/bin/env python3
"""
Fetch libsmb2 at a pinned revision into the gitignored external/libsmb2.

The tree is upstream's, except for the patches in components/libsmb2/patches/,
which are applied here. Their contents are part of the stamp, so adding or
editing one refetches and reapplies rather than looking finished. The patches
are bugs in upstream, not platform glue, and the intent is to send them
upstream and then drop them.

What upstream gets wrong for ESP-IDF is not usually one of its sources, it is
the platform glue they are built against: lib/CMakeLists.txt calls
register_component(), the pre-IDF-5 API IDF 6.1 no longer has, and the tree
expects readv() and writev() from a C library that has neither here.
components/libsmb2 registers the same sources itself and passes the two NEED_
macros compat.c already carries fallbacks behind; gethostname() comes from
espix_net, which already exports it.

Why not the component manager. The only version it publishes, sahlberg/libsmb2
3.0.1, was uploaded in 2021 and predates the server API entirely -- the
smb2_serve_port() this component exists to use is 2024 code -- and upstream's
own ESP glue does not build either way. The manager would fetch a tree we still
have to supply a component file for. Fetching by commit id also gives what the
manager's lock file cannot promise across machines, which is the reason
patch-lwext4.py gives in full. One mechanism for both dependencies.

The revision is master as of 2026-10-20. The port was designed against
2026-10-19's bb426b16, the first that carries the SMB2 server; this one adds
46 commits of libdcerpc hardening, which matters once smbd serves srvsvc with
libdcerpc's server runtime. Bumping it means re-reading SMB2_CORE_SOURCES in
lib/CMakeLists.txt, because components/libsmb2/CMakeLists.txt keeps a copy of
that list.

Runs on every configure. A stamp naming the revision and the patches makes that
a no-op.
"""

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

COMPONENT = "libsmb2"
REPO = "https://github.com/sahlberg/libsmb2.git"
REVISION = "57fea04fd29403e3978000179339c8e13ce46b5c"

STAMP = ".espix-stamp"

TOOLS = Path(__file__).resolve().parent


def die(msg):
    print(f"fetch-libsmb2: {msg}", file=sys.stderr)
    sys.exit(1)


def git(*args, cwd):
    done = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if done.returncode != 0:
        die(f"git {' '.join(args)} failed in {cwd}:\n{done.stdout}{done.stderr}")
    return done.stdout


def patch_dir(root):
    return root / "components" / COMPONENT / "patches"


def patches(root):
    return sorted(patch_dir(root).glob("*.patch"))


def patches_digest(root):
    h = hashlib.sha256()
    for p in patches(root):
        h.update(p.name.encode())
        h.update(p.read_bytes())
    return h.hexdigest()[:16]


def apply_patches(work, root):
    for p in patches(root):
        done = subprocess.run(
            ["git", "apply", "--whitespace=nowarn", str(p)],
            cwd=work, capture_output=True, text=True)
        if done.returncode != 0:
            die(f"applying {p.name} failed:\n{done.stdout}{done.stderr}")
        print(f"fetch-libsmb2: applied {p.name}")


def fetch(repo, revision, into):
    into.mkdir(parents=True, exist_ok=True)
    git("init", "-q", cwd=into)
    git("fetch", "-q", "--depth", "1", repo, revision, cwd=into)
    git("-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD", cwd=into)
    got = git("rev-parse", "HEAD", cwd=into).strip()
    if got != revision:
        die(f"{repo} gave {got}, not {revision}")


def main():
    root = TOOLS.parent
    dest = root / "external" / COMPONENT
    want = REVISION + " " + patches_digest(root) + "\n"

    try:
        if (dest / STAMP).read_text() == want:
            return 0
    except OSError:
        pass

    # Built beside the destination and swapped in, so an interrupted fetch never
    # leaves a half-populated tree that looks finished.
    work = dest.with_name(COMPONENT + ".tmp")
    shutil.rmtree(work, ignore_errors=True)
    fetch(REPO, REVISION, work)

    # Two files that have to be there, for two different reasons: the server API
    # is why this revision and not an older one, and include/esp/config.h is what
    # HAVE_CONFIG_H resolves to here -- ESP has no configure step.
    server = work / "lib" / "libsmb2.c"
    if not server.is_file() or "smb2_serve_port" not in server.read_text(encoding="utf-8"):
        die(f"{REVISION[:8]} has no smb2_serve_port(); this is not the revision "
            f"components/libsmb2 was written against.")
    if not (work / "include" / "esp" / "config.h").is_file():
        die(f"{REVISION[:8]} has no include/esp/config.h; ESP needs no configure "
            f"step only because upstream checks that file in.")

    apply_patches(work, root)

    (work / STAMP).write_text(want)
    shutil.rmtree(dest, ignore_errors=True)
    work.rename(dest)
    print(f"fetch-libsmb2: {COMPONENT} {REVISION[:8]} in external/{COMPONENT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
