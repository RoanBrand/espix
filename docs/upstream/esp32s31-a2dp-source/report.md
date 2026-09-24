### Answers checklist.
- [x] I have read the documentation [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/) and the issue is not addressed there.
- [x] I have updated my IDF branch (master or release) to the latest version and checked that the issue is present there.
- [x] I have searched the issue tracker for a similar issue and not found a similar issue.

### IDF version.
v6.1 (commit fff9895c, 2026-08-25), release branch. Stock checkout, no local modifications.

### Espressif SoC revision.
ESP32-S31, revision v0.0. Controller: `BREDR_INIT: BT controller compile version [beae498]`, `BTDM version [39ed4dc]`, `libbtbb d5590f7 (Jul 14 2026)`.

### Operating System used.
macOS

### How did you build your project?
Command line with idf.py

### Development Kit.
ESP32-S31 Function-Coreboard-1 V1.0 (WROOM-3 module; N16R16V, 16 MB flash / 16 MB PSRAM).

### Power Supply used.
USB

### What is the expected behavior?
The stock A2DP source example should connect to, and stream to, any powered, in-range A2DP sink.

### What is the actual behavior.
Sink-dependent. With one speaker the A2DP source **cannot** open a stream; with another it works.

**Fails — Audioengine HD3 (BD 00:22:d9:00:1a:ee).** Every ~10 s the example connects, the ACL comes up, the controller logs an LMP parse failure, and the open is aborted. This repeats indefinitely:

```
I (30750) BT_AV: a2dp connecting to peer: 00:22:d9:00:1a:ee
E (31930) OLM_LMP: acl lmp unpack failed, err:262!
E (31930) OLM_LMP: opcode:54
W (31950) BT_HCI: hcif conn complete: hdl 0x801, st 0x0
W (32300) BT_APPL: bta_dm_act no entry for connected service cbs
W (32310) BT_BTC: BTA_AV_OPEN_EVT::FAILED status: 3
W (34470) BT_HCI: hcif disc complete: hdl 0x801, rsn 0x13 dev_find 1
```

`status: 3` is `BTA_AV_FAIL_ROLE` (`bta_av_aact.c`, set when the A2DP-open role switch returns a non-success HCI status); the `OLM_LMP ... opcode:54` line is emitted by the controller immediately before it, and `rsn 0x13` is the remote dropping the link.

**Works — soundcore Space Q45 (BD e8:ee:cc:5d:0d:50).** Same build, same board, same example: the A2DP link opens and stays up, and the AVRCP controller receives the sink's ongoing volume-change notifications:

```
I (1874800) RC_CT: AVRC event notification: 13
I (1875160) RC_CT: AVRC event notification: 13
...
```

So the ESP32-S31 CAN act as an A2DP source; the Audioengine HD3 specifically triggers the LMP/role failure.

### Steps to reproduce.
1. Stock ESP-IDF v6.1, unmodified.
2. Build the bundled example: `examples/bluetooth/bluedroid/classic_bt/a2dp_source` for `esp32s31` (`idf.py --preview set-target esp32s31; idf.py build flash monitor`).
3. The Audioengine HD3 does not advertise a name in its inquiry response, so select the peer by BD address in `filter_inquiry_scan_result()` (a 6-line match; it changes nothing in the Bluetooth stack). For the HD3 use `{0x00,0x22,0xd9,0x00,0x1a,0xee}`; for the Q45 use `{0xe8,0xee,0xcc,0x5d,0x0d,0x50}`.
4. Power the sink, in range. With the HD3 the open fails every cycle as above; with the Q45 it connects and streams.

### Debug Logs.
Both logs are above. No other Bluetooth activity, Wi-Fi unused.

### More Information.
- Two sinks tested on the same board/build: the HD3 fails deterministically; the Q45 succeeds. That makes it a sink-interoperability problem rather than a general A2DP-source failure.
- Related reports: #4337 ("Bluetooth classic role switch fails for reconnecting HID devices"; "the failing devices disconnect if the role switch does not succeed") and #15913 (`esp_a2d_source_connect` fails repeatedly before connecting). This one is target-specific: the ESP32-S31 controller logs `OLM_LMP: acl lmp unpack failed ... opcode:54` at the moment the role switch would happen.
- Happy to run further captures against either sink, or additional sinks, if useful.
- We also tried supressing the host's master-role request (commenting out the switch in `bta_av_link_role_ok()`, `bta_av_main.c`), since the failure surfaces as `BTA_AV_FAIL_ROLE`. It made **no difference**: the `OLM_LMP ... opcode:54` parse failure occurs during link setup, before any profile open, so the host cannot avoid it. `status: 3` is a consequence, not the cause.
- The failing sink is a Qualcomm/CSR-class aptX speaker (aptX/aptX-HD/AAC/SBC); the working control is a Bluetooth 5 headset. This looks like an LMP negotiation the ESP32-S31 controller does not parse (`opcode 54`).
