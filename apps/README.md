# espix example apps

Each directory here is a **standalone ESP-IDF project**, not part of the espix
firmware. Each builds to a relocatable ELF that the device loads at runtime.

| app | shows |
|---|---|
| [hello](hello/) | the minimum: a C app, argv, an exit status |
| [neopixel](neopixel/) | an Arduino sketch and a real Arduino library, cross-compiled for espix |
| [plasma](plasma/) | the graphics/input ABI: a full-screen app the desktop launches from an icon |

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

The firmware build builds and stages these for you — `idf.py build` runs
`tools/build-apps.sh`, which drops each ELF into `fsroot/bin/` so `make flash-fs`
carries them. Skip it with `idf.py -DESPIX_BUILD_APPS=OFF build`.

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
# bash, not zsh: zsh does not word-split an unquoted variable, so a file list
# held in one gets searched as a single impossible filename and every symbol
# looks missing.
{ grep -o 'ESP_ELFSYM_EXPORT([a-zA-Z_0-9]*)'       managed_components/espressif__elf_loader/src/esp_elf_symbol.c
  grep -ho 'ESP_ELFSYM_EXPORT([a-zA-Z_0-9]*)' components/espix_proc/abi_*.c
} | sed 's/.*(\(.*\))//' | sort -u
```

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

## Three things that will catch you

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

## Graphics and input

`espix_gfx.h` is the app-facing graphics ABI: the whole screen, an optional
render surface, and a queue of input events. An app claims the screen with
`espix_gfx_open()`, draws into the framebuffer from `espix_gfx_lock()` -- or into
a surface and `espix_gfx_present_surface()`, which the PPA scales -- presents,
and drains `espix_gfx_poll_event()` once a frame. `espix_gfx_close()` gives the
screen back. `apps/plasma` is the worked example.

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
