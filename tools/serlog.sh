#!/usr/bin/env bash
#
# Capture the serial console to a timestamped log, and keep capturing.
#
#   tools/serlog.sh [port] [logfile]
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
#   tools/serlog.sh &                        # takes the port
#   ./tests/run.sh -j4                       # note: NO --port
#   ssh esp@<host> top                       # in another terminal; see below
#
# Run the suite *without* --port. run.sh then sets ESPIX_HAVE_SERIAL=no and the
# console suites skip, leaving this the only reader. That matters: macOS does
# not give exclusive access to a cu.* device, so two readers simply race and
# split the bytes between them. The first version of this died mid-run with
# "device reports readiness to read but returned no data (device disconnected or
# multiple access on port?)" the instant 50-console opened the same port -- the
# console suite passed and the capture was lost, which is the wrong one to lose.
# It now rides that out (see the retry loop), but skipping the console suites is
# still the honest arrangement rather than two readers each seeing half a line.
#
# The `top` session is a reproduction lever, not decoration. A busy SSH session
# keeps HMAC-SHA running continuously over the connection buffers in PSRAM, and
# esp_cache_msync() on those buffers is the call that faults. An idle extra
# session does not do this; one redrawing a screenful per frame does.
#
# This never writes to the port -- no reset, no keystrokes. The board being
# watched has to be the board that would have run anyway.

set -u

PORT="${1:-}"
LOG="${2:-serial.log}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "$PORT" ]; then
    PORT=$("$here/port.sh" 2>/dev/null || true)
    [ -n "$PORT" ] || { echo "serlog: no serial port found; pass one" >&2; exit 2; }
fi

# The IDF virtualenv has pyserial; the system python usually does not.
if [ -x "$here/idf.sh" ]; then
    eval "$(bash "$here/idf.sh" --env 2>/dev/null)" 2>/dev/null || true
fi
PY="${ESPIX_PYTHON:-python3}"

"$PY" -c 'import serial' 2>/dev/null \
    || { echo "serlog: $PY has no pyserial" >&2; exit 2; }

echo "serlog: $PORT -> $LOG  (^C to stop)" >&2

exec "$PY" - "$PORT" "$LOG" <<'PY'
import sys, time, serial

port, out = sys.argv[1], sys.argv[2]

# Append, never truncate: a second invocation against the same file must not
# throw away the panic the first one caught.
f = open(out, 'ab', buffering=0)
f.write(b'=== serlog start %s ===\n' % time.strftime('%Y-%m-%d %H:%M:%S').encode())

buf = b''
ser = None
while True:
    try:
        if ser is None:
            # timeout, not zero: a blocking-with-deadline read costs no CPU
            # between lines, and there are long quiet stretches in a test run.
            ser = serial.Serial(port, 115200, timeout=0.2)
        chunk = ser.read(4096)
    except (serial.SerialException, OSError) as e:
        # Reached when something else opens the same cu.* device -- macOS
        # permits that and both readers then get partial reads. Dying here is
        # what lost the capture the first time, so reopen and carry on: a gap in
        # the log beats no log.
        f.write(b'%s [serlog] %s -- reopening\n'
                % (time.strftime('%H:%M:%S').encode(), str(e).encode()))
        try:
            if ser is not None:
                ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(0.5)
        continue

    if not chunk:
        continue

    buf += chunk
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        # Timestamped per line, because attributing a panic to the suites that
        # were running at that moment is the point; run.sh's monitor records
        # which those were, with the same clock.
        f.write(b'%s %s\n' % (time.strftime('%H:%M:%S').encode(),
                              line.rstrip(b'\r')))
PY
