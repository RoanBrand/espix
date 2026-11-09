# The screen, from apps: plasma draws and quits on a key from the viewer, and
# Doom's timedemo plays through. On the S31 the same apps run as U-mode builds,
# whose surfaces are mapped into them, unmapped when freed, and given back when
# the app ends however it ends; an audio stream is paced by its sink
# (docs/USER-MODE.md).
#
# RESOURCES: exclusive -- one app owns the screen at a time, and a timedemo is a measurement.

GFX_BIN="$ESPIX_ROOT/fsroot/bin"
if [ ! -f "$GFX_BIN/plasma" ]; then
    espix_skip "plasma not built -- run 'make apps'"
    return 0
fi

GFX_DIR=$(mktemp -d)
GFX_HOME="/home/$ESPIX_USER"
GFX_PY="${ESPIX_PYTHON:-python3}"

# Something to draw on: VNC, started for the suite if it is not running, and stopped again after.
gfx_vnc_was=$(dev_run vnc)
case $gfx_vnc_was in *stopped*) dev_run "vnc start" >/dev/null ;; esac

gfx_stage() {
    dev_push "$1" "$2" >/dev/null && dev_run "chmod +x $2" >/dev/null
}

# The pid of the running process named $1, waited for.
gfx_pid() {
    local pid="" i=0
    while [ -z "$pid" ] && [ "$i" -lt 20 ]; do
        sleep 0.5
        pid=$(dev_run ps | sed -n '1,/^finished:/p' | awk -v n="$1" '$2 == n { print $1; exit }')
        i=$((i + 1))
    done
    printf '%s' "$pid"
}

# $1 is drawing: a third of the screen or more changes in a second, in many colours. $2 a key to send after.
gfx_moving() {
    local probe moved total colours
    probe=$("$GFX_PY" "$ESPIX_ROOT/tests/lib/gfx_probe.py" "$ESPIX_HOST" ${2:-} 2>&1)
    moved=$(printf '%s\n' "$probe" | sed -n 's/^moved \([0-9]*\) of.*/\1/p')
    total=$(printf '%s\n' "$probe" | sed -n 's/^moved [0-9]* of \([0-9]*\),.*/\1/p')
    colours=$(printf '%s\n' "$probe" | sed -n 's/.*colours \([0-9]*\)$/\1/p')
    if [ -n "$moved" ] && [ "$((moved * 3))" -gt "$total" ] && [ "$colours" -gt 64 ]; then
        espix_pass "$1: it draws ($moved of $total pixels changed in a second, $colours colours)"
    else
        espix_fail "$1: it draws" "$probe"
    fi
}

# Plasma built as $1: it draws, and a q from the viewer ends it with 0.
gfx_plasma() {
    local name="$1-$ESPIX_WORKER" bg st
    gfx_stage "$GFX_BIN/$1" "$GFX_HOME/$name"
    dev_once "$GFX_HOME/$name; echo rc=\$?" > "$GFX_DIR/$1" 2>&1 &
    bg=$!
    gfx_pid "$name" >/dev/null
    sleep 1
    gfx_moving "$1" q
    wait "$bg"
    st=$(sed -n 's/^rc=//p' "$GFX_DIR/$1" | tr -d '\r')
    assert_eq "$1: a key from the viewer ends it cleanly" "0" "$st"
    dev_run "rm $GFX_HOME/$name" >/dev/null
}

# Doom built as $1, in demo3's timedemo: it draws; then SIGKILL ends it, or with $2 "whole" it plays every tic.
# SIGKILL, because an M-mode app takes other signals only where it checks for them, and this loop does not.
gfx_doom() {
    local name="$1-$ESPIX_WORKER" bg pid
    gfx_stage "$GFX_BIN/$1" "$GFX_HOME/$name"
    ESPIX_SSH_TIMEOUT=200 dev_once "cd /tmp; $GFX_HOME/$name -timedemo demo3 > /tmp/$name.log; echo rc=\$?; grep timed /tmp/$name.log; rm /tmp/$name.log" > "$GFX_DIR/$1" 2>&1 &
    bg=$!
    pid=$(gfx_pid "$name")
    sleep 8
    gfx_moving "$1"
    if [ "${2:-}" = whole ]; then
        wait "$bg"
        assert_contains "$1: Doom's timedemo plays through" "timed 2134 gametics" "$(cat "$GFX_DIR/$1")"
    else
        [ -n "$pid" ] && dev_run "kill -9 $pid" >/dev/null
        wait "$bg"
        assert_not_contains "$1: SIGKILL ends it before the demo ends" "timed 2134" "$(cat "$GFX_DIR/$1")"
    fi
    dev_run "rm $GFX_HOME/$name" >/dev/null
}

gfx_plasma plasma

# The WAD is 4 MB of an 11 MB rootfs: staged for the suite when the board has none, and taken away after.
GFX_WAD=yes GFX_WAD_STAGED=""
case $(dev_run "ls /var/lib/doom/doom1.wad") in
    *"no such"*|"")
        GFX_WAD=""
        # A new filesystem has no /var/lib/doom; the WAD counts only once it is there.
        if [ -f "$ESPIX_ROOT/apps/doom/third_party/doom1.wad" ] &&
           ESPIX_SCP_TIMEOUT=120 dev_push "$ESPIX_ROOT/apps/doom/third_party/doom1.wad" "$GFX_HOME/doom1.wad-$ESPIX_WORKER" >/dev/null; then
            dev_run "sudo mkdir -p /var/lib/doom; sudo mv $GFX_HOME/doom1.wad-$ESPIX_WORKER /var/lib/doom/doom1.wad" >/dev/null
            case $(dev_run "ls /var/lib/doom/doom1.wad") in
                *"no such"*|"") dev_run "rm $GFX_HOME/doom1.wad-$ESPIX_WORKER" >/dev/null ;;
                *) GFX_WAD=yes GFX_WAD_STAGED=yes ;;
            esac
        fi
        ;;
esac
if [ -n "$GFX_WAD" ]; then
    gfx_doom doom
else
    espix_skip "no doom1.wad for the device -- build apps/doom to fetch it"
fi

if [ "$ESPIX_TARGET" = esp32s31 ]; then
    if [ -f "$GFX_BIN/plasma-u" ]; then
        gfx_plasma plasma-u
    else
        espix_skip "plasma-u not built -- run 'make apps'"
    fi
    if [ -n "$GFX_WAD" ] && [ -f "$GFX_BIN/doom-u" ]; then
        gfx_doom doom-u whole
    fi

    UAPP="$GFX_HOME/uapp-gfx-$ESPIX_WORKER"
    if "$ESPIX_ROOT/tools/espix-cc" -Os -I"$ESPIX_ROOT/components/espix_proc/include" \
           -I"$ESPIX_ROOT/components/espix_display/include" -I"$ESPIX_ROOT/components/espix_audio/include" \
           -o "$GFX_DIR/uapp" "$ESPIX_ROOT/tests/uapp/uapp.c" >/dev/null 2>&1; then
        gfx_stage "$GFX_DIR/uapp" "$UAPP"
        out=$(dev_run "$UAPP gfx")
        assert_contains "a U-mode surface is mapped into the app" "uapp: gfx: 64x48 stride 64 presented" "$out"
        assert_contains "and freeing it unmaps it" "Segmentation fault" "$out"
        assert_not_contains "so nothing is drawn through it after" "still mapped" "$out"

        # What an app holds when it ends is given back: ended on its own, and killed.
        before=$(dev_run "grep PsramFree /proc/meminfo" | tr -dc '0-9')
        for _ in 1 2 3 4 5 6; do
            dev_run "$UAPP gfxkeep" >/dev/null
        done
        dev_once "$UAPP gfxkeep 30" > "$GFX_DIR/keep" 2>&1 &
        bg=$!
        pid=$(gfx_pid "uapp-gfx-$ESPIX_WORKER")
        [ -n "$pid" ] && dev_run "kill -9 $pid" >/dev/null
        wait "$bg"
        after=$(dev_run "grep PsramFree /proc/meminfo" | tr -dc '0-9')
        if [ -n "$before" ] && [ -n "$after" ] && [ "$((before - after))" -lt 300 ]; then
            espix_pass "surfaces and streams an app ends holding are given back ($before KB, then $after KB)"
        else
            espix_fail "surfaces and streams an app ends holding are given back" "$before KB, then $after KB"
        fi

        out=$(dev_run "$UAPP audio 1000")
        case $out in
            *"no sink"*) espix_skip "no audio sink connected" ;;
            *)
                wrote=$(printf '%s\n' "$out" | sed -n 's/.*bytes in \([0-9]*\) ms,.*/\1/p')
                queued=$(printf '%s\n' "$out" | sed -n 's/.* ms, \([0-9]*\) us queued.*/\1/p')
                if [ -n "$wrote" ] && [ -n "$queued" ] && [ "$((wrote + queued / 1000))" -ge 850 ] &&
                   [ "$((wrote + queued / 1000))" -le 1300 ]; then
                    espix_pass "a second of audio is paced by the sink ($wrote ms written, $((queued / 1000)) ms queued)"
                else
                    espix_fail "a second of audio is paced by the sink" "$out"
                fi
                ;;
        esac
        dev_run "rm $UAPP" >/dev/null
    else
        espix_fail "the U-mode probe builds" "tools/espix-cc failed"
    fi
fi

[ -n "$GFX_WAD_STAGED" ] && dev_run "sudo rm /var/lib/doom/doom1.wad" >/dev/null
case $gfx_vnc_was in *stopped*) dev_run "vnc stop" >/dev/null ;; esac
rm -rf "$GFX_DIR"
