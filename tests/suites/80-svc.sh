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
# be emptied on the way out, which destroyed whatever was there. Kept in the
# home, not /tmp, which make flash-ota empties; and one still there is from a
# run that never got to the end, so it is the operator's and goes back first.
KEEP=/home/$ESPIX_USER/.svc-keep-$ESPIX_WORKER

if dev_status "ls $KEEP"; then
    dev_run "sudo cp $KEEP /etc/units" >/dev/null 2>&1
fi
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

# sshd is a unit too. A file that does not name it gets it back, or a reload of
# this suite's file would have closed the way in; and a login is its child.
assert_contains "a units file without sshd gets it back" "sshd  always  sshd" \
    "$(dev_run 'sudo cat /etc/units')"
spid=$(printf '%s' "$out" | awk '$1 == "sshd" && $4 == "running" {print $2; exit}')
if [ -n "$spid" ] && [ "$spid" -gt 0 ] 2>/dev/null; then
    espix_pass "sshd is a running unit ($spid)"
    me=$(dev_run 'echo $$')
    assert_eq "and this login is its child" "$spid" \
        "$(dev_run ps | awk -v p="$me" 'NR == 1 { for (i = 1; i <= NF; i++)
            if ($i == "PPID") c = i } $1 == p { print $c; exit }')"
else
    espix_fail "sshd is a running unit" "$out"
fi

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

# A builtin unit is a process too: a pid, a row in ps, and the same stop. It
# used to run on a bare task with none of those, so a reload that dropped its
# line left it running with nothing tracking it.
dev_run "echo sleepunit always sleep 600 > $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
out=$(svc_wait_for_state sleepunit running)
bpid=$(printf '%s' "$out" | awk '/sleepunit/ {print $2; exit}')
if [ -n "$bpid" ] && [ "$bpid" -gt 0 ] 2>/dev/null; then
    espix_pass "a builtin unit has a pid ($bpid)"
    assert_eq "and ps lists it under that pid" "sleepunit" \
        "$(dev_run 'ps' | awk -v p="$bpid" '$1 == p {print $2; exit}')"

    # SIGKILL asks espix's own code to stop rather than deleting it; the unit
    # ends itself, and being an always unit, comes back under a new pid.
    dev_run "kill -9 $bpid" >/dev/null 2>&1
    again=""
    for i in 1 2 3 4 5 6 7 8 9 10; do
        again=$(dev_run 'service' | awk '/sleepunit/ && $4 == "running" {print $2; exit}')
        [ -n "$again" ] && [ "$again" != "$bpid" ] && break
        sleep 1
    done
    if [ -n "$again" ] && [ "$again" != "$bpid" ]; then
        espix_pass "kill -9 stops a builtin unit, and always brings it back"
    else
        espix_fail "kill -9 stops a builtin unit, and always brings it back" \
            "$(dev_run 'service')"
    fi
    assert_not_contains "and it was asked, not deleted" "killed pid $bpid " \
        "$(dev_run 'dmesg') "

    # Dropping its line from the file ends it.
    dev_run "echo onceprobe once $APP exit 0 > $UNIT" >/dev/null 2>&1
    dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
    dev_run 'sudo service reload' >/dev/null 2>&1
    gone=""
    for i in 1 2 3 4 5; do
        gone=$(dev_run 'ps' | sed -n '1,/^finished:/p' | awk '$2 == "sleepunit"')
        [ -z "$gone" ] && break
        sleep 1
    done
    assert_eq "a reload that drops a builtin unit stops it" "" "$gone"
else
    espix_fail "a builtin unit has a pid" "no pid in: $out"
fi

# success=: exit statuses besides 0 that mean a unit worked, as systemd's
# SuccessExitStatus. upgrade --check exits 100 when there is an update.
dev_run "echo okcode once success=100 $APP exit 100 > $UNIT" >/dev/null 2>&1
dev_run "echo badcode once $APP exit 100 >> $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
out=$(svc_wait_for_state badcode failed)
assert_eq "a listed exit status is success" "done" \
    "$(printf '%s' "$out" | awk '$1 == "okcode" {print $4}')"
assert_eq "and an unlisted one is not" "failed" \
    "$(printf '%s' "$out" | awk '$1 == "badcode" {print $4}')"

# requires=: a unit starts only once what it requires is up. A once unit is up
# when it has exited successfully, so second waits out first's three seconds.
dev_run "echo first once $APP sleep 3 > $UNIT" >/dev/null 2>&1
dev_run "echo second once requires=first $APP exit 0 >> $UNIT" >/dev/null 2>&1
dev_run "echo bad once $APP exit 1 >> $UNIT" >/dev/null 2>&1
dev_run "echo blocked once requires=bad $APP exit 0 >> $UNIT" >/dev/null 2>&1
dev_run "echo orphan once requires=nosuch $APP exit 0 >> $UNIT" >/dev/null 2>&1
dev_run "sudo cp $UNIT /etc/units" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
out=$(svc_wait_for_state first running)
assert_eq "a unit waits while what it requires runs" "waiting" \
    "$(printf '%s' "$out" | awk '$1 == "second" {print $4}')"
out=$(svc_wait_for_state second done)
assert_eq "and starts once it has succeeded" "done" \
    "$(printf '%s' "$out" | awk '$1 == "second" {print $4}')"
assert_eq "a unit whose requirement failed does not start" "waiting" \
    "$(printf '%s' "$out" | awk '$1 == "blocked" {print $4}')"
assert_eq "nor one that requires a unit that does not exist" "waiting" \
    "$(printf '%s' "$out" | awk '$1 == "orphan" {print $4}')"
assert_contains "and the klog says why" "blocked: waiting for bad, which failed" \
    "$(dev_run 'dmesg')"

# Put the operator's file back, so nothing of this suite outlives it -- neither
# a test unit nor an emptied file.
dev_run "sudo cp $KEEP /etc/units" >/dev/null 2>&1
dev_run "sudo rm $KEEP" >/dev/null 2>&1
dev_run 'sudo service reload' >/dev/null 2>&1
sleep 1
