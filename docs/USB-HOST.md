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

**What this is not** is a filesystem that mounts itself. Nothing is mounted until
you say so, and no path under a USB stick is reachable before that: `lsblk` names
the filesystem, and `mount sda1 /mnt` is the next command. See
[Stage 2](#stage-2--mounting), including the one shape of mounting that would
have been worse than not having it at all.

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
| `NAME` | `sda`, `sdb`… in slot order, with `sda1`… for the partitions in the MBR. The number is the *entry's*, so a skipped one leaves a gap exactly as `fdisk` does |
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
known value are left out rather than printed empty. One key is a flag rather than a value: `SKIPPED="1"` says the partition table held entries espix could not represent, so the partition lines beside it are not the whole story. Both commands take names as
operands — `lsblk` a disk (`sda`), `blkid` a disk or a partition (`sda1`, which
prints that line alone, because the value is what was asked for) — and an operand
that names nothing is refused rather than quietly printing less. **Neither prints
anything at all when nothing is attached** — an empty table is an answer, and an
empty header is noise.

`mount` and `umount` take those same names — `mount sda1 /mnt` — and are described
in [Stage 2](#stage-2--mounting), which is where the "nothing is mounted until you
ask" part of this document is.

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
named and marked unsupported. Two filesystems keep nothing in sector 0 at all and are found by a read further in: **ext2/3/4**, whose superblock is at 1024, and **ISO 9660**, whose primary descriptor is at 32768 — which is why a Linux-prepared stick or an installer image read as a bare disk with an empty `FSTYPE` until those offsets were read. A stick that is empty, or holds something nothing here recognises, stays a bare disk, and nothing is missing from that answer: Linux's own `blkid` reports nothing for such a stick either.
- **Filesystems espix has no driver for are named, not hidden.** The partition
table says `exfat/ntfs` (one MBR code covers both, so it is as specific as sector
0 gets), `linux` (`0x83` is "Linux any"), `ext2/3/4` when that partition's own superblock says so, `iso9660` when its descriptor does, or `gpt`, each marked `(unsupported)`. `0xEF` — "EFI (FAT-12/16/32)" to `fdisk`, and what every Arch, CachyOS and Windows installer writes — is **`vfat`**, and mountable: it is FAT, and the library has no code for it, so espix adds that one itself. That is the whole reason
the command exists before mounting does: silence would read as an empty disk.

  `vfat`, `littlefs` and `raw` are *not* marked, and the marker is about the type
  rather than this build: FatFs is in the ESP-IDF image either way, and what is
  missing is the mount plumbing, not the driver.
- **Labels come from the filesystem's own first sector.** An MBR has nowhere to put a volume
  label, so the 11 space-padded bytes at `0x2B` (FAT12/16) or `0x47` (FAT32) are
  read from the partition's first sector, using the filesystem-type string the
  formatter wrote to decide which. A volume still called `NO NAME` reports none.
  Bytes are copied as they stand — a label in a non-ASCII code page is not
  transcoded, because nothing here knows which page that was. An ISO's 32-byte volume identifier is read the same way, from its descriptor — which is the block a hybrid image's partition begins with.
- **The partition table is walked by espix, not by the library.** The library
  ends the table at a `0x00` *type byte*; a hybrid ISO image — an Arch or CachyOS
  installer, whose first entry is typed `0x00` with a real start and size — then
  loses every entry after it, including the FAT EFI partition that is the only
  thing on such a stick espix can mount. Measured on a CachyOS 202604 installer:
  2.8G of ISO 9660 typed `0x00`, then 23M of EFI FAT typed `0xEF`, and espix
  showed neither. So the walk is espix's, and the table ends when an entry is
  *empty* — no start and no size — which is a different thing. The type table is
  still the library's, with the additions above; [UPSTREAM.md](UPSTREAM.md)
  carries the report.
- **Entries it still cannot show are counted, not hidden.** A type byte nothing
  here can name, or an entry pointing outside the device, leaves a row that is
  real but nameless: the row is printed anyway, and `lsblk` says so beneath the
  table — `sda: entries not shown (an unnameable type, or an entry outside the
  device)` — with `blkid` putting `SKIPPED="1"` on the disk line for a script.
  What it does not do is follow a chain: the logical partitions *inside* an
  extended partition (`0x05`/`0x0F`) are not read, so a stick with five
  partitions shows its primaries and the note.
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
| `espressif/esp_ext_part_tables` 0.5.0 | the partition type table — its codes, and the names espix prints for them. The MBR walk itself is espix's, because of the `0x00` rule below |

The MSC driver hands out an `esp_blockdev` handle per device (IDF 6.0.4 and
later), and espix keeps it: that is what Stage 2 mounts from —
`espix_usb_dev_blockdev()` lends it — and holding it does not tie up the driver's
own.

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
4. **Mount it:** `mount sda1 /mnt` is silent on success, `mount` lists it as
   `sda1 on /mnt type vfat`, `ls /mnt` shows the stick's files, and `cat` reads
   one. `cat /mnt/photo.jpg > /tmp/copy` on a 30MB stick is the read path end to
   end, and `umount /mnt` afterwards leaves `mount` printing nothing again.
   Refusals worth seeing once each: `mount sda2 /mnt` where sda2 is exFAT says
   `exfat/ntfs is not supported`, an installer stick's ISO partition says
   `iso9660 is not supported`; `umount /mnt` while a file is open says `busy`;
   and **do not pull the stick while it is mounted** — that is the gap in
   [KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem), not a test.
5. **Pull it out:** `dmesg` says `sda: removed`, `lsblk` no longer lists it, and
   the *name* is immediately reusable — a second device took `sda` again.
   `free` before and after is the check that the slot and its buffers went back.
6. **A hub:** the hub and anything non-storage is enumerated and listed by
   `lsusb`; one storage device attaches. A second storage device is refused for
   want of host channels (see above) — that is the part's limit, not a fault.
7. **The suite agrees:** `make test SUITE=usb` runs `75-usb.sh`, which asserts the
   role, the absence of `usb0`, and that both commands answer — and skips the
   enumeration half when nothing is plugged in. **`make test SUITE=net`** covers
   the other side: it now asserts that a build without USB-NCM really has no
   `usb0` rather than merely skipping the link checks.
8. **In a device-role build**, `lsblk` and `blkid` still exist and explain
   themselves: `lsblk: usb host was not built into this image
   (CONFIG_ESPIX_USB_ROLE_HOST)`. A command that vanished from a build would
   leave you comparing the device against this page and guessing.

### Measured

Image sizes, both roles built back to back on the same tree, ESP-IDF v6.1,
esp32s3, with the shipped defaults (`ESPIX_USB_VERBOSE=n`):

| build | `espix.bin` | free in a 4MB app partition |
|---|---|---|
| `ESPIX_USB_ROLE_HOST` (default) | 0x13ee10 — 1,306,128 B | 69% |
| ... and Stage 2 (mounting, FatFs) | 0x146310 — 1,336,080 B | 68% |
| `ESPIX_USB_ROLE_DEVICE` | 0x1339f0 — 1,260,016 B | 70% |

Stage 2 costs about **29KB**: FatFs itself (`ff.c` and its Unicode tables) plus
`fat.c`, the partition view and the two commands. The device-role figure above is
from Stage 1 and has not been re-measured — its image grows too, because `mount`
and `umount` exist in that build as well (they answer that the host was not built
in) and that is what keeps FatFs linked there.

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

## Stage 2 — mounting

`mount sda1 /mnt` works, `ls` and `cat` reach it like any other directory, and
`umount /mnt` gives the stick back. What follows is why it is built the way it
is, and the two places the plan on file was wrong.

### The volume is routed, not registered

The quick version — `esp_vfs_fat_bdl_mount(handle, "/mnt", ...)` — is wrong twice
over rather than merely inelegant, and both reasons were on file before any of
this was written:

- **It skips the permission check.** espix registers its VFS at IDF's *fallback*
  prefix `""`. Anything registered at `/mnt` is therefore routed by IDF *before*
espix sees the path: no `resolve()`, no `espix_fs_root_permits()`, no cwd. A
`confine`d process could read the whole stick.
- **`chmod` on a FAT file would write to the wrong partition.** `mode.c`'s
  `attr_store()` hardcodes `ESPIX_FS_ROOT_PARTITION`: a FAT path whose mode is
  not the rule's default would have stored littlefs attributes for a path that is
  not on littlefs.

So espix routes it itself, which is what [ROADMAP.md](ROADMAP.md#filesystem)
planned: `components/espix_fs/vfs.c` holds a table of mounts, picks one by
longest matching prefix, and reaches the filesystem below by a direct call to its
ops. `/mnt/photo.jpg` passes through the same code and the same check as
`/etc/passwd`.

| piece | what it is |
|---|---|
| `lower_t s_mounts[ESPIX_FS_MAX_MOUNTS]` | the single `lower_t` that was always described as "an array when mounting lands" |
| `espix_vfs_add_mount()` / `_del_mount()` | publish and remove a filesystem at a prefix; the root is slot 0 |
| `stored_metadata` | false for FAT, which is what makes `chmod` answer EPERM instead of filing metadata against the wrong volume |
| `components/espix_fs/fat.c` | the FAT driver: IDF's ops behind 24 shims that strip the mount prefix and hand IDF its own context back |
| `tools/patch-fatfs.py` | gives IDF's FatFs a mount-without-registering split; see below |

### Two corrections to the plan

**The fd packing ROADMAP.md expected is not needed.** With two filesystems below,
the worry was that LittleFS's fd 3 and FAT's fd 3 would collide when they came
back into espix's `read()`. They cannot: espix passes the lower filesystem's fd
through unchanged, and IDF allocates those from one global table, so an fd
identifies its filesystem by construction. What routing does need is the reverse
question — which mount does this fd belong to? — and that is a 256-byte array
indexed by fd, plus the same idea keyed by pointer for open `DIR` handles, which
carry no fd at all. About as much code as the packing would have been, and
ROADMAP.md is corrected.

**`esp_vfs_fat_bdl_mount()` cannot be trimmed down; IDF's FatFs is patched.**
Three obstacles, each of which rules out a cheaper option:

- every public entry point that mounts a block device ends in
  `esp_vfs_register_fs()`, so mounting through IDF *is* registering a prefix;
- the ops tables (`s_vfs_fat`, `s_vfs_fat_dir`) are file-scope static, so the
  driver cannot be reached directly either;
- the context those ops need is built *inside* `esp_vfs_fat_register()`, whose
  only other job is the registration, so the two cannot be separated from outside
  the file.

`tools/patch-fatfs.py` therefore adds `esp_vfs_fat_ctx_create()`,
`esp_vfs_fat_ctx_free()` and `esp_vfs_fat_get_ops()`, and lifts the context
construction out of `esp_vfs_fat_register()` rather than making a copy of it — one
implementation with two callers, and nothing to drift.
`tools/esp_vfs_fat-ctx.patch` is the same change as a patch ready to send, and
[UPSTREAM.md](UPSTREAM.md) carries the request.

fatfs is a **built-in** IDF component, and the registry publishes no
`espressif/fatfs` that could override it (checked — it 404s), so unlike
`joltwallet/littlefs` this patches the IDF installation itself. The change is
additive — `esp_vfs_fat_register()` does exactly what it did — but the hook is
written to fail loudly, because a patch in a tree espix does not own is precisely
the kind of thing that goes missing quietly. The script checks the IDF version and
every anchor, and CMake is told to re-configure when either patched file changes.
Without that last part, a reinstalled IDF sails past the hook and fails as an
undefined reference to `esp_vfs_fat_ctx_create()` pointing at espix instead of at
the real cause — which is not hypothetical: it is what the first version of the
hook did, and
[GOTCHAS.md](GOTCHAS.md#a-configure-time-hook-is-not-a-build-time-guarantee)
records it.

### What mounting does, and does not do

- **Nothing is formatted, ever.** `esp_vfs_fat_*_mount()` will `f_mkfs()` a volume
  that does not look like FAT when asked to. A stick somebody plugged in to read
  must never come back empty, so a failure to mount is reported as one, with the
  `FRESULT` named: "not a FAT filesystem" and "drive not ready" are different
  news.
- **FAT only.** `mount sda1 /mnt` on an exFAT or NTFS volume answers
  `exfat/ntfs is not supported` — the same words `lsblk` prints, for the same
  reason, and likewise `ext2/3/4 is not supported` and `iso9660 is not supported`
  for the volumes `lsblk` can now name precisely.
- **FAT has no modes to show.** FatFs reports `0777` for everything it stats, so the
  VFS *replaces* those bits with espix's own — the same rule a file on the rootfs
  gets — and keeps the type bits from the filesystem below. A mounted stick
  therefore reads `-rw-r--r--`/`drwxr-xr-x`, `chmod` refuses because there is
  nowhere to store a change, and what `stat()` reports is what the access check
  enforces. Or-ing the two together instead — which is what this did until a
  second filesystem made the difference visible — left every file on a stick
  world-writable and executable in the listing while the check said 0644, so a
  binary that the loader would refuse looked runnable.
- **A mounted volume belongs to whoever mounted it.** FAT stores no ownership, so
  the mount carries the mounting session's `uid` and `gid` — the `uid=`/`gid=`
  Linux gives a removable volume, so that the person who plugged the stick in can
  write to it rather than finding everything root's. `sudo mount` therefore gives
  a root-owned volume, exactly as a root mount does on Linux; `esp` writing a
  stick it plugged in itself wants espix to grow a non-root way to mount, which it
  does not have yet.
- **Root only**, as `mount(8)` is: it changes the namespace for every session.
  `mount` with no arguments lists what is mounted and anyone may run that, as
  anyone may read `/proc/mounts`.
- **`umount` refuses while something is open on the mount.** It answers `busy` and
  says why, rather than pulling a volume out from under a reader who is halfway
  through a file.
- **Unplugging a mounted stick is not handled yet.** The block device is borrowed
  from espix_usb, which gives it back when the device goes, and a filesystem still
  holding it would be reading memory that was freed. Unmount first —
  [KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem) has the mechanism and the fix
  that is next.
- **`df` still reports the rootfs.** Per-mount free space is one `f_getfree()`
  away and not yet wired to a command.
- **Two volumes at a time.** `CONFIG_FATFS_VOLUME_COUNT` is 2, and that is what
  FatFs sizes its drive table by — the mount table itself has room for three
  beyond the root, so FatFs's number is the one that bites. One USB storage
  device at a time is the tighter limit in practice; the knob is the answer if a
  device ever arrives with three FAT partitions worth mounting at once.
- **`/mnt` is in the boot skeleton** now, so a device whose image predates
  mounting still has somewhere to mount to.

### Devices have names in /dev

`/dev/sda` and `/dev/sda1` exist, so a volume can be named the way every other
system names it: `ls /dev` lists what is plugged in, `stat /dev/sda1` reports the
medium's size, and `mount /dev/sda1 /mnt` takes the operand someone would actually
type. `sda1` and `/dev/sda1` mean the same thing to `mount`, `umount` and `blkid`.

They are names rather than streams. Opening one answers `EOPNOTSUPP` instead of
handing back a file descriptor, because raw sector access would have to know
which device it holds and refuse to open one that is mounted — a feature with
decisions of its own, not something to fake meanwhile.

Where the halves meet is worth knowing, since it is where the layering could have
gone wrong: nothing in espix_fs knows what USB is and espix_usb knows nothing
about the VFS, and both were left that way. `espix_usb_set_dev_hook()` fires on
attach and detach with the device's row intact, and `main/espix_main.c` — which
already depends on both — turns that into `espix_dev_register_block()` and
`espix_dev_unregister_block()`. Registering a name that exists updates it rather
than adding a second, and the node pool is fixed at twenty (four disks and four
partitions each), so no sequence of attaches can fragment the heap or outgrow it.

## Roadmap, not now

- ~~**Unmount when the device is pulled.**~~ **Done**, in three pieces: the
  detach hook (`espix_usb_set_dev_hook()`), a mount that the hook marks dead, and
  a sentinel `lower_t` whose empty ops tables turn every operation on that mount
  into `ENOSYS` instead of a read through a released block device. `umount` skips
  the volume sync for a dead mount, because `f_mount(NULL)` would write a dirty
  volume back through the same freed device.
  Measured: an idle pull auto-unmounts and the shell survives; a pull with a file
  open keeps the mount marked, reads answer `ENOSYS`, `df` declines the row, and
  `umount` succeeds once the handle is gone. **One case is not espix's to fix** —
  a transfer already in flight when the device goes — and it is written up in
  [UPSTREAM.md](UPSTREAM.md#a-device-pulled-mid-transfer-takes-the-heap-with-it).
- **`df` per mount.** One `f_getfree()` behind an `espix_fs_stat_fat()`, and the
  point where `df` stops being a rootfs-only command.
- **`READ CAPACITY(16)`,** so a drive larger than 2 TiB reports its real size
  instead of a plausible 2 TiB. It is the class driver's choice of SCSI command,
  not espix's, so it belongs to the list below as much as to here.
- **A GPT reader**, once a disk turns up that needs one. The protective MBR is
  reported today rather than followed.
- **A USB keyboard (HID).** Deferred until there is a display, which is the
  honest position: with no screen, a keyboard's only use would be a test that
  prints what was typed, and nothing else in espix would consume the events. The
  test itself is cheap when a display arrives — SSH in over WiFi, run a command
  that prints decoded keystrokes, type on the keyboard — and it sidesteps the
  can't-plug-both-sockets problem entirely.
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
- **Whether VBUS needs board-side control: it does not, here.** Answered for one
  hub on one board — a PD hub that powers the board *and* enumerates devices
  works, with the board taking its power through the same socket the host
  controller uses. Nothing is left to do in software: a port that is not switched
  in hardware cannot be switched from code, so this is a wiring question for a
  future board rather than a task. `boards/*.conf` would be the only place to
  express it, and it has no precedent — those files carry flash size, PSRAM mode
  and a partition table, nothing else.

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
