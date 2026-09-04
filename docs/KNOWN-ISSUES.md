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

- **Apps have no working directory.** ESP-IDF has no process-wide cwd —
  `chdir()` is a hardcoded `ENOSYS` stub and `getcwd()` always answers `/` — so
  espix's cwd lives only in the session. A loaded app calling `fopen("data.txt")`
  gets `/data.txt`, never the directory you `cd`'d into. Apps should take paths
  as arguments.

- **The fault handler intercepts but does not recover.** A crash is recorded and
  reported in `dmesg` on the next boot, and then the system reboots.
  `espix_fault_request_reap()` exists with no callers.

## Filesystem

- **`ls -a` cannot show `.` and `..`.** It shows dotfiles, which is what you
  want it for, but the two directory entries themselves never arrive: the ESP
  LittleFS port's `readdir()` reads in a loop until it gets what it calls "a
  real object name", discarding both before espix sees them. So `-a` behaves as
  GNU `ls`'s `-A` and there is no way to make it behave otherwise from here.

- **`ls` holds at most 512 entries.** Sorting means buffering the directory, and
  past that ceiling the listing stops and says `ls: stopped at 512 entries`
  rather than silently ending. The same applies if the allocation fails partway.

- **Only the execute bit is enforced.** `ls -l` and `sftp ls -l` show nine
  permission bits and `chmod` sets any of them, but read and write are metadata:
  `chmod 000 /etc/motd` does not stop `cat` reading it. Enforcing them would
  mean checking at every entry to the filesystem, and an app does not go through
  espix to get there -- it calls libc, which reaches the VFS, which has no idea
  which process is asking. Checking only in the shell's own commands would be a
  boundary you could step around with `run`, which is worse than none. The
  execute bit is different because espix *owns* the exec path, and so can be
  asked.

  The *seam* now exists: espix registers the root VFS itself, so every
  `open()` — an app's included — passes through `espix_fs_access_check()`, which
  resolves the calling task to a process to a session to a user. What is still
  missing is an owner on a *file* to compare that against, so the check is wired
  and permissive. See [ROADMAP.md](ROADMAP.md#filesystem).

- **No file has an owner**, so `chown` does not exist and setuid, setgid and
  sticky are refused by `chmod` rather than stored. There are two identities --
  `root` on the console, `esp` over SSH -- but nothing records which of them a
  file belongs to, and all three of those bits are defined in terms of one.

- **SFTP silently drops setuid, setgid and sticky** where the shell's `chmod`
  refuses them out loud. SFTP has no partial-success status, so failing the
  request would fail an entire `scp -p` over a bit that was never going to be
  honoured. Setting the mode of a *directory* over SFTP is accepted and ignored
  for the same reason.

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
