# The broker apps/mosquitto builds, as an app on sockets: a client's message
# comes back through it, and SIGTERM ends it in its poll. The M-mode build on
# both targets; on the S31 the U-mode one too (docs/USER-MODE.md).
#
# RESOURCES: a TCP port per worker, 18830 + the worker number.

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

# The broker $1 in the background, a client's round trip from here, then SIGTERM; $2 names it.
mq_check() {
    local bin="/home/$ESPIX_USER/$1-$ESPIX_WORKER" pid got st
    dev_push "$MQ_BIN/$1" "$bin" >/dev/null
    dev_run "chmod +x $bin" >/dev/null
    pid=$(dev_run "$bin -c $MQ_CONF &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
    got=""
    for _ in 1 2 3 4 5; do
        sleep 1
        got=$("${ESPIX_PYTHON:-python3}" "$ESPIX_ROOT/tests/lib/mqtt_probe.py" "$ESPIX_HOST" "$MQ_PORT" "hello" 2>&1)
        case $got in got*) break ;; esac
    done
    assert_eq "$2: a client's message comes back through it" "got hello" "$got"
    dev_run "kill $pid" >/dev/null
    sleep 1
    st=$(dev_run ps | awk -v p="$pid" '$1 == p { print $NF }')
    assert_eq "$2: SIGTERM ends it in its poll, cleanly" "0" "$st"
    dev_run "kill -9 $pid; rm $bin" >/dev/null 2>&1
}

mq_check mosquitto "the M-mode broker"
if [ "$ESPIX_TARGET" = esp32s31 ]; then
    if [ -f "$MQ_BIN/mosquitto-u" ]; then
        mq_check mosquitto-u "the U-mode broker"
    else
        espix_skip "mosquitto-u not built -- run 'make apps'"
    fi
fi

dev_run "rm $MQ_CONF" >/dev/null
rm -rf "$MQ_DIR"
