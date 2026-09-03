# espix roadmap

Work espix might take on, and what each would cost. Nothing here is a promise;
the point is that the reasoning survives, so a decision to build something is
not made from scratch every time.

Two neighbouring documents: [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for behaviour
that is already implemented and will still surprise you, and
[UPSTREAM.md](UPSTREAM.md) for defects that belong to ESP-IDF and its
components rather than to espix. [ARCHITECTURE.md](ARCHITECTURE.md) covers why
things are as they are.

## Processes

- **Job control.** `jobs`, `fg`, `bg`, Ctrl-Z. SIGSTOP and SIGCONT landed with
  signals, which is the hard half — a stopped process parks itself at a delivery
  point and `ps` reports `T`. What is missing is the shell side: a job table, and
  a session that knows which job is in the foreground. `session->fg_pid` already
  exists and is written on every foreground run; job control would give it its
  first reader.
- **Reap a faulted task and keep running.** `espix_fault_request_reap()` is
  defined and has no callers, and `CONFIG_ESPIX_FAULT_REAP` is off — the reaper
  task and its queue exist, but nothing feeds them. Skipping the reboot is the
  easy half. The hard half is the comment block at the top of `reaper.c`: locks
  held by the dead task, no per-process ownership of heap and fds, and the fact
  that without an MMU most corruption never reaches the fault handler at all.
  Shipping the easy half alone produces a system that limps rather than one that
  recovers.
- **Per-app heap arenas.** The allocation path in `espix_proc` is the seam.
  Would shrink the blast radius of a crashing app without needing an MMU.
- ~~**A real `top`.**~~ Done. `ps` still reports cumulative share since boot,
  which is the right thing for a one-shot listing; `top` samples twice and
  reports the difference. It leaves the idle tasks out of the table and uses
  them for the busy figure instead: there is one per core, each soaks up
  whatever nothing else wants, and sorted by CPU they would otherwise occupy the
  top rows forever. Per-core occupancy falls out of the same numbers, since each
  idle task is pinned to one core.
- **A working directory for apps** — see the app ABI note above.

## Filesystem

- **A file surface for apps.** A loaded app cannot open a file. The ELF loader's
  built-in tables export `close` and `fwrite` and nothing else of the family --
  no `fopen`, `open`, `opendir`, `read`, `unlink`, `mkdir` or `rename` -- so an
  app that touches the filesystem fails to *load*, not to work. This is why the
  mode work stopped at the shell and SFTP and published no `chmod` to apps:
  exporting a permissions call to programs that cannot open a file would be
  scaffolding, not a feature. Doing it properly means one `abi_fs.c` covering
  the surface, with `stat` reporting `espix_fs_mode()` so an app sees the same
  bits `ls -l` does, and it should land before or alongside a working directory
  for apps -- the two are the same complaint.

- ~~**Move file modes onto the file.**~~ Done. Modes live in a LittleFS user
  attribute, so rename and delete are the filesystem's problem rather than
  espix's. What remains is getting the accessor upstream: see
  [UPSTREAM.md](UPSTREAM.md), and delete
  [tools/patch-littlefs.py](../tools/patch-littlefs.py) when it lands.

- **An owner on a file, and a uid on a process.** The on-disk room is already
  there: every stored mode carries `uid` and `gid` fields, written as zero and
  read by nothing, so filling them in costs no rewrite. espix already has two
  identities -- `root` on the console, `esp` over SSH -- and `espix_proc_info_t`
  already carries the session a process belongs to, so "who is asking" is
  answerable. What is missing is who a *file* belongs to. That is the
  prerequisite for `chown`, for setuid/setgid/sticky to be more than bits, and
  for `/etc/shadow` to be worth splitting out of `/etc/passwd` -- which
  `espix_auth.h` declined to do for exactly this reason. Note that read/write
  enforcement needs more than this: see [KNOWN-ISSUES.md](KNOWN-ISSUES.md), and
  the VFS entry below, which is what would make it possible at all.

  Prior art worth reading before designing this: NuttX's
  `CONFIG_SCHED_USER_IDENTITY` tracks a task's real and effective UID/GID and
  checks file permissions against the effective one.

- **Own the filesystem driver instead of consuming one.** espix mounts
  `joltwallet/littlefs` and lives above it. It could instead drive LittleFS
  itself and register its own VFS: ESP-IDF's VFS is pluggable through
  `esp_vfs_register_fs*()` and `esp_vfs_fs_ops_t`, with
  `ESP_VFS_FLAG_CONTEXT_PTR` for the mount context.

  This is the entry that unblocks the others. Every `fopen()` an app makes is
  dispatched by the VFS into the registered driver, so if that driver is
  espix's, it can ask `xTaskGetCurrentTaskHandle()` ->
  `espix_proc_pid_of_task()` -> the slot's session -> its user, and *enforce* a
  mode rather than merely storing one. Read and write permissions are
  unenforceable today precisely because that seam belongs to someone else --
  which is also why NuttX, which owns its VFS, can do it and espix cannot. It
  would additionally retire `tools/patch-littlefs.py`, since espix would hold
  the `lfs_t` that patch exists to expose.

  The cost, measured rather than guessed: `lfs.c` is 6558 lines of upstream
  LittleFS and would be used unchanged. What espix would take on is the glue --
  `esp_littlefs.c` is 3020 lines, though a good deal of that is multi-backend
  support (SD/MMC, block devices, partitions) that a single-partition mount does
  not need -- plus the 89-line flash block device in `littlefs_esp_part.c`. And
  the maintenance: bugs joltwallet currently fixes would become espix's.

## Signals

- **Delivery during `select()` and `read()`.** The sleep family and `pause()`
  are delivery points, and `xTaskAbortDelay()` wakes a target blocked in any of
  them — including a `select()` over UART and eventfd, because IDF blocks that
  on a single semaphore. It cannot reach a task inside lwIP's `socket_select`.
  A per-process eventfd, always in the app's read set, is the mechanism that
  would; `tty_console.c` already has the pattern. Deferred deliberately: no app
  does socket `select()` yet, and building it before there is one to test
  against would be guessing.

## SSH

- **Rekeying.** RFC 4253 recommends new keys after an hour or a gigabyte;
  espix does neither, and worse, ignores a client that asks — so a long or
  high-volume session is dropped rather than degraded. Two things to know before
  starting: the client's KEXINIT buffer is freed as soon as KEX finishes, on the
  strength of nothing ever reading it again, so that lifetime has to be
  revisited; and strict KEX resets sequence numbers after NEWKEYS, which a
  second exchange must honour too.
- **An ed25519 host key.** espix offers exactly one host key algorithm,
  `ecdsa-sha2-nistp256`. OpenSSH has been steadily narrowing its defaults, and a
  client release that drops ECDSA would lock every user out with no recourse
  from the device side. Adding `ssh-ed25519` alongside means key generation,
  storage and signing work in `ssh_kex.c` and the host-key path, and changes the
  fingerprint users have already accepted — so it wants doing deliberately
  rather than in a panic. Worth noting that the *reason* this is on the list is
  that the neighbouring assumption already broke once: OpenSSH 10.3's KEXINIT
  outgrew a fixed buffer and every connection was refused with a message
  claiming no common algorithm. Algorithm lists are not a stable surface.

## Shell and console

- **A text editor.** There is none. `echo >` and `>>` cover `key=value` config,
  which is why it has not bitten yet, but anything larger wants an `ed`-style
  line editor.
- **`dmesg -n`** — a runtime console loglevel, replacing the hardcoded list of
  quieted driver tags.
- **`/etc/motd`.** The file exists in the rootfs but nothing reads it; the
  console prints a fixed banner instead. Printing motd at session start is the
  right home for it, and worth doing when SSH makes "logging in" a real event.

## Platform

- **A 1000Hz FreeRTOS tick.** `CONFIG_FREERTOS_HZ` is IDF's default 100, so a
  tick is 10ms and an app calling `vTaskDelay(1)` or `usleep(1000)` sleeps ten
  times longer than it asked, with no way to ask for less. Nothing in espix
  itself needs finer granularity — its own delays are coarse timeouts, and the
  console blocks in `read()` rather than polling — but espix is a platform for
  other people's apps, and a sensor loop or a bit-banged protocol will trip
  over this without the author knowing why. Arduino-ESP32 ships 1000. The cost
  is roughly 1% of a core in extra tick interrupts and preemption; the risk is
  that IDF's WiFi and lwIP are tested at 100. Worth doing with a measurement
  (`ps` CPU shares and `free` before and after, plus an SSH throughput check)
  rather than on reasoning, and it belongs in the target-independent
  `sdkconfig.defaults` so every target inherits it.
- **OTA slots.** The partition table is `factory`-only. Two 4MB OTA slots plus
  `otadata` would cost ~4MB of the 11.9MB rootfs but allow kernel updates over
  the network. Changing this later means reflashing everything, so it is worth
  deciding before the layout is in the field. A commented-out variant is in
  [partitions/esp32s3-16mb.csv](../partitions/esp32s3-16mb.csv); the 8MB table
  notes why the same shape does not fit there.

## Networking and time

- **WiFi roaming and multiple networks.** One SSID, one AP, no BSSID
  reselection.
- **A floor under the clock before NTP answers.** On a cold boot espix reads
  1970 until SNTP replies, deliberately: an obviously wrong date cannot be
  mistaken for a real one, it costs no flash writes on a filesystem that pays a
  block erase per write, and there is no persisted state to go stale. A soft
  `reboot` keeps the clock — ESP-IDF holds the offset in an RTC retention
  register, verified by setting 2035, removing `/etc/wifi.conf` so nothing could
  re-sync, rebooting, and finding 2035 intact — so this is a cold-boot-only
  window, about 6.5 seconds with a working network.

  What makes it worth revisiting is *what* falls in that window. Everything
  espix writes for itself does, structurally: those files are written during
  boot, and boot is when the clock is wrong. On a fresh device `/etc/passwd`
  (2.9s), `/etc/ssh/host_ecdsa_key` (3.4s), `/etc/hostname` and
  `/etc/wifi.conf` are all created before the sync at 6.5s, and keep 1970
  mtimes for good.

  Three consequences, in increasing order of how much they will hurt. Time runs
  backwards across a power cycle, so a file written before it is dated 2026 and
  one written seconds after is dated 1970 — anything comparing mtimes (rsync,
  an sftp client syncing a directory, "newest wins") is silently wrong. A device
  with no network never gets a clock at all. And TLS is the forcing function:
  certificate validity is checked against the clock, so HTTPS, OTA and MQTT all
  fail at 1970, and the workaround people reach for is disabling validation,
  which is worse than a wrong clock.

  The argument for 1970 assumes the clock *value* is the signal that time is
  unverified. It is not — `espix_time_is_synced()` is, and it stays false
  whatever the clock reads. So a floor costs no honesty: it buys plausible file
  timestamps *and* keeps an accurate "not confirmed this boot", which is the
  split systemd already makes between `TimeEpoch` and timesyncd's state.

  The fix, when it is done: floor the clock at the later of the firmware build
  epoch (baked in by CMake, no writes at all) and a timestamp written when NTP
  confirms and on `reboot` — roughly one write per boot, against the hourly cron
  `fake-hwclock` uses on Raspberry Pi. Never move the clock backwards, and leave
  `espix_time_is_synced()` meaning exactly what it means now.

  Note while doing it that espix cites Raspberry Pi as precedent for having no
  RTC, which is true and reads as support for the current behaviour — but RPi OS
  runs `fake-hwclock` and does not sit at the epoch. The comparison argues the
  other way.
