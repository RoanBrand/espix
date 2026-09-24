# ESP32-S31 A2DP source: upstream report

Filed as **espressif/esp-idf#19130**:
<https://github.com/espressif/esp-idf/issues/19130>

## What this is

While bringing up Bluetooth audio on the S31, espix's A2DP source could not
connect to an Audioengine HD3 speaker. A clean-room check (stock ESP-IDF v6.1,
stock `examples/bluetooth/bluedroid/classic_bt/a2dp_source`, no espix) showed the
fault is **sink-dependent and controller-side**:

- the HD3 fails at link setup: the controller logs
  `OLM_LMP: acl lmp unpack failed ... opcode:54` and the open dies as
  `BTA_AV_OPEN_EVT::FAILED status: 3` (`BTA_AV_FAIL_ROLE`);
- a soundcore Space Q45 works on the same board and build (connects, streams,
  AVRCP live).

Suppressing the host's role-switch request in `bta_av_link_role_ok()` changed
nothing, so the failure is inside the closed controller, not the Bluedroid host.

## Contents

- `report.md` -- the body filed on the issue.
- `example-address-match.patch` -- the only change to the stock example: it
  selects the peer by BD address, because the HD3 does not advertise a name in
  its inquiry response. Touches nothing in the Bluetooth stack.
- `hd3-failure.log` -- serial capture, HD3: the failure every cycle.
- `q45-success.log` -- serial capture, Q45: connected, AVRCP notifications.
- `hd3-sleeping-pagetimeout.log` -- serial capture with the HD3 asleep, showing
  the separate page-timeout variant (`st 0x4`).

## Reproduce

```
cd $IDF_PATH/examples/bluetooth/bluedroid/classic_bt/a2dp_source
idf.py --preview set-target esp32s31
# apply example-address-match.patch (set the BD address for the sink under test)
idf.py build flash monitor
```

Build/hosts used: ESP-IDF v6.1 (fff9895c), ESP32-S31 Function-Coreboard-1 V1.0.
