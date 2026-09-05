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
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). What is not done yet lives beside
it: [ROADMAP.md](docs/ROADMAP.md) for work espix might take on,
[KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) for behaviour that will surprise you,
and [UPSTREAM.md](docs/UPSTREAM.md) for defects that belong to ESP-IDF.

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

That choice has real costs — every file espix creates for itself is written
inside that window — and is the sort of thing worth reading the reasoning on
before changing it; see **Networking and time** in
[docs/ROADMAP.md](docs/ROADMAP.md#networking-and-time).

`/etc/timezone` holds a **POSIX TZ string**, not a zoneinfo name — espix ships
no tzdata, so the rules live in the string: `SAST-2`, `EST5EDT,M3.2.0,M11.1.0`,
or `UTC0` (the offset is not optional). `timedatectl set-timezone` writes it.

### Copying files on and off

```bash
scp build/hello.app.elf esp@esp32s3-cb5d74:/bin/hello
scp esp@esp32s3-cb5d74:/etc/hostname .
sftp esp@esp32s3-cb5d74
```

This is how an app reaches the device — build it on a PC, copy it into `/bin`,
run it by name. No reflashing the filesystem. To build one, see
[tools/README.md](tools/README.md).

espix implements the SFTP subsystem that OpenSSH 9 and later use for `scp` by
default, so plain `scp` and graphical clients both work, with no `-O` needed. A
transfer is checked against the same permissions a shell login would face — the
two doors agree — and a client starts in its own home directory. Permissions a
client sends are applied, except setuid, setgid and sticky, which are masked off
rather than refused so that one bit cannot fail an entire `scp -p`. File size is
not a limit: a write is streamed to the file as it arrives rather than
reassembled in memory.

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

What the MMU rows in the matrix below rest on, since "has an MMU" covers two
quite different things:

- **The S31 has the kind that matters.** Espressif shipped a developer preview
  of a Linux BSP for it in August 2026, and there are community RV32 Linux ports
  running on the hardware. If Linux boots, per-process address spaces and
  therefore `fork()` are available to espix too — which is why those rows say
  *planned* rather than *no* now.
- **The P4's is an address-translation MMU**, documented by ESP-IDF as mapping
  physical to virtual so flash and PSRAM can be reached through a pointer. That
  plus RISC-V PMP gives region-based protection between tasks — an MPU-shaped
  boundary, not a `fork()`-shaped one. **Still to be confirmed against the P4
  Technical Reference Manual** rather than promised: what espix would get there
  is most likely fault isolation between tasks, not copy-on-write.

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
| Shell scripts, `#!`, control flow | **planned** | the executable bit is now real, so `#!` needs only the dispatch: a file that is executable but not an ELF is where the interpreter line would be read |

### Processes

| | | |
|---|---|---|
| Run a cross-compiled native app by name | **yes** | `/bin` search, argv, exit status |
| `ps`, `top` | **yes** | live CPU and memory, per core |
| Stop a running app — Ctrl-C, `kill` | **yes** | Ctrl-C is SIGINT; `kill` asks, then insists |
| Signals and handlers — `signal()`, `kill -9`, `-STOP`/`-CONT` | **yes** | real POSIX names; delivered when the app calls in, not asynchronously |
| `kill -l`, `ps` showing `T` for stopped | **yes** | |
| A crashing app not taking the system down | **planned** | intercepts and reports; does not yet reap |
| `grep`, `sed`, `head`, `tail`, `wc`, `sort`, `find` | **planned** | |
| `sleep` | **planned** | |
| Apps using the filesystem | **yes** | `fopen`, `opendir`, `stat`, `chmod`; `stat` reports the same mode `ls -l` shows |
| Per-process working directory | **yes** | an app's `chdir()` does not move the shell that ran it |
| `fork()` / `exec()` | **no** on S3, **planned** on S31 | needs an MMU for copy-on-write; the S31 has one |
| MMU-backed process isolation | **no** on S3, **planned** on S31 | see [Hardware Targets](#hardware-targets) and [Crash handling](#crash-handling-and-isolation) |
| setuid / setgid / sticky | **yes** | all three consulted; setuid is a guardrail on S3 and a boundary on S31 |

### Filesystem

| | | |
|---|---|---|
| LittleFS mounted as the real `/` | **yes** | survives reboot and firmware reflash |
| `ls` `cd` `pwd` `cat` `cp` `mv` `rm` `mkdir` `touch` `chmod` `df` | **yes** | |
| `ls -1ahltr` | **yes** | sorted by name, or by mtime with `-t`; `-R` is not implemented |
| `ls -i`, inode numbers | **no** | esp_littlefs reports `d_ino = 0` for every entry, and LittleFS exposes no file id |
| Per-session working directory | **yes** | your `cd` is not someone else's |
| File timestamps | **yes** | `ls -l` and `sftp ls -l` show mtime; files from the flashed image have none |
| `/proc`, `mount` / `umount` | **planned** | espix owns `/` but routes only `/`; a second mount needs its own routing table |
| Mode bits, `chmod` | **yes** | all twelve, octal or symbolic; `ls -l` and `sftp ls -l` show the same thing |
| An executable bit | **yes** | enforced — `chmod -x` stops a program running. A new binary is executable without anyone setting it |
| Read and write bits enforced | **yes** | in espix's root VFS, so builtins, loaded apps and SFTP are all checked the same way |
| `chown`, `chgrp`, owner and group | **yes** | stored per file, plus a rule so an unstamped rootfs still answers |
| setuid, setgid, sticky | **yes** | each consulted; `/tmp` is `1777` and sticky is what makes that safe |
| Symlinks, `ln` | **no** | cost, not principle: LittleFS has no link type, and following one means loop detection in every path lookup |

### Networking

| | | |
|---|---|---|
| WiFi station, DHCP lease, default route | **yes** | `wlan0`, reconnects on boot |
| `ip`, `ifconfig`, `route`, `ping` | **yes** | `ping` resolves names |
| SSH server | **yes** | password auth — [read this first](#a-word-on-the-ssh-server) |
| `scp` / `sftp` | **yes** | SFTP subsystem, permission-checked like the shell; starts in your home |
| Ethernet | **planned** | P4 and S31 (Original ESP32 also has) |
| USB-NCM | **planned** | IP network to USB host |
| `ssh host <cmd>` | **partial** | works, but can truncate a long command's output — see [KNOWN-ISSUES](docs/KNOWN-ISSUES.md) |
| SSH publickey auth, rekeying | **planned** | a long session is dropped today |
| Time of day, over NTP | **yes** | `date`, `timedatectl`; server from DHCP option 42, else `pool.ntp.org` |

### Users

| | | |
|---|---|---|
| Password authentication | **yes** | PBKDF2-SHA256, per-user salt, `/etc/passwd` |
| `passwd`, `whoami`, `id` | **yes** | `passwd` refuses to change another account's, unless root |
| More than one account | **yes** | `useradd` allocates a free uid; 8 accounts and 12 groups |
| uid/gid and file ownership | **yes** | stored per file, plus a rule so an unstamped rootfs still answers |
| Enforced read/write/execute | **yes** | in espix's VFS, for builtins and loaded apps alike |
| `chown`, `chgrp` | **yes** | changing an owner is root's, as in chown(2) |
| `sudo`, `sudo -u <user>` | **yes** | gated by `/etc/sudoers`, which takes names or `%group`; does not re-prompt, see below |
| `su` | **no** | `sudo` covers the need, and `su` wants the password prompt espix cannot give |
| Groups with members | **yes** | `/etc/group`, supplementary membership, and the group triad actually checked |
| `useradd`, `userdel`, `usermod` | **yes** | `-r` for a service account: locked, low uid, no home |
| `groupadd`, `groupdel`, `groups` | **yes** | |
| A root for an app, `run -R <dir>` | **yes** | it cannot *name* a path outside, which is the question permissions never ask |
| Restricting what an app may call | **partial** | the ELF loader's export table is one, but it is fixed rather than per-app |
| An editor | **no** | no `nano` or `ed`, so editing a config on the device means `echo >` |

Those two rows were one row saying "per-app capabilities", which conflated a
*filesystem view* with a *subset of what an app may call*. They are different
things, and only the first is done.

**Why a root, when the app already runs as its own user?** Because users answer
"may uid X open path P" and never stop P being named. The mode rule here hands
out `0755` directories and `0644` files, so a service account can walk the whole
tree and read all of it bar what someone remembered to lock — and the two things
nobody had remembered were `/etc/wifi.conf`, holding the WiFi PSK, and the SSH
host private key, both world-readable until this landed. Discretionary
permissions start out open and are closed by exception; a root is the other
default, where nothing is reachable but what was handed over. It also survives
getting the uid wrong, and it is per *app* rather than per *user*, which two
services sharing an account cannot otherwise be.

It restricts rather than chroots: paths stay globally absolute, so the app sees
`/srv/www/db` and not `/db`. A real chroot needs bind mounts to be worth
anything — a jail with no `/bin`, no `/etc` and no `/tmp` is not somewhere a
program runs — and mounts are still on the roadmap. The binary is read before
the confinement starts, exactly as `execve` does it, so it may live outside.

Worth being clear about what any of this can buy on a chip with no MMU: an app
shares the address space with the kernel, so filesystem permissions and a root
alike are a guardrail against mistakes rather than a sandbox around hostile
code. The real boundary is the ELF loader's export table — an app can only call
what espix publishes to it. Permissions make that boundary usable; they do not
replace it. The same caveat applies to setuid, which is implemented because the
S31 makes it a real boundary rather than because it is one on the S3.

Root follows the model Debian uses: the account exists but is locked, so nothing
can log in as it, and `sudo` is how you reach it. `sudo passwd root <pw>` gives
it a password if you want one, and `passwd -l root` takes it away again.
`/etc/sudoers` is seeded with `%sudo`, so membership of that group is what
grants it — the arrangement Debian ships, where RHEL would say `%wheel`.

Services get their own identity rather than running as whoever started them:
`useradd -r www` makes a locked account with a low uid and no home, and
`sudo -u www /bin/httpd &` runs the app as it. Add `run -R` and it gets its own
view of the filesystem as well — `sudo -u www run -R /srv/www /bin/httpd &` is
an account that owns nothing else and a process that can see nothing else. No
service manager is involved — that is the whole mechanism.

**`sudo` does not ask for your password.** espix cannot read input without
echoing it — the same limitation that makes `passwd` take the password as an
argument — so a prompt would print the thing it was protecting. The session is
already authenticated, and sudo(8)'s timestamp caching means a real one often
does not re-ask either, but an unattended terminal is a way in that Linux would
have closed. It is the first thing to fix when the reentrant line editor lands.

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

## Why not NuttX or Zephyr?

Read this section first. It is the one most likely to talk you out of espix,
which is why it comes before the others.

**[Apache NuttX](https://nuttx.apache.org/) already does most of what espix
does, on this chip, and has for years.** It loads ELF programs off a filesystem
and runs them by name from its shell (`CONFIG_ELF`, `CONFIG_NSH_FILE_APP`,
`CONFIG_LIBC_ENVPATH`) — which is espix's headline feature. It supports
LittleFS. It serves a shell over SSH, via a Dropbear port in `netutils`, with
password authentication and an ECDSA P-256 host key, which is feature for
feature what espix's SSH server does. It has WiFi with WPA3, BLE, SMP and most
ESP32-S3 peripherals. POSIX and ANSI compliance are stated project goals, not
aspirations.

It was also ahead where espix had written down that it was stuck. With
`CONFIG_SCHED_USER_IDENTITY` NuttX tracks a task's real and effective UID/GID
and enforces file permissions in the VFS; espix stored permission bits and
enforced only the execute one, because an app reached the filesystem through
libc and the VFS underneath had no idea which process was calling. That gap is
closed — espix owns the root VFS now, so the question is answerable here too,
and uid, gid and enforcement all landed on top of it. NuttX keeps the advantage
on what surrounds it: real groups, `su`, and a task model that was designed for
this rather than fitted to it.

**The actual difference is that espix is additive to ESP-IDF and NuttX is an
alternative to it.** Choosing NuttX means leaving `idf.py`, the component
registry, ESP-IDF's driver model, `esp_event`, NVS and OTA behind, and that is
not a porting detail: parts of ESP-IDF are written against FreeRTOS
synchronisation primitives, so IDF drivers and managed components do not travel
to another RTOS. espix's own dependencies — the ELF loader, the line editor,
the LittleFS port — are ESP-IDF components and would all have to be replaced.

So the claim espix can defend is narrow: *you already have an ESP-IDF codebase,
and you want a shell, a filesystem and runtime app loading without re-basing
onto a different operating system.* If that is not your situation, NuttX is
probably the better answer, and this README would rather say so than have you
find out later.

One concrete thing espix does better, for completeness: NuttX's Dropbear port
implements no SFTP, so file transfer needs `scp -O` and the pre-9.0 protocol.
espix implements the SFTP subsystem, so a current `scp` works unmodified.

**[Zephyr](https://www.zephyrproject.org/)** is the same trade with a different
ecosystem. Espressif supports it, and LLEXT gives it runtime-loadable ELF
extensions, though that path is younger on ESP32 than NuttX's ELF loader.
Zephyr's shell and filesystem are subsystems of an application rather than a
Unix userland, which is a different thing to want.

No performance or footprint comparison is offered here, because none has been
measured.

## Why not Linux on ESP32?

- **[GrieferPig/esp32-s31-linux](https://github.com/GrieferPig/esp32-s31-linux)**
  — genuine MMU RV32 Linux 6.18 booting natively on an ESP32-S31, executing in
  place from flash with a Buildroot rootfs, and experimental WiFi, Bluetooth and
  dual-core SMP. Self-described as experimental and not for production. It is
  the strongest evidence that this is possible at all, and it makes espix's case
  as much as its own: it needs an MMU part and 16MB of PSRAM alongside 16MB of
  flash to get there. espix targets the S3, which has no MMU.
- **[nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)**
  and **[paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)**
  — inspiration for the "why not just run something Linux-like on this chip"
  idea. Neither publishes clear numbers on flash/RAM headroom left for real
  applications after boot, which is the resource budget espix is explicitly
  designed around.

espix is deliberately not on this path. It is not a Linux kernel and does not
try to be binary-compatible with one; the target is a nommu-Linux-*style*
environment purpose-built for ESP-IDF, on hardware that cannot run the real
thing.

## Why not Esp32OS?

**[faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)** is the nearest
neighbour — a similar idea (FreeRTOS + ESP-IDF with a Linux-style shell,
`ps`/`top`/`free`/`dmesg`, process management commands). Good prior art for
shell ergonomics, but its "processes" are FreeRTOS tasks managed through a shell
layer, it uses SPIFFS (LittleFS is only a roadmap item there), and it has no ELF
loader or dynamic native app loading — which is espix's core requirement.
Because that requirement implies a different architecture (loader, syscall
boundary, LittleFS from day one) rather than incremental features, espix is
being built fresh instead of forked. Esp32OS is MIT-licensed with
attribution/branding requirements (see its `NOTICE.md` / `BRANDING_POLICY.md`);
any code directly reused from it will retain its license notice and be credited
here.

## License

MIT — see [LICENSE](LICENSE).

Any code directly incorporated from other MIT-licensed projects (e.g. Esp32OS)
retains its original license notice; see individual file headers / a `NOTICE.md`
once added. None is incorporated today — `espressif/elf_loader`,
`espressif/esp_linenoise` and `joltwallet/littlefs` are fetched at build time by
the IDF component manager rather than vendored into this tree.

`joltwallet/littlefs` is the one exception to "fetched and left alone": the
build adds two things to the downloaded copy — a public custom-attribute API,
because espix keeps a file's mode in a LittleFS user attribute and the port
exposes no way to reach one, and a mount-only entry point, because espix
registers the root VFS itself and needs the filesystem mounted without a name
of its own. Nothing is copied into this tree — see
[tools/patch-littlefs.py](tools/patch-littlefs.py) and
[docs/UPSTREAM.md](docs/UPSTREAM.md) — and the patch is written to be sent
upstream, at which point it goes away.

## Acknowledgements

- [Apache NuttX](https://nuttx.apache.org/) — prior art for nearly all of this,
  and the honest first stop for anyone who does not need to stay on ESP-IDF
- [GrieferPig/esp32-s31-linux](https://github.com/GrieferPig/esp32-s31-linux)
- [nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)
- [paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)
- [faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)
- Espressif ESP-IDF / FreeRTOS
