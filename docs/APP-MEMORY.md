# An app's own memory

Design sketch for **R-P1.2**, the per-app heap arena. It is written before the
code because the decisions interact: the allocator, the ELF loader's teardown
and the reaper all change together, and there is no way to try it halfway.

## The problem, measured

An app's `malloc()` is the firmware's. Nothing records what it allocated, so when
the app ends there is nothing to give back — the exit path can free the ELF image
and the argv block because espix allocated those *itself*, and can free nothing
else. From a fresh boot with 13.2 MB of PSRAM free, **two Doom runs left 571 KB** —
about 7.7 MB a run, and only a reboot returns it (KNOWN-ISSUES).

Two more consequences of the same gap:

- A hard kill (`kill -9`, or the grace expiring) leaks the same way, plus the fds
  and driver handles (R-P1.3, R-P1.6).
- An app's fragmentation is the *system's* fragmentation: a long-lived app that
  churns allocations degrades the shared pool, and the next app pays.

## What has to be true

1. An app's allocations are attributable to that app.
2. Exit **and** kill give all of them back, in one step, without walking a list of
   thousands of blocks.
3. `free()` still works on a pointer that was allocated *before* any of this — by
   the loader, or by a library that ran during relocation.
4. The app sees no ABI change: it still calls `malloc`.
5. A board with no PSRAM still works.
6. The reaper (R-P1.6) is the single teardown point that invokes it.

## The mechanism is the resolver, not a table

`elf_loader`'s own libc table answers for `malloc` and is searched *before* any
table espix registers, so a registered entry can never shadow it. The resolver
runs first — the same seam `sleep`, `getenv` and `exit` already use. That is
**R-P1.1**, and it is worth doing on its own: it makes an app's allocations
*visible* before anything is done with them.

## The decision: tagged bookkeeping or a private arena

**A — tagged allocations.** Keep using the global heap; wrap `malloc` to record
each block and `free` to forget it; at exit, free whatever is left.

- No fixed size, no range checks, `free` semantics untouched.
- But the record costs memory per allocation, it is another structure a wild app
  can corrupt, and it does nothing about fragmentation.

**B — a private arena per process.** IDF exposes this directly:
`multi_heap_register(start, size)` returns a `multi_heap_handle_t` with its own
`multi_heap_malloc/free/realloc/get_info`. Carve a region from PSRAM, register
it, and route the app's allocations there.

- Reclamation becomes one operation — destroy the heap — rather than a walk.
- An app's fragmentation is its own, and the region *is* the budget the roadmap
  asks for ("budgets before a reaper").
- But `multi_heap_register` cannot grow: the size is chosen up front per
  process. And `free()` needs a range check to tell an arena pointer from a
  global one.

**Recommendation: B.** The reclamation-is-one-operation property is the whole
point, and the fixed size is not a defect if it is done the right way: **create
the arena lazily on the app's first allocation, sized from a budget, and destroy
it on exit.** A process that never allocates costs nothing, so a 12-slot table
does not reserve twelve arenas.

## The hazards, in the order they will bite

1. **Pointers from before the arena.** The ELF image, argv and env blocks, and
   anything a library allocated during relocation. `free()` must not send those
   to the arena — hence the range check, which doubles as the answer to "is this
   mine". `realloc` across the boundary is the real case: a global pointer grown
   must be copied into the arena, and vice versa.
2. **The loader's own allocations stay global**, deliberately. Their lifetime is
   the loader's and `espix_proc_release_resources()` already ends it; putting
   them in an arena the app can exhaust would make loading fail when the app is
   merely busy.
3. **DMA.** An arena in PSRAM means more app buffers are in PSRAM, so more apps
   meet the cache/DMA contract in GOTCHAS.md. Nothing here changes that rule;
   it makes it more likely to be met, and the docs should say so where an app
   author will read it.
4. **Threads.** An app's pthreads share its arena — which is correct — but the
   arena handle has to be reachable from *any* task of that process, and today a
   process is identified by a single `info.task` handle. An app thread is not
   that task, so the lookup needs a per-task → process path that does not exist
   yet. This is the one structural surprise in the design.
5. **No PSRAM.** No arena; the global heap; the leak stays. Documented rather
   than pretended.
6. **Sizing.** A budget per process, and a defined answer for exhaustion: fall
   through to the global heap (reclaimable only by reboot) or fail the
   allocation. Failing is honest and makes the budget real, but a game that
   wants 6 MB is not served. Falling through keeps things working and keeps the
   leak. **This is the question to answer first**, because it decides whether the
   arena is a budget or a hint.

## What it unblocks

- The Doom leak, the hard-kill leak, and "nothing releases the screen" all become
  one mechanism (R-P1.2 + R-P1.6 + R-P1.7).
- fd ownership (R-P1.3) rides the same teardown.
- The reclaimable-cache registry in ROADMAP needs the same idea — a subsystem
  registering what may be evicted — so the interface is worth designing once.
- Services (R-P6.1) need a process whose exit reclaims everything, or a service
  manager is just a way to leak on a schedule.

## What it does not solve

- **Isolation.** On the S3 an arena is bookkeeping, not a boundary: a wild write
  still reaches the kernel. That is R-P3's territory and it needs the trap.
- A pointer handed to another process — there is no such thing today, and this
  does not create one.
- The unexplained PSRAM free-list corruption (KNOWN-ISSUES), if that is
  kernel-side rather than app-side. An arena would make it *easier* to attribute,
  which is a reason to do this sooner.

## Open questions

1. **Exhaustion**: fall through to the global heap, or fail? (Decides whether
   this is a budget or a hint.)
2. **Size**, and whether it is per-process or a shared pool with per-process
   accounting. A pool shares the fragmentation; per-process isolates it.
3. Should `free()` on a foreign pointer be a quiet fall-through, or logged? It
   has to *work* — that is hazard 1 — but whether it is worth a line is a
   separate question.
4. Does an app that never calls `malloc` get an arena? (Recommendation: no —
   lazy creation, so the answer is "no, until it asks".)
5. Is arena usage worth a column in `ps`, or is that noise? The budget is
   invisible until it is exceeded, and by then something has already failed.

## Staging

- **R-P1.1** publish `malloc/calloc/realloc/free/strdup` through the resolver,
  PSRAM-first. No arena; this alone makes allocations visible and is the seam
  everything else needs.
- **R-P1.2** the arena: lazy creation, range-checked `free`, whole-arena
  reclamation on exit and on kill.
- **R-P1.6** the reaper as the single teardown point that calls it, which is also
  where R-P1.3 (fds) and R-P1.7 (the screen) attach.
- **R-P1.5** the live table separated from the completed log, so a slot is
  recycled when it is reaped rather than when the table fills.
