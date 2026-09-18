#!/usr/bin/env python3
"""
Fail the build if a symbol elf_loader promises to apps is not in the image.

An app resolves its undefined symbols against tables compiled into the firmware,
so a name is usable only if it is *in the loaded image*. The names espix does not
publish itself come from elf_loader's own two tables (esp_elf_symbol.c, the libc
names and the IDF ones), and each entry there is what anchors its function into the
image -- so they are present unless something removes them, which is what this
looks for:

  * CONFIG_ELF_LOADER_LIBC_SYMBOLS or CONFIG_ELF_LOADER_ESPIDF_SYMBOLS switched
    off, which drops a whole table and every name in it;
  * an elf_loader version that dropped a name. The pin in main/idf_component.yml
    makes that a deliberate act rather than a surprise, but it is still an act,
    and what an app sees for it is "undefined symbol" -- at load, on a device.

**Why this is not a `_Static_assert`.** Naming a function in an assertion does
not prove its definition is in the image; it only proves the header declares it.
Referencing it to prove more is precisely what pulls it in, which is the cost the
assertion was meant to avoid. Reading the linked ELF costs the image nothing and
proves the thing that actually matters, so the check runs there. The list of names
this guards, and the reasoning, live in components/espix_proc/abi_libc.c.

Run by the build, after the link; safe to run by hand.
"""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "managed_components/espressif__elf_loader/src/esp_elf_symbol.c"


def die(msg):
    print(f"check-abi: {msg}", file=sys.stderr)
    sys.exit(1)


def export_names(text, include_conditional=False):
    """The names a table exports.

    The libc-conditional block in elf_loader's table chooses between newlib's own
    ABI names and picolibc wrappers, and which set is in the image depends on
    CONFIG_LIBC_PICOLIBC -- so the *presence* check reads only the unconditional
    exports. The names are the same either way in the one place that matters for
    the duplicate check, so that one asks for all of them.

    Nested #if is not expected here, but it is tracked rather than ignored so a
    future one cannot quietly hide names from this check.
    """
    names = []
    depth = 0

    for line in text.splitlines():
        stripped = line.strip()
        if re.match(r"#\s*(if|ifdef|ifndef)", stripped):
            depth += 1
            continue
        if re.match(r"#\s*endif", stripped):
            depth = max(0, depth - 1)
            continue
        if depth > 0 and not include_conditional:
            continue

        m = re.match(r"ESP_ELFSYM_EXPORT\((\w+)\)", stripped)
        if m:
            names.append(m.group(1))

    return names


def elf_symbols(nm, elf):
    """Every symbol the ELF defines. Undefined ones are left out on purpose: 'U'
    is the failure this is looking for, not evidence against it."""
    try:
        out = subprocess.run([nm, str(elf)], capture_output=True, text=True,
                             check=True)
    except FileNotFoundError:
        die(f"{nm} does not exist; pass --nm from the toolchain")
    except subprocess.CalledProcessError as exc:
        die(f"{nm} failed on {elf}: {exc.stderr.strip()}")

    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0] in ("U", "u", "w", "v"):
            continue
        syms.add(parts[-1])

    return syms


def espix_exports():
    """Every name espix's own tables export, and where each one is written.

    `ABI_SYM` is deliberately not collected: those tables belong to the resolver,
    which runs *before* the loader's own, so shadowing a name below is exactly what
    they are for. An ABI_SYM entry that is not shadowing anything is merely
    redundant, not unreachable.
    """
    found = {}
    pattern = re.compile(r"ESP_ELFSYM_EXPORT\((\w+)\)")

    for path in sorted(REPO.glob("components/*/abi*.c")) + \
                sorted(REPO.glob("components/*/abi*.cpp")):
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            m = pattern.search(line)
            if m:
                found.setdefault(m.group(1), f"{path.relative_to(REPO)}:{lineno}")

    return found


def duplicate_entries(below):
    """Names espix publishes that elf_loader's tables answer for first.

    They resolve, so nothing looks wrong -- the app simply gets the layer below and
    espix's entry is dead weight that reads as a promise. Three were in the tree
    until this check existed.
    """
    return [(name, where) for name, where in sorted(espix_exports().items())
            if name in below]


def main():
    argv = sys.argv[1:]
    elf = nm = None

    if "--elf" in argv:
        i = argv.index("--elf")
        elf = Path(argv[i + 1]) if i + 1 < len(argv) else None
    if "--nm" in argv:
        i = argv.index("--nm")
        nm = argv[i + 1] if i + 1 < len(argv) else None

    if elf is None or nm is None:
        die("usage: check-abi.py --elf <espix.elf> --nm <nm>")
    if not elf.is_file():
        die(f"{elf} does not exist")
    if not SOURCE.is_file():
        die(f"{SOURCE} does not exist; has the component moved?")

    source = SOURCE.read_text(encoding="utf-8")
    problems = []

    below_all = set(export_names(source, include_conditional=True))
    dupes = duplicate_entries(below_all)
    if dupes:
        problems.append(
            "published here and answered by elf_loader's own tables first, which are\n"
            "  searched before these, so the entries below are unreachable -- drop each\n"
            "  one, or move the intent to the resolver (abi_signal.c) if it was meant to\n"
            "  override:\n    "
            + "\n    ".join(f"{name:<24} {where}" for name, where in dupes)
        )

    promised = export_names(source)
    if not promised:
        die(f"no exports found in {SOURCE}; the file moved or changed shape")

    have = elf_symbols(nm, elf)
    missing = [name for name in promised if name not in have]
    if missing:
        problems.append(
            "published to apps but not in the image, so an app calling one fails to\n"
            "  load with 'undefined symbol':\n    "
            + "\n    ".join(missing)
            + "\n  Check CONFIG_ELF_LOADER_LIBC_SYMBOLS / _ESPIDF_SYMBOLS, and the\n"
            "  elf_loader pin in main/idf_component.yml."
        )

    if problems:
        die("\n".join(problems))

    print(f"check-abi: {len(promised)} names promised a layer below are in the image,"
          f" and none of the {len(espix_exports())} espix publishes is shadowed by them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
