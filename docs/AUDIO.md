# Bluetooth and audio

## Bluetooth

### Radios

| | BLE | Classic | LE Audio |
|---|---|---|---|
| ESP32-S3 | yes (5.0) | **no** | **no** |
| ESP32-S31 | yes | **yes** | **yes** |

So S3 is BLE-only -- no A2DP, no HFP, no LE Audio. S31 has the lot, and IDF's
`a2dp_source` example lists `ESP32 | ESP32-S31` as its targets.

### Host stacks

Chosen per target, like `ESPIX_USB_ROLE`:

- **S3: NimBLE** -- BLE only, small. There is no Classic to host.
- **S31: Bluedroid** -- Classic + BLE, which A2DP and HFP require. Much larger.

`bluetoothctl` therefore means different things per build: BLE central/
peripheral on S3, the full set on S31.

### bluetoothctl

A native command over `esp_bt`, not a BlueZ/D-Bus client -- espix has neither.
The verbs follow BlueZ's:

    power {on|off}
    scan {on|off}
    devices
    info <addr>
    pair <addr>
    trust <addr>
    connect <addr>
    disconnect <addr>
    remove <addr>
    agent {on|off}          # pairing prompts

On S3 it drives BLE scan/connect/GATT/advertise, HID, Mesh and provisioning; on
S31 it adds Classic discovery, pairing, A2DP (source and sink), HFP and AVRCP.

Pairing is `--pin` / auto-accept for now; an interactive agent (type the
passkey at the prompt) comes later.

Legacy `hciconfig` is not provided -- it is dead in BlueZ 5. `hcitool` still
owns raw HCI commands and scan parameters `bluetoothctl` does not expose, so a
small `hcitool`-shaped subset (inq, lescan, cmd) may follow if a real gap
appears; not before.

### Build options

- `ESPIX_BT` (off by default; Bluedroid Classic is the heavy one)
- stack: NimBLE vs Bluedroid (per target)
- `ESPIX_BT_AUDIO` -- A2DP/HFP/AVRCP

## Audio

### Design

A small audio server, not an ALSA/PulseAudio port. Linux's shape is a kernel
layer (PCM devices), a library (`libasound`) and a sound server (PulseAudio,
PipeWire) that mixes, routes and converts. espix takes the parts that matter:

- **Devices (sinks/sources):** A2DP source/sink, I2S codec, internal DAC, mic.
- **Streams:** PCM rate/channels/bits, a ring buffer, volume, pause.
- **A mixer/router:** sum streams, resample, route to a sink -- the PipeWire
  graph, without the graph.
- **App interface:** a native `espix_audio` API and app ABI, plus optionally
  an OSS-style `/dev/dsp` (open, ioctl format, write PCM).
- **Commands:** `play`, `aplay`/`arecord`, `amixer`-ish volume.

### Phase 1 (built now): Bluetooth speaker playback

S31 only. `bluetoothctl` pairs/connects the sink, then

    play <file|url>      # /home/esp/song.mp3, /mnt/song.wav, http(s)://...
    play status
    play stop

**The engine is not GMF.** `play` decodes with `esp_audio_simple_dec`
(`espressif/esp_audio_codec`) directly: one task of ours reads the file, feeds
the decoder, and writes the PCM into the A2DP source's ring buffer; the ring's
backpressure paces playback to the link. A GMF pipeline was tried twice
(`esp_audio_simple_player`, then an owned `gmf_loader` pipeline carrying the
hardware ASRC) and removed both times: on this part the GMF task spends its CPU
in `gmf_core`'s job/IO/event loop rather than in the decoder, and `gmf_loader`
pulls the whole GMF family into the image (3.57 MB against 2.86 MB). The direct
engine is smaller and easier to reason about. GMF stays the phase-2 reference,
not the engine.

**`play` requires a sink.** With none connected it refuses and allocates nothing:
`no audio sink: connect one first (bluetoothctl connect <addr>), or use --wait`.
It used to start the task anyway and fill the ring until A2DP arrived, which held
the decoder and its buffers for the whole wait -- measured at 38 s -- and made
`play` look like it had succeeded while nothing could be heard. `play --wait`
keeps that behaviour for a sink that is expected shortly.

**The stream is joint stereo, 44.1 kHz, bitpool <= 52 by default**, set with
`esp_a2d_source_set_pref_mcc()` on the sink-codec-caps event. `bluetoothctl
quality [0|1|2]` changes it at runtime (0 = mono/<=35, 1 = joint stereo/<=35,
2 = joint stereo/<=52) and restarts the controller, because the dial only applies
to a new codec negotiation. Mono was the first choice, taken from the stock
example; on the bench q2 measured the same CPU as q1 and sounded right on both
the soundcore Q45 and the Audioengine HD3, so it is the default now. A **mono
source** is still sent as two channels -- the ring is stereo by contract, so the
difference channel is empty.

The source role initialises AVRCP **controller** only; the target half is not
espix's, so the sink's attempt to reach it logs
`handle_rc_connect Connect failed with error code: 2` on every connect. Harmless,
and worth knowing before it is mistaken for a fault.

**PCM writes must be accounted, not retried.** `xStreamBufferSend()` takes what
fits and reports how much; while playing, the ring is full, so that is a partial
amount. Treating it as failure and retrying the whole chunk re-sends the bytes
already consumed and desynchronises the stream -- which is exactly the harsh
noise the engine had: clean for the first moment, then garbage. `play`'s feed
advances by the count the ring accepted.

**Resampling: none yet.** The direct engine converts neither rate nor channels,
so the source must match the rate the sink negotiated. SBC sinks pick 44.1 kHz
in practice (the soundcore Q45 and Audioengine HD3 both do), which a CD-rate MP3
already is. A 48 kHz WAV fed into a 44.1 kHz SBC stream is wrong (it sounds like
garbage), and with no resampler there is no way to play it yet. The S31 does have
a **hardware ASRC** (`CONFIG_SOC_ASRC_SUPPORTED`, `esp_asrc`), unused by this
engine; a software rate convert is the cheap alternative. Either is the fix for
non-44.1 kHz sources. Note the alternative to resampling: if the sink advertises
48 kHz, ask for 48 kHz in the preferred codec config and feed it natively.

**Reading the file limits uncompressed audio, and stdio is why.** The build
uses **newlib** (`CONFIG_LIBC_NEWLIB`), whose default stdio buffer is
`BUFSIZ` = **1024** -- and neither espix's VFS nor fatfs supplies `st_blksize`
(`CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0`), so newlib's `__smakebuf_r` never picks a
better one. A 16 kB `fread()` therefore becomes ~16 `read()` calls of 1 kB,
every one crossing the VFS into littlefs or FAT, and the per-call cost
saturates around **~176-200 kB/s** -- exactly what a 44.1 kHz stereo WAV needs
and no more, so it underruns (littlefs measured ~1050 ms of read per second of
audio; the same file on USB FAT32 ~880-960 ms/s, just enough for zero short
reads).

The engine now reads with **`open()`/`read()` directly**, 32 kB into a buffer it
owns in PSRAM -- no FILE buffer, one `read()` per chunk. (`setvbuf` is the
wrong fix here: a large stdio buffer is allocated from the **internal** heap,
which is the scarce one, and it starved the next task creation.) MP3 is
compressed (~16 kB/s of reads), so its file read is never the problem. See the
open items below for the read path proper.

**MP3 decode is fast now, and what fixed it was memory, not arithmetic.** There is
no hardware audio decoder on any ESP32, and on S31 no accelerated MP3 library
either -- the codec is the OpenCore/Helix fixed-point decoder, and its state and
tables are random-access, so they must live in internal RAM. Get that wrong and
it is **~7x slower**: `decode` measured **161 ms per 1 s interval** (~16% of one
core, ring full, `short 0`) against **1200 ms** (spilling, underrunning) with no
code change at all -- only where the buffers and the codec's allocations sat. Two
things fixed it:

- **The lwip/net80211/pp/bluedroid `.bss` moved to PSRAM**
  (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). IDF documents this and names
  exactly those libraries; it frees ~63 kB of internal. Two caveats, recorded
  beside the option in `sdkconfig.defaults.esp32s31`: PSRAM data is **not** in a
  core dump, and there is no runtime fallback if PSRAM init fails.
- **The reaper, ota:check and cmd_task stacks moved to PSRAM**
  (`xTaskCreate...WithCaps`, internal as the fallback). This is what put a
  *contiguous* block back: after Bluetooth, internal had plenty free but the
  largest single block was too small.

The codec's own `in`/`out` buffers stay internal by preference, with a PSRAM
fallback that is **logged** -- a silent 7x slowdown is worse than a loud one. A
reservation that opened the decoder early (before Bluetooth fragmented the heap)
was tried and then removed: once the task stacks moved, it bought nothing.

**The stream is started by `play` and suspended when it ends**, not at connect.
Starting it at connect made Bluedroid encode silence forever: 8% of a core on
`BTC_TASK`, link airtime, and a drain line a second, none of it audible. A phone
behaves the same way (AVDTP START/SUSPEND). START and SUSPEND are sent only on
transitions, so two quick `play`s cannot cross them.

**The sink is pre-rolled.** Playback starts with the ring empty and the sink
pulling, so the first moments underran (34 short calls on a mono tone, audible).
The A2DP callback now hands the sink silence until the ring holds 64 kB, which is
inaudible where a partly-filled frame is not.

**A WithCaps task must be deleted with `vTaskDeleteWithCaps()`.** IDF's own
comment says why: the idle task does not free a stack that `xTaskCreateWithCaps()`
allocated. The audio task and `cmd_task` both self-deleted with plain
`vTaskDelete()`, leaking their stacks on every play and every command that asked
for one. Fixed, and the check is PSRAM free memory **plateauing** across
play/power-off cycles rather than climbing ~6 kB each.
The S3 is not faster silicon, it is better codegen: its codec lib uses the LX7
DSP/MAC instructions (`mul16s`, `addx2/4/8`, `madd.s`).

**A second decoder was tried and cannot handle this file.**
`esphome/micro-mp3` is the same OpenCore decoder built from source with its own
per-file optimization flags, which is the one lever a prebuilt library denies.
It decodes streams that do not use the bit reservoir, but a 64 kbps stereo MP3
always does -- this one references up to 487 bytes of main data from earlier
frames in 198 of its first 200 frames -- and micro-mp3 hands pvmp3 a buffer that
starts at the frame and is bounded to it, so that look-back falls outside the
buffer and pvmp3 returns `NO_ENOUGH_MAIN_DATA_ERROR` (`MP3_DECODE_ERROR`). Both
its direct and buffered paths do this; only parallel paths down to pvmp3 differ.
The prebuilt `esp_audio_codec` feeds pvmp3 the caller's contiguous chunk, so the
reservoir is in range. Worth reporting upstream with the file; until then MP3
stays on the prebuilt decoder. See `play_mp3()` in `components/espix_audio`, kept
for that report.

**IDF version, on S31: use the `release/v6.1` branch, not the `v6.1` tag.** The
tag predates the S31 BR/EDR fixes (wrong TX-power table, ACL performance under
Wi-Fi coexistence, controller-lib LMP bugs). On the tag, A2DP drains at ~0.7x
realtime; on the branch it is realtime (~180 kB/s, ~352 SBC frames/s, zero ring
underruns), and the Audioengine HD3 -- which the tag could not connect at all --
connects. See docs/UPSTREAM.md.

### Known issues

- **The Q45 has a faint right-ear rumble that the HD3 does not.** Sink-specific:
  the same stream is clean on an Audioengine HD3, the Q45 is clean from a phone,
  and a mono stream is by construction identical in both ears. So it is an
  interaction between that unit and this source rather than our pipeline. Not
  chased further; other sinks are the way to test it.
- **littlefs read throughput** (~176 kB/s) -- too slow for raw PCM; fix or route
  around.
- **No resampling**, so non-44.1 kHz sources do not play correctly.
- **A sink's buttons only work while it is streaming.** espix runs AVRCP both
  ways now -- CT to send, TG to receive -- and the Q45's volume buttons do reach
  us and step the level. But it stops sending them when the stream is down: with
  nothing playing, pressing them produces no AVRCP event at all (checked in
  `dmesg`), so there is nothing to receive. That is the sink's behaviour rather
  than ours, and it is why the feature reads as "works during playback".
- **Volume is absolute volume, held on our side.** `bluetoothctl volume [0..127]`
  sets the sink's digital volume and, with no argument, prints the value: what the
  sink last reported if it ever has, otherwise what we last set. There is no PCM
  gain yet, so the sink's own absolute volume *is* the level -- which is why the
  TG's passthrough buttons adjust that. A sink whose knob is analogue (the HD3)
  has no AVRCP volume at all and never reports one, so setting it does nothing
  audible there, which is the honest answer rather than an error.
- **`OLM_LMP: acl lmp unpack failed, err:262! opcode:54` on every HD3 connect**,
  on the fixed branch too. It no longer aborts the open, so the fix made it
  non-fatal rather than making the LMP parser understand it -- see
  docs/UPSTREAM.md and issue #19130.
- **`HCI: unhandled HCI command, opcode:0xfc82`** on every connect/disconnect: an
  Espressif vendor command this controller library rejects as `Illegal Command`.
  Harmless so far, but it is a host/controller mismatch on a preview target.
- **A core dump does not include PSRAM data**, which matters now that the
  Bluetooth and Wi-Fi `.bss` lives there: a fault inside Bluedroid in this
  configuration cannot be reconstructed from its dump.

S3 I2S output is deferred. The S31 coreboard has a mono amp and a speaker header
(no speaker attached yet) and a mic; both are phase 2.

### Phase 2 (roadmap)

- **A PCM gain**, so the volume we hold is ours. The AVRCP **TG** exists now
  (the sink's passthrough buttons reach it, and the Q45's volume keys step the
  level), but with no mixer the step is applied to the sink's absolute volume. A
  gain in the feed would make it ours, and would also be what the sink's
  *play/pause* buttons drive once there is a pause/resume for them to map to --
  today the TG receives them and has nothing to do with them.
- Multiple simultaneous streams and real mixing; per-app streams and names.
- Format conversion, resampling, volume, mute; recording.
- `espix_audio` ABI and `/dev/dsp` so loaded apps play audio like on Linux.
- Network roles: HTTP/Icecast source and server, UPnP/DLNA renderer, Snapcast
  multiroom, AirPlay, an MPD-style music server.
- LE Audio on S31 (headsets, Auracast).
- I2S codec + mono amp + mic on S31; I2S on S3.

### Linux audio apps worth porting (roughly increasing pain)

| app | what it gives |
|---|---|
| `mpg123` | the smallest real MP3 player; proves the app audio ABI |
| `sox` / `play` | a familiar CLI and format toolbox |
| `mpd` + `mpc` | a genuine music server (library, playlists, network) |
| `gmediarender` | a UPnP/DLNA renderer -- "cast to espix" |
| `snapcast` | synchronised multiroom audio |
| `shairport-sync` | AirPlay -- the impressive one, and the most work |
| `darkice` + `icecast` | internet-radio source (and server) |

`ffmpeg`/`ffplay` are out. Order: `mpg123` first, then `mpd`/`mpc` or
`gmediarender` for the server story.

### Limitations

- S3 has no Bluetooth audio at all (no Classic, no LE Audio).
- WiFi and Classic Bluetooth share one radio on S31; streaming a URL to a BT
  speaker is the demanding case, and with the present single-task engine it is
  unproven.
- There is no hardware mixer; software mixing costs CPU and latency.
