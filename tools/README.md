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

Teaches the lwext4 core to honour the metadata checksum seed, which is what a
volume made by e2fsprogs 1.47 or later needs. `mke2fs` enables
`metadata_csum_seed` by default now, so every metadata checksum on such a volume
is seeded from the superblock rather than from the filesystem UUID — and the
core, seeding from the UUID in eight places, refused the volume at mount. Correctly
enough, since the feature was not implemented; the effect was that espix could not
read any ext4 volume a current Linux creates. See
[../docs/KNOWN-ISSUES.md](../docs/KNOWN-ISSUES.md) and
[../docs/ROADMAP.md](../docs/ROADMAP.md).

The firmware build runs it from a CMake hook, after `project()` because that is
when the download happens. It is idempotent, and it fails the build with a clear
message rather than skipping if the pinned revision moves or the tree has been
left half-patched.

```bash
./tools/patch-lwext4.py        # applied automatically; safe to run by hand
```

Unlike the other patch scripts here it *applies* `lwext4-csum-seed.patch` rather
than repeating the change in Python, and that is deliberate: the change is ten
files and eight functions, so a second copy of it in this directory would be one
more thing to keep in step. This way the patch that gets applied and the patch
that goes upstream are the same bytes.

**Temporary by construction.** `lwext4-csum-seed.patch` is the change as a plain
diff, ready to send to `gkostka/lwext4` — the core, not the port around it. The
port would pick it up by bumping its submodule. When it lands, delete this script,
the patch and the `execute_process()` block in the top-level `CMakeLists.txt`, and
update the pin in `main/idf_component.yml`.

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
construction. Pinning that declaration to `int` is correct for every project, with
or without a widened `off_t`, which is why it is worth touching a file outside the
IDF tree at all; the hook finds it by asking the compiler for its sysroot.

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
