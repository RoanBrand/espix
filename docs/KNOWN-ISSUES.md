# espix known issues

Behaviour that is already implemented and will still surprise you. Everything
here is deliberate, understood, or at least understood well enough to be worth
writing down — the purpose is that it gets recognised rather than debugged from
scratch a second time.

For work espix might take on see [ROADMAP.md](ROADMAP.md); for defects that
belong to ESP-IDF rather than to espix see [UPSTREAM.md](UPSTREAM.md); and for
the platform surprises that keep causing entries here — what may not be used
together, and where ESP32/IDF differs from what a POSIX or FreeRTOS habit
expects — see [GOTCHAS.md](GOTCHAS.md).

## Processes

- **A hard kill leaks whatever the app held.** SIGKILL deletes the task
  outright, so `teardown()` never runs. The neopixel app hands its RMT channel
  back there (`rmtDeinit()`), and without that the channel and its GPIO
  reservation outlive the app — which shows up as `GPIO 48 is not usable` on the
  next run and eventually as no free channel at all. `kill` and Ctrl-C ask first
  and only escalate after the grace — but they *do* escalate, so this is not
  specific to `kill -9`: an app that ignores SIGTERM leaks exactly the same
  way once `TERM_GRACE_MS` runs out. SIG_IGN buys the grace period, not
  survival, which is itself a divergence from POSIX worth knowing —
  `kill -TERM` on Unix leaves such a process running indefinitely.
  There is no address space to tear down and no per-process ownership of heap
  or fds, so nothing can reclaim it for the app. `tests/suites/35-signals.sh`
  pins both halves.

- **`ps` shows at most 8 finished processes.** `cmd_ps` stack-allocates
  `espix_proc_info_t procs[8]` while `ESPIX_PROC_MAX` is 12, so on a busy table
  some exits are silently missing from the `finished:` list. The array is on the
  session task's stack, which is why it is not simply `ESPIX_PROC_MAX` — each
  entry is ~184 bytes.

- **`session->fg_pid` is written and never read.** It is set on every foreground
  run and cleared afterwards, but nothing consults it. Scaffolding for job
  control; it does not mean anything yet, so do not build on it.

- **A compute loop never sees a signal.** Delivery happens at the points where
  an app calls into espix — `sleep`, `usleep`, `nanosleep`, `pause`. A loop that
  blocks on nothing has no delivery point and will not run a handler, which is
  exactly what `espix_sigcheck()` is exported for. `testapp sig spin` is the
  case in the flesh, and `tests/suites/35-signals.sh` covers it. SIGKILL is the answer when it is somebody else's binary.

- **A hard kill will not survive a process inside libc.** `proc_force_kill()`
  deletes the task, and `vTaskDelete()` then runs newlib's cleanup *on the
  killer's task* — `_reclaim_reent()` → `esp_cleanup_r()`, which fcloses the
  dead task's three streams. `fclose` needs each `FILE`'s lock, and the deleted
  task may have been holding one: it is exactly what `fgets(stdin)` does for as
  long as it waits.

  That reset the board every time until `proc_force_kill()` learned to let the
  target out first — it sets `stop_requested` (not a signal, so nothing
  becomes catchable) and gives it `KILL_UNWIND_MS` to leave libc, which a
  process blocked on input does at once because espix's blocking calls poll
  `espix_sigcheck()`. The residue is what remains: a process that ignores the
  hint *and* sits inside libc is still deleted at the end of that grace, and
  that case is unsafe. Nothing in espix does it today, and an app could.

  The control worth keeping, because it is what identified this: the same
  `kill -9` on a process blocked in `sleep()` was harmless, and SIGTERM on the
  blocked one was harmless too. `tests/suites/15-streams.sh` pins both kill
  paths, and both were made to fail before they were made to pass.

- **A second fault under sustained inbound traffic. Fixed once, and reopened
  2026-09-14 — see the recurrence at the end of this entry.** Kept in full,
  because what it looked like the day before it was understood is the useful
  part: the entry reasons its way to the doorstep and stops. The fix it reasoned
  its way to has now been outlived by the bug, so read the history knowing the
  conclusion did not hold.

  What was recorded at the time: during a throughput run, `exccause 0x47`
  (CacheError) inside `Cache_WriteBack_Addr`, reached from `psa_mac_update` →
  `esp_sha256_update` → `esp_sha_dma_process` → `esp_cache_msync`, on the SSH
  receive buffer. That buffer is `ssh_conn_t.in_buf`, and the connection struct
  is allocated from PSRAM, so the SHA driver takes its DMA path over external
  RAM. "This is not espix misusing the API — `sha.c` explicitly handles an
  external-RAM input with a cache sync — but espix is what puts a PSRAM buffer
  there." Not reproduced since: two further throughput runs, a full suite, and a
  targeted stress of concurrent flash reads against SSH crypto, all clean.

  Every one of those observations was right. The missing piece was a rule, not a
  fact: **`esp_cache_msync()` must not be called during a flash operation.** A
  flash write disables the cache, and cache maintenance inside that window does
  not stall, it faults. The "targeted stress of concurrent flash reads against
  SSH crypto" came closest and was clean because a *read* through the cache is
  not the hazard — an erase or a write is.

  It became reproducible when the test suite started running four suites at once
  (about one run in three) and was fixed by `CONFIG_SPIRAM_XIP_FROM_PSRAM`, which
  moves `.text` and `.rodata` into PSRAM so a flash operation no longer needs the
  cache off. See [GOTCHAS.md](GOTCHAS.md), "What a flash write actually stops".

  **Measured afterwards, because "fixed" was still an inference.** The claim
  rested on reading IDF's source, plus three clean runs. It is now a reading off
  the device: `free` reports `flash mmap: none live`, which is the actual
  condition in `spi1_start()` — a write keeps the cache only while
  `flash_mmap_remain()` is false. `tests/suites/65-flashstall.sh` asserts it, so
  the day something maps a partition and forgets to unmap it, a suite says so
  instead of the board panicking. The suite's older timing evidence (506ms vs
  510ms for an erase) was never capable of settling this either way — it was
  underpowered, not negative; the suite now says so in its own comment.

  **One recurrence since, and it does not reopen this.** A `-j4` run panicked
  with the same signature — `Cache_WriteBack_Addr` ← `cache_hal_writeback_addr`
  (`esp_cache_msync`) in `sshd:conn`, `exccause 71`, `EXCVADDR` 0. But the board
  turned out to be running an image built from an uncommitted working tree that
  no longer exists (`72354c6-dirty`, while `build/` held `b5f4232-dirty`) —
  `make test` does not flash. So it is not evidence about any committed code.
  `tests/run.sh` now refuses to start when the board and `build/` disagree, and
  `uname -a` carries the build id that makes that checkable.

  Two things learned from that dump are worth keeping regardless, and both are
  in [GOTCHAS.md](GOTCHAS.md) under "Reading a CacheError panic": `exccause 71`
  is `PANIC_RSN_CACHEERR + XCHAL_EXCCAUSE_NUM`, and *"Cache disabled but cached
  memory region accessed"* is only the **fallback** of seven possible cache
  errors — the specific one is printed to the UART and stored nowhere else. This
  entry's original diagnosis assumed the fallback. It may well have been right,
  but it was not read.

  **Reopened 2026-09-14: it recurred on an image whose identity checks out.**
  A `-j4` `make test` panicked and rebooted. This time the board and `build/`
  agreed — device `96a6320-dirty+279248328` against the same describe and ELF
  SHA in `build/espix.bin` — `run.sh`'s identity check passed, and `espcoredump`
  decoded the dump rather than refusing it. So the escape that dismissed the
  previous recurrence does not apply, and this is evidence about committed code.

  ```
  exccause 0x47 (CacheError)   excvaddr 0x0   pc Cache_WriteBack_Addr+65
  sshd:conn
    ssh_packet_read (ssh_transport.c:512)
      psa_mac_update -> esp_hmac_update_transparent -> esp_sha256_update
        -> esp_sha_dma_process (sha.c:289) -> esp_cache_msync(0x3c12f938, 1984)
          -> cache_hal_writeback_addr -> Cache_WriteBack_Addr
  ```

  Identical to the original signature, including the buffer: `c=0x3c12f5c0` puts
  the connection struct in the `0x3C…` external-memory window, so the SHA driver
  takes its DMA path over PSRAM exactly as described above.

  **What this costs the "fixed" verdict.** `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` was
  enabled in this build. The fix worked by removing the need to disable the
  cache during a flash operation; the fault recurred anyway. Either something
  still turns the cache off, or the cause was never *"cache disabled but cached
  memory region accessed"* — which is the **fallback** reason this entry admits
  it assumed and never read. The fix did make it much rarer, and that is not
  nothing, but rarer is not the same as explained.

  **And the identifying line was lost again.** Nothing was capturing the UART,
  so which of the seven faults fired is unknown for this occurrence too. That is
  now harder to repeat by accident: `make test-panic` runs the suite with
  `tools/serlog.sh` holding the port, and `run.sh` says outright when a panic
  went by with no capture running.

  Context worth keeping for a reproduction: the panic landed inside
  `run_program` → `chan_poll_interrupt` with `testapp sink` running — the stdin
  leg of `45-throughput`, in the quiet phase, with the suite running alone. So
  concurrency across suites is not required to trigger it.

- ~~**Every sftp session leaks about 440 bytes of internal heap.**~~ **Found and
  fixed:** the sftp channel built a login environment and never freed it.
  Roughly 9 TLSF blocks per session, and they never came back.

  Found because the per-suite heap attribution put `40-transfer` and
  `45-throughput` at `+2K / +45 blocks` twice running while every other suite sat
  at ±1K with zero net blocks. Both are the file-moving suites, and OpenSSH 10.3
  drives `scp` over SFTP.

  **Measured on an idle board, outside the suite entirely** (`used K`, `blocks`):

  ```
  A idle baseline                    177  337
  B after 6 scp downloads            180  392     +3K  +55
  C after 150s idle                  180  392     unchanged -- it does not come back
  ```

  Then bisected, 8 operations at a time:

  ```
  plain ssh, tiny command            +0K   +0     clean
  plain ssh, bulk output (dmesg)     +0K   +0     clean -- so not data volume
  sftp fetch of a MISSING file       +3K  +72     leaks
  sftp fetch that succeeds           +3K  +72     leaks the same
  ```

  So it is the sftp **session**, not the transfer: a fetch that opens no file at
  all leaks identically to one that moves bytes. Thirty sessions during this
  investigation took internal from 177K to 190K. PSRAM stayed flat at 98K, which
  matters because `sftp_t` is the one big allocation and it is PSRAM-backed
  (`CONFIG_ESPIX_SSH_SFTP_IN_PSRAM`) -- whatever leaks is internal and small.

  **Ruled out by measurement, not by reading:** TIME_WAIT. IDF's lwIP is built
  `MEM_LIBC_MALLOC`/`MEMP_MEM_MALLOC`, so PCBs come from the C heap, and
  `CONFIG_LWIP_TCP_MSL=60000` holds a closed connection for ~120s -- which fits
  "many small blocks, few bytes" so well that it was the working theory. Reading
  C above is what killed it.

  **Ruled out by reading:** `sftp.c` allocates in exactly two places and frees in
  one, and its teardown closes every handle (`fclose`/`closedir`) before the
  free; handles live in a fixed array inside `sftp_t`. The `subsystem` request
  branch in `ssh_channel.c` allocates nothing, the sftp branch builds its
  `espix_session_t` on the stack, and the shared `out:` path frees the locks,
  the stdin queue, `exec_cmd` and the channel. None of the obvious paths holds
  anything.

  **Instrumentation found it in one run, after reading had failed.**
  `CONFIG_HEAP_TRACING_STANDALONE` with `heap_trace_start(HEAP_TRACE_LEAKS)`
  around the whole sftp channel, cleanup included, dumped nine unfreed
  allocations totalling 336 bytes and named the allocator outright:

  ```
  ssh_channel_run          ssh_channel.c   <- the sftp branch
    apply_account          ssh_channel.c:1583
      espix_env_set_login_defaults  env.c:359
        espix_env_set               env.c:111/117/134
  ```

  292 bytes for the table plus 4/5/10/5/5/5/5/5 for the names and values. With
  TLSF's per-block overhead that is ~444 bytes, against the ~440 measured.

  **The bug:** `finish_session()` frees the session's environment
  (`espix_env_free()`), and the exec and shell paths both call it. The sftp
  branch went straight to `out:` instead, so the environment `apply_account()`
  had just built was never released -- and sftp never reads it in the first
  place, since `base_dir()` works from `session->cwd`.

  Fixed by freeing it on that branch. Not by calling `finish_session()`: that
  also hangs up processes and sends the channel close, and the sftp path has no
  processes and has already sent its own.

  **After:** sixteen transfers, downloads and uploads, move the internal heap by
  0 bytes and 0 blocks, against +72 blocks per eight before.

  A residue remains in those two suites -- about +28 blocks each, down from +45
  -- which is not scp: measured directly, downloads and uploads are now clean.
  The suites also write files and run `sftp -b`, and that is where to look next.

- **Killing a process under load strands the connection task that killed it.
  Open, with backtraces.** Two distinct deadlocks, both from deleting a task
  that holds a lock, and between them they are the `<<<dead-session>>>` cascade
  that `15-streams` and `35-signals` have been producing under `-j4` for weeks.

  A `make test` ended with `sshd-conn-tasks-held=4(was 2 at start)` and
  `55-sessions` skipping for lack of free slots. Twenty-four minutes later, idle,
  the board still held them -- states `R,B,B,B`, **none `D`**, so these are live
  tasks that will not exit, not reclamation running late. Internal had gone from
  166K to 203K and stayed. A deliberate `crash` captured all four:

  ```
  THREAD  5  cmd_top -> vTaskDelay                     <- a healthy session, the control
  THREAD 11  prvDeleteTCB -> _reclaim_reent -> esp_cleanup_r
               -> _fclose_r -> __retarget_lock_acquire_recursive   xTicksToWait = portMAX_DELAY
  THREAD 13  esp_linenoise_edit_insert -> refresh_line -> ssh_edit_write
               -> send_cooked (ssh_channel.c:396) -> xQueueTakeMutexRecursive
                                                     xTicksToWait = portMAX_DELAY
  ```

  Thread 11 is the hazard `proc.c:520` already describes in full: `vTaskDelete()`
  runs `_reclaim_reent()` **on the killer's task**, which fcloses the dead task's
  streams, and `fclose` needs the very FILE lock the deleted task was holding.
  Thread 13 is the same shape one level up -- the deleted task was inside a
  channel write holding `ch->tx_lock`, and the next writer waits forever.

  **Why only under load, which was the part that made it look mysterious.**
  `kill_unwind()` sets `stop_requested`, aborts the target's delay and gives it
  `KILL_UNWIND_MS` (250ms) to leave libc on its own; an app that does not is
  deleted anyway. Under `-j4` the cores are saturated -- the watchdog fired in
  this very run -- so the target may not be *scheduled at all* inside that
  window. The grace is a probability, not a guarantee, and load is what shifts
  it. `35-signals` then tests exactly this on purpose: "a process ignoring
  SIGTERM is stopped anyway, after the grace".

  **What is already right, and where it stops.** `finish_session()` takes
  `tx_lock` with a bounded wait and says why: *"the process above may have died
  inside a write still holding it -- espix_proc_kill() deletes the task outright
  and cannot unwind what it held."* That reasoning was applied at exactly one of
  the ten lock sites. The other eight `tx_lock`/`rx_lock` acquisitions in
  `ssh_channel.c` use `portMAX_DELAY`.

  **The fix is not one change.** Bounding those eight turns thread 13 from a
  permanent strand into a failed write and a clean teardown, and it follows a
  precedent already in the file. Thread 11 cannot be bounded that way -- the
  wait is inside FreeRTOS's own `prvDeleteTCB`, reached from `vTaskDelete()` --
  so it needs either a longer or load-aware unwind grace, or not force-deleting
  a task that is inside stdio at all. Lengthening the grace lowers the odds and
  closes nothing.

  Not to be confused with the deferred-reclamation case above: that one shows
  `D` tasks and clears itself, this one shows `B` and does not.

- **Something writes past the process table and silently disables half the ABI
  resolver. Open, and now watched for.** A `-j4` run failed 21 assertions across
  `15-streams`, `70-env` and `45-throughput`, every one of them:

  ```
  E (1583885) ELF: Can't find symbol getenv
  espix: /home/esp/testapp: undefined symbol: getenv   [exit 126]
  ```

  They failed alone too, and the board stayed that way: 35 minutes later, with
  no reboot, `tools/esp.sh '/home/esp/testapp env'` still reproduced it. That
  made it the first corruption here that could be interrogated while it was
  still happening, and the interrogation is the useful part of this entry.

  **How it was narrowed, with no debugger.** `getenv` is served only by espix's
  resolver in `abi_resolver.c` — the loader's default table deliberately does
  not export it (`abi_env.c` explains why publishing newlib's would be
  backwards). Two facts then separate the possibilities:

  - `testapp` relocates `getpid` *before* `getenv`, and `getpid` is **also**
    resolver-only. It resolved. So the resolver was installed and running.
  - On the broken board `/bin/sigtest` ran, and anything needing `getenv` did
    not. Registration order in `proc.c` is signal first, env second.

  So the resolver was walking `s_tables[0]` and not `s_tables[1]`: either
  `s_table_count` had gone from 2 to 1, or the second entry was cleared.

  **And the map says how that is reachable:**

  ```
  3fca8fd0  00001ec0  B g_espix_procs    <- 12 slots x 656 bytes, ends at 3fcaae90
  3fcaae90  00000004  b s_table_count
  3fcaae94  00000020  b s_tables
  ```

  `s_table_count` is the first word after the process table. One word written
  one past the end of `g_espix_procs` lands exactly on the counter that decides
  how many ABI tables get searched. Nothing rewrites it afterwards, which is why
  the damage is permanent and why it surfaces an hour later as a missing symbol
  rather than as anything resembling its cause.

  **This is a theory about the reach, not a finding about the writer.** Every
  `g_espix_procs[i]` loop in `proc.c` is correctly bounded, so it is not a
  visible off-by-one — a wild store, a bad `memcpy` size, or an overflow inside
  the last slot (`cwd[]` and `root[]` are the `ESPIX_PATH_MAX` arrays in there)
  all reach the same word.

  **So it is watched rather than guessed at.** `CONFIG_ESPIX_PROC_ABI_WATCHPOINT`
  (default y) arms one of the ESP32-S3's two hardware watchpoints on
  `s_table_count` and one on `s_tables[1]`, on both cores — the registers are
  per-CPU, and app tasks float between them. The next stray write panics at the
  instruction that did it, and the backtrace names the writer.

  Proven to fire before being trusted, which is what `crash abi` is for:

  ```
  Debug exception reason: Watchpoint 0 triggered
  A8 : 0x3fcaae90                     <- &s_table_count
  0x4202abd8: espix_proc_abi_watch_selftest at abi_resolver.c:151
  0x420181b6: cmd_crash at cmd_run.c:494
  ```

  **What is deliberately not done yet.** The resolver could check its own state
  and report "ABI tables damaged" instead of "Can't find symbol getenv". That is
  worth having and it is the wrong order: a guard that makes the symptom
  survivable also makes it quieter, and the writer is findable right now. It
  goes in with the fix.

  **A dump will not help you here, and that is a decision.**
  `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` would put `.bss` in the core dump, but IDF
  asks for at least 128KB of coredump partition and ours is 64KB; growing it
  moves `storage` and means reflashing the filesystem. See the note in
  `sdkconfig.defaults`. The watchpoint stops at the instruction, which is better
  evidence than the wreckage anyway.

  Not claimed to be the same bug as the CacheError above, the `Corrupted MAC on
  input` that killed a `top` session during the same run, or the 17K internal
  heap low-water those runs reached. They may share a cause. Saying so before
  measuring is the move that put a wrong verdict in this file twice.

- **The PSRAM heap's free list was found corrupt, once, and it is open.** A
  `-j4` run rebooted mid-suite; the core dump says:

  ```
  Panic reason: assert failed: insert_free_block tlsf_control_functions.h:400
                (current && "free list cannot have a null entry")
  tcpip_thread → ip4_input → tcp_input → pbuf_free → free() → tlsf_free → abort
  ```

  `tcpip` is the **finder, not the culprit**. lwip freed an ordinary pbuf and
  TLSF tripped over damage already done to `control` at the base of the PSRAM
  heap. `pbuf_free` in the TCP/IP thread is simply the most frequent `free()` on
  the box, so it gets there first — the same reason the entry below surfaced as
  four panics in four unrelated places.

  Reading the run that found it: 51 assertions failed, all but three of them
  `<<<dead-session>>>`. That is **one** event, not 51 — the reboot killed four
  worker sessions at the same instant. `reset-reason-changed('software'->'')`
  and `coredump-unanswered` in the same report are empty answers from a device
  that was still coming up, not findings.

  What is ruled out, and it is the tempting one: this is *not* the PSRAM/DMA
  cache-line spill. The entry below records that theory being tested and
  disproved — taking the SSH buffers out of PSRAM entirely only changed which
  heap the corruption landed on.

  Hunting it since with `CONFIG_HEAP_POISONING_COMPREHENSIVE`: four full runs,
  no catch. Note when you try: the poisoned build's per-allocation canaries cost
  enough internal RAM that eight concurrent sessions drove `min free internal
  since boot` to **0 K**, which fails assertions in ways that read as unrelated
  bugs (`ESP_ERR_NO_MEM` spawning an app, a session dying with status 255).
  `tests/run.sh` now prints that low-water mark every run so it cannot be missed
  a second time.

- ~~**One earlier heap corruption remains unexplained.**~~ **Fixed.** It was a
  double free in espix's own command history, and the whole shape of it is worth
  keeping, because almost nothing about the way it presented pointed at the
  cause.

  Command history is owned by the *user* rather than the session, so two
  sessions logged in as the same account are handed the same `espix_history_t`.
  The module had a mutex and it covered only `espix_history_for()`, the slot
  lookup. Every mutation after that ran unlocked, and two sessions arriving at a
  full list together both ran `free(h->entries[h->count - 1]); h->count--;` on
  the same pointer.

  It surfaced as four different panics, none of them in `history.c`:
  `tlsf_free` inside `esp_vfs_select` from `chan_poll_interrupt()` on the PSRAM
  heap, a `CacheError` on IDLE0, a `free(0x15)` inside `esp_linenoise`'s own
  history, and only once as the double free itself. A corrupted heap is noticed
  by whoever frees next, and `chan_poll_interrupt()` polls `select()` every
  50ms, so it usually got there first.

  Reading the code did not find it. The leading theory for most of a day was a
  cache-line spill from DMA over the PSRAM crypto buffers -- plausible,
  documented as a real hazard, and wrong. `tools/soak.sh` found it in four runs
  by separating two knobs: **209 logins with no commands ran clean for ten
  minutes**, while **three sessions typing commands panicked in 99 seconds**.
  History is pushed per command. Taking the SSH buffers out of PSRAM entirely
  then changed nothing except which heap the corruption landed on, which
  finished the PSRAM theory off in a single run.

  The fix is the whole table under one lock. Confirmed on an uninstrumented
  build with PSRAM restored: the arm that panicked in 99s ran clean for 900s.

- **`esp_linenoise` was handed a garbage pointer under load -- and it was ours.**
  Seen once, on the serial console, during a four-worker run: faulting task
  `main`, `free(0x15)` inside `esp_linenoise_history_free()` from
  `espix_history_apply()`, tripping heap_caps_base.c's "free() target pointer is
  outside heap areas".

  Written up here as a probable defect in the component. It was not.
  `espix_history_apply()` walks the shared list and hands each entry to
  `esp_linenoise_history_add()`, and the entry above is why that list could hold
  a pointer another session had already freed. The editor stored it and freed it
  again later. Kept as an entry because "the crash is inside the library" was
  the wrong first instinct, and the correction is the useful part.

  One genuine gap does remain in the component, worth reporting rather than
  working around: `esp_linenoise_edit()`'s ENTER case does
  `state->history_length--; free(config->history[state->history_length]);` with
  no check that the length is above zero. Nothing espix does reaches it, and
  nothing stops it either.

- **Loading an app can fault the cache — latent since XIP, not fixed.**
  Faulting task `app:testapp`, `exccause 0x47 (CacheError)` in
  `Cache_WriteBack_Items` ← `Cache_WriteBack_All` ← `esp_elf_arch_flush` ←
  `esp_elf_relocate`, seen once in eight parallel test runs. The `elf_loader`
  component writes back the whole cache *outside* the flash lock it takes on the
  very next line, so a concurrent flash operation pulled the cache out from
  under it. Written up in [UPSTREAM.md](UPSTREAM.md).

  `CONFIG_SPIRAM_XIP_FROM_PSRAM` closes the window rather than the bug: a flash
  operation no longer disables the cache, so there is nothing to be pulled out
  from under. Three full parallel runs since without a recurrence, which is
  consistency and not proof — the sample before the change was one occurrence in
  eight runs, so three clean runs would be unsurprising either way.

  The "no longer disables the cache" half is now measured rather than reasoned
  — see the `flash mmap: none live` note in the entry above — so what remains
  unproven here is only whether the window is *fully* closed, not whether it
  narrowed.

  It stays here rather than moving to fixed, because the ordering upstream is
  still wrong and the fault returns the moment XIP is off. The three
  espix-side ways out, none free, remain the same:

  - Turn off `CONFIG_ELF_LOADER_LOAD_PSRAM`. The flush is not called at all
    then — but every loaded app's image moves into internal RAM, and this board
    already spends ~12K of it per open SSH session.
  - Serialise espix's own flash traffic against app loading. Every file
    operation already funnels through `espix_fs_access_check()`, so there is one
    place to put it, at the cost of a lock across all filesystem I/O.
  - Wait for the component, and pin the version when it is fixed.

- ~~**A gone SSH client left its command running, holding the session slot.**~~
  **Fixed.** `chan_poll_interrupt()` is the poll a foreground command runs every
  slice to notice Ctrl-C, and it answered "no interrupt" for a dead peer:

  ```c
  if (ch->closed)              { return hit; }   /* hit is false here */
  ...
  if (chan_pump(ch) != ESP_OK) { return hit; }
  ```

  FIN makes the socket readable, so `select()` fires and `chan_pump()` fails on
  EOF -- and the command is told nothing happened. It carried on writing into a
  socket nobody would read, each write blocking until TCP gave up.

  **Never a `top` bug**, though that is what exposed it. The poll has two
  callers and the other matters more: `cmd_run.c:100`, the foreground-app wait.
  Any app run in the foreground kept running after its client vanished, with the
  connection task waiting on it. `top` is simply what `55-sessions.sh` holds its
  connections with.

  **Present since `bca0fd2` (2026-08-25)**, the commit that introduced Ctrl-C.
  It hid because drain time depends on how many writes the held command has
  left, which varies: the same suite recorded "back to 2 after 4s" one morning
  and over 30s the same evening. Slots now return in **1s**, and `55-sessions`
  went from 50s with two failures to 14s green.

  Found while chasing what looked like a 25K memory regression from
  `MBEDTLS_ECP_FIXED_POINT_OPTIM`. That was wrong: with teardown fixed the same
  measurement reads 45K with the option on and 44K off, and the missing 24K was
  connections not yet gone being counted as live. **A number measured on a
  broken system is not a baseline.**

- ~~**An ordinary account cannot write `/dev/null`.**~~ **Never true of any
  committed build, and the way it got recorded is the part worth keeping.**

  What was seen was real: `scp` to `/dev/null` failed with `Permission denied`
  as `esp` while the same write from root succeeded, and it made
  `45-throughput`'s upload floor meaningless -- every upload failed, both
  failures cost the same handshake, and the rate (a difference between a 512KB
  and a 2MB transfer) was computed from noise. It read
  `scp upload: 307200 KB/s (floor 200)` and passed.

  **The diagnosis was wrong twice, and both mistakes are the same mistake.**

  First: "the node is espix's own, not IDF's -- IDF's null VFS is registered
  nowhere". It is registered, by ESP-IDF itself, under
  `CONFIG_VFS_INITIALIZE_DEV_NULL` (default `y`) in `vfs/nullfs.c`. The grep
  that produced "nowhere" was scoped to espix's tree and could not have found an
  IDF-internal `ESP_SYSTEM_INIT_FN`. **A conclusion drawn from a gap the search
  itself made.** Because IDF's prefix `/dev/null` is longer than espix's `""`,
  it outranked espix's node and bypassed `espix_fs_access_check()` entirely --
  which is why writes worked, for everyone, on every build that had it.

  Second: "other runs read 666, 556 and 922 KB/s ... the same noise". Those were
  real measurements, taken before the condition existed.

  **How the condition existed at all: `sdkconfig` is gitignored and carries
  settings across branches.** A build of the `/dev` feature branch wrote
  `CONFIG_VFS_INITIALIZE_DEV_NULL=n` into it. Checking out `main` and rebuilding
  did not revert that -- `sdkconfig.defaults` only overrides a value still at its
  Kconfig default -- so `main` was being built with the branch's config and
  without the branch's fix. espix owned `/dev/null`, and `mode_from_rule()`
  answered `0644` for a node the table declares `0666`. Exactly the broken
  middle state that branch's commit message predicts.

  So `build/` was not a build of the tree in front of it, which is the same
  class of error the stale-firmware guard exists for -- and it was invisible
  because the guard compares the *board* against `build/`, and both agreed.
  `sdkconfig.defaults` now warns about it.

  **Fixed for real** by the `/dev` work: espix owns the whole subtree, and
  `espix_fs_mode()` answers from the device table, so the check, `stat()` and
  `ls -l` agree on `0666`. `tests/suites/10-fs.sh` asserts an ordinary account
  may write the sink, and `45-throughput`'s upload leg measures again
  (616 KB/s, beside the 666/556/922 above).

  **What survives, and earned its place.** `rate_kbs()` refuses to answer when
  the two transfers finish within 100ms of each other, and the upload leg checks
  the destination accepts a write before timing writes to it. A suite whose
  header says it exists because "a process's stdin came to run at 5 KB/s without
  anyone noticing" had reported 300 MB/s over WiFi and called it a pass.

- **A session occasionally dies under parallel load, and nothing explains it
  yet.** Seen in one full `-j 4` run out of two: `35-signals` lost its SSH
  session partway through and the harness reported seven failures that were one
  event — `session gone before: ps`, then everything downstream comparing
  against the dead-session sentinel. The device was fine throughout: no reboot,
  no core dump, and the very next run was 149 assertions green.

  **It recurred, so the teardown fix was not it.** The entry above — a gone
  client leaving its foreground command running with the slot held — was fixed
  2026-09-08, and this said "if this does not recur, that was it". On
  2026-09-14 a `-j 4` run reproduced the signature exactly: `35-signals`, seven
  failures, `session gone before: ps` first, and the suite green on its own
  re-run seconds later (15 ok). Device healthy throughout — no reboot, no core
  dump, 178 of 185 assertions passing around it.

  So that hypothesis is closed off rather than left hanging. Whatever this is,
  it survives the teardown fix.

  It is not new and it is not the panics. The same shape turned up early in the
  parallel work, before any of the fixes: one login failure in nine rounds of
  "open a session, then open four connections at once", which the login-timeout
  change did **not** explain — a login measures 4.0s alone and 8.7–11.7s with
  five at once, nowhere near the 25s budget it was blamed on.

  What is known: the connection goes away rather than hanging, the device does
  not notice anything, and it is rare enough that a rate needs tens of runs to
  measure. What would settle it is the serial console open with `dmesg -n debug`
  while a parallel run goes, so the device's own account of the disconnect is
  captured — which is how the `Corrupted MAC` bug was eventually caught, and for
  the same reason: never debug a transport through itself.

- **The fault handler intercepts but does not recover.** A crash is recorded and
  reported in `dmesg` on the next boot, and then the system reboots.
  `espix_fault_request_reap()` exists with no callers.

## Filesystem

- **A directory's mode does not hide what is inside it.** Unix requires search
  (`x`) permission on every component of a path; espix checks the final
  component, plus the parent for anything that creates or removes a name. So
  `chmod 700 /home/esp` stops `ls /home/esp`, but a user who already knows the
  full path can still `cat /home/esp/notes`. A full walk costs a stat per
  component on every file operation in the system, and computing a rule-derived
  mode already costs an open and a four-byte read per file. Worth revisiting if
  path resolution ever caches directory modes.

- **Permission checks apply to espix's own tools, not to espix itself.** A task
  that is neither a process nor inside a session -- SNTP, the WiFi driver, an
  SSH connection task before it has authenticated anyone -- is the kernel and is
  not checked. That is what lets boot read `/etc/wifi.conf` before there is
  anybody to be. It also means anything espix runs on such a task is, in effect,
  root; the seam to watch is `espix_fs_priv_begin()`, which deliberately grants
  the same thing to `espix_auth` and to the ELF-magic probe, and which should
  stay at those two callers.

- **`sudo` does not ask for a password.** espix cannot read input without
  echoing it, which is why `passwd` takes the password as an argument, so a
  prompt would print what it was meant to protect. `sudo` therefore authorises
  on `/etc/sudoers` membership alone: anyone who reaches an authenticated
  session of a listed account can become root, including at a terminal its owner
  walked away from. Linux closes that with a timestamp and a re-prompt. First
  thing to revisit when the reentrant line editor lands.

- **The name caches in espix_auth are shared and unlocked.** `ls -l` resolves a
  uid and a gid to names through one-entry caches in file scope, and the console
  and an SSH session can both be inside `ls` at once — the same race
  `cmd_fs.c`'s comparators were written to avoid. The consequence is bounded to
  a wrong or mangled *name* in a listing, since every write is a `strlcpy` into
  a fixed buffer, but it is a race. A mutex would fix it; so would resolving
  into the caller's own buffer and keeping no cache at all, at the cost of
  re-reading the file per directory entry.

- **A reused uid inherits the previous account's files.** `useradd` hands out
  the lowest free id, so deleting an account and making another gives the new
  one the old one's number — and every file still stamped with it. Standard Unix
  behaviour, but likelier here: there are eight account slots and no `find -uid`
  to hunt the leftovers down with. `userdel -r` clears the home directory;
  anything the account owned elsewhere is yours to find.

- **A group change needs a new login.** An identity's groups are resolved once,
  when the session starts, and copied into a process at spawn — the file is the
  authority but not something to re-read on the path of every `open()`. So
  `usermod -aG` does not affect a session that is already open. A real system
  has `newgrp` for that; espix has logging out.

- **Eight groups, twelve entries, eight accounts.** `ESPIX_NGROUPS_MAX` is a
  size as much as a limit, because credentials are copied rather than looked up.
  Membership beyond the eighth group is silently not carried, which is the one
  place these tables fail quietly rather than loudly.

- **The shell drops an empty quoted argument.** `usermod -G "" esp` — the usual
  way to clear somebody's supplementary groups — arrives as `usermod -G esp`,
  so `-G` eats the username and the command prints its usage. Name a group you
  do want instead, or use `userdel`. It is a shell limitation rather than a
  usermod one.

- **`su` does not exist.** `sudo` covers the need, and `su` is the command that
  most wants the password prompt espix cannot yet give.


- **A device VFS is usable but invisible.** `/dev/uart` is registered by
  ESP-IDF's UART driver and is live in this build, and because its prefix is
  longer than espix's `""` it outranks the root and routes straight to the
  driver. Its ops table carries `open`, `read`, `write`, `close`, `fstat`,
  `fcntl` and `fsync`, so those work — but `ls` cannot show it.
  `ls -l /dev/uart/0` fails because the UART VFS's *directory* ops contain only
  `access` — there is no `stat` for `ls` to call, and `readdir` never merges
  mount points, so it does not appear in a listing of `/dev` either.

  **`ls /dev` itself now lists espix's own nodes**, `/dev/null` and
  `/dev/factory`. `/dev` is a real littlefs directory — the boot skeleton makes
  it, and it is what keeps `ls /` showing `dev` — but espix answers the
  directory itself and everything under it from the device table in `dev.c`:
  `vfs_opendir("/dev")` returns a synthetic `DIR` (a small static pool), and
  `vfs_readdir` yields the two nodes. Nothing inside reaches littlefs, so a file
  left in an older image's `/dev` is neither listed nor reachable, and none of
  `mkdir`/`unlink`/`rename`/`truncate`/`utime` under `/dev` will touch it.

  Two consequences worth knowing. **`chmod`/`chown` on a device is refused**
  (`operation not permitted`): a device's mode is declared in the table, not
  stored, and there is no inode to carry a changed one. And a device's mode is
  what the permission check reads — the table's `0666` for `/dev/null`, not the
  rule's `0644` — so a non-root shell can redirect into the sink.

  `/dev/uart` still does not appear, and that is deliberate: it is ESP-IDF's,
  its prefix is longer than espix's so it never reaches this VFS, and it is the
  serial console rather than a general device tree. The listing shows only what
  espix owns.

- **Only the root filesystem gets espix's permission check.** Anything
  registered at its own prefix is routed by ESP-IDF before espix sees it, so
  mounting FAT on an SD card at `/mnt/sd` would leave
  `espix_fs_access_check()` uncalled for every file on it. Harmless for
  devices, which have no mode to check; the problem is a second *filesystem*.
  See [ROADMAP.md](ROADMAP.md#filesystem) for what closing it costs.

- **Unmounting is two steps now, and `esp_vfs_littlefs_unregister()` is not one
  of them.** espix mounts through `esp_littlefs_mount()` and never registers
  LittleFS with the VFS, so the port's unregister has no registration to tear
  down and would fail. Nothing calls it — espix has no `umount` — but whoever
  adds one needs to unregister espix's VFS and unmount LittleFS separately,
  mirroring the two halves that mounting became.

- **`fcntl(F_GETPATH)` is untested.** `CONFIG_LITTLEFS_FCNTL_GET_PATH` is on and
  the port answers by concatenating its `base_path` with the file's path; espix
  sets that to `""`, so the answer should be the same absolute path espix uses.
  Nothing in espix calls it, so that is reasoning rather than observation.

- **Power-loss safety has not been re-tested since espix took the root VFS.** It
  should be unaffected — crash safety lives in `lfs.c`, which espix does not
  touch, and the on-disk format is unchanged — but every file operation now runs
  through new code and the verification did not include pulling power
  mid-write. Treat it as inherited, not as confirmed.

- **`ls -a` cannot show `.` and `..`.** It shows dotfiles, which is what you
  want it for, but the two directory entries themselves never arrive: the ESP
  LittleFS port's `readdir()` reads in a loop until it gets what it calls "a
  real object name", discarding both before espix sees them. So `-a` behaves as
  GNU `ls`'s `-A` and there is no way to make it behave otherwise from here.

- **`ls` holds at most 512 entries.** Sorting means buffering the directory, and
  past that ceiling the listing stops and says `ls: stopped at 512 entries`
  rather than silently ending. The same applies if the allocation fails partway.

- **SFTP silently drops setuid, setgid and sticky** where the shell's `chmod`
  refuses them out loud. SFTP has no partial-success status, so failing the
  request would fail an entire `scp -p` over one bit.

  Note the reason has inverted since this was written. It used to be that the
  bits were never honoured anyway, so dropping them cost nothing; espix now acts
  on setuid, which is precisely why an upload must not be able to set it. The
  silence is the part that remains wrong, not the dropping. Setting the mode of
  a *directory* over SFTP is accepted and ignored for the same reason.

- **A process root is a filesystem boundary, and only that.** `confine <dir>`
  stops a process resolving a path outside `<dir>` — see
  [ROADMAP.md](ROADMAP.md) — but four things sit outside what it covers:

  - **Other VFSes are not routed through it.** `/dev/uart` and sockets register
    longer prefixes, and ESP-IDF matches those before espix's fallback, so espix
    never sees the call. This is what keeps a confined app's stdio working, and
    it is the reason the boundary is a filesystem one rather than a sandbox.
  - **No MMU means it is a guardrail, not a sandbox.** A loaded app shares the
    address space with the kernel and can call what the loader exported or write
    memory directly. Same caveat as setuid, and the same reason to have built
    it: the S31 makes both real.
  - **`getcwd()` returns the true path**, so a confined app can see where its
    root sits in the wider filesystem. That follows from restricting rather than
    chrooting, and it is not a leak of anything it can reach.
  - **Sessions are not confined, only processes.** There is no restricted login
    shell; `-R` applies to a program you run, not to whoever runs it.

- **A *backgrounded* app's output is not redirected.** `app > file &` writes to
  the terminal and leaves `file` empty; `2>` likewise. A foreground app
  redirects correctly, as do builtins, an app's exit status, and espix's own
  diagnostics about it.

  The reason is lifetime, and it is why the two cases differ at all: the
  redirect `FILE` belongs to the shell and `redirects_release()` closes it when
  the command returns. `run_program()` blocks in `espix_proc_wait()` for a
  foreground process, so the `FILE` outlives it; a backgrounded one outlives
  the `FILE`, and pointing its streams at one would be a use-after-free the
  moment somebody typed `&`. See the stream note in `espix_proc/exec.c`, which
  also records the one narrow hazard that remains — an app force-killed inside
  an `fwrite` to a redirect leaves that `FILE`'s lock held.

  Closing it properly needs the redirect to be reference-counted or handed to
  the process outright, which is job-control territory.

  Exit statuses *are* right: 127 when the file cannot be read, 126 when it is
  there and will not run, and the app's own status otherwise.

## Shell and console

- **Kernel log lines land in the middle of what you are typing.** espix writes
  klog straight to the same UART the line editor is drawing on, with nothing
  between them, so a message arriving mid-keystroke splits the echo. Typing
  `whoami` while the network was busy produced

  ```
  root:/# whoam
  espix: sshchan: esp logged out
  iespix: sshd: connection closed
  ```

  — the `i` on the far side of two log lines. The command still runs; it is the
  display, and anything parsing the display, that is wrecked.

  Arguably correct: a Unix console does this too, which is why `dmesg -n`
  exists, and espix has it. It became worth writing down when the test suite
  started running four SSH suites at once, which generates a connection message
  every few seconds and turned an occasional annoyance into the normal case --
  `tests/suites/50-console.sh` now quiets the console for its duration and puts
  the level back.

  Doing better means holding the console's write path while a klog line is
  emitted and redrawing the prompt afterwards, the way Linux does. Worth having;
  not done.

- **Kernel messages land on your prompt.** That is deliberate and matches Linux,
  where kernel output goes to the console and remote users run `dmesg`. Since
  the console-prompt work the line is ended and reissued underneath, so the
  prompt is never left buried — but the messages themselves are not going to
  stop appearing. Do not "fix" this by routing klog through the current session.

## SSH

- **Only a *process* reads stdin, not a builtin.** `ssh host 'testapp cat'
  < file` works: a loaded app gets a real `stdin` over the channel, and
  `fgets`/`fread`/`read` are in its ABI. But no espix builtin reads standard
  input, and there is no `<` redirection, so `ssh host 'cat' < file` still will
  not do what you mean — the builtin `cat` takes paths and nothing else.

  Not much of a gap in practice: with no pipes and no `<`, a builtin has
  nothing to read *from* except the network, which is the case an app already
  covers. It becomes worth doing alongside pipes — see
  [ROADMAP.md](ROADMAP.md).

- **Only a foreground process reads stdin.** `chan_poll_interrupt()` is the
  single consumer of the channel's receive buffer and the thing that fills a
  process's stdin queue, and it runs from the foreground wait loop in
  `cmd_run.c`. So a backgrounded app's `stdin` is open but nothing arrives on
  it — it blocks until the session ends. `chan_pump()` overwrites that buffer
  rather than appending, so a second reader is not a small change: it needs
  the buffer to become a ring first.

- **Two concurrent SFTP transfers break.** Reproducible, and not new: two
  `scp` downloads started at the same time end with one connection failing
  (rc=255, a partial file) and the other hanging until it is killed. `ps` during
  the hang shows an `sshd:conn` task sitting at priority 18 — the tcpip task's
  priority, inherited — so tcpip is waiting on a mutex that connection holds.

  The control matters, because this was first noticed through `/dev/factory`
  and looked like a bug in it: two concurrent downloads of an ordinary file
  (`/bin/neopixel`) fail exactly the same way, and a single 4MB `/dev/factory`
  download succeeds every time. So it is SFTP concurrency, not the device.

  One transfer at a time is the working configuration, which is what the test
  suite does and probably what anyone does by hand.

- **One command per `exec`.** The shell has no `;`, `&&` or pipes, so
  `ssh host 'cd /bin && ls'` fails in the parser rather than in the channel.
  `scp -O` — the pre-9.0 protocol — is answered with a message pointing at
  SFTP, because espix implements no `scp` command for it to run.

- **A client that decides to rekey hangs the session.** espix reads
  `SSH_MSG_KEXINIT` exactly once, during the handshake; one arriving mid-session
  falls through to the channel loop's `default:` case and is ignored. The client
  has by then stopped sending ordinary traffic and is waiting for the server's
  KEXINIT, so the connection stalls and dies. Rarely reached rather than
  harmless: OpenSSH's default is 2^32 blocks, which for `aes256-ctr` is 64 GiB,
  with no time-based limit — but `RekeyLimit 1G 1h` in a client's config gets
  there in an hour. See [ROADMAP.md](ROADMAP.md#ssh).

- **Client algorithm lists are not a stable surface.** OpenSSH 10.3 added
  post-quantum key exchange and pushed its KEXINIT to 1656 bytes, which outgrew
  a fixed 1600-byte buffer, and every connection was refused — with a message
  claiming "no common algorithm", because one string was sent for every
  negotiation failure. Both are fixed. The lesson generalises: a client release
  can break a working server without either side being wrong, and a single
  catch-all error string will misdirect the diagnosis when it does.

- **Only one host key algorithm is offered**, `ecdsa-sha2-nistp256`. It works
  with current OpenSSH. See [ROADMAP.md](ROADMAP.md#ssh) for why that is worth
  not leaving alone.

## Networking and time

- **DHCP option 42 is implemented but has never been exercised.**
  `CONFIG_LWIP_DHCP_GET_NTP_SRV` is on and SNTP is configured to take a server
  from DHCP when `/etc/wifi.conf` does not name one, but the network it has been
  tested on offers no NTP server — so only the `pool.ntp.org` fallback has ever
  run. Treat the DHCP path as untested code.

- **The clock reads 1970 until NTP answers**, for about 6.5 seconds on a cold
  boot with a working network, and indefinitely without one. A soft `reboot`
  keeps the time. See [ROADMAP.md](ROADMAP.md#networking-and-time) for what
  falls in that window and what a fix would cost.
