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
- **Streams** carry PCM to a sink; volume, mixing and routing are the next layer.
- **App interface:** a native `espix_audio` API and app ABI, plus optionally
  an OSS-style `/dev/dsp`.

### Phase 1 (built now): Bluetooth and local speaker playback

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

**Resampling is `esp_asrc`.** When a source's rate differs from the sink's, the
decoded frame goes through esp_asrc -- the S31's hardware ASRC, and an
optimized software path where there is none -- before the sink sees it. It
depends only on `esp_audio_effects`, so this is not the GMF pipeline returning.
A channel-only difference (mono into a stereo sink) stays the engine's own
duplication, which is cheaper than the ASRC's channel matrix, and a source
already at the sink's rate never opens it. Measured: a 48 kHz WAV into the
44.1 kHz Q45 link resamples at realtime, with the ASRC cost in the noise.

Negotiating the sink's rate for a matching source (so no conversion happens at
all) is still worth doing and is the cheaper path when the source is known
before the link is opened.

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

### Phase 2 (roadmap)

- Resampling: negotiate the sink's rate first, then the hardware ASRC.
- A PCM gain and per-sink volume; per-app streams and mixing.
- The `espix_audio` app ABI, and `/dev/dsp`.
- I2S **source** (the ES8311's ADC and the on-board mic). The sink half --
  `espix_i2s`, the ES8311 DAC and NS4150B PA -- is built and audible.
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
