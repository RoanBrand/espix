# espix example apps

Each directory here is a standalone project, not part of the espix firmware. Each
builds to an ELF that the device loads at runtime. Most are ordinary POSIX
programs built with `tools/espix-cc`; `neopixel` is an ESP-IDF project, and the
worked example for building an app that way.

| app | shows |
|---|---|
| [hello](hello/) | the minimum: a C app, argv, an exit status |
| [plasma](plasma/) | the graphics/input ABI: a full-screen app the desktop launches from an icon |
| [doom](doom/) | a large C program from upstream, with sound, input and a video mode of its own |
| [mosquitto](mosquitto/) | a real server as an app: the Eclipse Mosquitto MQTT broker, built from upstream with a small platform seam and no patches |
| [neopixel](neopixel/) | an Arduino sketch and a real Arduino library, as an ESP-IDF project |

The firmware build builds and stages these for you: `idf.py build` runs
`tools/build-apps.sh`, which drops each one into `fsroot/bin/` so
`make flash-fs` carries it. Skip it with `idf.py -DESPIX_BUILD_APPS=OFF build`.
The staged binaries are build artifacts and are not committed; the sources here
are.

## POSIX apps

A POSIX app is C against standard headers, with nothing espix-specific in its
build. `tools/espix-cc` is its compiler, as `musl-gcc` is for musl, and
`cmake/espix-uapp.cmake` makes it a CMake project's:

```bash
cmake -S apps/hello -B apps/hello/build -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake -DESPIX_TARGET=esp32s3
cmake --build apps/hello/build
tools/espix-cc --strip apps/hello/build/hello hello.staged
scp hello.staged esp@<host>:/bin/hello
```

The same source builds for both chips: a U-mode executable on the S31, a shared
object the kernel relocates on the S3. Either way it carries libc's state
(getopt, the environment, stdio, atexit) and shares libc's code, the one
`/lib/libc.so` every app uses. `-static` links all of libc into the app instead.
docs/USER-MODE.md has the details.

espix's own calls (graphics, audio, `espix_proc.h`) are headers in
`components/*/include`; `apps/plasma/CMakeLists.txt` shows the include lines.

## IDF-project apps

An app that needs ESP-IDF components, such as Arduino's core and its libraries,
can be an IDF project instead. `neopixel` is one, and the example to copy. Its
libc is its own, as a POSIX app's is: `cmake/espix-app.cmake` links libespix and
the app's part of libc, so only IDF's drivers, FreeRTOS and the C++ runtime come
from the firmware. It runs on the S3 only (`neopixel/targets`).

`neopixel` is a normal `setup()`/`loop()` sketch with one addition: a
`teardown()`, called when someone stops the app. An Arduino sketch never needs
one, because a board runs until the power goes; an espix app is a process, and
what it switched on is its own to switch off.

The `main()` that calls all three lives in `neopixel/main/espix_sketch.{h,cpp}`
— the app's own shim, not something espix provides. Copy those two files and
`ctors.ld` beside a sketch of your own to get the same shape.

**`delay()` is a cancellation point.** The shim wraps it, so a stop request ends
the app inside whichever `delay()` is running: `teardown()` runs and that
`delay()` never returns. Worth knowing, because it is the one place the sketch
does not show you what is happening — a `delay(5000)` interrupted after 127ms
looks like nothing until you know. A `loop()` that never delays is still
stopped, checked between iterations.

The sketch departs from stock Arduino in three places, each commented where it
appears: the NeoPixel object is a pointer built in `setup()` (see the note on
global constructors below), output is `printf` rather than `Serial` — espix
gives each app a stdout belonging to whoever ran it, where `Serial` would write
to the physical UART and be invisible over SSH — and delays are multiples of
10ms, because espix runs a 100Hz tick and Arduino's `delay()` truncates.

To build one by hand:

```bash
cd apps/neopixel
idf.py -G 'Unix Makefiles' set-target esp32s3   # once; enables `idf.py elf`
idf.py elf
scp build/neopixel.app.elf esp@<host>:/bin/neopixel
```

### What an IDF-project app may call

Its own code, the IDF and Arduino components it links, its libc (libespix,
libc's per-app part, and `/lib/libc.so`), and what the firmware publishes:
`espix_proc/abi_drivers.c` for peripherals, FreeRTOS and IDF's heap and logging,
`abi_cxx.cpp` for the C++ runtime, `abi_alloc.c`, `abi_pthread.c` and
`abi_signal.c` for memory, threads and signals, and `abi_gfx.c` and `abi_audio.c`
for the screen and sound. The firmware publishes no Arduino API: a sketch gets
Arduino by linking the Arduino component, which is why `apps/neopixel` carries
that dependency and the firmware does not.

IDF's socket headers make `socket()` and the rest inline calls to lwIP's
`lwip_socket()` and friends. libespix answers those names too, so an IDF
project's sockets are its process's own, like its files.

Anything else fails the load with `Can't find symbol X`, on the line above
espix's own `undefined symbol`, and in `dmesg`. To see what an app needs:

```bash
xtensa-esp32s3-elf-readelf -sW build/app.app.elf | awk '$7=="UND"{print $8}' | sort -u
```

`tools/check-abi.py` reads every table on every build and fails it on an entry
that cannot be reached.

**Why the tables are hand-written.** With no MMU they are the sandbox: an app
shares the address space and is bounded by what it can name. elf_loader's
`tool/symbols.py` would generate a table from the whole firmware and publish the
WiFi driver, the flash API and espix's internals to every app, so espix leaves it
off. Adding a name is nearly free, but it should be something that cannot reach
past the caller's own memory.

### What its CMakeLists has to say

One line, after `project()`:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/espix-app.cmake")
```

It links the app's libc and sets its entry point, and makes the app agree with
the kernel about `off_t`, and so about the layout of `struct stat`.
`tools/check-abi.py` fails the firmware build when a project under `apps/` does
not include it. It puts the app's libc in `ELF_LIBS`, so an app adds its own link
options to that variable (`string(APPEND ELF_LIBS " ...")`) rather than setting
it.

### Three things that will catch you

**C++ entry points need `extern "C"`.** `project_elf()` compiles with
`-Dmain=app_main`, and libespix's entry calls `app_main` as `main`. Without
`extern "C"` the name is mangled and the link fails for want of `app_main`.

**Global constructors need a linker script.** They work, but not for free. The
loader resolves an address only inside five sections it knows by name —
`.text`, `.data`, `.rodata`, `.data.rel.ro`, `.bss` — and a constructor pointer
normally lands in `.ctors`, which is not one of them. `esp_elf_map_sym()` returns
0 for it and the loader dereferences that, so the app dies during relocation,
before its first instruction.

`neopixel/ctors.ld` folds `.ctors` into `.data` and names its bounds, and the
shim walks them before `setup()`. Copy that file and the `set(ELF_LIBS ...)`
line from `neopixel/CMakeLists.txt` to get the same in another app. An app also
needs to define `__dso_handle` (see `espix_sketch.cpp`): a global with a
*destructor* registers it through `__cxa_atexit`, and `-nostdlib` never links
the `crtbegin.o` that normally supplies that symbol — without it the link fails
with the memorably unhelpful `final link failed: bad value`.

**Static link order matters.** `ELF_COMPONENTS` becomes one link line of plain
archives, resolved left to right. A library must be listed *before* the
component it draws symbols from, or those symbols are silently left undefined —
`-nostdlib -shared` produces an ELF regardless, and the failure only appears at
load.

## Stopping cleanly

`kill` asks before it deletes. A POSIX app gets `SIGTERM` and may handle it as
on Linux. An IDF-project app polls `espix_app_stopping()` to put its hardware
back — see `neopixel`, which turns the LED off rather than leaving it lit:

```c
extern "C" bool espix_app_stopping(void);

while (!espix_app_stopping()) { /* ... */ }
/* tidy up here */
```

An app that ignores it is deleted a few hundred milliseconds later.

## Graphics and input

`espix_gfx.h` is the app-facing graphics ABI: the whole screen, an optional
render surface, and a queue of input events. An app claims the screen with
`espix_gfx_open()`, draws into the framebuffer from
`espix_gfx_lock()` -- or into a surface and `espix_gfx_present_surface()`,
which the PPA scales -- presents, and drains `espix_gfx_poll_event()` once a
frame. `espix_gfx_close()` gives the screen back. `apps/plasma` is the worked
example.

`espix_gfx_open_mode(w, h)` is the same claim with the app's video mode: the
canvas it wants to itself while it owns the screen. `espix_gfx_open()` is
`espix_gfx_open_mode(0, 0)` -- "leave it as it is" -- which is what an app that
draws into a small surface and lets the display scale it up wants, and plasma
passes. Doom passes `320, 240`: its own height rather than the 320x200 it
renders, because that is the size that puts Doom's non-square pixels back at
4:3. The desktop's resolution is restored when the app closes, and the mode is
never written to `/etc/display.conf`.

The canvas is RGB565 and shared with the desktop and the VNC encoder, so the
lock is real: draw inside it and present promptly. A process killed while
holding it is the one case it cannot survive -- the holder's TCB is gone, and
handing a FreeRTOS mutex back walks it -- so the process layer **orphans** the
lock instead. The canvas loses mutual exclusion for the rest of the boot, which
is the price of not taking a watchdog reset.

**The desktop launches apps from an icon.** Double-clicking a desktop icon runs
the program full-screen: it claims the screen, and the desktop reclaims it when
the app exits or is killed. Adding a program is one row in the `s_icons` table
in `components/espix_desktop/desktop.c` plus its ELF in `/bin`; the launcher and
its waiter live in the same file. The desktop also resets its pointer state on
reclaim, because the click that started the app sent its *release* to the app --
without that, the first click after every app would be swallowed.
