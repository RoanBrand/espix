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
        assert_contains "blkid takes the /dev spelling" "$disk:" \
                        "$(dev_run "blkid /dev/$disk")"

        # The names live in /dev too, which is what makes `mount /dev/sda1` --
        # and reading the name off `ls /dev` -- work at all.
        assert_contains "the disk has a node in /dev" "$disk" "$(dev_run 'ls /dev')"
        assert_status "a device that is not attached has no node" 1 \
                      dev_status 'ls /dev/sdq'

        # A partition appears under its disk. The tree characters that draw it are
        # three bytes each, which is a formatting detail rather than something to
        # assert on here: the row's own TYPE column says what it is.
        # A partition's name in column 1 carries the tree glyph ("├─sda1"), and
        # /dev lists the bare name, so the glyph has to go before comparing. The
        # suite failed against a working device until it did.
        part=$(printf '%s\n' "$lsblk_out" |
               awk 'NR > 1 && $3 == "part" { print $1; exit }' |
               sed 's/^[^[:alnum:]]*//')

        if [ -n "$part" ]; then
            espix_pass "a partition is listed under its disk"
            assert_contains "the partition has a node in /dev" "$part" \
                            "$(dev_run 'ls /dev')"

            # A partition operand takes the /dev spelling too, which is how an
            # fstab rule writes it. This is a partition rather than the disk above,
            # and it used to fail: blkid normalised the operand while validating it
            # and then compared the raw argument while selecting the row, so
            # `blkid /dev/sda1` printed nothing at all and exited 0 -- no line, no
            # error. The disk-only assertion above could not see that.
            assert_contains "blkid takes the /dev spelling for a partition" \
                            "$part:" "$(dev_run "blkid /dev/$part")"

            # A partition's PARTUUID has one of two shapes: an MBR's disk
            # signature and entry number, or a GPT entry's 36-character GUID.
            # Worth asserting wherever a partition is listed, because this is the
            # value a /etc/fstab rule takes, and a malformed one is a rule that
            # can never match -- on a disk that otherwise looks entirely fine.
            puuid=$(dev_run "blkid $part" |
                    sed -n 's/.*PARTUUID="\([^"]*\)".*/\1/p')

            if [ -z "$puuid" ]; then
                espix_skip "the partition's table carries no PARTUUID"
            elif printf '%s' "$puuid" | grep -qE '^[0-9a-f]{8}-[0-9]{2}$'; then
                espix_pass "the partition's PARTUUID is an MBR signature and entry"
            elif printf '%s' "$puuid" |
                 grep -qE '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$'; then
                espix_pass "the partition's PARTUUID is a GPT entry GUID"
            else
                espix_fail "the partition's PARTUUID is neither spelling" \
                           "actual: $puuid"
            fi
        else
            espix_skip "no partitions on the attached device"
        fi

        # An ext partition is named by its version rather than as a range. The name
        # comes from the superblock's feature words (host.c's ext_region_fstype),
        # because ext stores no version: ext4 arrived with extents, 64-bit and
        # flex_bg, and a journal is what ext3 added over ext2.
        #
        # Asserted on the shape rather than on "ext4", because the volume here is
        # whatever was plugged in -- an ext2 stick saying ext2 is right, and a test
        # that demanded ext4 would fail on a working device.
        extname=$(printf '%s\n' "$lsblk_out" | awk '$4 ~ /^ext/ { print $4; exit }')

        if [ -z "$extname" ]; then
            espix_skip "no ext partition attached; the version naming is not exercised"
        elif printf '%s' "$extname" | grep -qE '^ext[234]$'; then
            espix_pass "an ext partition is named by its version ($extname)"
        else
            espix_fail "an ext partition is named by its version" \
                       "actual: $extname" \
                       "the feature words should give ext2, ext3 or ext4"
        fi

        # --------------------------------------------------------- read-only ---
        #
        # `mount -o ro` is enforced by the block device, not by espix: the view
        # is marked read-only, IDF's diskio status callback answers STA_PROTECT
        # for it, and FatFs then refuses a write with FR_WRITE_PROTECTED
        # (ff.c:3508) before it reaches the disk at all. That is the property
        # worth asserting, because a mount that *said* ro while the volume below
        # still accepted writes would be worse than one that said nothing.
        #
        # Only mounts something nothing else is using: this suite runs alone, and
        # an example mount point is created and removed around it.
        if [ -n "$part" ]; then
            ros=/tmp/espix-ro-test

            if ! dev_run "mkdir -p $ros" >/dev/null 2>&1; then
                espix_skip "could not make a mount point for the read-only check"
            elif dev_status "sudo mount -o ro /dev/$part $ros" ; then
                espix_pass "a volume mounts read-only"

                assert_contains "mount names it read-only" "(ro)" "$(dev_run 'mount')"

                # The write must be refused. `touch` is the cheapest write there
                # is: it creates a directory entry, so it fails for a read-only
                # volume whatever the file's contents would have been.
                assert_status "and refuses a write through it" 1 \
                    dev_status "touch $ros/ro-probe"

                # The read side must still work -- enforcing this by breaking
                # reads would pass the assertion above for the wrong reason.
                assert_status "while still reading" 0 dev_status "ls -l $ros"

                dev_run "sudo umount $ros" >/dev/null 2>&1
                assert_not_contains "and unmounts cleanly" "$ros" "$(dev_run 'mount')"
            else
                espix_skip "could not mount /dev/$part read-only to check it"
            fi

            dev_run "rmdir $ros" >/dev/null 2>&1
        fi

        # ------------------------------------------------------------ sizes ---
        #
        # The disk row's SIZE is the same value blkid reports in bytes, run
        # through espix_cmd_size() -- the formatter every command shares. Its
        # unit table stopped at 'G' for a while, so a 3.1TB partition read
        # "3214G" here, in `df -h`, and in `ls` alike.
        #
        # Only a device at or past 1TB can show this, so a small stick is skipped
        # rather than asserted against: a stick that prints "29G" is right, and a
        # test that demanded "T" of it would be asserting the wrong thing about a
        # working device. T9 owners get the assertion; everyone else gets a skip
        # that says why.
        #
        # PRODUCT is what tells the disk's line from a partition's: blkid prints
        # it only for the disk, and a partition's SIZE is part of the disk's.
        disk_size=$(printf '%s\n' "$lsblk_out" |
                    awk -v d="$disk" '$1 == d { print $2; exit }')
        # One line, so it survives being run through `sh -c` on the device:
        # sub() drops everything but SIZE, which PRODUCT pins to the disk's line
        # rather than a partition's.
        blkid_bytes=$(dev_run "blkid $disk" |
                      awk '/PRODUCT=/ { sub(/.*SIZE="/, ""); sub(/".*/, ""); print; exit }')

        if [ -z "$blkid_bytes" ]; then
            espix_skip "the disk reports no byte size, so its unit cannot be checked"
        elif [ "$blkid_bytes" -lt 1000000000000 ]; then
            espix_skip "disk is under 1TB, so the terabyte unit cannot be exercised"
        else
            # 1TB is at least "1.0T", and the rounding that makes 3.638 TiB read
            # "3.6T" rather than "3.7T" happens here too -- so a wrong one shows
            # up as a T value a whole unit too high.
            case "$disk_size" in
                *T) espix_pass "a disk past 1TB is sized in terabytes ($disk_size)" ;;
                *)  espix_fail "a disk past 1TB should be sized in terabytes" \
                               "lsblk SIZE: $disk_size (blkid: ${blkid_bytes} bytes)" ;;
            esac
        fi
    fi
fi
