# Bluetooth and audio

## Bluetooth

### Radios

| | BLE | Classic | LE Audio |
|---|---|---|---|
| ESP32-S3 | yes (5.0) | **no** | **no** |
| ESP32-S31 | yes | **yes** | **yes** |

The S3 is BLE-only, so it has no A2DP and no LE Audio; its audio path is I2S.
The S31 has Classic and LE Audio.

### Host stacks

Per target: **S3 takes NimBLE** (BLE only, small), **S31 takes Bluedroid**
(Classic + BLE, which A2DP requires, and the host that can also do LE Audio).

### bluetoothctl

A native command over `esp_bt`, not a BlueZ/D-Bus client:

    power {on|off}
    scan {on|off}
    devices
    info <addr>
    pair <addr>
    trust <addr>
    connect <addr>
    disconnect <addr>
    remove <addr>
    agent {on|off}
    quality {0|1|2}
    volume [0..127]

### Build options

- `ESPIX_BT` (off by default)
- Classic audio lives in `espix_bt` today; HFP and LE Audio are later

## Audio

### Design

A small audio server, not an ALSA/PipeWire port. The parts that matter:

- **Sinks and sources are registered**, not compiled in. The engine knows only
  `espix_audio_sink_ops_t` (`components/espix_audio/include/espix_audio_sink.h`):
  a name, a format, and write/start/suspend. The A2DP source and the S31's
  ES8311 (`espix_i2s`) are the sinks today; the RF transmitters are next. The
  dependency points provider -> engine, so **audio does not depend on
  Bluetooth**: a target with no sink still builds and links, and `play` says
  none is available. `play` uses the first *connected* sink, so the board plays
  to Bluetooth when a link is up and to its own speaker when it is not, with no
  selector.
- **Streams** are how a producer reaches a sink: open, write, close. The engine
  mixes up to `STREAM_MAX` of them into the one sink the system is using, with
  per-stream volume and mute, and the mixer task exists only while at least one
  is open. A stream states the format it *has*; reconciling that with the sink
  is the engine's job (see *Rate handling* below).
- **Sources** are the other direction and mirror the sinks exactly: a registry
  of `espix_audio_source_ops_t` (name, format, read, start, stop), read through
  `espix_audio_capture_open()`/`read()`/`close()`. There is deliberately no task
  and no ring behind capture: the caller's thread reads and the source paces it,
  `read()` returning zero when the producer has nothing, which is what ALSA's
  capture `read()` does. One client at a time, because with no fan-out two
  readers cannot be given the same samples. The ES8311's ADC is the source
  registered today.
- **Pipes** connect a source to a sink and leave it connected -- microphone to
  headset, radio to speaker. Internally it is a capture client on one side and a
  stream on the other, so nothing new is involved; what is new is being able to
  ask the system to hold one open.
- **App interface:** the playback stream and the capture calls are published to
  apps through the ABI (`components/espix_proc/abi_audio.c`), so an app plays
  and records without knowing what a sink is. `/dev/dsp` is still open.

### Phase 1 (built now): playback and capture

S31 only. With a connected A2DP sink, or the board's own ES8311 when nothing is
linked:

    play <file|url>
    play --wait <file|url>
    play {stop|status}

`play` decodes with `esp_audio_simple_dec` and writes PCM into the sink's
ring; the ring's backpressure paces playback to the link. There is **no GMF on
the data path** -- the GMF player and an owned pipeline were tried twice and
lost to `gmf_core`'s task/IO/event overhead, and they pulled the whole GMF
family into the image.

**The decoder set is chosen in `make menu`** (`espix audio -> Codecs`), which
selects the `esp_audio_codec` symbols. The default is **MP3 + the WAV
container**; AAC, FLAC, Opus, Vorbis, SBC and LC3 are opt-in, and nothing else
links -- the component defaults every decoder to y, so all of them are pinned
off in `sdkconfig.defaults` and only the menu's choices turn back on.

**Memory is the whole story.** The MP3 decoder's state is random-access, so it
must be internal RAM; the chunk buffers around it (`in`, `out`, and the mono
upmix scratch) are streamed, so they live in PSRAM. That split is what makes
playback realtime: with the chunk buffers internal, an A2DP link left the
heap's largest block at ~3 kB, the codec spilled to PSRAM, and playback ran at
~0.6x realtime; with them in PSRAM the decoder is ~11% of a core and the ring
stays full. See `ESPIX_AUDIO_IO_PSRAM` and `ESPIX_AUDIO_CODEC_PSRAM`.

**Rate handling is negotiation first, conversion second.** A stream states the
format it has and the engine answers in the cheapest way that works:

1. **forward** -- the sink already runs at that rate, so nothing happens;
2. **adopt** -- the sink can be asked to run at it instead, which is free. The
   ES8311 and its I2S clock are ours to set, so a 16 kHz stereo file plays at
   16 kHz rather than being resampled. Adopting is only safe while nothing else
   is playing, because the mixer sums every stream at the sink's one rate;
3. **convert** -- otherwise the stream converts at its **own edge**, with
   `esp_asrc`, so the ring still holds the sink's format and the mixer is
   untouched.

The ASRC is not a general resampler: it converts between 8, 16, 32, 44.1 and
48 kHz and nothing else, and the pair is checked before a conversion is
promised. Doom's 11 kHz DMX effects can never go through it -- which is fine,
because the app producing them upsamples them itself. It is one peripheral, so
its process call is serialized across the streams using it.

**`play` streams what it decoded.** It used to resample to the sink's rate and
open an already-converted stream; it now opens the stream at the file's rate and
writes the decoded bytes. That is what `aplay` does with ALSA's `plug` behind it,
and what `pw-play` does with the graph behind it: the producer streams what it
has and the layer below reconciles it with the hardware. One conversion policy,
at one layer.

Measured: a 16 kHz stereo WAV to the ES8311 makes the codec adopt 16 kHz
(`sink adopts 16000 Hz`); a mono file at the same rate converts instead, the
codec path being stereo; a 48 kHz WAV into the 44.1 kHz Q45 link resamples at
realtime with the ASRC cost in the noise.

**Capture is `record`.** The same ES8311, the other direction:

    record <file.wav> [seconds]

It drives the registered source (the ES8311's ADC, the on-board microphone) and
writes a 16-bit PCM WAV. Capture and the disk are decoupled by a ring in PSRAM
and a writer task, and that is the difference between capturing and dropping:
the I2S has no backpressure, so a write that stalls -- the first ones, while the
FAT and directory are built, are the worst, and one measured 336 ms against a
21 ms average -- would otherwise cost samples. Measured on an old USB stick: a
serial read/write loop lost ~2.5% of a 20 s take, all at the start; with the
ring it captures the full 20 s with no ring stalls. The capture *level* is the
source's: the PGA is already at its 42 dB maximum, so a quiet, distant input
stays quiet.

**Reading:** `open()`/`read()` into a PSRAM chunk, not stdio. littlefs/FAT
through newlib's 1 kB `BUFSIZ` tops out near 180 kB/s -- exactly a 44.1 kHz
stereo WAV, and no more.

### Known issues

- **The Audioengine HD3 takes the board down.** Its LMP role-switch reaches
  `r_olm_lmp_rs_accepted` -> `olc_acl_rsw_req`, where the preview S31 BT
  controller asserts (`r_co_assert_info`) and the `btdm` task faults. Same
  family as the recorded `opcode:54` interop trouble, and upstream. The
  soundcore Q45 connects and streams.
- **littlefs read throughput** (~176 kB/s) -- too slow for raw PCM; the USB
  path is no better.
- **PSRAM is not in a core dump**, which matters because the BT/Wi-Fi `.bss`
  lives there.
- **A long stream can be cut at the end.** `suspend` flushes the sink's queue, so
  the engine pads with silence and waits for the link to take it before
  suspending. If the ring is still full when that wait times out -- measured as
  `drain: the link did not take it all (262143 B left, 162304 B/s last)` -- the
  tail can be lost. Re-measure before fixing: that figure was taken against a
  256 KB ring, and the ring is now 16 KB.

### Phase 2 (roadmap)

Built since this file was last revised, and now the baseline: the stream API and
mixer, per-stream volume and mute, the app ABI for playback **and** capture, rate
negotiation, and pipes.

**Remaining, in the order it is worth doing:**

1. **Per-stream sink selection.** A stream goes to the sink the system is using;
   there is no way to send two producers to two sinks at once. The mixer assumes
   a single output -- one buffer, one sink, one lifecycle -- and the rest follows:
   each sink needs its own mix of its own streams and its own
   start/drain/suspend, and probably its own mixer task, so a slow sink cannot
   stall a fast one. The mixing is the small part; the negotiation already stores
   each stream's sink format in `ring_fmt`. `play --sink` and `pipe` set the
   system's choice today and refuse any other sink; this is what makes them
   per-stream.
2. **An app that records.** The capture API is published and ABI-checked, but
   only `record` and a pipe have driven the source. A small recorder app would
   prove the ABI end to end, which is the point of having published it.
3. **A second source.** The registry is proven with one; an HTTP/Icecast source
   proves it is plural, needs no new hardware, and gives a pipe something to
   carry that is not the microphone.
4. **Per-sink volume**, then per-app streams: a volume per producer rather than
   per connection.
5. **`/dev/dsp`** for OSS-shaped apps, when something wants it.
6. **More sinks**: I2S out through GPIO, the DAC/amp on the core and Korvo
   boards, an RF transmitter; then LE Audio on S31.

### Testing

What is checked today, and how:

| what | how |
|---|---|
| the three negotiation answers | `play --sink es8311` with a 16 kHz stereo WAV (adopt), a 44.1 kHz file (forward), a mono 16 kHz file (convert); each logs which path it took |
| unknown sink or source refused | `play --sink nosuch ...`, `record --source nosuch ...` |
| pipes | `pipe add es8311 es8311`, `pipe`, `pipe del 1`; a sink that is not the engine's is refused with the reason |
| the mixer holds realtime | the counters behind `CONFIG_ESPIX_AUDIO_TRACE` and `ESPIX_SFX_TRACE`: block rate, ring level, retries |
| the app ABI | the build's `check-abi.py` |

What needs a test and does not have one:

- **None of the above is automatic.** Each line is a hand-run command, and these
  are the faults a build cannot catch: a rate that drifts, a stream that starves,
  a sink that never suspends. A suite that played a known file and asserted the
  drain rate is realtime would have caught the mixer producing 169206 frames a
  second in one run.
- **The app audio ABI has no test app.** `tests/app` has the infrastructure.
- **A pipe under load**: a capture and a stream at different rates, run for
  minutes, watched for a stall.
- **Per-sink mixing**, once it exists: two streams to two sinks, one slow, and
  the other must not glitch.

### Linux audio apps worth porting

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
- WiFi and Classic Bluetooth share one radio on S31.
- There is no hardware mixer; software mixing costs CPU and latency.
