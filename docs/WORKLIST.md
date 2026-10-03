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
  table *is* the boundary. The follow-on decision: the names the loader's own
  libc/IDF tables currently answer for apps -- 62 by the build's own count, not
  the old estimate of 84 -- are now intercepted by espix (R-P3.1), so
  that one layer defines the whole surface. See R-P1 and R-P3.1.
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


## R-P1 — per-process ownership (the keystone)

**Design written before the code: [APP-MEMORY.md](APP-MEMORY.md).** It records the
measured problem, the six things that must be true, the choice between tagging
allocations and a private arena, the hazards in the order they bite, and the
decisions, and an **implementation plan in the order it has to happen**.

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| R-P1.9 | The C++ allocator path is unverified at runtime | `arduino-esp32` has **no S31 support at all** — zero files mention `ESP32S31`, against 35 for the P4 — so neopixel cannot be built for this target (its `apps/neopixel/targets` says `esp32s3` and `build-apps.sh` skips it correctly) and there is no C++ app to exercise `operator new`/`delete`. Compile, link and the resolver mapping are checked; the runtime path is not. Nothing to do here until an S31-capable C++ app exists — porting Arduino to the S31 is an upstream project, not an espix one. A whole half of the allocator ABI has no runtime coverage | — | revisit when a C++ app can run on the S31 | parked |

## R-P2 — the shell surface

| id | what | why | where | done when | status |
|---|---|---|---|---|---|
| R-P2.7 | Globbing | Planned in POSIX.md | `session.c` expansion | `ls *.c` works | parked |

## R-P3 — the boundary (staged; see D2)

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P3.2 | Design (not build) the `ecall` form: a tiny app-side libespix, a trap handler, and PMP-protected kernel | This is what makes the boundary *enforced* rather than *named*, and it works on S3 and P4 too — it needs no MMU | a written design in ARCHITECTURE.md | parked |
| R-P3.3 | Sv32 page tables + CoW on S31 (`fork`) | Only `fork`-without-`exec` needs it | only if something needs it | parked |

## R-P4 — scheduling and idle

| id | what | why | done when | status |
|---|---|---|---|---|

## R-P5 — memory policy

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P5.4 | Move the process table's string fields out of the slot | ~656 B/slot is mostly `cwd[128]`+`root[128]`+`path[128]`; shrinking the table makes raising `ESPIX_PROC_MAX` cheap | slot noticeably smaller | parked |

## R-P6 — services and filesystem views

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P6.4 | Absorb `confine` into the unit file rather than delete it | It is `unveil(2)`-shaped, not `chroot`; the need is real and systemd answers it with `ProtectSystem=`/`ReadWritePaths=` | a unit declares its own view | parked |

## R-P7 — sweeps

| id | what | why | done when | status |
|---|---|---|---|---|
| R-P7.3 | `ESPIX_PROC_MAX` / `ESPIX_FS_FD_MAX` (32 system-wide) / `ESPIX_FS_DIRS` (8) sizing review | Sized for a shell; pipelines and services will need more | sized from measurement | parked |
| R-P7.5 | Decide the USB-storage role: data volume (works today), and later an eviction target for registered caches | Real swap/paging needs the MMU (R-P3.3) *and* would page-fault over USB at hundreds of microseconds — a poor fit for a 320 MHz core with no coherent DMA. Linux on S31 uses PSRAM as RAM and SD for rootfs, no swap. "Spill a registered cache to disk" is the useful, buildable version of the idea (`docs/ROADMAP.md` reclaimer entry)| a decision is written down | parked |
| R-P7.8 | Audit every espix task that is **not** reaper-managed for cleanup on every exit path: the matching WithCaps/plain delete, every fd it opened, every block it allocated | R-P1.6 moved process teardown to the reaper and left internal tasks to FreeRTOS idle cleanup, so their correctness is entirely their own responsibility. The first pass found two stack-pool mismatches -- `sshd:conn` used an alignment-sensitive check that always said "internal", and `espix:appstart` simply used the wrong call, so both deleted PSRAM stacks with plain `vTaskDelete` and leaked ~19 KB per connection (fixed, `e12376f`). ~0.9 KB per SSH connection is still unexplained, and fds and heap are not yet audited (files: `ssh_server.c`, `session.c`, `desktop.c`, `rfb.c`, `canvas_console.c`, `espix_audio.c`, `ota.c`)| each self-deleting task has every path checked and a leak test | doing -- also found: two orphaned `sh:pipe` stage tasks left blocked (waiting on a pipe nobody will close) after a malformed pipeline whose client went away. A clean pipeline does not leak -- a dmesg-into-wc-l pipeline left the count unchanged -- because `run_pipeline()` waits for its stages; a session deleted mid-pipeline has nobody left to do that wait. |
| R-P7.11 | Full-suite load-only dead-sessions: 10-fs, 35-signals and 30-proc fail with dead-session under four-way load and pass alone | The runner says so itself. A long-running boot's internal low-water falls (105 K to 56 K) and a lost SSH connection cannot be told from a device fault, so a red full run says less than it should | a full run is green with only the known 60-net failures | **parked** -- stopped after the investigation. The dead-sessions are not a command timeout (60s did not stop them; every failure is 'session gone before' with no error frame), they need the full four-way pool (the pair alone passes 84/84 twice), and they cluster in 35-signals after a kill; one run also left two finished tasks live. Root cause not found -- the next step would be the serial console and a task dump, watching espix:reaper. Several of the assertions involved also read asynchronous or system-wide state without polling, so the tests themselves are fragile and as likely to be what needs fixing. Landed independently, and kept: the eventfd spawn-failure leak, the session-error surfacing, and the fd probe moved to 95-fdpool (exclusive). |

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
- `g_espix_proc_table` is **static .bss**, 736 B x 12 slots + guard = 8840 B (8.6 KB); the completed log `s_done` adds 512 B (`ESPIX_PROC_DONE_MAX` 8 x 64 B), both measured from the ELF. `s_fds` is 0x500 = 1280 B.
- klog ring is a heap pointer (PSRAM), 256 x ~132 B = ~33 KB (R-P0.8 raised it from 96).
- App stack: 256 KB PSRAM, core 1 (PIE). Relocation runs on an 8 KB internal stack.
- 71 commands registered; zero text utilities beyond `cat`/`ls`/`echo`/`df`/`sha256sum`.
- 53 `ESPIX_KLOG_DEBUG` call sites; 494 `espix_klog` calls; 8 `ESP_LOGx` in espix's own code.
- Six USB-related tasks exist across host+NCM modes; four in host mode (see R-P5.2).

## Reference reading

- S31 datasheet: `~/Downloads/esp32-s31_datasheet_en.pdf` (MMU/Sv32 on p.44).
- [espressif/esp-linux-bsp](https://github.com/espressif/esp-linux-bsp) — OpenSBI + U-Boot + kernel + Buildroot. *Replaces* the IDF runtime; not linkable into espix.
- [GrieferPig/esp32-s31-linux](https://github.com/GrieferPig/esp32-s31-linux) — Sv32 MMU Linux 6.18, XIP.
- [annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux) — its `docs/internals.md` documents the SoC quirks (no PLIC, no Zicbom, no coherent DMA, no uncached alias) and is the useful one for espix's cache/DMA work.

## Expected: 60-net and 75-usb want a USB-NCM build

The full suite reads 261 passed / 3 failed, and all three are one cause with
nothing to do with parentage: usb0 is absent and the usb command is not found,
so this image has USB host (the MSC and HID tasks are up) but not USB-NCM, while
those two suites assert an NCM build. Per the project's own rule that suites
skip according to config, hardware and attached peripherals, these should SKIP
rather than FAIL -- a small test-side fix, not a kernel one. No kernel
consequence, and not a reason to hold anything back.

**The heap line is neither, and reading it as a regression cost time.** "fell to
46K this run (was 93K)" compares the internal low-water mark before and after a
run -- cumulative since boot and never recovering (device.sh:1081) -- so it can
only fall, and a run that does more (the whole suite, lwIP included) drives it
lower than one that does less. The controlled comparison is the same suite
across builds: at 96 and at 256 log lines the full suite reads the same 46K, so
the ring is irrelevant, which is also what settled R-P0.8.

## R-P1.5 -- the design, decided

Linux-correct and resource-efficient agree on all of this; the choices are
recorded as decisions so the implementation is mechanical.

**Today the slot is the zombie.** A finished process keeps its slot -- state,
exit code -- until espix_proc_alloc_slot() recycles the oldest finished one, so
history competes with concurrency inside a fixed 12-slot table. That is the
problem; everything below follows from fixing it.

1. **The completed log is a small fixed ring, separate from the live table.**
   Eight entries, statically allocated in espix_proc, holding what ps and wait
   need: pid, name, state, exit code, ppid, start and end times. Fixed and
   static because it is bookkeeping, not data: it must never be the reason a
   spawn fails. Linux keeps only unreaped zombies, and even those are bounded
   by the process table; eight is the same idea sized to what a person reads.

2. **wait consumes the entry, as POSIX does.** espix_proc_wait() looks for the
   pid in the live table first -- still running, so block -- and otherwise in
   the completed log, takes the status and marks the entry reaped. A second
   wait for the same pid then fails, which is ECHILD. This is the behaviour
   waitpid(2) specifies and the reason a zombie exists at all.

3. **An unwaited entry lives until it is reaped or the ring wraps.** Linux
   would keep it forever, and that is a leak the parent owes; a fixed ring is
   the resource-efficient compromise, and the honest divergence is that the
   status of a process nobody waits for can be lost once eight more finish.
   R-P1.6 is what makes it deterministic: the reaper is the thing that waits
   for the processes nothing else will.

4. **espix_proc_find() becomes live-only, so kill and signals reach only live
   processes.** A zombie cannot be signalled in Linux either (kill(2) gives
   ESRCH once it is reaped), and today find() includes finished slots, so a
   finished process is still signallable. The log is consulted by ps and wait
   and by nothing else.

5. **The slot is released at finish**, after its contents are copied into the
   ring, marked FREE last and under the lock. The one real rewiring this
   forces is the wait path: it currently blocks on a per-slot event bit, and a
   slot can now be reused immediately, so a waiter must be woken by the finish
   itself. One global event bit or semaphore set at every finish, with the
   waiter re-scanning the live table and then the log, is the simplest correct
   equivalent of Linux's wait queue -- and it removes the index-tied event bit
   that the current wait depends on.

**Verification.** ps keeps showing history, from the ring instead of the
table; a finished process no longer occupies a concurrency slot (spawn twelve,
finish them, spawn twelve more); wait returns a status exactly once and fails
the second time; kill on a finished pid reports not-found; and the existing
exit-status tests in 30-proc.sh still pass unchanged.

**Built** (`bf12f51`). Two places the code decided something the sketch did not,
and one thing it implements but no test yet observes:

- **The ring shows reaped entries too.** It is a log as well as the zombie
  store, and reaping only marks the record -- so a reaped exit leaves wait's
  reach but stays readable in `ps` until the ring wraps. Linux drops it from
  `ps` at once; the affordance was judged worth the divergence, and it is what
  keeps `ps` useful once the shell has reaped everything it ran.
- **`espix_proc_wait()` consumes, but espix's own non-parent waiters do not.**
  `proc_wait_gone()` in `proc.c` is observe-only, and kill's escalation and
  `espix_proc_hangup()` use it. Without the split, `kill -9` racing a
  foreground app's own exit would collect the status, and the shell's `run`
  would then report `pid N: ESP_ERR_NOT_FOUND` for a process that exited
  normally.
- **"wait fails the second time" and "spawn twelve, finish, spawn twelve
  more" are properties of the code, not observed tests.** There is no
  app-visible `waitpid` for a suite to call twice, and filling the table to
  prove a slot came back risks a full table that cannot load the next app's
  binary. The observable half -- the ring bounded to eight, and a finished pid
  answering "no such process" -- is tested. This is the same shape as R-P1.4's
  dormant half and closes the same way: when an app can wait.

## R-P1.8 -- the design, as built

IDF has no dup, and the work is about the two tables espix sits between: the
number an app holds is IDF's descriptor table, the open file is espix's key.

1. **A duplicate is the same key, counted.** `fd_slot_t` gained `refs`;
   `dup`/`dup2`/`F_DUPFD` register a second IDF entry whose `local_fd` is the
   *same* key, so the offset is shared as POSIX requires, and `vfs_close()`
   drops one reference and only the last calls the filesystem.
2. **The floor is reached with placeholders.** `register_fd()` only ever hands
   out the lowest free number, so `dup2(newfd)` and `F_DUPFD(min)` briefly
   register every free number below the wanted one, then release them. The
   honest cost: another task allocating in that window can take the number
   first, and `dup2` then fails EMFILE rather than close a descriptor espix
   does not own.
3. **The reaper closes every entry on a key.** `espix_fs_fds_close_owned()`
   used to stop at the first descriptor matching a key; a process that died
   holding duplicates would have leaked the rest of the pool.
4. **Only espix file keys.** A socket or device descriptor travels through
   another VFS and has no key to share, so it is refused with EBADF rather
   than half-done.

**The trap.** The first version used `esp_vfs_register_fs_with_id()` to learn
the VFS index `register_fd()` needs. It registers a *path-less* VFS
(`path_prefix_len = LEN_PATH_PREFIX_IGNORED`), and `get_vfs_for_path()` skips
those entirely -- so the root VFS answered no path, a later boot step's
`ESP_ERROR_CHECK` aborted, and the loader rolled the image back. The comment in
`espix_vfs_register_root()` had warned about exactly this shape. The index now
comes from the descriptor's own table entry (`get_fd_entry()`), which needs no
registration change and works for any VFS.

**Verified.** 30-proc is 57/57 on the S31. The app's command writes,
duplicates, closes the original, reads the bytes back through the duplicate,
`dup2`s onto 10 and reads again, takes `fcntl(F_DUPFD, 8)`, and reopens to show
the first number came back -- so a leak fails it. A second mode exits holding
three duplicates, and the descriptor capacity is unchanged afterwards.

## R-P1.3 -- the history, and why it is kept

**The approach that was wrong.** Every open and close must be espix's, with nothing held by IDF that espix cannot release. Concretely: allocate the fd the way `dev.c` already does for *device* fds -- `espix_dev_fd()` / `ESPIX_DEV_FD_BASE` and the loop at vfs.c:1359 show the in-repo pattern -- so that `local_fd == fd` (`vfs.c:561`), keep that number in the slot, and release it from the reaper with `esp_vfs_unregister_fd()`. Then espix's `s_fds` is indexed by the one number everyone uses, the `128+` base and the key indirection disappear, and IDF holds no per-open state that outlives a dead process. This also retires the `ESPIX_DEV_FD_BASE` collision check at vfs.c:604, which exists only because two number spaces had to be kept apart. **Do it as one piece** -- it changes fd numbering for every file operation, so half of it is every app's I/O broken. Two things follow: find out whether `espix_proc_self_pid()` is stamping an owner at open time at all (the most likely failure -- the release then finds nothing to match), and treat *fd-table exhaustion taking the board down* as a bug in its own right, because it is reachable today by any app that leaks descriptors.
