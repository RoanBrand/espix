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

The same source builds for both chips. On the S31 it is a static U-mode
executable. On the S3 it is a shared object the kernel relocates: it carries
libc's state (getopt, the environment, stdio, atexit) and binds libc's code to
the one `/lib/libc.so` every app shares. `-static` links all of libc into the
app instead. docs/USER-MODE.md has the details.

espix's own calls (graphics, audio, `espix_proc.h`) are headers in
`components/*/include`; `apps/plasma/CMakeLists.txt` shows the include lines.

## IDF-project apps

An app that needs ESP-IDF components, such as Arduino's core and its libraries,
can be an IDF project instead. `neopixel` is one, and the example to copy. Its
libc and everything else come from the tables the firmware publishes, described
below, so it shares libc state with the kernel. It runs on the S3 only
(`neopixel/targets`).

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

An IDF-project app resolves its undefined symbols at load time against tables
the firmware publishes: `espix_proc/abi_libc.c` and `abi_libm.c` for libc,
`espix_net/abi.c` for sockets and name resolution, `espix_proc/abi_cxx.cpp` for
the C++ runtime, `espix_proc/abi_drivers.c` for peripherals and FreeRTOS.

espix publishes **only** libc, libm, FreeRTOS, ESP-IDF and its own calls. It does
not publish an Arduino API: a sketch gets Arduino by linking the Arduino
component into the app, which is why `apps/neopixel` carries that dependency and
the firmware does not.

libm is published by `espix_proc/abi_libm.c` — `sinf`, `sqrtf`, `atan2f`,
`powf` and the rest — because an app cannot carry its own. An app is linked
`-fPIC -shared` and the toolchain's `libm.a` is not PIC, so `ld` refuses it
("relocation R_RISCV_HI20 ... can not be used when making a shared object").
The firmware already links `-lm`; naming the calls is what pulls them into the
image. The S31 has a hardware FPU, so `sqrtf`/`fabsf`/`floorf` are single
instructions and this is not the compromise it would be on a chip without one.

Anything not in those tables fails the load with `Can't find symbol X` — the
loader names it, on the line directly above espix's own `relocation failed`, and
it is in `dmesg` too. Read both lines before going looking; the answer is
usually already on the screen.

To see what an app needs, before running it:

```bash
xtensa-esp32s3-elf-readelf -sW build/app.app.elf | awk '$7=="UND"{print $8}' | sort -u
```

And to see what is actually published, which is the other half of the question:

```bash
grep -ho 'ESP_ELFSYM_EXPORT([a-zA-Z_0-9]*)' components/espix_proc/abi_*.c \
    | sed 's/.*(\(.*\))/\1/' | sort -u
```

That grep finds one of the three mechanisms espix publishes through. The
resolver (`ABI_SYM(...)`) and the tables written out by hand are not
`ESP_ELFSYM_EXPORT` -- so it misses them, and they are where an entry that
cannot be reached hides. `tools/check-abi.py` reads all three and runs on every
build; it is the answer to "is this name actually reachable".

### Why the list is hand-written

elf_loader ships `tool/symbols.py`, which generates a complete table from a
firmware ELF into `g_customer_elfsyms`. espix leaves it off on purpose.

With no MMU, this table *is* the sandbox: an app shares the address space and is
bounded only by what it can name. Generating it from the firmware would publish
the WiFi driver, the flash API and espix's internals to every app. So the tables
are an allowlist — `abi_fs.c`, `abi_time.c`, `abi_drivers.c` and `abi_libc.c`
are it being curated. Adding a name is nearly free (a string and a pointer in
the firmware, nothing in any app), but it should be something that cannot reach
past the caller's own memory.

### What its CMakeLists has to say

One line connects an app to the kernel, and it is the app saying *what it is*
rather than knowing how espix is built:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/espix-app.cmake")
```

It is where the two sides agree about the ABI they share — today that means
`off_t`, and therefore the layout of `struct stat`, which crosses the loader's
boundary in both directions. An app that leaves it out compiles cleanly, with a
32-bit `off_t`, and then has `struct stat` written past the end of its buffer.
Nothing on either side can see that at runtime, so `tools/check-abi.py` fails
the firmware build when a project under `apps/` or `tests/` does not include
it. An app author should not have to know any of this; the check exists so they
do not have to.

### Three things that will catch you

**C++ entry points need `extern "C"`.** `project_elf()` links with `-e app_main`
and compiles with `-Dmain=app_main`. Without `extern "C"`, the entry is
name-mangled, the linker says *"cannot find entry symbol app_main"*, and you get
an ELF with no sections.

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
