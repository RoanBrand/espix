# Interfaces and routing.
#
# RESOURCES: none -- read-only, touches no shared state.

link_out=$(dev_run 'ip link')

assert_contains "lo is listed"    "lo:"    "$link_out"
assert_contains "wlan0 is listed" "wlan0:" "$link_out"

# We are talking to the device over this interface, so it had better be up with
# an address -- which makes this a check on `ip` rather than on the radio.
assert_contains "wlan0 is up" "wlan0: <UP" "$link_out"
assert_contains "wlan0 has an address" "inet " "$(dev_run 'ip addr show wlan0')"

# ---------------------------------------------------------------------------
# USB-NCM
#
# Whether a cable is plugged in during a test run is not something the suite can
# know, so nothing here asserts a particular link state. What it asserts is that
# the two ways of asking agree: `usb status` and `ip link` must not disagree
# about whether a host is attached, in either direction.
#
# That is the invariant a regression would break. usb0's UP flag comes from the
# lwip netif, and "attached" from TinyUSB's own enumeration callback; they are
# updated together in on_usb_event() and nowhere else, so if they ever diverge
# the link state has stopped tracking the cable.
# ---------------------------------------------------------------------------

usb_out=$(dev_run 'usb status')

case "$usb_out" in
    *"not built into this image"*)
        # This is a host-role build: the one OTG peripheral is driving storage
        # instead of presenting usb0, so usb0 must genuinely be absent rather
        # than merely unconfigured. The skip below records what is not being
        # tested; these two assertions are what would notice the default
        # changing back under the suite (docs/USB-HOST.md).
        assert_not_contains "no usb0 in a build without USB-NCM" "usb0:" "$link_out"
        assert_not_contains "the port is not reporting a failed host either" \
                            "usb host is not running" "$(dev_run 'lsblk')"
        espix_skip "usb-ncm is not in this build (the OTG port is the USB host)"
        ;;
    *)
        assert_contains "usb0 is listed whenever it is built in" "usb0:" "$link_out"

        attached=no
        case "$usb_out" in *"link:   attached"*) attached=yes ;; esac

        up=no
        case "$link_out" in *"usb0: <UP"*) up=yes ;; esac

        if [ "$attached" = "$up" ]; then
            espix_pass "usb status and ip link agree on the link (attached=$attached)"
        else
            espix_fail "usb status and ip link disagree about usb0" \
                       "usb status says attached=$attached, ip link says up=$up" \
                       "link state has stopped tracking enumeration"
        fi

        assert_contains "usb status names a mode" "mode:" "$usb_out"
        ;;
esac

# ---------------------------------------------------------------------------
# The default route belongs to WiFi.
#
# usb0 carries the route priority ESP-IDF gives Ethernet, below WiFi's, so
# plugging in a cable adds a way to reach the board without redirecting what the
# board itself sends. Worth a check because getting that backwards would be
# invisible until something on the device could not reach the internet with a
# cable in it.
# ---------------------------------------------------------------------------

route_out=$(dev_run 'ip route')
case "$route_out" in
    *"default via"*)
        case "$route_out" in
            *"default via"*"dev wlan0"*)
                espix_pass "the default route is wlan0's" ;;
            *)
                espix_fail "the default route is wlan0's" \
                           "actual: $route_out" \
                           "usb0 must not outrank WiFi" ;;
        esac
        ;;
    *)
        espix_skip "no default route to check"
        ;;
esac
