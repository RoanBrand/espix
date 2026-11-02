# User-mode apps on the S31

On the ESP32-S31 an app can run in RISC-V user mode, in an address space of its
own. The S3 keeps M-mode apps only; [the S3](#the-s3) says what the two builds
share. Spike measurements are in [ROADMAP.md](ROADMAP.md#processes).

## Why

An M-mode app shares the kernel's privilege and address space
([ARCHITECTURE.md](ARCHITECTURE.md#there-is-no-mmu-so-there-is-one-address-space)),
so:

- a wild write corrupts the kernel, and a fault reboots the board;
- a signal handler runs only when the app calls in, and SIGSTOP parks a process
  in itself, because at any other moment its task may hold a kernel lock;
- `fork()` is impossible;
- stacks are fixed, and memory an app reserves is memory it holds.

The S31 makes the boundary cheap: 37 cycles from `mret` into U-mode to the trap
back, 32 for a page fault, 11 for a TLB miss, and ASIDs that work.

## The shape: M for the kernel, U for apps

FreeRTOS, ESP-IDF, the drivers and espix run in M-mode. Apps run in U-mode.
S-mode is not used: it would need a translated kernel and a call boundary between
espix and IDF around every driver, to buy only what a software page walk gives.

## Address spaces

**One Sv32 table per process.** A thread runs with its process's `satp`; espix's
own tasks run in M-mode, where `satp` does not apply.

**Every app is linked at the same virtual addresses**, as a static executable is
on Linux:

| range | what |
|---|---|
| `0x00010000` up | the image: text, rodata, data, bss |
| above the image | the heap, grown by `brk` |
| below `0x70000000` | `mmap`, growing down |
| `0x70000000`-`0x7f800000` | 31 thread stacks of 8 MB, an unmapped page at each end |
| below `0x80000000` | the main thread's stack, 8 MB |

**Frames are 4 KiB of PSRAM with a reference count**, so one frame can be in two
tables: shared text, copy-on-write, a swap cache.

**Pages are mapped when touched.** bss, the heap and the stacks are reserved
ranges; the first touch maps a zeroed frame, whether the app or a syscall copying
into its buffer makes it. Memory reserved and never used costs nothing: a 4 MB
`malloc` with 64 pages touched costs 256 KB. The heap grows by `brk` up to
`0x60000000`, and shrinking it frees its frames. Only the pages that hold the
file's bytes are loaded at exec.

**Tables live in PSRAM.** The walker reads through the D-cache, so a PTE write
needs no writeback. A typical app's table is 16 KiB.

**ASIDs come from an allocator**, one per live process. A switch writes `satp`
and keeps the TLB; a freed ASID is flushed on each core (`sfence.vma x0, asid`)
before reuse.

**A page that becomes executable** is written back from the D-cache and
invalidated in the I-cache (`esp_cache_msync`). `fence.i` does not clear this
I-cache.

**`cpu_apm` region 0 is open to REE.** U-mode is REE, and the region covers flash
and PSRAM; the page tables do the isolating. DMA goes through `hp_apm`, which
stays as it is.

**Apps always load into RAM.** A littlefs file is neither contiguous nor
page-aligned, so it cannot execute in place; with the MMU, loading means mapping
what is touched. Running the *kernel* from flash is a roadmap item.

## Copy-on-write

A shared frame is read-only in both tables, marked copy-on-write in a PTE
software bit. A write faults; the kernel copies the frame for the writer, or makes
it writable when the count is one. Its users: two instances of one program
(shared text, data copied as written), then `fork()`. mosquitto `-d` is the first
`fork()` user.

## Entering and leaving the kernel

`tools/patch-umode.py` patches IDF's `vectors.S` and the IPC interrupt's
handler. Every way in from U goes through one entry and every way back through
one exit.

**No exception is handled where it was taken.** This core locks up when an
exception arrives while an earlier one is unretired, even another task's: the
second traps inside the first's handler, the core stops with interrupts dead,
and the watchdog resets the chip. So the vector sends an exception from U to
`espix_um_trap`, which `mret`s at once into `espix_um_kentry()`, with the
user's registers where the vector saved them and the stack below them. There
the syscall, page fault or fault runs as ordinary task code: it can block,
sleep, take the FPU through IDF's lazy trap, or end the process.
`espix_um_resume()` then `mret`s back to U. After a watchdog reset
the boot log shows each core's last two traps, from the debug-assist lockup
monitor.

- **The entry swaps `mscratch` with `t0`**, never with `sp`: IDF's hardware stack
  guard checks every write to `sp`. `mscratch` holds the thread's kernel stack top
  while it runs in U, and 0 in M.
- **On entry from U**, `satp` is set to bare and `gp`/`tp` are reloaded for the
  kernel; the IPC interrupt saves the app's `satp` and puts it back.
- **Interrupt frames are 160 bytes and carry the task's privilege.** Interrupts
  reach U-mode regardless of `mintthresh`, so FreeRTOS preempts an app with no
  extra work, and the exit returns to whichever privilege the frame says.
- **Every exit writes an `mcause`** whose MPP, MPIE and level agree with where it
  returns: the CLIC keeps them there, and `mret` uses them.
- **IDF's lazy FPU trap takes FLW and FSW** as a task's first FPU instruction on
  this S31 (`umode flw`), although its EXT_ILL workaround is built for the P4
  only.
- **The exit to U** loads the thread's `satp`, flushes a stale ASID, and hands the
  FPU to the thread eagerly (`fpu_take()`), so U-mode never takes IDF's lazy FPU
  trap.

**Exceptions from U-mode** are routed before IDF's panic path:

| cause | goes to |
|---|---|
| `ecall` from U (8) | the syscall dispatcher |
| page fault (12, 13, 15) | map on touch, copy-on-write, stack growth, or SIGSEGV |
| access fault (1, 5, 7) | SIGSEGV or SIGBUS |
| illegal instruction (2) | an AMO the loader replaced, done here; else SIGILL |
| breakpoint, misaligned | SIGTRAP, SIGBUS |

An app's exit, its faults and its fatal signals end the process from task
context, as an M-mode app's `exit()` does.

## The app side

**A U-mode app links its own libc**, as on Linux: picolibc, the same library the
kernel uses, statically linked, plus `libespix` (`user/libespix/`), espix's OS
layer under it:

- `crt0`: the entry, with argv and environ on the user stack;
- the calls picolibc's POSIX layer makes (`read`, `write`, `open`, `sbrk`...) as
  `ecall` wrappers;
- stdin, stdout and stderr on fds 0-2, and thread-local storage through `tp`;
- POSIX threads, on the kernel's threads and futexes;
- signals: `sigaction`, masks, `alarm` and `setitimer`, and the restorer a
  handler returns through;
- sockets, `poll` and `select` (over `poll`), name lookup, `getifaddrs`, the
  user and group database, `getrandom`, and `syslog` into the kernel log;
- headers picolibc lacks (`sys/socket.h`, `netinet/in.h`, `netdb.h`,
  `semaphore.h`, `gettid()`), and in `user/include/common` the ones M-mode apps
  get from espix too (`syslog.h`, `ifaddrs.h`, `dlfcn.h`, `poll.h`).

`tools/espix-cc` compiles and links an app, as `musl-gcc` does for musl, with the
kernel's 64-bit `off_t`, no `cm.popret` (as IDF builds the kernel; with it
libespix's mutex deadlocked, [GOTCHAS](GOTCHAS.md)), and the barriers and
spinlocks picolibc's headers leave out (their feature macros follow what IDF's
own pthread implements). It links `libespix` whole and ahead of libc, because the
toolchain's `libc.a` carries weak `nosys` stubs that would otherwise win the
archive scan.

App source is unchanged; it includes only standard headers. The kernel's libc
cannot be shared: its state (stdio, `errno`, `strtok`, time results) is kernel
memory. `hello` is 10.8 KiB of text.

**Built without PIE and the hardware loop** (`xespv`, `xesploop`). Only core 1
has them, and IDF pins a task that uses one to core 1 for good. Without them a
U-mode app and its threads run on either core.

**M-mode apps stay.** The loader chooses by ELF type: `ET_EXEC` is a U-mode app.

| app | what changes |
|---|---|
| hello | nothing beyond the relink |
| plasma | the surface mapped into the app |
| doom | gfx as plasma; audio writes become syscalls |
| mosquitto | sockets and poll as syscalls |
| neopixel | stays M-mode: its RMT callbacks run in an interrupt |

**Built-in programs.** espix's own commands and daemons are processes too, and
can become U-mode processes later: linked into the kernel image as a section of
their own, built against the U-mode runtime, mapped into each instance (text
shared, data copy-on-write), and reaching the kernel only by syscalls. The
mapping works wherever the kernel's text lives.

## Syscalls

**A number in `a7`, arguments in `a0`-`a5`, the result in `a0`**, negative errno
on failure, like Linux. The numbers and the structs are espix's own and
fixed-width; flag and errno values are picolibc's, which both sides use. Socket
options, flags and sockaddrs are lwIP's and poll's values IDF's, which M-mode
apps see too: the kernel passes them to lwIP unchanged, `user/include` mirrors
them, and `umode_net.c` asserts each at build time. The list is
`include/espix_syscall.h`, one X-macro that the kernel's dispatch table and
`libespix`'s numbers are generated from.

**Each process has its own descriptor table**, as on Linux: small numbers from 0,
the lowest free one for each open, 0-2 the process's streams, and behind each a
kernel descriptor, directory or socket. An app can name only its own, and `dup2`
onto 0-2 redirects them; a socket's descriptors share it, and its `O_NONBLOCK`.
The table is closed before the kernel's owned-descriptor sweep, while its
numbers are still the process's.

**A blocking call waits where a signal can end it.** lwIP's socket is always
non-blocking; a call the app made blocking waits in `esp_vfs_select()` beside the
process's wake eventfd, the same wait an M-mode app's `select` and `poll` use. A
signal writes the eventfd, and then a handler that is due ends the call with
EINTR (restarted under `SA_RESTART`, except `poll`, as on Linux), a stop parks
the thread inside the wait, and a signal whose action is nothing leaves it
waiting. `SO_RCVTIMEO` and `SO_SNDTIMEO` are the kernel's to keep. A regular
file in `poll` is always ready; stdin is reported readable until the transports
have a wake a select can wait on.

**The syscall table is part of espix's ABI.** An M-mode app calls kernel functions
through the symbol tables (`abi_*.c`); a U-mode app reaches the same functions by
syscall, with its pointers copied in front. libc itself is inside a U-mode app, so
the table holds only kernel work:

- files and directories;
- sockets, select and poll;
- processes, signals and identity;
- time and sleep;
- memory (`brk`, `mmap`);
- a futex-like wait and wake for pthread mutexes;
- gfx, audio and the drivers espix keeps.

A result the kernel would point into its own memory for (getpw*, name lookup) is
copied into a buffer the app passes.

**The dispatcher runs in the calling thread's task, on its kernel stack, with
interrupts on.** A syscall that blocks is an ordinary FreeRTOS block.

**User pointers go through a software page walk**, a page at a time, mapping an
untouched page on the way. The kernel never uses `MPRV`: an `MPRV` access to an
invalid PTE stops the hart without a trap.

## Threads and stacks

**A thread is a FreeRTOS task with two stacks**: its task stack is the *kernel*
stack, used only in syscalls and interrupts, and its *user* stack is in the
app's address space. Across the test suite the kernel stack's high-water is
3.8 KB, exec and file I/O included; a U-mode app's task gets 8 KB
(`CONFIG_ESPIX_UM_KSTACK`), chosen at spawn from the ELF's header, whatever its
name. Its own stack and heap are sized by what it touches.

**Thread creation** (`THREAD_CREATE`) makes the task as a thread of the
process, gives it a stack span, and enters U at the start routine with the TLS
block the app passes in `tp`, as Linux's `clone` with `CLONE_SETTLS` does. The
thread writes its tid into a word the app names before running, and the kernel
zeroes that word and wakes it when the thread ends (`CLONE_CHILD_CLEARTID`);
`pthread_join` waits on it. A thread's task is unpinned until it first runs, and
then stays on that core for the FPU, so threads spread over both cores;
`sched_getcpu()` (`GETCPU`) says which.

**`exit()` from any thread ends the process**, with that status. Every thread is
stopped as a kill stops it, and the main thread tears the process down once the
others have left. The main thread's `pthread_exit` leaves the process to its
threads, and it ends with 0 when the last one does, as on Linux.

**User stacks grow** into an 8 MB reservation (Linux's `ulimit -s`), with an
unmapped guard page below; a fault in the guard is SIGSEGV. No app needs a stack
size chosen for it.

**Futexes** (`FUTEX`, wait and wake) are private to the process: its own list of
waiters, under its lock. A waker notifies the waiting task while holding that
lock, so the waiter cannot leave, and its task cannot end, before the notify
lands. `libespix` builds mutexes, condition variables, rwlocks, barriers,
spinlocks, semaphores, once and keys on them, and the locks picolibc's `malloc`
and stdio take. Its read-modify-writes are LR/SC, never AMOs.

**The kernel does an app's AMOs.** A U-mode AMO on this S31 can lose an update
against an AMO on the same word from the other core ([GOTCHAS](GOTCHAS.md));
LR/SC and M-mode never do. So the loader replaces each AMO in the image's code
sections with `unimp` and keeps a table of their addresses and instructions;
the illegal-instruction exception looks `mepc` up there, does the operation on
the user's word as LR/SC, puts the old value in `rd` and resumes. One costs
about 1,150 cycles, a null syscall's worth. `uapp amo` checks it from both
cores; with `CONFIG_ESPIX_UM_AMO_TRAP` off it measures the hardware instead.

**The tables are shared, so changes to them are locked and flushed everywhere.**
A page fault on a page another thread just mapped is resumed, not a SIGSEGV.
An unmap (a shrinking heap, an ended thread's stack) marks the ASID stale on the
other core, and if that core runs it now, flushes it there by a cross-core
interrupt before the frames are freed. A frame a syscall is reading or writing
into is pinned, and an unmap meanwhile frees it when the syscall lets go.
Pinning a page that is mapped already takes no lock: the pinner counts itself
before it reads the PTE, and an unmap clears the PTE before it reads the count,
so one of them sees the other.

## Signals, stopping and faults

A thread in U-mode holds no kernel lock, so a signal may reach it at any
instruction. Every return to U checks for one: a signal pending that the thread
does not block, a stop or a kill. If there is one, the return goes to a pc that
is never mapped instead (`espix_um_return_to()`). The fault that follows hands
the work to `espix_um_sig_deliver()` as the thread's own task code, where it may
block and touch user memory; nothing of it runs inside an interrupt.

| the target is | it takes the signal |
|---|---|
| running in U on this core | at the next interrupt's exit |
| running in U on the other core | at once, by a cross-core yield: 42 µs from `kill` to the handler (`uapp sigasync`) |
| inside a syscall | on the way back to U; a blocking call a caught signal ends returns `EINTR`, or restarts under `SA_RESTART` |

**A handler** gets a frame pushed onto the stack it was running on
(`struct espix_sigframe`): the `siginfo_t`, the mask to go back to, pc and
x1-x31, and f0-f31 with `fcsr`. It is entered with the signal in `a0`, the
`siginfo_t` in `a1`, the frame in `a2` and `ra` at libespix's restorer, whose
`SIGRETURN` puts everything back. `SA_SIGINFO`, `SA_RESTART`, `SA_RESETHAND` and
`SA_NODEFER` work; reads and writes restart, sleeps and futex waits return
`EINTR`, as on Linux.

**Actions are the process's, masks each thread's.** A process's signal goes to
one thread that does not block it: the first back to U, or the first blocking
call to claim it, so only that call ends early. `raise()` and a fault go to the
calling thread.

**A fault is a signal.** It goes to the thread's handler with `si_addr` set; if
the signal is blocked or not caught, the process ends with 128 + the signal, and
the board keeps running.

**SIGSTOP** parks each thread as it next returns to U or wakes in a blocking
call, holding nothing; SIGCONT lets them go, and SIGKILL wakes a stopped process
to end it. SIGTSTP, SIGTTIN and SIGTTOU stop it the same way unless caught, so
Ctrl-Z stops a computing app at once and the shell makes it a job for `fg` and
`bg`. **A fatal signal** ends the process from the thread that takes it,
as `exit()` does; SIGKILL gives 137, as on Linux.

## Beside an S31 Linux port

[annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux)
runs Linux in S-mode on core 1, OpenSBI in M-mode, and IDF on core 0. Where the
two meet the same silicon:

| | that port | espix |
|---|---|---|
| leaving a trap | `handle_exception` `sret`s to itself before its handlers: only `sret` lowers the CLIC level | the trap `mret`s into `espix_um_kentry()` before any work: an exception taken inside an unretired one locks the core |
| interrupt level on return | `sret` restores it only from an interrupt-type `scause` | every exit writes an `mcause` with the level |
| a null syscall | `getpid` 1.1-1.2 µs, trap code (4.6 KiB) run from SRAM | `getppid` 2.9 µs, trap path in SRAM |
| FPU | off, soft-float: Linux needs D, which the chip lacks | F for U-mode apps (ilp32f), handed over eagerly; IDF's lazy trap takes FLW/FSW and their compressed forms |
| new code | `fence.i` leaves stale lines; OpenSBI writes back D and invalidates I | `esp_cache_msync`, D written back then I invalidated, per executable page |
| isolation | Sv32 for processes, PMP keeps IDF's core out of Linux memory | Sv32 per process, `cpu_apm` open to U, no PMP (IDF locks it) |

## The S3

One kernel, two ways in:

| part | S3 | S31 |
|---|---|---|
| VFS, fds, processes, signals, sockets, gfx, audio | shared | shared |
| symbol-table ABI for M-mode apps | yes | yes |
| syscall dispatcher, page tables, fault paths, the vectors patch | no | yes |
| `libespix`, `espix-cc` | no | yes |

An app has one source and two builds: an M-mode ELF for the S3 and a U-mode ELF
for the S31. The S3 does not get isolation, asynchronous signals, surviving an
app's fault, growing stacks or copy-on-write; tests that need them skip there.

## Milestones

Each leaves `make test` green on both targets.

| | what | done when |
|---|---|---|
| M1 | vectors patch, address spaces, the loader, `write` and `exit`, the app runtime | done: `tests/suites/32-umode.sh` |
| M2 | files, time, identity; `brk` and pages mapped on touch; testapp relinked; the kernel stack measured | done: `32-umode`, and `ESPIX_UAPP=1 make test` skips only what M4 and M7 bring |
| M3 | threads, futex wait and wake, TLS; shared tables flushed on both cores | done: `32-umode`, and the thread checks of `30-proc` pass in U-mode |
| M4 | asynchronous signals, SIGSTOP and Ctrl-Z anywhere, faults to handlers | done: `32-umode`, and `35-signals` in U-mode |
| M5 | gfx surfaces mapped into the app; audio writes | plasma and Doom run in U-mode |
| M6 | sockets, select, poll | mosquitto runs in U-mode |
| M7 | growable stacks; `mmap`; shared text between instances | Doom runs without a fixed stack; two instances share text |
| M8 | `fork()` on copy-on-write, `exec()` in place | `mosquitto -d` daemonises |

Open:
- **A null syscall costs 920 cycles (2.9 µs)** round trip (`uapp bench`), about
  330 instructions, nearly all cache hits. A load or store takes 3 cycles and a
  CSR write 5 to 6 (`umode cost`), so the 31 registers saved and restored are
  190 of them. The cache controller counts I-cache hits, misses and fills per
  core (`0x2C000180` on), which tells whether moving code would help before it
  is moved: the I-cache is 2-way, and three hot lines in one set miss every
  time.
- **Why a U-mode AMO is not atomic across cores, and why `cm.popret` broke the
  mutex**, are not known; both look like rev v0.0 silicon. The AMO needs U-mode
  execution: data translated at U privilege (`MPRV`) is clean, which leaves
  translated instruction fetch or the core's mode ([GOTCHAS](GOTCHAS.md)). A
  lead: S-mode code with `TEE_SUPERVISOR_PRIV_SEL` set and clear, to tell the
  bus's REE path from the core. Espressif's own U-mode world, ESP-TEE, runs on
  single-core chips only. For Espressif: both, with `uapp amo` as the
  reproducer, and whether the S31 shares the popret issue IDF flags on the C5,
  C61, H4 and P4 (`SOC_CPU_ZCMP_POPRET_ISSUE`).
- **An app's atomics cost a trap each** while the kernel does them. A toolchain
  with GCC's Zalrsc-only exchange fix ([UPSTREAM](UPSTREAM.md)) lets `espix-cc`
  build without Zaamo, and picolibc and libstdc++ rebuilt the same way, so that
  apps carry no AMOs and pay nothing.
- **Deleting a task preempted in U-mode leaks about 1 KB of internal RAM.** A kill
  only comes to that when an app stays over 250 ms inside a syscall that cannot
  be interrupted; otherwise the app leaves on its own. A thread deleted that way
  also leaves its `espix_um_thread_t`; a TLS deletion callback on
  `ESPIX_TLS_UM_IDX` (IDF calls it as the task is freed) would free both.
- **Signals are a set per process, not a queue**: two of one kind before
  delivery are one, and there are no real-time signals, `sigqueue`, `sigwait`
  or `sigaltstack` yet, nor `pthread_kill` to another thread.
- **picolibc's `malloc` zero-fills every block**, so a large allocation is
  touched in full; a large `malloc` served by `mmap`, as glibc's is, comes with
  M7.

## Decisions

- M and U only; no S-mode.
- One fixed link address for apps; a table per process; ASIDs from an allocator.
- Refcounted 4 KiB frames; pages mapped on touch; copy-on-write.
- Page tables in PSRAM; user pointers through a software walk, never `MPRV`.
- `cpu_apm` region 0 open to REE; the MMU isolates.
- U-mode apps link picolibc and `libespix`; M-mode apps stay, chosen by ELF type.
- Syscall numbers and structs are espix's own.

## Open questions

- **The app build for `apps/*`:** a CMake toolchain file sharing the flags of the
  U-mode compiler wrapper.
- **Where frames come from:** the PSRAM heap as needed, or a region reserved at
  boot, which is simpler to account for and to swap from.
- **Drivers from U-mode apps:** RMT and GPIO are MMIO and interrupt callbacks,
  M-mode-app features until a driver gets a syscall face.
