#!/usr/bin/env python3
"""Split the apps' libc.a into the part every app shares and the part each app keeps.

    tools/libc-split.py --tool <prefix> --libc <libc.a> --os <libespix.a>
                        [--os-names user/libc-os.<target>] [--own-alloc] --out <dir> [--why NAME]

A member is *private* (linked into each app) when it owns writable data, calls
the allocator, calls the OS layer, or needs a private member. Everything else is
*shared*: pure code, one copy for every app. The OS layer is libespix plus what
the kernel provides (--os-names): what it defines is never taken from libc, and
calling it makes a member private, except the hooks the shared part may call
(SHARED_HOOKS). The allocator is the OS layer's, unless --own-alloc keeps libc's
in each app; either way a member that allocates is private.

Writes <dir>/libc-app.a (private), <dir>/libc-shared.a (shared), and
<dir>/libc.syms (the shared part's names: the ABI apps bind to).
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

ALLOC = {'malloc', 'free', 'calloc', 'realloc', 'reallocarray', 'memalign',
         'aligned_alloc', 'posix_memalign', 'malloc_usable_size', 'valloc', 'pvalloc'}

# What the shared part may reach outside itself: one per-thread errno, and stdio's locks.
SHARED_HOOKS = {'__errno', '__lock___libc_recursive_mutex',
                '__retarget_lock_init', '__retarget_lock_init_recursive',
                '__retarget_lock_close', '__retarget_lock_close_recursive',
                '__retarget_lock_acquire', '__retarget_lock_acquire_recursive',
                '__retarget_lock_try_acquire', '__retarget_lock_try_acquire_recursive',
                '__retarget_lock_release', '__retarget_lock_release_recursive'}

WRITABLE = re.compile(r'^\.(s?data|s?bss)(\.|$)')

# Members holding picolibc's atexit runner (members() fills it).
ONEXIT = set()


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def members(tool, archive):
    """{member: (defined globals, undefined names, has writable data)}."""
    defs, undefs = collections.defaultdict(set), collections.defaultdict(set)
    for line in run(tool + 'nm', '-A', '-g', archive).splitlines():
        m = re.match(r'^[^:]+:([^:]+):\s*([0-9a-fA-F]+\s+)?([A-Za-z])\s+(\S+)$', line)
        if not m:
            continue
        mem, addr, kind, name = m.groups()
        if kind == 'U' or addr is None:
            undefs[mem].add(name)   # weak ones too: a linker-script symbol is the app's
        else:
            defs[mem].add(name)
    writable, cur = set(), None
    for line in run(tool + 'objdump', '-h', archive).splitlines():
        m = re.match(r'^(\S+\.o):\s+file format', line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s', line)
        if m and cur and int(m.group(2), 16):
            if WRITABLE.match(m.group(1)):
                writable.add(cur)
            if m.group(1) == '.fini_array_onexit':
                ONEXIT.add(cur)
    names = set(defs) | set(undefs)
    return {n: (defs[n], undefs[n], n in writable) for n in names}


def split(libc, os_defs, rt_defs):
    # A member defining anything the OS layer provides is never linked (picolibc's own heap), nor one
    # that needs a name only a dropped member defines (its mallinfo).
    dropped = {m for m, (d, _, _) in libc.items() if d & os_defs}
    changed = True
    while changed:
        live = {n for m, (d, _, _) in libc.items() if m not in dropped for n in d}
        dropped_names = {n for m in dropped for n in libc[m][0]} - live - os_defs
        more = {m for m, (_, u, _) in libc.items() if m not in dropped and u & dropped_names}
        changed = bool(more)
        dropped |= more
    owner = {}
    for mem, (d, _, _) in libc.items():
        if mem not in dropped:
            for name in d - os_defs:
                owner.setdefault(name, mem)
    os_calls = os_defs - SHARED_HOOKS - rt_defs
    # A name neither libc, the OS nor libgcc defines comes from the app's own link (its init arrays).
    def app_only(u):
        return any(n not in owner and n not in os_defs and n not in rt_defs for n in u)
    private = {m for m, (_, u, w) in libc.items()
               if m not in dropped and (w or (u & ALLOC) or (u & os_calls) or app_only(u))}
    changed = True
    while changed:
        changed = False
        for mem, (_, u, _) in libc.items():
            if mem not in private and mem not in dropped and any(owner.get(n) in private for n in u):
                private.add(mem)
                changed = True
    shared = set(libc) - private - dropped
    return shared, private, dropped


def pack(tool, archive, chosen, path, tmp, onexit=(), onexit_to='.fini_array'):
    os.makedirs(tmp, exist_ok=True)
    subprocess.run([tool + 'ar', 'x', os.path.abspath(archive)], cwd=tmp, check=True)
    objs = sorted(chosen)
    # atexit's runner is a destructor in a section only picolibc's own linker script folds in.
    for obj in sorted(set(onexit) & set(objs)):
        subprocess.run([tool + 'objcopy', '--rename-section', '.fini_array_onexit=' + onexit_to, obj],
                       cwd=tmp, check=True)
    if os.path.exists(path):
        os.remove(path)
    if objs:
        subprocess.run([tool + 'ar', 'rcs', os.path.abspath(path)] + objs, cwd=tmp, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tool', required=True, help='binutils prefix, e.g. /path/xtensa-esp-elf-')
    ap.add_argument('--libc', required=True)
    ap.add_argument('--os', required=True, help='the OS layer archive (libespix.a)')
    ap.add_argument('--os-names', help="a file of the names the kernel provides (user/libc-os.<target>)")
    ap.add_argument('--rt', required=True, help="the compiler's runtime (libgcc.a), linked wherever needed")
    ap.add_argument('--out', required=True)
    ap.add_argument('--why', metavar='NAME', help='say why NAME is private, and stop')
    ap.add_argument('--own-alloc', action='store_true',
                    help="libc's allocator is the app's own (S31), not the OS layer's")
    ap.add_argument('--dtors', default='.fini_array',
                    help="the section destructors go in: .fini_array, or Xtensa's .dtors.00000 (first to run)")
    a = ap.parse_args()

    libc = members(a.tool, a.libc)
    os_defs = set().union(*(d for d, _, _ in members(a.tool, a.os).values()))
    if not a.own_alloc:
        os_defs |= ALLOC
    if a.os_names:
        with open(a.os_names) as f:
            os_defs |= {n for line in f for n in line.split('#', 1)[0].split()}
    rt_defs = set().union(*(d for d, _, _ in members(a.tool, a.rt).values()))
    shared, private, dropped = split(libc, os_defs, rt_defs)

    if a.why:
        owner = {n: m for m, (d, _, _) in libc.items() if m not in dropped for n in d - os_defs}
        name, seen = a.why, set()
        while name in owner and owner[name] in private and name not in seen:
            seen.add(name)
            mem = owner[name]
            d, u, w = libc[mem]
            if w:
                print(f'{name}: {mem} owns writable data')
                break
            if u & ALLOC:
                print(f'{name}: {mem} allocates ({", ".join(sorted(u & ALLOC))})')
                break
            calls = u & (os_defs - SHARED_HOOKS - rt_defs)
            if calls:
                print(f'{name}: {mem} calls the OS layer ({", ".join(sorted(calls))})')
                break
            nxt = sorted(n for n in u if owner.get(n) in private)
            print(f'{name}: {mem} needs {", ".join(nxt)}')
            name = nxt[0]
        return 0

    os.makedirs(a.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        pack(a.tool, a.libc, shared, os.path.join(a.out, 'libc-shared.a'), os.path.join(tmp, 's'))
        pack(a.tool, a.libc, private, os.path.join(a.out, 'libc-app.a'), os.path.join(tmp, 'p'), ONEXIT, a.dtors)
    names = sorted(set().union(*(libc[m][0] for m in shared)) if shared else [])
    with open(os.path.join(a.out, 'libc.syms'), 'w') as f:
        f.write(''.join(n + '\n' for n in names))
    print(f'libc-split: {len(shared)} shared members ({len(names)} names), '
          f'{len(private)} private, {len(dropped)} left to the OS layer')
    return 0


if __name__ == '__main__':
    sys.exit(main())
