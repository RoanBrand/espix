#!/usr/bin/env python3
"""
Make elf_loader apply the addend of an Xtensa GLOB_DAT/JMP_SLOT relocation.

An Xtensa app reaches a firmware data symbol through a literal-pool word, and when
the reference is to an offset into that symbol rather than its start, the linker
records the offset as the relocation's addend: `R_XTENSA_GLOB_DAT _ctype_b + 80`.
The value the word must hold is symbol + addend -- what uClibc's Xtensa ld.so
writes, and what elf_loader's own RISC-V port does for R_RISCV_32. Its Xtensa port
writes the symbol alone, so every such reference lands `addend` bytes early.

The one that showed is ctype. Every ctype macro indexes a table at an offset from
its start: newlib's `(_ctype_ + 1)[c]`, picolibc's `(_ctype_b + 127 + 1)[c]`. So on
the S3 islower('a') read the entry for '`' and answered no, toupper() was the
identity, and Doom's lump lookup missed names that were there -- first diagnosed as
an Xtensa codegen bug, worked around in Doom by forcing the ctype calls. Under
picolibc the addend is 128, so the same fault would misclassify every character in
every S3 app that uses a ctype macro. RISC-V has never had it: its loader adds the
addend.

**Temporary by construction.** tools/elf_loader-xtensa-addend.patch is the same
change as a plain diff, ready to send to espressif/esp-idf-extra-components. When
it lands, delete this script, that patch and the execute_process() hook in
CMakeLists.txt, and bump the version.

Loud, never silent: a version this was not written against, or an anchor that
moved, stops the build -- skipping quietly would bring back a fault that looks
like an application bug.
"""

import re
import sys
from pathlib import Path

COMPONENT = "espressif__elf_loader"
EXPECTED_VERSION = "1.3.5"

FILE = "src/arch/esp_elf_xtensa.c"

OLD = """    case R_XTENSA_GLOB_DAT:
    case R_XTENSA_JMP_SLOT:
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, addr);
#else
        *where = addr;
#endif
        break;"""

NEW = """    case R_XTENSA_GLOB_DAT:
    case R_XTENSA_JMP_SLOT:
        /* S + A: a reference into a symbol, not to its start, carries the
         * offset as the addend -- `_ctype_b + 128` is how every ctype macro
         * reaches its table. */
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, addr + rela->addend);
#else
        *where = addr + rela->addend;
#endif
        break;"""


def die(msg):
    print(f"patch-elf-loader: {msg}", file=sys.stderr)
    sys.exit(1)


def check_version(root):
    """Refuse to patch a version this was not written against."""
    lock = root / "dependencies.lock"
    if not lock.exists():
        return  # first configure, before the manager has written one
    m = re.search(r"^  espressif/elf_loader:\n(?:.*?\n)*?^    version: (\S+)$",
                  lock.read_text(encoding="utf-8"), re.M)
    if m and m.group(1).strip("'\"") != EXPECTED_VERSION:
        die(
            f"espressif/elf_loader is {m.group(1)}, but this patch was written "
            f"for {EXPECTED_VERSION}.\n"
            f"  Re-check the anchor in tools/patch-elf-loader.py against the new "
            f"source, then update EXPECTED_VERSION.\n"
            f"  If the version now applies the addend itself, delete this script "
            f"and the hook in CMakeLists.txt."
        )


def main():
    root = Path(__file__).resolve().parent.parent
    comp = root / "managed_components" / COMPONENT

    if not comp.is_dir():
        # Nothing downloaded yet. Not an error: CMake reconfigures after the
        # component manager runs, and this will be called again.
        return 0

    target = comp / FILE
    if not target.is_file():
        die(f"expected {target} to exist")

    text = target.read_text(encoding="utf-8")
    if NEW in text:
        return 0  # configure runs this every build

    check_version(root)
    if text.count(OLD) != 1:
        die(
            f"expected exactly one anchor in {FILE}, found {text.count(OLD)}.\n"
            f"  The upstream source moved. Re-derive the anchor rather than "
            f"loosening this check."
        )

    target.write_text(text.replace(OLD, NEW), encoding="utf-8")
    print(f"patch-elf-loader: applied the Xtensa GLOB_DAT addend in "
          f"{COMPONENT} {EXPECTED_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
