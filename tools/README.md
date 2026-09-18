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
