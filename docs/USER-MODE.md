# User-mode apps on the S31

A design, written before the code (as [APP-MEMORY.md](APP-MEMORY.md) was for
R-P1). It says how an espix app would run in RISC-V user mode, in an address
space of its own, on the ESP32-S31, and in what order to build it. The S3 keeps
today's model; [the S3](#the-s3) says what the two builds share.

The numbers come from the `umode` spike (R-P8.6); its results table is in
[ROADMAP.md](ROADMAP.md#processes). The ABI figures come from an inventory of
the 406 names an app can resolve today.

## Why

Today an app runs in the kernel's privilege level and address space
([ARCHITECTURE.md](ARCHITECTURE.md#there-is-no-mmu-so-there-is-one-address-space)).
Everything that follows from that is a symptom of one missing thing, a boundary:

- a wild write corrupts the kernel, and a fault reboots the board;
- a signal handler runs only when the app calls in, because at any other moment
  its task may be inside kernel code holding a lock;
- SIGSTOP parks a process *in itself*, for the same reason;
- `fork()` is impossible, because an app's state is tangled with kernel frames on
  one stack;
- stacks are a fixed size chosen up front, and memory an app reserves is memory
  it holds.

The spike showed the S31 provides the boundary cheaply: 37 cycles from `mret`
into U-mode to the trap back, 32 cycles for a page fault, 11 cycles for a TLB
miss, ASIDs that work.

## The shape: M for the kernel, U for apps

FreeRTOS, ESP-IDF, the drivers and espix stay in M-mode, unchanged in
privilege. Apps run in U-mode. S-mode is not used.

S-mode would buy safe kernel access to user memory through `SUM` and traps
delegated past M. It would cost a translated kernel, mapped into every address
space, and a call boundary between espix (S) and IDF (M) around the VFS, lwIP
and every driver: a rewrite that protects nothing apps need. The one thing `SUM`
would fix, reading user pointers, has a cheaper answer in M (below).

## Address spaces

**One Sv32 table per process, mapping only that process's memory.** A thread of
the process runs with its `satp`; espix's own tasks run in M-mode, where `satp`
does not apply.

**Every app is linked at the same fixed virtual address**, as a static
executable is on Linux:

| range | what |
|---|---|
| `0x00010000` up | the image: text, rodata, data, bss |
| above the image | the heap, grown by `brk` |
| below `0x70000000` | `mmap`, growing down |
| below `0x80000000` | thread stacks, each with an unmapped guard |

Every process can use the same addresses because each has its own table. The
loader maps the ELF's segments where they say they go; there is nothing to
relocate, which makes it simpler than today's.

An earlier draft identity-mapped instead (virtual equal to physical), to keep the
relocating loader. That rules out everything that saves memory by moving or
sharing pages: copy-on-write, swap, a page mapped only when touched, one copy of
a program's text for two instances. Apps are relinked anyway (below), so a fixed
link address costs nothing that identity mapping saved.

**Physical memory is 4 KiB frames with a reference count**, taken from PSRAM in
64 KiB chunks so the heap is not fragmented page by page. The count is what lets
a frame be in two tables at once: shared text, copy-on-write, a page in a swap
cache. The arena of [APP-MEMORY.md](APP-MEMORY.md) becomes the frames a process
holds, freed whole when it ends.

**Pages are mapped when touched.** bss, the heap and the stacks start as
reserved ranges with nothing behind them. The first touch faults, the kernel
maps a zeroed frame, and the app resumes. Memory an app reserves but never uses
costs nothing.

**Tables live in PSRAM.** The walker reads through the D-cache: the spike found a
PTE in the cache while PSRAM still held zero. No writeback after a PTE write, and
no internal RAM. A table costs a 4 KiB root plus one 4 KiB second level per
4 MiB range used: 16 KiB for a typical app (image and heap, mmap, stacks).

**ASIDs from a small allocator**, 9 bits wide. A switch writes `satp` (12
cycles) and keeps the TLB. A recycled ASID is flushed with `sfence.vma x0,asid`,
which the spike found to flush only that ASID. GrieferPig's Linux port saw the
opposite; the spike tested one entry on one core, so M3 re-tests it under load.

**`cpu_apm` region 0 is opened to REE at boot.** It covers flash and PSRAM for
TEE (M-mode) only, and U-mode is REE: a U-mode load from PSRAM is an access fault
until it is opened. Opening it hands isolation to the page tables, where it
belongs. DMA masters go through `hp_apm`, which this does not touch.

## Copy-on-write

A frame shared between two tables is mapped read-only in both, with one of the
PTE's software bits (RSW) marking it copy-on-write. A write faults; the kernel
copies the frame, maps the copy writable in the writer's table, and drops the
original's count. A frame whose count has fallen to one is made writable without
a copy.

What uses it, in the order it pays:

1. **Two instances of one program share its text**, which is read-only and never
   copied. Its data starts shared and is copied a page at a time as each
   instance writes.
2. **`fork()`** duplicates the table, marking every writable page copy-on-write.
   The parts it needs exist for (1). mosquitto is the first user: `-d` forks and
   lets the parent exit, which espix's port answers with `ENOSYS` today.

The cost is small: the reference counts (2 bytes a frame, 8 KiB for all of
PSRAM), one PTE bit, and a fault path that copies 4 KiB.

## Entering and leaving the kernel

Every way into M-mode from U-mode goes through one espix entry, and every way
back through one exit.

**IDF's vectors need a patch.** `vectors.S` saves its frame on whatever `sp` it
finds and restores `sp` by arithmetic. From U-mode that `sp` is the app's. A
`tools/patch-vectors.py`, in the style of the other IDF patches, makes the
entries switch stacks when they come from U-mode. `mscratch` holds the current
thread's kernel stack top while it runs in U, and 0 while it runs in M. The
first instruction swaps it with `sp`, a zero sends it back, and the user `sp`
goes into the frame's `RV_STK_SP`. The exit reloads `sp` from the frame when
returning to U.

**Interrupts reach U-mode regardless of `mintthresh`**, the spike found, straight
into IDF's vectored handler. So FreeRTOS preempts an app with no further work: the
tick saves the app's registers with `mstatus.MPP = U`, and a later `mret` resumes
it in U. The patched entry is what makes that safe.

**A context switch sets two things for the incoming thread**: `satp` (its
process's table and ASID, or bare for an espix task) and `mscratch` (its kernel
stack top, or 0). The hook is the switch-in path, `traceTASK_SWITCHED_IN` or the
patched exit. M1 decides which.

**Exceptions from U-mode** arrive at `mtvec`'s base, IDF's `_panic_handler`. The
patch routes them before the panic path:

| cause | goes to |
|---|---|
| `ecall` from U (8) | the syscall dispatcher |
| page fault (12, 13, 15) | map on first touch, copy-on-write, stack growth, or SIGSEGV |
| access fault (1, 5, 7) | SIGSEGV (or SIGBUS) |
| illegal instruction (2) | IDF's lazy FPU and PIE save first, as today, else SIGILL |
| breakpoint, misaligned | SIGTRAP, SIGBUS |

A fault in an app becomes a signal to the app, never a panic.

## The app side: an app links its own runtime

**An app cannot keep calling the firmware's libc.** Of the 406 names:

- only 82 are truly pure;
- 33 look pure and are not:
  - number parsing and the `printf` family reach a newlib state kept inside the
    FreeRTOS task struct, in internal RAM;
  - `strtok` and `rand` keep state there too;
  - the time functions hold static results;
- 59 live in mask ROM and reach `malloc`, `errno` and locks through a kernel
  function table (`syscall_table_ptr`);
- several hand the app pointers into kernel memory: `getenv`, `argv`, `readdir`'s
  dirent, `localtime`'s result, `stdout`'s FILE, getpw*, `gethostbyname`, and the
  display buffers plasma and Doom draw into.

Mapping the firmware's code into user space would make the pure 82 work and leave
everything else faulting on state the app cannot see.

**So a U-mode app links a static runtime of its own**, which is the Linux model:

- **picolibc**, statically linked. The toolchain ships it for the S31's exact ISA
  (`--specs=picolibc.specs`), and IDF supports it as the firmware's own libc too.
  Its `errno` is thread-local through `tp`, so there is no per-thread state
  struct to manage.
- **`libespix-u`**, espix's half:
  - `crt0`: the entry, argv and environ, which the kernel puts on the user stack;
  - the porting layer (`read`, `write`, `open`, `sbrk`...) as `ecall` stubs;
  - thread-local storage through `tp`;
  - stubs for espix's own calls: gfx, audio, the drivers that stay.
- **Headers stay the same**, so app *source* stays the same. The binary changes:
  an app is relinked, and its ELF says it is a U-mode app.
- **Built without PIE and the hardware loop** (`xespv`, `xesploop`). IDF's
  `-march` for the S31 includes both, and both exist only on core 1: a task that
  executes one on core 0 traps, and IDF pins it to core 1 for good
  (`vPortTaskUsedPIEOnCPU0`). The flag only *permits* them -- the firmware holds
  34 PIE instructions, all in FreeRTOS's own save routine, and today's apps hold
  none -- but building without it makes the guarantee. A U-mode app's threads
  then run on either core. An app that wants the SIMD opts in and is pinned.

**The kernel keeps newlib, for now.** `sdkconfig.defaults` pinned it for two
reasons. The first, IDF's console linenoise needing a TLS-stdio shim, no longer
applies: espix's `esp_linenoise` reads and writes through callbacks. Two that do:
- **Per-process stdio.** Each process's task gets its own `stdout` in newlib's
  per-task state. Under picolibc the three streams are global, with thread-local
  copies only through `CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY`.
- **M-mode apps' ABI.** They are compiled against newlib's headers and share its
  structures (`FILE`, `struct _reent`, the widened `off_t`), so the kernel's libc
  is their ABI.

Moving the kernel to picolibc is a separate ROADMAP item. A U-mode app shares
nothing with the kernel's libc but the syscall table, whose structures espix
defines, so it uses picolibc either way. `libespix-u` converts where the two
disagree; errno values are the first thing M1 checks.

**What it costs in size**, measured with the toolchain's libraries, statically
linked, `-Os`, with stub syscalls (text bytes):

| program | newlib | newlib-nano | picolibc |
|---|---|---|---|
| `puts` | 4,172 | 4,172 | 240 |
| `printf` with `%d %s` (hello) | 40,156 | 4,552 | 8,816 |
| `printf("%f")` and `strtod` | 46,772 | 32,594 | 12,556 |
| files, `malloc`, time formatting, floats | 84,566 | 46,564 | not measured |

Today's `hello` is 1,356 bytes because it borrows the firmware's libc. With
picolibc it is about 9 KiB; an app that already uses stdio and floats, like Doom
(333 KiB) or mosquitto (209 KiB), grows by roughly 10-20 KiB. With 13 MiB of PSRAM
free that is affordable. Two instances of one app share their text.

A later step could share libc itself: every process has the same layout, so one
libc linked at a fixed address (say `0x6f000000`) could be mapped read-only into
all of them, copy-on-write for its data. That is the old a.out shared-library
trick, with no position-independent code. Not in the milestones; noted because
the fixed layout makes it possible.

**M-mode apps stay.** The S3 has no other kind, and an app not yet ported keeps
working on the S31. The loader chooses by the ELF's mark.

What each app needs, from the inventory:

| app | imports | what changes |
|---|---|---|
| hello | 3 | nothing beyond the relink: M1's target |
| plasma | 10, all gfx | the surface mapped into the app instead of handed over by pointer |
| doom | 58 | gfx as plasma; audio writes (43 a second, 4 KiB each) become syscalls |
| mosquitto | 111 | sockets and poll as syscalls; its libc use moves in-app |
| neopixel | 46 | RMT callbacks run in an interrupt: no U-mode app can do that; stays M-mode |

## Syscalls

**A number in `a7`, arguments in `a0`-`a5`, the result in `a0`**, like Linux.
Negative errno on failure, turned into `errno` by the stub. The table is espix's
own, generated from one list, as the ABI tables are now. Linux's RV32 numbers
were considered: they would let a static musl binary run some day, but they bring
Linux's struct layouts with them (64-bit `time_t`, its `stat`). Deferred, not
refused.

**The dispatcher runs in the calling thread's task, on its kernel stack, with
interrupts on.** A syscall that blocks is an ordinary FreeRTOS block. FreeRTOS
sees an M-mode task doing kernel work, and nothing about its scheduling changes.

**User pointers are translated by a software walk, then used.** Two loads a
page find the frame and confirm V, U and the needed R or W; the kernel then reads
or writes the frame's physical address, a page at a time. A page not yet touched
is mapped on the way, as the app's own fault would. The kernel never uses
`MPRV`: on rev v0.0 an `MPRV` access to an invalid PTE stops the hart without a
trap (the spike reproduced it twice).

**The surface is smaller than the ABI.** libc's 115 pure and stateful names, and
getopt, rand, time formatting and strtok, run in the app. What remains is the
kernel's real work:

- files and directories, including `getdents` in place of `readdir`;
- sockets, select and poll;
- processes, signals and identity;
- time and sleep;
- memory (`brk`, `mmap`);
- a futex-like wait and wake for pthread mutexes, which live in user memory;
- gfx, audio and the drivers espix chooses to keep.

Results the kernel used to point into its own memory for (getpw*, name lookup)
are copied into a buffer the app passes.

## Threads and stacks

**A thread is a FreeRTOS task with two stacks.** Its task stack becomes its
*kernel* stack: small, fixed, used only in syscalls and interrupts. Its *user*
stack is in the app's address space.

**The kernel stack starts at 4 KiB, in PSRAM, and M2 measures it.** It holds:

- the trap frame, about 150 bytes, and the lazy FPU/PIE save area at its top,
  a few hundred more;
- the deepest syscall: a file call through the VFS into FATFS or lwext4 and
  down to USB mass storage is the likely winner, 2-3 KiB by estimate; socket
  calls hand their work to lwIP's thread and stay shallow;
- an interrupt's frame, before IDF moves to its own interrupt stack.

App pthreads today run app code *and* kernel calls on a 3 KiB internal-RAM stack,
so kernel paths already fit in under that. Linux gives an RV32 thread 8 KiB. The
`ps` STACK column is the high-water mark, so M2 sets the number from the test
suite rather than this estimate. Formatting moves out of the kernel with libc, so
`printf` no longer runs on it.

**Thread creation** makes the task, reserves a user stack, and starts the task in
a kernel trampoline that `mret`s into the start routine. A thread that returns
lands in a `libespix-u` stub that calls `exit_thread`. Keys and destructors are
the app runtime's business; the kernel never calls back into app code on its own
task.

**User stacks grow.** Each is a reserved range below `0x80000000` with an
unmapped guard below it. A fault inside the range maps a zeroed page and resumes;
a fault in the guard is SIGSEGV. Doom's fixed 256 KiB becomes what it touches.

## Signals, stopping and faults

**Whether a thread can be interrupted safely is in its saved `mstatus.MPP`.** A
thread interrupted in U-mode is provably holding no kernel lock: kernel state is
touched only in M-mode, inside a syscall. So when a signal arrives:

| the target is | what happens |
|---|---|
| running in U on this core | delivered at the interrupt's exit |
| running in U on the other core | a cross-core interrupt, then the same |
| inside a syscall | pending; a blocking call ends with `EINTR`, and the signal is delivered on the way back to U |
| stopped by SIGSTOP or Ctrl-Z | its task is suspended, which is safe for the same reason |

**Delivering a handler** pushes a signal frame (the saved registers) onto the
user stack, points the saved `mepc` at the handler with the signal number in
`a0`, and makes `ra` a `sigreturn` stub. The handler's return restores the
frame. That is how Linux does it, and it makes D4's cooperative delivery points
unnecessary for U-mode apps. D9's default action stays.

**A fault is a signal.** SIGSEGV's default action ends the process. The reaper
frees its address space, its frames and its fds, and the board keeps running:
the ROADMAP's "reap a faulted task" is finally sound for U-mode apps, because
their faults cannot have corrupted kernel memory. Deleting a thread is safe for
the same reason when it is in U-mode; one inside a syscall is told to end and
does so on the way out.

## The S3

**One kernel, two ways in.** The S3 build is not a fork of the code. Everything
a syscall does -- the VFS, fds, processes, signals, sockets, gfx, audio -- is the
same code on both chips. What the S31 adds is a layer in front of it:

| part | S3 | S31 |
|---|---|---|
| VFS, fds, processes, signals, sockets, gfx, audio | shared | shared |
| ABI tables: an M-mode app calls kernel functions directly | yes | yes, for apps not yet ported |
| syscall dispatcher: checks user pointers, then calls the same functions | no | yes |
| page tables, frames, copy-on-write, the fault paths | no | yes |
| the vectors patch, `satp`/`mscratch` switching | no | yes |
| `libespix-u`, picolibc | no | yes |

**An app has one source and two builds**: today's M-mode ELF, which the S3 runs,
and a U-mode ELF for the S31. They see the same names and the same behaviour.

**What the S3 does not get**, and the docs say so where it matters: isolation (a
wild write still corrupts the kernel), signal handlers inside a compute loop,
stopping an app at any instruction, surviving an app's fault, growing stacks,
copy-on-write and swap. Tests that depend on these skip on the S3, as
target-specific tests do now.

## Milestones

Each is a commit that leaves `make test` green on both targets.

| | what | done when |
|---|---|---|
| M1 | vectors patch, `satp`/`mscratch` switch, frame allocator, the fixed-address loader, a minimal syscall set (`write`, `exit_group`), `libespix-u` with `crt0` and picolibc, `cpu_apm` opened | `hello` runs in U-mode; a bad pointer in an app is SIGSEGV and the board keeps running; preemption and FPU use in U-mode work |
| M2 | files, time, env, `brk` with pages mapped on touch; tests/app relinked; the kernel stack size measured | the app suites pass with tests/app in U-mode |
| M3 | threads: create and exit, futex wait and wake, TLS; ASIDs under load | the pthread suites pass in U-mode |
| M4 | asynchronous signals, SIGSTOP and Ctrl-Z anywhere, fault reaping | a compute loop's handler runs; `kill -STOP` stops it at once; a segfault ends only the app |
| M5 | gfx surfaces mapped into the app; audio writes | plasma and Doom run in U-mode |
| M6 | sockets, select, poll | mosquitto runs in U-mode |
| M7 | growable stacks with a guard; `mmap`; shared text and copy-on-write data between instances | Doom runs without a fixed stack size; two instances of one app share text |
| M8 | `fork()` on copy-on-write, `exec()` in place | `mosquitto -d` daemonises: it forks, the parent exits, and the child carries on as the broker |

## Decisions taken

- M and U only; no S-mode.
- Apps linked at one fixed virtual address; every process has its own table.
- Refcounted 4 KiB frames; pages mapped on first touch; copy-on-write.
- Page tables in PSRAM.
- User pointers through a software walk; never `MPRV`.
- `cpu_apm` region 0 opened to REE; the MMU isolates.
- U-mode apps link picolibc and `libespix-u`; M-mode apps stay, chosen per ELF.
- Kernel stacks 4 KiB in PSRAM until M2 measures them.
- Syscall numbers are espix's own.

## Open questions

- **Where `satp` is switched:** a FreeRTOS trace hook, or the patched exit path.
  The hook is simpler; the exit path is the one place that knows it is returning
  to U.
- **How an ELF says it is a U-mode app:** an ELF note, a section, or a flag in
  `e_flags`.
- **The app build:** a CMake toolchain fragment apps include, or a second app
  template under `apps/`.
- **ASIDs under load:** the spike's one-entry test against GrieferPig's report.
- **Where frames come from:** chunks from the PSRAM heap as needed, or a region
  reserved at boot. Chunks share memory with everything else; a region is
  simpler to account for and to swap from.
- **Drivers from U-mode apps:** RMT and GPIO today are MMIO pointers and
  interrupt-time callbacks. They stay M-mode-app features until a driver is
  given a syscall face.
