# Services: a unit runs unattended and outlives the login that started it.
#
# RESOURCES: none -- it rewrites /etc/units and puts the operator's file back
# afterwards, and only ever looks at its own unit by name. A unit's output goes
# to the klog, not to this session.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

APP="/home/$ESPIX_USER/testapp"
UNIT=/tmp/svc-units-$ESPIX_WORKER
# The operator's file, kept so it can be put back: this suite rewrites
# /etc/units, and on a board somebody is using that file is theirs. It used to
# be emptied on the way out, which destroyed whatever was there.
KEEP=/tmp/svc-keep-$ESPIX_WORKER

dev_run "sudo cp /etc/units $KEEP" >/dev/null 2>&1

# One always-unit, running the test app as a daemon. The file is built in /tmp
# and copied into place as root: the redirection belongs to this session, so
# sudo cannot raise it. The reload is root's as well -- the file is 0600 root,
# which is also what makes reading it the privileged half of the command.
dev_run "echo svcprobe always $APP hold 300 1000 > $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1

# A reload sets the intent; the supervisor acts on it on its next pass, up to a
# second later. Asking for the state straight afterwards is a race, and the
# answer is exactly what the assertions below are about -- so wait for it rather
# than sleeping a guessed amount and hoping.
svc_wait_for_state() {                # <name> <state> [tries]
    local name="$1" want="$2" tries="${3:-10}" seen=""
    while [ "$tries" -gt 0 ]; do
        seen=$(dev_run 'service')
        case "$seen" in
            *"$name"*"$want"*) printf '%s' "$seen"; return 0 ;;
        esac
        sleep 1
        tries=$((tries - 1))
    done
    printf '%s' "$seen"
    return 1
}

assert_status "the units file reloads" 0 dev_status 'sudo service reload'

out=$(svc_wait_for_state svcprobe running)
assert_contains "a unit is listed" "svcprobe" "$out"
assert_contains "and it is running" "running" "$out"

# The point of the whole thing: a unit is a process, not a job, so nothing
# about its life belongs to the session that started it. A different login
# sees it.
saw=$(dev_once 'service | grep -c svcprobe')
saw=$(printf '%s' "$saw" | tr -d '[:space:]')
assert_eq "a different login sees the unit" "1" "$saw"

# Restart policy: an always unit comes back after it is killed.
pid=$(printf '%s' "$out" | awk '/svcprobe/ {print $2; exit}')
if [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null; then
    dev_run "kill -9 $pid" >/dev/null 2>&1
    # The supervisor reaps it, applies the policy and starts it again on one
    # pass, so this is a wait rather than a sleep.
    restarted=$(svc_wait_for_state svcprobe running |
                awk '/svcprobe/ {print $4; exit}')
    assert_eq "an always unit is restarted" "running" "$restarted"
else
    espix_fail "an always unit is restarted" "no pid in: $out"
fi

# stop means stop, and it stays stopped.
dev_run 'service stop svcprobe' >/dev/null 2>&1
stopped=$(svc_wait_for_state svcprobe stopped |
          awk '/svcprobe/ {print $4; exit}')
assert_eq "a stopped unit stays stopped" "stopped" "$stopped"

# A once unit runs and is not brought back.
dev_run "echo onceprobe once $APP exit 0 > $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
# A oneshot that ran is "done", not "stopped": it exited and its effect did not
# (svc: a oneshot that ran is done, not stopped). Waiting for it to have run and
# then leaving it alone for a beat is what shows it is not brought back -- a
# second run would put it back to running.
once=$(svc_wait_for_state onceprobe done | awk '/onceprobe/ {print $4; exit}')
assert_eq "a once unit runs and is done" "done" "$once"
sleep 3
still=$(dev_run 'service' | awk '/onceprobe/ {print $4; exit}')
assert_eq "and it is not restarted" "done" "$still"

# Put the operator's file back, so nothing of this suite outlives it -- neither
# a test unit nor an emptied file.
dev_run "sudo cp $KEEP /etc/units" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
sleep 1
