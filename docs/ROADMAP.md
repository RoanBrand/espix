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

## Filesystem

- **`mount`, `umount` and `/proc`, with espix's own mount table.** espix owns
  the root VFS but routes only the root: it holds one pointer to one filesystem.
  Anything else mounted — FAT on an SD card, a second LittleFS partition,
  `/proc` — would be registered with ESP-IDF's VFS at its own prefix and would
  outrank espix's fallback, so espix would never see those calls and its
  permission check would not apply to them.

  Doing it properly means espix keeping a path-to-lower-ops table of its own and
  routing internally, rather than letting IDF route. `esp_littlefs_mount()`
  already returns exactly what such a table stores, and the FAT and SPIFFS ports
  would need the same mount-without-registering split — which is the argument
  for getting it upstream rather than carrying it (see
  [UPSTREAM.md](UPSTREAM.md)). `/proc` is then espix's own ops rather than a
  filesystem at all, which is what makes it the cheap one to do first.

  **"Routing internally" is smaller than it sounds**, and worth costing before
  rejecting it as reinventing the VFS. ESP-IDF keeps the libc glue and the
  global fd table either way; what espix adds is a prefix lookup (an array and a
  longest-match `strncmp`, ~30 lines) and one wrinkle — with two filesystems
  below, LittleFS fd 3 and FAT fd 3 collide when they come back into espix's
  `read()`, so the mount index gets packed into the fd espix returns and
  unpacked on the way in. About ten lines and no second table. Call it eighty
  lines, not a VFS.

  Note this is a *precondition* for uniform permissions, not a nice-to-have
  beside them: see [KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem).

  **Why not extend ESP-IDF's VFS instead?** It has no hook of any kind —
  `esp_vfs.h` offers nothing to intercept with. Patching `$IDF_PATH` is a
  non-starter: it is shared by every project on the machine, where
  `managed_components/` is per-project and gitignored. The real alternative is
  shadowing the `vfs` component with a patched copy in `components/vfs/`, which
  a project component may do — and that is *architecturally the better answer*,
  putting the check in `esp_vfs_open()` where Linux puts it and covering every
  mount with no routing code in espix at all. It is not first choice only
  because of what it costs: `vfs.c` and `vfs_calls.c` are ~58KB of core code
  that the console, sockets and eventfd all depend on, to be re-merged on every
  IDF upgrade. Worth revisiting if the eighty lines above turn out to be wrong.

- ~~**A file surface for apps.**~~ Done. `abi_fs.c` publishes fopen, open,
  read, stat, opendir and the rest, and almost all of it is unwrapped libc
  because those calls already dispatch into espix's own VFS -- so they get the
  permission check and the working-directory resolution for free. Three are
  espix's own because IDF's are stubs: `chdir`, `getcwd` and `chmod`, the last
  because IDF's returns success without doing anything (see
  [UPSTREAM.md](UPSTREAM.md)).

  `access` is still absent, deliberately: LittleFS's port never implemented it,
  so espix's VFS leaves it NULL, and exporting a call that always fails would be
  worse than an app failing to load and being told which symbol was missing.

- ~~**Move file modes onto the file.**~~ Done. Modes live in a LittleFS user
  attribute, so rename and delete are the filesystem's problem rather than
  espix's. What remains is getting the accessor upstream: see
  [UPSTREAM.md](UPSTREAM.md), and delete
  [tools/patch-littlefs.py](../tools/patch-littlefs.py) when it lands.

- ~~**An owner on a file, and a uid on a process.**~~ Done. Accounts carry a uid
  and a gid, `/etc/passwd` grew both fields with a one-time migration for older
  three-field records, and `root` became a real account -- locked, so it is
  reachable from the console but never over SSH. A session resolves its
  credentials once at login and a process copies them at spawn rather than
  following the session pointer, which would be a use-after-free for anything
  backgrounded.

  Files get their owner from the stored attribute when there is one and from a
  rule when there is not: the account whose home contains the path, longest home
  winning, and root otherwise. Since root's home is `/`, that makes the rootfs
  root's and `/home/esp` esp's with nothing written to flash to say so -- which
  is what keeps a freshly imaged device free of attribute data and what makes a
  storage-flash come back correct.

  What is left of the case for `/etc/shadow`: very little. The file is 0600
  root, and every reader goes through espix_auth, which raises privilege for its
  own open -- espix has no setuid, so that seam is what stands in for it. A
  split would separate the hashes from the names, which matters only once
  something other than espix_auth needs to read the names.

- ~~**Own the filesystem driver instead of consuming one.**~~ Done differently,
  and better. espix registers the root VFS and stacks LittleFS underneath it by
  pointer rather than by path, so the permission seam exists without espix
  taking on 3000 lines of POSIX-to-LittleFS translation or giving up SD/MMC and
  the block-device layer. See [ARCHITECTURE.md](ARCHITECTURE.md). The remaining
  work is the *policy*, which is the item above, and getting the two patched
  entry points upstream, which is [UPSTREAM.md](UPSTREAM.md).

- ~~**Enforce read and write, once files have owners.**~~ Done, and it applies
  to builtins as well as to loaded apps: credentials come from the process when
  there is one and from the task's current session otherwise, so `cat` and `rm`
  over SSH are checked like anything else. Only espix itself -- a task that is
  neither -- goes unchecked, which is what lets boot read its own configuration.

  `stat` is still deliberately not gated, and `access` is still unimplemented to
  match the port. What remains: search permission is checked on the final path
  component and on the parent for anything that creates or removes a name, not
  on every intermediate directory; see [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

- ~~**A way up to root that is not the serial console.**~~ Done, as `sudo`:
  `/etc/sudoers` lists the accounts that may run a command as uid 0, seeded with
  the default account the way an installer puts the first user in the `sudo`
  group. root stays locked, and `sudo passwd root <pw>` gives it a password if
  somebody wants one, with `passwd -l root` to take it away again. What it does
  not do is re-authenticate; see [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

- ~~**The other three mode bits.**~~ Done, and each is consulted: setuid and
  setgid give a process the binary's ids at exec, and sticky lets a shared
  directory allow writes without allowing deletions -- which is what made a 1777
  `/tmp` possible, and `/tmp` is why the bit was worth having. The combinations
  espix does not act on -- setuid or setgid on a directory, sticky on a file --
  are refused by name rather than stored and ignored.

  setuid is a guardrail rather than a boundary on the S3, which has no MMU. It
  is implemented now because the S31 does.

- ~~**Check SFTP against the same rules as everything else.**~~ Done. The SFTP
  subsystem gets a session carrying the connection's credentials and makes it
  the task's current one, so `espix_fs_access_check()` finds a caller and every
  transfer is checked exactly as the shell is. Uploads are now owned by the
  account that made them, which follows from there being a session at all.

  It also moved the client's starting directory from `/` to the account's home,
  where every other SFTP server puts it. That had to land together: `/` is
  root-owned, so enforcing the check while leaving the client there would have
  broken `scp file host:` with no remote path — the commonest invocation there
  is.

- ~~**Groups that are more than a number.**~~ Done. `/etc/group` carries
  `name:gid:members`, an identity holds a set of groups rather than one, and the
  permission check matches the group triad against any of them — so two accounts
  can share a file, which is the only thing that ever made that middle column
  worth printing.

  Credentials are resolved at login and copied into a process at spawn, so a
  change to `/etc/group` takes effect at the next login rather than mid-session.
  That is the same bargain espix already makes for the uid, and it is what
  `newgrp` exists for on a real system.

- ~~**A root for an app.**~~ Done, as `run -R <dir>`: the process may not
  resolve a path outside that directory, and everything else answers ENOENT.
  `resolve()` in `espix_fs/vfs.c` covers everything that reaches the
  filesystem through the VFS -- which is every path operation an app makes,
  including `stat` and `utime`, both of which the permission check deliberately
  does not gate.

  `chmod` and `chown` are checked separately, in `espix_fs_admin_check()`, and
  that is not belt and braces: they never enter the VFS. `abi_fs.c` resolves
  their paths itself and calls `espix_fs_chmod()` directly, because ESP-IDF's
  `chmod` is a stub that returns success without doing anything. It is the same
  gap `espix_fs_admin_check()` was written for in the first place -- it says so
  in its own comment -- and the root shipped without covering it for exactly one
  commit. `apps/hello chmod` is the regression test that keeps it covered.

  The root is asked *before* the uid short-circuit there, so a confined process
  running as uid 0 is still confined. A root is not a permission and does not
  yield to one; that it survives the uid being wrong is much of why it is worth
  having on top of ownership.

  **Why it is not what users already do.** Permissions decide whether a uid may
  *open* a path; they never stop it being *named*, and the mode rule gives
  directories 0755 and files 0644, so a service account could walk the whole
  tree and read everything nobody had explicitly locked. Two things nobody had
  locked were the WiFi PSK and the SSH host private key. That is the shape of
  the difference: discretionary permissions are open by default and closed by
  someone remembering, and a root is closed by default and opened by handing
  something over. It also does not depend on the uid being right, and it is per
  *app* where a uid can only be per *user*.

  **Restriction, not chroot.** Paths stay globally absolute. Real chroot wants
  `getcwd` translation and, far more importantly, bind mounts: a jail holding no
  `/bin`, no `/etc` and no `/tmp` is not somewhere a program can run, and espix
  has no mount table yet -- the first item in this section is the precondition.
  When it lands, chroot semantics become worth building on top of this.

  **The confinement starts after the ELF is loaded**, immediately before the
  entry point, which is where `execve(2)` draws the same line: espix opens the
  binary through the caller's view of the filesystem, and only the program runs
  in the new one. Arming it at spawn instead confines the loader, and then no
  rooted process could ever be given a program from outside its own root.
  Raising privilege around the load was the other way to get there and is
  wrong -- privilege bypasses the permission check too, so it would load files
  the caller may not read.

  What it is not: a sandbox. Device VFSes register longer prefixes and ESP-IDF
  routes them before espix's fallback is consulted, which is exactly what keeps
  a confined app's stdio working and also means this is a filesystem boundary
  and nothing else. With no MMU an app shares the address space regardless. Like
  setuid, it is a guardrail here and a real boundary on a part with an MMU.

- **Per-app export tables.** The other half of what the README used to call
  "per-app capabilities", and still open. espix publishes one fixed set of
  symbols to every app it loads; an app that has no business calling
  `esp_wifi_*` is handed it anyway. The loader resolves against a table, so
  subsetting per app is a matter of choosing which table, and the cost is
  deciding where the per-app policy is written down -- a manifest beside the
  binary, or something in the ELF itself.

- ~~**More than one account.**~~ Done, with the `useradd` family rather than
  Debian's `adduser` wrappers: one command per job, and the names that exist on
  every distribution. `useradd -r` is a service account — locked, a uid in
  100–999, no home — which with `sudo -u` is the whole mechanism for running an
  app under its own identity. `passwd` no longer creates accounts, which is what
  fixed it handing every new one uid 1000.

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
  line editor. It is also why so much of espix's configuration ended up behind
  commands — `wifi connect`, `passwd`, `useradd` — rather than as files to edit.
  That is a reasonable shape for a device, but it should be a choice rather than
  what happens because there is no alternative.
- ~~**`dmesg -n`.**~~ Done. `klog_store()` echoed on a hardcoded
  `level <= ESPIX_KLOG_INFO`; that is now a variable and `dmesg -n <0-3>` moves
  it, taking `err`/`warn`/`info`/`debug` as well as numbers because remembering
  which direction 3 is proves harder than it sounds. The ring is unaffected --
  `dmesg` still lists everything, whatever the console is set to. Root's, since
  it is a property of the device and one user quieting it silences the serial
  line for whoever is sitting at it, and not persisted, so a device left in a
  debugging setting does not stay there.

  The original entry said this would replace "the hardcoded list of quieted
  driver tags". It does not, and cannot: those are three `esp_log_level_set()`
  calls in `coredump.c`, `wifi.c` and `proc.c`, which configure **ESP-IDF's**
  logger. espix's klog is a separate ring with its own levels, and the two only
  meet where `espix_kernel_early_init()` hooks esp_log's output into the ring.
  Making the IDF tags runtime-settable is a different, smaller job that nobody
  has asked for.
- ~~**`/etc/motd`.**~~ Decided against, and the file is gone. The condition this
  entry waited on -- "worth doing when SSH makes logging in a real event" -- did
  arrive, and the answer turned out to be the other one: `cmd_motd.c` generates
  a greeting from what the system actually knows (version, uptime, address, disk)
  and both transports print it at session start. A static file repeating a
  directory guide at every login is noise beside that, and it had already gone
  stale -- it ended by telling you to edit it, on a system with no editor.

  What a real `/etc/motd` is *for* is a local administrator's message, which is
  worth having again once there is an editor to write one with. It would then be
  printed alongside the generated greeting rather than instead of it.

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

## Further out

Not costed, not committed to, and further from the current shape of espix than
anything above. Kept because they are why the project has the targets it has --
the P4's display and the S31's MMU are on the hardware list for these, not the
other way round.

- **Terminal output to a display**, and input from a USB keyboard and mouse. The
  first is the interesting half: a framebuffer console is a second transport
  beside UART and SSH, and the session layer was built transport-agnostic
  precisely so a third one could be added without touching the shell. The P4 is
  the target -- MIPI DSI, with an HDMI variant.

- **A minimal 2D desktop environment**: a filesystem browser, a JPEG viewer, an
  audio player for whatever formats decode cheaply, and video on the P4, which
  has the hardware for it. These are *apps*, not kernel work -- which is the
  point of the ELF loader and the export table, and the best argument for
  keeping that boundary honest. The gap between here and there is mostly a
  graphics stack and a windowing model, neither of which espix has any business
  inventing.

These arrived with the project and predate almost everything in this file; they
are recorded here rather than in a separate note so there is one place to look.
