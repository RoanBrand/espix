# USB host: the OTG port's role, and what it enumerates.
#
# RESOURCES: none -- read-only, touches no shared state.
#
# No test runner can plug a stick in, so what is asserted here is everything that
# does not need one: that the role this image was built with is the role it
# reports, that a host build really has no usb0, that the port did not report a
# failed stack, and that the two commands describing storage answer coherently.
# The device-dependent half skips loudly when nothing is attached rather than
# passing on an empty table -- an empty `lsblk` is a legitimate answer and a
# useless test.

lsblk_out=$(dev_run 'lsblk')

# The command says which build this is rather than disappearing, so "was it built
# in" is a question its own output answers.
host_role=no
case "$lsblk_out" in
    *"usb host was not built into this image"*) host_role=no ;;
    *) host_role=yes ;;
esac

if [ "$host_role" = no ]; then
    # A legitimate build, not a failure: USB host and USB-NCM are two uses of the
    # one OTG peripheral, and the device role is the other answer.
    assert_contains "the device role says which option would have built it" \
                    "CONFIG_ESPIX_USB_ROLE_HOST" "$lsblk_out"
    assert_contains "blkid explains itself the same way" \
                    "usb host was not built into this image" "$(dev_run 'blkid')"
    espix_skip "this image is the device role; usb0 storage is not built in"

else
    # A host build that could not install the stack says so, and that is the one
    # failure this suite can see without a stick: `lsblk` distinguishes "nothing
    # attached" (no output) from "the port is not running" (a diagnostic).
    assert_not_contains "the host stack installed" "usb host is not running" "$lsblk_out"

    # The roles are exclusive on this part, so `usb0` must be absent -- this is
    # the behaviour change the host default brought, and the assertion that would
    # notice it being undone.
    assert_not_contains "a host build has no usb0" "usb0:" "$(dev_run 'ip link')"
    assert_contains "usb status says USB-NCM is not in this image" \
                    "not built into this image" "$(dev_run 'usb status')"

    # Both commands answer, and an operand that names nothing is refused rather
    # than silently matching nothing.
    assert_status "lsblk on an unknown device fails" 1 dev_status 'lsblk sdq'
    assert_status "blkid on an unknown device fails" 1 dev_status 'blkid sdq'

    if [ -z "$lsblk_out" ]; then
        espix_skip "nothing attached; plug a stick into the OTG socket to exercise enumeration"
    else
        assert_contains "lsblk prints its header" "NAME" "$lsblk_out"
        assert_contains "the header names the filesystem column" "FSTYPE" "$lsblk_out"

        # The disk row is the thing blkid and lsblk have to agree about, and its
        # name is what an operand takes.
        disk=$(printf '%s\n' "$lsblk_out" |
               awk 'NR > 1 && $3 == "disk" { print $1; exit }')

        blkid_out=$(dev_run 'blkid')
        assert_contains "blkid knows the same disk" "$disk:" "$blkid_out"
        assert_contains "blkid names the same type column" 'TYPE="disk"' "$blkid_out"
        assert_contains "blkid takes a device name" "$disk:" "$(dev_run "blkid $disk")"

        # A partition appears under its disk. The tree characters that draw it are
        # three bytes each, which is a formatting detail rather than something to
        # assert on here: the row's own TYPE column says what it is.
        if printf '%s\n' "$lsblk_out" |
                awk 'NR > 1 && $3 == "part" { found = 1 } END { exit !found }'; then
            espix_pass "a partition is listed under its disk"
        else
            espix_skip "no partitions on the attached device"
        fi
    fi
fi
