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

- **Reap a faulted task and keep running.** `espix_fault_request_reap()` is
  defined and has no callers, and `CONFIG_ESPIX_FAULT_REAP` is off — the reaper
  task and its queue exist, but nothing feeds them. Skipping the reboot is the
  easy half. The hard half is the comment block at the top of `reaper.c`: locks
  held by the dead task, and the fact that without an MMU most corruption never
  reaches the fault handler at all. Shipping the easy half alone produces a
  system that limps rather than one that recovers.

- **Kernel services as processes, so one table can stop them all.** espix runs
  things on tasks of their own in two ways today: an app, which is a process
  with a pid, a state and an exit status, and a *builtin unit*, which is a
  `svc_unit_t` holding a bare `task` handle and a cooperative `stop` flag. The
  second is invisible to everything that makes the first useful: `kill` cannot
  reach it, the shutdown sequence stops it by a separate path, and
  `units_load()` cannot stop one at all when its line is removed -- it signals
  only units that have a pid, so deleting an `nfsd` line leaves the daemon
  running untracked, which is how it was noticed (the reload logged "cannot
  bind 111, 20048 or 2049 (already running?)").

  The table was built for both already: `espix_proc_info_t.image_bytes` is zero
  for a kernel task, and the ELF image and the private arena are optional
  fields. `ps` even lists kernel tasks today -- it walks the FreeRTOS task
  list and prints `-` for their pid. What is missing is a way for a task the
  kernel created to *join* the table, and a `kind` in the record so teardown
  can differ: an app's image and arena are freed, a service's stack belongs to
  whoever made it. Then `svc_unit_t` keeps only policy -- restart and schedule
  -- keyed by pid, `espix_svc_stopping()` becomes `espix_proc_stopping()`, the
  shutdown sequence's UNITS and APPS phases become one, and `kill nfsd` works
  because `service stop nfsd` is the same call underneath.

  They get a pid from the same allocator as an app's, in the same space and
  never reused -- that is what makes the rest work, and it is how the table
  already behaves: a pid is never recycled, so `pid > 0` is the test for an
  entry being live. A `kind` in the record tells them apart, not the number; a
  reserved range would be a second namespace to keep in step. Their parent
  stays `ESPIX_PID_NONE`, which the header already defines as "espix started it
  itself": the kernel is the parent and the kernel has no pid, so there is
  nothing to invent.

  A pid is identity and not authority: it is a name `kill` and `wait` can
  reach, and the same rule has to survive the move -- a service with no delivery
  point is asked and waited for, never deleted, because the pid now makes it
  *reachable*, and reachable is not the same as safe.

  **The exceptions are the design, not an afterthought.** Four kinds of task
  stay out, each for a reason no better bookkeeping fixes: the reaper, which
  tears processes down and so cannot be one; the klog flusher and the
  supervisor, whose death is the system's (Linux's PID 1 is special for the
  same reason); anything on the panic path, which runs with the scheduler
  frozen and cannot touch the table; and the tasks ESP-IDF owns -- `wifi`,
  `tcpip`, `esp_timer`, `ipc0`, `emac_rx`, `mdns`, the USB host's -- which
  espix did not create and cannot outlive. Listing those and leaving them
  uncontrollable is the honest state, and it should say so in the code.

  It also does not make a service killable. A task with no delivery point --
  blocked in a driver call -- cannot be SIGKILLed safely, and that stays true
  however the table is arranged; the stop path would still ask and wait. What
  it buys is one identity and one control plane, not a licence to delete
  kernel tasks. What would show it worked: `ps` gives `nfsd` a pid and a
  state, `kill nfsd` stops it and it is reaped, and the shutdown sequence has
  one process phase instead of two.

- **Ctrl-Z.** Job control is done for jobs, fg and bg (R-P2.6), but nothing
  stops a foreground process. The transports now report which key arrived
  rather than only that one did (R-P7.7), so the field a handler needs exists;
  Ctrl-Z itself is still missing.

## Filesystem

- **Owning the syscalls (option C), if the triggers arrive.** espix routes
  paths through ESP-IDF's VFS, which is enough for files but not for `select()`
  on one — that answers `ENOSYS` today. Owning the syscalls means implementing
  the libc entry points directly, with an fd table of espix's own, permissions
  at a single entry point, `/dev` nodes with real semantics, and no prefix
  matching to be surprised by. The obstacle is not files but **sockets**: lwIP's
  sockets live in IDF's table and are the reason `select()` matters at all, so
  this needs a mapping layer between espix's fds and IDF's socket fds before any
  file moves. Worth doing when device files need real read/write semantics,
  sockets must be selectable in the same set as open files, there are
  per-process bind mounts, or a second filesystem type that must not be
  reachable by path.

### The POSIX surface, and which layer would have to change

espix presents a POSIX-shaped interface over filesystems that are not POSIX and a
VFS that is not the kernel's, so some of what an app can call behaves differently
or not at all. This is the list, with the honest answer for each: a fix and the
layer it belongs to, or a choice and the reason.

| surface | espix today | what would change it |
|---|---|---|
| `fstat().st_uid`, `.st_gid` | still `0`, and marked in the code | the rule is path-based and a descriptor is not a path; the fix is to remember the path on the fd slot, or to leave it |
| `stat().st_blksize`, `.st_blocks` | plausible constants | a real value, or leave and document |
| `access()` on a path | not espix's to answer | implement in the VFS |
| `link()`, `symlink()` | unsupported | the VFS, then the lower filesystems |
| `select()` on a file | `ENOSYS` | a select in the VFS, or owning the syscalls (option C above) |
| `dup()`, `fcntl()` | partial | the VFS |
| `mmap()`, `statvfs()` | absent | the VFS |
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
  beside them.

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
  mode path already consults the mount for its owner.

- **Per-app export tables.** The other half of what the README used to call
  "per-app capabilities", and still open. espix publishes one fixed set of
  symbols to every app it loads; an app that has no business calling
  `esp_wifi_*` is handed it anyway. The loader resolves against a table, so
  subsetting per app is a matter of choosing which table, and the cost is
  deciding where the per-app policy is written down -- a manifest beside the
  binary, or something in the ELF itself.

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

The layering matters as much as the list. elf_loader's own tables used to answer
for 62 standard names *before* espix's were consulted, which made the loader's
example list part of the ABI. R-P3.1 turned them off — `Check_abi.py` fails the
link if a name they answered for is not published by espix — so there is one
definition now instead of two searched in order. Both mistakes are still reported
on every link: an entry an earlier table answers for, and a name the loader's
tables answered for that espix does not publish now that they are off. That is
what caught `memset`, `strtol` and `ets_printf` sitting unreachable in a
driver table. The resolver still runs before every table; that is the seam for a
name espix must *own* rather than merely allow.

`components/espix_proc/abi_libc.c` carries the full statement, the list of
which file owns which name, and why none of this can be a `_Static_assert`; `tools/check-abi.py`
enforces it; `tests/suites/12-vfs.sh` calls the published names from an app, and
checks that an app is refused what the shell is refused.

### ext2/3/4, via a port rather than a library

The port is integrated: `components/espix_fs/ext.c` mounts ext2, ext3 and ext4
through espix's VFS, read-only by default and writable with `mount -o rw`, in
which case espix starts the journal itself. What remains is the failure path and
the surfaces that are not filesystem-specific.

**The error path is the part to prove, not to write.** Writes go through the
port's experimental extent implementation, and its own list applies in full:
mutation requires an active journal transaction, an unwritten extent returns
`ENOTSUP` on a create/write lookup, and *"these checks do not qualify the
implementation for arbitrary power loss, injected I/O failure, hostile-image
fuzzing, or all Linux-created extent layouts."* espix carries the port's
error-propagation patch — `ext4_fwrite()` overwrites an allocation error at its
`Finish` label, so the abort/commit decision could see the wrong value — applied
by `tools/patch-lwext4.py`, and belonging upstream rather than here. What a
patch cannot settle is the rest of that sentence: passing `e2fsck` is not
evidence that an injected write failure rolls back. That is testable here in a
way it was not for the port's author, because espix owns the BDL wrapper — an
adapter that fails the Nth write on demand exercises the abort path directly,
and the host closes the loop the port used:

    write on the device → umount → attach the stick to a PC → e2fsck -f -n

That is also the only way to check a journal replay after a pulled device, and
both need the stick attached, so they are tests for an OTG run rather than the
wireless suite. The injection aimed so far lands in the journal's own writes
rather than in `ext4_fwrite()`, which is why that A/B is still to be built.

**The default stays read-only**, for the reason `ext.c` opens with: a volume
mounted here may be somebody's only copy of something. A volume without a
journal cannot be mounted writable at all, which is reported rather than
downgraded to a silent read-only mount.

**The licence is a fork in the road rather than a detail.** lwext4 is GPLv2 with
exactly two GPL files (`ext4_xattr.c`, `ext4_extents.c`); the port's default
build compiles an **MIT** extents implementation and an xattr stub instead,
keeping both out and the firmware BSD-3/MIT. Selecting upstream extents instead
makes the binary GPL-2.0. The MIT extent code is the port's own "experimental"
replacement, verified depth-2 against `e2fsck` and `debugfs`.

**A real ext volume would still find these missing**, all model questions rather
than additions: `statfs`/`statvfs` as a filesystem-agnostic surface (the
capability is in lwext4's `ext4_mount_point_stats()`, but `df` reaches for
`espix_fs_stat_fat()` by name), hardlinks (lwext4 supports them; espix has no
`link()`), and symlinks (`symlink()`/`readlink()` plus resolution in the VFS,
which the tree has nowhere).

**And the 64-bit `off_t` did not reach the append path.** `cp` truncates and
streams, so copying in is unaffected, but appending past 4 GiB goes through the
same 32-bit stdio seek as an SFTP transfer and has the same fix — moving those
paths onto descriptors. See [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

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

- **`sshd` as a unit, so it can be turned off from /etc/units.** The server is
  started by espix itself today -- `ssh_server.c` creates its accept task at
  boot -- and there is no `sshd` builtin, so a line in the units file would do
  nothing (which is what the example file used to imply). What it costs is a
  builtin that starts the server and then blocks, which a unit with `always`
  keeps up, plus removing the boot-time start. What it buys is the control every
  other daemon already has. The decision to settle first is the way back in: a
  device whose SSH is off is reached over the serial console, so the default has
  to stay on, and turning it off has to be a deliberate edit rather than an
  uncommented line somebody copied.

- **More than one channel per connection.** `direct-tcpip` landed, so
  `ssh -N -L` works -- but the server carries exactly one channel for the life
  of a connection, which is why `ssh -L` (a shell *and* a forward) is refused.
  Closing that gap changes the shape of `ssh_channel.c`: a table of channels, a
  receive loop that dispatches on the recipient channel id instead of assuming
  there is one, and per-channel windows. The blocking I/O the shell depends on
  (`chan_read_line`, the editor's own callbacks) makes that more than a
  refactor -- today the connection task *is* the channel.
- **Split the connection task from the session task.** Today one task reads
  the wire, runs the shell, runs a foreground command and pumps a program's
  stdin, and it is the only reader of the socket -- which is why a foreground
  command has to poll for Ctrl-C rather than block on it (R-P7.1 item 2). A
  reader task feeding a queue turns that into a push, and several other things
  with it: the foreground wait could block on an event group instead of a 50 ms
  loop; chan_poll_interrupt's bounded drains could become a real recv or
  select; the transport's vTaskDelay(1) retries in ssh_transport.c would have
  somewhere to block (R-P7.1 item 5); Ctrl-Z and q need the key reported rather
  than consumed (R-P7.7); and the orphaned transmit lock a force-killed writer
  leaves behind (R-P7.9) is the same task doing the writing while being
  deletable. It is also what the multi-channel bullet above runs into from the
  other side. Not scheduled: the reasons are piling up, and this is where they
  land if one more arrives.

  **First stage landed.** A per-connection reader task now owns recv() and
  fills a ring; the connection task blocks on it. That removes the read
  half of R-P7.1 item 5. What item 2 still needs is not the task but the
  event plumbing: one object the reader (bytes) and the process finish
  (the child) can both set, so cmd_run() can block on either.
- **`ssh -R`, and `ssh -D`.** The other two directions. `-R` is a client
  asking espix to *listen*: the global `tcpip-forward` request plus
  `forwarded-tcpip`, and the listener side has no equivalent here at all. `-D`
  is a SOCKS proxy, which is `direct-tcpip` with the destination chosen per
  connection -- nearly free once `-L` exists, and it wants the same policy
  decision `ESPIX_SSH_TCPIP_FORWARD` already makes.
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

- **A text editor.** There is none. `echo >` and `>>` cover `key=value` config,
  which is why it has not bitten yet, but anything larger wants an `ed`-style
  line editor. It is also why so much of espix's configuration ended up behind
  commands — `wifi connect`, `passwd`, `useradd` — rather than as files to edit.
  That is a reasonable shape for a device, but it should be a choice rather than
  what happens because there is no alternative.

## Platform

- **A power-off that stays off, and a goodbye to whoever is connected.** The
  shutdown sequence refuses new work and stops everything espix controls, but
  it tells nobody: there is no `wall`, and NFSv3 has no message for it. Both
  are deliberate — the first wants a registry of SSH connections and the second
  wants a protocol that does not exist — and neither is worth doing before the
  sequence itself is.

  `poweroff` always arms the wake timer, too. Staying asleep until the power is
  cycled is the obvious option to add and is deliberately absent: on a headless
  board it is a one-way door, and the timer is what stops a device nobody
  remembers to unplug from being gone.

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

- **USB host beyond storage.** Enumeration, identification, the partition table,
  superfloppy volumes and hotplug are done and shipped — see
  [USB-HOST.md](USB-HOST.md) — and what is left divides into things that are cheap
  and things that need a decision. Two of the original open questions are now
  answered: a PD hub does power the board *and* enumerate devices, and **two
  storage devices do not fit** — the S3 has a fixed pool of host channels and a
  hub plus one disk consumes almost all of them, so the second disk is refused
  while the first works. A keyboard costs two channels where a disk costs three.

  - **exFAT** is a patched dependency rather than a feature, and the one item
    here with a legal question attached: exFAT is covered by Microsoft patents,
    and FatFs's author has said a licence may be needed for commercial use.
    **That has not been verified against IDF or FatFs here** — no patent or
    licence text ships with the bundled FatFs — so it is a "check the terms
    first" item, not a "just enable it" item. The patch itself
    (`tools/patch-fatfs.py`, behind `CONFIG_ESPIX_FS_EXFAT`) and what it edits
    are in [USB-HOST.md](USB-HOST.md#roadmap-not-now).
  - **lwext4** for ext2/3/4 has landed — see
    [ext2/3/4, via a port rather than a library](#ext234-via-a-port-rather-than-a-library).
    A much later `lwntfs` is the same shape and still open; NTFS stays
    recognised-and-unsupported, because FatFs reads exFAT, not NTFS.
  - **GPT** is read in `host.c` beside the MBR walk, and what asked for it was a
    4TB Samsung T9 — three partitions behind a protective MBR, one unreadable row
    before. What is left of it is small: an entry's UTF-16
    partition name, and the disk's own GUID, neither of which is printed
    today — plus one real oddity, not yet chased down: reading the same
    entry-array LBA twice in one `gpt_read()` call, once inside a clean
    whole-array checksum sweep and once during the interleaved entry walk,
    gets two different answers on the T9, moments apart. All three
    partitions still list correctly either way, so it costs nothing today;
    see [USB-HOST.md](USB-HOST.md#roadmap-not-now) for what was checked and
    what the fix would be.
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

- **A latency request, so a component that needs the radio can say so.** ssh,
  vnc, an sftp transfer, audio streamed over the network -- each of them is
  interactive and each of them pays for a power saving it did not ask for. The
  shape to copy is Linux's `PM QoS` and Android's `WifiLock`, because it is the
  same problem: the component knows what it needs, the subsystem knows what to do
  about it, and neither should know the other's details.

  Measured, and it is what makes this worth doing at all: the station's default
  `MIN_MODEM` costs **76 ms average and 46 ms standard deviation** pinging this
  board, against **4.9 ms and 2.0 ms** with the radio awake, and 4.1 ms / 1.7 ms
  on a cable. The radio is as good as the wire once it stops sleeping, so
  everything interactive has been paying a beacon interval per exchange for a
  saving nobody wanted. `wifi ps off` is the manual version; this is the
  automatic one.

  Three things decide whether it stays simple or becomes a liability, and they
  are worth settling before the first caller:

  - **A request is released by the task that made it.** A boolean leaks on every
    crashed session, and the radio then stays awake for good -- a silent battery
    and heat cost, and the classic `WifiLock` bug. Tying a request to a task and
    dropping it from the reaper is the same shape `task_gone` already has for a
    socket lock, so it cannot leak rather than being remembered not to.
  - **Count, do not flag.** vnc and ssh overlap, and the last one to leave is
    what turns it off.
  - **The vocabulary is the quality, not the mode.** A caller asks for low
    latency; espix decides that this means `PS_NONE` today and might mean TX
    power or a protocol mask later. An API that names `WIFI_PS_NONE` is a UI for
    IDF's API rather than a policy for espix's.

  Not decided: whether it should also cover Bluetooth coexistence while audio
  plays -- that would be an observation to make rather than assume -- and whether
  a short grace period before the last release is worth the state it costs, to
  stop the mode flapping during a pause in typing.

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
  absent on this link. The login was slow for an entirely different reason:
  **PBKDF2 cost 2030 ms of pure CPU**, against the "~100ms" its own comment
  claimed. That is now 223 ms, after `auth.c` stopped going through PSA — see
  [UPSTREAM.md](UPSTREAM.md#psas-pbkdf2-re-derives-the-hmac-key-on-every-iteration).

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

  **NFS is already a user of this, from outside the device.** A client asks for
  a file's times and prints whatever the volume holds, so a stick written before
  the first NTP sync reads 1970 on a machine whose own clock is perfectly good --
  and the client cannot correct it, because the timestamp is the server's to
  keep. `SETATTR` lets a client set a time it knows, but not the time a local
  write should have; only a floored clock does that. Nothing else in the
  ROADMAP's clock item has a user waiting on it, which is the argument for doing
  it sooner rather than when the RTC work happens.

- **An NFS handle table that does not pay a path per handle.** A handle names a
  slot in a table of paths, because 28 bytes cannot hold a path and a path is not
  what stays still when a directory is renamed -- [NFS.md](NFS.md) has the shape.
  Each slot is a fixed 256 bytes, the longest path espix allows anywhere, so 1024
  slots is 264 KB of PSRAM and the paths on a stick are 30 bytes. The table is
  now given back when the last client unmounts, so the idle cost is zero; the
  *mounted* cost is still a path's worst case multiplied by a directory's worth
  of entries.

  Keeping the paths in one pool and the slots as (offset, length) is eight bytes
  rather than 258: an 8 KB index and a 64 KB pool holding about two thousand short
  paths, so ~72 KB for more capacity than today. What makes it more than a
  rewrite is eviction, and it has to be decided rather than discovered: a path
  overwritten while a client still holds its handle resolves to the *wrong* file,
  which is worse than failing. Either the pool is never reclaimed until the table
  is released -- a full pool then means what a full table means today -- or the
  oldest slot goes first and the client recovers from `NFS3ERR_BADHANDLE` as it
  already does. Worth measuring which of the two a real stick reaches first.


- **NFSv4.** The version a client asks for first, and every distribution's
  `mount.nfs` tries it before being told `vers=3`. It is a different protocol
  rather than a later revision: COMPOUND, a pseudo-filesystem to walk, state and
  leases, a delegation model, and its own id-mapping. Weeks, not hours, and the
  reason to do it is that "mount it with no options at all" then means no
  options on any client -- worth knowing it is a project, not a patch.

- **Advisory locking, which is currently a lie told politely.** statd answers
  SM_MON so that a client will mount, and nothing arbitrates: two clients
  locking the same file both succeed and neither is told. What it costs is the
  NLM protocol -- LOCK, TEST, UNLOCK, the GRANTED callbacks, and a state table
  that has to survive a client vanishing -- plus the decision of what a lock
  means when the lock holder is the board and the file is on a stick somebody
  can pull. It buys correctness for the case the NAS is for: two people writing
  the same directory.

- **More than one request at a time.** The daemon serves one RPC per loop, so a
  slow directory scan is every other client's latency. A second worker task
  would need the handle table and the export table to be safe under concurrency,
  which they are not today -- they are single-threaded by construction and say
  so. Moderate, and only worth it when there are several busy clients.


## Audio

Phase 1 (Bluetooth speaker playback) is built; [AUDIO.md](AUDIO.md) has what it
is and what it is not. These are the things it needs next, roughly in the order
they block.

- **A source layer with read-ahead, above the filesystem.** The engine reads PCM
  with `read()` into a 32 kB chunk it owns in PSRAM, which is correct but
  blocking: the decoder waits on the filesystem, and the filesystem is slow
  enough that a 44.1 kHz stereo WAV sits right at the edge (~880-960 ms of read
  per second of audio on USB, ~1050 ms on littlefs, which underruns). A small
  double buffer, or a prefetch task, reading 32-64 kB ahead of the decoder,
  hides that latency instead of exposing it. It is also the one place a file and
  a URL source should share: an HTTP source needs the buffering more, and the
  decoder should not know which it is talking to. This is the page-cache and
  readahead analog, and it is the architectural fix rather than a bigger read.

  What it is not: a general page cache. It holds one stream ahead of the
  decoder, for as long as the stream is open, and frees it after.

- **A truthful `st_blksize` in espix's VFS.** newlib's `fopen` sizes its `FILE`
  buffer from `fstat`'s `st_blksize`, or its own `BUFSIZ` (1024) when there is
  none -- and espix's VFS sets neither, while fatfs is configured
  `CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0`. So every stdio user reads in 1 kB calls,
  and only the paths that avoid stdio (the `play` engine) read well. Reporting
  the real block size (4096 for littlefs; the FAT cluster or sector for fatfs) is
  a few lines and fixes every program at once, not just this one. Cheap, and
  worth doing before the source layer.

- **Chunk to the medium.** Reads should be a multiple of the filesystem's block,
  and for USB the mass-storage transfer size should be big enough to amortize
  the BOT overhead. The engine's 32 kB is a fixed number today; per-medium (and,
  for USB, per-transfer) sizing is the small version of this.

- **littlefs tuning.** Larger cache and read sizes, and never run the volume
  near-full -- littlefs degrades when it is. Held back deliberately: the cache
  is internal RAM, the scarce pool, so this waits for the memory work below
  rather than taking another kilobyte today.

- **Resampling, and when not to.** The engine converts neither rate nor
  channels, so a source must match the rate the sink negotiated; a 48 kHz file
  into a 44.1 kHz SBC stream is wrong. The order should be: (1) if the sink
  advertises the source's rate in its codec capabilities, ask for it in the
  preferred codec config and pass the PCM through untouched; (2) only if it does
  not, convert -- and then with the **hardware ASRC** (`esp_asrc`,
  `CONFIG_SOC_ASRC_SUPPORTED`) rather than a software converter. (1) is free and
  is the common case: the Q45 advertises every SBC sample rate.

- **The codec and quality choice should be our policy over the library's API.**
  `esp_a2d_source_set_pref_mcc()` is the right call and is in use, but its values
  are hardcoded (mono, bitpool <= 35). They should be *computed*: intersect what
  espix can encode with what the sink advertises -- rates, channel modes,
  subbands, block length, allocation, bitpool range -- take the best of the
  intersection, then de-rate to what the link has been *measured* to hold.
  Today's mono/35 is the measured point, not the capability: the sink advertises
  joint stereo and bitpool 52 and the link cannot hold it. Keeping capability and
  measured-reliable separate is what lets testing raise it later.

- **MP3 decode is the blocker for the phase-1 goal.** WAV decodes in 10-27 ms per
  second of audio; `esp_audio_simple_dec`'s MP3 path measures ~2900 ms, so the
  ring never stays fed. Next measurement: decode with Bluetooth off, to tell a
  slow library from CPU throttling under coexistence.

## Memory

Internal RAM is the scarce pool (about 296 kB, and Bluetooth Classic takes a
large share of it). PSRAM is 13 MB and nearly free. Most of the work so far has
been moving things off internal by hand -- GMF's allocator preference, this
engine's buffers, the PCM ring's storage -- and hand-tuning does not scale. It is
also how a subsystem ends up with no headroom for the one allocation that
matters.

- **Budgets before a reaper.** The simple, predictable version is a static cap
  per subsystem (cache bytes, stream bytes, task stacks), enforced at the
  allocation site. An MCU has no swap and no MMU to make overcommit safe, so the
  failure mode to design for is not "slow", it is "this allocation must not
  fail". Budgets give that, and they make the next idea safe by marking which
  memory is cache and may be lost.

- **A reclaimable-cache registry, then a reaper.** The idea worth keeping from
  the sketch: let caches register themselves -- a size, a priority, and an
  eviction callback -- so that when an allocation that *must* succeed would fail,
  the allocator can evict the least important cache and retry, disk/VFS cache
  first, oldest first. Two rules make it trustworthy rather than clever: only
  *registered caches* are evictable (never task stacks, DMA buffers, live
  streams, or anything with a pointer out), and eviction is the cache's own
  code, so it cannot break its invariants. Under those rules "keep little free
  memory and reclaim on demand" is safe -- but only for the pools it is allowed
  to touch. In practice that means PSRAM: internal RAM should stay budgeted,
  because reclaiming internal while a Bluetooth allocation waits is exactly the
  failure the reaper is meant to prevent.

  It pairs with the fault reaper in Processes: both free what a dead task held.
  And it is a policy layer, not isolation -- without an MMU it
  cannot reclaim memory an app has already corrupted, only memory a cache has
  declared expendable. The MMU/isolation work is what would turn it into a
  safety mechanism rather than a convenience.

  Not small, and not urgent while `play` holds one stream and the caches are
  small. Worth writing the registration interface early, so caches are born able
  to be evicted instead of retrofitted.

## A game as the showcase app

Attempted, and stopped at the memory wall. The target was **Quake 2** — the port
that already exists for the ESP32-P4 (`alexkid77/ESP32_QUAKE2`, the
`quake2generic` engine with its `ref_soft` software renderer) — packaged as an
espix app, launched from a desktop icon, taking the screen and giving it back.
It is the workload the whole loader/export-table/app-ABI stack exists for: a real
program that names only the ABI, with nothing added to the kernel for it.

**It builds, loads and runs up to the map.** The engine reads
`baseq2/pak0.pak` (1106 files), initialises its console, loads `ref_soft` at
320x240, runs `InitGame`, and starts the demo map:

```
====== Quake2 Initialized ======
FS_BIG: maps/demo2.bsp (2102792 bytes)
Error: Hunk_Alloc overflow
```

**The wall is PSRAM, not code.** The P4 port was tuned for 24 MB; this board has
16 MB (about 13.5 MB usable, shared with the firmware). The demo needs roughly:
map statics ~2.8 MB, other engine statics ~2.9 MB, hunk pool 5-6 MB (the P4 port
used 7 MB), zone + surface cache + images 2-3 MB, app text/data/image/stack
~1.6 MB, canvas 0.15-0.5 MB — about 15-16.5 MB. That is *after* quartering the
map limits, carving the hunk out of the arena, and cutting the surface cache and
the canvas; it does not close.

**Outlook.** A board with 24 MB of PSRAM should run this as-is. On 16 MB it needs
the engine's permanent statics moved into the hunk, where stock Quake 2 keeps
them, so the map arrays share the level pool instead of occupying `.bss` for the
system's lifetime. That is the one structural change that would close the gap,
and it is engine work rather than espix work — which is why it is parked here.

**Cheaper first steps, in order:**

- **PIE `memcpy`/memset`.** The P4 port's SIMD routines assemble for the S31's
  `xespv` extension unchanged, but S31 PIE is core-1-only, so the routines (and
  the tasks that call them) must be pinned there. Worth benchmarking on the S31
  before adopting.

## App data the program does not ship

A program's data is not the kernel's business, and the shape is the one real
systems use: the program declares what it needs in `apps/<name>/appdata.conf`
(`data <url> <path> sha256:<hex>`), and the launcher fetches it before spawning.
That is built -- `espix_net_fetch()` does the transfer, `espix_appdata_ensure()`
reads the manifest, the desktop and the shell both call it with their own
reporter, and the fetch runs on a task of its own so a download does not stop
the RFB encoder.

What remains is the progress UI: it is a bar rather than a cancel, so there is
no way to stop a first launch short of killing it.

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
  the deployment espix already has, and the unit is what makes a deployed sketch
  outlive the login that started it. Two things to settle first: whether the sketch is built by the
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

## The arena's sizing, derived rather than estimated

 sizes a region from an estimate of the heap's own bookkeeping and then
verifies the fit, growing the carve by whatever it was short when the estimate
missed. That is correct, but it is a measurement made one request at a time. The
bookkeeping is a function of the pool size, so it could be measured once: sweep
allocation sizes up to the board's maximum, record where a fresh region falls
short and by how much, and derive the formula from that. A region would then be
the right size the first time on every target, with the verify loop kept as an
assertion rather than as the mechanism.
