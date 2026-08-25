# tools

Host-side tooling. Mostly empty for now — this is where the app deployment
story lives, and right now that story is "rebuild the filesystem image".

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

Baking apps into the filesystem image still works and is how the rootfs gets
its initial contents, but it is no longer how you iterate:

```bash
cp build/hello.app.elf ../../fsroot/bin/hello
cd ../.. && idf.py build && idf.py -p <port> storage-flash
```

`storage-flash` replaces the whole filesystem, losing anything created on the
device. That is why `littlefs_create_partition_image()` deliberately does not
use `FLASH_IN_PROJECT`: a plain `idf.py flash` updates the firmware and leaves
the filesystem alone.

## What belongs here later

- A wrapper that builds an app and copies it across in one go.
- Host-side coredump/backtrace decoding for the fault records `dmesg` reports.
