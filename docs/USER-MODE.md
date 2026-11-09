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
| below `0x70000000` | `mmap`, and kernel memory lent to the app (a gfx surface), growing down |
| `0x70000000`-`0x7f800000` | 31 thread stacks of 8 MB, an unmapped page at each end |
| below `0x80000000` | the main thread's stack, 8 MB |

**Frames are 4 KiB of PSRAM, carved from 256 KiB chunks of the heap.** A 4 KiB
block aligned to 4 KiB leaves the heap's header in the page before it, so frames
allocated one by one cost two pages each and an app could map only half of what
was free. A chunk is smaller when the heap has no room for a whole one, and goes
back when its last frame does. Each frame counts the tables mapping it.

**Pages are mapped when touched.** Every range is a region: the image's segments,
bss, the heap, the stacks and each mapping. A page read before it is written is
one zero frame, shared read-only; the first write, by the app or by a syscall
copying into its buffer, gives it a frame of its own. Memory reserved and never
written costs nothing: a 4 MB `malloc` with 64 pages touched costs 256 KB. The
heap grows by `brk` up to `0x60000000`, and shrinking it frees its frames. Only
the pages that hold the file's bytes are loaded at exec. A process's resident
pages, the zero frame and lent memory aside, are its `HEAP` in `ps`, and
`/proc/meminfo`'s `UserMem` and `UserShared` count them over all apps.

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

## Copy-on-write and shared text

**A page its region lets the app write but its PTE does not is copy-on-write.**
A write faults; the kernel copies the frame for the writer, or makes it writable
when the writer's table is the only one mapping it. The zero frame is always
copied, and a shared mapping's page never is. A `fork()`'s child starts with its
parent's pages this way.

**Two instances of one program share its text.** A read-only segment that shares
no page with another is kept while a process maps it, found by its address, size
and CRC, and checked byte for byte; the next instance maps the same frames. A
process gives it up before its table goes, so the frames it names are always
mapped by someone, and a page of it written is the writer's copy. Data is each
instance's own.

## Mappings

**`mmap` takes anonymous and file memory, private or shared**, Linux's flags and
values (`user/include/sys/mman.h`), placed top-down below `0x70000000` with a
guard page between mappings:

- **A file's pages are read in as touched**, through a descriptor of the
  mapping's own, so the app may close its own. Past the file's end a page is zero.
- **A private page keeps its writes**; a shared writable one is written back by
  `msync`, `munmap` and the process's end. Two processes mapping one file each
  see their own pages until then.
- **`mprotect`** changes a range's permissions; `PROT_NONE` keeps the frame for
  when the range is opened again (a PTE software bit with V clear).
- **`MAP_FIXED`** replaces what is there, and `MAP_FIXED_NOREPLACE` refuses to,
  in the mapping range only. **`MADV_DONTNEED`** drops the pages, refilled when
  next touched. `MAP_POPULATE` maps them at once, and `mincore` says which are.
- **The brk heap and the thread stacks** are not the app's to unmap or protect.

**A `malloc` of 128 KiB or more is a mapping of its own**, as glibc's is: it costs
the pages touched, and `free` gives all of them back. `espix-cc` links
`libespix`'s allocator over picolibc's with `--wrap`, so libc's own allocations
take the same path. Doom's 6 MiB zone costs what it touches: `doom-u` holds about
3.5 MB while it plays.

## Processes: fork, vfork, exec, wait

**`fork()` copies the table, not the memory.** Each page the parent maps is
mapped in the child too, read-only where it was writable, so whichever writes
first gets its own copy. A shared mapping is filled first and then mapped
writable in both, so the two see each other's writes. The child gets:
- a copy of each descriptor: a file's shares its offset with the parent's, as on
  Linux, and a socket or pipe is the same one;
- the signal actions and the calling thread's mask;
- one thread, the caller's, returning 0 from the syscall with the caller's
  registers and FPU;
- streams made from the parent's session as the parent's were, so it writes
  where the parent does.

A surface, an audio stream, the screen and a directory stream stay the parent's.

**`vfork()` lends the parent's address space.** The child runs on the parent's
table until it calls `exec` or ends; until then the parent's calling thread
waits in the syscall, and its other threads go on. The table is counted: a
parent killed meanwhile ends, and its child keeps the memory and carries on
(`uapp vforkkill`). libespix's `vfork` is a stub with no stack frame of its own,
because the child returns on the parent's stack. `posix_spawn`, `system` and
`popen` are built on `vfork`, as musl's and uClibc's are.

**`exec` replaces the image in place**: the same pid, task and descriptor table.
Every other thread ends first, as at an exit. Then the main thread jumps back to
its task's start and:
- releases the old image: its texts, table, AMO list and surfaces;
- closes the close-on-exec descriptors;
- sets caught signals back to their default;
- loads the new program.

The mask, pending and ignored signals, the cwd and the credentials stay, and a
set-id binary's owner becomes the credentials, as at spawn. An M-mode ELF needs
a task of its own size, so an `exec` of one from U-mode fails with `ENOEXEC`.
Errors (`ENOENT`, `EACCES`, `ENOEXEC`, `E2BIG`) come back before anything has
changed.

**`waitpid()` sees the caller's children**, by pid, by group or any, with
`WNOHANG` and `WUNTRACED`. Statuses are Linux's encoding: a child a signal ends
is `WIFSIGNALED`, apart from one that calls `exit(128 + n)`. A child's end
raises SIGCHLD in its parent; under `SIG_IGN` or `SA_NOCLDWAIT` nobody waits for
it. The finished list keeps eight, and a status a living parent has not taken is
the last written over. A process's children pass to init (pid 1) when it ends,
their statuses unclaimed.

**`setsid()` leaves the shell's session.** The process's streams become its
terminal's own, or `/dev/null` without one, which outlive the session, and the
session's hangup no longer reaches it. That is how `mosquitto-u -d` serves on
after the `ssh` command that started it (`62-mqtt`). `setpgid`, `getpgid` and
`umask` work as on Linux. The umask is per process, inherited, and applied by
every create.

**A pipe is a kernel object for both kinds of app** (`pipe.c`):
- a 4 KB ring between two descriptors;
- a read waits for data or the last writer's close, and a write for room or the
  last reader's close (EPIPE, and SIGPIPE to the writer);
- a signal ends either wait, and a write of `PIPE_BUF` (512) or less is never
  split.

A U-mode app's pipe I/O goes through a kernel buffer, so no user page is held
while it waits.

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
- processes: `fork` (with `pthread_atfork` and libc's lock held across it),
  `vfork`, the `exec` family with a `PATH` search, `wait`, pipes, sessions and
  groups, `umask`, `posix_spawn`, `system`, `popen` and `daemon`; the last four
  are weak, for an app that brings its own;
- headers picolibc lacks (`sys/socket.h`, `netinet/in.h`, `netdb.h`,
  `semaphore.h`, `gettid()`, `popen()`), and in `user/include/common` the ones
  M-mode apps get from espix too (`syslog.h`, `ifaddrs.h`, `dlfcn.h`, `poll.h`).

`espix-cc` defines `ESPIX_UMODE_APP`, for source shared with an M-mode build
that must tell the two apart (mosquitto's seam defines `fork` only for M-mode).

`tools/espix-cc` compiles and links an app, as `musl-gcc` does for musl, with the
kernel's 64-bit `off_t`, no `cm.popret` (as IDF builds the kernel; with it
libespix's mutex deadlocked, [GOTCHAS](GOTCHAS.md)), and the barriers and
spinlocks picolibc's headers leave out (their feature macros follow what IDF's
own pthread implements). It links `libespix` whole and ahead of libc, because the
toolchain's `libc.a` carries weak `nosys` stubs that would otherwise win the
archive scan. `cmake/espix-uapp.cmake` is the CMake toolchain file over it: an
app with a `uapp/` CMake project (mosquitto's, plasma's, Doom's) is built that
way by `tools/build-apps.sh` and staged as `<name>-u`.

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
| plasma | runs in U-mode as `plasma-u`: the same source, its surface lent to it |
| doom | runs in U-mode as `doom-u`: the same engine, seam and entry; its sound through audio syscalls |
| mosquitto | runs in U-mode as `mosquitto-u`: the same sources and seam, through `uapp/` |
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

**The screen and audio are `espix_gfx.h` and `espix_audio.h` in both builds.** An
M-mode app calls the kernel's functions; a U-mode app's `libespix` makes them
syscalls on handles its process holds, eight at most, each a surface or a
stream. A call using a handle holds a reference, so a handle one thread frees
goes when another's call on it returns, and what a process ends holding is freed
or closed at its teardown, however it ends.

- **A surface's pixels are lent to the app.** The kernel allocates them
  page-aligned in PSRAM, zeroed, and maps them whole below `0x70000000`, with
  their PTEs marked lent (a software bit) so the table never frees them. The app
  draws where the PPA reads, and a present is one syscall. Freed, the surface is
  unmapped at once and released once no kernel call holds a pointer into it, as
  an unmapped frame is.
- **The screen's own lock** (`espix_gfx_lock`) is a surface the screen's size,
  copied to the canvas on present.
- **An audio write waits while the ring is full**, 50 ms at a time with a look
  for a signal between, so a kill ends it at once; after a second with no room it
  returns 0, as an M-mode write does. A stream belongs to the process, not to the
  task that opened it.
- **Doom's timedemo** (demo1, 5026 tics, both builds at `-Og`) runs at 39-41 fps
  in U-mode and 43.5 in M-mode.

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
| M5 | gfx surfaces mapped into the app; audio writes | done: `64-gfx` runs `plasma-u` and `doom-u` |
| M6 | sockets, select, poll | done: `32-umode`, and `62-mqtt` runs `mosquitto-u` |
| M7 | growable stacks; `mmap`; shared text between instances | done: `32-umode`; `doom-u` grows its stack and maps its zone |
| M8 | `fork()` on copy-on-write, `vfork()`, `exec()` in place, `wait`, pipes, sessions | done: `32-umode`, and `62-mqtt` daemonises `mosquitto-u -d` |

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
- **Futexes are private to a process**, so a mutex in memory shared across
  `fork()` cannot be waited on from both sides: a shared futex keyed by the
  frame would make process-shared mutexes, condition variables and semaphores
  work.
- **Signals are a set per process, not a queue**: two of one kind before
  delivery are one, and there are no real-time signals, `sigqueue`, `sigwait`
  or `sigaltstack` yet, nor `pthread_kill` to another thread.

## Decisions

- M and U only; no S-mode.
- One fixed link address for apps; a table per process; ASIDs from an allocator.
- Refcounted 4 KiB frames; pages mapped on touch; copy-on-write.
- Page tables in PSRAM; user pointers through a software walk, never `MPRV`.
- `cpu_apm` region 0 open to REE; the MMU isolates.
- U-mode apps link picolibc and `libespix`; M-mode apps stay, chosen by ELF type.
- Syscall numbers and structs are espix's own.

## Open questions

- **Drivers from U-mode apps:** RMT and GPIO are MMIO and interrupt callbacks,
  M-mode-app features until a driver gets a syscall face.
