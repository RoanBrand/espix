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

- **An init and a service manager.** espix cannot run anything unattended.
  There is no init, no autostart, no supervision and no `nohup`, `setsid` or
  `disown`; `app_main()` is a boot sequence and nothing else, and nothing in the
  tree spawns a program. Worse than absent: `finish_session()` calls
  `espix_proc_hangup()`, which kills the session's processes on the way out and
  logs "killed N processes on exit". So a backgrounded program dies when you log
  out, by design and correctly -- there is nowhere else for it to belong.

  This is the gap behind `confine` and `sudo -u`. Both exist to run a program as
  a service with its own identity and its own view of the filesystem, and the
  README's `sudo -u www confine /srv/www /bin/httpd &` cannot survive the login
  that started it. The pieces are built; there is nothing to attach them to.

  **A service still needs somewhere for its output to go.** It cannot be the
  SSH channel of the session that started it, because that channel is freed
  when the session ends -- which is the same lifetime problem that limits a
  backgrounded app's redirection today (see KNOWN-ISSUES). The stream split
  itself has landed; what a service needs is a *destination that outlives a
  login*, and espix has klog and a ring already, which is most of the answer.

  Worth stealing systemd's shape rather than inventing one, because it settles
  where policy lives: a unit is a *file* declaring identity and confinement
  together (`User=` beside `ProtectSystem=strict` and `ReadWritePaths=`), which
  is exactly the pair espix currently spells as two nested commands on a shell
  line that nothing can replay at boot. `confine` and `sudo -u` would be
  absorbed into that file rather than replaced by it -- they stay useful for the
  interactive case, the way `systemd-run` does.

  Not small: a unit parser, a supervisor task with restart policy, dependency
  ordering, and `systemctl`-shaped commands to inspect it. But the alternative
  is that espix stays a device you log into rather than one that runs something.

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

- **`mount`, `umount` and `/proc`, with espix's own mount table.** *Done for FAT
  on USB — see [USB-HOST.md](USB-HOST.md#stage-2--mounting); `/proc` is the rest
  of it.* espix owns
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

  **The second filesystem exists, and it mounts.** Enumeration, identification
  and the partition table shipped first — [USB-HOST.md](USB-HOST.md) — and
  `mount sda1 /mnt` now puts a FAT volume in the namespace with the permission
  check still applying to it. Both defects that blocked it are closed: the
  filesystem is reached through espix's table rather than registered at a prefix,
  and a mount that stores no metadata of its own (FAT) has `chmod` answer EPERM
  instead of filing littlefs attributes for a path that is not on littlefs.

  **"Routing internally" is smaller than it sounds**, and worth costing before
  rejecting it as reinventing the VFS. ESP-IDF keeps the libc glue and the global
  fd table either way; what espix adds is a prefix lookup (an array and a
  longest-match `strncmp`) and one wrinkle — and the wrinkle, written down wrong
  here twice, is the fd. The earlier version of this paragraph said there was no
  collision, because espix passes the lower filesystem's fd through unchanged
  and IDF allocates from one global table. That is not what happens. The ops
  espix calls are FatFs's *inner* ops — the ones `tools/patch-fatfs.py` exposes
  precisely so that `esp_vfs` is bypassed — and those hand out FatFs's own
  `fat_ctx->files[]` slots: 0, 1, 2, … While IDF's global table, and the
  console, are handing out those same numbers. So a FAT file is fd 2 while the
  console is fd 2, and anything keyed on the fd without knowing which filesystem
  it came from is wrong.

  Measured, and written up in [KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem): the
  first two writes after a boot fail — one `EBADF` on close, one silently — and
  every write after that succeeds until the next boot.

  So the fds need *packing*, which is what this paragraph twice talked itself out
  of: a file opened below gets a number from espix's own space and every op maps
  it back. That is the reverse map plus an offset, and `lower_t s_mounts[]` in
  `vfs.c` has only the second half of it.

  The options, and what each costs:

  **A — formalise the seam.** One registration, espix's own fd keys, every
  assumption written down with the IDF line it depends on, and tests that fail if
  IDF changes one. Cheapest, keeps IDF's libc glue, sockets and `select()`
  working unchanged, and it is what espix does now — see
  `tests/suites/12-vfs.sh`, which asserts the five things that matter.

  **B — one registered VFS per mount.** The IDF-native shape, and wrong here:
  each mount publishes a second name for its filesystem, which is exactly what
  the stacked design exists to avoid, and the permission check cannot move into
  vendored filesystems, so it would be bypassable.

  **C — own the syscalls.** espix implements the libc entry points itself and
  keeps an fd table at whatever size it likes, with permissions at the single
  entry point, `/dev` nodes with real semantics, and no prefix matching to be
  surprised by. The obstacle is not files, it is **sockets**: lwIP's sockets live
  in IDF's table and are the reason `select()` matters at all, so C needs a
  mapping layer between espix's fds and IDF's socket fds before any file moves.
  Worth doing when one of these is true rather than when the tidiness annoys the
  most: device files that need real read/write semantics, sockets selectable in
  the same set as open files, per-process bind mounts, or a second filesystem
  type that must not be reachable by path.

  **D — extend IDF.** Cheapest per unit of pain removed, and the ask to lead with
  upstream, because two of the three are housekeeping rather than features:

  1. **Document that `local_fd` is opaque and stored verbatim.** It already
     behaves that way (`vfs.c:764`), and it is the only reason espix can hand out
     keys of its own rather than borrowing the lower filesystem's numbers. One
     sentence in the header turns an accident into a contract.
  2. **Let a release work on non-permanent entries.** `esp_vfs_unregister_fd()`
     refuses anything not registered permanent (`vfs.c:679`), so a driver that
     allocates an entry per open and frees it after the close path has no way to
     do both. That cost espix one leaked entry per open, and eventually a table
     with no room left.
  3. **A stacking registration.** A VFS that receives the untranslated path
     *before* the prefix match and may forward to the next match — the one thing
     that would let espix stop being "the default VFS that happens to be on top"
     and start being a layer with a defined place in the order.

  **And a smaller patch worth considering: 32-bit `off_t`.** `/dev/sda4` lists as
  `0` for 23 GiB, and no file over 4GB can be reported or seeked correctly. There
  is no Kconfig for it (UPSTREAM.md has the detail), so it is a define on the
  newlib headers, in the shape of the other two patches — with the difference that
  it would touch every `struct stat` in the image. That is why it wants its own
  decision rather than riding along with something else.

### The POSIX surface, and which layer would have to change

espix presents a POSIX-shaped interface over filesystems that are not POSIX and a
VFS that is not the kernel's, so some of what an app can call behaves differently
or not at all. This is the list, with the honest answer for each: a fix and the
layer it belongs to, or a choice and the reason.

| surface | espix today | what would change it |
|---|---|---|
| `stat().st_uid`, `.st_gid` | the owner rule's answer, for a path | **done**: `vfs_stat()` fills both from `espix_fs_owner()` |
| `fstat().st_uid`, `.st_gid` | still `0`, and marked in the code | the rule is path-based and a descriptor is not a path; the fix is to remember the path on the fd slot, or to leave it |
| `stat().st_blksize`, `.st_blocks` | plausible constants | a real value, or leave and document |
| `access()` on a path | not espix's to answer | implement in the VFS |
| `link()`, `symlink()` | unsupported | the VFS, then the lower filesystems |
| `select()` on a file | `ENOSYS` | a select in the VFS, or option C below |
| `dup()`, `fcntl()` | partial | the VFS |
| `mmap()`, `statvfs()`, `utime()` | partial or absent | the VFS, and the littlefs port's Kconfig for utime |
| `/dev/<device>` opened as a file | `EOPNOTSUPP` (a name, not a stream) | raw block I/O as its own feature |
| a volume whose device was pulled | `ENOSYS` from every operation | deliberate; `EIO` would need a refusing helper per op |
| `chmod`/`chown` on metadata-less FAT | refused | deliberate: there is nowhere to store it |
| an fd's number | espix's own (128–159), not the lower fs's | deliberate; an fd is opaque, so nothing should care |
| `.` and `..` in a directory listing | absent; `..` still resolves in a path | the lower filesystem's doing; the VFS could synthesise them |

Two things this table is for. It is where a `ESPIX_NOT_POSIX:` marker in the code
points, so a reader can find out what to do rather than only what is wrong. And it
is the checklist that decides option C: when the rows saying *the VFS* outnumber
the rows saying *deliberate*, owning the syscalls stops being tidiness and starts
being the shortest path.

  Note this is a *precondition* for uniform permissions, not a nice-to-have
  beside them: see [KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem).

  **Why not extend ESP-IDF's VFS instead?** It has no hook of any kind —
  `esp_vfs.h` offers nothing to intercept with. Patching `$IDF_PATH` for *this* is
  a non-starter: it is shared by every project on the machine, where
  `managed_components/` is per-project and gitignored — and a behavioural change
  to core VFS code is not something to carry in a tree espix does not own. The
  real alternative is shadowing the `vfs` component with a patched copy in
  `components/vfs/`, which a project component may do — and that is
  *architecturally the better answer*, putting the check in `esp_vfs_open()` where
  Linux puts it and covering every mount with no routing code in espix at all. It
  is not first choice only because of what it costs: `vfs.c` and `vfs_calls.c` are
  ~58KB of core code that the console, sockets and eventfd all depend on, to be
  re-merged on every IDF upgrade. Worth revisiting if those eighty lines turn out
  to be wrong.

  Stage 2 did patch `$IDF_PATH` — for `fatfs`, not `vfs`, and the distinction is
  the whole reason it was acceptable: three *additive* functions and a refactor
  whose absence is a link error on the first build, rather than a change to core
  code whose absence would be a behavioural difference nobody would notice until
  it mattered. See [UPSTREAM.md](UPSTREAM.md) — the request goes there either way.

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

- ~~**A root for an app.**~~ Done, as `confine <dir>`: the process may not
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

  **The closer relative is `unveil(2)`, not `chroot(2)`**, and it is worth being
  precise because the name is a trap. chroot changes what `/` *means*: a process
  under `chroot /srv/www` opens `/index.html` and the kernel hands it
  `/srv/www/index.html`. espix rewrites nothing -- `resolve()` in
  `espix_fs/vfs.c` resolves the path normally and *then* refuses anything
  outside the root, so the program must still say `/srv/www/index.html`. That is
  OpenBSD's `unveil(2)`, or Linux's Landlock: a visibility filter over ordinary
  path resolution. Calling this `chroot` would silently break anyone porting a
  real chroot invocation, which matters for a security feature. Whatever the
  confinement launcher ends up being called (see the `run` item under Shell and
  console), it should not be called chroot until it earns the name.

  **It has no automated test**, which is the gap worth closing first. The only
  coverage is `apps/hello`, run by hand. This is a boundary that already
  regressed once -- `abi_chmod` resolved its own paths and bypassed the root for
  a commit -- and `tests/suites/30-proc.sh` is where it belongs, beside the
  existing "an app is refused /etc/passwd". Held back only so it lands with
  whatever the command is finally called.

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

- **`SERIAL=` in /etc/fstab, for the device rather than a volume.** The USB device
  serial is read already (`espix_usb_dev_t.serial`) and printed by `blkid`, so
  reading it costs nothing. It is the one identifier that survives a whole-disk
  `dd` clone, where `LABEL=`, `UUID=` and `PARTUUID=` all travel with the copy.

  What is not done is the question underneath it, rather than the reading: **a
  serial identifies a disk, and a rule mounts a volume.** `SERIAL=X` is
  unambiguous only for a superfloppy, and any real rule has to say which partition
  of that disk -- which means either a composite field (`SERIAL=X/1`) or a second
  column. One is a new spelling to learn, the other a change to the file's shape,
  and it is worth deciding deliberately rather than by precedence.

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

- ~~**Remove `run`.**~~ Done. It was redundant, and only history explained it:
  there was no executable bit when it was written, so something had to say "this
  file is a program". The shell's exec fallback (`exec_fallback()` in
  `cmd_run.c`) does everything a Unix shell does with a word that is not a
  builtin -- resolves a name containing a slash as a path and anything else in
  `/bin`, checks the execute bit for 126 `Permission denied`, checks the ELF
  magic for `Exec format error`, honours a trailing `&`, and reports 127
  otherwise. `hello` and `/bin/hello` are how you run a program.

  The one thing `run` had that the fallback lacked, `-R`, became `confine`; see
  **A root for an app** under Filesystem for why it is not called `chroot`.

  The ELF magic check stayed, and the comment on `program_gate()` says why at
  length, because "the executable bit exists now, so the magic check is
  redundant" is a reasonable-sounding thing somebody will think later. Linux
  does the same two gates in the same order -- execve() checks the bit, then
  binfmt handlers read the first bytes and fail with ENOEXEC when none matches.
  It is also the seam where `#!` support hooks in.

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
- **Let a loaded app have real IRAM.** `IRAM_ATTR` in an app compiles, links,
  loads, runs and does nothing: espix's ELF loader has one allocator for every
  section and, with `CONFIG_ELF_LOADER_LOAD_PSRAM`, hands out PSRAM regardless
  of the `exec` flag it is passed. The whole app is in PSRAM.

  Since XIP from PSRAM this is much less serious than it was — the usual reason
  to mark a handler `IRAM_ATTR` is surviving the cache being disabled during a
  flash write, and the cache is no longer disabled. What is left is timing:
  instruction fetch from PSRAM is slower and jitterier than internal SRAM, so a
  handler with a sub-microsecond deadline can still miss it.

  It matters because of what espix wants to run. A portable C app never asks for
  this and should not — it knows only `malloc()`, and where its code lives is a
  build option of the system it lands on. But **Arduino sketches** use
  `attachInterrupt` with `IRAM_ATTR` handlers as a matter of course, and
  **stock IDF examples** are written the same way; espix already ships one of
  the former (`apps/neopixel`, whose WS2812 routine is marked and does not get
  it). Both build and load today, silently short of what they asked for.

  The shape for a fix is already in the loader: `esp_elf_malloc()` takes an
  `exec` flag and its non-PSRAM branch already uses `MALLOC_CAP_EXEC`. It needs
  a section slot for `.iram1.*` allocated `MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC`
  — and note the loader captures sections *by name* and silently skips the rest,
  so an app whose link preserves `.iram1` today has that code dropped rather
  than misplaced. It also needs an app-side link that keeps the section: the
  shipped neopixel ELF has no `.iram1` at all, its functions having been folded
  into `.text`.

  Two costs to weigh before doing it. Internal RAM is the same pool as the heap
  (see [GOTCHAS.md](GOTCHAS.md)), so every app that asks for IRAM takes it from
  the memory that decides how many SSH sessions fit. And a per-app IRAM budget
  needs a policy — an app asking for 64KB of it should be refused, not obeyed.

- **An app's `malloc()` should prefer PSRAM, the way espix's own allocations
  do.** Today it does not: an app calls the firmware's `malloc`, which uses
  IDF's default policy — internal for anything under
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16KB), external above it. So a dozen
  small allocations in an app come out of the 145K of internal RAM that decides
  how many SSH sessions fit, while eight megabytes sit unused.

  espix already makes the opposite choice for itself where it matters —
  `conn_alloc()` asks for `MALLOC_CAP_SPIRAM` and falls back to internal — and
  the ELF loader puts every app *section* in PSRAM. It is only the app's runtime
  allocations that go the other way.

  The mechanism is now in place: publish `malloc`, `calloc`, `realloc`, `free`
  and `strdup` through `abi_resolver.c` the way `getenv` is, backed by
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)` with an internal fallback. `free()`
  needs no override in principle — `heap_caps_free()` handles either region —
  but publishing it alongside keeps the pair legible.

  Three things to settle before doing it. A PSRAM buffer handed to DMA inherits
  the cache-alignment contract (see [GOTCHAS.md](GOTCHAS.md)), so an app doing
  its own DMA would need to know; small allocations from PSRAM are slower to
  reach than internal ones, which is why IDF's default is what it is; and a
  board with no PSRAM at all must fall back cleanly, which wants a build and a
  boot to prove rather than an argument. That last one is worth doing anyway:
  espix has never been built with `CONFIG_SPIRAM=n`, and XIP-from-PSRAM would
  have to come off with it.

- **Detect the flash chip and say which cache strategy the board can have.**
  espix ships `CONFIG_SPIRAM_XIP_FROM_PSRAM` because a flash write otherwise
  disables the cache and faults any crypto that is mid-`esp_cache_msync` — see
  [GOTCHAS.md](GOTCHAS.md). It costs ~1MB of PSRAM and 88ms of boot.

  `CONFIG_SPI_FLASH_AUTO_SUSPEND` is the better answer where it is available:
  the flash chip suspends an erase to serve a read, so the cache is never
  disabled, the code stays in flash and the PSRAM stays free. It is not
  available here, and whether it is available anywhere is a property of the
  board rather than of the chip family.

  ESP-IDF whitelists it **per flash chip ID**, in the `get_caps` of each
  `spi_flash_chip_*.c`:

  | driver | IDs claiming `SPI_FLASH_CHIP_CAP_SUSPEND` |
  |---|---|
  | GigaDevice | `0xC84016`, `0xC84017`, `0xC84018`, `0xC84319` |
  | Winbond | `0xEF4017` only |
  | everything else, Boya included | none |

  So a DevKitC-1 may or may not qualify: the ESP32-S3-WROOM-1 datasheet does not
  name the flash vendor and it varies by production batch. A 16MB GigaDevice is
  on the list; a 16MB Winbond is not, because only the 8MB `0xEF4017` appears.
  This board reports `0x68`, Boya, and IDF's driver says "flash-suspend is not
  supported" in a comment before omitting the flag.

  The shape of the work: a `tools/flash-caps.sh` that reads the ID off the
  attached board with `esptool flash-id`, matches it against IDF's own tables —
  *grepped from the SDK source rather than copied*, so it stays right as
  Espressif adds chips — and reports which strategy fits and which is
  configured.

  Two constraints, so the design is not re-derived later. It cannot be a Kconfig
  `depends on`: a build has to work with no board attached, which is how CI
  builds. And it should not prompt mid-build, for the same reason. A tool the
  developer runs, plus one line from `make flash` when the detected chip and the
  configured strategy disagree, is the shape that works.

- **OTA slots.** The partition table is `factory`-only. Two 4MB OTA slots plus
  `otadata` would cost ~4MB of the 11.9MB rootfs but allow kernel updates over
  the network. Changing this later means reflashing everything, so it is worth
  deciding before the layout is in the field. A commented-out variant is in
  [partitions/esp32s3-16mb.csv](../partitions/esp32s3-16mb.csv); the 8MB table
  notes why the same shape does not fit there.

- **USB host beyond storage.** Enumeration, identification, the partition table,
  superfloppy volumes and hotplug are done and shipped — see
  [USB-HOST.md](USB-HOST.md) — and what is left divides into things that are cheap
  and things that need a decision. Two of the original open questions are now
  answered: a PD hub does power the board *and* enumerate devices, and **two
  storage devices do not fit** — the S3 has a fixed pool of host channels and a
  hub plus one disk consumes almost all of them, so the second disk is refused
  while the first works. A keyboard costs two channels where a disk costs three.

  - **exFAT** is a patched dependency rather than a feature. `FF_FS_EXFAT` is
    hardcoded `0` in IDF's `components/fatfs/src/ffconf.h` with no Kconfig to
    change it, so it means carrying a patch the way
    `tools/patch-littlefs.py` carries one. It is also the one item here with a
    legal question attached: exFAT is covered by Microsoft patents, and FatFs's
    author has said a licence may be needed for commercial use. **That has not
    been verified against IDF or FatFs here** — no patent or licence text ships
    with the bundled FatFs — so it is a "check the terms first" item, not a
    "just enable it" item.
  - **lwext4** for ext2/3/4, and a much later `lwntfs`, are new components with
    the same shape as the filesystem work in [Filesystem](#filesystem). `lsblk`
    already names both as recognised and unsupported, which is the honest
    position until then — and names ext2/3/4 specifically, from the superblock,
    on a partition (`0x83`) and on a whole-device volume alike.
  - **GPT** wants a partition-table reader rather than a parser change: the
    protective MBR is reported today rather than followed.
  - **A USB keyboard** is the interesting one and needs no display and no serial
    port to test: SSH in over WiFi, print decoded keystrokes, and type. That
    sidesteps the hub-blocks-the-UART-socket problem entirely, which is the part
    of this that costs real time.
  - **Runtime role switching** (device ↔ host without a reflash) was rejected
    rather than deferred: it needs both stacks linked, which gives up the whole
    saving, and nothing has verified that the peripheral can be handed over
    cleanly at runtime.
  - **VBUS.** Whether a PD hub powers the board *and* enumerates devices is what
    [USB-HOST.md](USB-HOST.md) is there to find out. If power to the port turns
    out to need switching from the board, `boards/*.conf` is the only place
    per-board wiring can be expressed today and there is **no precedent in it**:
    those files carry flash size, PSRAM mode and a partition table, and nothing
    else.

## Networking and time

- **Routing and NAT: be the bridge people buy a Raspberry Pi for.** With WiFi on
  one side and USB-NCM or Ethernet on the other, espix is one feature short of
  being an access point, a bridge or a range extender — which is a large part of
  what a Pi gets bought and left plugged in behind a TV to do.

  The plumbing is already in lwIP and simply switched off: `LWIP_IP_FORWARD`,
  `LWIP_IPV4_NAPT` and `LWIP_IPV4_NAPT_PORTMAP` all exist in IDF 6.1 and are all
  `n`. So this is not a stack to write; it is a Kconfig flip plus the policy and
  the commands around it — somewhere to say which interfaces forward (a
  `sysctl`-shaped `net.ipv4.ip_forward`, since that is the name everyone already
  knows), and something `iptables`-shaped for masquerading and port forwards.

  Two things to know before starting. Forwarding on a device with 300KB of
  internal RAM is bounded by lwIP's pbuf pool long before it is bounded by the
  CPU, so this wants measuring rather than assuming. And an AP on the WiFi side
  means `ESPIX_IF_WIFI_AP`, which is in the interface-kind enum and has never
  been used — `CONFIG_LWIP_DHCPS` is already on, so the DHCP server that
  USB-NCM's server mode uses is the same one an AP would.

  The S31 and P4 are on the hardware list partly for this: more RAM on one, and
  a second real Ethernet MAC on the other.

- **ESP-NOW, as a device rather than an ABI.** espix publishes lwIP sockets and
  the resolver to apps and nothing else, so the radio is unreachable from an
  app: no `esp_wifi_*`, no `esp_now_*`. USB-NCM makes that worth fixing, because
  a board whose uplink is the cable has a radio doing nothing — and ESP-NOW
  peers must sit on the station's channel while it is associated, so such a
  board can choose its own channel instead of inheriting the access point's.

  **Expose it as `/dev/espnow`**, not as exported symbols. An app opens it,
  writes a frame, reads what arrives; peers are configuration rather than API
  calls. That is what a Unix does with a radio, it keeps the ABI from growing an
  `esp_now_*`-shaped hole that every future app links against, and it makes the
  permission story fall out of the file mode instead of needing one of its own.

  It would be espix's **first device driver of its own** -- `/dev/uart` is live
  in this build but it is ESP-IDF's. An espix `s_nodes[]` entry is now listed by
  `ls /dev` and typed by `ls -l`, so a node for this would be visible as soon as
  it is added; only ESP-IDF's own `/dev/uart` mount remains unlistable (see
  [KNOWN-ISSUES.md](KNOWN-ISSUES.md)). The mode comes from the table, so
  `chmod` on it is refused by design rather than being a way to change it.

  Related to **Per-app export tables**: handing every loaded app a radio is
  exactly the thing that item exists to stop, and a device node is how the
  answer gets to be "chmod it" rather than "maintain a second symbol table".

- **WiFi power save, decided rather than inherited.** espix never calls
  `esp_wifi_set_ps()`, so it runs IDF's default of `WIFI_PS_MIN_MODEM`: the
  station sleeps and wakes to hear a beacon once per DTIM period. That is the
  right default for a sensor that speaks once a minute and the wrong one for a
  device you hold an SSH session to, because every exchange can wait on the next
  beacon -- typically 100-300ms depending on what the access point advertises.

  Not the PHY rate, which was the first guess: that is rate-adaptive already,
  and AMPDU is on in both directions with a 16-frame block-ack window. The sleep
  schedule is the part nobody chose.

  **Measured, and it is not the problem. 2026-09-08.** The dynamic hold below was
  built and instrumented -- `WIFI_PS_NONE` held from the first SSH session to the
  last, off the existing session refcount -- and it changed nothing that could be
  measured:

  | | power save default | held off while connected |
  |---|---|---|
  | login, total | ~3.5s | ~3.5s |
  | ...of which key exchange | 1037 ms | 1031 ms |
  | scp upload / download | 815 / 720 KB/s | 807 / 674 KB/s |
  | ssh stdin | 236 KB/s | 237 KB/s |
  | ping RTT, 3s gaps | 55.9 ms avg | 62.1 ms avg |

  The ping A/B is the direct test and needed care to get right: pinging every
  0.5s keeps the radio awake in *both* arms, so the first attempt compared
  nothing. Re-run with three-second gaps -- long enough to sleep between -- the
  two arms produce the same descending 89/54/21 ms sawtooth, which is the access
  point's behaviour and not the station's sleep schedule.

  So the 100-300ms beacon penalty reasoned about below is real in principle and
  absent on this link. The login is slow for an entirely different reason:
  **PBKDF2 costs 2030 ms of pure CPU**, against the "~100ms" its own comment
  claims. See the auth entry.

  Kept open rather than closed, because "no benefit here" is not "no benefit" --
  a different access point with a longer DTIM could still show it. But it is no
  longer a latency fix, and holding power save off costs energy for nothing, so
  it should not be adopted without measuring on the link in question.

  What made it look interesting is that neither answer is right all the time, so
  it wants to be dynamic: `WIFI_PS_NONE` while a session or transfer is live, and
  back to `MIN_MODEM` when the device is idle. espix already knows when that is
  -- the SSH server tracks its sessions and `espix_proc` its processes -- so the
  policy has somewhere to live, and the shape is the same one a laptop uses when
  it stops power-saving on the interface you are actually using.

  Worth measuring rather than assuming, and the measurement now exists: the same
  `testapp out 5000` run over WiFi and over USB-NCM, where USB is the control
  because it has no radio and no sleep schedule. 347 lines/s against 389 today.

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

- **Pipes, `<` redirection, and stdin for builtins.** The three streams
  themselves are done: `espix_eprintf()` sits beside `espix_printf()`, `2>`,
  `2>>` and `2>&1` work, SSH carries diagnostics as `CHANNEL_EXTENDED_DATA`,
  and a loaded app gets a real `stdin`, `stdout` and `stderr` — see
  `tests/suites/15-streams.sh`.

  What is missing is the plumbing between commands. A builtin cannot read
  standard input, and there is no `<`; both are cheap on their own and neither
  is worth much without the other, because with no pipes there is nothing for
  a builtin to read *from*. So they go together, and `|` is the one that makes
  them pay: it needs a command's output to become another's input, which means
  a pipe object with two ends and a lifetime that outlives neither.

  Two known constraints from the stream work. `chan_poll_interrupt()` is the
  only consumer of the SSH channel's receive buffer and `chan_pump()`
  overwrites that buffer rather than appending, so a second reader needs it to
  become a ring first — which is also what a backgrounded process would need
  to read stdin at all. And the shell's redirect `FILE` is owned by the
  command, not the process, which is why only a foreground app can be pointed
  at one; pipes will want that ownership reference-counted.

  Job control wants the same objects, so the two are worth designing together.

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

  The keyboard half has somewhere to land now: USB host mode already enumerates
  devices and identifies them ([USB-HOST.md](USB-HOST.md)), so what it needs is a
  HID class driver beside the mass-storage one, not a second stack or a second
  role.

- **A minimal 2D desktop environment**: a filesystem browser, a JPEG viewer, an
  audio player for whatever formats decode cheaply, and video on the P4, which
  has the hardware for it. These are *apps*, not kernel work -- which is the
  point of the ELF loader and the export table, and the best argument for
  keeping that boundary honest. The gap between here and there is mostly a
  graphics stack and a windowing model, neither of which espix has any business
  inventing.

These arrived with the project and predate almost everything in this file; they
are recorded here rather than in a separate note so there is one place to look.
