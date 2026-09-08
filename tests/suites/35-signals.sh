# Signals: delivery, handlers, and the escalation to SIGKILL.
#
# Every claim about signals in docs/KNOWN-ISSUES.md was hand-tested until now.
# The fixture for it lived in apps/sigtest -- in the *examples* directory, so
# tools/build-apps.sh built it on every firmware build and shipped it in every
# rootfs image, while apps/README.md's table never mentioned it. It is a lever
# for this suite, so it moved into the test app as `testapp sig`.
#
# Assertions are on `ps` rather than on the app's own output. A backgrounded
# process writes to the session asynchronously, so its lines arrive inside
# whichever framed command happens to be in flight -- unassertable. Process
# state is not.
#
# RESOURCES: none -- every assertion here is scoped by pid, including the
# dmesg ones, so another suite's processes are invisible to it.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

APP="/home/$ESPIX_USER/testapp"

# `ps` prints running tasks, then a `finished:` section -- but that section
# shows at most 8 entries (see KNOWN-ISSUES: cmd_ps stack-allocates procs[8]),
# and by the time this suite runs the earlier ones have filled it. So how a
# process *ended* is read from dmesg, which is a ring rather than a fixed
# array, and whose wording is the exact discriminator wanted here:
#
#   "pid N stopped on request"      the handler ran and the app returned
#   "pid N did not stop when asked" the grace expired
#   "killed pid N"                  the task was deleted, so no cleanup ran
ps_running() { dev_run 'ps' | sed -n '1,/^finished:/p'; }

# Start `testapp sig <mode>` in the background and echo its pid, which the
# shell reports as `[13] /home/esp/testapp`.
bg_start() {
    dev_run "$APP sig $1 &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p'
}

is_running() {
    case "$(ps_running)" in
        *"$1 app:testapp"*) return 0 ;;
        *)                  return 1 ;;
    esac
}

# Wait for a pid to leave the running list. Bounded: a process that never goes
# is the failure, and waiting forever reports it as a hang instead.
wait_gone() {
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        is_running "$1" || return 0
        sleep 1
    done
    return 1
}

# ---------------------------------------------------------------------------
# A handled signal: the app runs its cleanup and returns.
#
# This is also the control for everything below, and it has to come first --
# the later checks say what happens when a signal is *not* handled, and a
# `kill` that did nothing at all would satisfy those just as well.
# ---------------------------------------------------------------------------

pid=$(bg_start handlers)
if [ -z "$pid" ]; then
    espix_fail "backgrounding an app reports its pid" "no [pid] line from the shell"
    return 0
fi
espix_pass "backgrounding an app reports its pid ($pid)"

assert_eq "and it is running" "yes" "$(is_running "$pid" && echo yes || echo no)"

dev_run "kill $pid" >/dev/null
if wait_gone "$pid"; then
    espix_pass "control: kill ends a process that handles SIGTERM"
else
    espix_fail "control: kill ends a process that handles SIGTERM" \
               "pid $pid still running 10s later" \
               "nothing below this line means anything if kill does not work"
    return 0
fi

# Stopped on request, not deleted. That difference is the whole point of espix
# having signals at all: the handler ran, `teardown` equivalents got to run,
# and main() returned.
assert_contains "and it stopped on request rather than being deleted" \
    "pid $pid stopped on request" "$(dev_run 'dmesg')"

# ---------------------------------------------------------------------------
# SIGUSR1 is survivable: the handler runs, returns, and the sleep it
# interrupted resumes. A process that treated every signal as fatal would fail
# this, and so would delivery that never happened.
# ---------------------------------------------------------------------------

pid=$(bg_start handlers)
dev_run "kill -USR1 $pid" >/dev/null
sleep 2
assert_eq "SIGUSR1 is handled and the process carries on" "yes" \
    "$(is_running "$pid" && echo yes || echo no)"

dev_run "kill $pid" >/dev/null
wait_gone "$pid" || espix_fail "cleanup: pid $pid would not stop"

# ---------------------------------------------------------------------------
# Ignoring the polite signals.
#
# espix's `kill` is not POSIX's: espix_proc_stop() sends SIGTERM, waits
# TERM_GRACE_MS, and deletes the task if it is still there. So SIG_IGN buys an
# app the grace period and not survival -- `kill -TERM` on Unix would leave it
# running for ever.
#
# Deliberate (an embedded system that cannot reclaim a task has a worse
# problem than a rude shutdown), and worth pinning precisely because it is a
# divergence someone will otherwise trip over. What is asserted is the
# *mechanism*: the grace expired, and then the task was deleted -- which is
# also why no cleanup ran.
# ---------------------------------------------------------------------------

pid=$(bg_start ignore)
sleep 1
dev_run "kill $pid" >/dev/null
if wait_gone "$pid"; then
    espix_pass "a process ignoring SIGTERM is stopped anyway, after the grace"
else
    espix_fail "a process ignoring SIGTERM is stopped anyway, after the grace" \
               "pid $pid still running"
    dev_run "kill -9 $pid" >/dev/null
fi

log=$(dev_run 'dmesg')
assert_contains "the grace expired before it was deleted" \
    "pid $pid did not stop when asked" "$log"
assert_contains "and then the task was deleted, so no cleanup ran" \
    "killed pid $pid" "$log"

# SIGKILL takes the same route with no grace at all.
pid=$(bg_start ignore)
sleep 1
dev_run "kill -9 $pid" >/dev/null
if wait_gone "$pid"; then
    espix_pass "kill -9 needs no grace"
else
    espix_fail "kill -9 needs no grace" "pid $pid survived SIGKILL"
fi
assert_not_contains "and does not ask first" "pid $pid did not stop when asked" \
    "$(dev_run 'dmesg')"

# ---------------------------------------------------------------------------
# A compute loop, and the delivery point it needs.
#
# espix delivers signals where a process calls into it, so a loop that blocks
# on nothing has no delivery point -- which is what espix_sigcheck() is
# exported for. `sig spin` calls it, so `kill` reaches it; the documented
# hazard is the loop that does not, and there is nothing to assert about a
# process that cannot be stopped except that it cannot.
# ---------------------------------------------------------------------------

pid=$(bg_start spin)
sleep 1
dev_run "kill $pid" >/dev/null
if wait_gone "$pid"; then
    espix_pass "a compute loop calling espix_sigcheck() can be stopped"
else
    espix_fail "a compute loop calling espix_sigcheck() can be stopped" \
               "pid $pid still spinning"
    dev_run "kill -9 $pid" >/dev/null
fi

# On request, not deleted -- so espix_sigcheck() really was the delivery point
# and the grace never had to expire.
assert_contains "and it stops on request, not by deletion" \
    "pid $pid stopped on request" "$(dev_run 'dmesg')"

# ---------------------------------------------------------------------------
# `kill -l`, and the names the numeric forms have to agree with.
# ---------------------------------------------------------------------------

sigs=$(dev_run 'kill -l')
assert_contains "kill -l names SIGTERM" "TERM" "$sigs"
assert_contains "kill -l names SIGKILL" "KILL" "$sigs"

assert_contains "kill rejects a pid that is not there" "no such process" \
    "$(dev_run 'kill 9999' 2>&1)"
