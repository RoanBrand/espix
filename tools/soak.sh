#!/usr/bin/env bash
#
# How long can the device carry N concurrent SSH sessions before it panics?
#
#   tools/soak.sh                  4 sessions, reconnecting every 20s, 15 min cap
#   tools/soak.sh -n 2 -r 0        two sessions, never reconnecting: traffic only
#   tools/soak.sh -n 4 -r 5 -q     four sessions doing nothing but logging in
#   tools/soak.sh -t 1800          the confirmation run
#
# This is a measuring instrument, not a test suite. It answers one question with
# one number -- **time to panic** -- because that is the only currency in which
# an intermittent fault can be compared before and after a change. A green run
# proves nothing here; a distribution does.
#
# Two knobs, and the split between them is the whole point. `-r` sets how often
# a session logs in again, which is the expensive cryptographic path: a key
# exchange plus PBKDF2 at 20 000 iterations, hashing a client KEXINIT that
# arrives in a freshly allocated buffer. `-q` stops the sessions doing anything
# else. Running one against the other says whether the fault lives in the login
# path or in the data path, which no amount of reading the code has settled.
#
# bash 3.2, like everything else here.

set -u

ESPIX_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPIX_ROOT="$(cd "$ESPIX_TOOLS_DIR/.." && pwd)"
ESPIX_TEST_DIR="$ESPIX_ROOT/tests"

. "$ESPIX_TEST_DIR/lib/portable.sh"
. "$ESPIX_TEST_DIR/lib/assert.sh"
. "$ESPIX_TEST_DIR/lib/device.sh"

WORKERS=4
RECONNECT=20
CAP=900
QUIET=""

while [ $# -gt 0 ]; do
    case "$1" in
        -n) WORKERS="$2"; shift 2 ;;
        -r) RECONNECT="$2"; shift 2 ;;
        -t) CAP="$2"; shift 2 ;;
        -q) QUIET=yes; shift ;;
        --host) ESPIX_HOST="$2"; shift 2 ;;
        -h|--help) sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "soak.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

command -v ssh >/dev/null 2>&1 || { echo "soak.sh: no ssh" >&2; exit 1; }

SOAKDIR=$(espix_mktemp_dir)
WORKER_PIDS=""

cleanup() {
    : > "$SOAKDIR/stop" 2>/dev/null
    local p
    for p in $WORKER_PIDS; do
        { kill -TERM "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; } 2>/dev/null
    done
    sleep 1
    for p in $WORKER_PIDS; do
        { kill -KILL "-$p" 2>/dev/null || kill -KILL "$p" 2>/dev/null; } 2>/dev/null
    done
    dev_session_stop
    dev_askpass_cleanup
    rm -rf "$SOAKDIR"
    return 0
}
trap 'cleanup' EXIT
trap 'printf "\ninterrupted\n" >&2; exit 130' INT TERM

dev_askpass_init

# ------------------------------------------------------------------ workers ---
#
# One session each, reopened every RECONNECT seconds. Everything a worker does
# is deliberately cheap: the load being applied is *sessions and logins*, not
# throughput, and a worker that spends its time waiting on a 2MB transfer is
# applying almost none of it.
soak_worker() {     # <n>
    local n="$1" opened=0 last_open=0

    # Disown the watcher's session before doing anything else.
    #
    # A worker is a subshell, so it inherits DEV_SESSION_PID and the open
    # descriptors 8 and 9 that belong to the *watcher's* session.py. Without
    # this it decides it already has a session, never opens one of its own, and
    # three processes then interleave framed commands down one FIFO pair until
    # everything wedges -- which is exactly what the first version of this did.
    DEV_SESSION_PID=""
    DEV_SESSION_DIR=""
    DEV_SESSION_DEAD=""
    DEV_PROMPT=""
    exec 8<&- 2>/dev/null
    exec 9>&- 2>/dev/null

    while [ ! -f "$SOAKDIR/stop" ]; do
        if [ -z "$DEV_SESSION_PID" ]; then
            if dev_session_start >/dev/null 2>&1; then
                opened=$((opened + 1))
                last_open=$SECONDS
                printf '%s\n' "$opened" > "$SOAKDIR/w$n.logins"
            else
                sleep 2
                continue
            fi
        fi

        if [ -z "$QUIET" ]; then
            dev_run 'uptime' >/dev/null 2>&1
            dev_run 'ls /'   >/dev/null 2>&1
            dev_run 'id'     >/dev/null 2>&1
        else
            sleep 1
        fi

        if [ "$RECONNECT" -gt 0 ] && [ $((SECONDS - last_open)) -ge "$RECONNECT" ]; then
            dev_session_stop
        fi
    done
    dev_session_stop
    return 0
}

# ------------------------------------------------------------------ watcher ---

printf 'soak: %s sessions, ' "$WORKERS"
if [ "$RECONNECT" -gt 0 ]; then
    printf 'reconnecting every %ss, ' "$RECONNECT"
else
    printf 'no reconnects, '
fi
[ -n "$QUIET" ] && printf 'no traffic, '
printf 'cap %ss\n' "$CAP"

if ! dev_session_start; then
    echo "soak: cannot open the watcher session" >&2
    exit 1
fi
BEFORE_UP=$(_dev_parse_uptime "$(dev_run 'uptime')")
printf 'soak: device up %s min at the start\n' "$BEFORE_UP"

n=1
while [ "$n" -le "$WORKERS" ]; do
    # Own process group: soak_worker is a shell function, so `&` returns a
    # subshell pid and signalling that would leave its ssh alive holding a
    # session slot until the device reboots. See portable.sh.
    set -m
    soak_worker "$n" &
    WORKER_PIDS="$WORKER_PIDS $!"
    set +m
    n=$((n + 1))
done

T0=$SECONDS
VERDICT="clean"
LOST_AT=0

while [ $((SECONDS - T0)) -lt "$CAP" ]; do
    sleep 2
    out=$(dev_run 'uptime')
    if [ "$out" = "$DEV_DEAD" ]; then
        LOST_AT=$((SECONDS - T0))
        VERDICT="lost"
        break
    fi
    printf '\rsoak: %ss, %s\033[K' "$((SECONDS - T0))" "$out"
done
printf '\n'

: > "$SOAKDIR/stop"
for p in $WORKER_PIDS; do
    { kill -TERM "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; } 2>/dev/null
done
for p in $WORKER_PIDS; do wait "$p" 2>/dev/null; done
WORKER_PIDS=""

logins=0
for f in "$SOAKDIR"/w*.logins; do
    [ -f "$f" ] || continue
    logins=$((logins + $(cat "$f" 2>/dev/null || echo 0)))
done

# ------------------------------------------------------------------ verdict ---
#
# Losing the watcher's session is not the same as the device panicking, and the
# difference has to be established rather than assumed -- a dropped TCP
# connection and a reboot look identical from here. So reconnect and ask.

if [ "$VERDICT" = clean ]; then
    printf 'soak: CLEAN for %ss (%s logins, n=%s, reconnect %ss)\n' \
           "$CAP" "$logins" "$WORKERS" "$RECONNECT"
    exit 0
fi

dev_session_stop
i=0
while [ "$i" -lt 20 ]; do
    sleep 3
    dev_session_start >/dev/null 2>&1 && break
    i=$((i + 1))
done

if [ -z "$DEV_PROMPT" ]; then
    printf 'soak: the device stopped answering after %ss and did not come back\n' \
           "$LOST_AT"
    exit 1
fi

after_up=$(_dev_parse_uptime "$(dev_run 'uptime')")
reason=$(dev_run 'uptime' | sed -n 's/.*last reset: //p')
cores=$(dev_run 'coredump' | head -1)

if [ "$after_up" -ge 0 ] && [ "$BEFORE_UP" -ge 0 ] && [ "$after_up" -lt "$BEFORE_UP" ]; then
    printf 'soak: PANIC after %ss (%s logins, n=%s, reconnect %ss)\n' \
           "$LOST_AT" "$logins" "$WORKERS" "$RECONNECT"
    printf '      last reset: %s\n      %s\n' "$reason" "$cores"
    exit 1
fi

printf 'soak: session lost after %ss but the device did not reboot ' "$LOST_AT"
printf '(up %s min, last reset: %s) -- not a panic\n' "$after_up" "$reason"
exit 2
