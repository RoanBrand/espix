#!/usr/bin/env bash
#
# espix test runner.
#
#   tests/run.sh                     everything
#   tests/run.sh --suite fs          just the suite whose name contains "fs"
#   tests/run.sh --host 10.0.0.5     a different device
#   tests/run.sh --port /dev/ttyUSB0 enable the console suites
#   tests/run.sh --stress [--stress-n 50]   include the stress suites
#
# bash 3.2 throughout -- macOS ships it and always will (GPLv3), so a bash-4-ism
# here is green on Linux and red on the author's own machine. See README.md.

set -u
set -o pipefail

ESPIX_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPIX_ROOT="$(cd "$ESPIX_TEST_DIR/.." && pwd)"

. "$ESPIX_TEST_DIR/lib/portable.sh"
. "$ESPIX_TEST_DIR/lib/assert.sh"
. "$ESPIX_TEST_DIR/lib/device.sh"

export ESPIX_ROOT

SUITE_FILTER=""
: "${ESPIX_STRESS:=0}"
: "${ESPIX_STRESS_N:=30}"
: "${ESPIX_STRESS_LINES:=2000}"
: "${ESPIX_STRESS_LIMIT:=0}"
: "${ESPIX_SUITE_TIMEOUT:=240}"
export ESPIX_STRESS ESPIX_STRESS_N ESPIX_STRESS_LINES ESPIX_STRESS_LIMIT
while [ $# -gt 0 ]; do
    case "$1" in
        --suite) SUITE_FILTER="$2"; shift 2 ;;
        --stress) ESPIX_STRESS=1; shift ;;
        --stress-n) ESPIX_STRESS_N="$2"; shift 2 ;;
        --stress-lines) ESPIX_STRESS_LINES="$2"; shift 2 ;;
        --stress-limit) ESPIX_STRESS_LIMIT="$2"; shift 2 ;;
        --host)  ESPIX_HOST="$2";   shift 2 ;;
        --user)  ESPIX_USER="$2";   shift 2 ;;
        --pass)  ESPIX_PASS="$2";   shift 2 ;;
        --port)  [ $# -ge 2 ] || { echo "run.sh: --port needs a value" >&2; exit 2; }
                 ESPIX_PORT="$2";   shift 2 ;;
        -h|--help) sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

# ------------------------------------------------------------ preflight ---

fatal() { printf 'run.sh: %s\n' "$*" >&2; exit 1; }

command -v ssh >/dev/null 2>&1 || fatal "no ssh"
command -v scp >/dev/null 2>&1 || fatal "no scp"
"$ESPIX_PYTHON" -c 'import sys; sys.exit(0)' 2>/dev/null \
    || fatal "ESPIX_PYTHON ($ESPIX_PYTHON) does not run"

# Serial suites need pyserial, which the system python usually lacks. Skipping
# them is right; failing the whole run because one interpreter is thin is not.
ESPIX_HAVE_SERIAL=no
if [ -n "$ESPIX_PORT" ]; then
    if "$ESPIX_PYTHON" -c 'import serial' 2>/dev/null; then
        ESPIX_HAVE_SERIAL=yes
    else
        echo "run.sh: $ESPIX_PYTHON has no pyserial; console suites will skip" >&2
    fi
fi
export ESPIX_HAVE_SERIAL

dev_askpass_init
trap 'dev_session_stop; dev_console_stop; dev_askpass_cleanup' EXIT

printf 'espix tests -- %s@%s' "$ESPIX_USER" "$ESPIX_HOST"
[ -n "$ESPIX_PORT" ] && printf ', console %s' "$ESPIX_PORT"
printf '\n'

# dev_status carries its own deadline now (see device.sh), so no wrapper here.
# The wrapper is what created the orphans: espix_timeout was handed this shell
# function, killed the subshell it got back, and left ssh holding a connection
# slot on every failed preflight.
if ! dev_status 'uptime'; then
    fatal "cannot reach $ESPIX_HOST as $ESPIX_USER (is it up, and is the password right?)"
fi

dev_health_begin

# -------------------------------------------------------------- the run ---

SUITES_RUN=0
SUITES_FAILED=""
START=$SECONDS

for suite in "$ESPIX_TEST_DIR"/suites/*.sh; do
    [ -f "$suite" ] || continue
    name=$(basename "$suite" .sh)
    if [ -n "$SUITE_FILTER" ]; then
        case "$name" in *"$SUITE_FILTER"*) ;; *) continue ;; esac
    fi

    before_fail=$ESPIX_N_FAIL
    espix_suite_begin "$name"

    # Each suite gets a fresh session: one login amortised over its assertions,
    # and a suite that wedges its session cannot poison the next one.
    if dev_session_start; then
        suite_start=$SECONDS
        . "$suite"

        # Reported after the fact, not enforced: the suite is sourced into
        # this shell, so there is nothing to signal. What stops a run hanging
        # is that every device call inside now carries its own deadline (see
        # ESPIX_SSH_TIMEOUT in device.sh); this catches the suite that stays
        # under each of those and still takes far longer than it should, which
        # is what a device with connection slots held looks like.
        if [ "$name" != "90-stress" ] \
           && [ $((SECONDS - suite_start)) -gt "$ESPIX_SUITE_TIMEOUT" ]; then
            espix_fail "$name: suite exceeded ${ESPIX_SUITE_TIMEOUT}s" \
                       "took $((SECONDS - suite_start))s"
        fi
    else
        espix_fail "$name: could not open a session"
    fi
    dev_session_stop

    SUITES_RUN=$((SUITES_RUN + 1))

    if health=$(dev_health_check); then :; else
        espix_fail "$name: device health after suite" "$health"
    fi

    [ "$ESPIX_N_FAIL" -gt "$before_fail" ] && SUITES_FAILED="$SUITES_FAILED $name"
done

# --------------------------------------------------------------- report ---

printf '\n--------------------------------------------------\n'
printf '%d suites, %d passed, %d failed, %d skipped, %ds\n' \
       "$SUITES_RUN" "$ESPIX_N_PASS" "$ESPIX_N_FAIL" "$ESPIX_N_SKIP" \
       "$((SECONDS - START))"

if [ "$ESPIX_N_FAIL" -gt 0 ]; then
    printf 'failed suites:%s\n' "$SUITES_FAILED"
    exit 1
fi
printf 'all good\n'
exit 0
