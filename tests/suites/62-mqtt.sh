# The broker apps/mosquitto builds, as an app on sockets: a client's message
# comes back through it, and SIGTERM ends it in its poll. The M-mode build and the
# POSIX one (mosquitto-u), on both targets (docs/USER-MODE.md).
#
# RESOURCES: none -- a TCP port per worker, 18830 + the worker number.

# The kernel's suites reach it through the shell and the test app; this one runs a program
# from apps/, which holds memory and the screen the others would then contend for.
if [ "${ESPIX_APPS:-0}" != 1 ]; then
    espix_skip "app suites need --apps (or 'make test-apps')"
    return 0
fi

MQ_BIN="$ESPIX_ROOT/fsroot/bin"
if [ ! -f "$MQ_BIN/mosquitto" ]; then
    espix_skip "mosquitto not built -- run 'make apps'"
    return 0
fi

MQ_PORT=$((18830 + ${ESPIX_WORKER:-0}))
MQ_CONF="/home/$ESPIX_USER/mq-$ESPIX_WORKER.conf"
MQ_DIR=$(mktemp -d)
printf 'listener %s 0.0.0.0\nallow_anonymous true\n' "$MQ_PORT" > "$MQ_DIR/mq.conf"
dev_push "$MQ_DIR/mq.conf" "$MQ_CONF" >/dev/null

# The broker $1 in an exec of its own, a client's round trip from here, then SIGTERM from the
# suite's session; $2 names it. Its status is its shell's: ps keeps only the last 8 that ended.
mq_check() {
    local bin="/home/$ESPIX_USER/$1-$ESPIX_WORKER" pid="" got st bg i=0
    dev_push "$MQ_BIN/$1" "$bin" >/dev/null
    dev_run "chmod +x $bin" >/dev/null
    # Room under load for the start, the probe and the kill: a cut session would lose rc.
    ESPIX_SSH_TIMEOUT=90 dev_once "$bin -c $MQ_CONF; echo rc=\$?" > "$MQ_DIR/run" 2>&1 &
    bg=$!
    while [ -z "$pid" ] && [ "$i" -lt 40 ]; do
        sleep 0.5
        pid=$(dev_run ps | sed -n '1,/^finished:/p' | awk -v n="$1-$ESPIX_WORKER" '$2 == n { print $1; exit }')
        i=$((i + 1))
    done
    got=""
    for _ in 1 2 3 4 5; do
        sleep 1
        got=$("${ESPIX_PYTHON:-python3}" "$ESPIX_ROOT/tests/lib/mqtt_probe.py" "$ESPIX_HOST" "$MQ_PORT" "hello" 2>&1)
        case $got in got*) break ;; esac
    done
    assert_eq "$2: a client's message comes back through it" "got hello" "$got"
    if [ -n "$pid" ]; then
        dev_run "kill $pid" >/dev/null
    else
        kill "$bg" 2>/dev/null
    fi
    wait "$bg"
    st=$(sed -n 's/^rc=//p' "$MQ_DIR/run" | tr -d '\r')
    assert_eq "$2: SIGTERM ends it in its poll, cleanly" "0" "$st"
    dev_run "rm $bin" >/dev/null 2>&1
}

# mosquitto -d: the broker forks, its parent exits 0 at once, and the child serves on in a session of its own.
mq_daemon() {
    local bin="/home/$ESPIX_USER/$1-$ESPIX_WORKER" out row pid ppid got
    dev_push "$MQ_BIN/$1" "$bin" >/dev/null
    dev_run "chmod +x $bin" >/dev/null
    out=$(dev_run "$bin -d -c $MQ_CONF; echo rc=\$?")
    assert_contains "$2: -d returns at once, with 0" "rc=0" "$out"
    row=$(dev_run ps | sed -n '1,/^finished:/p' | awk -v n="$1-$ESPIX_WORKER" '$2 == n { print $1, $8; exit }')
    pid=${row%% *}
    ppid=${row##* }
    assert_eq "$2: the daemon is a child of init" "1" "$ppid"
    got=""
    for _ in 1 2 3 4 5; do
        got=$("${ESPIX_PYTHON:-python3}" "$ESPIX_ROOT/tests/lib/mqtt_probe.py" "$ESPIX_HOST" "$MQ_PORT" "hello" 2>&1)
        case $got in got*) break ;; esac
        sleep 1
    done
    assert_eq "$2: and serves after the session that started it ended" "got hello" "$got"
    if [ -n "$pid" ]; then
        dev_run "kill $pid" >/dev/null
        sleep 1
    fi
    assert_eq "$2: SIGTERM ends the daemon" "" \
        "$(dev_run ps | sed -n '1,/^finished:/p' | awk -v n="$1-$ESPIX_WORKER" '$2 == n { print $1 }')"
    dev_run "rm $bin" >/dev/null 2>&1
}

mq_check mosquitto "the M-mode broker"
# The POSIX build: U-mode on the S31; on the S3 without an MMU, where -d needs the fork it has not.
if [ -f "$MQ_BIN/mosquitto-u" ]; then
    mq_check mosquitto-u "the POSIX broker"
    if [ "$ESPIX_TARGET" = esp32s31 ]; then
        mq_daemon mosquitto-u "the POSIX broker daemonised"
    fi
else
    espix_skip "mosquitto-u not built -- run 'make apps'"
fi

dev_run "rm $MQ_CONF" >/dev/null
rm -rf "$MQ_DIR"
