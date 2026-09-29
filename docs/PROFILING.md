# Profiling and tracing

What exists for ESP32-S31, what works today, and what needs the USB-JTAG port
plugged in. This exists because the alternative -- adding timing instrumentation,
rebuilding, flashing and testing by hand -- cost far too much during the audio
work, and could not answer "which task is actually running" at all.

## Works today, no JTAG

### Per-task CPU and stack: `top` and `ps`

FreeRTOS runtime stats are already on (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`,
`CONFIG_FREERTOS_USE_TRACE_FACILITY=y`), and espix already reads them:
`uxTaskGetSystemState()` in `components/espix_cmds/cmd_sys.c`, where `top` samples
`ulRunTimeCounter` twice and reports the difference per task, and `ps` reports the
one-shot share and stack high-water. This is the first thing to reach for, and it
is how the audio task was shown burning a core flat while making little progress.

    top                     # per-task CPU%, sampled
    ps                      # one-shot share, stack, priority, core

### Heap sampling: `heap_trace` (to add)

`CONFIG_HEAP_TRACING_STANDALONE` records every allocation and free into an in-RAM
buffer; a `heap_trace` command would start it, stop it and dump the records,
answering "who allocates, and does it come back". It needs a trace buffer, so it
belongs behind a debug config option, not in the default build.

### Cycle counting

For a focused question, `esp_cpu_get_cycle_count()` around a region is exact and
cheap; the audio engine already carries a version of this (the read/decode/feed
milliseconds). It is the fallback when the question is narrower than a profiler.

## Ports on this board

Two USB ports, and which one is which matters:

| port | device | what it carries |
|---|---|---|
| USB-UART (external chip) | \`/dev/cu.usbserial-*\` | **the console**, input and output -- UART0 |
| USB-DBG (USB-Serial/JTAG) | \`/dev/cu.usbmodem*\` | **JTAG** for OpenOCD/GDB, plus a **mirrored console output** |

The console is configured with a primary and a secondary:

    CONFIG_ESP_CONSOLE_UART_DEFAULT=y                    primary = UART0
    CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y       secondary = output mirror

The secondary is **output-only** -- the USB-DBG port prints the boot log but
ignores typed input. That is why it looks like a console and is not one. It is
IDF's default because many S3/S31 boards expose only the USB-Serial/JTAG, so the
console appears whichever cable is plugged in; on a board with a real UART it is
redundant and can be dropped with \`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y\`.

Neither port's presence changes the halt problem below: a halted CPU prints on
nothing. The two are separate USB devices, so OpenOCD and the console do not
contend at the USB level.

## Needs the USB-JTAG port

The S31 has built-in USB-JTAG, and the newer OpenOCD in the tool tree has S31
support -- `board/esp32s31-builtin.cfg` and `target/esp32s31.cfg` under
`openocd-esp32/v0.12.0-esp32-20260703`. **Watch the version**: the IDF-pinned
`20260424` in the same directory has no S31 target and `idf.py openocd` will
choose it, so point `OPENOCD` at the 20260703 binary. With no port connected
(`/dev/cu.usbmodem*` absent) OpenOCD finds the target but cannot identify it:

    Error: [esp32s31.hp.cpu0] Unsupported DTM version: -1
    Error: [esp32s31.hp.cpu0] Could not identify target type.

### 1. OpenOCD's `profile` command: not in this build

There is no gprof producer. The ESP32 OpenOCD fork dropped the upstream
`profile` command, so this fails outright:

    openocd -f board/esp32s31-builtin.cfg -c 'profile 10 gmon.out' -c shutdown
    Error: invalid command name "profile"

The toolchain still ships `riscv32-esp-elf-gprof`, but nothing writes the
`gmon.out` it would read. Sampling has to come through GDB instead, which is
the next section -- and it is the better tool anyway, because it recovers call
stacks rather than a bare program counter.

### 2. Stack sampling into a flamegraph

Better, because it yields call stacks rather than a single PC. GDB halts, dumps
every task's backtrace, resumes, and repeats; the folded stacks aggregate into a
flamegraph, the same shape as sampling profilers on Linux. The unwind comes from
DWARF, so `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER` is **not** needed -- which
matters here, since frame pointers cost ~152 kB of code and do not fit in the
kernel slot.

This is `tools/jtag-profile.py`. A live capture (25 samples) confirms the
unwind: every blocked task comes back as a full symbolised stack down to
`vPortTaskWrapper` -- for example `sshd` ends
`xQueueReceive -> sys_arch_mbox_fetch -> netconn_accept -> lwip_accept ->
accept_task -> vPortTaskWrapper`. That is a real call graph of the whole
system with nothing added to the build.

Two things about it, both measured:

- **The blocked-task leaf is an artifact.** A halted blocked task saves a PC in
  `vPortClearInterruptMaskFromISR`, so every blocked task shows that frame; it
  says nothing about what the task is doing. The sampler keeps the running
  task(s) only, rather than let the artifact swamp the histogram.
- **Halting starves anything socket- or radio-driven.** The network stack runs
  on the halted core, so a client cannot send the request that would make the
  server work. Sampling the VNC path while a viewer drags is therefore
  self-defeating in the same way the BT audio case below is: the sample stops
  the thing being measured. It catches `update_send` when a request is already
  queued, but the busy fraction it reports is an underestimate. A target left
  halted is worse than useless -- it answers no SSH, no ping and no VNC until
  something resumes it (`monitor resume`).

### 3. SystemView: the timeline

Task switches, ISR entry and CPU load on a timeline, streaming without halting --
so it can watch the VNC or audio path *while it runs*. It is a separate build:

    make PROFILE=sysview build
    make PROFILE=sysview flash-ota

`profiles/sysview.conf` becomes `sdkconfig.<target>-sysview` in
`build-<target>-sysview`; a release is the normal build and links none of it
(the release image is byte-identical with the component declared). The encoder is
`espressif/esp_sysview`; `main/espix_main.c` overrides
`esp_trace_get_user_params()` to name it "sysview" -- Kconfig's external-library
default is the generic "ext" -- and to select the CPU. Without that override the
image aborts at boot in `ipc0` with `init function ... has failed (0x105)`.

**Transport: USB-Serial/JTAG, one CPU.** `esp_trace` has two transports and on
S31 only one works:

| | apptrace over JTAG (`esp sysview` / `_mcore`) | USB-Serial/JTAG |
|---|---|---|
| host | OpenOCD + GDB | SEGGER SystemView directly |
| cores | multi-core, one file | one CPU, filtered |
| S31 | **broken** | works |

OpenOCD 20260703 and 20260831 both fail before touching the firmware --
`Failed to get max trace block size!` / `Failed to init cmd ctx (-4)!` --
because the apptrace control block comes from the target's semihosting parameter
(`esp_riscv.c:842`) and never resolves for S31. No `CONFIG_` reaches it and no
upstream issue covers it; `tools/sysview.py` automates the JTAG route for when
that changes. Until then the profile traces **one CPU**:
`CONFIG_ESPIX_TRACE_CORE` is 0 or 1, and since espix pins `espix:vnc` to core 1,
the display path is 1.

**Recording.** In SystemView: Target -> Start Recording, Target Interface
**UART**, COM port `/dev/cu.usbmodem101` (the USB-DBG port -- not
`/dev/cu.usbserial-*`, which is the shell console), any baud: the CDC ignores
it. Use **SEGGER's stock** `SYSVIEW_FreeRTOS.txt`; the component emits SEGGER's
event IDs, while the IDF file under `tools/esp_app_trace/` is the legacy
apptrace mapping and mis-names every event. `Export Data -> CSV` is what the
analyzer reads; saving the `.SVDat` is not needed.

**Reading it.** `tools/sysview-csv.py <export.csv>` reduces the RFB markers to a
per-update table -- canvas, encode CPU (encode minus wire), wire, the copy vs
pixels path, and bytes and flushes per packet. The markers come from `rfb.c` in
this build only: 0 canvas, 1 encode, 2 wire, 3 copy, 4 pixels.

**Worked example: the drag.** The tool first reported that 69% of drag updates
sent no copy, which looked like the bug. It was not: cross-tabbing `moves`
against bytes showed the *copy* updates were the expensive ones, ~190 KB each,
because CopyRect moved only the overlap of the old and new window and the newly
exposed side went as pixels. Copying the whole window clipped to the canvas
instead took the drag from **9.4 MB to 2.05 MB**, the wire from **67% to 34%** of
update time, and removed the encode tail. Two changes: `espix_canvas_moved()`
now merges consecutive motions of one rectangle (the note that says what moved),
and `espix_window_move()` copies the clipped window rather than just its
overlap. See docs/DISPLAY.md.

**Volume.** A busy trace is ~3.5k events/s, most of it the SEGGER port's own
`xTaskGetTickCountFromISR` and `vTaskSetApplicationTaskTag` bookkeeping. The USJ
ring is raised to 32 KB in `profiles/sysview.conf`; the 2 KB default overflows
on the first busy second.

### What halting cannot sample: live Bluetooth audio

The GDB stack sampler **halts the cores** to read them. OpenOCD's `init` halts
them too, which is why simply leaving OpenOCD running freezes the console. For
a live A2DP stream that is fatal: each sample stops the CPU for the round trip,
the link starves, and the sink drops.
So these tools cannot profile the audio path *while it is playing* -- the
measurement prevents the thing being measured.

The honest split:

- **Realtime / BT audio timing** -> **SystemView** (section 3). It streams
events without halting, so the target keeps running. That is the tool for "is
the audio task getting scheduled", not the sampler.
- **Anything not tied to the link** -- boot, filesystem, a decode loop, a
  benchmark command -- the halt sampler is ideal and instrumentation-free.
- **Narrow questions** -> the timing already in the build (read/decode/feed ms)
or `esp_cpu_get_cycle_count()` around one region.

### 4. Core dump and live GDB

`tools/coredump.sh` already decodes panic dumps. With JTAG the same GDB attaches
to a live target instead, so a fault can be inspected where it happened rather
than reconstructed from a written image.

## What to reach for

- **"What is eating the CPU"** -> `top`. Built, no JTAG, no reflash.
- **"What is the display path costing"** -> the counters already in the build.
  `log vnc info` turns on `send: 100 updates, mean N us (canvas, encode,
  wire), worst M, mean R rects`; `log display info` turns on the input-queue
  line. This is permanent instrumentation, not something rebuilt per question,
  and it survives halting because nothing is sampled.
- **"Show me the call graph"** -> `tools/jtag-profile.py`. Verified working.
  It halts, so it suits boot, filesystems, decode loops and anything not driven
  by the network or the radio.
- **"Show me the schedule"** -> `make PROFILE=sysview flash-ota`, then SystemView
  over USB-Serial/JTAG (section 3). Not free -- a separate build and reflash --
  but it is the only live, non-halting view, and the JTAG route that would give
  both cores at once is broken on S31.
- `heap_trace` behind a config option, for allocation questions.
