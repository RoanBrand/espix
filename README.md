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
[UPSTREAM.md](docs/UPSTREAM.md) for defects that belong to ESP-IDF, and
[GOTCHAS.md](docs/GOTCHAS.md) for the ESP32/IDF surprises — where the platform
differs from what a POSIX or FreeRTOS habit expects, and what may not be used
together.

## Capabilities

<sub>Run on ESP32-S3N16R8 and ESP32-S31 (WROOM-3) with ESP-IDF v6.1; the ESP32-P4 is planned</sub>

| Category | | | |
|---|---|---|---|
| Storage | LittleFS mounted as the real `/` | **yes** | survives reboot and a firmware reflash |
| Storage | USB host: enumerate, identify, read the partition table (MBR and GPT) | **yes** | `lsusb`, `lsblk`, `blkid`; four device slots, for a hub |
| Storage | Mount a FAT volume into the namespace | **yes** | through espix's own VFS, so the permission check applies |
| Storage | `/etc/fstab`, applied on attach | **yes** | device column takes a name, a wildcard, or `LABEL=`/`UUID=`/`PARTUUID=` |
| Storage | `mount -o uid=,gid=` | **yes** | root hands a volume to a user without giving them root |
| Storage | `mount -o ro` | **yes** | refused by FatFs at the block device, not by policy; `/etc/fstab` takes `ro` too |
| Storage | Volumes mounted at once | **partial** | two FAT or exFAT volumes (`CONFIG_FATFS_VOLUME_COUNT`), two ext (`EXT_MAX_MOUNTS`), plus the rootfs |
| Storage | exFAT volumes | **yes** | on by default (`ESPIX_FS_EXFAT`, +7.0KB ROM, no static RAM); `FF_LBA64` and the 64-bit diskio fix come with it, so a volume past 2TiB is readable too |
| Storage | ext2/3/4 volumes | **yes** | read-only by default, writable with `mount -o rw`; a driver of espix's own, two mount slots. Writes go through the port's experimental extent implementation, and a volume without a journal cannot be mounted writable at all — see [KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) |
| Storage | `mkfs`: make a filesystem | **planned** | nothing is ever formatted today |
| Storage | Serve the stick over the network | **planned** | NFS or SMB, the NAS case |
| Programs | Run a native app: load, argv, exit status | **yes** | cross-compiled on a PC, copied over, run by name |
| Programs | An app's identity, filesystem and environment | **yes** | the published ABI: `getuid`, `open`/`stat`, `getenv` — an allowlist in `components/espix_proc/abi_*.c`, so a name espix does not publish stops an app loading rather than loading and answering ENOSYS |
| Programs | Signals and handlers | **yes** | delivered when the app calls in, not asynchronously |
| Programs | A root for one app — `confine` | **yes** | it cannot *name* a path outside |
| Programs | Serve a web UI or an API | **planned** | an app behind `confine`, serving out of its own view of the filesystem |
| Programs | USB keyboard and mouse | **planned** | a console you type on, on the host port |
| Programs | Arduino sketches as apps | **partial** | `apps/neopixel` is a sketch with an app-side shim; a runtime shared by every sketch, and an Arduino IDE board that deploys over `scp`, are in [ROADMAP.md](docs/ROADMAP.md#further-out) |
| Shell | Serial console and SSH, same commands | **yes** | 61 commands |
| Shell | Redirection, quoting, exit status | **yes** | `2>` and `2>&1` separate over SSH too |
| Shell | Line editing, history, TAB completion | **yes** | |
| Networking | WiFi, DHCP, NTP | **yes** | comes up as `wlan0`, reconnects on boot |
| Networking | SSH server, `scp`/`sftp` | **yes** | permission-checked like the shell |
| Networking | USB-NCM | **yes** | device role: an Ethernet adapter with no WiFi at all |
| Networking | Ethernet | **yes** | `eth0` on the S31 (RGMII, DHCP, Ethernet-first route), verified on hardware; the S3 has no wired peripheral |
| Networking | IP routing, NAT and bridging | **planned** | `route` exists; forwarding and NAT do not — a router built from an ESP32 |
| Networking | DHCP server and DNS for the LAN | **planned** | an app-side resolver already exists to build on |
| Networking | A VPN endpoint | **planned** | WireGuard-shaped, for the router case |
| Faults | Permissions enforced in espix's own VFS | **yes** | builtins, loaded apps and SFTP alike |
| Faults | Interception and reporting | **partial** | recorded for the next boot, not reaped — [Crash handling](#crash-handling-and-isolation) |
| Faults | Watchdogs | **yes** | the panic names itself in `dmesg`, without the UART |
| Display | A console on a panel | **planned** | parallel RGB or i8080 on any of the three; MIPI DSI is the P4's |
| Display | A window system | **planned** | the desktop case, once there is a panel and a pointer |
| Services | Something that starts at boot and stays up | **planned** | no init or supervision yet — [ROADMAP](docs/ROADMAP.md) |
| Services | Scheduled work: a `cron` | **planned** | the same missing supervisor, from the other end |

## Targets

| | **S3** — verified | **P4** — planned | **S31** — verified |
|---|---|---|---|
| ISA | Xtensa | RISC-V | RISC-V |
| MMU | none | address translation and RISC-V PMP | a real one — a Linux BSP exists |
| Process isolation | guardrail only | fault isolation between tasks, to confirm | **planned**, `fork()`-shaped |
| USB | one OTG: host **or** device | two, so both at once | one OTG |
| Radio | WiFi | none built in — companion chip needed | WiFi |
| Wired | — | 100M Ethernet | Gigabit Ethernet |
| Display | parallel RGB and i8080, through `LCD_CAM` | MIPI DSI, plus RGB, i8080 and PARLIO | RGB, i8080 and PARLIO; no MIPI, and weaker than the P4 |
| Runs today | **yes** | no | **yes** |

The MMU rows rest on what is written down in [Hardware Targets](#hardware-targets),
which is also where the one build option hardware decides today is explained.

*The Unix surface, row by row, with what is deliberate and what is **no** —
[docs/POSIX.md](docs/POSIX.md).*

## Getting started

**Requires ESP-IDF v6.1**, checked out by tag. Older releases will not work:
espix's SSH and password hashing are written against PSA Crypto, which arrives
with Mbed TLS 4.x in 6.1, and `espix_kernel` names `CHIP_ESP32S31`, added in the
same release. `main/idf_component.yml` declares `idf: ">=6.1"`, so an older one
is refused with a single clear line rather than failing halfway through a
compile.

Choose a target once — `tools/espix` remembers it in `.espix/`, which the
Makefile and `tools/idf.sh` both read:

```bash
make menu                 # target, board, options, reset -- remembered
tools/espix target s3     # or s31, without the menu
```

Each target keeps its own `sdkconfig.<target>` and `build-<target>/`, so
switching is remember-and-build rather than a full reconfigure. `tools/espix
config` runs `idf.py menuconfig` for the selected target; `tools/espix reset`
drops its saved options and returns it to the defaults.

Then use the Makefile, which finds the SDK and the serial port itself and needs
nothing sourced first. (`make flash` writes the app by offset; `idf.py flash`
would put it in the loader's slot.)

```bash
make flash          # bootloader, table, loader and kernel -- rootfs untouched
make flash-fs       # the rootfs image -- REPLACES what is on the device
make flash-all      # both, in the order a first boot needs
make monitor        # attach without resetting
make test           # the test suite -- see tests/README.md
make release        # tag, build and publish one target's GitHub release
make release-all    # every target, one release with combined notes
```

**Those write different things.** `make flash` writes the firmware -- the
bootloader, the partition table, the loader (`ota_0`) and the kernel (`ota_1`)
-- and leaves the filesystem alone. `make flash-fs` writes the rootfs: the apps
built out of `apps/`, in a small image the kernel grows to the whole partition
on first mount. espix creates the rest for itself on first boot — the directory
skeleton, `/etc/passwd`, `/etc/group`, `/etc/sudoers`, `/etc/hostname`, your
home directory and the SSH host key — so skipping `make flash-fs` costs you
`/bin`, not a working system.

There is no separate download or configure step. The first build fetches the
managed components at the versions pinned in `dependencies.lock` — it needs
network the first time — and generates `sdkconfig.<target>` from the
`sdkconfig.defaults*` files. No `menuconfig` required.

Boot prints kernel messages, then the greeting at the top of this README, with
`Network` reading `not connected` until you join one. `help` lists every
command; `motd` reprints the greeting.

### Updating later

```bash
make flash                              # firmware only; leaves your files alone
make flash-fs                           # WARNING: replaces the whole rootfs
```

Keeping them separate is deliberate: reflashing firmware should not destroy what
is on the device.

#### Over the network, without the cable

The kernel is a file, not a slot: `upgrade` writes it to `/boot` and the loader
installs it on the next reboot. If it does not confirm itself, the bootloader
falls back to the loader, which restores the previous one.

From the device:

    upgrade --slots          # the app slots, their role and state, and /boot
    upgrade --check          # is there a newer release? (exit 1 means yes)
    sudo upgrade             # check, ask, then install
    sudo upgrade -y          # ...without asking
    sudo upgrade --file /mnt/sda1/espix.bin   # install from a file
    sudo upgrade <url>       # install from a URL

From the development machine, with no cable at all:

    make flash-ota           # build, push over SSH, reboot, wait for it back

It finds the board from the gitignored `.espix/hosts` (below), preferring a
live cable to WiFi, so the update goes over Ethernet when one is plugged in.
`ESPIX_HOST=1.2.3.4` skips the lookup and `ESPIX_NO_REBOOT=1` stops after
queueing it.

The update source is `ota.url` in `/etc/espix.conf`, defaulting to espix's GitHub
release page. A release publishes `espix-ota.json` (the manifest,
`tools/ota-manifest.sh`, with every target's entry merged by `tools/ota-merge.py`)
and the `espix-<model>-*` images (`s3`, `s31`). See [docs/OTA.md](docs/OTA.md).

**After changing ESP-IDF versions, clean twice.** `idf.py fullclean` covers the
firmware, but each project under `apps/` is a *separate* IDF project with its
own `build/` and `sdkconfig`, and they are not reached by it:

```bash
idf.py fullclean && rm -f sdkconfig
rm -rf apps/*/build apps/*/sdkconfig
```

Without the second line the app build fails with `'.../python/vX/venv/bin/python'
is currently active while the project was configured with '.../vY/...'`, which
names the problem but not where it lives. Both `sdkconfig` files are generated
from the tracked `sdkconfig.defaults*`, so deleting them loses nothing.

### Board variants

The default targets an **N16R8** module and covers every flash size of that
PSRAM: the image is built for 16MB but flashed with the 8MB table, and the
loader writes the table for the chip it finds. Only PSRAM needs its own build,
and only the default has been run on hardware.

| File | Module | Flash | PSRAM |
|---|---|---|---|
| *(none — the default)* | N16R8, N8R8 | 8-16MB | 8MB octal |
| [boards/esp32s3-n8r2.conf](boards/esp32s3-n8r2.conf) | N8R2 | 8MB | 2MB quad |
| [boards/esp32s3-n8.conf](boards/esp32s3-n8.conf) | N8 | 8MB | none |

Board files are S3-only, and selecting one only seeds a *new* `sdkconfig`, so
use the menu rather than editing it by hand:

```bash
tools/espix board esp32s3-n8r2   # `tools/espix board` lists them
make flash
make flash-fs
```

The S31 has no board file: one PSRAM mode, its size read at runtime, and the
loader provisions the table for the flash, so one build covers every module.

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

### Ethernet

On the S31 the Gigabit MAC comes up as `eth0` beside `wlan0`, a DHCP client
like it. It is on by default; when both have an address the default route
prefers `eth0` and falls back to `wlan0` if the link drops. The reference
board's YT8531 PHY and its pins are the defaults under `espix networking` in
`menuconfig`.

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

**The default zone is UTC**, and there is no `/etc/timezone` until you set one —
`timedatectl set-timezone` writes the file, the same way `wifi connect` writes
`/etc/wifi.conf`. It holds a **POSIX TZ string**, not a zoneinfo name, because
espix ships no tzdata and the rules have to live in the string: `SAST-2`,
`EST5EDT,M3.2.0,M11.1.0`, or `UTC0` (the offset is not optional). Run
`timedatectl set-timezone` with no argument for the format, including why the
sign is inverted.

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
not a limit, because a write is streamed to the file as it arrives rather than
reassembled in memory — though a transfer *offset* past 4 GiB is refused, stdio's
seek being 32 bits, which is a property of the C library espix links against rather
than of the filesystem. See [KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).

Downloads run at about 355KB/s over 2.4GHz WiFi. Uploads are much slower, and
bounded by LittleFS erasing a block per write rather than by the network — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the measurements and for where
the per-connection buffers live.

Eight connections may be open at once (`ESPIX_SSH_MAX_SESSIONS`), so a transfer
can run while you are logged in. The serial console stays independent, and `dmesg`
is how a remote user reads kernel messages.

## Hardware Targets

The chips, and what each one changes, are in the grid at
[Targets](#targets). What follows is the hardware detail those rows
rest on.


Support priority and per-chip feature availability (isolation model,
display, networking) still to be finalized as the design matures.

A second hardware fact decides another: **`SOC_EMAC_SUPPORTED` separates the
S31 from the S3.** The S31 has a Gigabit Ethernet MAC, so `ESPIX_ETH_ENABLED`
exists only where the MAC does and the S3 build never sees it. It is on by
default on the S31 (RGMII, an external YT8531 on the reference board); the P4's
100M RMII MAC is not brought up yet.

One hardware fact decides a build option today: **`SOC_USB_OTG_PERIPH_NUM` is 1
on the S3 and the S31, and 2 only on the P4.** The OTG peripheral is either a
device (USB-NCM, `usb0`) or a host (USB storage), so espix asks which in
`ESPIX_USB_ROLE` and defaults to **host**. The P4 could do both at once, but
builds one role like the others until a board file can say which socket reaches
which controller — [USB-HOST](docs/USB-HOST.md).

What the MMU rows in the matrix above rest on, since "has an MMU" covers two
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

**The display interfaces, read from `soc_caps.h` rather than from a datasheet.**
`SOC_LCDCAM_I80_LCD_SUPPORTED` and `SOC_LCDCAM_RGB_LCD_SUPPORTED` are set
for all three parts, so a parallel panel -- i8080 or RGB -- is drivable on any
of them, through the same `LCD_CAM` peripheral the camera uses.
`SOC_MIPI_DSI_SUPPORTED` is the P4's alone, and the S31 has `SOC_PARLIO_LCD_SUPPORTED`
instead: that is the concrete form of "weaker/limited display output vs P4"
above, and it is why the grid says what it says.
`SOC_LCDCAM_CAM_SUPPORTED` is set for all three as well, so the same peripheral
takes a camera on any of them.

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
once added. Nothing is incorporated today: everything espix depends on is fetched
at build time by the IDF component manager rather than vendored into this tree.

**Five build-time patches, each written to be sent upstream** —
[tools/README.md](tools/README.md) has the detail, and
[docs/UPSTREAM.md](docs/UPSTREAM.md) the half that belongs to ESP-IDF rather than
to espix:

- `joltwallet/littlefs` gains a public custom-attribute API, because espix keeps a
  file's mode in a LittleFS user attribute and the port exposes no way to reach
  one, and a mount entry point, because espix registers the root VFS itself and
  needs the filesystem mounted without a name of its own.
- `fatfs`, which is built into IDF, gains the same kind of entry point — mount
  without registering a base path — for the same reason.
- `usb_host_msc` gains `READ CAPACITY(16)`, `READ(16)` and `WRITE(16)`, without
  which a disk past 2TiB is unreadable and its real size never reported.
- The `lwext4` core gains the metadata checksum seed, without which it refuses
  every ext4 volume e2fsprogs 1.47 or later makes.
- `esp_libc`, plus one declaration in the toolchain's own `reent.h`, hold
  `_lseek_r` at the 32-bit ABI the prebuilt C library calls it with, so that
  `off_t` can be 64 bits.

Everything espix depends on is fetched by the IDF component manager at the
versions in `dependencies.lock` — `espressif/elf_loader`,
`espressif/esp_linenoise`, `joltwallet/littlefs`, `espressif/esp_tinyusb`,
`espressif/usb`, `espressif/usb_host_msc`, `espressif/esp_ext_part_tables`, and
`esp_lwext4` as a git dependency that carries the lwext4 core as its own submodule.
The patches are applied to the downloaded copies on every configure, so a
reinstall or an upgrade restores them, and an anchor that has moved stops the build
by name rather than producing an image that is quietly missing something.

## Acknowledgements

- [Apache NuttX](https://nuttx.apache.org/) — prior art for nearly all of this,
  and the honest first stop for anyone who does not need to stay on ESP-IDF
- [GrieferPig/esp32-s31-linux](https://github.com/GrieferPig/esp32-s31-linux)
- [nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)
- [paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)
- [faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)
- Espressif ESP-IDF / FreeRTOS
