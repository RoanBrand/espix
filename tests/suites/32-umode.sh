# U-mode apps on the S31 (docs/USER-MODE.md, M1): an app in its own address
# space reaches the kernel only through syscalls, and a fault ends only the app.
#
# RESOURCES: none -- the binaries carry the worker number.

if [ "$ESPIX_TARGET" != "esp32s31" ]; then
    espix_skip "user mode is S31-only"
    return 0
fi

UM_DIR=$(mktemp -d)
UAPP="/home/$ESPIX_USER/uapp-$ESPIX_WORKER"
UHELLO="/home/$ESPIX_USER/hello-u-$ESPIX_WORKER"

if ! "$ESPIX_ROOT/tools/build-uapp.sh" -o "$UM_DIR/uapp" "$ESPIX_ROOT/tests/uapp/uapp.c" >/dev/null 2>&1 ||
   ! "$ESPIX_ROOT/tools/build-uapp.sh" -o "$UM_DIR/hello-u" "$ESPIX_ROOT/apps/hello/main/hello.c" >/dev/null 2>&1; then
    espix_fail "the U-mode apps build" "tools/build-uapp.sh failed"
    rm -rf "$UM_DIR"
    return 0
fi
dev_push "$UM_DIR/uapp" "$UAPP" >/dev/null
dev_push "$UM_DIR/hello-u" "$UHELLO" >/dev/null
dev_run "chmod +x $UAPP $UHELLO" >/dev/null

# The same hello.c as the M-mode app, linked against picolibc and libespix-u.
hello_out=$(dev_run "$UHELLO one two")
assert_contains "hello runs in U-mode"          "hello from an espix app" "$hello_out"
assert_contains "and its arguments arrive"      "argv[2] = two"           "$hello_out"

assert_contains "a U-mode app prints through write()" "hello from U-mode, argc 2" \
    "$(dev_run "$UAPP hello")"
assert_status "its exit status reaches the client" 42 dev_status "$UAPP exit 42"

# Each fault is SIGSEGV for the app alone; the next assertion is the board still answering.
assert_status   "a write to address 0 ends the app with SIGSEGV" 139 dev_status "$UAPP null"
assert_contains "and the fault is reported"  "Segmentation fault" "$(dev_run "$UAPP null")"
assert_status   "kernel memory is out of reach"                  139 dev_status "$UAPP kernel"
assert_status   "the app's own data does not execute"            139 dev_status "$UAPP exec-data"
assert_contains "the board is still up after the faults" "up " "$(dev_run uptime)"

# Preemption and the FPU in U-mode, one app and then two sharing a core.
assert_contains "floating point survives preemption" "uapp: spun" "$(dev_run "$UAPP spin 1")"
# Their own connections: dev_run's session is one shell, and two commands in it interleave.
dev_once "$UAPP spin 1" > "$UM_DIR/a" 2>&1 & um_a=$!
dev_once "$UAPP spin 1" > "$UM_DIR/b" 2>&1 & um_b=$!
wait "$um_a" "$um_b"
assert_contains "two U-mode apps run side by side (first)"  "uapp: spun" "$(cat "$UM_DIR/a")"
assert_contains "two U-mode apps run side by side (second)" "uapp: spun" "$(cat "$UM_DIR/b")"
rm -rf "$UM_DIR"

dev_run "rm $UAPP $UHELLO" >/dev/null
