#!/usr/bin/env python3
"""
Keep _lseek_r's 32-bit ABI, now that off_t is 64 bits.

cmake/offt64.h widens off_t for the whole firmware, which is what lets a file over
4 GiB be reported and seeked. IDF's `_lseek_r` is an alias for `esp_vfs_lseek`, so
it follows the type -- but two of its callers do not, and cannot:

  * the **prebuilt** newlib libc. Its stdio.o carries the seek logic that fseek()
    and fseeko() end up in, and it was compiled when off_t was a 32-bit long, so
    it passes 32 bits;
  * the ROM's libc stub table, which declares an int offset for the same reason
    (esp_rom/<target>/rom/libc_stubs.h).

Followed the type, `_lseek_r` reads 64 bits where those callers wrote 32, and every
fseek() lands somewhere else. Measured before this was written, on a 12 KB upload:
`sftp: write failed: seek failed`.

So the boundary keeps the width it has always had and the widening happens inside:
`_lseek_r` takes and returns an int, and calls the 64-bit `esp_vfs_lseek` beneath
it. espix's own lseek(), pread() and pwrite() are 64-bit and unaffected, which is
where a file over 4 GiB is actually reached. stdio's seeking stays limited to
4 GiB -- a fact about the libc this is linked against rather than a choice, and the
reason espix's SFTP transfer path has to move off FILE* to go beyond it. See
docs/KNOWN-ISSUES.md.

Three files, not one. The weak declaration has to agree with the strong
definition, so reent_syscalls.c (a weak alias to syscall_not_implemented, for
builds without the VFS) and vfs_calls.c (the definition) both carry the width. The
third is the toolchain's own reent.h, which declares `_lseek_r` in terms of
`_off_t` — the very type cmake/offt64.h widens — and so has to be pinned to the
width the library was actually built with. That one is outside IDF and is the
reason the hook passes --toolchain-root.

**Temporary by construction.** tools/esp_libc-lseek-abi.patch is the same change
as a plain diff, ready to send to espressif/esp-idf. When it lands, delete both
files and the execute_process() block in the top-level CMakeLists.txt.
"""

import os
import sys
from pathlib import Path

EXPECTED_IDF = "6.1.0"

# The toolchain's own reent.h declares _lseek_r with _off_t, which is the type a
# project may have widened -- and cannot, because the libc that calls it was built
# with 32-bit _off_t. Pinning the declaration to the width the library implements
# is correct for every project, with or without a widened off_t, which is why this
# one file is worth touching outside IDF.
TOOLCHAIN = {
    "include/reent.h": (
        "extern _off_t _lseek_r (struct _reent *, int, _off_t, int);",
        "extern int _lseek_r (struct _reent *, int, int, int);",
    ),
}

# file -> (old, new). The definition, then the declaration that must match it.
PATCHES = {
    "components/vfs/vfs_calls.c": (
        'off_t _lseek_r(struct _reent *r, int fd, off_t size, int mode)\n'
        '    __attribute__((alias("esp_vfs_lseek")));',
        '''/*
 * A 32-bit ABI, deliberately, and not an alias for esp_vfs_lseek.
 *
 * _lseek_r is called by the prebuilt newlib libc -- stdio.o carries the seek
 * logic fseek() and fseeko() end up in -- which was compiled when off_t was a
 * 32-bit long, and by the ROM's libc stub table, which declares an int offset for
 * the same reason. A project may widen off_t; if this followed the type, those
 * callers would pass 32 bits where 64 are read and every fseek() would land
 * somewhere else. So the boundary keeps its width, the widening happens here, and
 * espix's own lseek()/pread()/pwrite() are 64-bit and unaffected.
 */
int _lseek_r(struct _reent *r, int fd, int offset, int whence)
{
    const off_t at = esp_vfs_lseek(r, fd, (off_t)offset, whence);

    /* Round-trip rather than a limit constant: it is the same question, and it
     * needs no header. */
    if (at != (off_t)(int)at) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)at;
}''',
    ),
    "components/esp_libc/src/reent_syscalls.c": (
        "off_t _lseek_r(struct _reent *r, int fd, off_t size, int mode)",
        "int _lseek_r(struct _reent *r, int fd, int offset, int whence)",
    ),
}

def die(msg):
    print(f"patch-libc-offt: {msg}", file=sys.stderr)
    sys.exit(1)


def arg_path(argv, flag):
    """A path the CMake hook passed, or None."""
    if flag in argv:
        i = argv.index(flag)
        if i + 1 < len(argv):
            return Path(argv[i + 1])
    return None


def idf_root(argv):
    """Where IDF is: --idf-path as the CMake hook passes it, else IDF_PATH."""
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
            f"  Check the anchors in tools/patch-libc-offt.py against the new "
            f"source, then update EXPECTED_IDF."
        )


def main():
    idf = idf_root(sys.argv)

    for rel, (old, new) in PATCHES.items():
        target = idf / rel
        if not target.is_file():
            die(f"{target} does not exist; is the IDF path right?")

        text = target.read_text(encoding="utf-8")

        # Already patched? Configure runs this every build, so it has to be a
        # no-op from the second time on. Tested against the replacement rather
        # than a marker comment, because one of the two files' replacement is a
        # declaration line and has nowhere to put a marker.
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
        print(f"patch-libc-offt: gave _lseek_r its 32-bit ABI back in {rel}")

    # The toolchain, when the build hands us its sysroot. Without it the two
    # above still apply, and the compile then stops on the declaration instead --
    # which is a clearer failure than a build that links and seeks wrongly.
    toolchain = arg_path(sys.argv, "--toolchain-root")
    if toolchain is None:
        return 0

    for rel, (old, new) in TOOLCHAIN.items():
        target = toolchain / rel
        if not target.is_file():
            die(f"{target} does not exist; is --toolchain-root right?")

        text = target.read_text(encoding="utf-8")
        if new in text:
            continue
        if text.count(old) != 1:
            die(
                f"expected exactly one anchor in {rel}, found "
                f"{text.count(old)}.\n  The file moved; re-read it before "
                f"changing this script."
            )

        target.write_text(text.replace(old, new), encoding="utf-8")
        print(f"patch-libc-offt: pinned _lseek_r's declaration in {rel}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
