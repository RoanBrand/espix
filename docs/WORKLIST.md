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
| R-P0.3 | `check-abi.py`: strip comments/strings before matching; cover resolver + explicit `{name,&sym}` tables + C++ entries; fail the build on a shadowed duplicate | It currently parses comments as code and cannot see two thirds of the surface | `tools/check-abi.py` | a deliberately shadowed name fails the build | todo |
| R-P0.4 | Prototype checking at build time: a generated TU that takes each published symbol's address through its *public header* declaration, under `-Werror` | Names-only checking is how the `off_t` widening slipped through | new `tools/` + CMake | flipping a prototype in one place breaks the build | todo |
| R-P0.5 | SSH connection task stack to PSRAM (`xTaskCreateWithCaps`, `MALLOC_CAP_SPIRAM`) | 8192 B x up to 8 = **64 KB** of internal RAM, the largest single consumer | `ssh_server.c:636` | 8 sessions open, internal low-water mark up ~64 KB, throughput unchanged | todo |
| R-P0.6 | `ssh_chan_t` to PSRAM | ~2.2 KB x 8 = 17.6 KB, from a plain `calloc` | `ssh_channel.c:2188` | as above | todo |
| R-P0.7 | Establish whether `ssh_conn_t`'s `SPIRAM\|DMA` request actually succeeds | If it falls back, ~13.4 KB x 8 = **107 KB** is silently internal | `ssh_server.c:245` | a logged answer, and throughput measured either way | todo |
| R-P0.8 | `CONFIG_ESPIX_KLOG_LINES` 96 -> 256 | Ring is already PSRAM, so this costs ~32 KB of PSRAM and nothing internal | Kconfig | dmesg shows 256 lines | todo |
| R-P0.9 | Log cleanup: review the **53** `ESPIX_KLOG_DEBUG` call sites (of 494 total), demote/delete what was debugging scaffolding | Leftovers cost ring space and mask real messages | `grep -rn ESPIX_KLOG_DEBUG` | each one justified or gone | todo |
| R-P0.10 | Soak with `CONFIG_ESPIX_PROC_ABI_WATCHPOINT` armed to catch the writer past `g_espix_procs` | That write disables half the ABI resolver until reboot; the watchpoint is armed and waiting | `proc.c`, `KNOWN-ISSUES.md:346` | the writer is named, or the entry closes as unreproducible | todo |
| R-P0.12 | Move the exit-path tests out of `apps/hello` into `tests/app`, and slim `hello` back to a showcase | `hello` is the example of an app, not the ABI's test; it had grown filesystem coverage (which `tests/app` already has as `stat`/`write`/`read`/`chmod`/`probe`) and the exit cases | `apps/hello/main/hello.c`, `tests/app/main/testapp.c`, `tests/suites/30-proc.sh` | `hello` is 37 lines; `testapp exitcall/_Exit/abort/assert` exist; 30-proc asserts all four | **done** |
| R-P0.11 | Make an unhandled signal's default action delete the task outright (as POSIX "terminate" means), instead of setting a cooperative flag | Today the default is *also* cooperative, so `kill` cannot touch an app that busy-spins and installed no handler — the common case for code we did not write | `proc.c` `sig_dispatch`/default actions, `proc_force_kill()` | `kill <pid>` stops a handlerless spin loop | todo |

## R-P1 — per-process ownership (the keystone)

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| R-P1.1 | Intercept `malloc`/`calloc`/`realloc`/`free`/`strdup` through the resolver, PSRAM-first with an internal fallback | Apps get the firmware's allocator, which puts everything under 16 KB in internal RAM; and it is the seam the arena needs | `abi_resolver.c` (the `getenv` pattern) | an app's allocations are visible to espix | todo |
| R-P1.2 | Per-app heap arena, freed whole on exit and on kill | This is the Doom leak: 13.2 MB -> 571 KB over two runs. Nothing can reclaim what was never recorded | `exec.c`, `proc.c` | two Doom runs return the PSRAM | todo |
| R-P1.3 | Give `fd_slot_t` an owner and close a dead process's fds | `fd_slot_t` is `{lower_fd, mount}`, so a force-killed app leaks its descriptors | `vfs.c:182` | a killed app's `cat` releases its fd | todo |
| R-P1.4 | `ppid`/children tracking + `SIGCHLD` | Nothing tracks parentage; ownership is "which session pointer you hold". Needed by services, job control and `waitpid` | `espix_proc.h:59` | a child exit notifies its parent | todo |
| R-P1.5 | Separate the **live table** from a small **completed log**; recycle a slot on reap | Retaining finished slots is a debugging affordance, not POSIX — Linux keeps only unreaped zombies, and `ps` history is not a thing there. The live table then sizes to *concurrency*, not to history | `proc.c:133`, `cmd_sys.c` `ps` | `ps` shows history from the ring; slots recycle at once | todo |
| R-P1.6 | Make `espix:reaper` the **single teardown point**: per-invocation tasks (command, ssh conn, app) stop calling `vTaskDelete(NULL)` and park instead, and the reaper deletes them — reclaiming the arena, fds and screen in the same pass | `prvCheckTasksWaitingTermination()` is a private static in FreeRTOS's `tasks.c` and cannot be called. But deleting *another* task frees its TCB in the caller, so if nothing self-deletes, the idle task stops being *required* for espix's cleanup. IDF components and app-created pthreads still self-delete, so this reduces the dependence rather than removing it. Also retires the `espix_gfx_recover()` special case | `reaper.c`, `exec.c:616`, `session.c:469`, `ssh_server.c:495` | a finished process is torn down entirely by the reaper | todo |
| R-P1.7 | Reclaim the screen through R-P1.6 instead of the `espix_gfx_recover()` special case | Today the canvas is the one resource that *is* reclaimed, by hand | `proc.c:236` | the special case is gone | todo |
| R-P1.8 | `dup`/`dup2`/`fcntl(F_DUPFD)` in the VFS | Currently "left out" only because IDF stubs it. Prerequisite for pipes and redirection, and portable to all targets | `vfs.c` | `dup2` from an app works | todo |

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
| R-P4.1 | Stop the TWDT watching idle tasks (or lengthen the timeout) | It is what stops a low-priority app spinning. Today: enabled, both cores' idle checked; `PANIC` is **off**, so confirm on hardware whether it warns or resets | a busy app at priority > idle is not a reset | todo |
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
