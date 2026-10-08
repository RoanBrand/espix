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
  a name, a format, and write/start/suspend. The A2DP source is one sink; an
  I2S codec or the RF transmitters are next. The dependency points provider ->
  engine, so **audio does not depend on Bluetooth**: a target with no sink
  still builds and links, and `play` says none is available.
- **Streams** carry PCM to a sink; volume, mixing and routing are the next layer.
- **App interface:** a native `espix_audio` API and app ABI, plus optionally
  an OSS-style `/dev/dsp`.

### Phase 1 (built now): Bluetooth speaker playback

S31 only, with a connected A2DP sink:

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

**Resampling: none yet.** The engine converts neither rate nor channels, so a
source must match the rate the sink negotiated. A 48 kHz file into a 44.1 kHz
SBC stream plays at the wrong speed, and sounding wrong is its only symptom.
The order should be: (1) if the sink advertises the source's rate, ask for it
in the preferred codec config and pass the PCM through untouched; (2) only if
it does not, convert -- with the S31's hardware ASRC (`esp_asrc`) rather than
a software converter. SBC sinks generally advertise 48 kHz as well as 44.1.

**Reading:** `open()`/`read()` into a PSRAM chunk, not stdio. littlefs/FAT
through newlib's 1 kB `BUFSIZ` tops out near 180 kB/s -- exactly a 44.1 kHz
stereo WAV, and no more.

### Known issues

- **The Audioengine HD3 takes the board down.** Its LMP role-switch reaches
  `r_olm_lmp_rs_accepted` -> `olc_acl_rsw_req`, where the preview S31 BT
  controller asserts (`r_co_assert_info`) and the `btdm` task faults. Same
  family as the recorded `opcode:54` interop trouble, and upstream. The
  soundcore Q45 connects and streams.
- **No resampling**, so a source at the sink's rate is required.
- **littlefs read throughput** (~176 kB/s) -- too slow for raw PCM; the USB
  path is no better.
- **PSRAM is not in a core dump**, which matters because the BT/Wi-Fi `.bss`
  lives there.

### Phase 2 (roadmap)

- Resampling: negotiate the sink's rate first, then the hardware ASRC.
- A PCM gain and per-sink volume; per-app streams and mixing.
- The `espix_audio` app ABI, and `/dev/dsp`.
- I2S sink and source (`esp_codec_dev`), as named sinks/sources.
- Network roles: HTTP/Icecast source, UPnP/DLNA, Snapcast, AirPlay, MPD.
- LE Audio on S31 (phase 3).

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
