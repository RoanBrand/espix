# espix OTA: kernels as files, installed by a loader

**Status: built and running.** Sections 1--10 work out the A/B design; section
11 is the single-kernel-slot + loader design that replaced it and is what the
tree implements. The reasoning in 1--10 about space, version identity, the
manifest, security and costs still holds, but wherever those sections describe
*writing the passive slot* as the mechanism, section 11 governs: the kernel
never writes a slot. It archives the running image to `/boot`, verifies and
queues other images, and the loader installs them.

The goal is the Ubuntu/pi A/B experience applied to espix: an update arrives,
the bootloader switches to it, and a kernel that does not come up is rolled back
automatically. Plus the part that makes it
pleasant to use: `upgrade` on the device, an update repository it can point at,
and a login that mentions an available update without going near the network.

## 1. Space: do we have enough?

The question as asked. The answer is not just "yes" -- the A/B layout is very
nearly free.

Current 16MB image, from `build/espix.bin`:

| | |
|---|---|
| `espix.bin` | 1,486,608 B = **1.42 MiB** |
| `factory` partition | 4 MiB (2.8x the image -- heavily over-provisioned) |
| `storage` (littlefs `/`) | 11.875 MiB |

**The "41%" is headroom over the *image*, not the size of a slot.** Today's
`factory` is 4 MiB but the image is 1.42 MiB, so 2.58 MiB of it is simply unused.
A 2 MiB slot holds the same 1.42 MiB image and still leaves 596 KiB spare -- that
spare is the "41% room to grow before the image stops fitting". Two 2 MiB slots
occupy the *same 4 MiB* the single over-provisioned `factory` already occupied,
which is why the rootfs barely moves. Nothing is being cut down to 2 MiB; the
current partition is just far larger than the thing in it.

| layout | rootfs | slot headroom over the image |
|---|---|---|
| 16MB today (`factory`, no OTA) | 11.8750 MiB | 2.58 MiB, unused, one slot |
| **16MB A/B, `1.9375 MiB` slots, `/` untouched** | **11.8750 MiB** | 532 KiB (**+36.7%**) |
| 16MB A/B, 2 MiB slots, `/` moved | 11.8125 MiB | 596 KiB (+41%) |
| 16MB A/B, 2.5 MiB slots, `/` moved | 10.8125 MiB | 1.08 MiB (+76%) |
| 8MB today (`factory`) | 4.8750 MiB | -- |
| 8MB A/B, 1.75 MiB slots, `/` moved | 4.3125 MiB | 348 KiB (+23%) |
| 8MB A/B, 2 MiB slots, `/` moved | 3.8125 MiB | 596 KiB (+41%) |
| 8MB A/B, `1.46875 MiB` slots, `/` untouched | 4.8750 MiB | 53 KiB (+3.6%, too tight) |

Both 16MB rows end at the last flash byte and leave a working second slot. The
difference between them is whether the rootfs moves, and the recommended row is
the one that does not.

The 4 MiB-slot variant considered here was rejected for the same reason: it
costs 4 MiB of rootfs (11.9 -> 7.8 MiB) -- paying for a slot size 2.8x the image
for nothing.

### Proposed 16MB table -- and `/` keeps everything on it

The recommended layout holds `storage`, `coredump` and `nvs` at **the exact
offset and size they have today**, so `/` does not have to be reflashed or
reformatted:

```
# Name,     Type, SubType,  Offset,    Size,      Flags
nvs,        data, nvs,      0x9000,    0x6000,
otadata,    data, ota,      0xf000,    0x2000,
phy_init,   data, phy,      0x11000,   0x1000,
ota_0,      app,  ota_0,    0x20000,   0x1F0000,
ota_1,      app,  ota_1,    0x210000,  0x1F0000,
coredump,   data, coredump, 0x410000,  0x10000,
storage,    data, littlefs, 0x420000,  0xBE0000,
```

Against today: `nvs` is byte-for-byte where it was, `coredump` is where it was,
and `storage` is at 0x420000 with the same 0xBE0000 size -- littlefs mounts the
existing filesystem untouched, which was **verified on hardware** by flashing this
table over the old one and finding the same files and the same used space. What
changes is that `otadata` appears at 0xf000, `phy_init` moves from 0xf000 to
0x11000 (regenerable PHY calibration data, nothing is lost), and the old single
4 MiB `factory` region becomes two 0x1F0000 slots.

**Why 0x1F0000 and not 0x1F8000.** The binding constraint is not the image size
but the partition table: an *application partition offset* must be 0x10000
aligned. With `ota_0` at 0x20000 the next slot can only start on a 64 KiB
boundary, so a slot size that is not a multiple of 0x10000 leaves `ota_1`
misaligned -- 0x1F8000 puts it at 0x218000 and the generator rejects the table
outright ("Offset 0x218000 is not aligned to 0x10000"). A full 2 MiB slot would
put `ota_1` at 0x220000 and end the slots at 0x420000, which is exactly where
`storage` starts, leaving nowhere for `coredump` without moving the rootfs. The
answer is 0x1F0000 slots plus the 64 KiB of slack between 0x400000 and 0x410000.
Only the *size* may be merely 4 KiB aligned; the 0x10000 size rule belongs to
Secure Boot v1, which espix does not use.

An exact 2 MiB slot is still possible by starting `ota_0` at 0x10000 instead of
0x20000, but that needs `nvs` shrunk from 0x6000 to 0x4000 to fit `otadata` below
0x10000, and `nvs` holds live WiFi state. Not worth 64 KiB per slot.

### What the gap actually costs

| layout | unused bytes before the rootfs | capacity in one slot | per-slot headroom |
|---|---|---|---|
| today (`factory`) | 36 KiB (bootloader + partition table) | 4 MiB | -- |
| recommended, 0x1F0000 slots | 148 KiB | 1.9375 MiB | 532 KiB (+36.7%) |
| exact 2 MiB (shrink `nvs`) | 36 KiB | 2 MiB | 596 KiB |

Two gaps make up the 148 KiB: the 56 KiB between 0x12000 and 0x20000, where
`ota_0` has to start because `nvs` + `otadata` + `phy_init` end at 0x12000; and
the 64 KiB between 0x400000 and 0x410000, which the 64 KiB offset alignment of
`ota_1` leaves behind. Together that is 0.9% of the flash, and the exact-2 MiB
alternative buys it back only by shrinking `nvs`, which holds live WiFi state.
The old `factory` layout had no gap because nothing had to sit between `nvs` and
the app.

### What `otadata` is for

`otadata` (type `data`, subtype `ota`, 0x2000) is the OTA data partition, and it
is not optional: **without it there is no A/B at all.** It holds two
sector-sized copies of an entry per slot -- `{ota_seq, ota_state, crc}` -- written
alternately with a counter, so losing power mid-write cannot corrupt the choice.
The bootloader reads `ota_seq` to decide which slot to boot
(`boot_index = (ota_seq - 1) % app_count`), and `ota_state` carries the rollback
state (`NEW`, `PENDING_VERIFY`, `VALID`, `INVALID`, `ABORTED`). With otadata
all-0xFF the bootloader boots `factory` if there is one, otherwise `ota_0` --
which is why a freshly UART-flashed board still boots with no OTA history. It is
8 KiB of flash and there is nothing to tune.

### The migration cost

On 16MB, none -- the rootfs stays put as described above, so the change is a
partition-table reflash and an app reflash, not a wipe. The "decide before this
layout is in the field" note in the CSV still applies in spirit: changing the
slot size *later* does move `storage`, and at that point `/` is lost. Choosing
the slot size now is what avoids that second reflash.

### When the image outgrows the slot

The image is 1.69 MiB of a 1.9375 MiB slot, so there is room. The day it does
not fit, the layout has to change -- and that is the one operation IDF's OTA
cannot do. `esp_ota_*` writes inside a partition whose offset and size are
fixed; nothing in IDF resizes a partition or rewrites the partition table. A
layout change is a flash operation, whoever performs it.

What can be preserved:

* **Only `storage` really has to move.** Growing two adjacent slots by X needs
  X of free space in front of them, and everything after shifts by X. `nvs`,
  `otadata`, `phy_init` and `coredump` can move freely -- they are
  regenerated. `storage` holds `/`, and a littlefs superblock names its
  own block count and begins at the partition's first block, so move the
  partition and the old filesystem is no longer at offset zero and will not
  mount. littlefs has no online resize: shrinking or moving it means copying the
  files out, formatting, and copying back.
* **If only non-storage partitions move, `/` survives.** The 2 MiB-slot
  variant above is exactly that: shrink `nvs` from 0x6000 to 0x4000, start
  `ota_0` at 0x10000, and `storage` stays at 0x420000 with everything on it
  intact. A cable reflash of the table and the app is enough.
* **Once `storage` must shrink or move, the files come out first.** A USB disk
  is the natural home -- espix already mounts one, and `cp -a / /mnt/sda1/` is
  the whole backup. Or PSRAM, for the image rather than the rootfs: 6.7 MiB free
  holds a 2 MiB app comfortably, and 12 MiB of rootfs not at all.

Ways to do it, cheapest first:

1. **Buy the headroom now.** `/` is about 21% used. Spending another half a
   megabyte or so of it on bigger slots costs one reflash today and removes the
   problem for a long time. If the image is expected to grow, this is far and
   away the cheapest answer, and the only one that avoids a field migration.
2. **A non-storage table change** -- shrink `nvs`, move `otadata`: a cable
   reflash, with `/` intact. That covers the next step, about 2 MiB slots.
3. **A migration build, delivered by OTA.** A build that still fits the old slots
   can carry the new layout and perform it: back `/` up, write the new images
   at their new offsets with raw flash writes, write the new partition table at
   0x8000 last, reboot, format the new `storage`, restore. OTA is only the
   delivery here -- the repartition is still raw flash work, and losing power
   before the table switch leaves a board that needs a cable. The usual
   hardening applies: the table goes last, and a valid image stays at the offset
   the old table points to for as long as possible.
4. **A recovery partition.** A small app in its own partition, valid in both
   layouts, whose only job is maintenance: reformat the rootfs, write a table.
   That is the safe home for step 3's dangerous part, and what to build if field
   migrations become routine. `factory` + `ota_0` + `ota_1` is the
   IDF-sanctioned shape, at the cost of a third copy of the app.

Two notes on the workaround of downloading into `/` and overwriting both
slots. Fetching is not the hard part -- PSRAM holds the image and a USB disk
holds anything -- so landing it on the rootfs buys nothing and costs space. And
overwriting both slots does not help either: the slots' `offsets` are what
change, not the space between them, and after the table switch a slot only counts
if an image sits where the new table says. The rootfs is the real casualty, and
it is a data question, not a slot question.

The board is always recoverable with a cable: `make flash-all` writes the
bootloader, the table, the app and a fresh rootfs. That is not a fallback to be
embarrassed about -- it is the honest answer to a layout change, and the design
goal is to need it as rarely as possible.

### 8MB boards

`/` cannot be preserved on 8MB with two useful slots: keeping `storage` at
0x320000 leaves only 0x2F0000 for both, i.e. 1.46875 MiB each and 3.6% headroom
against a 1.42 MiB image. On 8MB this is a genuine trade, not a free one.

* **Superseded.** The A/B-era decision was "no OTA, `factory` only" for 8MB,
  because two useful slots did not fit. The loader design (section 11) removed
  that trade -- one 2MB kernel slot, a 320KB loader and a 5.5MB rootfs fit -- so
  the 8MB table now uses the same layout as the 16MB one and OTA is on for both.
  The arithmetic above is what set the slot sizes, not whether to have any.

One slot plus OTA is not a thing, and rollback is not why. OTA needs a *passive*
slot to write while the active one runs; the partition the running image is
executing from cannot be rewritten in place. Two slots are the hard requirement.
Rollback then costs no flash on top of them -- it is state bits inside otadata --
so turning rollback off would save nothing at all.

## 2. What ESP-IDF actually provides -- and a correction

The plan in the request assumed "IDF has a standard protocol to listen on a
port, so firmware can be pushed remotely instead of over UART". **That protocol
does not exist.** Verified in the v6.1 tree:

* `examples/system/ota/native_ota_example` is **not a server**. It is an
  HTTP(S) client that *pulls* an image from a host-run web server; the only
  ports in it (8002, 8070) are host-side test scaffolding in
  `pytest_native_ota.py`.
* There is **no `espota.py`** anywhere in ESP-IDF. (That tool is Arduino's, a
  different project.)
* `idf.py` has **no `ota` or `ota-flash`**. Its only OTA commands are
  `erase-otadata` and `read-otadata`, both **serial**.
* `components/app_update/otatool.py` is **serial-only**, a wrapper over
  esptool; there is no network path.
* IDF ships **no host tool that pushes firmware over TCP** to a device-side
  server.

What IDF does provide, and what we should use:

* `esp_https_ota` -- a complete HTTPS pull-and-flash client with redirect
  following (301/302/303/307/308; an HTTPS origin may not redirect down to
  HTTP), CA-bundle support, chunked responses, and progress callbacks.
* `app_update` -- `esp_ota_begin/write/end/set_boot_partition`, the passive-slot
  selection, and the rollback state machine.
* The bootloader -- slot selection and the rollback-on-unconfirmed-boot.

So the dev-iteration goal ("no more swapping the hub and the UART cable") is
still met, just by a different route than a listening port: **the device pulls
from a server the developer already has running.** That is what `make ota` will
do, and it needs no new on-device attack surface.

### The remote-push path we already have for free: SSH

For pushing a locally built image there is a second, arguably better route that
needs no HTTP server and no new listener at all: the SSH session is already an
authenticated, encrypted channel that espix owns. `make flash-ota` finds the
board in the gitignored `.espix/hosts`, copies the image over, installs it and
reboots to run it:

    scp build/espix.bin esp@espix:/tmp/espix.bin
    ssh esp@espix 'sudo upgrade --file /tmp/espix.bin'

or, with the image already on a USB disk:

    upgrade --file /mnt/sda1/espix.bin

**Streaming it in on stdin does not work, and the reason is worth recording.**
The obvious form -- `cat build/espix.bin | ssh esp@espix 'sudo upgrade --stdin'` --
stalls: a command that declares a stack of its own runs on a new task while the
connection task waits on it (`session.c`, `run_on_own_task`), and the connection
task is the only reader of the wire. Nothing drains the channel, so the reader
waits forever and the session dies with otadata left mid-write. A foreground
*app* gets away with reading stdin because the connection task pumps while it
runs; a builtin on its own task does not. Making `--stdin` work means teaching
`run_on_own_task` to pump, which is a transport change rather than an OTA one.
The scp form costs 1.7 MB of `/tmp` -- cleared at boot anyway -- and works today.

An unauthenticated OTA listener on a LAN port would be the single most dangerous
piece of code in the system; not building one is a feature.

### Not planned: an ArduinoOTA-style push listener

The other project's convenience came from ArduinoOTA -- the IDE finds the board
and pushes firmware with no cable -- and it is tempting to hand-roll the same
small protocol (UDP discovery, a TCP transfer with an MD5 password challenge) so
PlatformIO's `espota.py` would work with no Arduino dependency. It is
deliberately **not planned**:

* An unauthenticated listener that rewrites firmware is the single most dangerous
  thing in the system, and a password-challenged one is still a second
  authentication system to build and maintain.
* SSH already does exactly this job, authenticated and encrypted, the Unix way: a
  firmware push is `ssh`, not a bespoke protocol. Adding a listener when the
  secure path already exists is surface for its own sake.

Recorded so it is a decision rather than an oversight. The protocol is small, so
if it is ever wanted it can be added behind a build option that defaults off.

### The device-pull path

    upgrade                 # check the manifest, prompt y/N
    sudo upgrade -y         # check, then install without asking
    upgrade --check         # report only; exit 0 if current, 1 if newer

The manifest is one small HTTPS GET; the image is fetched only if the version
is newer. Default source: espix's own GitHub releases.

## 3. Version identity, and how to compare

espix carries **two** version notions today, and they are not the same:

* `espix_version()` (`components/espix_kernel/include/espix_kernel.h`) is the
  hand-maintained `0.3.0` -- a promise about behaviour.
* `esp_app_desc_t.version` -- what OTA tooling sees, and what `build/espix.bin`
  carries at offset 0x30 -- is `CONFIG_APP_PROJECT_VER`, the git describe
  (`2376160`). `espix_build_id()` composes it with the ELF SHA prefix for the
  flash-identity guard.

So a manifest saying `0.4.0` has nothing to compare against: the device's own
descriptor says `2376160`. The two options, more plainly than before:

1. **Keep both notions, and put both in the manifest.** `version` (semver) is
   compared against the running `ESPIX_VERSION_STR`; `build` (the git describe)
   is compared against the app descriptor of the *downloaded* image, to prove the
   bytes are the ones the manifest named. No plumbing changes -- but two version
   fields that can disagree, and OTA tooling (`idf.py image-info`, `esptool`,
   anything else) still sees a commit hash where it expects a version.
2. **Unify: make the release version *the* version.** `esp_app_desc_t.version`
   becomes the semver, which is what every OTA tool and the bootloader expect.
   The build identity stays the ELF SHA.

**Recommendation: unify, now.** It is smaller than it looks -- the macros are
used in exactly three places (`espix_kernel.h`, `kernel.c`, and the SSH version
banner) -- and doing it after the manifest format is fixed means re-releasing
with a changed format. Concretely:

* Add `version.txt` at the project root. IDF already gives it top priority for
  `PROJECT_VER`, so the app descriptor in `build/espix.bin` carries the semver
  with no other change.
* `PROJECT_VER` is passed as a compile definition *only* to `esp_app_desc.c`, so
  the SSH banner needs a small generated header (`configure_file()` in the top
  CMakeLists from `${PROJECT_VER}`). `ESPIX_VERSION_MAJOR/MINOR/PATCH` go away
  and `ESPIX_VERSION_STR` comes from that header: one source of truth, not two.
* `espix_build_id()` becomes `<version>+<elfsha9>` -- the same shape as today, so
  `tests/run.sh`'s `dd` extraction from offsets 0x30 and 0xB0 keeps working
  unchanged. The git describe drops out of the *identity* but can stay for display
  via a generated `ESPIX_GIT_DESCRIBE`; the SHA is what actually catches "rebuilt
  without flashing", so the guard loses no power.
* One release check: git tag `vX.Y.Z` must equal `version.txt`.

**Done, and verified on hardware.** `version.txt` exists; the header is generated
from it by `components/espix_kernel/CMakeLists.txt`; the app descriptor and the
SSH banner both read `0.3.0`. `espix_build_id()` is now the nine-hex content
hash, so the device reports:

    $ uname -a
    espix esp32s3-cb5d74 0.3.0 #acad9bd8d ESP32-S3 ESP-IDF v6.1-dirty
    $ uname -r
    0.3.0
    $ uname -v
    #acad9bd8d

`uname -a` is sysname, node, release, build, machine, then the SDK -- `rev0.2`
and `2-core` were dropped because Linux reports neither there. A release
build shows `#1` in the build field where a development build shows the content
hash, the same rule motd follows, so an official espix carries no hash at all in
its identity strings.

Linux's field order, one hash, and motd shows `espix 0.3.0` for a release build
or `espix 0.3.0+bfbb8cd` for a development one. `uname` takes the real option
letters (`-s -n -r -v -m -a`, combinable like `uname -sn`). `tests/run.sh`
compares `uname -v` against the ELF SHA read out of `build/espix.bin`, and
00-smoke is 15/15.

### Deciding whether there is an update

With both sides speaking the same language, the rule is:

* manifest `version` newer than the device's -> update;
* same `version`, different `build` -> update (a rebuilt or rolling release);
* older `version` -> never, unless explicitly forced;
* same `version` and same `build` -> up to date.

The second rule is the one to be deliberate about, and it is a good default for a
pre-1.0 project that rebuilds `main` often: a new build at the same version is
picked up, and once installed the device's own `build` matches, so it is not
offered again. The failure mode is the stable case -- if a published release
asset is ever rebuilt in place at the same version, every device on that version
is offered an update once. That is fine now and should be revisited when releases
freeze; a `channel=stable|dev` setting could gate it.

`build` should be the **ELF SHA-256**, the identity `espix_build_id()` already
uses, because it is content-addressed and comparable on both sides. The
manifest's `sha256` (of the `.bin`) is a third, independent check.

## 4. The device side

### A new component: `espix_ota`

Follows the existing graph: depends on `espix_kernel`, `espix_net`,
`espix_time`, `espix_fs`; `espix_cmds` owns the command and calls into it, the
way `cmd_motd` and `cmd_net` already do. Nothing else gains an OTA dependency,
so the graph stays acyclic.

Public surface, roughly:

* `espix_ota_init()` -- read config, archive the running image, start the
  periodic checker.
* `espix_ota_check(url, ...)` -- fetch and parse the manifest, compare.
* `espix_ota_download(url, name, sha256, ...)` -- fetch an image into `/boot`,
  verifying it against the manifest's hash.
* `espix_ota_adopt(path, ...)` and `espix_ota_queue(name, ...)` -- put a local
  image in `/boot` and hand it to the loader.
* `espix_ota_archive_self(...)` -- keep this running image as a file, verified
  against the hash in its own header.
* `espix_ota_known_update(char *ver, ...)` -- the cached answer, for motd.
  Touches no network.
* `espix_ota_running_slot_name()` -- `ota0`/`ota1`, for `uname`/motd.

### Build options

Two options, each defaulted so a plain build is right:

* `CONFIG_ESPIX_OTA_ENABLED` -- default `y` for 16MB flash, `n` otherwise
  (`default y if ESPTOOLPY_FLASHSIZE_16MB`). Off, the command is not registered,
  no check runs, and nothing pulls in `esp_https_ota`.
* `CONFIG_ESPIX_OTA_URL` -- the default manifest URL, baked in as this repo's
  GitHub `releases/latest/download/espix-ota.json`. Runtime `/etc/espix.conf`
  (`ota.url=`) overrides it, and `ota.auto_check=off` disables the periodic check
  while leaving `upgrade` usable.

**The option gates code, not layout.** Both the 16MB and 8MB tables carry
`ota_0`/`ota_1` whether or not the option is set, so a build with OTA compiled
out still flashes to `ota_0` and boots normally, and enabling the option later
needs no repartitioning. The tables are the same shape at both sizes.

The one mismatch worth catching is an OTA-enabled build paired with a table that
has no loader (no `ota_1`). The top-level CMake could parse the selected CSV and
fail the build with a clear message, rather than letting it appear later as
`upgrade` refusing.

### Memory: handle the failure, and say what was needed

Internal RAM is the binding constraint (section 6 has the measurements), but the
policy is not a gate that refuses before trying -- it is to handle each failure
where it happens, cleanly, and to report numbers worth acting on:

* Every allocation on the path is checked, and a NULL is handled at the point it
  occurs rather than surfacing later as a crash. No failure touches the running
  image.
* The error carries both figures, because "no memory" alone is not actionable:
  `out of memory: internal free 18 KiB, largest block 12 KiB; a TLS handshake
  needs about 40 KiB -- close a session and retry`.
* Everything that does not have to be internal goes to PSRAM. The pool is the
  other way round from the numbers people expect -- about **6.7 MiB free in
  PSRAM against ~107 KiB internal** -- so the OTA download buffer
  (`buffer_caps = MALLOC_CAP_SPIRAM`), the manifest, our own buffers and the OTA
  task's stack (the `...WithCaps` task API) all come from PSRAM. IDF's docs
  suggest internal memory for the download buffer when there is room, for
  throughput -- it should be the first thing moved back if internal ever stops
  being tight.
* What genuinely cannot move is the mbedTLS context and its in/out buffers
  (`MBEDTLS_INTERNAL_MEM_ALLOC`), and that is the `40 KiB` that can fail. If it
  proves painful, `CONFIG_MBEDTLS_DYNAMIC_BUFFER` cuts the peak from roughly
  42 KiB to 22 KiB at some throughput cost -- a documented lever, not a guess.
* One install at a time.

### Config and state

Config follows the house pattern -- a text file read with `espix_fs_conf_get()`,
the same way `/etc/wifi.conf` and `/etc/usb.conf` work, and hand-editable with
the expected effect:

```
# /etc/espix.conf
ota.url=https://github.com/RoanBrand/espix/releases/latest/download/espix-ota.json
ota.auto_check=on
```

Machine state -- when we last checked, and what we found -- is a separate
question. Recommendation: **a small world-readable file under `/var/lib/espix/`**
rather than NVS. espix's whole personality is inspectable text on a filesystem;
a user can `cat` why motd mentioned an update, and NVS stays reserved for
ESP-IDF's own WiFi/PHY use. The alternative, an NVS namespace, is more atomic
and does not touch littlefs -- a legitimate choice, and worth settling before
the code lands.

### The manifest

Attached to every release as a constant-named asset, alongside the image:

```
{
  "name": "espix",
  "version": "0.4.0",
  "build": "a1b2c3d",
  "chip": "esp32s3",
  "min_version": "0.3.0",
  "url": "https://github.com/RoanBrand/espix/releases/download/v0.4.0/espix.bin"
}
```

Constant asset names (`espix.bin`, `espix-ota.json`) are what make
`releases/latest/download/<asset>` resolve without the GitHub API. Checked
against live repositories rather than assumed:

* **Two redirects, not one.** `latest/download/<asset>` answers 302 to
  `releases/download/<tag>/<asset>`, which answers 302 to
  `release-assets.githubusercontent.com/...` with a signed, time-limited query
  string (about an hour). `esp_https_ota` follows redirects by default and
  refuses an HTTPS-to-HTTP downgrade, so this works -- but a client that did not
  follow redirects, or that retried a stale signed URL much later, would fail.
* **No API call**, which is the point: the unauthenticated limit it avoids is
  **60 requests/hour/IP** (`x-ratelimit-limit: 60`). GitHub does publish SHA-256
  digests for release assets now, but reading them means the API, so the
  practical integrity path is to bake the digest into the manifest.
* **Limits**: up to 1000 assets per release, each file under **2 GiB**, with no
  documented total-size or bandwidth cap. A 1.4 MiB image is nowhere near any of
  it.
* **TLS**: `github.com` chains to USERTrust ECC / Sectigo; the asset host serves
  GitHub's wildcard certificate (SAN covers `*.githubusercontent.com`), which on
  today's chain runs through Let's Encrypt's *Generation Y* hierarchy --
  `ISRG Root YR`, served cross-signed by `ISRG Root X1`. `ISRG Root X1` and
  USERTrust ECC are both in `cacrt_all.pem`, which
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` builds from, so the device
  validates today with no pinned certificate. **`ISRG Root YR` itself is not in
  the bundle**; the chain only builds because the cross-signed root is served.
  If GitHub ever stops sending it, the bundle must be updated or the asset host
  pinned -- worth knowing before it surfaces as a mystery "Failed to verify
  certificate".
* GitHub still serves HTTP/1.1, which is all `esp_http_client` speaks. One
  esp_https_ota quirk on this path: partial downloads do not work with chunked
  transfer encoding, so the simple non-partial download is the one to use.

The manifest is still worth having over reading the image header directly: one
small GET yields the version, the URL and the release notes, whereas a header
probe means opening a full TLS connection and starting an OTA just to answer
"is there anything new?".

Integrity comes from three independent checks: the transport (TLS to
github.com), the image's own appended SHA-256 that `esp_ota_end` verifies, and
a comparison of the downloaded image's app descriptor against the manifest's
`build`.

### `/dev/factory` has to be repointed

`components/espix_fs/dev.c` exposes the app partition as a readable `/dev/factory`
node, and it looks up **`ESP_PARTITION_SUBTYPE_APP_FACTORY` specifically**. With
the A/B table there is no factory partition, so it returns NULL, logs "no factory
partition to expose", and every read fails with `EIO`. That is not theoretical:
`tests/suites/45-throughput.sh` pulls `/dev/factory` to measure scp download
throughput, and the docs use `cp /dev/factory /mnt/sd1/` to pull the running image
off the board.

Settled design:

* **`/dev/ota0` and `/dev/ota1`** name the app partitions that exist -- in the
  loader layout, the kernel and the loader. `ota0` rather than `ota_0`: IDF labels the partitions
  `ota_0`/`ota_1`, but `/dev` reads better in the `sda1` style and the mapping is
  one line.
* **`/dev/factory` is present only when a `factory` partition exists.** Neither
  shipped table has one now, so the node is absent rather than broken; a table
  that does would keep it exactly as before. That is the honest reading of the
  name -- it is the factory partition, not "the running image" -- and it removes
  the `EIO` failure without giving the node a second meaning.
* **Which slot is running is implicit** -- `upgrade --slots` is run by the
  kernel -- and that is where each slot's role, otadata state
  (`NEW`/`PENDING_VERIFY`/`VALID`/`INVALID`/`ABORTED`), offset and size belong,
  along with the images in `/boot` and their good / previous / pending roles, the
  state a slot table cannot show. There is deliberately no "next boot" column:
  otadata points back at the kernel before a prompt is available.
* `45-throughput.sh` pulls whichever of `/dev/ota0` or `/dev/factory` exists, and
  sizes the transfer from the bytes it actually received rather than a hardcoded
  4 MiB.

### motd, without a login-time network call

The request was explicit and correct: **no synchronous check at login.** motd
and the greeting read only the cached state. When a check has previously found
something newer, the greeting gains a line such as:

    Update   espix 0.4.0 is available; run 'upgrade'

and nothing at all when there is none. `espix_ota_known_update()` is a memory
read.

### The periodic check

A low-priority task, deliberately undemanding:

* woken by an `IP_EVENT` when an address appears, so it runs as soon as there
  is a route rather than on a fixed delay, and never in the boot path;
* otherwise due at most once per 24h, measured on the monotonic clock so an NTP
  step cannot move the interval; the stored wall time guards only the
  across-reboot case, and only while the clock is synced (a device with no time
  source checks once per boot);
* a 15-minute timeout is the backstop for the 24h re-check and for a route that
  was already up when the handler was registered;
* never while an install is running;
* records the outcome for motd and moves on.

Reachability is the gate, not the clock. The clock is not required for the TLS
handshake here (`CONFIG_MBEDTLS_HAVE_TIME_DATE` is off, so certificate
validity dates are not checked), and gating on it would disable the check
entirely on a device whose time server is unreachable but whose network is
fine.

## 5. Rollback, and when espix counts as "good"

### What piboot actually does

Worth stating precisely, because it is the model this feature is copied from and
it is not quite "two fixed slots". Ubuntu's Pi A/B lives in `/boot/firmware` as
folders: `current/` (always present), `new/` (the untested one), and optionally
`old/` (the previous known-good, deleted to reclaim space). State is a per-folder
`state` file reading `good`, `unknown`, `trying` or `bad`:

* flashing writes `new/` and sets `new/state=unknown`, never touching `current/`;
* the next boot a service flips it to `trying` and reboots into the Pi's
  "tryboot" mode, whose selector is a clear-on-read register -- so any failure
  falls back to `current/`;
* a later service, `piboot-try-validate`, runs at `multi-user.target` and -- by
  default -- calls a validation script that just runs `true`, then marks `new/`
  `good` and swaps the names;
* failures are caught by a hardware watchdog plus `panic=10`, and a fallback
  writes `bad`.

Two things carry over. First, the "did the boot succeed" decision belongs to
userspace, late in boot, not to the bootloader -- espix's equivalent is the
point in `app_main` where the console is up. Second, the attempt is ephemeral by
construction: one try, then either commit or fall back. That is very close to
IDF's `PENDING_VERIFY`, which also gives exactly one attempt -- piboot simply
names the intermediate states and keeps the previous image around more
explicitly.

### What IDF gives us

Enabling `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (currently off) changes the
semantics in a way that must be designed for, not discovered:

* On success, `esp_ota_set_boot_partition()` marks the new slot
  `ESP_OTA_IMG_NEW`.
* On the next boot the bootloader sets it to `PENDING_VERIFY` and boots it.
* If the new image does not *confirm itself* before the next reset, the next
  boot turns `PENDING_VERIFY` into `ABORTED`, which is not selectable -- so the
  previous image boots. **One boot attempt, no retry counter.** A panic in early
  init therefore rolls back for free.
* A running image in `PENDING_VERIFY` **cannot begin another OTA**;
  `esp_ota_begin` returns `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` until it is
  valid.

So espix must call `esp_ota_mark_app_valid_cancel_rollback()` at a defined
point. Too early and a broken-but-not-crashing image is blessed; too late and a
user cannot re-`upgrade` during the first boot.

Recommendation: **after `app_main`'s init completes and the console is up** --
i.e. the system reached the point where a human can ask it anything. That is the
espix analogue of piboot's "userspace says the boot succeeded". It is
deliberately *not* conditioned on a login, which would break a headless boot.
A short grace delay could be added if early-boot flakiness shows up in practice.

One interaction to check on hardware: `espix_fault` intercepts panics and
stores a core dump. A panic on the first boot of a new image must still lead to
a reset (and thus a rollback) rather than to a wedged board.

## 6. Costs and hazards measured, not guessed

**Internal RAM is the real constraint, and it is manageable.** Measured on the
board over SSH:

| | internal heap free |
|---|---|
| idle, one session | 107 K |
| six held sessions | 52 K |
| min since boot | 38 K |

Each session costs roughly 11 K. A TLS handshake needs about 35--40 K with the
current mbedTLS settings (`IN_CONTENT_LEN=16384`, `OUT=4096`,
`MBEDTLS_INTERNAL_MEM_ALLOC=y`; IDF's own measured figure is ~42 K for a
validating HTTPS request, ~22 K with dynamic buffers). With one or two sessions
that fits comfortably; at five-plus it does not. Therefore `upgrade` must
**check free internal heap before starting and refuse with a clear message**
rather than half-download and fail obscurely. The OTA download buffer itself can
live in PSRAM (`esp_https_ota_config_t.buffer_caps`), which helps but does not
remove the TLS requirement.

**ROM is cheaper than expected.** The ELF already links `esp_ota_ops.c.obj`
wholesale -- `esp_ota_begin/write/end/set_boot_partition/get_state_partition`
are all in the image, pulled in by the flash coredump -- and
`mbedtls_ssl_handshake_client_step`, x509 parsing and the full CA bundle are
already linked via wpa_supplicant's `esp_eap_client.c`. So HTTPS OTA adds only
`esp_https_ota` + `esp_http_client` + `esp-tls` + our code: tens of KB, not
the ~100 KB an HTTPS stack usually costs from scratch. To be measured properly
once there is a build.

**Flash writes freeze the world, and this board cannot avoid it.** `esp_ota_write`
disables the cache while programming: tasks running from flash are suspended,
only IRAM-safe interrupts run. The usual mitigation is
`CONFIG_SPI_FLASH_AUTO_SUSPEND`, but this board's flash (Boya, `0x68`) does not
support suspend -- already documented in `docs/ROADMAP.md`. So installs will
cause brief, repeated system stalls, and the SSH connection carrying the command
will hiccup. Use `esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES, ...)` so erases
are interleaved in small chunks rather than one long bulk erase, and document
the behaviour. `ESP_TASK_WDT` is 5 s with panic off; watch it during the first
install.

**The flash-identity guard will fire on a release-pulled board.**
`tests/run.sh` refuses to run when the running build id is not
`build/espix.bin`. For the local push paths that guard stays exactly right --
the board runs the bytes you just built. For a release pulled from GitHub it
fires correctly and needs a deliberate, explicit bypass, not a silent one.

## 7. What OTA does not cover

**Only the application is updated. `/` is not.** Two slots replace the kernel
plus espix's own components; the littlefs rootfs, `/bin`, `/etc`, NVS and the
core dump partition are untouched. A release that only changes code in
`components/` ships cleanly this way; a release that adds or changes files under
`/bin` or `/etc` will not. That is worth stating plainly now, because the
Ubuntu analogy the feature is modelled on *does* cover the whole system, and the
difference will surprise someone eventually. A future "rootfs image" slot, or
shipping a tarball the release also updates, is where that would go.

## 8. Security posture

* OTA writes to a partition: **root only**, which is the existing `sudo` model.
* HTTPS with the CA bundle for the network paths;
  `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` stays **off** in release builds. A dev build
  can enable it to pull from a laptop, and that is the only thing it is for.
* No on-device listener at all. The dev push is over SSH, which is already
  authenticated; section 2 records why the ArduinoOTA-style variant is not
  planned.
* TLS authenticates the *server*; the image hash authenticates the *bytes*.
  Neither proves the release was built by someone trustworthy, and note that
  IDF does **not** enforce any "this version is newer" rule -- the application
  compares versions itself, which is the whole reason for the manifest. Secure
  Boot v2 and signed images are the real answer and are a separate decision;
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` together with
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` is the cheap half -- signed
  images without the full secure-boot eFuse commitment -- and is worth
  considering in the same breath as the rest of this.

## 9. Milestones

Sequenced so each step is useful on its own and the risky parts are proven
early.

1. **Unify the version.** `version.txt`, the generated header,
   `espix_build_id()` in its new shape, `tests/run.sh` still green. Small, and
   everything after it depends on the identity being one thing.
2. **Partition table + rollback + local install.** *Done and verified on
   hardware.* The new 16MB table (rootfs untouched), rollback enabled,
   `upgrade --slots` and `upgrade --file`, `mark_app_valid` from `app_main`.
   Installing the image into `ota_1`, rebooting into it and watching the state
   go `NEW` -> confirmed `VALID` is the whole path minus the transport.
3. **Dev push loop.** *Done, with one substitution.* `esp_https_ota` and
   `esp_http_client` are in, `upgrade <url>` fetches over TLS (verified against
   GitHub, including a clean refusal of a body that is not an app image), and
   `make flash-ota` installs over SSH. The `--stdin` form was dropped for the
   reason in section 2. This is the milestone that removes the UART cable from
   day-to-day work.
4. **Manifest + GitHub + check/install UX.** *Done.* `/etc/espix.conf`
   (`ota.url`, re-read every time so editing it takes effect at once), the
   manifest parser, `upgrade` / `-y` / `--check`, `min_version`, the free-heap
   reporting, progress and reboot handling, and `tools/ota-manifest.sh`, which
   writes the manifest a release publishes (its `build` is the same content hash
   the device reports as `uname -v`, read from the descriptor so the two cannot
   disagree). Verified over HTTPS: a manifest advertising `0.3.1` reports an
   update and exits 1, one matching the running version and build reports up to
   date and exits 0. A positive install straight from the repo needs a published
   release -- or an HTTP dev server plus `CONFIG_ESPIX_OTA_ALLOW_HTTP`, which is
   off by default and was used for the local checks.
5. **Notification.** *Done and verified.* A low-priority task checks five
   minutes after boot and then at most once a day, only with a default route and
   a set clock, recording the verdict in `/var/lib/espix/update` (a plain text
   file -- `cat` it to see why the greeting says what it says). The greeting reads
   that cache and never the network: with a cached `0.3.1` it shows
   `Updates   espix 0.3.1; run 'upgrade'`, and with no cache it shows nothing.
6. **Documentation and cleanup.** README "Updating later", `KNOWN-ISSUES.md`,
   `ROADMAP.md`, and the removal of the `TAB completes, UP/DOWN walks history`
   line from the greeting, which no modern system says.

## 10. Open decisions

Decided in this document: the A/B slot size (0x1F0000, so the rootfs does not
move), one table per board regardless of the option, the device-pull model, SSH
rather than a push listener, and unifying the version rather than carrying two.
(Section 11 then replaced the layout: both sizes now run one kernel slot and a
loader, and the kernel carries the version while the loader has its own.)

Still open:

1. **Update state: a file under `/var/lib/espix/`, or NVS?** (section 4)
2. **Signed images / Secure Boot now, or after OTA works?**
3. **Rootfs updates: explicitly out of scope for v1?** (section 7)

## 11. Alternative: one kernel slot and a loader

A second shape, prototyped on the `bootloader-and-bigger-kernel-space` branch.
Instead of two equal A/B slots, a small loader and one big kernel slot. The
kernel gets the space two slots used to share, the loader goes first so it can
do its work before a kernel exists, and rootfs does not move.

```
# Name,     Type, SubType,  Offset,    Size,      Flags
nvs,        data, nvs,      0x9000,    0x6000,
otadata,    data, ota,      0xf000,    0x2000,
phy_init,   data, phy,      0x11000,   0x1000,
ota_0,      app,  ota_0,    0x20000,   0x70000,     # loader, runs first
ota_1,      app,  ota_1,    0x90000,   0x380000,    # kernel
coredump,   data, coredump, 0x410000,  0x10000,
storage,    data, littlefs, 0x420000,  0xBE0000,
```

3.5 MiB for the kernel and 448 KiB for the loader -- the two app slots occupy
exactly the region two equal A/B slots did -- and `storage` is byte-for-byte
where it was.

The loader is built with `-Os` (the kernel is on `-Og` and stays there),
asserts off, nano printf and no err-to-name. It keeps INFO logging, a banner and
a line per decision: it runs for about a second and reboots, so those lines are
only visible to whoever is watching UART, and that is worth the few kilobytes.

| loader build | size |
|---|---|
| `-Os`, INFO logs and banner, asserts off, nano printf | **198 KiB** |
| plus `-flto` | does not link -- IDF's asm stubs lose `xt_unhandled_exception` |

198 KiB sits in a 448 KiB slot with 57% free, which is the headroom the partition
tables it will carry are meant to use.

**The loader must be an OTA partition, not `factory`.** `factory` is not an
OTA subtype (`esp_ota_ops.c`, `is_ota_partition()`), so
`esp_ota_set_boot_partition()` cannot target it, and `esp_ota_begin()` on it is
`ESP_ERR_INVALID_ARG`. As `ota_0` it is both writable and selectable, and
`esp_ota_set_boot_partition()` switches to it like any slot.

**Why rollback still works, for free.** The kernel in `ota_1` boots as
`PENDING_VERIFY`; if it does not confirm, the bootloader marks it `ABORTED` and
falls back to the other app -- `ota_0`, the loader -- which restores the previous
kernel file. The kernel decides it is healthy; the loader performs the restore.
That is the same division of labour as `piboot-try-validate`, and it is
IDF's rollback machinery rather than a replacement for it.

**State does not live in rootfs.** Two different things, two places:

* **Did the last kernel confirm?** `otadata` already answers this -- `NEW`,
  `PENDING_VERIFY`, `VALID`, `ABORTED` -- and IDF maintains it. The loader only
  has to read `esp_ota_get_state_partition()` (or
  `esp_ota_get_last_invalid_partition()`) to know a try failed.
* **Which file is which?** NVS: a namespace with a couple of keys naming the
  good and pending files. Small, persistent, survives a rootfs wipe, and does
  not live in the thing being replaced. If NVS and rootfs disagree, the loader
  treats the image actually in `ota_1` as good -- it can read its descriptor --
  and repairs the record.

### Who does what

The split that keeps the loader trivial: **the kernel archives itself, and the
loader only ever installs a file that is already there.**

* **Kernel, on boot:** if `/boot` has no file matching its own build id, copy its
  own partition there -- `/boot/espix-<version>-<build>.bin`. That happens once
  per freshly flashed image, it makes the running image visible like any other,
  and it means a single-image board still has a rollback target. Reading its own
  partition is safe; nothing writes `ota_1` while the kernel is running.
* **Kernel, on upgrade:** write the new image to `/boot`, set NVS `pending` to
  that filename, select `ota_0` (the loader), reboot.
* **Loader, every run:** if `ota_1` is `ABORTED` -- a try failed -- install NVS
  `good`; else if `pending` is set, install that; else do nothing. Select
  `ota_1`, reboot. It never writes `/boot` and never downloads, which is why
  it needs no TLS and stays under 200 KiB.
* **Kernel, on confirm:** `good` becomes itself, `pending` cleared.

The invariant is one line: **every image in `ota_1` has a file in `/boot`, and the
loader only ever writes `ota_1` from one of those files.** A wiped rootfs breaks
it, and the kernel repairs it by re-archiving itself on the next boot.

Two corners to decide up front: if `good` is missing when a rollback is needed
(a rootfs wiped at the wrong moment) the loader has nothing to restore, and it
must say so rather than loop on a failed `ota_1`; and `/boot` should keep the
current and pending images only, deleting older ones after a confirm, or a
12 MiB rootfs slowly fills with kernels.

### Why the loader is `ota_0`

**Chosen: loader first (`ota_0`), kernel second (`ota_1`).** A blank otadata
boots `ota_0`, so the loader runs before the kernel on a fresh flash and after
every update. That is what lets it own the work that must happen before a kernel
exists or before a kernel can be trusted: writing the partition table for the
detected flash size, and selecting the kernel. The kernel keeps only what is its
own -- creating, mounting and seeding the rootfs -- and the bootloader still
provides rollback: a kernel that does not confirm falls back to the loader.

The cost is one extra reset on a fresh flash, since the loader selects the kernel
and reboots. Kernel-first was tried and worked, but it left the loader unable to
do anything before the first kernel boot, which is exactly what provisioning
needs.

### One image for every flash size

The build targets the largest supported flash (16 MB), so the whole chip is
addressable, but it is flashed with the *smallest* partition table (8 MB), which
is valid on any larger chip. On the first boot the loader reads
`esp_flash_get_physical_size()` -- the real chip size, as opposed to
`esp_flash_get_size()`, which a build configured for 8 MB would clamp -- writes
the matching table at 0x8000, verifies it with `esp_partition_table_verify()`,
reads it back, and restarts. The kernel then sees the full partition.

The two tables differ only in `storage`'s size, so this is a pure size fix-up.
Both are generated from the same CSVs at build time and carried as byte arrays,
so the loader needs no partition-table format knowledge.

**It took a Kconfig option to be allowed to do it.** IDF refuses application
writes outside a partition: `main_flash_region_protected()` returns
`ESP_ERR_NOT_SUPPORTED` for the partition-table region, and `CHECK_WRITE_ADDRESS`
turns that into `abort()` under the default
`CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS`. The symptom was a board that reset in
a loop with no error at all; `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y` in the
loader -- the knob for an app that legitimately writes the table -- is what makes
the write land.

The cost is one build-time warning: IDF checks the app against the first OTA
partition, which here is the *loader*, so it prints

    Warning: 1/2 app partitions are too small for binary espix.bin ...
      - Part 'ota_0' ... size 0x70000 (overflow ...)

The kernel goes to `ota_1` (0x380000) and fits; the warning is aimed at the
wrong slot and is harmless.

The board identity follows from this: target and PSRAM, with flash size
deliberately absent because one image covers them all. On the S3 the two PSRAM
modes differ, so the name carries it (`s3-r8` octal, `s3-r2` quad); on the S31
there is only one PSRAM mode and its size is detected at runtime, so the identity
is plain `s31`, and one build serves every WROOM-3 module (the image targets
that module's 16 MB flash).

The S31's table also starts fresh, with no legacy to match: the loader takes
320 KB, the kernel slot 3.5 MB, and `storage` begins at 0x3F0000 and fills the
rest -- about 4 MB even on an 8 MB module. There are 8, 16 and 32 MB tables,
differing only in `storage`'s size, exactly as the S3's pair does.

**Two hashes live in the image, and they answer different questions:**

* `app_elf_sha256` in the descriptor -- a SHA-256 of the *ELF*, patched in by
  esptool. That is the build identity `uname -v` already prints, and it is what
  a `/boot` filename should carry. Naming a file needs no hashing at all.
* an appended SHA-256 of the image itself -- what `esp_image_verify()` checks.

A digest computed on the device is only worth what it can be compared against,
so:

* **Archiving the running image** is guarded by
  `esp_image_verify(ESP_IMAGE_VERIFY_SILENT, ...)`: the device recomputes the
  image hash and compares it to the one baked in at build time, so a corrupted
  flash fails instead of being certified by a digest of its own corrupt bytes.
  (A corrupt flash is caught even earlier in practice -- esptool verifies after
  writing and the bootloader verifies before jumping -- but this is the right
  reference and it is cheap.)
* **Installing a download** is where a self-computed hash is exactly right,
  because the manifest is the independent authority: hash the file, compare to
  the manifest's `sha256`, and only then record it as pending. That check
  belongs in the running kernel, before the loader is ever selected, so a bad
  download is reported to the session that fetched it rather than becoming a
  reboot, a failed install and a rollback with nobody watching.

The files themselves live in `/boot`, named for their version, which is the
whole point: a kernel version is a file you can list, copy and keep, not an
offset.

```
/boot/espix-0.3.0-26ea8a3ab.bin   good
/boot/espix-0.4.0-9f1c2b7d4.bin   pending
```

**The loader never needs TLS.** Downloading happens in the running kernel, which
writes the new image to `/boot` and records it as pending; the loader then has
a local file and nothing else to do. The only path that needs the network is the
one that is still running.

### Keeping the previous version, and running out of room

Steady state is two files: the running image and the one before it.

* **On confirm**, the kernel sets `previous = good`, `good = <itself>`, and
  deletes anything else. The pair rolls forward one step each time.
* **A failed boot** still restores `good` -- the loader never consults
  `previous`. The second file is not part of the automatic path.
* **What `previous` buys** is a *manual* rollback: if a new image boots but a
  runtime bug makes it unwelcome, `upgrade --rollback` queues the previous
  file and the loader installs it. That is the A/B convenience without a second
  slot, for one NVS key, one retention rule and a flag. Confirming a rollback
  swaps the pair again, so it is not one-way.

Every upgrade path ends the same way: `upgrade <url>`, `upgrade --file` and
`upgrade --rollback` all *queue a `/boot` file* and select the loader. The
kernel never writes a slot, the loader never downloads.

Space needs a policy, because images now live on the rootfs:

* Downloading wants room for a third file beyond `good` and `previous`.
  With room, verify first and commit after -- a failed download then costs
  nothing.
* If only two will fit, delete `previous` first and say so. The fallback for
  this upgrade is still `good`, so nothing important is given up.
* If two will not fit, refuse. Upgrading would mean deleting the only known-good
  image, so a boot that fails has nothing to restore. A `--force` can
  proceed with that stated plainly, for someone who would rather risk a cable
  than keep the old version.

This is the one place the loader design is weaker than A/B: two fixed slots
always have room for an image, and a full rootfs does not. Better said out loud
than found at three in the morning.

### Two things the loader bring-up taught us

Each of these was a real failure on hardware, not a hypothetical:

* **IDF's VFS will not mount at `/`.** `is_path_prefix_valid()` requires at
  least two characters, so the loader mounts the rootfs at `/fs` and reaches the
  images as `/fs/boot/<name>`. The kernel never notices: it does not register
  littlefs at a path at all.
* **The loader must enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.**
  `esp_ota_set_boot_partition()` writes otadata's state as
  `set_new_state_otadata()`, which is `ESP_OTA_IMG_NEW` only when the *writing*
  app has rollback enabled. Without it the loader leaves the installed kernel
  `UNDEFINED`, the bootloader never marks it `PENDING_VERIFY`, and a kernel
  that does not come up is never rolled back -- the whole point of the design,
  silently off.

What looked like a third cause was not. The rootfs image is built with
`--name-max=64`, and the loader does set `CONFIG_LITTLEFS_OBJ_NAME_LEN=64` --
but littlefs only refuses to mount when the image's stored `name_max` is
*larger* than the mounter's, and it adopts the smaller of the two. The loader's
default was already 64, so nothing was wrong there. (The setting is a format
parameter: it sizes the generated image, not a run-time buffer, so there is
almost nothing to trade against it.) It must merely never be set *below* the
kernel's value.

The loader logs at INFO: a banner and a line per decision (`make flash-monitor`
flashes without resetting and lets the monitor's reset be the only one, which is
how you see them). It runs for about a second and reboots, so those lines are
only visible to whoever is watching UART -- which is exactly when they are
useful. The first version was silent, and that is how the mount failure above
went unnoticed.

---

### Publishing a release

`make release` tags `v<version.txt>`, rebuilds as a release, and publishes with
`gh`. The tag is created *before* the build because that is what makes motd stop
calling it a development build (`espix_kernel`'s CMakeLists checks for the exact
tag on a clean tree); the tag is deleted again if the build fails.

One build now covers every flash size of a PSRAM config, so the assets are named
for the model and what they are, not for the module:

    espix-s3-ota.bin         the kernel, for remote updating          (likewise s31)
    espix-s3-minimal.bin     first flash, no rootfs (the kernel provisions the FS)
    espix-s3-full.bin        first flash, with the stock apps
    espix-ota.json           one manifest, keyed by board identity

Each target's identity is an entry: the S3's is `s3-r8`/`s3-r2`, the S31's is
`s31`. The device finds its own entry and reads the `url` from it, so an asset
name never has to encode the flash size, and a new target adds an entry rather
than renaming anything. release.sh merges every target's entry that is present in
the tree (`tools/ota-merge.py`), rewriting the URL to the tag being released.
`make release-all` loops the targets and is the usual way to cut one; `make
release` does the active target alone, and running it once per target reaches the
same release. The body is regenerated from the merged manifest
(`tools/release-notes.py`) on every run, including a refresh, so a release that
gains a board also gains that board's notes. The default URL is
`.../releases/latest/download/espix-ota.json`, so one release is enough.

The loader is not part of an OTA release. It is in both flash images, is flashed
by cable, changes far less often than the kernel, and has its own version
(`loader/version.txt`), so an OTA update never has to carry it.

**What is still missing**: automated tests for the provisioning step, for the
fail-to-confirm-then-restore cycle, and for the retention and `--rollback`
policy. Everything else here is exercised on hardware -- the swap and the loader
banner (`make flash-monitor`), table provisioning from the 8 MB bootstrap, the
single manifest and its `s3-r8` lookup, and a full GitHub download, SHA-256
check, queue, loader install and confirm -- but by hand, not by the suite.

---

## Sources

Claims here that did not come from the ESP-IDF checkout or a measurement on the
board:

* Ubuntu piboot A/B:
  https://ubuntu.com/hardware/docs/boards/explanations/piboot-ab/
* GitHub `/releases/latest/download/<asset>`:
  https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases
* GitHub API rate limits (60/hour/IP unauthenticated):
  https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api
* Release asset limits (1000 assets, 2 GiB per file):
  https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
* ESP-IDF OTA, and partition tables:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html
