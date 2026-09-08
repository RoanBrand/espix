# Filesystem: the commands, the mode bits, and the refusals.
#
# RESOURCES: none -- the paths it makes carry the worker number, so two of
# these side by side cannot see each other's files.

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
