# Filesystem: the commands, the mode bits, and the refusals.
#
# RESOURCES: none for the paths it makes -- they carry the worker number, so two of
# these side by side cannot see each other's files. The ext section at the end needs
# a volume attached and root to mount it, and skips loudly when neither is there.

T=/tmp/espix-test-fs-$ESPIX_WORKER
# The full name, not the stem: the two assertions below look at a listing of
# /tmp, where another worker's espix-test-fs-2 is also sitting. Matching the
# stem would let this suite see its neighbour's directory and call it its own
# -- and worse, the "it is gone now" assertion would fail on a directory that
# is none of its business.
TNAME=espix-test-fs-$ESPIX_WORKER

dev_run "rm -r $T" >/dev/null 2>&1
dev_run "mkdir $T" >/dev/null
assert_contains "mkdir creates a directory" "$TNAME" "$(dev_run 'ls -l /tmp')"

dev_run "echo hello-espix > $T/a.txt" >/dev/null
assert_eq "a file round-trips through the shell" "hello-espix" "$(dev_run "cat $T/a.txt")"

ls_out=$(dev_run "ls -l $T")
assert_contains "ls -l shows the file"        "a.txt"      "$ls_out"
assert_contains "a new file is 0644 by rule"  "-rw-r--r--" "$ls_out"
assert_contains "a new file is owned by its creator" "$ESPIX_USER" "$ls_out"

dev_run "chmod 600 $T/a.txt" >/dev/null
assert_contains "chmod is visible in ls -l" "-rw-------" "$(dev_run "ls -l $T/a.txt")"

dev_run "chmod 644 $T/a.txt" >/dev/null
assert_contains "chmod back to the rule's answer" "-rw-r--r--" "$(dev_run "ls -l $T/a.txt")"

# Permissions are enforced, not decorative -- this is what the prompt sigil
# now depends on being true.
assert_contains "an ordinary account cannot read /etc/passwd" "denied" \
    "$(dev_run 'cat /etc/passwd')"
assert_contains "an ordinary account cannot read the wifi PSK" "denied" \
    "$(dev_run 'cat /etc/wifi.conf')"
assert_contains "an ordinary account cannot read the SSH host key" "denied" \
    "$(dev_run 'cat /etc/ssh/host_ecdsa_key')"
# /etc/hostname is 0644 and derived from the MAC, so assert on the shape
# rather than a literal -- the point is that a world-readable file is readable,
# not what this particular board is called.
assert_contains "world-readable files are still readable" "esp32s3-" \
    "$(dev_run 'cat /etc/hostname')"

dev_run "mv $T/a.txt $T/b.txt" >/dev/null
b_out=$(dev_run "ls -l $T")
assert_contains "mv renames"          "b.txt" "$b_out"
assert_not_contains "the old name is gone" "a.txt" "$b_out"

dev_run "cp $T/b.txt $T/c.txt" >/dev/null
assert_eq "cp copies the contents" "hello-espix" "$(dev_run "cat $T/c.txt")"

dev_run "rm -r $T" >/dev/null
assert_not_contains "rm -r removes the tree" "$TNAME" "$(dev_run 'ls /tmp')"

# ---------------------------------------------------------------------------
# /dev: a directory espix answers for itself.
#
# It is not on littlefs, its entries come from the device table, and nothing
# under it is a name anyone may create or remove. An older image's /dev may
# still hold files; none of them may show up here.
# ---------------------------------------------------------------------------

dev_out=$(dev_run 'ls /dev')
assert_contains     "ls /dev lists the null device"    "null"    "$dev_out"
assert_contains     "ls /dev lists the factory device" "factory" "$dev_out"
assert_not_contains "ls /dev hides anything else"      "keep"    "$dev_out"
# ESP-IDF's own UART VFS is a separate mount at a longer prefix; /dev lists
# only what espix owns.
assert_not_contains "ls /dev does not list the UART VFS" "uart" "$dev_out"

assert_contains "/ is still a directory listing with dev in it" "dev" \
    "$(dev_run 'ls /')"

dev_l=$(dev_run 'ls -l /dev')
assert_contains "null is a character device"  "c"           "$dev_l"
assert_contains "null is world-writable"      "crw-rw-rw-"  "$dev_l"
assert_contains "factory is read-only"        "-r--r--r--"  "$dev_l"

assert_eq "cat /dev/null is empty" "" "$(dev_run 'cat /dev/null')"

# A non-root shell must be able to redirect into the sink -- which is why the
# permission check has to read the device's declared mode, not the rule's 0644.
assert_status "an ordinary account may write /dev/null" 0 dev_status \
    'echo swallowed > /dev/null'

# Nothing in /dev is editable, in either direction.
assert_status "touch /dev/x is refused"     1 dev_status 'touch /dev/x'
assert_status "mkdir /dev/x is refused"     1 dev_status 'mkdir /dev/x'
assert_status "rm /dev/null is refused"     1 dev_status 'rm /dev/null'
assert_status "rm -r /dev is refused"       1 dev_status 'rm -r /dev'
assert_status "chmod on a device is refused" 1 dev_status 'chmod 600 /dev/null'

# And /dev is still there afterwards, in its own right.
assert_contains "/dev survives the attempts" "null" "$(dev_run 'ls /dev')"

# --------------------------------------------------------------- sizes ---
#
# `df` is the only place a size is visible without a USB device attached, so it
# is where the formatter can be checked on a board with nothing plugged in.
#
# The unit table reaches 'T' because it stopped at 'G' for a while and printed a
# 3.1TB volume as "3214G" -- the same value `lsblk` and `ls` report through the
# same function, so one command covers all of them. A rootfs is far too small to
# reach T here; what this asserts is the shape below it, which is what a wrong
# bound breaks first.
df_h=$(dev_run 'df -h')
assert_contains "df -h renames the size column" "Size"        "$df_h"
assert_not_contains "df -h does not claim 1K blocks" "1K-blocks" "$df_h"
assert_contains "df -h names the root filesystem" "littlefs"    "$df_h"

# A rootfs is 12M, so no assertion here can reach the unit that was missing --
# the T boundary is where the bug was, and the numbers either side of it are not
# reachable from a board with a small volume on it. What this pair does cover is
# that the command still runs and lays its table out with the new widths; the
# boundary values are checked against a real 3.1TB disk by the USB suite, and
# against the formatter directly wherever espix_cmd_size() is compiled for a host.
#
# The plain column is a 64-bit count -- 1K-blocks of a 3.1TB volume is over 2^32,
# and it used to be formatted through an `unsigned`.
df_plain=$(dev_run 'df')
assert_contains "df names the block column" "1K-blocks" "$df_plain"
assert_contains "df still names the root filesystem" "littlefs" "$df_plain"

# ------------------------------------------------------- silent truncation ---
#
# readdir answers NULL for "no more entries" *and* for an I/O error, and only
# sets errno for the second. Until this was fixed `ls` never looked, so a walk
# that failed part-way printed a short listing and a count -- on a 3.1TB exFAT
# volume it once said "13 entries" for a directory holding 19, which reads as a
# directory that holds 13 things.
#
# The failing half of that is asserted against the real volume by 75-usb.sh.
# What is asserted here is the half that guards the fix: errno is cleared before
# every readdir, and a stale value read after a *successful* walk would make an
# ordinary listing claim a failure. A clean directory must stay quiet.
ls_ok=$(dev_run 'ls -l /')
assert_not_contains "a clean listing reports no read error" "stopped after" "$ls_ok"
assert_not_contains "a clean listing reports no I/O error"  "Input/output error" "$ls_ok"
assert_contains     "and still prints its count"            "entries" "$ls_ok"
assert_not_contains "the synthetic /dev listing is quiet too" "stopped after" \
    "$(dev_run 'ls -l /dev')"

# ------------------------------------------------- ext: the inodes are the rules ---
#
# ext keeps modes and owners in its inodes, so espix reads them rather than its own
# rule: a volume's permissions are the volume's, which is how Linux treats a
# filesystem that carries its own. `lost+found` is what makes that decisive without
# creating anything on the volume -- mke2fs always makes it 0700 and root-owned,
# where the rule would have said 0755 for a directory and given it to the mount's
# owner.
#
# Needs an ext volume attached and root to mount it, so it skips loudly otherwise
# rather than passing against nothing.
ext_part=$(dev_run 'lsblk' | awk 'NR > 1 && $3 == "part" && $4 ~ /^ext/ { print $1; exit }')
if [ -z "$ext_part" ]; then
    espix_skip "no ext volume attached -- needs the stick on the OTG port"
else
    ext_mnt=/mnt/espix-ext-$ESPIX_WORKER
    dev_run "sudo mkdir -p $ext_mnt" >/dev/null 2>&1

    # Mounted with the options a metadata-less volume needs, deliberately: one that
    # keeps its own must ignore them, and that is half the point.
    mount_out=$(dev_run "sudo mount -o uid=1000,gid=1000 $ext_part $ext_mnt 2>&1")
    if [ -n "$mount_out" ]; then
        espix_skip "could not mount $ext_part: $mount_out"
    else
        listing=$(dev_run "ls -l $ext_mnt 2>&1")

        assert_contains "an ext volume's modes come from its inodes, not the rule" \
                        "drwx------" "$listing"
        assert_contains "and its owner is the inode's, not whoever mounted it" \
                        "root root" "$listing"

        # 0700 root, so the rule's 0755 for a directory would have let this through.
        opened=$(dev_run "ls $ext_mnt/lost+found 2>&1")
        assert_contains "a directory the volume keeps private is refused to esp" \
                        "cannot open" "$opened"

        # Changing one needs a writable mount, which is the next milestone rather
        # than this one -- so this is a refusal by status, not by message.
        assert_status "chmod on an ext volume refuses, having nowhere to write yet" \
                      1 dev_status "chmod $ext_mnt/lost+found 0755"

        # Both truncates, which nothing else in the suite calls: cp reaches
        # truncation by opening with O_TRUNC, which is the open path. The file is
        # root's, so it is made writable first -- and that exercises chmod's success
        # path on a writable volume while it is there.
        if ! dev_testapp_present; then
            espix_skip "test app not built -- run 'make test-app'"
        else
            app=/home/$ESPIX_USER/testapp
            dev_run "sudo cp /bin/hello $ext_mnt/trunc.txt" >/dev/null 2>&1
            dev_run "sudo chmod 0666 $ext_mnt/trunc.txt" >/dev/null 2>&1

            trunc_out=$(dev_run "$app truncate $ext_mnt/trunc.txt 2000 2>&1")
            assert_contains "truncate() reaches an ext inode" \
                            "truncate $ext_mnt/trunc.txt 2000 ok" "$trunc_out"
            assert_contains "and ftruncate() takes it to half" \
                            "ftruncate $ext_mnt/trunc.txt 1000 ok" "$trunc_out"
            assert_contains "the inode reports what they left" "1000" \
                            "$(dev_run "ls -l $ext_mnt/trunc.txt")"

            dev_run "sudo rm $ext_mnt/trunc.txt" >/dev/null 2>&1
        fi

        dev_run "sudo umount $ext_mnt" >/dev/null 2>&1
        dev_run "sudo rmdir $ext_mnt" >/dev/null 2>&1
    fi
fi

