# espix architecture

Notes on how the skeleton is put together and why. Design decisions that are
non-obvious from the code, and the ones that were verified against ESP-IDF
rather than assumed.

Target of record for this skeleton: **ESP32-S3, 16MB flash, 8MB octal PSRAM**,
on **ESP-IDF v6.1-beta1**.

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

### LittleFS is mounted as the real root `/`

`esp_vfs_littlefs_register()` is called with `base_path = ""`. ESP-IDF
documents the empty base path as registering a *fallback* VFS that handles any
path no other VFS claims, so paths are `/bin/hello` and `/etc/motd` rather than
`/storage/bin/hello`, while `/dev/*` still resolves to its own driver by
longest-prefix match.

This is documented but not a well-trodden path. If it misbehaves, the fallback
is to mount at `/storage` and add a rewrite shim — contained to `espix_fs`.

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

### There is no working directory outside a session

ESP-IDF has no process-wide CWD at all: `chdir()` is a hardcoded
`errno = ENOSYS; return -1;` stub and `getcwd()` unconditionally answers `"/"`
(`components/esp_libc/src/realpath.c`). espix's cwd therefore lives only in
`espix_session_t.cwd`, and every command resolves through `espix_fs_resolve()`
to an absolute path before touching the filesystem.

The consequence lands on the app ABI: **a loaded app that calls
`fopen("data.txt")` gets `/data.txt`**, never the directory you `cd`'d into.
`run` resolves its own `argv[0]` against the session cwd, so
`cd /home && run ./app` works, but an ambient cwd is not something apps can rely
on. Apps should take paths as arguments. If this becomes painful, the fix is
`--wrap` on `getcwd`/`chdir` backed by a per-task cwd and exported through the
ELF symbol table — deliberately not done yet, because it is an addition to the
app ABI and wants designing rather than bolting on.

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

Dumb mode is not merely "no line editing". It has two defects that corrupt
input:

- The line is terminated one byte late — `buffer[count + 1] = '\0'` in
  `esp_linenoise_dumb()` — so `buffer[count]` keeps a stale byte from the
  previous command. `df` typed after `whoami` runs as `dfo`, because `buf[2]`
  is still the `o`.
- ESC is `<= UNIT_SEP`, so it is dropped as non-printable while the rest of the
  sequence is kept: an arrow key is entered as the literal text `[A`.

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

### Asynchronous output clobbers the prompt, and that is accepted

The session task calls `linenoise()`, which writes the prompt and blocks on
read. Anything logged from another task — a link event, a timer — writes to
stdout meanwhile, so the line becomes `espix:/# I (1340) wifi:...` and the
prompt is gone. `refreshLine()` is `static` in linenoise, so no caller outside
that library can ask for a redraw.

There is a second-order effect worth knowing before it looks like a bug. espix
registers a hints callback for the `usage` strings, and `linenoiseEdit`'s ENTER
case calls `refreshLine()` to strip the hint before the newline. That rewrites
the *current* line, so with the prompt still where linenoise left it the refresh
is invisible and Enter yields one prompt. Once async output has moved the cursor
to a fresh line, the refresh draws a prompt *there*, `linenoiseRaw()` then emits
`\n`, and the session loop draws another — so Enter appears to produce two
prompts. It is a symptom of the splat, not an independent fault.

espix does not try to repair the line. Coupling klog to the shell so it could
wipe and redraw would cost a callback seam and still lose half-typed input,
which linenoise does not expose. Instead the noise is cut at the source: the
WiFi driver's chatty tags are lifted to WARN in `quiet_driver_logs()`, leaving
espix's own four link messages, which are the ones worth seeing. Kernel messages
landing on your terminal is what Linux does too.

The honest long-term answer is a runtime console threshold — `dmesg -n` — rather
than a fixed list of quieted tags. Deferred.

One case is not left alone, because it is not occasional: at boot the splat is
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

## Deferred

- **OTA slots.** The partition table is `factory`-only. Two 4MB OTA slots plus
  `otadata` would cost ~4MB of the 11.9MB rootfs but allow kernel updates over
  the network. Changing this later means reflashing everything, so it is worth
  deciding before the layout is in the field. A commented-out variant is in
  [partitions/esp32s3-16mb.csv](../partitions/esp32s3-16mb.csv); the 8MB table
  notes why the same shape does not fit there.
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
- **`dmesg -n`** — a runtime console loglevel, replacing the hardcoded list of
  quieted driver tags.
- **A text editor.** There is none. `echo >` and `>>` cover `key=value` config,
  which is why it has not bitten yet, but anything larger wants an `ed`-style
  line editor.
- **WiFi roaming and multiple networks.** One SSID, one AP, no BSSID
  reselection.
- **`/etc/motd`.** The file exists in the rootfs but nothing reads it; the
  console prints a fixed banner instead. Printing motd at session start is the
  right home for it, and worth doing when SSH makes "logging in" a real event.
- **`espix_net`**, and SSH/SCP on top of it.
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
