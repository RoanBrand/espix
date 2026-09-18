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
| `mmap()`, `statvfs()` | absent | the VFS |
| `utime()` | **done**, and asserted from an app | espix's VFS fills `utime_p` and the lower port's Kconfig has it on; `mmap`/`statvfs` above are the rest of that row |
| `/dev/<device>` opened as a file | `EOPNOTSUPP` (a name, not a stream) | raw block I/O as its own feature |
| a volume whose device was pulled | `ENOSYS` from every operation | deliberate; `EIO` would need a refusing helper per op |
| `chmod`/`chown` on metadata-less FAT | refused | deliberate: there is nowhere to store it. `-o fmode=`/`dmode=` would *declare* a mode rather than store one — the item below |
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

- **Mode-shaped mount options, the other half of what the rule does.**
  `mount -o uid=,gid=` already answers *who owns this volume* for a filesystem that
  keeps no owner of its own, which is what Linux's vfat driver takes `uid=`, `gid=`
  and `umask=` for. espix has the first two and none of the mode ones, so on a FAT
  or exFAT volume there is no way to say "what is here is executable": the rule
  gives `0644` to anything that is not an ELF, `chmod` is refused because there is
  nowhere to store it, and a shell script on a stick therefore cannot be run at all.
  `-o fmode=` and `-o dmode=` — with `umask=` being the same thing written the other
  way round — is a *declaration* rather than a store, and it slots in as tier 2 of
  the precedence in
  [ARCHITECTURE.md](ARCHITECTURE.md#modes-and-owners-the-filesystem-first-then-the-mount-then-a-rule):
  below a stored attribute, above the rule, which is where a mount's answer belongs.

  Small and self-contained — option parsing plus fields on the mount record, and the
  mode path already consults the mount for its owner. Worth doing *with* the ext
  ownership decision rather than before it, because both are answering one question,
  *who is asked for metadata, and in what order*, and doing them together is one
  change to `mode.c` instead of two.

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

### The app ABI: what an app may name, and who answers for it

An app resolves its undefined symbols at load time against tables built into the
firmware, so a name is reachable only if somebody typed it into one. There is no
directory of the image's own symbols and no pass-through: a function the firmware
calls a thousand times an hour is invisible to an app until it is listed. That is
what makes these tables the sandbox on a chip with no MMU — an app shares the
address space and can call only what it can name — and it makes every entry a
decision with a reason rather than a convenience.

| an entry is | when | what it is for |
|---|---|---|
| **published** | the call reaches espix — its VFS, its fd table, its signal delivery — or reaches nothing but the caller's memory | libc's `open()`, `stat()` and `read()`, which arrive in espix's VFS on the way in, so the permission check is not something they can skip |
| **overridden** | the libc or IDF implementation lies, bypasses espix, or has to become a delivery point | `chdir`, `getcwd` and `chmod` under libc's names because IDF's are stubs; `sleep` and `usleep` so a signal can cut them short |
| **left out** | espix cannot answer it, or cannot answer it truthfully | `access`, `lstat`, `isatty`, `dup`, `dup2`, `atexit`, `perror` — each with the reason written beside the table it is absent from |
| **carried elsewhere** | the surface is large, or belongs to a runtime rather than to the kernel | an app's own libraries, or a loadable module: see the Arduino item under **Further out** |

The layering matters as much as the list. elf_loader's own tables answer for 62
standard names *before* espix's are consulted, so those need no entry — and where
an entry does duplicate one, it resolves anyway and is dead weight that reads as a
promise. Both cases are reported on every link now rather than left to be
discovered, which is what caught `memset`, `strtol` and `ets_printf` sitting
unreachable in a driver table. A table cannot shadow what is answered below it;
that is what the resolver is for, and it runs first.

`components/espix_proc/abi_libc.c` carries the full statement, the list of what is
answered below, and why none of this can be a `_Static_assert`; `tools/check-abi.py`
enforces it; `tests/suites/12-vfs.sh` calls the published names from an app, and
checks that an app is refused what the shell is refused.

### ext2/3/4, via a port rather than a library

**The read-only milestone is built.** `components/espix_fs/ext.c` mounts ext2,
ext3 and ext4 through espix's VFS with `ESPIX_FS_EXT4` on: `mount`, `umount`,
`df` and the whole path-based shell reach it with no changes of their own, and
`lsblk` no longer marks the type unsupported. Measured cost, this option `n`
against the same tree with it `y`: **+119,860 bytes of image, +10,432 of `.bss`,
and no IRAM at all** — the number that matters most, since IRAM is already at its
whole 16KB. Three limits are real and written up in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md): a listing cannot tell its end from a failure,
a pulled device leaves lwext4's mount point behind, and a read-only mount is owned
by whoever mounted it rather than by its inodes. Writes are the next milestone, and
they are **staged below** — volumes without extents first, extents once the error
path has been tested — because what gates them is a one-line patch and an ownership
decision rather than a pile of missing code.

**The one thing that had to be fixed in lwext4 was upstream's, and now is.** A
volume made by e2fsprogs 1.47 or later enables `metadata_csum_seed`, which sets an
incompatible bit that lwext4 refused — correctly, since it seeded every metadata
checksum from the UUID while such a volume's are seeded from the superblock. The
copy the manager fetches now honours the seed instead: one helper and eight call
sites, applied by `tools/patch-lwext4.py` from `tools/lwext4-csum-seed.patch` —
the same change, ready to send to `gkostka/lwext4`, which is where it belongs.
Verified against a stock `mkfs.ext4` volume; [KNOWN-ISSUES.md](KNOWN-ISSUES.md)
has the measurements.

What asked for it: `lsblk` already named ext from the superblock, on a partition
(`0x83`) and on a whole-device volume alike — and it now names the version, `ext4`
or `ext2`/`ext3` by the feature words — while a Linux-formatted stick was the one
common volume that stayed unreadable.

**Use `huming2207/esp_lwext4`, not `gkostka/lwext4` directly.** The port is what
lwext4-on-ESP-IDF needs and what espix would otherwise write: a *generated*
`ext4_config.h` (upstream's own seam — `#if !CONFIG_USE_DEFAULT_CFG` → a
`generated/` header, so **nothing upstream is edited**), an adapter over
`esp_blockdev`, Kconfig-to-macro mapping, and a malloc layer with internal-SRAM /
PSRAM / prefer-PSRAM modes, which suits this tree's IRAM-first rule.
`espix_fs_partition_view()` already returns an `esp_blockdev_handle_t`, so the
block device is a direct fit rather than an adaptation.

**A git dependency, not a vendored copy.** esp_lwext4 publishes no
`idf_component.yml`, so it cannot come from the registry — but that does not mean
it has to be committed. `main/idf_component.yml` names it by repository and
revision, the manager resolves it into the same gitignored `managed_components/`
as every other dependency, and `dependencies.lock` pins the resolved SHA and a
content hash.

What stood here before was that a `git:` dependency would resolve to a component
with an empty `lwext4/`, because the core is a submodule. That is wrong for the
manager IDF 6.1 ships (3.1.2): its git source passes `with_submodules=True`
unconditionally, and a component with no manifest gets no gitignore filtering at
all, so the copy is the whole tree. Measured rather than assumed — the fetched
`lwext4/` holds 91 files, and `diff -r` against upstream says the port and the
core are byte-identical to the revisions pinned.

Two small things the copy does that are worth knowing. The submodule's `.git`
gitlink comes along, dangling and harmless — and it is why espix's patch is plain
text rather than `git apply`. And upstream's `test_apps/` does **not**, because
everything in it lives under `build_smoke/` and the manager's default excludes
drop `**/build_*/**/*` as a build artefact.

So **no vendored tree and no submodule of our own**: one manifest entry, one
patch script, and the row in `dependencies.lock` that comes with them.

**It also settles the licence**, which is the reason to prefer it to raw lwext4.
lwext4 is GPLv2 with exactly two GPL files (`ext4_xattr.c`, `ext4_extents.c`);
the port's default build compiles an **MIT** extents implementation and an xattr
stub instead, keeping both out and the firmware BSD-3/MIT. Selecting upstream
extents instead makes the binary GPL-2.0 — a fork in the road, and a bigger
decision than exFAT's patent question. Worth knowing that the MIT extent code is
the port's own "experimental" replacement, though he has verified depth-2 extent
trees against `e2fsck` and `debugfs`.

**Three things his `doc/CAVEATS.md` settles in advance**, each of which would
otherwise have been a surprise:

- **ESP-IDF's SDMMC block device truncates byte addresses to `size_t`**, wrapping
  media over 4GB silently and on reads as well as writes. Not espix's path —
  storage here is USB MSC, which is `uint64_t` throughout — but it is the third
  instance of that family in this tree, so it is written up in
  [UPSTREAM.md](UPSTREAM.md#a-partition-view-cannot-be-larger-than-4gb). His own
  instruction is the useful part: *"other lower BDL implementations must be
  audited separately"*, which is exactly what `part.c` already is.
- **Mutation requires journaling.** Allocation, tree splitting and extent removal
  return `ENOTSUP` unless journaling is compiled in *and* the mounted filesystem
  has an active transaction.
- **The pinned lwext4 core has an unfixed error-propagation bug**:
  `ext4_fwrite()` assigns the result of `ext4_fs_put_inode_ref()` to `r` at its
  `Finish` label, **overwriting an earlier allocation error** before it chooses
  between abort and commit. Passing `e2fsck` is therefore not evidence that an
  injected I/O failure rolls back safely — and that matters here more than
  anywhere, because the T9 this tree was developed against
  ([KNOWN-ISSUES.md](KNOWN-ISSUES.md)) returns wrong bytes on a repeated read.
  **A library with an unverified error path writing to that drive is the
  combination to avoid**, which is why the first milestone is read-only.

**What it took:** `components/espix_fs/ext.c`, a shim over `ext4_*` in the shape
of `fat.c`; the mount wiring (`mountable()`, `mount_by_type()`, the mount record,
fstab, `ESPIX_FS_EXT4`, and dropping `foreign` from the ext row); and `df`
onto lwext4's own `ext4_mount_point_stats()`, which was plumbing rather than new
capability. The genuinely new piece was an **fd table**: FatFs's glue handed espix
a file table and an `int` fd, where lwext4 has neither because the caller owns the
`ext4_file` — so espix allocates, bounds and recycles handles itself. Still
outstanding: a superblock reader for `blkid`'s label and UUID — FAT-only until
this, so an ext volume reported neither, and now both come out of the superblock
the type probe already reads. exFAT's label is read too, by following its own
geometry to the root directory, where exFAT keeps it.

Two things are easier than they looked, now that the headers have been read
rather than guessed at:

- **Read-only mounting is native.** `ext4_mount(dev_name, mount_point, bool
  read_only)` takes it as an argument, so none of the exFAT work is needed here:
  no view to mark read-only, no riding `STA_PROTECT` through the status callback.
- **`ext4_fseek()` takes an `int64_t` offset**, so seeking past 4 GiB is
  something the filesystem can already do. The ceiling is espix's `off_t` alone,
  which makes the fix for it purely a matter of espix's own types.

**The component builds against IDF 6.1 unmodified** — verified, not assumed: the
vendored tree, its `ExternalProject`, the generated `ext4_config.h` and the
`esp_blockdev` adapter all compile and link as they ship, with no source edits.
Everything it needs (`esp_blockdev`, `esp_blockdev_util`, `sdmmc`) exists in 6.1.
The size cost is the measurement in the opening paragraph: +119,860 bytes of
image, and nothing in IRAM.

**What it would find espix is missing** — the second reason to do it, after the
filesystem itself, because these are model questions rather than additions:

- **Real ownership and modes.** ext4 keeps uid/gid/mode in the inode. espix's
  model is a rule plus a side attribute (`mode.c`) for filesystems that keep none,
  so `espix_fs_owner()`, `chmod` and `chown` have to answer *who is asked first* —
  and **that is settled rather than open**: the filesystem is asked, then the mount,
  then the rule, which `espix_fs_meta_t` records per mount and
  [ARCHITECTURE.md](ARCHITECTURE.md#modes-and-owners-the-filesystem-first-then-the-mount-then-a-rule)
  writes up. An ext volume enforces its own permissions, `-o uid=`/`gid=` are
  ignored for it as they are on Linux, and `stat`, the check and `ls -l` read one
  source. What is left is the *writing* half, because a read-only mount cannot be
  `chmod`'d however it keeps metadata: that is milestone 1 of the writes plan above.
- **Inode numbers exist**, so `ls -i` becomes possible — littlefs cannot, and
  [UPSTREAM.md](UPSTREAM.md) has the reason under *`readdir()` reports no inode*.
- **Hardlinks** (lwext4 supports them) want a `link()` espix has no counterpart
  for.
- **Symlinks** want `symlink()`/`readlink()` plus resolution in the VFS. There is
  no symlink handling anywhere in the tree today.
- **`statfs`/`statvfs`** as a *generic* surface. The capability is already in
  lwext4 (`ext4_mount_point_stats()`); what espix lacks is a filesystem-agnostic
  place to ask, since `df` reaches for `espix_fs_stat_fat()` by name.
- ~~**The 32-bit `off_t` stops being theoretical.**~~ **Done.** ext4 volumes hold
  files over 4 GiB as a matter of course and `st_size` truncated, so the documented
  ceiling arrived as a practical blocker — and it was fixed where it belonged,
  in the type: `cmake/offt64.h`, a force-include, so the type itself is never
  patched, and the apps get it too because `struct stat` is an ABI between them and
  the kernel. [UPSTREAM.md](UPSTREAM.md#off_t-is-32-bits-and-no-kconfig-changes-it)
  has the mechanism, the `_lseek_r` declarations that turning `off_t` into an
  explicit ABI surfaced, the cost, and the one thing it does not fix — SFTP's
  transfer path, which is in [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

**Writes, staged, and why in that order.** Two milestones, because the risky half
is separable from the useful half — and the useful half needs nothing experimental.

*Milestone 1: the driver, which is built.* `mount -o rw` gives a writable ext
volume: the adapter is configured writable, `ext4_mount()` is told, and **the journal
is started by espix** (`ext4_mount()` does not do it, and every mutation answers
`ENOTSUP` until something does — fail-closed, which is the right direction to find
that out in). A volume with no journal is refused rather than mounted read-only
behind the caller's back. The ops are `ext4_fwrite` (with `ext4_fseek` behind
`pwrite`), `ext4_dir_mk`, `ext4_dir_rm`, `ext4_fremove`, `ext4_frename`,
`ext4_ftruncate`, `ext4_atime_set`/`ext4_mtime_set`, and the create path sets a
mode, because lwext4's create leaves a new inode at 0666 — every file written to a
stick would otherwise be world-writable.

- the BDL adapter configured writable: `read_only = false`,
  `sync_after_write = true`, and `lower_device_supports_rewrite = true` — true of
  USB MSC, and the port warns against setting it for raw erase-before-write flash.
- `ext4_mount(..., read_only = false)` and then **`ext4_journal_start()`**, which is
  also where journal recovery happens. Verified rather than assumed: `ext4_mount()`
  does *not* start it, so a mount that forgets this has every mutation answered
  `ENOTSUP` — fail-closed, which is the right direction to find it in.
- the ops, each already registered and returning `EROFS` today: `ext4_fwrite` (with
  `ext4_fseek` behind `pwrite`), `ext4_dir_mk`, `ext4_dir_rm`, `ext4_fremove`,
  `ext4_frename`, `ext4_ftruncate`.
- `ext_open` stops refusing `O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC` and passes the
  flags to `ext4_fopen2`, which already takes them. What mode a *created* file gets
  is worth checking rather than assuming: the create path takes no mode argument.
- `ext_fsync` becomes real — `ext4_cache_flush()` then the adapter's sync — instead
  of returning 0, and `close` commits a dirty handle.
- `ext4_journal_stop()` before `ext4_umount()` on the way out, while the dead-device
  branch keeps skipping all of it: a pulled device must not be written to.

**Built and exercised on a real volume.** A 7 GiB ext4 partition made by KDE
Partition Manager, extents and all: `mount -o rw` gives a writable mount; a copied
file arrives at `0644` rather than lwext4's `0666`; `mkdir` gives `0755`; `mv`,
`rm`, `rmdir` and a rewrite through `O_TRUNC` (3 KB down to 15 bytes) all work; the
data survives a clean unmount and a read-only remount. `chmod` and `chown` write
the inode through a setter the mount hands the VFS (`espix_fs_meta_ops_t`), so
`chmod 0755` gives `-rwxr-xr-x` and a changed group lands; a read-only mount
refuses, and the rootfs's own path is untouched. 10-fs, 12-vfs and 75-usb are
green with the stick attached.

`ext_truncate()` and `ext_ftruncate()` were the last two ops with nothing calling
them, `cp` reaching truncation through `O_TRUNC` at open; the testapp's new
`truncate` command calls both in turn, and 10-fs asserts that the inode ends up
at the length each one named. What is still unexercised is the failure path: the
injection's count lands in the journal's own writes rather than in
`ext4_fwrite()`, and the A/B that would prove the patch needs the unfixed core
built alongside it.

`utime`, `chmod` and `chown` are not blocked on lwext4: `ext4_mtime_set()`,
`ext4_mode_set()` and `ext4_owner_set()` each take a path, so the three calls that
refuse on an ext mount today have an implementation waiting for a writable mount
rather than an API to be added.

The gate that makes milestone 1 safe is a **volume** question, not a type one.
`type_always_read_only()` in `cmd_blk.c` currently answers for every ext type,
because the driver could only mount read-only; once it can write, the answer has to
come from the superblock — `incompat & EXT4_FINCOM_EXTENTS`, in the word the type
probe already reads. A volume with extents mounts read-only and says why; one
without is writable, and `mount -o rw` / `ro` already parse. The **default stays
read-only**, for the reason `ext.c` opens with: a volume mounted here may be
somebody's only copy of something.

*Milestone 2: extents* — where ext4 volumes made by anything current land. The
port's own list applies in full: mutation requires an active journal transaction
(which is milestone 1's work, done first), an unwritten extent returns `ENOTSUP` on
a create/write lookup, and *"these checks do not qualify the implementation for
arbitrary power loss, injected I/O failure, hostile-image fuzzing, or all
Linux-created extent layouts."*

**The error-propagation gate is a patch, not a blocker.** The defect is present in
the pinned core, at `lwext4/src/ext4.c:178`:

    Finish:
        r = ext4_fs_put_inode_ref(&ref);   /* clobbers the data/allocation error */
        if (r != EOK) ... abort, or commit, on the wrong value

One line and one variable: the abort/commit decision has to see the error that
actually happened. espix already carries five patches with the same discipline —
applied by a script on every configure, written to be sent upstream — so this is the
sixth, and it belongs upstream rather than here.

What a patch cannot settle is the rest of that sentence: passing `e2fsck` is not
evidence that an injected write failure rolls back. That is testable *here* in a way
it was not for the port's author, because espix owns the BDL wrapper: an adapter
that fails the Nth write on demand exercises the abort path directly, and the host
closes the same loop the port used —

    write on the device → umount → attach the stick to a PC → e2fsck -f -n

which is also the only way to check a journal replay after a pulled device. Both
need the stick attached, so they are tests for an OTG run rather than the wireless
suite.

**The ownership half of this is done, and it was the smaller half of the work.**
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) recorded that an ext mount was owned by whoever
mounted it while its `stat` reported the inode's values; the precedence now asks the
filesystem first (`ESPIX_FS_META_LOWER`), so an ext volume's own modes and owners are
what the check enforces, `-o uid=`/`gid=` are ignored for it as they are on Linux,
and `stat`, the check and `ls -l` read one source. What is left is the *writing*
half: `chmod` and `chown` refuse on ext because changing an inode needs a writable
mount, which is milestone 1 above rather than a design question.

One consequence to write down as well: the stdio seek limit now reaches *writes*.
`cp` truncates and streams, so copying in is unaffected, but the append path seeks —
so appending past 4 GiB is the same 32-bit limit as an SFTP transfer, and has the
same fix, moving those paths onto descriptors.

**Effort.** Done, for the read-only milestone. The shim and the fd table were the
bulk of it, `df` was plumbing onto `ext4_mount_point_stats()` rather than new
capability, and the vendored build was already solved — what the estimate is
still owed is the hardware testing. Writes are staged above rather than estimated
here, and for a reason: the driver work is the same shape as what the read-only
milestone already built, while what actually gates the first one is the ownership
decision and a one-line patch rather than missing code. A follow-on that fits the
port's own list — its *accidental power-failure* test is unchecked — is worth
contributing upstream rather than carrying, on the same reasoning that
`tools/patch-*.py` exists to be deleted.

Two smaller things to watch: the port's `ExternalProject` uses `BUILD_ALWAYS
TRUE`, so lwext4 recompiles every build, and its component `REQUIRES ... sdmmc`
because it ships an SDMMC adapter, where espix is USB MSC. Neither is a blocker;
both are the sort of thing that is cheaper to know now.

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
- **A session's memory — the ceiling that is arriving.** The sessions suite fills
  the connection limit and now finds the internal heap at or near zero: allocations
  fail, a session is dropped rather than refused, and the suite reports that
  instead of a limit. Not new — the low-water mark has been falling for a while,
  and the ext4 driver's 10 KB of `.bss` is only the latest bite (the figures are in
  `ESPIX_FS_EXT4`'s Kconfig help) — but it is now below the suite's own floor of
  20 KB free at eight connections.

  Which memory matters, because the fix differs. This is **internal DRAM**, not
  IRAM: IRAM is instruction memory and is already at its whole 16 KB, while what a
  session costs is stack and buffers from the internal heap — the figure `ps`
  reports. So the work is per-session: what a connection's task really needs, what
  is sized for a worst case that never happens, and what could live in PSRAM
  instead, with the usual exception for anything DMA touches or that sits on a
  latency-sensitive path. `CONFIG_ESPIX_SSH_MAX_SESSIONS` is the other lever, and a
  blunt one.

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

  - **exFAT** is a patched dependency rather than a feature, and it is done.
    `FF_FS_EXFAT` is hardcoded `0` in IDF's `components/fatfs/src/ffconf.h` with
    no Kconfig to change it, so it means carrying a patch the way
    `tools/patch-littlefs.py` carries one — `tools/patch-fatfs.py` behind
    `CONFIG_ESPIX_FS_EXFAT` (default `n`). Three more edits came with it, and
    none was optional: `FF_LBA64` (hardcoded `0` beside it, and without it
    `ff.c` refuses any volume past 2TiB — which is the whole point), the
    `uint32_t` sector in `ff_diskio_impl_t` (a silent truncation of every LBA
    FatFs passes above 2^32, now `DISKIO_SECT` so the mismatch is a build error),
    and `diskio_bdl.c` with it. Measured at +6,972 bytes of ROM and no static
    RAM; see [USB-HOST.md](USB-HOST.md#roadmap-not-now) for what each edit does
    and what is deliberately left alone. It is also the one item here with a
    legal question attached: exFAT is covered by Microsoft patents, and FatFs's
    author has said a licence may be needed for commercial use. **That has not
    been verified against IDF or FatFs here** — no patent or licence text ships
    with the bundled FatFs — so it is a "check the terms first" item, not a
    "just enable it" item.
  - **lwext4** for ext2/3/4, and a much later `lwntfs`, are new components with
    the same shape as the filesystem work in [Filesystem](#filesystem). `lsblk`
    already names both as recognised and unsupported, which is the honest
    position until then — and names ext2/3/4 specifically, from the superblock,
    on a partition (`0x83`) and on a whole-device volume alike. NTFS stays in
    that column even with the exFAT option on: FatFs reads exFAT, not NTFS.
    **ext2/3/4 is now planned and costed** rather than named: a third-party
    ESP-IDF port of lwext4 instead of the library itself, vendored and pinned,
    read-only first, and with a licence fork to decide — see
    [ext2/3/4, via a port rather than a library](#ext234-via-a-port-rather-than-a-library).
  - **GPT** is done, and arrived exactly as the line above predicted: a reader
    rather than a parser change, in `host.c` beside the MBR walk. What asked for
    it was a 4TB Samsung T9 — three partitions behind a protective MBR, one
    unreadable row before. What is left of it is small: an entry's UTF-16
    partition name, and the disk's own GUID, neither of which is printed
    today — plus one real oddity, not yet chased down: reading the same
    entry-array LBA twice in one `gpt_read()` call, once inside a clean
    whole-array checksum sweep and once during the interleaved entry walk,
    gets two different answers on the T9, moments apart. All three
    partitions still list correctly either way, so it costs nothing today;
    see [USB-HOST.md](USB-HOST.md#roadmap-not-now) for what was checked and
    what the fix would be.
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

- **Arduino sketches and ESP-IDF examples, as apps.** Both platforms have an
  audience espix does not, and neither needs a new execution model to serve them:
  an app is a cross-compiled ELF, so a sketch can simply *be* one. There is a
  working experiment — `apps/neopixel`, a 112-line sketch beside a shim
  (`main/espix_sketch.{h,cpp}` and `ctors.ld`) that supplies the `main()` espix
  calls, runs the global constructors the loader does not, makes Arduino's
  `delay()` a cancellation point through `--wrap`, and runs a named `teardown()`
  when a signal or Ctrl-C stops the app. Arduino is a dependency of *that app* and
  not of espix: the firmware stays Arduino-free, and that boundary is the thing to
  keep.

  **The cost worth attacking is that every sketch carries its own copy of the
  runtime.** `elf_loader` ships a dynamic-module API — `dlmod_relocate`,
  `dlmod_insert`, `dlmod_getaddr`, `dlmod_listname` — which espix uses none of, and
  `dlmod_getaddr()` is already the last step of the symbol lookup. So a module
  loaded once is resolvable by every app, with no kernel change at all: that is the
  "Arduino as a runtime environment" idea, already underneath. The gate is memory
  rather than API. The loader relocates a module into RAM exactly as it does an
  app, so a shared runtime would pin its resident size for the system's lifetime,
  where today it is paid per run and freed at exit. That has to be measured on a
  real sketch before anything is built on it — neopixel's ELF is 20 KB, most of it
  the Arduino core — and if it does not pay, the per-app copy is what already
  works.

  **How an app says it wants a runtime should not be a guess.** espix has no
  business sniffing a binary for `setup`/`loop`, or a name for `.ino`: the same
  mechanism has to serve IDF apps and plain POSIX ones, and a heuristic would be
  wrong in exactly the cases that matter. Two honest mechanisms, and they compose:
  *declare* it, the way an ELF declares its dependencies, so the loader loads the
  runtime before the app; or *reference* it, and let the miss trigger the load —
  the resolver hook already takes a symbol name and returns an address, so
  on-demand is a few lines inside something espix owns. Either way
  `Serial.println()` stops being `printf` with a comment explaining why, and
  becomes a global object the runtime provides, writing to the app's stdout, with
  `available()`/`read()` reading its stdin.

  **The Arduino IDE is a packaging job, and a pleasant one.** A board package is
  `boards.txt` plus `platform.txt`, and the upload step may be any tool — so
  "flash" becomes build, `scp` the ELF into `/bin`, add a unit, restart. That is
  the deployment espix already has, and the unit is the init and service manager
  item under **Processes**, which is what makes a deployed sketch outlive the login
  that started it. Two things to settle first: whether the sketch is built by the
  Arduino toolchain or by espix's, and how a target address is entered at all,
  the IDE's upload port being a serial idea.

  **ESP-IDF examples are a smaller audience and a larger surface**, and the reason
  is worth stating plainly: Arduino is a self-contained C++ core with one library
  pattern, while an IDF example assumes `app_main`, FreeRTOS tasks, NVS and
  stateful drivers. A symbol table can satisfy a stateless call and nothing else —
  `esp_timer` and `gpio` already are — so the honest staging is POSIX-shaped IDF
  calls first, which the ABI work above makes cheap, and the IDF application model
  not at all until something actually needs it.

These arrived with the project and predate almost everything in this file; they
are recorded here rather than in a separate note so there is one place to look.
