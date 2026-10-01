# espix worklist

A working checklist, not a design document. It exists so work can be resumed
without re-deriving anything: every item has an id, a reason, and a "done when".

- Why things are the way they are: [ARCHITECTURE.md](ARCHITECTURE.md)
- What might be worth doing and what it costs: [ROADMAP.md](ROADMAP.md)
- What will surprise you today: [KNOWN-ISSUES.md](KNOWN-ISSUES.md)

Status values: `todo` · `doing` · `done` · `parked` · `dropped`

---

## How to resume

1. Read "Locked decisions" — do not relitigate these without measuring.
2. Take the first `todo` in the lowest-numbered open phase.
3. When an item is finished, set it `done` with the commit hash. Do not delete
   it; the entry is the trail.
4. Anything that turns out to be a *permanent* reason rather than a task moves
   to ARCHITECTURE.md, and the item here points at it.

This file came out of a full review of the tree (first commit through
2026-10-01): memory placement, the Unix surface, the app ABI, tasks and polling.

---

## Locked decisions

- **D1 — The app ABI stays an allowlist, and it becomes espix's alone.**
  `elf_loader` can generate a table of every firmware global symbol; espix
  deliberately types each name instead, because on a chip with no MMU the export
  table *is* the boundary. The follow-on decision: the 84 names the loader's own
  libc/IDF tables currently answer for apps should eventually be intercepted by
  espix too, so that one layer defines the whole surface. See R-P1.
- **D2 — `fork()` is not built.** The S31 has a real Sv32 MMU (datasheet p.44,
  M/S/U modes; Linux boots on it via OpenSBI). Using it means apps in U-mode, an
  `ecall` boundary, page-fault handling and a frame allocator — a second kernel
  around FreeRTOS, for one of three targets. `fork`-without-`exec` is the only
  feature that needs it and nothing espix ships uses it. The staged path (R-P3)
  is recorded instead.
- **D3 — `dup2`, the spawn family and `waitpid` are built now**, because their
  implementation is identical on S3/S31/P4 and they are what pipes, services and
  a shell actually need.
- **D4 — Signals stay cooperative** (handlers run at delivery points), but the
  missing *generators* are added: SIGALRM, SIGCHLD, SIGWINCH, SIGPIPE, plus an
  eventfd so a process blocked in `socket_select` can be woken. No asynchronous
  delivery — that is the same cost as D2.
- **D5 — FreeRTOS's idle tasks are left alone.** Termination cleanup stays where
  FreeRTOS puts it. `espix:reaper` becomes the *process-resource* reclaimer
  (arena, fds, screen), which is a different job. The TWDT stops watching idle
  so a busy app is not a reset. See R-P4.
- **D6 — Per-process ownership is the keystone**, and it lands before anything
  that depends on it (fault reaping, the reclaimable-cache reaper, services).
- **D7 — PSRAM is the default; internal is the exception**, and every exception
  is written down with its reason (realtime, DMA, cache-frozen windows, IRAM).
- **D8 — "Work like a Linux app on a server" is the north star for syscalls**,
  meaning *observable semantics*, not isolation: on S3 an app still shares the
  address space and can corrupt the kernel.
- **D9 — A signal's *default* action terminates for real.** A handler needs a
  delivery point and therefore the app's cooperation; the default action needs
  neither, and is implemented as an actual task deletion rather than a flag the
  app must poll. This is what makes `kill` work on a busy loop that installed no
  handler. A handler *inside* a pure compute loop remains the one case that
  cannot be served without the trap boundary (R-P3.2).

---

## R-P0 — correctness and de-risking (do first, all small)

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| R-P0.1 | Intercept `exit`/`_Exit`/`_exit`/`abort`/`__assert_func` through the resolver; they longjmp to `proc_task`, which runs the normal teardown | An app calling `exit(0)` — or tripping `assert()` — **rebooted the board**. `assert()` could not be fixed by overriding the app's symbol: the failing assert calls the *firmware's* `__assert_func` | `abi_exit.c` (new), `exec.c`, priv header | `hello exit 5` → `[exit 5]`; `hello abort` → `[exit 134]`; `hello assert` prints and → `[exit 134]`; board up | **done** (694b9e5) |
| R-P0.2 | Remove the dead `putenv` (`abi_libc.c:179`, shadowed by the resolver) and the duplicate `fflush` (`abi_libc.c:115`, shadowed by `abi_cxx.cpp:158`) | Dead entries read as promises | `abi_libc.c` | build clean, no duplicates | **done** |
| R-P0.3 | `check-abi.py`: string-aware comment stripping; cover the resolver and the written-out `{name,&sym}` tables; read the registration order from `proc.c`; fail the build on a shadowed duplicate | It parsed comments as code and could see only one of the three mechanisms espix publishes through — it missed the two duplicates R-P0.2 removed by hand | `tools/check-abi.py` | a deliberately shadowed name fails the build | **done** |
| ~~R-P0.4~~ | **Done, re-scoped.** An app now includes one interface-named file, `cmake/espix-app.cmake` (which derives `ESPIX_ROOT` itself); `cmake/offt64.h` asserts `sizeof(off_t) == 8` where the mechanism lives; `check-abi.py` fails the firmware build if any project under `apps/` or `tests/` omits it | `off_t` is kept in agreement by a force-include both sides read, and an app that forgets to opt in compiles cleanly and then misreads `struct stat` — nothing catches that today. The original idea (a generated TU taking every published symbol's address through its header) is **not tractable**: mangled C++ names, register blocks and macro-renamed `lwip_*` names have no header to declare them against, and within one side the compiler already checks | `cmake/espix-app.cmake`, `cmake/offt64.h`, `tools/check-abi.py`, the five app CMakeLists, `apps/README.md` | an app missing the include fails the build; a toolchain change that unwidens `off_t` breaks both sides at compile time | **done** |
| R-P0.5 | SSH connection task stack to PSRAM (`xTaskCreateWithCaps`, with an internal fallback and a deletion that respects which pool it came from) | 8192 B x up to 8 = **64 KB** of internal RAM, the largest single consumer | `ssh_server.c` | 7 sessions: free internal **63 K → 114 K**; throughput unmoved | **done** |
| R-P0.6 | `ssh_chan_t` to PSRAM | ~2.2 KB x 8 = 17.6 KB, from a plain `calloc` | `ssh_channel.c` | at 8 connections: free internal **111 K → 129 K**, low-water 73 K → 83 K | **done** |
| R-P0.7 | Establish whether `ssh_conn_t`'s `SPIRAM\|DMA` request actually succeeds | If it fell back, ~13.4 KB x 8 = **107 KB** was silently internal | `ssh_server.c` | **answered**: it succeeds — `connection state in PSRAM (DMA-capable, aligned)`, reported once at the first connection | **done** |
| R-P0.8 | `CONFIG_ESPIX_KLOG_LINES` 96 -> 256 | Ring is already PSRAM, so this costs ~32 KB of PSRAM and nothing internal | Kconfig | dmesg shows 256 lines | todo |
| ~~R-P0.9~~ | **Done for the periodic lines.** `rfb.c` reports only a second with a stall or a real wait (threshold `QUEUE_REPORT_US`); `display.c`'s volume counter is a DEBUG, because no value of it means anything is wrong. **What the audit found:** the 55 `ESPIX_KLOG_DEBUG` sites are mostly boot-time "X published to apps" confirmations and per-operation traces (18 ssh, 9 proc, 8 usb, 4 shell, 4 bt) — already invisible at the console's INFO level, so the axis that matters for them is *frequency*, not count. The 163 INFO sites are per-event and fine. | A healthy device should be quiet; the two lines were the only periodic telemetry in the tree | `rfb.c`, `display.c` | done | **done** |
| ~~R-P0.10~~ | **Trap repaired rather than soaked.** The watchpoint was aimed at `s_table_count`, which is the word after the table on Xtensa (a linker accident) but lands in `.sbss` on RISC-V — so on the S31 it was aimed at nothing. The word past the table is now the table's own `guard` member, watched on both targets, with a software canary for the watchpoint-off build. **No soak:** the occurrence was on the S3, where the trap did work, the layout has moved since, and much has been fixed | A trap that is right on one target and wrong on another *by accident* was the real defect | `espix_proc_priv.h`, `proc.c`, `abi_resolver.c`, `KNOWN-ISSUES.md` | done | **done** |
| R-P0.13 | `LISTEN_BACKLOG` 1 → `CONFIG_ESPIX_SSH_MAX_SESSIONS` | It was left at 1 from the one-session milestone, and lwIP drops a SYN arriving with that queue full — no reset, no callback, nothing espix can log. A cliff, not a queue | `ssh_server.c` | 48 bursts of 8 clean; **not** claimed as the fix for the 1-in-72 failure, whose rate is too low for that sample | **done** |
| R-P0.12 | Move the exit-path tests out of `apps/hello` into `tests/app`, and slim `hello` back to a showcase | `hello` is the example of an app, not the ABI's test; it had grown filesystem coverage (which `tests/app` already has as `stat`/`write`/`read`/`chmod`/`probe`) and the exit cases | `apps/hello/main/hello.c`, `tests/app/main/testapp.c`, `tests/suites/30-proc.sh` | `hello` is 37 lines; `testapp exitcall/_Exit/abort/assert` exist; 30-proc asserts all four | **done** |
| ~~R-P0.11~~ | **The default action terminates now** — `espix_proc_exit(128 + sig)`, the same door an app's own `exit()` uses, instead of setting a flag the process may never read. An app wanting to put hardware back installs a handler and never arrives here; for a sketch that is the shim's job, not espix's | A default action cannot be cooperative: it is defined by needing nothing from the process. **Caveat: the suite cannot tell the two implementations apart** — 35-signals checks the process is gone and the grace did not expire, which both satisfy, so this rests on the mechanism (a path R-P0.1 proved) rather than on a test. Missing coverage: a mode that blocks without calling in, plus an assertion that its cleanup did *not* run | `proc.c` `sig_dispatch` | done, with that gap recorded | **done** |

## R-P1 — per-process ownership (the keystone)

**Design written before the code: [APP-MEMORY.md](APP-MEMORY.md).** It records the
measured problem, the six things that must be true, the choice between tagging
allocations and a private arena, the hazards in the order they bite, and the
decisions, and an **implementation plan in the order it has to happen**.

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| ~~R-P1.1~~ | Intercept `malloc`/`calloc`/`realloc`/`free`/`strdup` **and the C++ `operator new`/`delete`** through the resolver, PSRAM-first with an internal fallback | Apps got the firmware's allocator, and a C++ app's `new` did not even reach that — `abi_cxx.cpp` mapped it to the firmware's, whose internals call the firmware's malloc | `abi_alloc.c` (new), `abi_cxx.cpp`, the resolver | done | **done** (`eede071`, `cb98af2`) — runtime path of the C++ half untested, see R-P1.9 |
| R-P1.2 | Per-app **list of growable regions**, freed whole on exit and on kill | This is the Doom leak: 13.2 MB -> 571 KB over two runs. Nothing can reclaim what was never recorded | `abi_alloc.c`, `espix_proc_priv.h`, `proc.c`, `cmd_sys.c` | two Doom runs return the PSRAM | **done** (`6969357`) — regions made lazily and sized from the request that failed, the 4 MB PSRAM floor, and whole-arena release from `release_resources()`, so a clean exit and a kill return the same PSRAM. Proved on the S31 by `30-proc.sh`: `ps` shows the live arena, `kill -9` returns it to the byte, a request too large for the first region makes a second and both come back, and a free from a thread with no slot finds the right region. And the real case: with the display up, Doom held about 7 MB of PSRAM (11.7 MB -> 4.7 MB free) and `kill -9` returned it to 12.3 MB, where before R-P1.2 only a reboot did. Hardened after review: eight regions, and the region list **grows on demand** rather than spilling to the global heap, which was tried and rejected as a leak -- five 1 MB blocks used to return NULL with 12 MB free, because each took a region of its own |
| R-P1.3 | Give `fd_slot_t` an owner and close a dead process's fds | `fd_slot_t` is `{lower_fd, mount}`, so a force-killed app leaks its descriptors | `vfs.c:182` | a killed app's `cat` releases its fd | todo |
| R-P1.4 | `ppid`/children tracking + `SIGCHLD` | Nothing tracks parentage; ownership is "which session pointer you hold". Needed by services, job control and `waitpid` | `espix_proc.h:59` | a child exit notifies its parent | todo |
| R-P1.5 | Separate the **live table** from a small **completed log**; recycle a slot on reap | Retaining finished slots is a debugging affordance, not POSIX — Linux keeps only unreaped zombies, and `ps` history is not a thing there. The live table then sizes to *concurrency*, not to history | `proc.c:133`, `cmd_sys.c` `ps` | `ps` shows history from the ring; slots recycle at once | todo |
| R-P1.6 | Make `espix:reaper` the **single teardown point**: per-invocation tasks (command, ssh conn, app) stop calling `vTaskDelete(NULL)` and park instead, and the reaper deletes them — reclaiming the arena, fds and screen in the same pass | `prvCheckTasksWaitingTermination()` is a private static in FreeRTOS's `tasks.c` and cannot be called. But deleting *another* task frees its TCB in the caller, so if nothing self-deletes, the idle task stops being *required* for espix's cleanup. IDF components and app-created pthreads still self-delete, so this reduces the dependence rather than removing it. Also retires the `espix_gfx_recover()` special case | `reaper.c`, `exec.c:616`, `session.c:469`, `ssh_server.c:495` | a finished process is torn down entirely by the reaper | todo |
| R-P1.7 | Reclaim the screen through R-P1.6 instead of the `espix_gfx_recover()` special case | Today the canvas is the one resource that *is* reclaimed, by hand | `proc.c:236` | the special case is gone | todo |
| R-P1.8 | `dup`/`dup2`/`fcntl(F_DUPFD)` in the VFS | Currently "left out" only because IDF stubs it. Prerequisite for pipes and redirection, and portable to all targets | `vfs.c` | `dup2` from an app works | todo |

| R-P1.9 | The C++ allocator path is unverified at runtime | `arduino-esp32` has **no S31 support at all** — zero files mention `ESP32S31`, against 35 for the P4 — so neopixel cannot be built for this target (its `apps/neopixel/targets` says `esp32s3` and `build-apps.sh` skips it correctly) and there is no C++ app to exercise `operator new`/`delete`. Compile, link and the resolver mapping are checked; the runtime path is not. Nothing to do here until an S31-capable C++ app exists — porting Arduino to the S31 is an upstream project, not an espix one | A whole half of the allocator ABI has no runtime coverage | — | revisit when a C++ app can run on the S31 | parked |
| R-P1.10 | Give an app's **own threads** a slot, so their `malloc()` lands in the process's arena too | R-P1.2 gave a thread's `free()` the region, by address, because that was the corruption case. Its `malloc()` still has no task-to-process lookup, so a thread's allocations go to the global heap and are not reclaimed at exit -- the leak R-P1.2 exists to remove, just smaller. The fix is to publish `pthread_create` through the resolver and record the task it makes against the calling slot | `abi_alloc.c`, a pthread seam, `espix_proc.h` | memory an app thread allocates is returned at exit | todo (recommended next in R-P1) |

## R-P2 — the shell surface

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| R-P2.1 | `;` and `&&`/`\|\|` as parsed operators | Nearly free: split before dispatch, run in sequence, read `s->last_status`. No new objects | `session.c:533` | `ssh host 'cd /bin && ls'` works | todo |
| R-P2.2 | Make `&` a parser-level token | It is understood only by `exec_fallback` and `confine`; a builtin sees it as a filename, so `cat f &` reads a file named `&` | `cmd_run.c:301,433` | builtins background correctly | todo |
| R-P2.3 | Text utilities: `wc`, `head`, `tail`, `grep` (-F first), `sort`, `find`, `sleep`, `true`/`false`/`test` | `cat`, `ls`, `echo`, `df`, `sha256sum` are the entire text surface today | `espix_cmds/` | present and tested | todo |
| R-P2.4 | `<` redirection + stdin for builtins | Cheap alone, only pays with pipes | `session.c` | a builtin reads stdin | todo |
| R-P2.5 | Pipes (`\|`) as **one StreamBuffer** per pipe | A pipe is unidirectional and single-reader/single-writer, so one ring is exactly right; refcount the shell's redirect `FILE` | `session.c`, new pipe object | `ls \| wc -l` works | todo |
| R-P2.6 | Job control: a job table, `jobs`/`fg`/`bg`, Ctrl-Z -> SIGTSTP | SIGSTOP/SIGCONT and the `T` state already work; `session->fg_pid` is written and never read | `session.c`, `cmd_run.c` | a stopped job can be resumed | todo |
| R-P2.7 | Globbing | Planned in POSIX.md | `session.c` expansion | `ls *.c` works | parked |

## R-P3 — the boundary (staged; see D2)

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P3.1 | Make espix the single definition of the app ABI: intercept the ~84 names the loader's own tables answer for apps | Makes the boundary auditable and is a pure refactor with no new hardware | no app resolves a name outside espix's tables | todo |
| R-P3.2 | Design (not build) the `ecall` form: a tiny app-side libespix, a trap handler, and PMP-protected kernel | This is what makes the boundary *enforced* rather than *named*, and it works on S3 and P4 too — it needs no MMU | a written design in ARCHITECTURE.md | parked |
| R-P3.3 | Sv32 page tables + CoW on S31 (`fork`) | Only `fork`-without-`exec` needs it | only if something needs it | parked |

## R-P4 — scheduling and idle

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P4.1 | Stop the TWDT watching idle tasks (or lengthen the timeout) | **Confirmed on hardware, and costly even without PANIC off:** three `testapp sig spin` processes (prio 3) pinned CPU 1 and starved IDLE1, so the watchdog fired every 5 s for minutes — 46 times. Each warning prints a **full register dump**, which is ~30 lines: the 96-line klog ring rolled 309 lines in one reading, so everything else is lost while it happens. `PANIC` is off, so it warns and the board stays up (it kept serving SSH, with occasional "version exchange failed"). A quiet warning would be cheap; this is not | `sdkconfig` + `docs/KNOWN-ISSUES.md` | an app at priority > idle can spin without a register dump per 5 s | todo |
| R-P4.2 | Remove `espix_audio`'s twice-a-second yield | It exists only because idle starves and the watchdog trips | the yield is gone and audio is clean | todo |
| R-P4.3 | Leave the idle task; keep it for CPU accounting | FreeRTOS requires it and it does termination cleanup; a bare replacement would leak every self-deleting task | no change, recorded | done |
| R-P4.4 | Decide `CONFIG_FREERTOS_HZ` (100 -> 250) with measurement | 10 ms granularity is coarse for app authors; 250 gives 4 ms at modest cost. Check IDF's WiFi/lwIP tuning and whether tickless idle is available | `ps` CPU, `free`, throughput before/after | todo |
| R-P4.5 | Drop the idle tasks from `ps`'s listing, as `top` already does | Intended design: idle's only remaining job here is the busy figure, so it should not also appear as a process | `cmd_sys.c` `ps` | `ps` and `top` agree | todo |

## R-P5 — memory policy

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P5.1 | Experiment with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16384 -> 4096, then lower) | Every plain `malloc` under 16 KB goes to internal RAM while 8 MB of PSRAM sits idle. This is the largest structural drain | internal low-water mark up, throughput unchanged | todo |
| R-P5.2 | Merge USB 4 tasks -> 2: set `create_background_task=false` on the MSC and HID class drivers and pump them from `usb:host` | Saves 8 KB internal and three pumps become one. `usb:work` must stay separate (documented deadlock) | `75-usb.sh` passes; HID latency unchanged | todo |
| R-P5.3 | Move the USB task stacks to PSRAM where they are not DMA-adjacent | 14.3 KB internal, permanent | measured, no I/O regression | todo |
| R-P5.4 | Move the process table's string fields out of the slot | ~656 B/slot is mostly `cwd[128]`+`root[128]`+`path[128]`; shrinking the table makes raising `ESPIX_PROC_MAX` cheap | slot noticeably smaller | parked |
| R-P5.5 | Audio stays internal | Measured ~7x slower in PSRAM | no change, recorded | done |

## R-P6 — services and filesystem views

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P6.1 | A minimal service manager: a units file, a supervisor task, restart policy, output to klog | Nothing in espix runs unattended; background jobs die at logout by design, so `confine`+`sudo -u` have nothing to attach to | a unit survives the login that started it | todo |
| R-P6.2 | `cron` as a timer-shaped unit on the same supervisor | Same missing component from the other end | a periodic job runs | todo |
| R-P6.3 | `/proc` synthesised on demand from the process table, like `/dev` | Zero memory at rest, nothing generated unless read. `ps`/free already format every field | `cat /proc/meminfo` works and costs nothing idle | todo |
| R-P6.4 | Absorb `confine` into the unit file rather than delete it | It is `unveil(2)`-shaped, not `chroot`; the need is real and systemd answers it with `ProtectSystem=`/`ReadWritePaths=` | a unit declares its own view | parked |
| R-P6.5 | Add the missing signal generators: SIGALRM (`alarm`/`setitimer` on `esp_timer`), SIGCHLD, SIGWINCH, SIGPIPE | Mechanism exists; only INT/TERM/KILL/HUP are ever generated | `alarm` interrupts `sleep` | todo |
| R-P6.6 | A per-process eventfd, always in the app's read set | The one genuine hole: a process blocked in lwIP `socket_select` cannot be woken at all | `kill` reaches a networked app | todo |

## R-P7 — sweeps

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P7.1 | Polling -> events: boot-settle poll (a declared barrier already exists), klog 100 ms, `usb:host` 50 ms, terminal key queues, task-exit 10 ms spins, `cmd_run` 50 ms wait | Each is wakeups forever on an idle device | each is event-driven or has a written reason | todo |
| R-P7.2 | Short reads/writes and ignored error codes in SFTP and OTA | The user's own pitfall list; not yet audited | audited, findings fixed | todo |
| R-P7.3 | `ESPIX_PROC_MAX` / `ESPIX_FS_FD_MAX` (32 system-wide) / `ESPIX_FS_DIRS` (8) sizing review | Sized for a shell; pipelines and services will need more | sized from measurement | parked |
| R-P7.4 | `docs/KNOWN-ISSUES.md` "Polite, then forced" appears already implemented | `cmd_run.c:187-198` sends SIGINT per press then SIGKILL, one press per 50 ms slice | verified on hardware, entry struck | todo |
| R-P7.6 | `ps` names a running app `app:testapp` (its FreeRTOS task) but a finished one `testapp` (its table entry): the live section prints `tasks[i].pcTaskName` while `finished:` prints `procs[i].name`, so one process has two names and cannot be matched by name across states | It made a live process look absent, and a finished one look live, while chasing the runaway above. **Approach:** add `espix_proc_name_of(pid)` in proc.c mirroring `espix_proc_state_of`, and have the live rows print the *process* name whenever `pid != ESPIX_PID_NONE`, falling back to the task name for kernel tasks (which already have no pid). The bare task names were renamed to `espix:`-prefixed so the live table is one namespace | `espix_proc.h`/`proc.c`, `cmd_sys.c` | `testapp` reads the same before and after it exits, and `awk '$2=="testapp"'` does what it looks like | todo |
| R-P7.7 | `q` does not quit `top` | `poll_interrupt()` answers only *whether* a key arrived, and each transport drains what it read while looking for the ^C byte — so the keypress is seen and discarded. A `q` needs the session to report which key it was: one field set by the three transports (`tty_console.c`, `ssh_channel.c`, `term.c`) and read by `top`. The `Ctrl-C to quit` hint was removed in the meantime, since real ones do not print one | `espix_shell.h`, the three transports, `cmd_sys.c` | `q` leaves `top` | todo |
| R-P7.5 | Decide the USB-storage role: data volume (works today), and later an eviction target for registered caches | Real swap/paging needs the MMU (R-P3.3) *and* would page-fault over USB at hundreds of microseconds — a poor fit for a 320 MHz core with no coherent DMA. Linux on S31 uses PSRAM as RAM and SD for rootfs, no swap. "Spill a registered cache to disk" is the useful, buildable version of the idea | `docs/ROADMAP.md` reclaimer entry | a decision is written down | parked |

---

## Open questions

- **OQ1** Does `ssh_conn_t`'s `SPIRAM|DMA` allocation succeed on the S31? (R-P0.7 blocks the answer to "how much did we actually save".)
- **OQ2** With the TWDT not watching idle, what should `ps`/`top` report for a spinning app? "busy" is the honest answer; confirm that is wanted.
- **OQ3** `CONFIG_FREERTOS_HZ=250`: do IDF's WiFi and lwIP behave at 250? Is tickless idle available on S31/S3?
- **OQ4** Should `/proc` be a mount at all, or answered directly by the root VFS like `/dev`?
- **OQ5** If the reaper owns teardown (R-P1.6), what exit latency is acceptable and where does it sit in priority? It must be above idle and below anything interactive, and a burst of exits must not queue faster than it drains.

## Reference facts (so they are not re-derived)

- S31: RV32 dual-core at 320 MHz, **512 KiB SRAM**, 16 MiB octal PSRAM, Sv32 MMU, M/S/U modes, PMP/PMA. Datasheet p.44.
- Internal heap ~296 KB; PSRAM ~13 MB usable.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
- `CONFIG_FREERTOS_HZ=100`; `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536`.
- TWDT enabled, idle checked on both cores, `PANIC` off, timeout 5 s.
- `g_espix_procs` is **static .bss**, ~656 B x 12 = ~7.9 KB. `s_fds` is 0x500 = 1280 B.
- klog ring is a heap pointer (PSRAM), 96 x ~132 B = 12.4 KB.
- App stack: 256 KB PSRAM, core 1 (PIE). Relocation runs on an 8 KB internal stack.
- 71 commands registered; zero text utilities beyond `cat`/`ls`/`echo`/`df`/`sha256sum`.
- 53 `ESPIX_KLOG_DEBUG` call sites; 494 `espix_klog` calls; 8 `ESP_LOGx` in espix's own code.
- Six USB-related tasks exist across host+NCM modes; four in host mode (see R-P5.2).

## Reference reading

- S31 datasheet: `~/Downloads/esp32-s31_datasheet_en.pdf` (MMU/Sv32 on p.44).
- [espressif/esp-linux-bsp](https://github.com/espressif/esp-linux-bsp) — OpenSBI + U-Boot + kernel + Buildroot. *Replaces* the IDF runtime; not linkable into espix.
- [GrieferPig/esp32-s31-linux](https://github.com/GrieferPig/esp32-s31-linux) — Sv32 MMU Linux 6.18, XIP.
- [annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux) — its `docs/internals.md` documents the SoC quirks (no PLIC, no Zicbom, no coherent DMA, no uncached alias) and is the useful one for espix's cache/DMA work.
