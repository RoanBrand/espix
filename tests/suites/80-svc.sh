# Services: a unit runs unattended and outlives the login that started it.
#
# RESOURCES: none -- it rewrites /etc/units at the end and only ever looks at
# its own unit by name. A unit's output goes to the klog, not to this session.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

APP="/home/$ESPIX_USER/testapp"
UNIT=/tmp/svc-units-$ESPIX_WORKER

# One always-unit, running the test app as a daemon. The file is built in /tmp
# and copied into place as root: the redirection belongs to this session, so
# sudo cannot raise it.
dev_run "echo svcprobe always $APP hold 300 1000 > $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1

assert_status "the units file reloads" 0 dev_status 'service reload'

out=$(dev_run 'service')
assert_contains "a unit is listed" "svcprobe" "$out"
assert_contains "and it is running" "running" "$out"

# The point of the whole thing: a unit is a process, not a job, so nothing
# about its life belongs to the session that started it. A different login
# sees it.
saw=$(dev_once 'service | grep -c svcprobe')
assert_eq "a different login sees the unit" "1" "$saw"

# Restart policy: an always unit comes back after it is killed.
pid=$(printf '%s' "$out" | awk '/svcprobe/ {print $2; exit}')
if [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null; then
    dev_run "kill -9 $pid" >/dev/null 2>&1
    sleep 3
    restarted=$(dev_run 'service' | awk '/svcprobe/ {print $4; exit}')
    assert_eq "an always unit is restarted" "running" "$restarted"
else
    espix_fail "an always unit is restarted" "no pid in: $out"
fi

# stop means stop, and it stays stopped.
dev_run 'service stop svcprobe' >/dev/null 2>&1
sleep 2
stopped=$(dev_run 'service' | awk '/svcprobe/ {print $4; exit}')
assert_eq "a stopped unit stays stopped" "stopped" "$stopped"

# A once unit runs and is not brought back.
dev_run "echo onceprobe once $APP exit 0 > $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
dev_run 'service reload' >/dev/null 2>&1
sleep 3
once=$(dev_run 'service' | awk '/onceprobe/ {print $4; exit}')
assert_eq "a once unit is not restarted" "stopped" "$once"

# Empty the units file again, so no unit outlives this suite.
dev_run "sudo cp /dev/null /etc/units" >/dev/null 2>&1
dev_run 'service reload' >/dev/null 2>&1
