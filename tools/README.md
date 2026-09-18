# tools

Host-side tooling.

## build-apps.sh

Builds every project under `apps/` and stages the ELFs into `fsroot/bin/`, so
the rootfs image carries them. The firmware build runs it automatically —
apps are separate IDF projects rather than components, so without it `idf.py
build` produces no apps at all and a fresh clone would flash an empty `/bin`.

```bash
./tools/build-apps.sh          # all apps
./tools/build-apps.sh hello    # just one
```

An app whose staged binary is already newer than its sources is skipped, so the
usual cost to a firmware build is under a second. Turn it off entirely while
iterating on the firmware alone:

```bash
idf.py -DESPIX_BUILD_APPS=OFF build
```

An app needing build-time environment beyond `sdkconfig.defaults` puts it in
`apps/<name>/build.env`; `apps/neopixel/build.env` is the example, carrying the
`ARDUINO_SKIP_TICK_CHECK` that Arduino's 1000Hz assertion requires.

## patch-littlefs.py

Adds `esp_littlefs_setattr`/`getattr`/`removeattr` to the copy of
`joltwallet/littlefs` the component manager downloads, which is where espix
stores a file's mode. The port uses LittleFS user attributes itself, for mtime,
but exposes neither them nor the `lfs_t *` they need — and ESP-IDF's VFS has no
chmod hook to route around it. See [../docs/UPSTREAM.md](../docs/UPSTREAM.md).

The firmware build runs it from a CMake hook, after `project()` because that is
when the download happens. It is idempotent, and it fails the build with a clear
message rather than skipping if the pinned version or either anchor moves —
silently doing nothing would surface as an undefined reference pointing at
espix instead of at the real cause.

```bash
./tools/patch-littlefs.py      # applied automatically; safe to run by hand
```

**Temporary by construction.** `esp_littlefs-attrs.patch` is the same change as
a plain diff, ready to send to joltwallet/esp_littlefs. When it is upstreamed,
delete both files and the `execute_process()` block in the top-level
`CMakeLists.txt`, and bump the version in `main/idf_component.yml`.

## patch-lwext4.py

Teaches the lwext4 core the two things it needs that upstream lacks — both in the
*core*, which the component carries as a submodule, rather than in the port around
it.

`lwext4-csum-seed.patch` is the metadata checksum seed, which is what a volume made
by e2fsprogs 1.47 or later needs. `mke2fs` enables `metadata_csum_seed` by default
now, so every metadata checksum on such a volume is seeded from the superblock
rather than from the filesystem UUID — and the core, seeding from the UUID in eight
places, refused the volume at mount. Correctly enough, since the feature was not
implemented; the effect was that espix could not read any ext4 volume a current
Linux creates. See [../docs/KNOWN-ISSUES.md](../docs/KNOWN-ISSUES.md).

`lwext4-fwrite-error.patch` is the one that matters before anything writes to an
ext volume. `ext4_fwrite()` overwrites the error that sent it to its `Finish` label
with the result of releasing the inode reference, so a failed block write can
*commit* its transaction instead of aborting — and the caller is told the operation
succeeded. See [../docs/ROADMAP.md](../docs/ROADMAP.md), which is where the write
milestone is staged, and
[../components/espix_fs/ext.c](../components/espix_fs/ext.c) for why that milestone
is read-only until this lands.

The firmware build runs it from a CMake hook, after `project()` because that is
when the download happens. It is idempotent, and it fails the build with a clear
message rather than skipping if the pinned revision moves or the tree has been
left half-patched.

```bash
./tools/patch-lwext4.py        # applied automatically; safe to run by hand
```

Unlike the other patch scripts here it *applies* patch files rather than repeating
the change in Python, and that is deliberate: between them the changes are ten
files, eight functions and a two-line fix, so a second copy in this directory would
be one more thing to keep in step. This way the patch that gets applied and the
patch that goes upstream are the same bytes.

**Temporary by construction.** Both are plain diffs, ready to send to
`gkostka/lwext4` — the core, not the port around it. The port would pick them up by
bumping its submodule. When they land, delete this script, the two patches and the
`execute_process()` block in the top-level `CMakeLists.txt`, and update the pin in
`main/idf_component.yml`.

## patch-libc-offt.py

Keeps `_lseek_r`'s 32-bit ABI, now that `off_t` is 64 bits. `cmake/offt64.h` widens
`off_t` for the whole firmware — that is what lets a file over 4 GiB be reported and
seeked — and `_lseek_r` is an alias for `esp_vfs_lseek`, so it follows the type. Its
callers do not, and cannot: newlib's prebuilt `stdio.o` carries the seek logic
`fseek()` ends up in and was compiled when `off_t` was 32 bits, and the ROM's libc
stub table declares an int offset for the same reason. Followed the type, the
boundary reads 64 bits where those callers wrote 32, and every `fseek()` lands
somewhere else — measured, as a 12 KB SFTP upload failing with `seek failed`.

So `_lseek_r` keeps the width it has always had and widens inside, on its way to the
64-bit `esp_vfs_lseek`. espix's own `lseek()`, `pread()` and `pwrite()` are 64-bit
and unaffected, which is where a file over 4 GiB is actually reached. stdio's
seeking stays limited to 4 GiB — a property of the libc espix links against rather
than a choice, and the reason its SFTP transfer path has to move off `FILE*` to go
beyond it. See [../docs/KNOWN-ISSUES.md](../docs/KNOWN-ISSUES.md).

Three files, one of them outside IDF: the definition, the weak declaration that has
to agree with it, and — the one to know about — the toolchain's own `reent.h`, which
declares `_lseek_r` in terms of `_off_t` and so follows the widened type by
construction. The hook finds it by asking the compiler for its sysroot.

**That third file is outside IDF on purpose, and it is the only patch here that
is.** The others touch `$IDF_PATH` or `managed_components/` — input the build
already treats as fetched. `include/reent.h` belongs to the *compiler toolchain*,
shared by every project on the machine that uses it, so this is the one patch that
changes something espix does not own.

It is kept because both cannot be had: `_off_t` is what `off_t` widens, and that
declaration is written in terms of it, so it had to move for a 32-bit `_lseek_r` to
compile against a 64-bit `off_t`. And what it now says is *more* correct than what
was there: it declares the ABI the library was actually built with, which is what
its own compiled callers use.

**The cost, accepted knowingly.** For a project that never widens `off_t`, the
declared type becomes `int` where it was `long`: the same width and an identical
ABI, but a different declared type, which a pedantic build on another project could
notice.

**Undoing it.** Restore the line from `TOOLCHAIN` in the script, then delete that
dict, the `--toolchain-root` argument and its `CMAKE_CONFIGURE_DEPENDS` entry. The
`off_t` widening goes with it, for the reason above.

```bash
./tools/patch-libc-offt.py --idf-path "$IDF_PATH"    # or IDF_PATH=... with no argument
```

**Temporary by construction.** `esp_libc-lseek-abi.patch` is the IDF half of the
change as a plain diff, ready to send to espressif/esp-idf. When it lands, delete
both files and the `execute_process()` block in the top-level `CMakeLists.txt`. The
toolchain's `reent.h` is a separate matter with no upstream to send it to from here
— the patch file's closing note is addressed to whoever reads it next.

Unlike the other patch scripts this one is not about a *bug*: it is about an ABI
that was implicit while `off_t` was 32 bits, and stopped being implicit when a
project widened the type. The better fix upstream is a Kconfig knob for 64-bit
`off_t`, which would make the width explicit; there is none today.

## check-abi.py

Fails the build if a symbol an app resolves a layer below is not in the image.
espix's own tables are the allowlist, but two tables inside `elf_loader` answer for
62 standard names *first* — and each entry there is what anchors its function into
the image, so those names are present unless something takes them away: the
`CONFIG_ELF_LOADER_LIBC_SYMBOLS` / `_ESPIDF_SYMBOLS` options switched off, or a
newer `elf_loader` that dropped a name. Either way an app finds out as
`undefined symbol`, at load, on a device.

The build runs it after the link — a `POST_BUILD` step rather than a
configure-time hook like the patch scripts above, because there is no ELF to read
until then. It costs the image nothing, which is the whole reason it is not a
`_Static_assert`: naming a function to prove it exists is what pulls it in. That
argument, and the list it guards, are in
[`components/espix_proc/abi_libc.c`](../components/espix_proc/abi_libc.c).

```bash
python3 tools/check-abi.py --elf build/espix.elf --nm <toolchain>/xtensa-esp32s3-elf-nm
```

## Deploying an app

Build it on the host, copy it over, run it by name:

```bash
# 1. Build the app (a standalone IDF project producing a loadable ELF)
cd apps/hello
idf.py -G 'Unix Makefiles' set-target esp32s3    # once; enables `idf.py elf`
idf.py elf                                       # -> build/hello.app.elf

# 2. Copy it to the device and run it
scp build/hello.app.elf esp@esp32s3-cb5d74:/bin/hello
ssh esp@esp32s3-cb5d74 -t 'hello world'
```

No `-O` flag: espix implements the SFTP subsystem, which is what `scp` uses by
default from OpenSSH 9 onwards.

Baking apps into the filesystem image is how the rootfs gets its initial
contents, and now happens on its own:

```bash
idf.py build && idf.py -p <port> storage-flash
```

`scp` is still how you iterate — it does not disturb the rest of the filesystem,
where `storage-flash` replaces all of it.

`storage-flash` replaces the whole filesystem, losing anything created on the
device. That is why `littlefs_create_partition_image()` deliberately does not
use `FLASH_IN_PROJECT`: a plain `idf.py flash` updates the firmware and leaves
the filesystem alone.

## What belongs here later

- A wrapper that builds an app and copies it across in one go.
- Host-side coredump/backtrace decoding for the fault records `dmesg` reports.
