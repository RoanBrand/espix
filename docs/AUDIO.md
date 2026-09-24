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
  an OSS-style `/dev/dsp` (open, ioctl format, write PCM). This is the
  "ALSA/PipeWire for espix" in minimal code; no `libasound`, no D-Bus.
- **Commands:** `play`, `aplay`/`arecord`, `amixer`-ish volume.

Engine library: **esp-gmf** (modular; the newer framework) plus `esp_codec_dev`
(codecs/I2S) and `esp_audio_codec` (MP3/AAC). **esp-adf** is now built on top of
ESP-GMF and is product-oriented; it is not the layer espix wants. Either way the
framework stays below our own API, not exposed to apps.

### Phase 1 (built now): Bluetooth speaker playback

S31 only. `bluetoothctl` pairs/connects the sink, then

    play <file|url>      # /home/esp/song.mp3, http(s)://..., or embed://...
    play status
    play stop

It is **GMF-based**: `play` drives Espressif's `esp_audio_simple_player`, which
takes a URI, picks the decoder from the extension, converts bit depth, channels
and rate, and hands PCM to a callback -- espix's callback, which writes into the
A2DP source's PCM ring. (A decoder earlier in this phase did the seam by hand;
the GMF player replaced it, because that is the layer phase 2 builds on.)

`play` does not need the sink first: it fills the ring and blocks in its output
callback until A2DP connects, so it can be issued while the link is still down --
which matters, because a connected link is what costs a memory-tight shell its
SSH sessions.

**Rates.** Resampling is off -- but not because the part cannot do it. The S31
has a **hardware ASRC** (`CONFIG_SOC_ASRC_SUPPORTED`; `esp_asrc`, and GMF's
`aud_asrc` element with `perf_type AUTO`). The problem is that the player path
does not use it: `esp_audio_simple_player` hardcodes the **software** converter
(`aud_rate_cvt` -> `esp_audio_effects`), which against a 48 kHz source loses
~250 short reads/s (~110 kB/s) and starves the PCM ring -- where the same
pipeline without it has **zero** short reads. Until resampling rides the
hardware ASRC, the source must match the sink's negotiated rate; SBC sinks pick
44.1 kHz in practice (the soundcore Q45 does), which is what a CD-rate MP3
already is. Routing `play` through `esp_gmf_asrc` (or a custom pipeline that
uses it) is the fix, not accepting a limit the silicon does not have.

**IDF version, on S31: use the `release/v6.1` branch, not the `v6.1` tag.** The
tag predates the S31 BR/EDR fixes (wrong TX-power table, ACL performance under
Wi-Fi coexistence, controller-lib LMP bugs). On the tag, A2DP drains at ~0.7x
realtime -- music with gaps and noise; on the branch it is realtime (~180 kB/s,
~352 SBC frames/s, zero ring underruns). See docs/UPSTREAM.md.

S3 I2S output is deferred. The S31 coreboard has a mono amp and a speaker
header (no speaker attached yet) and a mic; both are phase 2.

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
  speaker is the demanding case -- fine at low bitrates, worth measuring.
- There is no hardware mixer; software mixing costs CPU and latency.
