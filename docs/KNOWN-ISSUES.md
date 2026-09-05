# espix known issues

Behaviour that is already implemented and will still surprise you. Everything
here is deliberate, understood, or at least understood well enough to be worth
writing down — the purpose is that it gets recognised rather than debugged from
scratch a second time.

For work espix might take on see [ROADMAP.md](ROADMAP.md); for defects that
belong to ESP-IDF rather than to espix see [UPSTREAM.md](UPSTREAM.md).

## Processes

- **A hard kill leaks whatever the app held.** SIGKILL deletes the task
  outright, so `teardown()` never runs. The neopixel app hands its RMT channel
  back there (`rmtDeinit()`), and without that the channel and its GPIO
  reservation outlive the app — which shows up as `GPIO 48 is not usable` on the
  next run and eventually as no free channel at all. `kill` and Ctrl-C ask first
  and only escalate after the grace, so this is specific to `kill -9` and to an
  app that ignores everything else. There is no address space to tear down and
  no per-process ownership of heap or fds, so nothing can reclaim it for the app.

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
  exactly what `espix_sigcheck()` is exported for. `apps/sigtest spin` is the
  case in the flesh. SIGKILL is the answer when it is somebody else's binary.

- **`ssh host <cmd>` was unreliable; the causes are now understood.** It
  truncated output and reported 255 for commands that had succeeded, roughly a
  third of the time, while the same app was correct every time on the serial
  console. Five separate defects turned out to be involved, all now fixed:

  - The client's `CHANNEL_EOF` was treated as the channel closing.
  - A stale event-group bit made `espix_proc_wait()` report a freshly spawned
    process as already finished.
  - `conn->out_buf` was filled outside the transmit lock at six sites, so one
    writer overwrote another's packet in flight — usually the exit status.
  - A use-after-free that rebooted the board: `esp_cleanup_r()` fcloses an app's
    stdout at `vTaskDelete()`, writing through a session whose channel
    `finish_session()` had already freed.
  - The connection was closed with a bare `close()` the moment the channel
    finished. The client's own `CHANNEL_CLOSE` was then still unread in the
    receive buffer, and `close()` with unread data sends an RST rather than a
    FIN — which discards whatever is still queued to send. That is what
    produced "Connection closed by remote host", and when the reset overtook
    the last packets the client lost the exit status and reported 255.

  The last one hid the others for a long time because the server log is
  identical either way: every one of those connections reaches "connection
  closed" normally. Nothing is wrong with the session — only with the goodbye.

- **A sixth cause is still open: an app's output can corrupt the SSH stream.**
  `ssh host 'run /bin/hello'`, whose app prints eight lines, fails about **2
  runs in 30** with `Corrupted MAC on input` and
  `ssh_dispatch_run_fatal: message authentication code incorrect`. The device is
  unharmed — it stays up, records no core dump, and keeps exactly one
  `sshd:conn` task — so this is a packet leaving the server malformed, not a
  crash. Sometimes the first lines arrive and the corruption follows mid-stream.

  Measured deliberately rather than inferred: **2 of 30 on `main` at 8e3d545**,
  and 2 of 30 on the branch that added the process root, which is the same rate
  and settles that the root did not cause it. Doubling
  `CONFIG_ESPIX_PROC_STACK_SIZE` to 16384 did not fix it either, which rules out
  the obvious guess that the app task was overflowing its stack — it was tried,
  and the failure survived it.

  **Ruled out by measurement, so nobody need pay for these twice:**

  - *Not the app task overflowing its stack.* Doubling
    `CONFIG_ESPIX_PROC_STACK_SIZE` to 16384 changed nothing.
  - *Not `write_all()` abandoning a partial send on EAGAIN.* That was a real
    defect and is fixed, but instrumenting the branch showed it is never taken:
    zero EAGAIN on every connection, corruption anyway.
  - *Not two tasks inside `ssh_packet_write()` at once* — the invariant
    `ssh_priv.h` documents. A guard counting overlapping entries recorded zero
    collisions across every connection, failing ones included.

  **What the packet trace shows on a healthy run**, which is where the next
  attempt should start: eight `CHANNEL_DATA` (type 94) written by the
  `app:hello` task, then `CHANNEL_REQUEST`/exit-status (98), `CHANNEL_EOF` (96)
  and `CHANNEL_CLOSE` (97) written by the `sshd:conn` task, sequence numbers
  strictly consecutive and every send reporting success. Two tasks, correctly
  serialised, so the handoff between the app's last write and the session's
  closing packets is the remaining place to look.

  **It hides under instrumentation**, which is the awkward part and worth
  knowing before chasing it: 2/30 on a stock build, 1/31 and 2/56 on builds
  carrying only counters, and **0/140 and 0/60** on builds that klog per packet.
  Anything that adds work between packets makes it disappear, so a fix must be
  proven on a build with no tracing in it, over a long run — 0/30 means nothing
  here, since 0/60 was observed with the bug still present.

  On a failure the connection task blocks and its teardown is delayed by about
  five seconds (`PARTIAL_READ_TIMEOUT_MS`), so `ps` shows two `sshd:conn` tasks
  and no "connection closed" line is logged for that session.

  **Usually that clears, but not always**, and the exception is not understood.
  A device left running will sometimes keep one extra `sshd:conn` blocked
  indefinitely -- observed at normal priority with a small stack, surviving
  minutes of idle time. What it does *not* do is accumulate: ten further
  connections after one appeared left the count at two, so it is one task stuck
  once rather than a leak per connection, and the `CONFIG_ESPIX_SSH_MAX_SESSIONS`
  ceiling of four is not being walked towards. A reboot clears it. Which event
  strands it has not been pinned down -- it has been seen both after a
  `Corrupted MAC` failure and after a run of ordinary connections with none.

  One thing found while chasing this *was* fixed, and it is worth knowing about
  because it could have produced exactly these symptoms: **a kernel log echoed
  through the calling task's `stdout`.** `klog_store()` printed with a plain
  `printf()`, and newlib's `stdout` is per-task — `espix_proc` points a loaded
  app's at the session it was launched from, so for an SSH session `printf()`
  *is* the encrypted channel. Any klog at INFO or above from the send path
  therefore re-entered that path: `klog` → `printf` → `chan_write` →
  `ssh_packet_write` → another klog, unbounded, on an 8KB app stack, with the
  inner packet rebuilding the shared `out_buf` and bumping `seq_out` underneath
  the outer send. `tx_lock` does not catch it — it is recursive by design so
  `chan_write()` can nest `send_data()`.

  klog now writes to a console stream captured at boot. This is not offered as
  the cause of the corruption above: the reachable sites are
  `wait_for_window()`'s two warnings and `ssh_packet_write()`'s two framing
  errors, and none of those strings appears anywhere in the several hundred
  `dmesg` lines captured across ~350 runs, failing ones included. They do not
  fire. A hazard closed, not a bug explained.


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
  `fcntl` and `fsync`, so those work — but `ls` cannot show it, for two
  unrelated reasons. `ls /dev` fails because `/dev` is not a directory in the
  root filesystem and `readdir` never merges mount points. `ls -l /dev/uart/0`
  fails because the UART VFS's *directory* ops contain only `access` — there is
  no `stat` for `ls` to call.

  **Unchanged by espix owning the root VFS**, so this is not a regression to go
  looking for: LittleFS was the fallback before too and `/dev/uart` outranked it
  identically. Both causes belong to ESP-IDF's driver and to what the rootfs
  happens to contain.

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

- **No file has an owner**, so `chown` does not exist and setuid, setgid and
  sticky are refused by `chmod` rather than stored. There are two identities --
  `root` on the console, `esp` over SSH -- but nothing records which of them a
  file belongs to, and all three of those bits are defined in terms of one.

- **SFTP silently drops setuid, setgid and sticky** where the shell's `chmod`
  refuses them out loud. SFTP has no partial-success status, so failing the
  request would fail an entire `scp -p` over a bit that was never going to be
  honoured. Setting the mode of a *directory* over SFTP is accepted and ignored
  for the same reason.

- **A process root is a filesystem boundary, and only that.** `run -R <dir>`
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

## Shell and console

- **Kernel messages land on your prompt.** That is deliberate and matches Linux,
  where kernel output goes to the console and remote users run `dmesg`. Since
  the console-prompt work the line is ended and reissued underneath, so the
  prompt is never left buried — but the messages themselves are not going to
  stop appearing. Do not "fix" this by routing klog through the current session.

## SSH

- **`ssh host <cmd>` has no stdin.** The command runs and its output and exit
  status come back, but nothing on espix reads standard input — apps have no
  file or stdin ABI at all — so `ssh host 'cat' < file` will not do what you
  mean. Worse than merely unread: while a foreground process runs,
  `chan_poll_interrupt()` consumes whatever has arrived looking for Ctrl-C, so
  client-sent data is discarded rather than queued.

- **`ssh host <cmd>` has no separate stderr.** espix has one output stream, so
  errors arrive interleaved on stdout and `2>` at the client separates nothing.

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
