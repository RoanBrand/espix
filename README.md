<p align="center">
  <img src="docs/banner.png" alt="espix login greeting: ASCII logo beside OS, host, uptime, memory, storage and network" width="80%" max-width="800px">
</p>

# espix

A Unix(-like) kernel/runtime environment for ESP32, built on ESP-IDF.

espix brings the parts of the Unix operational model that are useful on
a microcontroller — a real shell, filesystem, networking, and the
ability to cross-compile a native app on a PC and load it at runtime — while
leaving enough flash and RAM for those apps to do something. It is deliberately
not a Linux-compatible kernel; the target is closer to a nommu-Linux-style
environment purpose-built for ESP-IDF.

Design notes and the reasoning behind the structure are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

Early, but running on hardware. Verified on an ESP32-S3 (16MB flash, 8MB octal
PSRAM) with ESP-IDF v6.1-beta1: LittleFS mounted as the real `/`, a
transport-agnostic shell with 37 commands, a process table, and — the point of
the exercise — an app cross-compiled on a PC, copied over as a file, and loaded
and executed at runtime with argv and an exit status.

WiFi comes up as `wlan0` and an SSH server serves the same shell as the serial
console, so `scp` puts an app in `/bin` and you run it by name. Files survive a
reboot and a firmware reflash.

Fault interception is wired but only *reports* — see
[Crash handling and isolation](#crash-handling-and-isolation). For the full
picture of what is and is not implemented, see the
[feature matrix](#feature-matrix).

## Getting started

**Requires ESP-IDF v6.1**, checked out by tag. It is still a beta, so a fresh
`install.sh` gives you 5.x or 6.0 unless you ask for it — and those will not
work: espix's SSH and password hashing are written against PSA Crypto, which
arrives with Mbed TLS 4.x in 6.1. Older releases are refused up front rather
than failing halfway through a compile.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3                              # see Hardware Targets
idf.py -p /dev/ttyUSB0 flash storage-flash monitor     # macOS: /dev/cu.usbserial-*
```

**That flash command writes two images, and a first boot needs both.** `flash`
writes the firmware; `storage-flash` writes the rootfs — `/bin`, `/etc` and the
rest. Flashing only the firmware leaves you at a shell with an empty filesystem.

There is no separate download or configure step. `set-target` fetches the
managed components at the versions pinned in `dependencies.lock` — it needs
network the first time — and generates `sdkconfig` from the `sdkconfig.defaults*`
files. No `menuconfig` required.

Boot prints kernel messages, then the greeting at the top of this README, with
`Network` reading `not connected` until you join one. `help` lists every
command; `motd` reprints the greeting.

### Updating later

```bash
idf.py -p /dev/ttyUSB0 flash            # firmware only; leaves your files alone
idf.py -p /dev/ttyUSB0 storage-flash    # WARNING: replaces the whole rootfs
```

Keeping them separate is deliberate: reflashing firmware should not destroy what
is on the device.

### Board variants

The default targets an **N16R8** module — 16MB flash, 8MB octal PSRAM, as on the
ESP32-S3-DevKitC-1 v1.1. That is the only variant espix has actually been run
on; the rest are build-verified only.

| File | Module | Flash | PSRAM |
|---|---|---|---|
| *(none — the default)* | N16R8 | 16MB | 8MB octal |
| [boards/esp32s3-n8r8.conf](boards/esp32s3-n8r8.conf) | N8R8 | 8MB | 8MB octal |
| [boards/esp32s3-n8r2.conf](boards/esp32s3-n8r2.conf) | N8R2 | 8MB | 2MB quad |
| [boards/esp32s3-n8.conf](boards/esp32s3-n8.conf) | N8 | 8MB | none |

Board files only seed a *new* `sdkconfig`, so switching board on an existing
checkout means removing it first:

```bash
rm -f sdkconfig
SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-n8r8.conf" \
    idf.py set-target esp32s3
idf.py -p /dev/ttyUSB0 flash storage-flash monitor
```

Reflash the rootfs too when the flash size changes — the partition table moves,
so whatever was at the old offset is no longer there. Partition tables live in
[partitions/](partitions/) and are selected by the board file.

A board with **no PSRAM** builds and falls back to internal RAM, but WiFi, lwIP,
SSH and the app image then compete for ~343K instead of 8MB. Expect small apps
to work and larger ones to fail on allocation. Untested; reports welcome.

## Using it

### Joining a WiFi network

```
root:/# wifi connect <ssid> <passphrase>
```

That associates immediately *and* writes `/etc/wifi.conf`, so every later boot
reconnects on its own. `wifi status`, `wifi scan`, `ip addr` and `route` report
where it got to. The hostname is derived from the MAC — `esp32s3-cb5d74` — and
`hostname <name>` changes it.

### Logging in over SSH

`sshd` listens on port 22 from boot. Once `ip addr` shows an address:

```bash
ssh esp@esp32s3-cb5d74          # or the IP; .lan works if your router adds it
```

**The shipped account is `esp`, password `espix`**, and every login says so
until you change it with `passwd esp <new-password>`. Passwords are stored
hashed, never in plaintext.

The host key is generated on first boot and its fingerprint printed on the
serial console, so you can compare it against what your client shows:

```
espix: sshkey: host key SHA256:Sts8sx9+JuATlAMgo/iW1qYjBTbel+wXeXb7E2V2xhg
```

`exit` ends an SSH session and takes an optional status; on the console it
starts a fresh session instead, since there is no login to fall back to and a
device with no shell would be worse than useless.

### The clock

There is no battery-backed RTC, exactly as on a Raspberry Pi, so real time comes
from the network. SNTP starts as soon as any interface has an address, taking
the server from the DHCP lease (option 42) if one is offered and falling back to
`pool.ntp.org`. Put a `server=` line in `/etc/ntp.conf` to override both.

```
root:/# date
Mon 31 Aug 2026 09:12:53 UTC
root:/# timedatectl
               Local time: Mon 2026-08-31 11:12:53 SAST
           Universal time: Mon 2026-08-31 09:12:53 UTC
                Time zone: SAST-2
System clock synchronized: yes (4min ago)
               NTP server: 192.168.110.1 (dhcp)
```

**Before the first sync the clock reads 1970**, deliberately — an obviously
wrong date is a better signal than a plausible one, and `timedatectl` says so.
Files written in that window carry 1970 timestamps, which is the truth about
them. A `reboot` keeps the time, because ESP-IDF holds it in an RTC register
that survives a restart; only a power cycle starts over. `date -s` sets it by
hand where there is no network.

`/etc/timezone` holds a **POSIX TZ string**, not a zoneinfo name — espix ships
no tzdata, so the rules live in the string: `SAST-2`, `EST5EDT,M3.2.0,M11.1.0`,
or `UTC0` (the offset is not optional). `timedatectl set-timezone` writes it.

### Copying files on and off

```bash
scp build/hello.app.elf esp@esp32s3-cb5d74:/bin/hello
scp esp@esp32s3-cb5d74:/etc/motd .
sftp esp@esp32s3-cb5d74
```

This is how an app reaches the device — build it on a PC, copy it into `/bin`,
run it by name. No reflashing the filesystem. To build one, see
[tools/README.md](tools/README.md).

espix implements the SFTP subsystem that OpenSSH 9 and later use for `scp` by
default, so plain `scp` and graphical clients both work, with no `-O` needed.
Permissions a client sends are accepted and discarded, since there are no mode
bits to apply them to yet. File size is not a limit: a write is streamed to the
file as it arrives rather than reassembled in memory.

Downloads run at about 355KB/s over 2.4GHz WiFi. Uploads are much slower, and
bounded by LittleFS erasing a block per write rather than by the network — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the measurements and for where
the per-connection buffers live.

Two connections may be open at once, so a transfer can run while you are logged
in. The serial console stays independent, and `dmesg` is how a remote user reads
kernel messages.

## Hardware Targets

| Chip | Notes |
|---|---|
| **S3** | Most common / oldest of the three targets. Xtensa ISA, not RISC-V. No MMU available. |
| **P4** | Best for "desktop"-style use — best display output (MIPI DSI, HDMI variant exists), Ethernet (100M). Weaker MMU than S31 (not enough for real Linux-style isolation). **No built-in WiFi/BT** — needs a companion chip (typically ESP32-C6) over SDIO/SPI for wireless. |
| **S31** | Latest ESP32. CPU freq (320mhz) is lower than the P4, but has higher IPC efficiency. Has a real MMU. Gigabit Ethernet. Weaker/limited display output vs P4.  |

Support priority and per-chip feature availability (isolation model,
display, networking) still to be finalized as the design matures.

## Feature matrix

What works today, against the Unix surface people expect. **planned** means
intended but not built yet; **no** means deliberately out of scope rather than
merely missing.

### Shell

| | | |
|---|---|---|
| Interactive shell over serial and SSH | **yes** | same dispatch, same output on both |
| Line editing, history, TAB completion | **yes** | history follows the user, not the connection |
| Output redirection `>` `>>` | **yes** | |
| Quoting and backslash escapes | **yes** | |
| Exit status | **yes** | `exit 3` reaches an SSH client's `$?` |
| Background jobs `&` | **partial** | `run cmd &` works; no `jobs`, `fg`, `bg`, Ctrl-Z |
| Pipes <code>&#124;</code> | **planned** | |
| Input redirection `<`, `2>` as its own stream | **planned** | one output stream today |
| Environment variables, `export` | **planned** | there is no environment at all yet |
| Globbing `*` | **planned** | |
| Shell scripts, `#!`, control flow | **planned** | `#!` shares a dispatch point with the executable bit: `run` already sniffs the ELF magic to decide what a file is |

### Processes

| | | |
|---|---|---|
| Run a cross-compiled native app by name | **yes** | `/bin` search, argv, exit status |
| `ps`, `top` | **yes** | live CPU and memory, per core |
| Stop a running app — Ctrl-C, `kill` | **yes** | cooperative: the app polls a flag |
| Real signals and handlers | **planned** | `kill` cannot force today |
| A crashing app not taking the system down | **planned** | intercepts and reports; does not yet reap |
| `grep`, `sed`, `head`, `tail`, `wc`, `sort`, `find` | **planned** | |
| `sleep` | **planned** | |
| `fork()` / `exec()` | **no** | no MMU, no copy-on-write |
| MMU-backed process isolation | **no** on S3 | possible later on S31 — see [Crash handling](#crash-handling-and-isolation) |

### Filesystem

| | | |
|---|---|---|
| LittleFS mounted as the real `/` | **yes** | survives reboot and firmware reflash |
| `ls` `cd` `pwd` `cat` `cp` `mv` `rm` `mkdir` `touch` `df` | **yes** | |
| Per-session working directory | **yes** | your `cd` is not someone else's |
| File timestamps | **yes** | `ls -l` and `sftp ls -l` show mtime; files from the flashed image have none |
| `/proc`, `mount` / `umount` | **planned** | |
| Mode bits, `chmod`, `chown` | **planned** | LittleFS has no native mode bits, but custom attributes can carry them — the mtime the port already stores works exactly that way |
| An executable bit | **planned** | today espix gates on the ELF header instead, which is why a non-program answers `Exec format error` |
| Symlinks, `ln` | **no** | cost, not principle: LittleFS has no link type, and following one means loop detection in every path lookup |

### Networking

| | | |
|---|---|---|
| WiFi station, DHCP lease, default route | **yes** | `wlan0`, reconnects on boot |
| `ip`, `ifconfig`, `route`, `ping` | **yes** | `ping` resolves names |
| SSH server | **yes** | password auth — [read this first](#a-word-on-the-ssh-server) |
| `scp` / `sftp` | **yes** | SFTP subsystem; enough for `get`, `put`, `ls`, `cd`, `mkdir`, `rm` |
| Ethernet | **planned** | P4 and S31 (Original ESP32 also has) |
| USB-NCM | **planned** | IP network to USB host |
| SSH publickey auth, rekeying | **planned** | a long session is dropped today |
| Time of day, over NTP | **yes** | `date`, `timedatectl`; server from DHCP option 42, else `pool.ntp.org` |

### Users

| | | |
|---|---|---|
| Password authentication | **yes** | PBKDF2-SHA256, per-user salt, `/etc/passwd` |
| `passwd`, `whoami` | **yes** | |
| More than one account | **partial** | the file format holds them; no `adduser` yet |
| uid/gid, file ownership, `su` | **planned** | a minimal user system, not a full POSIX one |
| `sudo`, groups, per-app capabilities | **planned** | so an app need not run as root or see the whole filesystem |

Worth being clear about what that last row can buy on a chip with no MMU: an app
shares the address space with the kernel, so filesystem permissions are a
guardrail against mistakes rather than a sandbox around hostile code. The real
boundary is the ELF loader's export table — an app can only call what espix
publishes to it. Permissions make that boundary usable; they do not replace it.

## A word on the SSH server

espix implements SSH itself rather than linking an existing one. The only SSH
server on the ESP component registry is GPL-or-commercial, which would have
forced the licence of any firmware image built on espix; mbed TLS was already
linked and its PSA Crypto API covers everything the protocol needs.

What that buys is one algorithm per role — `curve25519-sha256`,
`ecdsa-sha2-nistp256`, `aes256-ctr`, `hmac-sha2-256-etm@openssh.com` — with the
Terrapin (CVE-2023-48795) mitigation, and no negotiation logic to get wrong.

What it costs is stated plainly: **this is a hand-rolled implementation of a
security protocol and it has not been audited.** That is a reasonable trade on a
trusted LAN and a bad one facing the internet — do not port-forward it. The host
private key is also stored in plaintext on LittleFS, consistent with espix's
trusted-code model: anyone who can read the filesystem can impersonate the
device.

## Crash handling and isolation

espix does **not** provide MMU-based memory isolation between apps on chips that
lack a real MMU (S3, and P4 to a lesser extent — see
[Hardware Targets](#hardware-targets)). This is a deliberate, accepted tradeoff,
conceptually similar to nommu Linux: apps run in a shared address space and are
expected to be trusted, self-compiled code, not a security sandbox for untrusted
binaries.

What espix does today:

- A hook on the panic path (`-Wl,--wrap=esp_panic_handler`, the same seam
  ESP-IDF's own test suite uses) intercepts every fault — `LoadProhibited`,
  `StoreProhibited`, illegal instruction, watchdogs, aborts.
- It records the core, exception class, faulting address, task name and espix
  pid into memory that survives the reset, prints one line, and then delegates to
  the normal handler. The next boot reports the post-mortem on the console and in
  `dmesg`:

  ```
  espix: fault: previous boot: fault in task 'main' at 0x4200f226 (StoreProhibited), core 0
  ```

  The `crash` command triggers this on demand. `coredump` inspects the full dump
  ESP-IDF writes alongside it.

What it does **not** do yet — deliberately:

- Reap the faulting task and keep running. The reaper task and its queue exist
  (`espix_fault_request_reap()`), but nothing feeds them. Skipping the reboot is
  the easy half; the hard half is below, and shipping the easy half alone would
  produce a system that limps rather than one that recovers.
- Known limitations to design around rather than ignore:
  - It only catches invalid-memory-access faults, not general memory corruption
    (buffer overflows into valid memory, heap corruption, one task's wild write
    landing inside another task's stack or the kernel). Those go undetected, same
    as on nommu Linux.
  - Locks held by a reaped task (heap/malloc lock, VFS/filesystem mutex, driver
    mutexes) need explicit handling — timeout-based acquisition and/or per-app
    heap arenas — or a reaped task can wedge the rest of the system instead of
    just itself.
  - Stack overflow is treated as its own fault class; once it happens the task's
    stack contents can't be trusted, so recovery = reap, not "resume."
- Longer-term, the syscall/loader boundary should be designed to allow chips with
  a real MMU (S31) to eventually get actual MPU/MMU-backed process isolation as
  an opt-in, without requiring a rewrite. S3 stays in "catch and reap" mode
  regardless.

## Why not just use ESP32OS / esp32-running-linux / Linux-on-esp32-S3?

espix is inspired by prior art in this space, but diverges enough in
architecture that it's being built as an independent project rather than a fork:

- **[nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)**
  and **[paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)**
  — inspiration for the "why not just run something Linux-like on this chip"
  idea. Neither publishes clear numbers on flash/RAM headroom left for real
  applications after boot, which is the resource budget espix is explicitly
  designed around.
- **[faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)** — a similar
  idea (FreeRTOS + ESP-IDF with a Linux-style shell, `ps`/`top`/`free`/`dmesg`,
  process management commands). Good prior art for shell ergonomics, but its
  "processes" are FreeRTOS tasks managed through a shell layer, it uses SPIFFS
  (LittleFS is only a roadmap item there), and it has no ELF loader or dynamic
  native app loading — which is espix's core requirement. Because that
  requirement implies a different architecture (loader, syscall boundary,
  LittleFS from day one) rather than incremental features, espix is being built
  fresh instead of forked. Esp32OS is MIT-licensed with attribution/branding
  requirements (see its `NOTICE.md` / `BRANDING_POLICY.md`); any code directly
  reused from it will retain its license notice and be credited here.

## License

MIT — see [LICENSE](LICENSE).

Any code directly incorporated from other MIT-licensed projects (e.g. Esp32OS)
retains its original license notice; see individual file headers / a `NOTICE.md`
once added. None is incorporated today — `espressif/elf_loader`,
`espressif/esp_linenoise` and `joltwallet/littlefs` are fetched at build time by
the IDF component manager rather than vendored into this tree.

## Acknowledgements

- [nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)
- [paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)
- [faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)
- Espressif ESP-IDF / FreeRTOS
