# espix

A Unix(-like) kernel/runtime environment for ESP32, built on ESP-IDF.

espix aims to bring the parts of the Linux/Unix operational model that
are actually useful on a microcontroller — a real shell, a real
filesystem, real networking, and the ability to dynamically load and run
native apps — while leaving enough flash/RAM headroom for those apps to
actually do something. It intentionally does **not** try to be a
Linux-compatible kernel; the target is closer to a "nommu Linux"-style
environment purpose-built for ESP-IDF.

## Status

Early, but running on hardware. Verified on an ESP32-S3 (16MB flash,
8MB octal PSRAM) with ESP-IDF v6.1-beta1: LittleFS mounted as the real
`/`, a transport-agnostic shell with ~23 commands, a process table, and
— the point of the exercise — an app cross-compiled on a PC, deployed
as a file, and loaded and executed at runtime by the ELF loader, with
argv and an exit status. Files created on the device survive a reboot
and a firmware reflash.

Fault interception is wired but only *reports*; it does not yet keep the
system up (see [Crash Handling &
Isolation Model](#crash-handling--isolation-model)).

Design notes and the decisions behind the structure are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Goals

- A "real" kernel/supervisor layer on top of FreeRTOS + ESP-IDF, not
  just a shell bolted onto example code.
- A crashing app does not crash the system.
- A real filesystem with mutability and persistence (LittleFS).
- Real networking (WiFi / Ethernet / USB-NCM where supported).
- A shell, available over UART/USB, and over SSH on any IP network.
- SSH/SCP.
- Most common, useful POSIX-ish commands available.
- `top`/`htop`-style live view of tasks/apps and CPU/memory stats.
- **Cross-compile C apps on a PC, dynamically deploy and run them on the
  device** — this is the centerpiece feature. An ELF loader, not just
  statically linked-in firmware.
- More requirements will surface as the design matures.

## Non-goals (for now)

- Full Linux binary/ABI compatibility.
- Memory-protected process isolation on chips without an MMU (see
  [Crash Handling & Isolation Model](#crash-handling--isolation-model)).
- Being a drop-in replacement for Linux — this is a purpose-built
  environment, not an emulation layer.

## Why not just use ESP32OS / esp32-running-linux / Linux-on-esp32-S3?

espix is inspired by prior art in this space, but diverges enough in
architecture that it's being built as an independent project rather
than a fork:

- **[nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)**
  and **[paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)**
  — inspiration for the "why not just run something Linux-like on this
  chip" idea. Neither publishes clear numbers on flash/RAM headroom left
  for real applications after boot, which is the resource budget espix
  is explicitly designed around.
- **[faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)** — a
  similar idea (FreeRTOS + ESP-IDF with a Linux-style shell, `ps`/`top`/
  `free`/`dmesg`, process management commands). Good prior art for shell
  ergonomics, but its "processes" are FreeRTOS tasks managed through a
  shell layer, it uses SPIFFS (LittleFS is only a roadmap item there),
  and it has no ELF loader or dynamic native app loading — which is
  espix's core requirement. Because that requirement implies a
  different architecture (loader, syscall boundary, LittleFS from day
  one) rather than incremental features, espix is being built fresh
  instead of forked. Esp32OS is MIT-licensed with attribution/branding
  requirements (see its `NOTICE.md` / `BRANDING_POLICY.md`); any code
  directly reused from it will retain its license notice and be
  credited here.

## Crash Handling & Isolation Model

espix does **not** provide MMU-based memory isolation between apps on
chips that lack a real MMU (S3, and P4 to a lesser extent — see
[Hardware Targets](#hardware-targets)). This is a deliberate, accepted
tradeoff, conceptually similar to nommu Linux: apps run in a shared
address space and are expected to be trusted, self-compiled code, not a
security sandbox for untrusted binaries.

What espix does today:

- A hook on the panic path (`-Wl,--wrap=esp_panic_handler`, the same
  seam ESP-IDF's own test suite uses) intercepts every fault —
  `LoadProhibited`, `StoreProhibited`, illegal instruction, watchdogs,
  aborts.
- It records the core, exception class, faulting address, task name and
  espix pid into memory that survives the reset, prints one line, and
  then delegates to the normal handler. The next boot reports the
  post-mortem on the console and in `dmesg`:

  ```
  espix: fault: previous boot: fault in task 'main' at 0x4200f226 (StoreProhibited), core 0
  ```

  The `crash` command triggers this on demand. `coredump` inspects the
  full dump ESP-IDF writes alongside it.

What it does **not** do yet — deliberately:

- Reap the faulting task and keep running. The reaper task and its
  queue exist (`espix_fault_request_reap()`), but nothing feeds them.
  Skipping the reboot is the easy half; the hard half is below, and
  shipping the easy half alone would produce a system that limps rather
  than one that recovers.
- Known limitations to design around rather than ignore:
  - It only catches invalid-memory-access faults, not general memory
    corruption (buffer overflows into valid memory, heap corruption,
    one task's wild write landing inside another task's stack or the
    kernel). Those go undetected, same as on nommu Linux.
  - Locks held by a reaped task (heap/malloc lock, VFS/filesystem
    mutex, driver mutexes) need explicit handling — timeout-based
    acquisition and/or per-app heap arenas — or a reaped task can wedge
    the rest of the system instead of just itself.
  - Stack overflow is treated as its own fault class; once it happens
    the task's stack contents can't be trusted, so recovery = reap, not
    "resume."
- Longer-term, the syscall/loader boundary should be designed to allow
  chips with a real MMU (S31) to eventually get actual MPU/MMU-backed
  process isolation as an opt-in, without requiring a rewrite. S3 stays
  in "catch and reap" mode regardless.

## Hardware Targets

| Chip | Notes |
|---|---|
| **S3** | Most common / oldest of the three targets. Typically 16/32MB external flash, 8/16MB PSRAM. Xtensa, no MMU. |
| **P4** | Best for "desktop"-style use — best display output (MIPI DSI, HDMI variant exists), can drive Ethernet (100M) and a screen simultaneously.  Weaker MMU than S31 (not enough for real Linux-style isolation). **No built-in WiFi/BT** — needs a companion chip (typically ESP32-C6) over SDIO/SPI for wireless. |
| **S31** | Latest ESP32. CPU 320mhz lower than the P4, but has higher per-MHz instruction efficiency. Has a real MMU. Gigabit Ethernet. Weaker/limited display output than P4. Cannot use Ethernet and a display at the same time (AFAIK). |

Support priority and per-chip feature availability (isolation model,
display, networking) still to be finalized as the design matures.

## Roadmap

1. ~~Switch filesystem to LittleFS.~~ Done — mounted as the real `/`.
2. ~~Integrate an ELF loader for dynamically loaded native apps.~~ Done
   via `espressif/elf_loader`, which covers all three target chips.
3. ~~Ship a working example of a cross-compiled app built on a PC and
   deployed/run on-device.~~ Done — [apps/hello](apps/hello), running
   on hardware with argv and an exit status.
4. Make a crashing app actually not crash the system (the reaper, held
   locks, per-process resource ownership).
5. Networking, then SSH/SCP — which is what turns the session
   abstraction into more than one session.
6. `top`/`htop`-style live stats, a fuller command surface, pipes and
   job control.

## Building

Requires ESP-IDF v6.1 or newer.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3        # see Hardware Targets
idf.py build
idf.py -p <port> flash monitor
```

`idf.py flash` updates the firmware only. The filesystem is a separate
image, flashed explicitly — a plain firmware flash deliberately leaves
whatever is on the device alone:

```bash
idf.py -p <port> storage-flash   # WARNING: replaces the whole rootfs
```

To build and deploy an app, see [tools/README.md](tools/README.md).

Board assumptions live in
[sdkconfig.defaults.esp32s3](sdkconfig.defaults.esp32s3) (16MB flash,
8MB octal PSRAM) and [partitions.csv](partitions.csv) (4MB app,
11.9MB rootfs).

## License

MIT — see [LICENSE](LICENSE).

Any code directly incorporated from other MIT-licensed projects (e.g.
Esp32OS) retains its original license notice; see individual file
headers / a `NOTICE.md` once added. None is incorporated today —
`espressif/elf_loader` and `joltwallet/littlefs` are fetched at build
time by the IDF component manager rather than vendored into this tree.

## Acknowledgements

- [nodestark/esp32-running-linux](https://github.com/nodestark/esp32-running-linux)
- [paulneja/Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3)
- [faizannazir/Esp32OS](https://github.com/faizannazir/Esp32OS)
- Espressif ESP-IDF / FreeRTOS
