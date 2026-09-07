# CPU accounting in `top` and `ps`.
#
# `top` reports an instantaneous percentage by subtracting two samples a second
# apart, and it has to pair "this task now" with "this task a second ago". It
# used to pair them by TaskHandle_t -- which is the address of the task's
# control block, freed on vTaskDelete and very often handed straight back to the
# next task created.
#
# So a connection task would spend ~390ms on its key exchange, disconnect, and
# the next connection would land at the same address with a counter of ~0. The
# subtraction wrapped in unsigned 32-bit arithmetic and `sshd:conn` was reported
# at 386491%, sorted to the top of a table ordered by CPU. Churning connections
# is exactly what `make test` does, which is when it was noticed.
#
# The invariant is simple enough to state and impossible to satisfy by accident:
# no task can use more than one core-second per second.
#
# PARALLEL_SAFE=no -- opens and closes connections as fast as it can.

if [ -z "${DEV_PROMPT:-}" ]; then
    espix_skip "no session"
    return 0
fi

# Largest CPU% in some `top`/`ps` output. The column is last and ends in '%';
# a task seen only once prints "-" and is skipped by the pattern.
max_cpu() {
    printf '%s\n' "$1" \
        | awk '/[0-9]+%$/ { gsub(/%/,"",$NF); if ($NF+0 > m) m = $NF+0 } END { print m+0 }'
}

# ---------------------------------------------------------------------------
# The quiet case first, so a failure below is about churn and not about the
# arithmetic being broken outright.
# ---------------------------------------------------------------------------

quiet=$(dev_run 'top -n 3')
assert_contains "top -n draws frames and exits" "top - up" "$quiet"

if [ "$(printf '%s\n' "$quiet" | grep -c '^top - up')" = "3" ]; then
    espix_pass "top -n 3 draws exactly three frames"
else
    espix_fail "top -n 3 draws exactly three frames" \
               "counted $(printf '%s\n' "$quiet" | grep -c '^top - up')"
fi

qmax=$(max_cpu "$quiet")
if [ "$qmax" -le 100 ]; then
    espix_pass "idle: no task above 100% (max ${qmax}%)"
else
    espix_fail "idle: no task above 100%" "max was ${qmax}%"
fi

# The first frame has nothing to subtract from, so it must not claim a busy
# figure -- it read a confident "100% busy" until `top -n 1` made that the
# whole output.
assert_contains "the first frame reports no busy figure" "Cpu:  -%" \
    "$(dev_run 'top -n 1')"

# ---------------------------------------------------------------------------
# Now the case that produced it: connections opening and closing while top
# samples, so control blocks are freed and reissued between frames.
# ---------------------------------------------------------------------------

churn() {
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        dev_status 'whoami' >/dev/null 2>&1
        dev_status 'uptime' >/dev/null 2>&1
    done
}

churn &
churn_pid=$!
churned=$(dev_run 'top -n 6')
wait "$churn_pid" 2>/dev/null

cmax=$(max_cpu "$churned")
if [ "$cmax" -le 100 ]; then
    espix_pass "under connection churn: no task above 100% (max ${cmax}%)"
else
    espix_fail "under connection churn: no task above 100%" \
               "max was ${cmax}%" \
               "a recycled task control block is being matched to a dead task"
fi

# The control for the check above: it is only meaningful if the churn actually
# reached the device while top was sampling. Several frames' worth of
# connections should leave more than one sshd:conn behind at some point.
assert_contains "control: the churn was visible to top" "sshd:conn" "$churned"

# ps uses a different formula -- a lifetime share rather than a delta -- and
# divides by a clock that used to wrap every 71.6 minutes.
pmax=$(max_cpu "$(dev_run 'ps')")
if [ "$pmax" -le 100 ]; then
    espix_pass "ps reports no task above 100% (max ${pmax}%)"
else
    espix_fail "ps reports no task above 100%" "max was ${pmax}%"
fi

# ---------------------------------------------------------------------------
# Argument handling, since `-n` means something else on macOS and a wrong
# count should say so rather than looping for ever.
# ---------------------------------------------------------------------------

assert_contains "top -n rejects a non-count" "positive count" \
    "$(dev_run 'top -n zero' 2>&1)"
assert_contains "top -n rejects zero"        "positive count" \
    "$(dev_run 'top -n 0' 2>&1)"
assert_contains "top rejects an unknown flag" "usage: top" \
    "$(dev_run 'top -z' 2>&1)"
assert_contains "top -b is accepted as a synonym" "top - up" \
    "$(dev_run 'top -b -n 1')"
