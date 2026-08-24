# tools

Host-side tooling. Mostly empty for now — this is where the app deployment
story lives, and right now that story is "rebuild the filesystem image".

## Deploying an app today

There is no SCP yet (deferred under roadmap item 5), so apps reach the device
by being baked into the rootfs image and flashed:

```bash
# 1. Build the app (a standalone IDF project producing a loadable ELF)
cd apps/hello
idf.py -G 'Unix Makefiles' set-target esp32s3    # once; enables `idf.py elf`
idf.py elf                                       # -> build/hello.app.elf

# 2. Stage it into the rootfs
cp build/hello.app.elf ../../fsroot/bin/hello

# 3. Rebuild the image and flash just the storage partition
cd ../..
idf.py build
idf.py -p <port> storage-flash
```

`storage-flash` writes only the `storage` partition. Note that this replaces
the whole filesystem — anything created on the device is lost. That is why the
firmware's `littlefs_create_partition_image()` call deliberately does *not* use
`FLASH_IN_PROJECT`: a plain `idf.py flash` updates the firmware and leaves the
filesystem alone.

## What belongs here later

- `espix-push` — copy a file to a running device over the console or SSH,
  so step 3 above stops involving a full filesystem rewrite.
- A wrapper that builds an app and pushes it in one go.
- Host-side coredump/backtrace decoding for the fault records `dmesg` reports.
