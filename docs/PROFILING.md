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

Task switches, ISR entry and CPU load on a timeline, which no sampling profiler
gives. In IDF v6.1 this lives in the **`esp_trace`** component: it coordinates
encoders (the `espressif/esp_sysview` component provides the SystemView encoder)
and transports (apptrace over JTAG, or UART for real-time viewing). The resulting
trace opens in SEGGER's SystemView application. `app_trace` still carries
`APPTRACE_DEST_JTAG` for the transport end.

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
- **"Show me the schedule"** -> SystemView, and only then, because it is not
  free: it needs the `espressif/esp_sysview` component fetched from the
  registry, a `CONFIG_ESP_TRACE_*` selection, a rebuild and a reflash. Once
  in, it streams over the USB-JTAG port without halting, which is the only way
  to watch the VNC or audio path while it runs.
- `heap_trace` behind a config option, for allocation questions.
