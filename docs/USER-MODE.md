# User-mode apps on the S31

A design, written before the code (as [APP-MEMORY.md](APP-MEMORY.md) was for
R-P1). It says how an espix app would run in RISC-V user mode, in an address
space of its own, on the ESP32-S31, and in what order to build it. Nothing here
applies to the S3, which has no MMU and keeps today's model.

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
- stacks are a fixed size chosen up front.

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

**Identity-mapped: a virtual address equals its physical one.** The loader
already relocates an image for the address it loaded it at, so it needs no
change. A pointer the app passes the kernel is already a physical address once
it is checked. Isolation does not depend on the addresses, only on which pages
the table maps. The cost is `fork()` (copy-on-write needs two pages at one
virtual address), which D2 does not build. Stacks are the planned exception, so
that they can grow (see Threads and stacks).

**Tables live in PSRAM.** The walker reads through the D-cache: the spike found a
PTE in the cache while PSRAM still held zero. No writeback after a PTE write, and
no internal RAM. A process costs a 4 KiB root plus one 4 KiB table per 4 MiB
region it touches: 8 to 20 KiB.

**ASIDs from a small allocator**, 9 bits wide. A switch writes `satp` (12
cycles) and keeps the TLB. A recycled ASID is flushed with `sfence.vma x0,asid`,
which the spike found to flush only that ASID. GrieferPig's Linux port saw the
opposite; the spike tested one entry on one core, so M3 re-tests it under load.

**`cpu_apm` region 0 is opened to REE at boot.** It covers flash and PSRAM for
TEE (M-mode) only, and U-mode is REE: a U-mode load from PSRAM is an access fault
until it is opened. Opening it hands isolation to the page tables, where it
belongs. DMA masters go through `hp_apm`, which this does not touch.

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
| page fault (12, 13, 15) | stack growth, or SIGSEGV |
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

- **The toolchain's newlib**, statically linked. It already ships a `libc.a` for
  the S31's exact ISA (`rv32imafc_..._zcb_zcmp_zcmt/ilp32f`). It leaves
  `__getreent` and the `_write`/`_read`/`_open`/`_sbrk`... porting layer to the
  platform: that is newlib's standard port.
- **`libespix-u`**, espix's half:
  - `crt0`: the entry, argv and environ, which the kernel puts on the user stack;
  - the porting layer as `ecall` stubs;
  - a per-thread `struct _reent` behind `__getreent`, through `tp`;
  - `malloc` on newlib's, over pages the kernel maps on `sbrk`/`mmap`, which
    become the arena of [APP-MEMORY.md](APP-MEMORY.md);
  - stubs for espix's own calls: gfx, audio, the drivers that stay.
- **Headers stay the same**, so app *source* stays the same. The binary changes:
  an app is relinked, and its ELF says it is a U-mode app.

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
Linux's struct layouts with them (64-bit `time_t`, its `stat`), which newlib does
not share. Deferred, not refused.

**The dispatcher runs in the calling thread's task, on its kernel stack, with
interrupts on.** A syscall that blocks is an ordinary FreeRTOS block. FreeRTOS
sees an M-mode task doing kernel work, and nothing about its scheduling changes.

**User pointers are checked, then used directly.** The check walks the process's
table in software: two loads a page, confirming V, U and the needed R/W. The
kernel then reads or writes the physical address. It never uses `MPRV`: on rev
v0.0 an `MPRV` access to an invalid PTE stops the hart without a trap (the spike
reproduced it twice). The walk handles identity and non-identity regions alike.

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

**Thread creation** makes the task, maps a user stack, and starts the task in a
kernel trampoline that `mret`s into the start routine. A thread that returns
lands in a `libespix-u` stub that calls `exit_thread`. Keys and destructors are
the app runtime's business; the kernel never calls back into app code on its own
task.

**Stacks grow.** Each user stack is a reserved range at a non-identity address,
with an unmapped guard below it. A page fault inside the range maps a zeroed page
and resumes; a fault in the guard is SIGSEGV. Doom's fixed 256 KiB becomes what it
touches. M1 maps a fixed stack; growth is M7.

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
frees its address space, its pages and its fds, and the board keeps running:
the ROADMAP's "reap a faulted task" is finally sound for U-mode apps, because
their faults cannot have corrupted kernel memory.

## Milestones

Each is a commit that leaves `make test` green on both targets.

| | what | done when |
|---|---|---|
| M1 | vectors patch, `satp`/`mscratch` switch, a minimal syscall set (`write`, `exit_group`), `libespix-u` with `crt0` and newlib, the loader's U-mode path, `cpu_apm` opened | `hello` runs in U-mode; a bad pointer in an app is SIGSEGV and the board keeps running; preemption and FPU use in U-mode work |
| M2 | files, time, env, `brk`; tests/app relinked | the app suites pass with tests/app in U-mode |
| M3 | threads: create and exit, futex wait and wake, TLS; ASIDs under load | the pthread suites pass in U-mode |
| M4 | asynchronous signals, SIGSTOP and Ctrl-Z anywhere, fault reaping | a compute loop's handler runs; `kill -STOP` stops it at once; a segfault ends only the app |
| M5 | gfx surfaces mapped into the app; audio writes | plasma and Doom run in U-mode |
| M6 | sockets, select, poll | mosquitto runs in U-mode |
| M7 | growable stacks with a guard; `mmap` | Doom runs without a fixed stack size |

`fork()` and `exec()` in place stay out until something needs them (D2); the
design does not rule them out.

## Decisions taken

- M and U only; no S-mode.
- Identity-mapped address spaces; stacks are the exception.
- Page tables in PSRAM.
- User pointers through a software walk; never `MPRV`.
- `cpu_apm` region 0 opened to REE; the MMU isolates.
- U-mode apps link their own newlib and `libespix-u`; M-mode apps stay, chosen
  per ELF.
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
- **Kernel stack size** per U-mode thread. It holds the deepest syscall plus an
  interrupt frame; measure it in M2 with the watermark.
- **Drivers from U-mode apps:** RMT and GPIO today are MMIO pointers and
  interrupt-time callbacks. They stay M-mode-app features until a driver is
  given a syscall face.
