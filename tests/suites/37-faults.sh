# A fault ends the app with 128 + its signal, never the board: an M-mode app on
# both targets (components/espix_proc/app_fault.c), a U-mode one on the S31
# (docs/USER-MODE.md) when ESPIX_UAPP=1 stages that build as the test app. Each
# assertion that follows a fault is the board answering again.
#
# RESOURCES: none -- every fault is the test app's own.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

APP="$DEV_TESTAPP"
up_before=$(dev_run 'cat /proc/uptime' | awk '{ print int($1) }')

assert_status   "a write to address 0 ends the app with SIGSEGV" 139 dev_status "$APP fault null"
assert_contains "and the app's session is told" "Segmentation fault" "$(dev_run "$APP fault null")"
assert_status   "a read of an unmapped address is SIGSEGV" 139 dev_status "$APP fault read"
assert_status   "an illegal instruction is SIGILL" 132 dev_status "$APP fault ill"
exec_st=0
dev_status "$APP fault exec" || exec_st=$?
if [ "$exec_st" = 139 ] || [ "$exec_st" = 132 ]; then
    espix_pass "running the app's data ends it ($exec_st)"
else
    espix_fail "running the app's data ends it" "expected status: 139 or 132" "actual status:   $exec_st"
fi
assert_status   "a bad pointer handed to the C library ends the app" 139 dev_status "$APP fault kernel"
assert_contains "a SIGSEGV handler runs, and can jump out of the fault" "recovered from signal 11" \
    "$(dev_run "$APP fault handler")"
assert_status   "a fault in a thread ends the whole process" 139 dev_status "$APP fault thread"
if [ "$ESPIX_TARGET" = esp32s3 ]; then
    assert_status "a division by zero is SIGFPE" 136 dev_status "$APP fault div"
fi
up_after=$(dev_run 'cat /proc/uptime' | awk '{ print int($1) }')
if [ -n "$up_before" ] && [ -n "$up_after" ] && [ "$up_after" -ge "$up_before" ]; then
    espix_pass "the board ran through every fault without a reset (up ${up_before}s, then ${up_after}s)"
else
    espix_fail "the board ran through every fault without a reset" "uptime ${up_before:-?}s, then ${up_after:-?}s"
fi
