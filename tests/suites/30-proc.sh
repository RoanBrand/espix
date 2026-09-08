# Processes and the app ABI, through the test app.
#
# RESOURCES: none -- its /tmp paths carry the worker number, and the test app
# is staged once by the runner rather than by each suite that wants it.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi
espix_pass "test app is present and current"

# Everything this suite makes on the device carries the worker number, so a
# second copy of it running beside this one is invisible to it.
APPTXT=/tmp/espix-app-$ESPIX_WORKER.txt

APP="/home/$ESPIX_USER/testapp"

# argv, which is the whole reason a loaded app is more than a script.
argv_out=$(dev_run "$APP argv one two")
assert_contains "argc counts the app's own name" "argc 4" "$argv_out"
assert_contains "argv[0] is the app path"        "argv[0] $APP" "$argv_out"
assert_contains "arguments arrive in order"      "argv[2] one"  "$argv_out"

# Exit status has to come from its own connection: the shell has no $?.
assert_status "an app's exit status reaches the client" 7 dev_status "$APP exit 7"
assert_status "and zero is zero"                        0 dev_status "$APP exit 0"

# The file ABI: an app reaching the filesystem through libc, checked by espix.
dev_run "rm $APPTXT" >/dev/null 2>&1
assert_contains "an app can create a file" "ok" \
    "$(dev_run "$APP write $APPTXT hello-from-app")"
assert_contains "and read it back" "[hello-from-app]" \
    "$(dev_run "$APP read $APPTXT")"

# Permissions apply to apps exactly as to builtins -- checking in the shell
# alone would be a boundary you step around by running a program.
assert_contains "an app is refused /etc/passwd too" "EACCES" \
    "$(dev_run "$APP probe /etc/passwd")"
assert_contains "an app can read a world-readable file" "ok" \
    "$(dev_run "$APP probe /etc/hostname")"

# ps sees the processes that ran.
assert_contains "ps lists finished processes" "testapp" "$(dev_run 'ps')"

dev_run "rm $APPTXT" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# Confinement: `confine <dir> <cmd>` -- the process may not name a path outside
# <dir>, and gets ENOENT rather than EACCES so it cannot map the filesystem by
# probing one path at a time.
#
# The controls matter as much as the checks. /etc/hostname is world-readable and
# is asserted readable above, so if the confined probe fails it is confinement
# doing it and not permissions -- otherwise this suite would pass just as well
# against a root that did nothing.
# ---------------------------------------------------------------------------

JAIL=/tmp/espix-jail-$ESPIX_WORKER
dev_run "rm $JAIL/inside.txt" >/dev/null 2>&1
dev_run "rm -r $JAIL"         >/dev/null 2>&1
dev_run "mkdir $JAIL"         >/dev/null 2>&1

assert_contains "a confined app reads a file inside its root" "ok" \
    "$(dev_run "confine $JAIL $APP write $JAIL/inside.txt hello")"

assert_contains "a confined app cannot see outside it" "ENOENT" \
    "$(dev_run "confine $JAIL $APP probe /etc/hostname")"

# The binary itself lives outside the root, which is not a loophole: it is read
# before the process exists, exactly where execve(2) draws the same line.
assert_contains "and the binary may live outside the root" "argc" \
    "$(dev_run "confine $JAIL $APP argv")"

# chmod does not enter the VFS -- abi_fs.c resolves its own paths and calls
# espix_fs_chmod() directly, because IDF's chmod is a stub. So the root has to
# be checked separately in espix_fs_admin_check(), and for exactly one commit it
# was not. This is that regression.
#
# The target has to be a file the app *could* chmod when unconfined, or the
# check proves nothing: an earlier version of this aimed at /etc/hostname, which
# is root-owned, so the confined chmod failed on permissions and would have
# passed just as happily with the root bypassed. Hence the control below -- the
# same call, the same file, differing only in the confinement.
OUTSIDE=/tmp/espix-outside-$ESPIX_WORKER.txt
dev_run "rm $OUTSIDE" >/dev/null 2>&1
dev_run "$APP write $OUTSIDE owned-by-the-app" >/dev/null 2>&1

assert_contains "control: unconfined, the app may chmod its own file" \
    "chmod $OUTSIDE ok" \
    "$(dev_run "$APP chmod $OUTSIDE 0644")"

assert_not_contains "chmod is not a way out of the root" \
    "chmod $OUTSIDE ok" \
    "$(dev_run "confine $JAIL $APP chmod $OUTSIDE 0600")"

dev_run "rm $OUTSIDE" >/dev/null 2>&1

# A root that is not there gives the process nothing at all, so say so rather
# than starting something that cannot reach anything.
assert_contains "confine refuses a directory that does not exist" "not a directory" \
    "$(dev_run "confine $JAIL/nope $APP argv" 2>&1)"
assert_contains "confine refuses a file"                          "not a directory" \
    "$(dev_run "confine $JAIL/inside.txt $APP argv" 2>&1)"

dev_run "rm $JAIL/inside.txt" >/dev/null 2>&1
dev_run "rm -r $JAIL"         >/dev/null 2>&1
