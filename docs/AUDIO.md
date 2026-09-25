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

`play` does not need the sink first: it fills the ring and blocks in its output
callback until A2DP connects, so it can be issued while the link is still down.

**The stream is mono, 44.1 kHz, bitpool <= 35**, set with
`esp_a2d_source_set_pref_mcc()` on the sink-codec-caps event -- the same choice
IDF's stock `a2dp_source` example makes (mono, bitpool 35, loudness, 8
subbands, block 16). With the sink's defaults (joint stereo, bitpool up to 52)
the link cannot hold the stream together and known-good sinks play harsh noise.
The source role also initialises AVRCP **controller** only; the target half is
not espix's.

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

**MP3 decode is software, scalar, and just under realtime.** There is no
hardware audio decoder on any ESP32, and on S31 there is no accelerated MP3
library either: the codec is the OpenCore/Helix fixed point decoder built for
the target, and its `pvmp3_poly_phase_synthesis`/`pvmp3_equalizer` measured at
**~93% of one core** for 44.1 kHz stereo (the log's read/decode/feed interval is
several seconds, so read it as a fraction of the interval, not per second). The
S31 does have a PIE/SIMD coprocessor, but `esp_audio_codec`'s assembly variant
uses it only for **LC3 (114 sites) and Opus (19)** -- never for MP3 -- and only
core 1 has PIE at all, which is why the assembly option pins its caller there.
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

- **MP3 decode is ~2.9x realtime on S31.** `esp_audio_simple_dec`'s MP3 path
  measures ~2900 ms of decode per second of audio (WAV is 10-27 ms), so the ring
  cannot stay fed and MP3 playback is choppy at any source rate. This is the
  main blocker for the phase-1 goal. Next measurement: decode with Bluetooth
  off, to separate a slow library from CPU throttling under coexistence.
- **littlefs read throughput** (~176 kB/s) -- too slow for raw PCM; fix or route
  around.
- **No resampling**, so non-44.1 kHz sources do not play correctly.
- **No volume or AVRCP absolute volume** yet.

S3 I2S output is deferred. The S31 coreboard has a mono amp and a speaker header
(no speaker attached yet) and a mic; both are phase 2.

### Phase 2 (roadmap)

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
