# Measure the `Corrupted MAC` failure rate.
#
# Not part of a normal run, and that is deliberate. The fault happens a few
# times in thirty, so gating on it would make `make test` intermittently red for
# a bug that is already known and open -- and an intermittently red suite gets
# ignored within a week, taking the credibility of every other assertion with
# it. This reports a *number to compare against* instead.
#
# See docs/KNOWN-ISSUES.md, which records 2/30 on main and 3/30 on the IDF 6.1
# build. A wildly different result here means this harness is wrong before it
# means the bug moved.
#
# Nothing in here may add logging to the firmware. The previous investigation
# died exactly there: 0/140 with per-packet tracing, 2/30 without. The fault
# hides under instrumentation, so it has to be measured on a build with none.
#
# PARALLEL_SAFE=no -- saturates the one transport it is measuring.

if [ "${ESPIX_STRESS:-0}" != 1 ]; then
    espix_skip "stress suites need --stress (or 'make stress')"
    return 0
fi

if ! dev_testapp_sync "$ESPIX_ROOT/fsroot/home/$ESPIX_USER/testapp"; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

N=${ESPIX_STRESS_N:-30}

# 2000 lines, and deliberately far above where this used to break.
#
# This suite's first job was to characterise a transport bug: the stream was
# clean below a threshold and fell apart above it -- 0 bad in 20 at each of 8,
# 25, 50 and 100 lines, and 26 bad in 30 at 200. A cliff, not a slope. While
# that was unfixed the default sat *below* the cliff at 100, so that any failure
# meant a new regression rather than the known bug reappearing.
#
# The bug is fixed (a buffer shared between the send and receive paths; see
# docs/KNOWN-ISSUES.md), so the default now sits where the fault used to be
# reliable -- it failed 6 runs in 8 at this volume. That makes this the
# regression guard for it: anything above 0% here means it is back.
LINES=${ESPIX_STRESS_LINES:-2000}
APP="/home/$ESPIX_USER/testapp"

# Zero, and it should stay zero: 77 consecutive clean runs above the old cliff
# were measured after the fix (30 at 2000 lines, 20 more at 2000, 15 at 5000,
# and 12 across concurrent sessions). Raise it only to measure a known-bad
# build on purpose:
#     make stress N=30                 regression check, expects 0%
#     ./tests/run.sh --suite stress --stress --stress-lines 5000
LIMIT=${ESPIX_STRESS_LIMIT:-0}

printf '  running %d iterations of `run testapp out %d`...\n' "$N" "$LINES"

corrupt=0
short=0
ok=0
i=1
while [ "$i" -le "$N" ]; do
    out=$(dev_once "run $APP out $LINES")

    case "$out" in
        *"Corrupted MAC"*|*"message authentication code incorrect"*|\
        *"dispatch_protocol_error"*|*"Connection closed"*)
            corrupt=$((corrupt + 1)) ;;
        *)
            # Success is not "ssh exited zero": the original report was output
            # being *truncated*, which was noticed long before the MAC failures
            # underneath it were. So count the lines and check the last one.
            got=$(printf '%s\n' "$out" | grep -c '^line ')
            if [ "$got" -ne "$LINES" ]; then
                short=$((short + 1))
            else
                case "$out" in
                    *"line $LINES of $LINES"*) ok=$((ok + 1)) ;;
                    *) short=$((short + 1)) ;;
                esac
            fi
            ;;
    esac
    i=$((i + 1))
done

pct=$(( (corrupt + short) * 100 / N ))
printf '  corrupted: %d of %d, truncated: %d of %d, clean: %d (%d%% bad)\n' \
       "$corrupt" "$N" "$short" "$N" "$ok" "$pct"

if [ "$pct" -le "$LIMIT" ]; then
    espix_pass "transport failure rate ${pct}% is within the known ${LIMIT}% ceiling"
else
    espix_fail "transport failure rate ${pct}% exceeds the known ${LIMIT}% ceiling" \
               "corrupted: $corrupt, truncated: $short, clean: $ok, of $N" \
               "KNOWN-ISSUES records 2/30 and 3/30 -- this is materially worse"
fi
