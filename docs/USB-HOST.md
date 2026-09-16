# The USB host port

espix drives the devices plugged into its USB-OTG socket. Today that means
storage: a stick or a card reader is found, identified, and its partition table
read, and `lsblk` and `blkid` report what is there.

```sh
$ lsblk
NAME   SIZE    TYPE FSTYPE LABEL
sda    29G     disk        SanDisk Ultra
└─sda1 29G     part vfat   MYSTICK
```

**What this is not** is a filesystem. Nothing is mounted, and no path under a USB
stick is reachable: `lsblk` names the filesystem and stops there. That is the
first stage on purpose — see [Stage 2](#stage-2--mounting-and-why-it-is-not-done),
where the quick version of mounting turns out to be worse than not having it.

The same socket is the one [USB-NETWORKING.md](USB-NETWORKING.md) uses to present
`usb0`, and the two cannot both have it: which of them this build is, and what
that costs, is the next section.

## One port, one role

The S3 has **one** general-purpose USB controller: `SOC_USB_OTG_PERIPH_NUM` is 1.
Host mode and USB-NCM are two uses of that one peripheral, so a build is one or
the other, and the choice is made in Kconfig rather than at runtime.

**Host is the default**, and this is a behaviour change worth knowing before you
flash it:

- **`usb0` disappears.** A board that was reachable over the cable — no WiFi, no
  access point, `ssh esp@192.168.7.1` — is not any more. The way back is the
  device role: `make menuconfig` under *espix networking*, or edit the generated
  `sdkconfig` and rebuild (see [the knob](#the-knob)).
- **On a devkit the hub blocks the UART socket.** The two sockets on an
  ESP32-S3-DevKitC-1 are close enough that a hub in the OTG socket makes the UART
  one unusable. SSH over WiFi is then the only console, and **the recovery is
  unplugging the hub**, not a reflash. This matters because the symptom is "I
  cannot reach the board", which looks like a fault and is not one.

So: confirm SSH works *before* plugging the hub in. That is the lifeline, and
with the hub attached there is no other way in.

The **P4** is the only espix target with two OTG peripherals
(`SOC_USB_OTG_PERIPH_NUM` is 2), so it is the only one that could be a host on one
port and a USB-NCM device on the other. espix does not build both for it yet:
which socket reaches which controller is a property of the board's wiring, and no
espix board file describes that today.

## What it does

`lsblk` is the command to run after plugging something in, and the example above
is what it prints. The columns:

| column | what it is |
|---|---|
| `NAME` | `sda`, `sdb`… in slot order, with `sda1`… for the partitions in the MBR |
| `SIZE` | the size the device reports |
| `TYPE` | `disk` or `part` |
| `FSTYPE` | what is on it — a partition's filesystem, or on a disk line the filesystem the disk *itself* carries (a superfloppy); empty when there is neither, and `unreadable` when sector 0 could not be read |
| `LABEL` | the volume label, or for a disk with no filesystem of its own the product string it reports |

Sizes go through the same formatter `ls -lh` uses, so they are coreutils' idea
of a size rather than util-linux's: a 28.7GB stick reads `29G` here where
`lsblk` on a computer would print `28.7G`. One formatter for the whole system is
worth more than matching another tool's digits.

The columns are measured per listing, so a stick with one FAT partition is not
laid out for a disk full of Linux partitions — and a listing that needs the room
gets it:

```
NAME   SIZE    TYPE FSTYPE                   LABEL
sdb    466G    disk                          Elements 25A3
├─sdb1 200G    part exfat/ntfs (unsupported) MYBOOK
└─sdb2 266G    part linux (unsupported)
```

`blkid` is the same information one `KEY="value"` line at a time, for a script
rather than a person:

```
$ blkid
sda: TYPE="disk" PRODUCT="SanDisk Ultra" SERIAL="4C530001120509116250" VID="0781" PID="5583" SIZE="30752030720"
sda1: TYPE="vfat" LABEL="MYSTICK" START="1048576" SIZE="30719950848"
```

Two deliberate differences from Linux's `blkid`: `TYPE` on a whole-disk line
reads `disk`, because there is no filesystem to name there; and keys with no
known value are left out rather than printed empty. Both commands take names as
operands — `lsblk` a disk (`sda`), `blkid` a disk or a partition (`sda1`, which
prints that line alone, because the value is what was asked for) — and an operand
that names nothing is refused rather than quietly printing less. **Neither prints
anything at all when nothing is attached** — an empty table is an answer, and an
empty header is noise.

### What it reports, and what it cannot

`lsblk`'s own help line carries the headline limitation — `help lsblk` says
"(MBR only, no GPT)" — because that is where someone will read it. The detail:

- **MBR only, no GPT.** Sector 0 is read and its partition table parsed. A GPT
disk shows as its *protective MBR* entry — a partition of type `gpt`, marked
unsupported — which is the honest answer from sector 0 alone.
- **A disk with no partition table is read as a superfloppy.** One volume covering
the whole device, no table at all — how most sticks used to ship and how some
still arrive (the case that produced this code: a SanDisk Cruzer Blade prepared
by an appliance, which read as a bare unpartitioned disk until sector 0 was
examined). Sector 0 is then the filesystem's own boot sector: a FAT one gives
`vfat` on the disk row plus its volume label, and exFAT or NTFS boot sectors are
named and marked unsupported. Anything else stays a bare disk.
- **Filesystems espix has no driver for are named, not hidden.** The partition
table says `exfat/ntfs` (one MBR code covers both, so it is as specific as sector
0 gets), `linux` (`0x83` is "Linux any" — this is not "ext4", because sector 0
does not say), or `gpt`, each marked `(unsupported)`. That is the whole reason
the command exists before mounting does: silence would read as an empty disk.

  `vfat`, `littlefs` and `raw` are *not* marked, and the marker is about the type
  rather than this build: FatFs is in the ESP-IDF image either way, and what is
  missing is the mount plumbing, not the driver.
- **Labels come from the FAT boot sector.** An MBR has nowhere to put a volume
  label, so the 11 space-padded bytes at `0x2B` (FAT12/16) or `0x47` (FAT32) are
  read from the partition's first sector, using the filesystem-type string the
  formatter wrote to decide which. A volume still called `NO NAME` reports none.
  Bytes are copied as they stand — a label in a non-ASCII code page is not
  transcoded, because nothing here knows which page that was.
- **Four device slots.** `sda`…`sdd`; a fifth device is refused with a log line
  rather than silently displacing one. A name stays with a device for as long as
  it is plugged in.
- **Sizes above 2 TiB are reported *wrong*, not refused.** The MSC layer reads
capacity with SCSI `READ CAPACITY(10)`, whose block count is 32 bits, so a 4 TB
drive reports exactly 2 TiB: measured on a Samsung PSSD T9, `2199023255040` bytes
`= 2³² × 512`. Nothing warns; the number simply looks plausible. `READ
CAPACITY(16)` would fix it and the class driver does not use it.
- **One storage device at a time.** Not a policy — the number of host channels the
S3's USB core has, and what a hub plus two drives would need. See below.

### Hubs, and more than one device

A hub works, and that is one line of configuration rather than a feature:
`CONFIG_USB_HOST_HUBS_SUPPORTED` **defaults to `n`** in the host stack, and
without it the devices behind a hub never enumerate and nothing says why. It is
set in `sdkconfig.defaults`; multi-level hubs (a hub behind a hub) already
default to on.

**But one storage device at a time is all this part can do, and the reason is
channels.** The S3's USB core has a fixed pool of them, read from the hardware's
own configuration register at install (`usb_dwc_ll_ghwcfg_get_channel_num()`,
i.e. `GHWCFG2.NumHstChnl + 1`), and every pipe takes one:

| consumer | channels |
|---|---|
| the root port's control pipe | 1 |
| a hub, once open: its control pipe, plus one for its interrupt endpoint | 2 |
| a bulk-only storage device: control pipe, bulk IN, bulk OUT | 3 |
| a *second* storage device | 3 more, and there are none left |

When that pool runs out the allocation does not fail politely: the class driver
returns `ESP_ERR_NOT_SUPPORTED` and the HCD says why in its own words —

```
E HCD DWC: No more HCD channels available
W usb: device at address 4: no free host channels (hcd_pipe_alloc); a second storage device does not fit
```

— while the device itself is perfectly enumerated: `lsusb` lists it, its
interface says `08/06/50`, and it is the *first* device that wins. Pull the first
one out and the second attaches on the next sweep, which is exactly what happened
here: a Cruzer Blade held the channels, the PSSD T9 was refused, and 27 seconds
after the Blade was unplugged the SSD was `sda`. The practical answer for a hub
is therefore **one storage device plus whatever else you have that is not
storage**, and a keyboard or a serial adapter costs nothing like as much.

Hotplug works in both directions, including a stick pulled out mid-read: the
driver reports a sudden removal the same way as an orderly one, the name is
released, and the next device takes the next free slot:

```
D USB_MSC: Device suddenly disconnected
I usb: sda: removed
I usb: addr 2 removed (0781:5567)
I usb: addr 4 looks like storage; claiming it
I usb: sda: addr 4 PSSD T9 (04e8:61fd) 2199023255040 bytes, 1 partition
```

## The stack

ESP-IDF **6.1 does not ship a USB host component**. It was in the tree through
5.x and moved out at 6.0, so `components/` has `esp_hal_usb`,
`esp_driver_usb_serial_jtag` and `esp_usb_cdc_rom_console` — the PHY, the HAL and
the ROM console, none of which is the host stack. A `PRIV_REQUIRES usb` alone
therefore does not work; the host stack is a managed component like any other.

`main/idf_component.yml` carries three entries for it:

| component | what it is |
|---|---|
| `espressif/usb` 1.5.0 | the host library: enumeration, transfers, and the hub driver |
| `espressif/usb_host_msc` 1.3.0 | the class driver for mass storage: bulk-only transport and SCSI |
| `espressif/esp_ext_part_tables` 0.5.0 | reads the partition table out of sector 0 |

The MSC driver hands out an `esp_blockdev` handle per device (IDF 6.0.4 and
later), and espix keeps it: that is the handle Stage 2 mounts from, and holding it
does not tie up the driver's own.

**Both stacks are always downloaded; only one is linked.** The component manager
resolves manifests before Kconfig exists, so it cannot see
`CONFIG_ESPIX_USB_ROLE`. What the role choice decides is what gets *built*:
`components/espix_usb/CMakeLists.txt` adds `host.c` only in a host build, and
`components/espix_net/CMakeLists.txt` adds `usb_ncm.c` only in a device build.
The requirement is unconditional in both, which is not sloppiness: IDF expands a
component's `CMakeLists.txt` twice and `CONFIG_*` does not exist on the first
pass, so a conditional `REQUIRES` drops the dependency and the build fails on a
missing header.

### Three tasks, and the one that must not block

The host library needs one task calling `usb_host_lib_handle_events()` forever, or
it stops enumerating; the MSC driver starts a second and invokes the event
callback from it; and `usb:work` is espix's own, which does the device work. All
three are ordinary tasks (`usb:host` priority 4, `USB MSC` 5, `usb:work` 3, 4096
bytes each), and the split is not tidiness — **it is the difference between a
device attaching in one second and taking ten minutes.**

A client's transfer completions are delivered only from inside
`usb_host_client_handle_events()`: that is where its endpoint list is serviced and
where a transfer's callback runs (`usb_host.c:1136`). The MSC driver calls our
callback from inside that function, so an install started there waits for
completions that only the loop it is blocking could deliver. Each transfer burns
its full 5 s timeout (`msc_host.c:703`) and the ready-state retry does that up to
fifty times — a device took **minutes to fail, silently**, with every appearance
of a driver that had simply ignored it. Espressif's own example does the same
thing, which is why the trap is worth stating rather than assuming.

So the callback now only notes the address on a queue and returns; `usb:work`
does the install, describe and partition read. Measured on the same SSD: **1.0 s**
from the arrival event to `lsblk` listing it, against the ~10 minutes it spent
grinding timeouts before.

`usb:work` also sweeps the device pool every five seconds when nothing has been
queued, and claims anything storage-shaped that has no driver yet. That is
insurance against an event that never arrives — the device is claimable whether
or not anyone announced it — and it is what recovered the SSD after the first
device was unplugged. Three commands expose the same machinery by hand:

| command | what it does |
|---|---|
| `lsusb [-v]` | what the port can see, hub included, and the pool/client counts |
| `usbscan` | claim every storage device the pool is offering, now |
| `usbprobe <addr>` | claim one, and print the step that refused if it does |

### Buffers are internal RAM, and on the S3 cannot be anything else

The buffer a read lands in goes on the USB wire unchanged, so it is allocated
with `MALLOC_CAP_DMA`. On the S3 that means internal RAM: the USB-DWC DMA engine
cannot reach external RAM, which is why the host stack's
`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` is gated on `IDF_TARGET_ESP32P4 &&
SPIRAM` and cannot be set for this target at all. It would be worth setting on a
P4, where it moves those buffers off the scarce heap; here it is not an option
that exists.

## Checking it

1. **SSH first.** Before the hub goes in, from the console. The host role was
   flashed and exercised this way: `uname -a`, then `lsblk`, over WiFi, with the
   serial port untouched.
2. **Nothing attached:** `lsblk` prints nothing and exits 0. That is success, and
   `dmesg` should show `usb: host mode, 4 device slots` from boot.
3. **A stick in:** `dmesg` shows the device line within a second, and `lsblk` the
   disk. A partitioned stick gives `sda1` with its filesystem; a stick with no
   partition table gives the filesystem on the disk row instead (superfloppy).
4. **Pull it out:** `dmesg` says `sda: removed`, `lsblk` no longer lists it, and
   the *name* is immediately reusable — a second device took `sda` again.
   `free` before and after is the check that the slot and its buffers went back.
5. **A hub:** the hub and anything non-storage is enumerated and listed by
   `lsusb`; one storage device attaches. A second storage device is refused for
   want of host channels (see above) — that is the part's limit, not a fault.
6. **The suite agrees:** `make test SUITE=usb` runs `75-usb.sh`, which asserts the
   role, the absence of `usb0`, and that both commands answer — and skips the
   enumeration half when nothing is plugged in. **`make test SUITE=net`** covers
   the other side: it now asserts that a build without USB-NCM really has no
   `usb0` rather than merely skipping the link checks.
7. **In a device-role build**, `lsblk` and `blkid` still exist and explain
   themselves: `lsblk: usb host was not built into this image
   (CONFIG_ESPIX_USB_ROLE_HOST)`. A command that vanished from a build would
   leave you comparing the device against this page and guessing.

### Measured

Image sizes, both roles built back to back on the same tree, ESP-IDF v6.1,
esp32s3, with the shipped defaults (`ESPIX_USB_VERBOSE=n`):

| build | `espix.bin` | free in a 4MB app partition |
|---|---|---|
| `ESPIX_USB_ROLE_HOST` (default) | 0x13ee10 — 1,307,152 B | 69% |
| `ESPIX_USB_ROLE_DEVICE` | 0x1339f0 — 1,260,016 B | 70% |

So the host stack costs about **46KB of image** more than the TinyUSB NCM stack it
replaces — and that number includes `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`, which
compiles in DEBUG format strings for the whole system rather than just the USB
stack. It is the price of the verbose switch being *usable*; without it those
messages do not exist in the image at all, and "no output" means nothing. `nm` on
the two ELFs confirms exactly one stack is linked each way: the host build has
`usb_host_install`, `msc_host_install` and `esp_mbr_parse` and no
`tinyusb_driver_install`; the device build is the mirror image.

RAM, from `free` and `ps` on the N16R8 board. The tasks are the exact part:

| task | priority | stack configured | high-water seen |
|---|---|---|---|
| `usb:host` — the library's event loop | 4 | 4096 | 3352 B |
| `USB MSC` — the class driver's own | 5 | 4096 | ~2100 B |
| `usb:work` — installs, reads, sweeps | 3 | 4096 | 3376 B |

The internal heap is 327K on this board, and its low-water mark settled at
**82–89K** across the bring-up builds and device states tried. That is a bring-up
figure rather than a specification, and it is dominated by diagnostics: the
256-line kernel ring used while chasing the faults above costs ~22K on its own,
and `ESPIX_USB_VERBOSE=y` adds more. Re-take it with `free` on the build you care
about, with the device attached and detached, before quoting it.

## Stage 2 — mounting, and why it is not done

The quick version is `esp_vfs_fat_bdl_mount(handle, "/mnt", ...)`, and it is
wrong twice over rather than merely inelegant:

- **It skips the permission check.** espix registers its VFS at IDF's *fallback*
  prefix `""`. Anything registered at `/mnt` is therefore routed by IDF *before*
espix sees the path: no `resolve()`, no `espix_fs_root_permits()`, no cwd. A
`confine`d process could read the whole stick. This is not a new discovery —
`components/espix_fs/dev.c` says a second prefix is deliberately forbidden, and
[KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem) already records it as a gap.
- **`chmod` on a FAT file would write to the wrong partition.**
  `components/espix_fs/mode.c` hardcodes `ESPIX_FS_ROOT_PARTITION` in
  `attr_store()`: a FAT file whose mode is not the rule's default would store
  littlefs attributes for a path that is not on littlefs.

The design that avoids both is already written down in
[ROADMAP.md](ROADMAP.md#filesystem): espix keeps a path-to-lower-ops table and
routes internally, so every call still passes its own check. Three pieces are
already shaped for it:

- `lower_t` in `components/espix_fs/vfs.c` is a struct *passed as VFS context*
  specifically because a mount was expected; it becomes an array with a
  longest-match prefix lookup.
- `tools/patch-littlefs.py` is the precedent for a mount-without-registering
  split, and FatFs needs the same treatment because
  `esp_vfs_fat_*_mount()` registers.
- The `/dev` overlay is a working example of espix owning a subtree by string
  match inside its own VFS.
- The mode *rule* (`mode.c`) is already the policy a filesystem with no stored
  metadata needs — what Linux expresses as `-o uid=,gid=,fmask=` — which is
  exactly what FAT wants.

Known wrinkles, so they are not rediscovered: fds 240–255 are reserved for
devices and IDF's `local_fd_t` is a `uint8_t`, so a mount index packed into the
fd has to coexist with that; `/mnt` is not in the boot skeleton, so it has to be
added the way `/dev` was; and **unmount does not exist as a concept** anywhere in
espix, which removable media needs more than anything else does.

Everything Stage 1 does is a prerequisite for that and none of it is wasted: the
device table, the block-device handle and the partition list are what a mount
would be given.

## Roadmap, not now

- **Mounting, which is Stage 2 above** — still the largest item, and still not
  cheap.
- **`READ CAPACITY(16)`,** so a drive larger than 2 TiB reports its real size
  instead of a plausible 2 TiB. It is the class driver's choice of SCSI command,
  not espix's, so it belongs to the list below as much as to here.
- **A GPT reader**, once a disk turns up that needs one. The protective MBR is
  reported today rather than followed.
- **A USB keyboard (HID).** Deferred, and cheap when it happens: the test needs
  no display and no serial port — SSH in over WiFi, run a command that prints
  decoded keystrokes, and type on the USB keyboard. That sidesteps the
  can't-plug-both-sockets problem entirely, and at two channels it is also
  comfortably inside the budget rather than competing with a disk.
- **exFAT.** `FF_FS_EXFAT` is hardcoded `0` in IDF's `components/fatfs/src/ffconf.h`
  with no Kconfig to change it, so enabling it means patching a dependency — the
  `tools/patch-littlefs.py` precedent. Separately: exFAT is covered by Microsoft
  patents and FatFs's author has historically noted that a licence may be needed
  for commercial use. **That claim was not verified against IDF or FatFs here — no
  patent or licence text ships with the bundled FatFs — so check Microsoft's own
  terms before shipping it on by default.** `lsblk` and `blkid` name exFAT and
  NTFS today, whole-device or partition, which is the honest half.
- **lwext4** for ext2/3/4, and a much later `lwntfs`. Until then, `lsblk` naming
  them as recognised-but-unsupported is the honest position, and it is what this
  stage delivers.
- **Whether VBUS needs board-side control.** Answered for one hub on one board —
  a PD hub that powers the board *and* enumerates devices works, with the board
  taking its power through the same socket the host controller uses. If a board
  ever needs power switched to the port, `boards/*.conf` is the only place
  per-board wiring can be expressed today and there is **no precedent in it**:
  those files carry flash size, PSRAM mode and a partition table, nothing else.

## The knob

| config | what it does |
|---|---|
| `ESPIX_USB_ROLE_HOST` | the OTG port is a host. **Default.** |
| `ESPIX_USB_ROLE_DEVICE` | the OTG port is USB-NCM; `usb0` exists and `lsblk` explains that the host was not built |
| `USB_HOST_HUBS_SUPPORTED` | set to `y` in `sdkconfig.defaults`; defaults to `n` upstream and a hub does nothing without it |
| `LOG_MAXIMUM_LEVEL_DEBUG` | set to `DEBUG` in `sdkconfig.defaults`, because the USB stack logs almost everything at DEBUG: without it those messages **do not exist in the image**, so "no output" means nothing. The runtime level stays `INFO`, so nothing gets noisier unless a tag is raised — it costs image size, not noise |
| `ESPIX_USB_VERBOSE` | raises exactly the USB tags (`USBH`, `ENUM`, `EXT_HUB`, `EXT_PORT`, `HUB`, `HCD DWC`, `USB HOST`, `USB_MSC`) to DEBUG at boot. **The switch to reach for when a device does not appear** — with it off, a working enumeration and an empty socket look identical. Costs log volume: the ring holds `ESPIX_KLOG_LINES` (96) lines and this fills it |
| `ESPIX_USB_NCM_ENABLED` | unchanged, now `depends on ESPIX_USB_ROLE_DEVICE` |

One trap, because it has cost espix a build before: **an existing `sdkconfig`
wins over `sdkconfig.defaults` and over Kconfig defaults**, silently. Changing
the default, or adding a line to `sdkconfig.defaults`, can be correct in every
tracked file and absent from the build. `sdkconfig` is gitignored here, so the
cure is to delete it (or edit it) and rebuild —
[GOTCHAS.md](GOTCHAS.md) has the details — and the check is
`CONFIG_USB_HOST_HUBS_SUPPORTED` appearing in the generated `sdkconfig`, not
in `sdkconfig.defaults`.
