<p align="center">
  <img width="486" height="239" alt="espix login greeting" src="https://github.com/user-attachments/assets/457b6049-acb2-4080-8ad2-40222d47d782" />
</p>

<table align="center">
  <tr>
    <td width="440">
      <video src="https://github.com/user-attachments/assets/50dd1712-ef03-4bdc-8989-9bc64e26e0ef"></video>
    </td>
    <td width="440">
      <video src="https://github.com/user-attachments/assets/0d29384d-b44a-4fc5-8c46-e333b48fac86"></video>
    </td>
  </tr>
</table>

# espix

A Unix/Linux(-like) kernel/runtime environment for ESP32, on top of the ESP-IDF.

Replace Raspberry Pi for smaller use cases, that can fit in memory.
With a familiar, linux-like environment. A real shell, filesystem, networking, and the
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

## Features
- USB hosting: Mass storage devices, hubs, keyboard, mouse.
- Networking:
  - IP routing, NAT, bridging. Firewall planned.
  - With wifi, eth, USB-NCM, and virtual interfaces.
  - File sharing with NFS v3 (NFSv4, SMB planned)
  - Wireguard server (PiVPN shaped), and client.
- File systems:
  - VFAT, exFAT, ext4 (via lwext4) for USB disks, LittleFS for `/` on internal flash.
  - MBR and GPT partition tables.
  - \>4GB files, and \>2TB partitions. (exFAT & ext4)
- Simple graphical desktop:
  - Showcase, mostly. very lacking.
  - Simple windows, dragging, min/max/close btns. Taskbar, start btn/menu, and systray.
  - Accelerated with PPA/2D-DMA/JPEG hardware, where present (only P4/S31)
- Remote desktop with VNC(RFB):
  - Access desktop or console, remotely.
  - Showcase desktop, and gfx things, without an actual screen.
- Audio:
  - Play mp3 or wav files.
  - Stream to BT speakers or headphones, or the onboard ES8311 speaker (S31).
  - Record from the onboard microphone; pipe a source to a sink.
  - Mix several apps into one output, with per-app volume.
  - Planned:
    - Per-stream sink selection.
    - More sinks: BLE Audio, dac/amps on the Korvo board, i2s in/out through gpio, etc.
    - More sources: HTTP/Icecast, UPnP/DLNA.

Use cases:
- Router, NAS, Audio streaming/server, "nano" server/PC.

## Targets

| | **S3** — verified | **P4** — planned | **S31** — verified |
|---|---|---|---|
| ISA | Xtensa | RISC-V | RISC-V |
| MMU | - | simple: address translation and RISC-V PMP | compliant **Sv32 MMU** |
| Process isolation | guardrail only | fault isolation between tasks, to confirm | **planned**, `fork()`-shaped |
| USB | 1x USB1.1 | 1x USB1.1 / 1x **USB2.0** | 1x **USB2.0** |
| Radio | WiFi 4 / BLE 5.0 | - | WiFi **6** / BT **Classic** / BT 5.4 LE / IEEE 802.15.4 |
| Ethernet | — | 100M | **1000M** |
| Display | parallel RGB and i8080 (`LCD_CAM`) | **MIPI DSI (1080p/30)**, RGB, i8080 and PARLIO | RGB, i8080 and PARLIO; **no** MIPI (**weaker than P4**) |
| Runs today | **yes** | no | **yes** |

The MMU rows rest on what is written down in [Hardware Targets](#hardware-targets),
which is also where the one build option hardware decides today is explained.

*The Unix surface, row by row, with what is deliberate and what is **no** —
[docs/POSIX.md](docs/POSIX.md).*

## Long list

<sub>Run on ESP32-S3N16R8 and ESP32-S31 (WROOM-3) with ESP-IDF v6.1; the ESP32-P4 is planned</sub>

| Category | | | |
|---|---|---|---|
| Storage | LittleFS mounted as the real `/` | **yes** | survives reboot and a firmware reflash |
| Storage | USB host: enumerate, identify, read the partition table (MBR and GPT) | **yes** | `lsusb`, `lsblk`, `blkid`; four device slots, for a hub |
| Storage | Mount a FAT volume into the namespace | **yes** | through espix's own VFS, so the permission check applies |
| Storage | `/etc/fstab`, applied on attach | **yes** | device column takes a name, a wildcard, or `LABEL=`/`UUID=`/`PARTUUID=`; the options column takes `noauto`, `ro`, `uid=` and `gid=`, and an entry naming no owner mounts as `esp` |
| Storage | `mount -o uid=,gid=` | **yes** | root hands a volume to a user without giving them root |
| Storage | `mount -o ro` | **yes** | refused by FatFs at the block device, not by policy; `/etc/fstab` takes `ro` too |
| Storage | exFAT volumes | **yes** | on by default (`ESPIX_FS_EXFAT`, +7.0KB ROM, no static RAM); `FF_LBA64` and the 64-bit diskio fix come with it, so a volume past 2TiB is readable too |
| Storage | ext2/3/4 volumes | **yes** | read-only by default, writable with `mount -o rw`; a driver of espix's own, two mount slots. Writes go through the port's experimental extent implementation, and a volume without a journal cannot be mounted writable at all — see [KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) |
| Storage | `mkfs`: make a filesystem | **planned** | nothing is ever formatted today |
| Storage | Serve the stick over the network | **yes** | NFSv3: the portmapper, mountd and nfsd, statd so a client needs no lock option, and a write path (writable unless the export says `ro`); a Linux or macOS client mounts it with no options, a 500-entry listing is one directory walk, and 4 MiB copies at ~410 KB/s — [NFS.md](docs/NFS.md) |
| Programs | Run a native app: load, argv, exit status | **yes** | cross-compiled on a PC, copied over, run by name |
| Programs | An app's identity, filesystem and environment | **yes** | the published ABI: `getuid`, `open`/`stat`, `getenv` — an allowlist in `components/espix_proc/abi_*.c`, so a name espix does not publish stops an app loading rather than loading and answering ENOSYS |
| Programs | A root for one app — `confine` | **yes** | it cannot *name* a path outside |
| Programs | Serve a web UI or an API | **planned** | an app behind `confine`, serving out of its own view of the filesystem |
| Programs | USB keyboard and mouse | **yes** | boot-protocol HID on the host port, decoded to X11 keysyms and fed to the same input path a viewer's events use; a keyboard and a wireless receiver's mouse both verified — [USB-HOST.md](docs/USB-HOST.md) |
| Programs | A game as an app | **partial** | `apps/doom` runs the shareware's levels, launched from a desktop icon, keyboard only, with its WAD fetched on the first launch instead of shipped; its memory is a region of its own now and comes back on exit or kill. The frame rate is the RFB encoder's (hextile on game imagery is ~50x its desktop cost), not the engine's, and the showcase target has moved to Quake 2 — which builds, loads and runs to the map, then meets the PSRAM wall — [ROADMAP.md](docs/ROADMAP.md#a-game-as-the-showcase-app) |
| Programs | Fetching a file over HTTP(S) | **yes** | `fetch <url> <path>`, sharing `upgrade`'s HTTP and TLS stack and certificate bundle; checks room first and says where to get some back. An app declares its data in `/etc/apps/<name>.conf` and the launcher — desktop icon or command line — fetches anything missing before it starts, so no program holds a network handle of its own |
| Programs | Arduino sketches as apps | **partial** | `apps/neopixel` is a sketch with an app-side shim; a runtime shared by every sketch, and an Arduino IDE board that deploys over `scp`, are in [ROADMAP.md](docs/ROADMAP.md#further-out) |
| Shell | Serial console and SSH, same commands | **yes** | 89 commands |
| System | Services and timers that outlive a login | **yes** | `/etc/units` is written on the first boot from a self-documenting template at 0600: `always`, `once` or `every <interval>` units (a program or a shell builtin), each a process, with `success=` exit statuses and `requires=` ordering, a supervisor task, `service` to manage them, and safe mode after a fault; `sshd` (which safe mode still starts) and the update check `autoupdate` are on by default |
| System | `reboot`, `poweroff` | **yes** | one sequence for both: the units are asked to stop, every process is signalled and waited for, every volume is flushed and unmounted, and then the reset. `poweroff` is deep sleep with a 10-minute timer, because the chip cannot cut its own power; both are root only — [POSIX.md](docs/POSIX.md#processes) |
| Networking | WiFi, DHCP, NTP | **yes** | comes up as `wlan0`, reconnects on boot |
| Networking | WiFi access point | **yes** | `wifi ap start` brings up `wlan1` beside the station and NATs clients out the uplink; one radio, one AP, shared channel |
| Networking | SSH server, `scp`/`sftp` | **yes** | permission-checked like the shell |
| Networking | SSH port forwarding, `ssh -L` | **yes** | `direct-tcpip`, so `ssh -N -L 5900:127.0.0.1:5900 esp@<board>` reaches a service on the board over an encrypted, authenticated connection; destinations default to the device itself, and `ssh -R` is not implemented |
| Networking | mDNS/DNS-SD: <hostname>.local | **yes** | advertises the hostname and _ssh/_sftp-ssh, so ssh esp@esp32s31-d0762a.local works without knowing the address; on by default like a Raspberry Pi image, and enabled=no in /etc/mdns.conf turns the announcement off without a rebuild (CONFIG_ESPIX_MDNS=n leaves it out of the image) |
| Networking | WireGuard VPN server | **yes** | the lwip port as a server for the house: `vpn setup` then `up`/`down`/`status`, peers added by name with a QR code for a phone, clients reach the LAN and the internet through `wg0`, `vpn dns router` hands them the home resolver, and a `vpn` unit brings it all back at boot |
| Networking | USB-NCM | **yes** | device role: an Ethernet adapter with no WiFi at all |
| Networking | Ethernet | **yes** | `eth0` on the S31 (RGMII, DHCP, Ethernet-first route), verified on hardware; the S3 has no wired peripheral |
| Networking | IPv4 routing and NAT | **yes** | `nat on <dev>` masquerades an inside interface out the default route; `ESPIX_NET_ROUTER` is on by default |
| Networking | L2 bridging | **yes** | `bridge add <port>` then reboot: `br0` owns the address, ports have none; AP↔Ethernet, never a station (802.11 three-address frames) |
| Networking | DHCP server for the LAN | **partial** | the AP and `usb0` both serve; DNS is the uplink's resolver passed through, not a local one |
| Networking | Isolation and firewall | **planned** | NAT is not a firewall; guest zones and port-forwards are later |
| Faults | Interception and reporting | **partial** | recorded for the next boot, not reaped — [Crash handling](#crash-handling-and-isolation) |
| Faults | Watchdogs | **yes** | the panic names itself in `dmesg`, without the UART |
| Audio | Play an MP3 or WAV to a Bluetooth speaker | **yes** | S31 only: `bluetoothctl` pairs/connects the sink, then `play <file|url>` streams to it — a real 64 kbps stereo MP3 plays clean at ~16% of one core, ring full, no underruns; [AUDIO.md](docs/AUDIO.md) |
| Audio | Choose the SBC quality | **yes** | `bluetoothctl quality 0|1|2` (mono ≤35, joint stereo ≤35, joint stereo ≤52); q2 is the default and applies to the next connect |
| Audio | An I2S codec or the internal DAC | **planned** | the S31 coreboard's amp and mic are phase 2 |
| Audio | Resampling, volume, mixing | **planned** | so non-44.1 kHz sources and two streams at once wait — [ROADMAP](docs/ROADMAP.md#audio) |
| Display | A desktop, served over VNC | **yes** | `vnc start` puts an 800x600 RGB565 framebuffer on port 5900 and speaks RFB (RFC 6143), so any VNC client is the monitor — no panel needed; Hextile keeps a flat first frame near 10 KiB rather than 1.9 MiB; [DISPLAY.md](docs/DISPLAY.md) |
| Display | The clients people actually use | **yes** | RFB 3.3 and 3.7/3.8, plus VNC authentication for the ones that insist on a password: TigerVNC, RealVNC on Android and macOS Screen Sharing have each painted a full desktop |
| Display | Authentication | **partial** | VNC auth (DES) with a built-in default password, public on purpose; security type None is still offered to 3.7+ clients, so it is a compatibility mechanism rather than a security boundary — [DISPLAY.md](docs/DISPLAY.md) |
| Display | A cursor, and a keyboard round-trip | **yes** | a server-side cursor plus a window that echoes keys; input is one entry point, so a viewer and a local keyboard or mouse arrive the same way |
| Display | PPA and JPEG acceleration | **yes** | the reason the virtual screen exists: a fill/blit/scale path and a JPEG codec, each measured against the CPU path it replaces and kept, because the S3 has neither — [DISPLAY.md](docs/DISPLAY.md#what-it-costs) |
| Display | Dragging a window | **yes** | CopyRect, so a four-pixel motion costs 8 KB rather than 301 KB — 37x, and the difference between a drag and a slideshow |
| Display | 2D-DMA for moves and colour conversion | **no** | PPA SRM already does what the desktop needs, so it is unwritten until something needs it |
| Display | Testing a physical display | **planned** | parallel RGB or i8080 on any of the three; MIPI DSI is the P4's |
| Display | A window system | **yes** | surfaces, z-order, focus, title-bar drag, minimise, maximise and close, and dirty-region repaint, with a taskbar: a launcher, a button per window and a live clock |
| Display | Fitting a picture to a window | **yes** | `espix_surface_scale` — PPA SRM with the scale factors set, and a nearest-neighbour loop where there is no PPA at all, which is the S3 |
| Display | Playing a video | **partial** | raw MJPEG only — the one shape every target can decode (the JPEG codec on the S31 and the P4, TJpgDec on the S3); a 640x480 frame decodes in 5-6 ms and the player holds 24 fps, while the RFB encoder is what caps it near 8 updates a second — [DISPLAY.md](docs/DISPLAY.md#known-limitations) |
| Display | A terminal you can work in | **yes** | the desktop's terminal window runs a real shell session — the same `espix_term` as the on-screen console, so the motd, the prompt, history, arrows, Ctrl-C and every command work there; a close button sits in the title bar |
| Services | Scheduled work: a `cron` | **partial** | units with `every <interval>` — `autoupdate` is one, every 6h; crontab syntax, per-user jobs and `at` are not there |

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
    upgrade --check          # is there a newer release? (exit 100 means yes)
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

### S3 Board variants

The default targets an **N16R8** module and covers every flash size of that
PSRAM: the image is built for 16MB but flashed with the 8MB table, and the
loader writes the table for the chip it finds. Only PSRAM needs its own build,
and only the default has been run on hardware. Tested only on N16R8, others probably don't work yet.

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

### Logging in over SSH

`sshd` listens on port 22 from boot, as the first unit in `/etc/units`. Once `ip addr` shows an address:

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
[docs/PROFILING.md](docs/PROFILING.md#transfer-throughput-scp-and-the-filesystem)
for the measurements.

Eight connections may be open at once (`ESPIX_SSH_MAX_SESSIONS`), so a transfer
can run while you are logged in. The serial console stays independent, and `dmesg`
is how a remote user reads kernel messages.

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

## Why not Linux on ESP32?
 - Only supports S31. Espix should be able to support all ESP32 models.
 - Cannot do much with it yet.

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
