# espix example apps

Each directory here is a **standalone ESP-IDF project**, not part of the espix
firmware. Each builds to a relocatable ELF that the device loads at runtime.

| app | shows |
|---|---|
| [hello](hello/) | the minimum: a C app, argv, an exit status |
| [neopixel](neopixel/) | an Arduino sketch and a real Arduino library, cross-compiled for espix |

`neopixel` is a normal `setup()`/`loop()` sketch. The `main()` that calls them
lives in `neopixel/main/espix_sketch.{h,cpp}` — the app's own shim, not
something espix provides, so copying those two files beside a sketch of your own
is all it takes to get the same shape. It also keeps the `extern "C"` off the
sketch, and turns espix's cooperative stop into `espixStopping()` and
`espixExit()`.

The sketch departs from stock Arduino in three places, each commented where it
appears: the NeoPixel object is a pointer built in `setup()` (see the note on
global constructors below), output is `printf` rather than `Serial` — espix
gives each app a stdout belonging to whoever ran it, where `Serial` would write
to the physical UART and be invisible over SSH — and delays are multiples of
10ms, because espix runs a 100Hz tick and Arduino's `delay()` truncates.

The firmware build builds and stages these for you — `idf.py build` runs
`tools/build-apps.sh`, which drops each ELF into `fsroot/bin/` so
`idf.py storage-flash` carries them. Skip it with
`idf.py -DESPIX_BUILD_APPS=OFF build`.

To build one by hand, or to iterate without reflashing the whole rootfs:

```bash
cd apps/hello
idf.py -G 'Unix Makefiles' set-target esp32s3   # once; enables `idf.py elf`
idf.py elf
scp build/hello.app.elf esp@<host>:/bin/hello
```

The staged binaries are build artifacts and are not committed; the sources here
are.

## What an app may call

An app resolves its undefined symbols at load time against tables the firmware
publishes: the ELF loader's own libc/IDF tables, plus espix's
(`espix_net/abi.c` for sockets and name resolution, `espix_proc/abi_cxx.cpp` for
the C++ runtime, `espix_proc/abi_drivers.c` for peripherals and FreeRTOS).

espix publishes **only** libc, FreeRTOS, ESP-IDF and its own calls. It does not
publish an Arduino API: a sketch gets Arduino by linking the Arduino component
into the app, which is why `apps/neopixel` carries that dependency and the
firmware does not.

Anything not in those tables fails the load with `Can't find symbol X`. To see
what an app needs:

```bash
xtensa-esp32s3-elf-readelf -sW build/app.app.elf | awk '$7=="UND"{print $8}' | sort -u
```

## Three things that will catch you

**C++ entry points need `extern "C"`.** `project_elf()` links with `-e app_main`
and compiles with `-Dmain=app_main`. Without `extern "C"`, the entry is
name-mangled, the linker says *"cannot find entry symbol app_main"*, and you get
an ELF with no sections.

**No global constructors.** The loader does not run `.ctors`, and worse, it
crashes trying: a relocation against `.ctors` makes `esp_elf_map_sym()` return 0
for a section it does not track, and the loader dereferences that without
checking. A global C++ object therefore takes the process down during
relocation, before your first instruction. Construct inside `main()`.

**Static link order matters.** `ELF_COMPONENTS` becomes one link line of plain
archives, resolved left to right. A library must be listed *before* the
component it draws symbols from, or those symbols are silently left undefined —
`-nostdlib -shared` produces an ELF regardless, and the failure only appears at
load.

## Stopping cleanly

`kill` asks before it deletes. An app that polls `espix_app_stopping()` gets to
put its hardware back — see `neopixel`, which turns the LED off rather than
leaving it lit:

```c
extern "C" bool espix_app_stopping(void);

while (!espix_app_stopping()) { /* ... */ }
/* tidy up here */
```

An app that ignores it is deleted a few hundred milliseconds later, exactly as
before.
