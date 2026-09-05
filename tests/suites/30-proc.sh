# Processes and the app ABI, through the test app.
#
# PARALLEL_SAFE=no -- writes files under /tmp and pushes a binary.

if ! dev_testapp_sync "$ESPIX_ROOT/fsroot/home/$ESPIX_USER/testapp"; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi
espix_pass "test app is present and current"

APP="/home/$ESPIX_USER/testapp"

# argv, which is the whole reason a loaded app is more than a script.
argv_out=$(dev_run "run $APP argv one two")
assert_contains "argc counts the app's own name" "argc 4" "$argv_out"
assert_contains "argv[0] is the app path"        "argv[0] $APP" "$argv_out"
assert_contains "arguments arrive in order"      "argv[2] one"  "$argv_out"

# Exit status has to come from its own connection: the shell has no $?.
assert_status "an app's exit status reaches the client" 7 dev_status "run $APP exit 7"
assert_status "and zero is zero"                        0 dev_status "run $APP exit 0"

# The file ABI: an app reaching the filesystem through libc, checked by espix.
dev_run "rm /tmp/espix-app.txt" >/dev/null 2>&1
assert_contains "an app can create a file" "ok" \
    "$(dev_run "run $APP write /tmp/espix-app.txt hello-from-app")"
assert_contains "and read it back" "[hello-from-app]" \
    "$(dev_run "run $APP read /tmp/espix-app.txt")"

# Permissions apply to apps exactly as to builtins -- checking in the shell
# alone would be a boundary you step around by running a program.
assert_contains "an app is refused /etc/passwd too" "EACCES" \
    "$(dev_run "run $APP probe /etc/passwd")"
assert_contains "an app can read a world-readable file" "ok" \
    "$(dev_run "run $APP probe /etc/hostname")"

# ps sees the processes that ran.
assert_contains "ps lists finished processes" "testapp" "$(dev_run 'ps')"

dev_run "rm /tmp/espix-app.txt" >/dev/null 2>&1
