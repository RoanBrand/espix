#!/usr/bin/env bash
#
# Capture the serial console to a timestamped log, and keep capturing.
#
#   tools/serlog.sh [port] [logfile]     start (backgrounds itself)
#   tools/serlog.sh stop [logfile]       stop the one this started
#   tools/serlog.sh status [logfile]     is it running, and who holds the port
#
# WHY THIS EXISTS, and it is not "so we have logs".
#
# When the ESP32-S3 takes a cache error, the panic handler calls
# esp_cache_err_get_panic_info() and prints which of seven distinct faults
# fired -- "Dcache sync parameter configuration error", "Write back error
# occurred while dcache tries to write back to flash", "MMU entry fault error",
# and so on, with "Cache disabled but cached memory region accessed" only as the
# fallback when no status bit is set. Those are different bugs with different
# fixes.
#
# That string goes to this UART and **nowhere else**. It is not in the core
# dump: espcoredump's PANIC_DETAILS note is only ever filled for watchdog
# panics (elf_add_wdt_panic_details in core_dump_elf.c). Afterwards the dump
# gives you exccause 71 -- PANIC_RSN_CACHEERR plus XCHAL_EXCCAUSE_NUM -- and
# every one of the seven looks identical. So a cache-error panic caught without
# this log costs another day of reproducing it; caught with it, the cause is on
# screen.
#
# HOW TO USE IT WHEN HUNTING A PANIC
#
#   tools/serlog.sh                          # takes the port, backgrounds
#   ./tests/run.sh -j 4                      # note: NO --port
#   ssh esp@<host> top                       # in another terminal; see below
#   tools/serlog.sh stop                     # before the next flash
#
# Run the suite *without* --port. run.sh then sets ESPIX_HAVE_SERIAL=no and the
# console suites skip, leaving this the only reader. That matters: macOS does
# not give exclusive access to a cu.* device, so two readers simply race and
# split the bytes between them.
#
# The `top` session is a reproduction lever, not decoration. A busy SSH session
# keeps HMAC-SHA running continuously over the connection buffers in PSRAM, and
# esp_cache_msync() on those buffers is the call that faults. An idle extra
# session does not do this; one redrawing a screenful per frame does.
#
# This never writes to the port -- no reset, no keystrokes. The board being
# watched has to be the board that would have run anyway.
#
# THREE THINGS IT DOES THAT THE FIRST VERSION DID NOT, each paid for:
#
#  1. A pidfile and a `stop`. The first version ended in `exec "$PY" ...`, so
#     the running process's command line was the interpreter and a heredoc --
#     the string "serlog" was gone from it, and `pkill -f serlog` silently did
#     nothing. It survived being "stopped" three times, and one survivor later
#     broke an `idf.py flash` outright.
#
#  2. It refuses to start when something already holds the port, and says what.
#     Two readers on one cu.* device split the byte stream, so a second copy
#     does not just waste effort -- it corrupts both captures.
#
#  3. A heartbeat every HEARTBEAT_S of silence. Without it a dead capture and an
#     idle board are indistinguishable: the log simply stops. That cost three
#     wrong readings in one session, including "the device is emitting nothing"
#     about a board that was answering commands normally at the time. A log that
#     says `# quiet` once a minute cannot make that mistake.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ACTION=start
case "${1:-}" in
    stop|status) ACTION="$1"; shift ;;
esac

PORT="${1:-}"
LOG="${2:-serial.log}"

# `stop`/`status` are addressed by logfile, since that is what identifies a
# capture -- the port may already have been taken by something else.
if [ "$ACTION" != start ]; then
    LOG="${1:-serial.log}"
fi
PIDFILE="$LOG.pid"

port_holder() {
    command -v lsof >/dev/null 2>&1 || return 1
    lsof "$1" 2>/dev/null | awk 'NR>1 {print $1" (pid "$2")"; exit}'
}

case "$ACTION" in
stop)
    if [ ! -f "$PIDFILE" ]; then
        echo "serlog: no pidfile at $PIDFILE" >&2
        exit 1
    fi
    pid=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        # It is reading a serial port with a timeout, so it wakes and exits
        # promptly; escalate only if it does not.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
        echo "serlog: stopped $pid"
    else
        echo "serlog: pid $pid is not running (stale pidfile)"
    fi
    rm -f "$PIDFILE"
    exit 0
    ;;
status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        echo "serlog: running as pid $(cat "$PIDFILE") -> $LOG"
    else
        echo "serlog: not running"
    fi
    exit 0
    ;;
esac

if [ -z "$PORT" ]; then
    PORT=$("$here/port.sh" 2>/dev/null || true)
    [ -n "$PORT" ] || { echo "serlog: no serial port found; pass one" >&2; exit 2; }
fi

# Refuse rather than race. See note 2 above.
holder=$(port_holder "$PORT")
if [ -n "$holder" ]; then
    echo "serlog: $PORT is already held by $holder" >&2
    echo "serlog: two readers split the byte stream; stop that one first" >&2
    echo "serlog:   tools/serlog.sh stop [logfile]   if it is a previous capture" >&2
    exit 3
fi

# The IDF virtualenv has pyserial; the system python usually does not.
if [ -x "$here/idf.sh" ]; then
    eval "$(bash "$here/idf.sh" --env 2>/dev/null)" 2>/dev/null || true
fi
PY="${ESPIX_PYTHON:-python3}"

"$PY" -c 'import serial' 2>/dev/null \
    || { echo "serlog: $PY has no pyserial" >&2; exit 2; }

# Backgrounded here rather than by the caller, so the pidfile is written by the
# thing that knows the pid. Not `exec`: the shell stays out of the way but the
# child keeps a command line with "serlog" in it, so pkill works as anyone would
# expect even though `stop` no longer needs it to.
"$PY" "$here/serlog.py" "$PORT" "$LOG" &
child=$!
echo "$child" > "$PIDFILE"

echo "serlog: $PORT -> $LOG  (pid $child; tools/serlog.sh stop $LOG)" >&2
