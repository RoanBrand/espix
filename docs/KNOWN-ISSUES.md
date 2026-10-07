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

- **`exit()` from a thread the app created ends the thread, not the process.**
  POSIX says `exit()` in any thread terminates the whole process; espix ends the
  calling thread and logs a warning. The reason is mechanical rather than a
  choice: a process's exit path is a `longjmp` back to `proc_task()`, and
  jumping there from another task's stack would land in a frame that task does
  not own -- undefined behaviour, and it would corrupt the app's own stack.
  An app that wants its process to end should call `exit()` from the task that
  entered `app_main()`, which is where a normal `return` goes anyway.

  `atexit()` is still unpublished, so no handler runs on the way out either.

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
  There is no address space to tear down, and although a process's heap and
  files are given back now, the peripherals its own `teardown()` would release
  — an RMT channel, a GPIO reservation — are not something espix can reclaim
  for it. `tests/suites/35-signals.sh` pins both halves.

- **`ps` shows only the last eight exits.** The finished list is a fixed
  eight-entry ring (`ESPIX_PROC_DONE_MAX`), separate from the 12-slot live
  table, so a ninth exit pushes the oldest out of the `finished:` list. A ring
  because it is bookkeeping, and a spawn must never fail for it.

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

  A third mechanism of the same defect is recorded here rather than in an entry
  of its own, because this is the entry that would explain it: a task deleted
  while it sits inside lwIP's send() loses the completion the connection is
  waiting on (conn->op_completed), and that socket is then stuck. Nothing
  structurally prevents it -- write_all() and chan_write() check
  espix_proc_stopping() so an ssh write leaves when it is asked to, which is a
  courtesy and not a guarantee. It has not been seen live.

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

- **About 28 net heap blocks per run in the two file-moving suites are still
  unexplained.** The sftp session itself measures clean now, downloads and
  uploads included; this is the residue `40-transfer` and `45-throughput`
  still leave, down from +45 blocks before. Both suites also write files and
  run `sftp -b`, which is where to look next.

- **A force-killed process leaks its `funopen` streams.** They are the
  victim's stdout and stderr, `funopen()` objects over the SSH channel, and a
  hard kill detaches them rather than closing them, because closing them is
  the very call that blocks. Measured at ~2.5K per kill over 20. It is
  bounded, and only a hard kill pays it; a process that exits closes them.

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

  **Where that reach comes from, and what changed since.** The map quoted above
  is the S3 build, and on Xtensa `s_table_count` really is the word after the
  table — but that is a linker accident rather than a guarantee, and it stopped
  being true on the S31: there the counter lands in `.sbss`, nowhere near the
  table, so the watchpoint aimed at it was aimed at nothing. The word past the
  table now belongs to the table's own struct (`espix_proc_table_t.guard`), so
  the adjacency is the language's promise rather than the linker's whim and both
  targets watch the same address. The guard is also checked in software at every
  spawn, which is what covers a build with `ESPIX_PROC_ABI_WATCHPOINT` off.

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
  the box, so it gets there first.

  Reading the run that found it: 51 assertions failed, all but three of them
  `<<<dead-session>>>`. That is **one** event, not 51 — the reboot killed four
  worker sessions at the same instant. `reset-reason-changed('software'->'')`
  and `coredump-unanswered` in the same report are empty answers from a device
  that was still coming up, not findings.

  What is ruled out, and it is the tempting one: this is *not* the PSRAM/DMA
  cache-line spill. That theory was tested and disproved — taking the SSH
  buffers out of PSRAM entirely only changed which heap the corruption landed
  on.

  Hunting it since with `CONFIG_HEAP_POISONING_COMPREHENSIVE`: four full runs,
  no catch. Note when you try: the poisoned build's per-allocation canaries cost
  enough internal RAM that eight concurrent sessions drove `min free internal
  since boot` to **0 K**, which fails assertions in ways that read as unrelated
  bugs (`ESP_ERR_NO_MEM` spawning an app, a session dying with status 255).
  `tests/run.sh` now prints that low-water mark every run so it cannot be missed
  a second time.

  **Re-measured on the S3 and narrowed (2026-10-07).** It is a wild store, and
  it needs neither a force-kill nor unusual load:

  | | |
  | --- | --- |
  | reproducer | `tests/run.sh -j 2 --seed 36501` -- two concurrent suites |
  | time to abort | ~120s, repeatable |
  | serial (`-j 1`) | 20 suites, 374 assertions, no abort |
  | `80-svc` alone | passes (8/8, 32s) |

  Force-killing is **not** the mechanism: with both `kill -9` calls removed from
  15-streams the run still died, so "the kill tests are in every aborting run"
  was correlation and not cause.

  The panic site is whichever task is hot, and it is a victim rather than a
  writer. It has been, across runs: a ROM `mem*` called from an app task
  (`StoreProhibited` with `0xffffffff` operands), lwIP's `netif` inside
  `ip4_addr_isbroadcast_u32`, littlefs's `crc` pointer inside `lfs_bd_crc`
  (read as `4`), mbedTLS bignum during the SSH key exchange, and FreeRTOS's own
  `uxMutexesHeld` and `lock->count`. Every one is a pointer or a count that
  must be valid and is garbage.

  Ruled out since, each by measurement on a clean kernel:

  - **Stack exhaustion.** A per-task stack watchpoint, correctly armed, never
    fired -- and arming it correctly required disabling
    `CONFIG_ESPIX_PROC_ABI_WATCHPOINT` first, because espix uses both hardware
    watchpoint registers and IDF's stack watchpoint is overwritten on every task
    switch, which silently invalidated a first attempt. Sampling `ps`'s STACK
    column through a reproducing run leaves the tightest espix task ~1 KB.
  - **A process killed holding the arena lock.** `s_region_lock` is a plain
    mutex with no orphan treatment, so this was worth testing; an instrument in
    `espix_proc_regions_release()` never reported a foreign holder.
  - **The victim's stream teardown re-entering the channel.** Already fixed: the
    kill path prints `[stdio detached]` and the teardown does not reach it.
  - **PSRAM task stacks with the cache disabled.** Routine flash I/O on the S3
    never calls `spi_flash_disable_interrupts_caches_and_other_cpu()`, and
    relocation already runs on an internal-stacked task for this reason. IDF's
    guard never fired.
  - **`free()` of a pointer outside an arena.** The hits in the captures were
    internal-RAM pointers, which is the handled case.

  Why it is still open: a hardware watchpoint needs a *stable* victim and the
  victims move every run, so there is nothing to aim one at. Do not hunt it with
  `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` while the ABI watchpoint is on:
  they contend for the same registers and the resulting debug exception panics
  with no JTAG attached. That is an instrument artefact, not the bug, and it
  cost a run to learn.

- **A killed app's threads are not reaped, and the arena lock is not orphaned.**
  Two holes found while hunting the above, each real on its own.

  `abi_pthread.c` publishes `pthread_create` so a thread's allocations land in
  its process's arena, but nothing walks the threads when the process is
  force-killed: only `slot->info.task` is deleted. A surviving thread keeps
  running with thread-local storage still naming the slot, allocating from
  regions that were just released -- and from a slot that may already belong to
  another process.

  `s_region_lock` is taken with `portMAX_DELAY` on every allocation and on the
  release path itself, and unlike `tx_lock` and `rx_lock` nothing orphans it,
  so a process killed inside an allocation leaves it recorded against a freed
  TCB and the next taker walks that memory for priority inheritance. Neither is
  the writer being hunted above (the instrument for the second one never fired),
  and both are the same class of bug the ssh locks were fixed for.

- **`esp_linenoise_edit()` can free history that is not there.** Its ENTER
  case does `state->history_length--; free(config->history[state->history_length]);`
  with no check that the length is above zero, so an empty history underflows.
  Nothing espix does reaches it, and nothing stops it either; it is the
  component's, not espix's use of it.

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

- **A session occasionally dies under parallel load, and nothing explains it
  yet.** Seen in one full `-j 4` run out of two: `35-signals` lost its SSH
  session partway through and the harness reported seven failures that were one
  event — `session gone before: ps`, then everything downstream comparing
  against the dead-session sentinel. The device was fine throughout: no reboot,
  no core dump, and the very next run was 149 assertions green.

  **It recurred on 2026-09-14.** A `-j 4` run reproduced the signature
  exactly: `35-signals`, seven failures, `session gone before: ps` first, and
  the suite green on its own re-run seconds later (15 ok). Device healthy
  throughout — no reboot, no core dump, 178 of 185 assertions passing around
  it.

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

- **An ext listing cannot tell its end from a failure.** lwext4's
  `ext4_dir_entry_next()` returns `NULL` for both — `ext4.c:3180`, where the same
  `return de` covers `next_off == EXT4_DIR_ENTRY_OFFSET_TERM` and every failing
  `goto Finish` — and there is no error to read afterwards. So `ls` on a
  directory whose listing fails part-way prints a short listing rather than an
  error, and a truncated directory looks exactly like a small one.

  **Measured, 2026-09-19.** On a SanDisk Cruzer whose ext4 volume `e2fsck`s clean
  and whose `n250` directory a Linux host lists as **300** entries (E001–E300),
  espix lists **226**, consistently, with the missing E-numbers scattered through
  the range (001, 007, 010, … 299, 300) — the directory's on-disk order is not
  name order, so an early stop drops an arbitrary subset rather than a tail. The
  FAT32 and exFAT volumes of the same stick, with the same directory shapes, list
  all 300. So this is not the drive and not the USB stack; it is lwext4's
  directory traversal. Cross-referencing a listing against a known-good host is
  the only way to see it today.

  FAT does not have this: FatFs's `f_readdir` returns a `FRESULT`, and the FatFs
  readdir diagnostic reports a truncated listing rather than hiding it. The fix
  belongs in the vendored lwext4 rather than in `ext.c` — that function needs an
  out-parameter or an `errno` — and a patch there is one to re-verify against
  `e2fsck`-checked images, so it is not done yet. `ext.c` records the same thing
  where the call is made.

- **An ext mount whose device is pulled leaves lwext4's mount point behind.**
  The FAT pull is handled with a dead-mount sentinel; this one leaves the mount
  point behind, and the loss is bigger, because everything that would give
  lwext4's side back touches the device:
  `ext4_umount()` writes the superblock back and flushes the block cache, and
  `lwext4_port_bdl_destroy()` syncs the lower BDL. On a pulled device both would
  run through a block device `espix_usb` has already released, so both are
  skipped and only espix's own slot is returned.

  `CONFIG_EXT4_MOUNTPOINTS_COUNT` and `CONFIG_EXT4_BLOCKDEVS_COUNT` are both 2,
  so a pull with a file open costs one of two ext mounts, plus the adapter's
  buffer, until the board next boots. FatFs loses a volume slot the same way and
  has more of them to spare. Closing it means each of those two calls needing a
  variant that frees without flushing — which is a change to the vendored port,
  and would still leave the superblock un-written-back at the point where it
  cannot be written back anyway.

- **A uid above 65535 on an ext volume is truncated to its low 16 bits.**
  espix's uid model is 16 bits throughout (`espix_fs_posix_attr_t`) where an
  ext inode carries 32. Nothing here has an account up there and a removable
  volume is unlikely to, but it is a truncation rather than a refusal.

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

- **One USB storage device at a time, and a hub spends channels before you get
  there.** The S3's USB core has a fixed pool of host-controller channels: the
  root port takes one, an open hub two more (its control pipe plus an interrupt
  endpoint), and a bulk-only storage device three (control, bulk IN, bulk OUT).
  A second storage device therefore finds nothing left and is refused with
  `ESP_ERR_NOT_SUPPORTED`, while `lsusb` shows it enumerated, addressed and with a
  perfectly good interface. **The first device to enumerate wins** — which reads
  as a flaky port unless you know, and it means the second drive appears only
  after the first is unplugged (the sweep claims it within a few seconds). A
  keyboard costs far less than a disk, so "one disk at a time" is the practical
  rule rather than "one device". See [USB-HOST.md](USB-HOST.md#hubs-and-more-than-one-device).

- **A drive larger than 2 TiB reports as 2 TiB.** Not an error, no warning: the
  MSC layer reads capacity with SCSI `READ CAPACITY(10)`, whose block count is 32
  bits, so a 4 TB disk prints `2199023255040` bytes (`2³² × 512`) — a plausible
  number that is wrong by half. `READ CAPACITY(16)` would fix it and the class
  driver does not use it.

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

- **A backgrounded app's output cannot be redirected.** `app > file &` is
  refused (`redirection with & is not supported yet`), and `2>` likewise; a
  foreground app redirects correctly, as do builtins.

  The reason is lifetime: the redirect `FILE` belongs to the shell and
  `redirects_release()` closes it when the command returns. `run_program()`
  blocks in `espix_proc_wait()` for a foreground process, so the `FILE`
  outlives it; a backgrounded one outlives the `FILE`, and pointing its
  streams at one would be a use-after-free the moment somebody typed `&`.
  Closing it properly needs the redirect to be reference-counted or handed to
  the process outright. See the stream note in `espix_proc/exec.c`, which also
  records the one narrow hazard that remains — an app force-killed inside an
  `fwrite` to a redirect leaves that `FILE`'s lock held.

- **An NTFS volume's label is not read.** exFAT's is now, by following the
  boot sector's geometry to its root directory; NTFS keeps its label in the
  `$Volume` metadata file rather than in sector 0, so `lsblk` leaves that
  column empty where Linux fills it.

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

- **An IDF component's line arrives as two ring entries, and the second one's
  level is guessed wrong.** espix captures `ESP_LOGx` output by installing a
  `esp_log_set_vprintf()` hook (`klog.c`), because most of what espix cannot see
  is logged by IDF rather than by espix — a FatFs `FRESULT`, a USB transfer's
  failure. IDF's formatter reaches that hook more than once per message, so one
  `ESP_LOGW` becomes two lines in `dmesg`: the header, and then the text.

      [     2.400] W W (2400) wifi:
      [     2.400] I Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2

  The first line is espix's own level letter followed by IDF's, which reads as a
  duplicated `W W (2400) wifi:`; the second has no prefix at all, so
  `level_from_esp_log()` falls back to the first character of the text and lands
  on `I` — the message above is a *warning*. Neither is repaired: joining the two
  needs the hook to hold the header across two calls, and it is called from every
  task, so that is shared state on the logging path.

  Cosmetic, and it does not lose the message — which is the point of capturing
  IDF at all. Worth knowing before reading a level off an IDF line: trust the
  header's letter, not the body's. The same test that reads IDF lines should
  read them through `log <tag> debug`, which is what raises a tag's level at
  runtime.

## Writing to an ext volume

- **Writes on an ext4 volume go through the port's experimental extent
  implementation.** An ext4 volume's files are extent-mapped, so allocating a
  block means mutating an extent tree, and the implementation espix compiles is the
  port's own (`components/esp_lwext4/port/lwext4_extent.c`; its
  `doc/CAVEATS.md` is the whole story). What that means in practice, and all of it
  is deliberate rather than discovered: a writable mount **needs a journal**, and a
  volume without one is refused rather than quietly mounted read-only — mutating a
  tree changes several metadata blocks at once and lwext4 will not do it without a
  transaction; an **unwritten extent** returns `ENOTSUP` when written into, because
  the port does not create or convert them (`fallocate()` and some copy tools make
  them, so a file prepared that way on Linux is readable and not writable in that
  region); and every write costs a **transaction**, so writes are slow in a way
  that is lwext4's design rather than the port's overhead.

  Read-only remains the default. A volume is somebody's own data far more often
  than a stick is, which is why this is opt-in per mount (`mount -o rw`, or `rw` in
  `/etc/fstab`) and why the mount says so in `dmesg` when the extent tree is in
  play.

- **Pulling a writable ext volume costs whatever is in its journal.** Unmounting
  is not the formality here that it nearly is for FAT: a writable mount holds an
  open journal transaction, and the volume is only put right when the next mount
  replays it. Pull the device while it is writable and the replay happens next time
  instead -- fine until it is not, and then not obviously fine at all.

  Observed, in exactly that order. A file was created and read back on a writable
  mount (15 bytes, mode 0644); the board was then power-cycled with the volume still
  mounted, because the cable was swapped rather than the volume unmounted; and the
  next mount ran with a fault injected into its block device, so the replay began
  and failed partway. The file came back with its directory entry intact and its
  inode reading size 0, mode 000 -- the entry had been written, the inode's own
  fields had not, because they were still inside the transaction that never
  finished. The volume's pre-existing files were untouched, and a mount that can
  replay normally does.

  So `umount` before pulling. This is also, accidentally, the evidence the port's
  `doc/CAVEATS.md` asks for -- that a failed operation on this path is not safely
  rolled back -- produced by an experiment meant to test something else.

## SSH

- **SFTP transfers cannot reach past 4 GiB, and say so.** The read and write
  handlers seek with `fseek()`, which takes a `long`; `off_t` is 64 bits now, so
  the client's 64-bit offset is truncated on the way in. The 64-bit sibling
  `fseeko()` is not a way out either, because it lives in the **prebuilt** libc,
  which was compiled when `off_t` was 32 bits — calling it with the widened type
  would hand it half a value. And seeking the descriptor under the `FILE` is worse
  than either: stdio's buffer would still hold the old position, so the next
  `fread` would return the wrong bytes *without* an error.

  So an offset whose high word is non-zero is refused with
  `offset beyond 4 GiB (transfer path is 32-bit)` rather than served from a
  truncated one. That is what used to happen, silently, and it would be a worse
  bug now that everything else reports the real size.

  The fix is to move those two handlers off `FILE *` and onto the descriptors,
  where espix's own 64-bit `lseek`/`pread`/`pwrite` apply — `sftp.c` keeps a
  `FILE *` per handle, so it is the handle and the streaming loop that change,
  not the protocol. Everything that does not transfer works today over SFTP: a
  file over 4 GiB reports its real size in `stat` and in the long listing, because
  those read the same widened `struct stat`.

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

- **A client that decides to rekey hangs the session.** espix reads
  `SSH_MSG_KEXINIT` exactly once, during the handshake; one arriving mid-session
  falls through to the channel loop's `default:` case and is ignored. The client
  has by then stopped sending ordinary traffic and is waiting for the server's
  KEXINIT, so the connection stalls and dies. Rarely reached rather than
  harmless: OpenSSH's default is 2^32 blocks, which for `aes256-ctr` is 64 GiB,
  with no time-based limit — but `RekeyLimit 1G 1h` in a client's config gets
  there in an hour. See [ROADMAP.md](ROADMAP.md#ssh).

- **Only one host key algorithm is offered**, `ecdsa-sha2-nistp256`. It works
  with current OpenSSH. See [ROADMAP.md](ROADMAP.md#ssh) for why that is worth
  not leaving alone.

## Networking and time

- **Ethernet can be up and still pass no traffic after a hard reset.** After an
  `esptool` reset over the USB-Serial/JTAG (`--after hard-reset`, and the
  coredump read does one), the board boots normally and the console prints
  `eth: eth0: link up, 1000 Mbps full duplex` and `eth0: 192.168.110.203/24`
  -- but it never answers ARP, so SSH and ping time out until it is power-cycled.
  A soft `reboot` and an OTA upgrade both come back fine, so it is the hard
  reset specifically, not the reboot: the EMAC/PHY is left in a state the boot
  path does not clear. It cost this investigation several power cycles before it
  was pinned down.

- **A default build has no `usb0`.** USB host and USB-NCM are two uses of the one
  OTG peripheral, and the host role is the default — so a board flashed with the
  standard image has lost the cable-reachable interface a previous image had.
  `ip link` does not list `usb0`, and `usb status` says `usb-ncm was not built
  into this image (CONFIG_ESPIX_USB_NCM_ENABLED)`, because the option's
  dependency on the device role leaves it out of the build entirely rather than
  setting it to `n`. The way back is `ESPIX_USB_ROLE_DEVICE`; the other half of
  the trade, a hub blocking the UART socket, is in
  [USB-HOST.md](USB-HOST.md).

- **DHCP option 42 is implemented but has never been exercised.**
  `CONFIG_LWIP_DHCP_GET_NTP_SRV` is on and SNTP is configured to take a server
  from DHCP when `/etc/wifi.conf` does not name one, but the network it has been
  tested on offers no NTP server — so only the `pool.ntp.org` fallback has ever
  run. Treat the DHCP path as untested code.

- **The clock reads 1970 until NTP answers**, for about 6.5 seconds on a cold
  boot with a working network, and indefinitely without one. A soft `reboot`
  keeps the time. See [ROADMAP.md](ROADMAP.md#networking-and-time) for what
  falls in that window and what a fix would cost.

## USB host

- **Pulling a stick during a write can corrupt the heap.** Everything else about
  a pull is handled, and measured: the mount is marked dead, reads answer
  `ENOSYS`, `df` declines the row, an idle pull auto-unmounts, and `umount` works
  afterwards. But a transfer already in flight when the device goes cannot be
  recalled, and it completes against a block device `espix_usb` has released --
  seen corrupting the heap and taking the board down from an unrelated task. It
  needs the USB layer to quiesce before the release, which is not something espix
  can reach, so it is written up in [UPSTREAM.md](UPSTREAM.md#a-device-pulled-mid-transfer-takes-the-heap-with-it)
  rather than fixed here.

- **Removing a device can panic inside the library's hub driver.** With an
  external hub on the port, unplugging a device — or the hub — can reach an
  assert at `ext_hub.c:508` in `device_release()`: the driver keeps a
  `waiting_release` flag per hub device, sets it in three places and clears it in
  one, and a second release of the same device arrives with the flag already
  clear. The fault is in the library's *own* event loop, so espix's only frame in
  the stack is the `usb_host_lib_handle_events()` call that drives it, and the
  board goes down: the recovery is the reboot that follows. Nothing is corrupted,
  and no volume should be involved — unmount before pulling anything, as
  everywhere else.

  It is rare: seen once in a day of plugging and unplugging, on
  `espressif/usb` 1.5.0. It is also the easiest panic here to diagnose, because
  the assert names its own file and line: `dmesg` on the next boot prints it, and
  `coredump` keeps it — in full, after the kernel log has rolled — with no serial
  port involved. `idf.py coredump-info`, via `make coredump`, is for the backtrace
  rather than for the reason. [UPSTREAM.md](UPSTREAM.md) carries the report.

- **A read on the 3.1TB T9 returns different bytes when it is repeated.** This is
  measured, not inferred, and it is the root of everything the listing symptoms
  looked like.

  A probe read the first 200 single-sector accesses after each mount a second
  time and compared the two buffers. It was removed once it had answered -- 512
  bytes of `.bss` and a doubled read for the first accesses of every mount is a
  lot to carry for a question that is settled -- so this is a measurement that
  was made, not one that can be repeated by running something today. On a failing mount, six of them disagreed --
  the same address, read twice, giving different bytes. **No read failed:** the
  block device reported success every time, which is why `diskio_bdl.c`'s own
  "read failed" log stayed empty and why this looked like a filesystem fault for
  so long. FatFs is what notices, via exFAT's entry-set checksum (`ff.c:2190`),
  and reports `FR_INT_ERR` (`fresult=2`, measured) -- which IDF maps to `EIO`,
  the same errno as a genuine `FR_DISK_ERR`, so the surface cannot tell "a read
  failed" from "a read lied".

  Where it happens: at the **start of the partition**, in the first accesses after
  a mount. The differing addresses clustered in the partition's first ~22 KB
  (16,777,728 through 16,799,744 -- the partition begins at 16,777,216), and the
  same probe caught one read at **426 GiB**, an address a directory listing has no
  business touching. That one is most likely a *consequence*: FatFs followed a
  cluster number it had read wrongly.

  So the shape is: a cold read comes back wrong, FatFs caches it and builds on
  it, and the damage surfaces later as a short listing or a parse error. Which
  also explains why a broken listing does **not** always show a mismatch -- if the
  two reads agree on the same wrong bytes, or the bad read was a multi-sector one
  the probe skips, the corruption is real and undetected by it.

  Rate, mounted read-only: 0 in 115 listings of the root when warm, 0 in 60 on a
  read-write mount, 8 in 25 when the root was listed for the first time after
  mounting, 2 in 6 on `Games/` warm.

  **Nothing is lost.** Every entry that went missing reappeared on a re-list; the
  bytes are on the medium and the read is simply not reliable the first time.
  `ls` reports it ([UPSTREAM.md](UPSTREAM.md#a-failed-readdir-and-the-end-of-a-directory-are-the-same-null)),
  so a short listing is visible rather than silent.

  **Two corrections, in order.** This was first written up as "below espix" on
  the strength of "nothing in espix's stack fails" -- which the paragraph above
  disproves, because the whole point is that no read *fails*. So it was blamed on
  the ESP32-S3 cache instead: the USB host invalidates the cache over the transfer
  buffer when a read completes, that call requires address *and* size alignment,
  and the caller's buffer was assumed to be the transfer buffer. Every caller of a
  block-device read was therefore treated as a DMA target, and FatFs's window,
  espix's probes and a user's own `read()` buffer were aligned and bounced to suit.

  The second correction is that the assumption was wrong. The USB host's cache
  layer is **not compiled at all on this chip**: `hcd_dwc.c` gates it on
  `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`, which is defined for the P4 and not the
  S3, so no `esp_cache_msync()` executes -- and on an internal S3 address one
  would return `ESP_ERR_NOT_SUPPORTED` regardless. The transfer buffer is not the
  caller's either: the BOT class driver DMAs into its own URB and `memcpy()`s to
  the caller, so a block-device destination is a CPU copy. `MALLOC_CAP_DMA |
  MALLOC_CAP_INTERNAL` is what keeps that URB reachable by the USB-DWC engine, and
  `MALLOC_CAP_CACHE_ALIGNED` is inert for internal S3 RAM. See
  [GOTCHAS.md](GOTCHAS.md), which had this backwards. The alignment work could not
  have changed anything, which is exactly what the identical counts before and
  after said.

  So the signature the docs had measured was the answer all along: the read is
  unreliable, and a cache returns the *same* wrong bytes every time where this
  does not. The GPT array makes it precise: reading one sector five times in a
  row, one read in roughly sixteen is wrong while the other four agree, and the
  bad value is fixed per LBA across reboots. An *immediate* reread of a bad read
  usually comes back correct -- which lets the GPT allow "two consecutive reads
  agree" repair it -- but not always, and an exFAT directory scan whose reads
  happen to agree on the bad bytes for the moment is not repaired by it. The
  device is a Samsung PSSD T9 behind a hub. A low-power SanDisk Cruzer lists the
  same directory shapes **completely** on FAT and exFAT from this same board and
  stack, which is what points at the T9 (or its power) rather than espix's exFAT
  code; the T9 direct on the OTG port (no hub) has not been tried.

  Fixed, and verified on the drive:

  - `gpt_read_sector()` reads a GPT sector, reads it again, and on disagreement
    re-reads until two consecutive reads agree, replacing the buffer with the
    agreed bytes; a run that never agrees fails the read. With the header CRC and
    the bounded entry LBAs beside it, `lsblk` no longer reports `entries not
    shown` on the T9.
  - A short transfer no longer skips the CSW. `bot_execute_command()` consumes it
    and the block layer re-issues the whole SCSI command, so one short read does
    not desync every later command (`tools/patch-msc.py`).
  - The optional block-device probe (`CONFIG_ESPIX_USB_VERIFY_READS`) now
    **fails** a read that never agrees rather than majority-voting it; a majority
    is exactly what a device returning the same wrong bytes every time would win.

  Parked -- with the leading idea for a future session. The exFAT directory scan:
  on the T9 a ~451-entry directory stops at a different entry each time (measured
  at 8, 13 and 38), because the wrong bytes vary over time while the block-device
  verify's reread lands inside the same brief wrong window. The fix to try is a
  **checksum-aware retry at the directory layer**: on `FR_INT_ERR` (exFAT's
  entry-set checksum, surfaced as `EBADMSG`), close and re-open the directory and
  scan again. It runs only after a read has already failed, so a healthy drive
  pays nothing. The plumbing is the work -- FatFs caches the bad sector in its
  window, so the retry has to restart the scan rather than re-call `readdir()` at
  the same position, and it wants to sit where the FRESULT is still visible.

  `lsblk`'s "entries not shown" was reporting this accurately: entries that are
  really unused came back as nonsense and the entry-array checksum refused the
  table. **Mounting this drive read-only** remains good advice for other reasons,
  but it was never the fix for this.

## Display and desktop

- **On a freshly created 320x240 desktop the icons cannot be clicked.** The
  terminal window is 76 columns wide, which is 616 pixels, and
  `espix_window_new()` keeps a window in the work area by moving it — `x = ww - w`,
  then `x = 0` when that comes out negative. On a 320-pixel canvas the terminal
  therefore lands at x = 0 and covers the whole launcher column, so the icons are
  behind it and unpressable until it is minimised from the taskbar. A desktop
  created at 640x480 has the terminal at x = 120, beside them — and one that is
  *resized* down keeps that position, because `desktop_resized()` only pulls a
  window back when it has left the work area entirely. So the same board behaves
  differently depending on the resolution the desktop happened to start at, which
  is why it is easy to miss. Nothing in the app launch depends on it, but
  "launch it from the desktop icon" does.

- **The game's mouse is an adapter, and two of its limits are deliberate.**
  doomgeneric has no mouse hook at all -- `DG_GetKey` is its only input entry,
  and its SDL mouse case is commented out -- so the app builds the engine's own
  `ev_mouse` and posts it with `D_PostEvent()`. Two consequences: vertical
  movement is *ignored* (the engine's only use for it is `forward += mousey`, so
  a pointer drifting as you sweep sideways would walk you, and its `novert`
  option is declared and never consulted), and turning with a *viewer's* pointer
  stops at the canvas edge, because that pointer is absolute -- a local USB mouse
  sends deltas and has no such limit. The magnitude is one constant,
  `MOUSE_GAIN` in `apps/doom/components/doomplatform/doom_espix.c`.

  The wrinkle worth knowing for anything else that grows a mouse: a `POINTER`
  event is a *place*, not a movement, unless it continues a run of them. An
  absolute device sends one per motion, so the difference between two is the
  movement; a relative device sends `MOTION` for the movement and a `POINTER`
  only when its buttons change, carrying wherever the pointer got to. Differencing
  across that reported the whole distance since the last click as one delta, and
  a click after a turn snapped the view to a new direction.

- **Nothing releases the screen when a process exits.** `espix_display_release()`
  is reached only through `espix_gfx_close()`, and an app that exits without
  calling it -- the game does, deliberately: its quit path `longjmp`s out of the
  engine's teardown -- leaves the display's owner pointing at a screen record
  whose process no longer exists. It survives because the next owner *takes* the
  screen rather than waiting to be given it, which is how the desktop comes back
  after a game, and because the canvas size an app asked for is put back by
  whoever claims next. What it costs is the console: an app run from a shell
  never gives the screen back to it, so the canvas stays at the app's size with
  nobody owning it until something claims. `espix_gfx.h` and apps/README used to
  say the process teardown released the screen, which is not true and is now
  corrected there.

## Building espix

- **Building espix changes one line of your toolchain's headers.** `off_t` and
  `_off_t` are the same type, so widening `off_t` — which is what lets a file over
  4 GiB be reported and seeked, see [UPSTREAM.md](UPSTREAM.md) — necessarily widens
  `_off_t`, and the toolchain's own `reent.h` declares `_lseek_r` in terms of it.
  `tools/patch-libc-offt.py` therefore pins that declaration to its 32-bit width on
  every configure, in the *toolchain installation* rather than in the IDF tree: the
  one patch here that touches something espix does not own.

  It matters because that toolchain is shared. Another project built with it on the
  same machine gets the same declaration — `int` where it was `long`, the same width
  and an identical ABI, but a different declared type. That is deliberate rather than
  accidental, and [tools/README.md](../tools/README.md) says why, what the cost is,
  and how to undo it if you would rather not.

## Updates

- **`/dev/factory` does not exist on the 16MB table.** That layout has no
  `factory` partition, so the node is gone and `/dev/ota0` and `/dev/ota1` take its
  place -- the kernel slot and the loader. Anything reading `/dev/factory` to pull
  the running image, `45-throughput.sh` included, has to take whichever node the
  image actually exposes. Which slot is *running* is `upgrade --slots`, not the
  directory listing.

- **A command with its own task cannot read stdin.** `run_on_own_task()` spawns
  the command and waits on it, and the connection task is the only reader of the
  SSH wire -- so nothing drains the channel, a read blocks forever, and the
  session ends dead with otadata mid-write. Foreground *apps* read stdin fine
  because the connection task pumps while they run. This is why `upgrade
  --stdin` does not exist and `make flash-ota` copies the image to `/tmp` and
  queues it with `--file`.

- **The update cache is root's to write.** The background check records what it
  found in `/var/lib/espix/update`, and `/var` is root-owned, so `upgrade --check`
  run by an ordinary account reports the answer without updating the cache --
  `sudo upgrade --check` updates it. The greeting reads that cache and never the
  network, so it only ever mentions an update a check has already recorded.

- **A same-version rebuild counts as an update.** Newer semver, or the same
  semver with a different build id, is offered; an older one never is. That is
  what a rolling pre-1.0 project wants and would be surprising for a frozen one
  -- see the note in [OTA.md](OTA.md).


## A crash's dump is overwritten by the next one

ESP-IDF erases the sectors a new dump needs before writing it, and there is a
single `coredump` partition, so a second crash destroys the evidence for the
first. `CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is unset, which is that
behaviour; turning it on keeps the first dump until `coredump erase`, at the
cost of erasing the partition before the next dump can be written.

Anything that maps the MMU while reading a dump — `coredump`, `upgrade` — must
run on an internal stack, which is what `espix_cmd_t.internal_stack` is for.

