#!/usr/bin/env python3
"""
Check the app ABI at build time: every name an app may resolve, and every entry
that can never be reached.

An app resolves its undefined symbols against tables compiled into the firmware,
so a name is usable only if it is *in the loaded image* and *published before
anything else answers for it*. Two things are checked, and they fail in opposite
directions:

  * a name elf_loader promises but the image does not define -- the app fails to
    load with "undefined symbol", on a device. Caused by
    CONFIG_ELF_LOADER_LIBC_SYMBOLS / _ESPIDF_SYMBOLS being off, or by an
    elf_loader version that dropped a name.

  * an entry espix publishes that something searched earlier already answers --
    it resolves, so nothing looks wrong, and espix's entry is dead weight that
    reads as a promise.

**The resolution order is the whole check**, and it is one list:

  1. espix's symbol resolver  (abi_resolver.c, before everything)
  2. elf_loader's own libc table, then its IDF table  (esp_elf_symbol.c)
  3. espix's registered tables, earliest-registered first
  4. dlmod  (off)

So an espix *table* entry for a name the loader answers is dead, while an espix
*resolver* entry for the same name is exactly the point of the resolver. Three
entries were dead in the tree before this compared the lists; two more were
found by the version that also reads ABI_SYM and the written-out tables.

Reading the linked ELF costs the image nothing, and it proves the thing that
actually matters, which a _Static_assert cannot: naming a function in an
assertion proves only that the header declares it. The names this guards, and
the reasoning, live in components/espix_proc/abi_libc.c.

Run by the build, after the link; safe to run by hand.
"""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "managed_components/espressif__elf_loader/src/esp_elf_symbol.c"
PROC = REPO / "components/espix_proc/proc.c"

# The two ways a name reaches an app. Separate because their places in the
# resolution order differ, which is what decides whether an entry is live.
RESOLVER = "resolver"
REGISTERED = "registered"


def die(msg):
    print(f"check-abi: {msg}", file=sys.stderr)
    sys.exit(1)


def strip_comments(text):
    """Comments removed; everything else, string literals included, kept.

    Both halves matter. A comment that *mentions* an export is not one --
    abi_env.c's header says ESP_ELFSYM_EXPORT(getenv) in prose, and the scanner
    that matched raw lines counted it, which inflated its total and could have
    failed the build for a name the loader answers for. And the names inside
    ABI_SYM("...") and { "name", &sym } *are* string literals, so they have to
    survive -- which is also why this is a scanner rather than a regex: a "//"
    inside a string is not a comment.

    Removed characters become spaces, newlines excepted, so every offset and
    line number below still points at the line it came from.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]

        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n":
                    # Unterminated literal: do not swallow the rest of the file.
                    i += 1
                    break
                i += 1
            continue

        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue

        out.append(c)
        i += 1

    return "".join(out)


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

    for line in strip_comments(text).splitlines():
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


def abi_sources():
    return (sorted(REPO.glob("components/*/abi*.c"))
            + sorted(REPO.glob("components/*/abi*.cpp")))


def module_of(relpath):
    """The registration token for an abi file: abi_cxx.cpp -> cxx."""
    stem = Path(relpath).stem
    return stem[len("abi_"):] if stem.startswith("abi_") else ""


def registration_order():
    """The order espix registers its ABI modules in, as tokens.

    Read from proc.c rather than written down here, because the two must not be
    able to disagree: this order is what decides which of two entries for one
    name is the live one. espix_net registers from its own init, after all of
    these, so it is not in this list and sorts last.
    """
    if not PROC.is_file():
        return []
    return re.findall(r"espix_proc_abi_(\w+)_register\s*\(\s*\)",
                      strip_comments(PROC.read_text(encoding="utf-8")))


# The three ways a name is written into a table. ESP_ELFSYM_EXPORT and the
# written-out entries are the *same* mechanism -- both are tables handed to
# esp_elf_register_symbol() -- and are grouped so a name in two of them is seen
# as the duplicate it is.
PUB_ESP = re.compile(r"ESP_ELFSYM_EXPORT\((\w+)\)")
PUB_RESOLVER = re.compile(r'ABI_SYM\(\s*"([^"]+)"')
PUB_EXPLICIT = re.compile(
    r'^\s*\{\s*"([A-Za-z_]\w*)",\s*(?:\(const void \*\)|\(void \*\)|reinterpret_cast)')


def publications():
    """Every name espix publishes: name -> [(kind, "file:line"), ...]."""
    found = {}
    for path in abi_sources():
        rel = path.relative_to(REPO)
        code = strip_comments(path.read_text(encoding="utf-8"))
        for lineno, line in enumerate(code.splitlines(), 1):
            for kind, pattern in ((RESOLVER, PUB_RESOLVER),
                                  (REGISTERED, PUB_ESP),
                                  (REGISTERED, PUB_EXPLICIT)):
                for m in pattern.finditer(line):
                    found.setdefault(m.group(1), []).append(
                        (kind, f"{rel}:{lineno}"))
    return found


def rank(where, order):
    """Sort key: how early this entry is searched. Lower is earlier."""
    module = module_of(where.split(":")[0])
    return order.index(module) if module in order else len(order)


def dead_entries(found, below, order):
    """Entries that can never be reached, each with the entry that beats it.

    The chain below is the resolution order written out for one name: resolver
    entries first, then elf_loader's own tables, then espix's registered tables.
    Anything after the winner is dead, and the reason says which rule did it --
    because the fix differs: a shadowed table entry is deleted, while a name
    that was *meant* to override belongs in the resolver instead.
    """
    dead = []

    for name, entries in sorted(found.items()):
        resolvers = sorted((w for k, w in entries if k == RESOLVER),
                           key=lambda w: rank(w, order))
        tables = sorted((w for k, w in entries if k == REGISTERED),
                        key=lambda w: rank(w, order))

        if resolvers:
            live = resolvers[0]
            for where in resolvers[1:]:
                dead.append((name, where, live,
                             "an earlier resolver entry"))
            for where in tables:
                dead.append((name, where, live,
                             "the resolver is searched before every table"))
        elif name in below:
            for where in tables:
                dead.append((name, where, "elf_loader's own table",
                             "it is searched before espix's tables"))
        elif tables:
            live = tables[0]
            for where in tables[1:]:
                dead.append((name, where, live, "an earlier-registered table wins"))

    return dead


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

    order = registration_order()
    if not order:
        die(f"no ABI registrations found in {PROC}; it moved or changed shape")

    found = publications()
    below_all = set(export_names(source, include_conditional=True))

    dead = dead_entries(found, below_all, order)
    if dead:
        problems.append(
            "published by espix but unreachable, because something earlier in the\n"
            "  resolution order already answers for the name. Each line is the dead\n"
            "  entry, then what beats it. Drop the dead one -- or, if it was meant to\n"
            "  override, move it to the resolver (abi_resolver.c):\n"
            + "\n".join(f"    {name:<24} {where}\n      {live} wins -- {why}"
                         for name, where, live, why in dead)
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

    published = sum(len(entries) for entries in found.values())
    print(f"check-abi: {len(promised)} names promised a layer below are in the image;"
          f" {len(found)} names published by espix in {published} entries across"
          f" {len(abi_sources())} files, none of them shadowed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
