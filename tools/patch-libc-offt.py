#!/usr/bin/env python3
"""
Keep _lseek_r's 32-bit ABI, now that off_t is 64 bits.

cmake/offt64.h widens the public off_t for the whole firmware, which is what lets a
file over 4 GiB be reported and seeked -- and redirects every lseek() it compiles to
espix's 64-bit lseek64. The plain `lseek` symbol has to stay 32-bit, because the
*prebuilt* picolibc's fdopen() and fseek() bind to it by name and were compiled
with a 32-bit off_t. That `lseek` is IDF's, in esp_libc's syscalls.c, and esp_libc
is compiled without the force-include for exactly that reason; it calls
`_lseek_r` with a 32-bit offset.

IDF defines `_lseek_r` in vfs_calls.c as an alias for `esp_vfs_lseek`, whose
signature follows off_t -- 64 bits in this build. Left alone, esp_libc's 32-bit
call would land in a 64-bit function: the offset's high half and the whence would
be read from registers nobody set, and every fseek() would go somewhere else.
Measured, under newlib and the same mechanism: a 12 KB SFTP upload failing with
`sftp: write failed: seek failed`.

So this turns `_lseek_r` into a real 32-bit function that calls the 64-bit
`esp_vfs_lseek` beneath it, and fails a result that does not fit with EOVERFLOW,
as a 32-bit lseek() does on Linux. One file, in IDF; nothing in the toolchain.

**Temporary by construction.** tools/esp_libc-lseek-abi.patch is the same change
as a plain diff, ready to send to espressif/esp-idf. When it lands, delete both
files and the execute_process() block in the top-level CMakeLists.txt.
"""

import os
import sys
from pathlib import Path

EXPECTED_IDF = "v6.1"

_LSEEK_R = """/*
 * A 32-bit ABI, deliberately, and not an alias for esp_vfs_lseek.
 *
 * Its caller is esp_libc's lseek(), the one the prebuilt picolibc's fdopen() and
 * fseek() bind to by name, and that library was compiled with a 32-bit off_t. A
 * project may widen off_t; if this followed the type, a 32-bit offset would be
 * read as 64 and every fseek() would land somewhere else. So the boundary keeps
 * its width and the widening happens here.
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
}"""

# file -> (original, patched, [earlier patched forms to upgrade in place]).
PATCHES = {
    "components/vfs/vfs_calls.c": (
        'off_t _lseek_r(struct _reent *r, int fd, off_t size, int mode)\n'
        '    __attribute__((alias("esp_vfs_lseek")));',
        _LSEEK_R,
        ["/*\n * A 32-bit ABI, deliberately, and not an alias for esp_vfs_lseek.\n *\n * _lseek_r is called by the prebuilt newlib libc -- stdio.o carries the seek\n * logic fseek() and fseeko() end up in -- which was compiled when off_t was a\n * 32-bit long, and by the ROM's libc stub table, which declares an int offset for\n * the same reason. A project may widen off_t; if this followed the type, those\n * callers would pass 32 bits where 64 are read and every fseek() would land\n * somewhere else. So the boundary keeps its width, the widening happens here, and\n * espix's own lseek()/pread()/pwrite() are 64-bit and unaffected.\n */\nint _lseek_r(struct _reent *r, int fd, int offset, int whence)\n{\n    const off_t at = esp_vfs_lseek(r, fd, (off_t)offset, whence);\n\n    /* Round-trip rather than a limit constant: it is the same question, and it\n     * needs no header. */\n    if (at != (off_t)(int)at) {\n        errno = EOVERFLOW;\n        return -1;\n    }\n    return (int)at;\n}"],
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

    for rel, (old, new, earlier) in PATCHES.items():
        target = idf / rel
        if not target.is_file():
            die(f"{target} does not exist; is the IDF path right?")

        text = target.read_text(encoding="utf-8")

        # Already patched? Configure runs this every build, so it has to be a
        # no-op from the second time on.
        if new in text:
            continue

        # Patched by an earlier version of this script: same code, older words.
        anchor = next((e for e in earlier if e in text), old)
        if text.count(anchor) != 1:
            die(
                f"expected exactly one anchor in {rel}, found "
                f"{text.count(anchor)}.\n  The file moved; re-read it before "
                f"changing this script."
            )

        check_version(idf)
        target.write_text(text.replace(anchor, new), encoding="utf-8")
        print(f"patch-libc-offt: gave _lseek_r its 32-bit ABI back in {rel}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
