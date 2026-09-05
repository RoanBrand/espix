# espix architecture

Notes on how the skeleton is put together and why. Design decisions that are
non-obvious from the code, and the ones that were verified against ESP-IDF
rather than assumed.

Target of record for this skeleton: **ESP32-S3, 16MB flash, 8MB octal PSRAM**,
on **ESP-IDF v6.1**.

## Component graph

`main` owns the init order; nothing else knows it. That is what keeps the graph
acyclic — otherwise a "kernel" component that boots everything would depend on
the commands, while the commands depend on the kernel.

```
espix_kernel     klog ring (dmesg), version, uptime — no espix dependencies
    ↑
espix_fs         LittleFS as /, path resolution, rm -rf
    ↑
espix_shell      sessions, command registry, dispatch, console transport
    ↑
espix_proc       process table, ELF exec
    ↑
espix_fault      panic interception, reaper skeleton
    ↑
espix_cmds       the actual commands
    ↑
  main           app_main = the init sequence, nothing else
```

`espix_net` sits outside this — a header-only placeholder until SSH work
begins.

## Decisions worth knowing

### `/` is real, and it is espix's

Paths are `/bin/hello` and `/etc/hostname`, not `/storage/bin/hello`, because
espix registers a VFS with `base_path = ""` — which ESP-IDF documents as a
*fallback* handling any path no other VFS claims. `/dev/*` still reaches its own
driver, by being a longer prefix.

That registration used to be LittleFS's. It is now espix's own VFS, with
LittleFS mounted beneath it and holding no path of its own; the next section is
why. The property described here is unchanged — one filesystem, at the real
root — and the empty base path is still what delivers it.

### espix owns the root VFS, and that is where permissions belong

Every file call in the system — from a shell command or from an app loaded off
the filesystem — goes through ESP-IDF's VFS, which routes by path prefix to
whichever driver registered it. espix used to register joltwallet's LittleFS
port at `""` (the fallback, which is what makes a filesystem the root) and live
above it. That left espix nowhere to stand: an app calls `fopen()`, libc calls
the VFS, the VFS calls the port, and no espix code runs on the path at all.

**Linux and NuttX both check permissions in the VFS, not in filesystems.**
`inode_permission()` / `generic_permission()` decide; ext4 and btrfs carry no
permission checks and merely supply `i_mode`/`i_uid`/`i_gid`. So espix
registers its own VFS as the root, with LittleFS underneath it, and
`espix_fs_access_check()` sits in `open`, `opendir`, `unlink`, `rename`,
`mkdir`, `rmdir` and `truncate`. Not `stat`: POSIX gates that on execute
permission for each parent directory rather than read on the file, and espix
does no path-traversal checks, so gating it would be stricter than Unix rather
than closer.

#### Stacked by pointer, not by path

The obvious way to reach the layer below is to give it a base path — mount
LittleFS at `/.lfs` and rewrite `/etc/hostname` into `/.lfs/etc/hostname`. That
works, and it publishes a *second name for the root*: anything spelling `/.lfs/...`
addresses the filesystem with the checks skipped. Which would make the whole
exercise decorative.

A stackable filesystem does not route through the namespace; it holds a pointer
to the layer below. `esp_littlefs_mount()` — added by
[tools/patch-littlefs.py](../tools/patch-littlefs.py) — mounts without
registering a base path and returns the driver's ops and context, so LittleFS is
live and reachable by **no path at all**. `ls /.lfs` answers "no such file or
directory", and that is a test worth keeping.

Two words hide in "mount", and separating them is the whole trick:

| | what it means | the call |
|---|---|---|
| Mount the filesystem | read the superblock, make `lfs_t` live | `lfs_mount()` |
| Publish a name | tell the VFS "paths under X route here" | `esp_vfs_register_fs()` |

`esp_vfs_littlefs_register()` does both, which is why espix could never have one
without the other. Note also that `""` and `"/"` are not two entries to divide
between two filesystems: `is_path_prefix_valid()` requires two characters or
more, so registering at `"/"` fails outright. `""` *is* the root, and one VFS
holds it.

Three things fall out of forwarding by pointer. No second name. No path
translation, since the port wants a mount-relative path and espix's base is
`""`. And no `DIR` wrapper — `esp_vfs_opendir()` stamps `dd_vfs_idx` on
whatever handle comes back, marking LittleFS's handle as espix's, which is
harmless only because forwarding is a direct call: the port's `readdir` casts
the pointer to its own type and never reads that field. Routing through a path
would have sent `readdir` back into espix, forever.

`df` still reports `littlefs` on `/`, and that stays true: espix's VFS holds no
storage, so the filesystem really is LittleFS. It resolves by partition label,
not by path, so it never noticed the change.

#### What the check compares against

`espix_fs_access_check()` finds the caller in one of three places, in order: the
process running on this task, whose credentials were copied from its session at
spawn; failing that the task's current session, which is what covers builtins,
since `cat` and `rm` run in the session task rather than as processes; failing
that nobody, which means espix itself and is allowed.

Copying a process's credentials rather than following its session pointer is not
an optimisation. The session lives on the stack of the command that started the
process, and anything backgrounded outlives that frame — so reading through the
pointer would be a use-after-free on the path that decides whether a file may be
opened.

uid 0 skips the checks. Otherwise the owner triad applies if the uids match, the
group triad if the gids do, and the other triad if neither, with the first match
final — so a file whose mode is `0077` is unreadable to its owner, which is Unix
behaviour and surprises people. Creating and removing a name is checked against
the *directory*, because that is whose list is being edited; getting that
backwards would let anyone delete a file they cannot write.

Two things sit outside the check because they cannot go through it.
`espix_fs_chmod()` and `espix_fs_chown()` write a littlefs attribute directly
rather than through the VFS, so they carry their own test — only the owner may
change a mode, and only root may change an owner. And `espix_fs_priv_begin()`
raises privilege for two callers that would otherwise deadlock the design:
`espix_auth`, which owns a `/etc/passwd` that is 0600 root and must still be
readable to `ls -l` and writable by `passwd` (espix has no setuid, and this seam
is what stands in for it), and the ELF-magic probe inside the mode rule, which
opens a file in order to answer what the permission check is asking about — left
unprivileged it recursed until the stack was gone.

The DEBUG log records refusals: `D fs: uid 1000 denied open /etc/passwd`.

SFTP reaches the check the same way, though it took a second step to get there.
It runs on the SSH connection task, which is neither a process nor a session, so
for a while it was read as espix itself and skipped every check — an
authenticated client could fetch files its own shell login was refused. It now
gets a session carrying the connection's credentials and makes it the task's
current one for the duration, which is all the check needed. The session is
cleared before that call returns: the connection task lives on afterwards, and a
pointer left behind to a caller's stack frame is the use-after-free that used to
reboot the board from the exec path.

### File modes are a rule plus an attribute on the file

LittleFS stores no permission bits: `esp_littlefs`'s `stat()` fills in `S_IFREG`
or `S_IFDIR` and stops. But it does carry *user attributes* -- small blobs in an
entry's metadata, which `SPEC.md` describes as meant for exactly this and which
the port already uses to hold mtime. That is where a mode belongs, and it is
what the README always said this would use.

Reaching them needs the `lfs_t *` the port keeps private, so espix patches in a
public accessor; the finding and the patch are in [UPSTREAM.md](UPSTREAM.md).

The mode is then split in two.

A **rule** supplies the default: directories are `0755`, a file whose first four
bytes are the ELF magic is `0755`, everything else is `0644`. Nothing is stored
for any of that, and it is not an optimisation -- the image builder writes no
attributes at all, so every file in a freshly flashed rootfs arrives without
one, and without the rule nothing in `/bin` would be executable after a
`storage-flash`. Two more things fall out of it: a flash write happens when you
run `chmod` and at no other time, on a filesystem that pays a block erase per
write; and a device nobody has chmod'd has no mode state to be wrong.

The **attribute** then records only what someone changed. `LFS_ERR_NOATTR` means
"use the rule", which is also what a portable littlefs implementation is
supposed to do with an attribute it does not recognise. A mode matching the rule
again *removes* the attribute rather than storing it, so `chmod +x` followed by
`chmod -x` on an ordinary file leaves no trace.

The record is `{uint16 mode, uint16 uid, uint16 gid}` under type `0x70`. There
is no registry of attribute types, so that is a local choice, and the port's
mtime uses `'t'`. `uid` and `gid` were written as zero and read by nothing for
as long as there was no owner model, which cost four bytes a file and saved
rewriting every stored mode once there was one — and that is exactly how it
played out: ownership landed without touching a single file already on disk.

Ownership follows the same two-part shape as the mode. A file with no stored
attribute belongs to the account whose home directory contains it, longest home
winning, and to root otherwise; root's home is `/`, so the rootfs is root's and
`/home/esp` is esp's with nothing written to flash to say so. `chown` stores the
attribute only when it disagrees with that rule, and removes it when it agrees
again. Because the mode and the owner share one record, both are read through
one path that fills the fields nobody is changing from the rules — otherwise a
`chmod` would write a blank owner and quietly hand a file in `/home/esp` to
root.

What this shape avoids is bookkeeping. An earlier version kept the deviations in
`/etc/modes` keyed by path, which meant every rename and every delete had to be
followed by hand in `mv`, `rm`, `rm -r` and the SFTP server, and a rename espix
could not see would have lost the mode. LittleFS moves an attribute with the
entry and drops it with the file, so all of that code is gone and the cases it
was covering cannot arise.

### Only the execute bit is enforced, and that is not laziness

Nine bits are stored and shown; one is consulted. The asymmetry is about where
espix sits in the call path, not about how much work each would be.

espix owns execution. `run` and the shell's fallback both go through
`espix_proc_spawn_elf()`, so "may this run" is a question there is somewhere to
ask.

Reading and writing had no such place until espix took the root VFS — an app
opened a file through libc and the VFS reached the filesystem without passing
espix, and enforcing in `cat` alone would have been a boundary you step around
with `run`. That seam now exists (see above); what is still missing is an
*owner* on a file to compare a mode against, so the check is wired and
permissive.

Two identities exist — `root` on the console, `esp` over SSH — so it is not that
there is nobody to distinguish between. What is missing is an *owner* on the
file, which is also why `chown` does not exist and why `chmod` refuses setuid,
setgid and sticky rather than storing bits that name a privilege transition
nothing performs. [ROADMAP.md](ROADMAP.md) carries what adding one would buy.

### espix does not use `esp_console_run()`

The console component copies every command line through a single shared
`static char *s_tmp_line_buf` (`components/console/commands.c`), so it is not
reentrant. With one console session that is invisible; with a console plus an
SSH session it is corruption. espix therefore keeps its own registry and
dispatch (`espix_shell_exec`) and reuses only the reentrant pieces of that
component:

- `esp_console_split_argv()` — operates on the caller's buffer
- linenoise — line editing, history, completion
- argtable3 — available, not yet used (its arg structs are per-command
  statics, the same reentrancy trap; adopting it needs per-invocation
  allocation)

`esp_console_cmd_t.func_w_context` does not help: context is bound at
registration, not per invocation.

A session is `{read_line, write, cwd, ...}`. The console implements it over
UART/USB-Serial-JTAG; an SSH channel will implement the same two callbacks.
linenoise itself keeps global state and reads the calling task's stdin, so
exactly one console session can exist — which is why SSH sessions will supply
their own `read_line` rather than sharing this one.

Redirection (`>` / `>>`) is handled in dispatch, not per command, by setting
`session->redirect` for the duration of the call. It therefore works for every
command. It does *not* capture a spawned app's own stdout — see the stdio note
below for why that is deliberate rather than unfinished.

### A working directory, once espix owned the VFS

ESP-IDF offers none: `chdir()` is a hardcoded `errno = ENOSYS; return -1;` stub
and `getcwd()` unconditionally answers `"/"`
(`components/esp_libc/src/realpath.c`). For a long time espix's conclusion
followed from that — the cwd lived only in `espix_session_t.cwd`, every command
resolved through `espix_fs_resolve()` before touching the filesystem, and a
loaded app calling `fopen("data.txt")` got `/data.txt`.

That conclusion stopped being true when espix took the root VFS, and the reason
is worth knowing because it is not obvious. `get_vfs_for_path()` matches an
entry with `path_prefix_len == 0` against *any* path — a relative one included,
since `memcmp(path, "", 0)` always succeeds — and `translate_path()` then
returns `src_path + 0`, the string verbatim. So `"data.txt"` arrives at espix's
own `open()` unchanged, and espix resolves it against the caller itself.
ESP-IDF's stubs never come into it, because espix never asks them.

So a process has a real working directory: `cwd` on its slot in
`espix_proc_slot_t`, seeded from the session that spawned it. Per process rather
than per session, so an app's `chdir()` leaves the shell where it was — which is
what fork/exec gives you everywhere else, and the difference shows the moment
you run two things.

`chdir()` and `getcwd()` are published to apps as espix's own
(`abi_fs.c`), for the same reason `chmod` is: IDF's are stubs, and a stub that
answers `"/"` to `getcwd()` is worse than absent because it looks like it
worked.

### Apps are entered directly, not via esp_elf_request()

`espix_proc` calls `elf.entry(argc, argv)` itself. `esp_elf_request()` is
literally `elf->entry(argc, argv); return 0;` — it discards the app's return
value, so an exit status could never reach the shell. [exec.c](../components/espix_proc/exec.c)
replicates its only other behaviour (a NULL entry check) and keeps the result.

### The current session lives in FreeRTOS TLS

Command functions may run in a task other than the session's, so
`espix_shell_current()` reads TLS slot `ESPIX_TLS_SESSION_IDX` (1 — index 0 is
taken by ESP-IDF's pthread implementation). Hence
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`.

### Fault handling intercepts, but does not yet recover

`-Wl,--wrap=esp_panic_handler`, applied from `espix_fault/CMakeLists.txt`. This
is the same seam ESP-IDF's own test suite uses. Two details matter:

- The component must be registered `WHOLE_ARCHIVE`. `__wrap_esp_panic_handler`
  is referenced only by the linker, so otherwise the object is dropped and the
  wrap silently does nothing.
- The handler reads task names and the process table, which needs flash cache,
  so `CONFIG_ESP_PANIC_HANDLER_IRAM` must stay off. There is an `#error`
  guarding that.

Today the hook records core / exception / faulting address / task name / pid
into a `__NOINIT_ATTR` struct that survives the reset, prints one line, and
delegates to the real handler. The next boot reports it in `dmesg`.

It deliberately does *not* reap and resume. See the comment block at the top of
`reaper.c` for the three things that block that: locks held by the dead task,
no per-process ownership of heap and fds, and the fact that without an MMU most
corruption never reaches the fault handler at all.

### Signals are delivered when a process calls in, not asynchronously

ESP-IDF ships the signal *vocabulary* and no machinery. `<signal.h>` declares
`signal`, `sigaction`, `sigprocmask`, `sigsuspend`, `pause`, `alarm` and
`pthread_kill`; the toolchain defines **none** of them — libc's `signal.o`
holds one unused variable, because it was compiled with `SIGNAL_PROVIDED` on
the assumption the platform supplies the rest. `kill()` resolves to a stub
returning `ENOSYS` and `raise()` to one that calls `abort()`. IDF's pthread has
no `pthread_kill`, and its `pthread_sigmask` returns success while doing
nothing. FreeRTOS-Plus-POSIX, vendored as `components/rt`, is the message-queue
slice only; upstream never implemented signals either.

So all of it is espix's, in `abi_signal.c` — and because that namespace is
unclaimed, it is written under the real names. An app calls `signal()` and
`kill()`, not `espix_signal_*`. The numbers come from `<signal.h>` and espix
defines none: the toolchain uses the BSD set, where `SIGUSR1` is **30** and
`SIGUSR2` **31**, and a kernel carrying its own table would have disagreed
silently with every app compiled against the real header.

**A handler runs in the app's own task, at a delivery point, and returns to
where execution was.** It is not asynchronous. A real kernel interrupts the
thread at an arbitrary instruction and manufactures a signal frame on its
stack; doing that here means rewriting a FreeRTOS task's saved program counter
on windowed-register Xtensa, which is not a trade worth making. The delivery
points are the blocking calls espix publishes — `sleep`, `usleep`,
`nanosleep`, `pause` — which in practice is every app that ever waits.

The honest limit: a pure compute loop that never calls into espix has no
delivery point and never sees a signal. `espix_sigcheck()` is exported for
exactly that app, and `kill -9` is the answer when it is somebody else's
binary. `apps/sigtest spin` is the case in the flesh.

**Signalling wakes a blocked process.** `espix_proc_signal()` sets the pending
bit and then calls `xTaskAbortDelay()`, which cuts a `vTaskDelay()` short.
Without it a process in `sleep(60)` would not reach a delivery point for a
minute — long past the grace period — and would be deleted mid-sleep with its
handler never run and its cleanup never done. Waking it is what makes a handler
worth writing. Two properties to keep in mind: `xTaskAbortDelay` returns
`pdFAIL` for a task that was not blocked, so the pending bit is written *first*
and a target racing into a blocking call sees it rather than sleeping through
it; and IDF discards the semaphore result in `esp_vfs_select()`, so an aborted
`select()` returns 0 rather than `EINTR` and a woken task must consult its own
pending mask. A per-process eventfd is the only way to break a task blocked in
lwIP's `socket_select`, which `xTaskAbortDelay` cannot touch — deferred until
an app does socket `select()`, with `tty_console.c` already holding the pattern
to copy.

**SIGSTOP parks the process in itself.** Not `vTaskSuspend()` from outside:
suspending a task parked inside `malloc()`, stdio's `flockfile` or the VFS lock
holds that mutex for as long as the stop lasts and wedges every other task that
touches it — the same hazard `reaper.c` refuses to accept for the fault path,
and worse here because both cores are live. So SIGSTOP sets a flag, and the
target suspends itself at its next delivery point, where it demonstrably holds
none of those locks. It parks on a per-process semaphore rather than
`vTaskSuspend()` because a give that lands before the take is remembered:
SIGCONT arriving in the window between deciding to park and actually parking
cannot be lost, and the same race against `vTaskResume()` has no fix that does
not involve polling `eTaskGetState()`.

Two deliberate deviations from POSIX, both forced by having no job control:

- **Any signal but SIGSTOP lifts a stop.** POSIX leaves the process stopped
  with the signal pending until SIGCONT. espix has no `fg` to deliver that, so
  a stopped process holding a SIGTERM would sit on it until the grace ran out
  and then be deleted, cleanup and all.
- **`kill <pid>` escalates.** SIGTERM, a two-second grace, then SIGKILL. Unix
  would leave an app that ignores SIGTERM running, and on a device whose only
  console may be the one you are typing into that is a worse default. Any
  *named* signal (`kill -INT`, `-USR1`) is delivered and nothing more, which is
  what asking for a specific signal means. Ctrl-C does not escalate either —
  it sends SIGINT, and the third press is the escape hatch.

`sa_mask` and `sa_flags` are accepted and ignored. There is no signal frame to
apply a mask around, no restartable syscall for `SA_RESTART`, and nothing to
put in a `siginfo_t`. Ignoring them lets ordinary code that fills in a
`struct sigaction` work; pretending to honour them would be worse than either.

### The app ABI intercepts symbols rather than adding to a table

`espix_proc_abi_signal_register()` installs a resolver with
`elf_set_symbol_resolver()` instead of registering another symbol table, and
that is not a stylistic choice. `elf_find_sym_default()` searches the loader's
*own* libc table first, and that table already answers for `sleep` and
`usleep` — so a table registered with `esp_elf_register_symbol()` is consulted
too late to shadow them, and an app's `sleep()` has to be one a signal can cut
short. The loader documents the hook for "symbol interception and hooking",
which is exactly this, and it costs no fork of the component.

Every function in `abi_signal.c` is prefixed and mapped to its POSIX name by
that resolver rather than being *named* `signal()` and `kill()`. The names
espix could safely define are not the set apps need: `sleep` and `usleep` are
real functions in the firmware already, and `getpid` and `raise` are
force-linked by `esp_libc` with `-u`, so defining any of those would be a
duplicate symbol. Prefixing all of them keeps one rule instead of four
exceptions.

This works because **an app links no libc at all.** `project_elf()` produces a
relocatable ELF whose every libc call is an undefined symbol — `readelf
--dyn-syms` on `sigtest.app.elf` shows `printf`, `sleep`, `getpid` and
`signal` all `UND`. That is what makes interception total, and it is worth
knowing before assuming a `--wrap` is needed somewhere: it is not.

### The ELF loader needs memory protection disabled

`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` (and `ESP_SYSTEM_PMP_IDRAM_SPLIT=n` on
the RISC-V targets). The loader writes a relocated image into memory and then
executes it, which is precisely what that feature exists to prevent.
Espressif's own elf_loader examples do the same. It is also, bluntly, part of
why apps are not isolated from the kernel.

`espix_proc` reads app files itself rather than calling `esp_elf_open()`, which
only resolves names relative to `CONFIG_ELF_FILE_SYSTEM_BASE_PATH`; espix needs
to run an ELF at any absolute path the shell hands it.

### libc is pinned to newlib

IDF 6.1 defaults to picolibc. Under picolibc `stdin`/`stdout` are thread-local
and linenoise depends on a TLS-stdio shim in `components/console/private_include`,
which cannot be included from outside that component. Newlib also matches what
elf_loader's libc symbol table — how a loaded app resolves `printf` against the
firmware — was written against.

### The rootfs image is not flashed by `idf.py flash`

`littlefs_create_partition_image(storage fsroot)` without `FLASH_IN_PROJECT`.
With it, every firmware flash would rewrite the filesystem and destroy anything
created on the device. Flashing the rootfs is an explicit `idf.py storage-flash`.

### Upload throughput is bounded by flash erases, not by the network

A 1MB `scp` upload runs at about 78KB/s on a filesystem that has been used, and
about 195KB/s when the storage partition has just been erased. Downloads are
unaffected, at ~355KB/s.

Demonstrated on demand rather than inferred, on one board in one position:

| storage partition | upload KB/s | download KB/s |
|---|---|---|
| used | 78.3 | 356.0 |
| freshly erased (`storage-flash`) | 195.5 | 354.0 |
| used again, after writing 12MB | 78.2 | 357.9 |

Download is the control: it never erases a block, and it stays flat while upload
swings by a factor of 2.5. LittleFS frees a block when a file is deleted but does
not erase it, so every later write to that block pays an erase first. `df`
reporting 0% used says nothing about how many blocks are dirty.

Two traps for anyone benchmarking this:

- **Signal strength moves downloads and not uploads.** Going from -66dBm to
  -49dBm took downloads from 172 to 356KB/s and left uploads at 78KB/s either
  side. Upload is device-bound; download is link-bound. A figure quoted without
  the RSSI beside it is not a measurement.
- **Upload is not a crypto or protocol benchmark.** It is a flash benchmark. It
  was briefly suspected of being a PSRAM regression and of being a fault in the
  streaming SFTP write; it was neither, and the streaming write was never a
  candidate anyway, since the fast figures were themselves measured with it.

### The terminal probe's verdict is deliberately overruled

`esp_linenoise_create_instance()` probes the terminal once and latches
`allow_dumb_mode` from the result. On a device that probe runs as the console
starts — before anyone has attached a terminal, and always before
`idf.py monitor --no-reset` reattaches to a board already running. Nothing
answers, so it fails, and the console stays in dumb mode until reboot.

Dumb mode is not merely "no line editing". It has two defects that actively
corrupt input — a line terminated one byte late, so a stale byte from the
previous command survives, and ESC dropped as non-printable while the rest of
its sequence is kept. Both are written up with the source references in
[UPSTREAM.md](UPSTREAM.md#dumb-mode-corrupts-input-two-ways).

Both were reported as "the console goes strange until reboot". espix therefore
calls `esp_linenoise_set_dumb_mode(false)` regardless of what the probe decided.
Assuming a capable terminal and being wrong puts escape codes on screen;
assuming a dumb one and being wrong costs editing, history, and the integrity of
every command typed.

The consequence is that raw mode now runs against terminals that may never
answer a cursor-position query, and `get_cursor_position()` would block in a
read loop that swallows up to 31 typed characters hunting for its `R`. So the
console bounds that wait by wall-clock time, and once a query has gone
unanswered it stops sending them and synthesises the reply — the same thing
`ssh_channel.c` has always had to do, having no terminal to ask.

A related trap for anyone touching this: the paste heuristic stamps the clock
*before* the read and measures how long the read took, so it is really asking
whether the byte was already waiting. Anything that makes a read return
instantly turns typing into pasting, and pasted bytes bypass the escape parser
entirely.

### Networking is a naming layer, not a stack

`esp_netif` already provides what a Unix user expects — interfaces with
addresses, a default route, DHCP, DNS. `espix_net` adds a table mapping
`esp_netif_t*` to Linux-style names (`wlan0`, `eth0`, `usb0`, plus a synthesised
`lo`), and `ip`/`ifconfig`/`route` render that one table so the modern and
net-tools views cannot disagree. `ping` is `esp_ping_new_session()` from the
lwip component.

Three things worth knowing:

- **The hostname is per-netif.** lwip stores it on each `netif`, so it is
  applied in `espix_net_register_if()` rather than in the WiFi path — every
  future interface inherits it. Skipping this is why a stock arduino-esp32 board
  reports `esp32s3-xxxxxx` over WiFi but `espressif` over USB-NCM.
- **Nothing on the network path may block the boot.** `espix_net_init()` returns
  as soon as bring-up has *started*; association and DHCP run on the event loop.
  Likewise the disconnect handler retries via an `esp_timer` one-shot, never
  `vTaskDelay()` — it executes on the default event loop task, and sleeping
  there stalls every other event, including the IP events we are waiting for.
- **Credentials in `/etc/wifi.conf` are plaintext.** Consistent with the
  trusted-code-only model: there is no permissions system, and any app can
  already read any file.
- **Retries back off, and distinguish two kinds of failure.** 5s doubling to a
  60s ceiling, because a flat interval retried forever splats the prompt every
  few seconds indefinitely and churns the 96-line klog ring until boot history
  is gone. The reason code then separates the cases: credential rejections
  (`AUTH_FAIL`, `HANDSHAKE_TIMEOUT`, `4WAY_HANDSHAKE_TIMEOUT`, `MIC_FAILURE`…)
  cannot succeed on retry, so espix gives up after four and points at
  `wifi connect` — but not on the first, since APs do emit spurious handshake
  timeouts. Everything else (`NO_AP_FOUND` and its threshold variants, beacon
  timeouts) means "not visible right now", so those retry at the ceiling
  forever: a headless board must recover on its own when the router comes back,
  and silently staying offline after a reboot would be worse than the noise the
  backoff removes. A successful association clears the backoff, so a link that
  flaps once does not carry a minute-long delay into its next outage.

### Asynchronous output is fitted around the prompt

The session task calls the line editor, which writes the prompt and blocks on
read. Anything logged from another task — a link event, a timer — writes to
stdout meanwhile. Left alone the line becomes
`root:/# espix: wifi: wlan0: 192.168.110.55/24` and the prompt is gone; worse,
it *looks* gone, so people wait instead of pressing Enter. That confused several
people before it was fixed.

**This section used to argue the splat was unavoidable, and that argument no
longer holds.** It rested on `refreshLine()` being `static` in classic
linenoise, so nothing outside the library could force a redraw. espix has since
moved to `espressif/esp_linenoise`, which still exposes no redraw and still
warns that it is not thread safe — but it takes `read_bytes_cb` and
`write_bytes_cb`, and espix supplies both. Every byte in and out of the editor
passes through espix's own code, which is enough.

**Do not try to repair the line from outside the editor.** The first attempt at
this did, and it is worth knowing why it failed before reaching for it again.
`esp_linenoise_refresh_multi_line()` clears the rows it used last time by
walking *upward* — `\r ESC[0K ESC[1A` repeated `max_rows_used - 1` times
(`esp_linenoise.c:221`) — where `max_rows_used` is private, sticky within a
line, and counted from where the editor believes its prompt sits. Wiping the
prompt and redrawing it a row lower leaves that count wrong, and the next
refresh walks up into rows now holding kernel output and erases them. Pressing
Up for history is enough to see it: the recalled line appears and the message
above it vanishes as the window scrolls.

Printing a fresh prompt below the message without erasing anything has the same
defect — the prompt still ends up on a row the editor does not know about. The
difference the first version made was that the prompt was now *interactive*, so a
long-standing inconsistency became reachable. It survives while a line fits one
row and breaks once one wraps to two, which the usage hints make reachable,
since a hint is written but not counted in `rows`.

**What works is restarting the line, not repairing it.** `espix_kernel` publishes
an `espix_klog_console_hooks_t` — one `output_done()` callback — that the console
installs; both write points, klog's own echo and the forwarded ESP_LOGx path,
call it after writing. Notification only: the kernel hands over no text, because
the ESP_LOGx path forwards verbatim to the previous vprintf handler and a
formatted string would have to be clipped at `ESPIX_KLOG_LINE_MAX`.

The console sets a flag, and `console_read_bytes()` returns 0 when it sees it.
That makes `esp_linenoise_edit()` return normally — `if (nread <= 0) return
state->len;` (`esp_linenoise.c:772`) — so the session loop calls `get_line()`
again, and `edit()` resets `max_rows_used`, both cursor positions and the column
width on entry (`esp_linenoise.c:732-735`) before writing the prompt wherever
the cursor now is. The editor's picture of the screen is correct by construction
rather than by repair, and **multi-line editing keeps working**.

The read waits on the console descriptor **and an eventfd** in one `select()`
with no timeout, so the console task still sleeps until something actually
happens — a keystroke or a message — and costs nothing while idle. An earlier
draft polled on a 250ms timeout instead, which meant four wakeups a second
forever on a device that spends most of its life sitting at a prompt. Polling
was never necessary: the event is known exactly, the only problem was waking a
task blocked in `read()`.

That is also the mechanism the library uses for its own
`esp_linenoise_abort()` — `state.abort_read_fd`, selected on by
`esp_linenoise_default_read_bytes()`. espix has to repeat it because abort is
documented as having no effect once a custom `read_bytes_cb` is supplied, which
espix supplies.

With the restart in place the erase becomes safe, so `output_begin()` does clear
the prompt with `\r ESC[K` before the message — the stale row belief never
survives long enough to be acted on, because the restart immediately follows and
`edit()` re-reads everything. Without the erase, every message line would carry
a dead `root:/#` prefix, which is worse than what it replaced.

**The flag that gates this is a safety interlock, not a nicety.** `edit()`
returns `state->len`, so restarting a line with half a command on it hands that
half back through `get_line()` and the shell *runs it*. So `s_line_dirty` clears
on almost nothing — only CR, LF, Ctrl-C and Ctrl-U — and every other byte sets
it, TAB included, since completion can insert text with no printable byte behind
it. Erring towards "dirty" costs a redraw that does not happen, at a moment when
the user is visibly at the keyboard. Erring the other way executes a command
nobody typed.

One consequence to keep in mind when touching this: `edit()` adds an empty
placeholder to its history on entry and only the ENTER path pops it, so every
restart leaks one. `console_read_line()` therefore calls
`espix_history_apply()` on *every* line rather than only accepted ones — it
frees the editor's list and writes espix's own back, so the strays cannot
accumulate against a 32-entry history.

An earlier version of this note also described Enter appearing to produce two
prompts, as a second-order effect of the hint callback refreshing a line the
cursor had already left. That was a symptom of the splat and goes with it.

Noise is still cut at the source as well, because fitting a message around the
prompt is no reason to print one nobody wants: the WiFi driver's chatty tags are
lifted to WARN in `quiet_driver_logs()`, leaving espix's own four link messages.
Kernel messages landing on your terminal is what Linux does too — the complaint
was never that they appear, only that they ate the prompt on the way.

The honest long-term answer for *which* messages is a runtime console threshold
— `dmesg -n` — rather than a fixed list of quieted tags. On the
[roadmap](ROADMAP.md#shell-and-console), not done.

Boot is handled separately, and still is, because there the splat is
*guaranteed*. `espix_console_session_start()` would draw its first prompt about a
second in, while the WiFi association and DHCP it kicked off are still narrating.
So the console holds its **first** prompt until boot has settled —
`wait_for_boot_settled()` in
[tty_console.c](../components/espix_shell/tty_console.c).

The signal is *declared, not inferred*. `espix_kernel` keeps a boot-barrier count
(`espix_kernel_boot_hold()` / `_release()` / `_pending()`); a subsystem whose
bring-up continues past its init call takes a hold and drops it once it has
settled either way. `espix_net` holds across the boot connect and releases on
`IP_EVENT_STA_GOT_IP`, or on the first `WIFI_EVENT_STA_DISCONNECTED` — after one
failure the retry loop is not "settling" and the shell should come up. A later
`wifi connect` from the shell takes no hold at all.

A plain count, rather than the console asking `espix_net` anything: `eth0`,
`usb0` and an SSH listener are coming, and each should be able to declare its own
bring-up without the shell learning about it.

**Worth recording, because the obvious approach fails.** The first attempt waited
purely for klog to fall silent for 300ms. It releases too early on real hardware:
association and the DHCP lease are 1 to 1.4s apart, so the gate let go inside
that gap and the address lines landed on a freshly drawn prompt. Log silence is a
bad proxy for "settled" when the thing being waited on goes quiet mid-way, and no
constant window fixes it — DHCP timing belongs to the AP. A 250ms quiet tail
survives only so the banner is not glued to the last message.

The cap (~5s) is a backstop, not the mechanism: an AP that is powered off answers
neither association nor disconnect promptly, and a shell that never starts is
worse than a clobbered prompt.

Note which failures the cap does *not* cover, because it is easy to think you
have tested it when you have not. A wrong PSK raises
`WIFI_EVENT_STA_DISCONNECTED` with `WIFI_REASON_HANDSHAKE_TIMEOUT` (204) or
`WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` (15); an SSID that does not exist raises the
*same event* with `WIFI_REASON_NO_AP_FOUND` (201). Both therefore resolve through
release-on-first-disconnect, promptly. The cap is reached only when a hold is
taken and neither GOT_IP nor a disconnect arrives at all — in practice
"associated, but DHCP never answers", which needs an AP with DHCP disabled to
reproduce. It is accepted on inspection rather than tested on hardware: the check
is the first, unconditional statement in the loop, so no holder can bypass it,
and the unsigned millisecond arithmetic is wrap-safe.

Scope is deliberately narrow: once, before the session loop is entered, in the
console transport only. `session.c` — the part SSH will share — has no gate, and
an SSH session needs none, since it can only exist after the network is up and
kernel messages never reach it anyway.

### Kernel messages go to the console, command output goes to the session

Two different paths, and the difference is load-bearing:

- `klog.c` echoes with a bare `printf()` — stdout, i.e. the serial console.
- Commands write via `session_out()` → `s->write`, i.e. whichever session invoked
  them.

So kernel messages will never splat an SSH session's prompt, because they never
reach it. That matches Linux, where kernel output goes to the console and remote
users run `dmesg`. **Do not "fix" this later by routing klog through the current
session** — the current behaviour is the correct one.

A loaded app's own `printf()` is a separate matter, because it does not go
through `session_out()` at all — it writes to its task's libc stdout. espix
rebinds that per task, in `proc_task()`:

```c
stdout = out;
stderr = out;
```

This works because ESP-IDF gives every task its own `struct _reent` whose
streams are pre-pointed at the global ones and whose `__sdidinit` is already
set (`esp_reent_init`, `components/esp_libc/src/reent_init.c`). The assignment
therefore affects one task and is never undone by a lazy `__sinit`. It is the
same mechanism `components/console/esp_console_common.c` uses.

Where that `FILE *` comes from is the part worth recording. The obvious answer —
`fdopen()` on the channel's socket — is **wrong for SSH**: channel output has to
be wrapped in `CHANNEL_DATA` and encrypted, so writing to the raw descriptor
would put unframed plaintext on the wire and desynchronise the connection. The
stream is built with `funopen()` over the transport's own write path instead,
which is why `espix_session_t` carries a `FILE *out` rather than descriptors.
The console leaves it `NULL`: its apps already print to the right place.

Two consequences follow from an app's stdout reaching the transport:

- **The connection acquires a second writer.** Packets share `out_buf` and a
  sequence number the MAC covers, so `ssh_channel.c` serialises transmission
  with a recursive mutex, held across a whole `chan_write()` so one message
  cannot be spliced into another. The read side has its own lock, because a
  write that blocks on the peer's window must read the adjustment, and two
  readers on one socket would swallow each other's packets.
- **A backgrounded app can outlive the session that owns its stdout.**
  `espix_proc_hangup()` kills anything still owned by a session before its
  stream is closed — which is what a hangup means on a real terminal.

Redirection is deliberately *not* wired into this: `session->redirect` is closed
when the command returns, and `run app > file &` would leave an app holding a
dead `FILE *`. So `>` still captures a command's output and not an app's.

### The login greeting is a command

Both transports open with the same fetch-style block — logo left, system facts
right — and neither renders it. `espix_shell_exec(s, "motd")` does, from
[cmd_motd.c](../components/espix_cmds/cmd_motd.c).

That indirection is the point. Reporting uptime, heap, rootfs usage and an IP
address means depending on `espix_kernel`, `espix_net`, `espix_fs` and `heap`;
`espix_cmds` already does, while `espix_shell` and `espix_ssh` do not and should
not acquire those dependencies to draw a banner. Routing through the registry
keeps the arrows pointing one way, and makes the greeting re-runnable as
`motd`, which is what anyone reaching for `fastfetch` wants anyway.

Colour is gated on `session->ansi`, which the transport sets: the console takes
it from `linenoiseProbe()`, an SSH session always has a pty here.

There is no boot-time banner. The console session prints the greeting once the
boot barrier releases, so boot shows kernel log lines and then a login — the
order Unix uses, and the reason the two no longer overwrite each other.

### A connection owns its session keys, and `free()` cannot reach them

`ssh_conn_t` holds `mbedtls_svc_key_id_t` values, which are *identifiers* — the
key material lives in mbedTLS's PSA key store. So `free(c)` releases the struct
and orphans four slots (a cipher and a MAC key for each direction) plus two
cipher operations, every connection. `ssh_kex_release_keys()` exists to give
them back and is called from the connection teardown beside
`kexinit_c_release()`, on every path including the ones where the handshake
never finished.

This was a real leak before it was a rule: roughly 200-400 bytes of internal
heap per connection, never recovered, about 4000 connections from exhausting
RAM. It stayed quiet because `MBEDTLS_PSA_KEY_STORE_DYNAMIC` is on by default in
tf-psa-crypto, so slots are heap-allocated with no fixed count — under the
static key store it would have failed a key exchange after
`MBEDTLS_PSA_KEY_SLOT_COUNT / 4` connections, which is a far easier symptom to
chase than a slow drain.

The general shape, worth remembering for anything else that reaches for PSA:
if a struct holds a PSA identifier, freeing the struct is not releasing the
resource.

### `exec` and `shell` are one dispatch with two sources of lines

`ssh host <cmd>` and an interactive SSH shell run the same code. The difference
is only where the line comes from: `esp_linenoise` in one case, a string out of
the `exec` channel request in the other. Everything a terminal implies — the
editor instance, history, the motd, `login` being true so `logout` means
something — belongs to the shell path and not to exec.

Two things were pulled out of the shell path so the two could not drift, rather
than copied into the second one:

- `espix_shell_run_line()` in `espix_shell`. `espix_shell_exec()` returns
  `ESPIX_SHELL_ENOENT` for a first word it cannot resolve, and turning that into
  `command not found` and status 127 used to live in the REPL. Exec hands its
  status straight to the client, so calling `exec()` directly would have
  reported `-127` and printed nothing.
- `finish_session()` in `ssh_channel.c`, which hangs up the session's processes
  and then *probes* the transmit lock rather than waiting on it, because a
  process killed by the hangup may have died holding it. That subtlety was
  written once for the shell and applies just as much to `ssh host 'app &'`.

Output cooking is decided per channel, not per build. `send_cooked()` turns
every `\n` into `\r\n`, which is what a terminal wants and what a pipe does
not: `ssh host 'cat /etc/hostname' > file` would otherwise put carriage returns
in the file. So an exec channel with no `pty-req` writes raw, and `ssh -t host cmd`
— which does send one — gets cooking and `ansi`. The interactive path is
unconditionally cooked, deliberately unchanged.

### The app network ABI is ours

lwip's `getaddrinfo`, `inet_ntop` and `ntohs` are *macros* over
`lwip_getaddrinfo`, `lwip_inet_ntop` and `lwip_htons`, so the symbol an app's
object file actually references is the prefixed one — that is what
[abi.c](../components/espix_net/abi.c) exports via `esp_elf_register_symbol()`.
`ntohs`/`ntohl` need nothing, being macros onto `htons`/`htonl`, which the
loader already exports. `gai_strerror` exists nowhere in lwip or esp_libc, so
apps cannot use it.

This is the seam where espix's syscall surface becomes something espix owns
rather than inherits, and it costs no fork of the loader component.

## Two config traps, both hit once already

Worth knowing before editing `sdkconfig.defaults` or a component `Kconfig`:

- **`sdkconfig.defaults` only seeds a *new* `sdkconfig`.** It does not override
  a value already present in the generated file. Changing a default that has
  already been baked in requires deleting `sdkconfig` and re-running
  `idf.py reconfigure`, or editing the generated file directly.
- **An undefined bool is indistinguishable from `n`.** A `default y` Kconfig
  option that fails to regenerate silently compiles to its off path with no
  warning. espix therefore names boolean options as opt-*outs* where it can —
  `ESPIX_KLOG_QUIET` (default `n`) rather than `ESPIX_KLOG_ECHO_CONSOLE`
  (default `y`) — so a missing symbol yields the behaviour we wanted anyway.

Related: keep `CONFIG_LOG_MAXIMUM_LEVEL` at INFO. It is a compile-time ceiling,
not just a runtime permission — `espcoredump`'s macros test `LOG_LOCAL_LEVEL`
at compile time and write via `esp_rom_printf`, bypassing the runtime level
entirely, so a DEBUG ceiling buries every crash under a page of tracing (and
costs ~6 KB of format strings).

## What is not done yet

Open work has moved out of this file, so that it stays what its title says —
why things are the way they are, rather than what they are not yet.

- [ROADMAP.md](ROADMAP.md) — work espix might take on, and what it would cost.
- [KNOWN-ISSUES.md](KNOWN-ISSUES.md) — behaviour already implemented that will
  still surprise you.
- [UPSTREAM.md](UPSTREAM.md) — defects in ESP-IDF and its components, with the
  workaround espix carries for each.
