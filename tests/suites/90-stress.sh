# Transport regression checks: stream integrity under volume, and connection
# reclaim when a peer goes silent. Both guard bugs that were real and are fixed.
#
# Not part of a normal run, and that is deliberate: these are slow (minutes, not
# seconds) because both faults need volume or a timeout to show themselves.
#
# Nothing in here may add logging to the firmware. The investigation into the
# first of these died exactly there -- 0/140 with per-packet tracing, 2/30
# without -- so a fix has to be proven on a build with no tracing in it, and a
# short clean run proves nothing (0/60 was recorded with the bug still present).
#
# What finally caught that bug was neither: it was the serial console at
# `dmesg -n debug`, where the failure announces itself as
# `ssh: MAC mismatch on packet N`. Worth having open while running this.
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
    espix_pass "transport failure rate ${pct}% is within the ${LIMIT}% ceiling"
else
    espix_fail "transport failure rate ${pct}% exceeds the ${LIMIT}% ceiling" \
               "corrupted: $corrupt, truncated: $short, clean: $ok, of $N" \
               "this used to be a shared tx/rx buffer; see docs/KNOWN-ISSUES.md"
fi

# ---------------------------------------------------------------------------
# A peer that stops reading and does not close must not keep a connection slot.
#
# The socket had SO_RCVTIMEO and no SO_SNDTIMEO, so a silent-but-open peer
# parked its connection task inside send() forever. Four of them exhausted
# CONFIG_ESPIX_SSH_MAX_SESSIONS and the server refused everything -- reachable
# by anyone who can open a socket to port 22.
#
# Getting the signal right took two tries, both instructive. Counting *free
# slots* is useless: only one of four is in use, so a slot is always free and the
# check can never fail. Counting `sshd:conn` tasks is nearly as bad in the other
# direction -- each poll opens its own connection and espix drains a closing one
# for up to PARTIAL_READ_TIMEOUT_MS, so the previous poller is often still
# present and the count never falls to 1 even after the peer is gone.
#
# Count `app:testapp` instead. The stalled session is the only thing running the
# app; pollers run `ps`. So it reads 1 while the peer is stranded and 0 the
# moment its connection is reclaimed, with nothing else able to muddy it.
#
# Verified to discriminate, which matters more than it sounds: on a build with
# the send timeout removed this reports STRANDED after 197s, and with it the
# connection is reclaimed in about 15s -- BLOCKED_WRITE_TIMEOUT_MS, as intended.
#
# Reclaim is not instant by design. SO_SNDTIMEO is only 250ms, but that merely
# bounds one send() call; write_all() retries and gives up after 15s of *no
# progress*, and progress resets that clock. A slow peer is tolerated, a stuck
# one is dropped. Expect longer than 15s if the client buffers generously.
# ---------------------------------------------------------------------------

RECLAIM_TIMEOUT=${ESPIX_RECLAIM_TIMEOUT:-180}
PROXY_PORT=${ESPIX_STALL_PORT:-2223}

stall_dir=$(mktemp -d 2>/dev/null || mktemp -d -t espix-stall)
marker="$stall_dir/stalled"
proxy_log="$stall_dir/proxy.log"

"${ESPIX_PYTHON:-python3}" "$ESPIX_ROOT/tests/lib/stallproxy.py" \
    --port "$PROXY_PORT" --host "$ESPIX_HOST" --stall-after 8192 \
    --marker "$marker" > "$proxy_log" 2>&1 &
proxy_pid=$!

# Wait for the listener before pointing ssh at it.
waited=0
while [ "$waited" -lt 50 ] && ! grep -q listening "$proxy_log" 2>/dev/null; do
    sleep 0.1
    waited=$((waited + 1))
done

if ! grep -q listening "$proxy_log" 2>/dev/null; then
    kill "$proxy_pid" 2>/dev/null
    espix_fail "stall proxy never started" "$(cat "$proxy_log" 2>/dev/null)"
else
    printf '  stalling a session mid-stream, then watching for the reclaim...\n'

    # Hangs by design -- the proxy stops relaying its output. Backgrounded and
    # killed at the end.
    SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS -p "$PROXY_PORT" "$ESPIX_USER@127.0.0.1" \
        "run $APP out 20000" >/dev/null 2>&1 &
    stalled_ssh=$!

    waited=0
    while [ "$waited" -lt 300 ] && [ ! -f "$marker" ]; do
        sleep 0.1
        waited=$((waited + 1))
    done

    if [ ! -f "$marker" ]; then
        espix_fail "the session never streamed enough to stall" \
                   "$(cat "$proxy_log" 2>/dev/null)" \
                   "without a stall this check measures nothing"
    else
        elapsed=0
        held=1
        while [ "$elapsed" -lt "$RECLAIM_TIMEOUT" ]; do
            sleep 5
            elapsed=$((elapsed + 5))
            tasks=$(dev_once 'ps' | grep -c 'app:testapp')
            if [ "$tasks" -eq 0 ]; then
                held=0
                break
            fi
        done

        if [ "$held" -eq 0 ]; then
            espix_pass "a silent peer's connection was reclaimed (${elapsed}s)"
        else
            espix_fail "a silent peer held its connection for ${elapsed}s" \
                       "app:testapp tasks still $tasks, expected 0" \
                       "SO_SNDTIMEO missing, or write_all no longer gives up"
        fi
    fi

    kill "$stalled_ssh" 2>/dev/null
    wait "$stalled_ssh" 2>/dev/null
fi

kill "$proxy_pid" 2>/dev/null
wait "$proxy_pid" 2>/dev/null      # reap quietly; otherwise bash prints "Terminated"
rm -rf "$stall_dir"
