#!/usr/bin/env bash
#
# espix test runner.
#
#   tests/run.sh                     everything, four suites at a time
#   tests/run.sh -j 8                more workers (the device allows 8 sessions)
#   tests/run.sh --serial            one at a time, printing as it goes
#   tests/run.sh --seed 48213        replay a particular random order
#   tests/run.sh --suite fs          just the suite whose name contains "fs"
#   tests/run.sh --host 10.0.0.5     a different device
#   tests/run.sh --port /dev/ttyUSB0 enable the console suites
#   tests/run.sh --stress [--stress-n 50]   include the stress suites
#   tests/run.sh --overlap-transfers        let transfers run concurrently
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
. "$ESPIX_TEST_DIR/lib/pool.sh"

export ESPIX_ROOT

SUITE_FILTER=""
: "${ESPIX_STRESS:=0}"
: "${ESPIX_STRESS_N:=30}"
: "${ESPIX_STRESS_LINES:=2000}"
: "${ESPIX_STRESS_LIMIT:=0}"
: "${ESPIX_SUITE_TIMEOUT:=240}"
: "${ESPIX_RUN_TIMEOUT:=1200}"
# Four at a time.
#
# The number comes from the device: CONFIG_ESPIX_SSH_MAX_SESSIONS is 8, and four
# workers means four long-lived sessions plus the health monitor, with room left
# for the one-shot connection a suite opens for an exit status or a transfer and
# for somebody watching `top` from another window.
#
# This was 1 for exactly as long as it took to find out why concurrency
# rebooted the board -- a double free in shared command history, fixed. If a
# parallel run starts panicking again, `--serial` is the way to get a trustworthy
# answer while tools/soak.sh finds out why.
: "${ESPIX_JOBS:=4}"
: "${ESPIX_SEED:=}"
export ESPIX_STRESS ESPIX_STRESS_N ESPIX_STRESS_LINES ESPIX_STRESS_LIMIT

while [ $# -gt 0 ]; do
    case "$1" in
        --suite) SUITE_FILTER="$2"; shift 2 ;;
        --stress) ESPIX_STRESS=1; shift ;;
        --stress-n) ESPIX_STRESS_N="$2"; shift 2 ;;
        --stress-lines) ESPIX_STRESS_LINES="$2"; shift 2 ;;
        --stress-limit) ESPIX_STRESS_LIMIT="$2"; shift 2 ;;
        -j|--jobs) ESPIX_JOBS="$2"; shift 2 ;;
        --serial) ESPIX_JOBS=1; shift ;;
        --seed) ESPIX_SEED="$2"; shift 2 ;;
        --overlap-transfers) ESPIX_OVERLAP_TRANSFERS=1; shift ;;
        --host)  ESPIX_HOST="$2";   shift 2 ;;
        --user)  ESPIX_USER="$2";   shift 2 ;;
        --pass)  ESPIX_PASS="$2";   shift 2 ;;
        --port)  [ $# -ge 2 ] || { echo "run.sh: --port needs a value" >&2; exit 2; }
                 ESPIX_PORT="$2";   shift 2 ;;
        -h|--help) sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

case "$ESPIX_JOBS" in
    ''|*[!0-9]*|0) echo "run.sh: -j needs a positive count" >&2; exit 2 ;;
esac
export ESPIX_OVERLAP_TRANSFERS

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

RUNDIR=$(espix_mktemp_dir)
mkdir -p "$RUNDIR/out" "$RUNDIR/res" "$RUNDIR/rerun/out" "$RUNDIR/rerun/res"
: > "$RUNDIR/retries"
ESPIX_RUNDIR="$RUNDIR"
export ESPIX_RUNDIR

MONITOR_PID=""
WORKER_PIDS=""
WORKER_PID=()       # by worker number, for the per-suite deadline

cleanup() {
    : > "$RUNDIR/stop" 2>/dev/null
    local p
    for p in $WORKER_PIDS $MONITOR_PID; do
        { kill -TERM "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; } 2>/dev/null
    done
    dev_session_stop
    dev_console_stop
    dev_askpass_cleanup
    rm -rf "$RUNDIR"
    [ -t 1 ] && printf '\033[?25h'      # cursor back, if a frame was interrupted
    return 0
}

dev_askpass_init
trap 'cleanup' EXIT
trap 'printf "\ninterrupted\n" >&2; exit 130' INT TERM

# dev_status carries its own deadline (see device.sh), so no wrapper here. The
# wrapper is what created the orphans: espix_timeout was handed this shell
# function, killed the subshell it got back, and left ssh holding a connection
# slot on every failed preflight.
if ! dev_status 'uptime'; then
    fatal "cannot reach $ESPIX_HOST as $ESPIX_USER (is it up, and is the password right?)"
fi

# --------------------------------------------------------- what to run ---

POOL_SUITES=""
EXCL_SUITES=""
N_SUITES=0

for suite in "$ESPIX_TEST_DIR"/suites/*.sh; do
    [ -f "$suite" ] || continue
    name=$(basename "$suite" .sh)
    if [ -n "$SUITE_FILTER" ]; then
        case "$name" in *"$SUITE_FILTER"*) ;; *) continue ;; esac
    fi
    N_SUITES=$((N_SUITES + 1))

    # Measurements and anything that saturates the device run alone, at the end.
    # A throughput floor taken beside three other suites measures the pool.
    case "$(pool_resources "$suite")" in
        exclusive) EXCL_SUITES="$EXCL_SUITES $name" ;;
        *)         POOL_SUITES="$POOL_SUITES $name" ;;
    esac
done

[ "$N_SUITES" -gt 0 ] || fatal "no suites matched '${SUITE_FILTER:-*}'"

# One worker per suite at most: spare workers cost a login each and do nothing.
POOL_COUNT=$(printf '%s\n' $POOL_SUITES | grep -c . || true)
JOBS=$ESPIX_JOBS
[ "$JOBS" -gt "${POOL_COUNT:-1}" ] && [ "${POOL_COUNT:-0}" -gt 0 ] && JOBS=$POOL_COUNT

# The seed is printed and replayable. Random order is what makes successive runs
# try different combinations; a random order that cannot be run again is what
# makes a concurrency finding useless.
if [ -z "$ESPIX_SEED" ]; then
    ESPIX_SEED=$(( ($(date +%s) ^ $$) % 100000 ))
fi

printf 'espix tests -- %s@%s' "$ESPIX_USER" "$ESPIX_HOST"
[ -n "$ESPIX_PORT" ] && printf ', console %s' "$ESPIX_PORT"
if [ "$ESPIX_JOBS" = 1 ]; then
    printf '   serial\n'
else
    printf '   -j%s  seed %s\n' "$JOBS" "$ESPIX_SEED"
fi

# One session for the whole preflight: the health baseline and the test app,
# which between them used to cost four logins before a single assertion ran.
if dev_session_start; then
    dev_health_begin
    # The test app goes over once, here, before any worker exists. Four suites
    # used to sync it themselves, which is a race the moment two of them run at
    # the same time.
    dev_testapp_sync "$ESPIX_ROOT/fsroot/home/$ESPIX_USER/testapp" >/dev/null \
        || printf '  %s test app not staged; suites needing it will skip\n' \
                  "$(_espix_dim note:)"
    dev_session_stop
else
    printf '  %s no preflight session; health baseline taken over one-shots\n' \
           "$(_espix_dim note:)"
    dev_health_begin
fi

START=$SECONDS
FAILED_POOL=""

# ------------------------------------------------------------ the phases ---

run_serial_list() {     # <how> <name>...
    local how="$1"; shift
    local name outdir="$RUNDIR"

    [ "$how" = rerun ] && outdir="$RUNDIR/rerun"
    for name in "$@"; do
        [ -n "$name" ] || continue
        if [ "$POOL_CAPTURE" = 1 ]; then
            printf '  %s ' "$(_espix_dim "-> $name")"
        fi
        pool_run_suite "$outdir" "$name" "$how"
        if [ "$POOL_CAPTURE" = 1 ] && [ -f "$outdir/res/$name.res" ]; then
            read -r p f s secs rest < "$outdir/res/$name.res"
            printf '%ss, %s ok, %s fail, %s skip\n' "$secs" "$p" "$f" "$s"
        fi
    done
}

ESPIX_WORKER=0
export ESPIX_WORKER

# The monitor watches both modes. A serial run has no pool to abort, but it can
# still spend ten minutes asking a rebooted device questions, which is the thing
# being fixed here rather than a parallel-only nicety.
#
# In its own process group because it is a shell function: `&` gives back a
# subshell pid, and killing that would leave its session.py -- and the ssh under
# *that* -- alive, holding a connection slot until the device reboots.
set -m
dev_monitor_run "$RUNDIR" >/dev/null 2>&1 &
MONITOR_PID=$!
set +m

if [ "$ESPIX_JOBS" = 1 ]; then
    # Serial: print as it goes, exactly as this runner always has.
    POOL_CAPTURE=0
    export POOL_CAPTURE
    run_serial_list serial $POOL_SUITES $EXCL_SUITES
else
    POOL_CAPTURE=1
    export POOL_CAPTURE

    # --- phase 1: the pool -------------------------------------------------
    if [ -n "$POOL_SUITES" ]; then
        pool_shuffle "$ESPIX_SEED" $POOL_SUITES > "$RUNDIR/queue"

        # An indexed array, which bash 3.2 does have -- it is the *associative*
        # kind it lacks. The index matters here: the deadline below has to map a
        # stalled worker number back to a pid it can signal.
        n=1
        while [ "$n" -le "$JOBS" ]; do
            # Each worker in its own process group, so this loop and cleanup()
            # can reach the ssh underneath it. Backgrounding a shell function
            # gives back a subshell pid and signalling that leaves the ssh alive
            # holding a session slot -- this project has lost a day to that.
            set -m
            pool_worker "$RUNDIR" "$n" &
            WORKER_PID[$n]=$!
            WORKER_PIDS="$WORKER_PIDS $!"
            set +m
            n=$((n + 1))
        done

        [ -t 1 ] && printf '\n\033[?25l'
        while :; do
            alive=""
            for p in $WORKER_PIDS; do
                kill -0 "$p" 2>/dev/null && alive=yes
            done
            pool_grid_draw "$RUNDIR" "$JOBS" "$POOL_COUNT" "$START"
            [ -n "$alive" ] || break

            if [ -f "$RUNDIR/health.alert" ]; then
                : > "$RUNDIR/stop"
            fi

            # A suite deadline that actually ends the suite.
            #
            # It used to be reported after the fact and nothing more, because
            # the suite was sourced into this very shell and there was nothing
            # to signal. Under the pool there is: a worker is a subshell in its
            # own process group, and w<n>.cur already carries when it started.
            n=1
            while [ "$n" -le "$JOBS" ]; do
                if [ -f "$RUNDIR/w$n.cur" ]; then
                    read -r stuck_name stuck_at _rest < "$RUNDIR/w$n.cur" 2>/dev/null
                    case "${stuck_at:-x}" in
                        ''|*[!0-9]*) ;;
                        *)
                            if [ $(( $(date +%s) - stuck_at )) -gt "$ESPIX_SUITE_TIMEOUT" ]; then
                                p=${WORKER_PID[$n]:-}
                                if [ -n "$p" ]; then
                                    { kill -KILL "-$p" 2>/dev/null || kill -KILL "$p" 2>/dev/null; } 2>/dev/null
                                fi
                                printf '0 1 0 %s timeout\n' "$ESPIX_SUITE_TIMEOUT" \
                                    > "$RUNDIR/res/$stuck_name.res"
                                printf '  %s %s: exceeded %ss and was killed\n' \
                                    "$(_espix_red FAIL)" "$stuck_name" \
                                    "$ESPIX_SUITE_TIMEOUT" >> "$RUNDIR/out/$stuck_name.log"
                                rm -f "$RUNDIR/w$n.cur"
                            fi ;;
                    esac
                fi
                n=$((n + 1))
            done

            # And a backstop, so no run is unbounded whatever else goes wrong.
            if [ $((SECONDS - START)) -gt "$ESPIX_RUN_TIMEOUT" ]; then
                dev_abort "the run exceeded ${ESPIX_RUN_TIMEOUT}s"
                : > "$RUNDIR/stop"
            fi

            sleep 0.25
        done
        for p in $WORKER_PIDS; do wait "$p" 2>/dev/null; done
        WORKER_PIDS=""
        [ -t 1 ] && printf '\033[?25h'
        pool_grid_end

        # --- phase 2: settle ------------------------------------------------
        #
        # Everything below this line runs on a device that is supposed to be
        # quiet, and one still reclaiming connections is not: close_gracefully()
        # drains until the peer hangs up, bounded at five seconds. A
        # measurement taken during that measures the pool, and a re-run during
        # it fails for the wrong reason -- which would be the worst possible
        # answer from the step whose whole job is to say *why* something failed.
        for res in "$RUNDIR"/res/*.res; do
            [ -f "$res" ] || continue
            read -r p f s secs rest < "$res"
            [ "$rest" = aborted ] && continue
            [ "${f:-0}" -gt 0 ] && FAILED_POOL="$FAILED_POOL $(basename "$res" .res)"
        done

        if [ -n "$EXCL_SUITES" ] || [ -n "$FAILED_POOL" ]; then
            printf '\n  %s\n' "$(_espix_dim 'settling...')"
            sleep 8
        fi
    fi

    # --- phase 3: serial re-runs of anything that failed -------------------
    #
    # Not when the run aborted. Re-running against a board that has just
    # rebooted answers a question nobody asked -- the suites did not fail, the
    # device went away underneath them, and the report says so instead.
    if [ -n "$FAILED_POOL" ] && ! dev_aborted; then
        printf '  %s\n' \
            "$(_espix_dim 're-running what failed, alone, to tell a bug from a collision:')"
        run_serial_list rerun $FAILED_POOL
        sleep 5
    fi

    # --- phase 4: the measurements, alone and last -------------------------
    if [ -n "$EXCL_SUITES" ] && ! dev_aborted; then
        printf '  %s\n' "$(_espix_dim 'quiet phase (measurements run alone):')"
        run_serial_list serial $EXCL_SUITES
    fi
fi

# The monitor holds a session of its own, and the leak check below counts
# sessions, so it has to be gone before that runs.
: > "$RUNDIR/stop"
if [ -n "$MONITOR_PID" ]; then
    wait "$MONITOR_PID" 2>/dev/null
    MONITOR_PID=""
fi

# ---------------------------------------------------------------- report ---

# Logs in suite order, whatever order they ran in, so two runs diff cleanly.
if [ "$POOL_CAPTURE" = 1 ]; then
    for log in "$RUNDIR"/out/*.log; do
        [ -f "$log" ] || continue
        cat "$log"
    done
fi

N_PASS=0; N_FAIL=0; N_SKIP=0; SUITES_RUN=0; SUITES_FAILED=""
N_ABORTED=0; SUITES_ABORTED=""
for res in "$RUNDIR"/res/*.res; do
    [ -f "$res" ] || continue
    read -r p f s secs rest < "$res"
    name=$(basename "$res" .res)
    if [ "$rest" = aborted ]; then
        N_ABORTED=$((N_ABORTED + 1))
        SUITES_ABORTED="$SUITES_ABORTED $name"
        continue
    fi
    SUITES_RUN=$((SUITES_RUN + 1))
    N_PASS=$((N_PASS + ${p:-0}))
    N_FAIL=$((N_FAIL + ${f:-0}))
    N_SKIP=$((N_SKIP + ${s:-0}))
    [ "${f:-0}" -gt 0 ] && SUITES_FAILED="$SUITES_FAILED $name"
done

# Suites nobody ever got to: the queue still had them when the run stopped.
NEVER_RAN=$((N_SUITES - SUITES_RUN - N_ABORTED))
[ "$NEVER_RAN" -lt 0 ] && NEVER_RAN=0

printf '\n--------------------------------------------------\n'

# The reason comes first when there is one. A reboot is a single event, and
# reporting it as twenty test failures buries the only line that matters --
# which is exactly what a run did before this: 37 failures, one cause.
if [ -f "$RUNDIR/abort" ]; then
    printf '%s %s\n' "$(_espix_red 'RUN ABORTED:')" "$(head -1 "$RUNDIR/abort")"
    [ -f "$RUNDIR/health.alert" ] && sed -n '2p' "$RUNDIR/health.alert" | sed 's/^/  /'
    printf '  %ds in.' "$((SECONDS - START))"
    [ -n "$SUITES_ABORTED" ] && printf ' cut short:%s' "$SUITES_ABORTED"
    [ "$NEVER_RAN" -gt 0 ] && printf ' %d never started.' "$NEVER_RAN"
    printf '\n\n'
fi

printf '%d suites, %d passed, %d failed, %d skipped, %ds\n' \
       "$SUITES_RUN" "$N_PASS" "$N_FAIL" "$N_SKIP" "$((SECONDS - START))"

RETRIES=$(grep -c . "$RUNDIR/retries" 2>/dev/null || echo 0)
case "$RETRIES" in ''|*[!0-9]*) RETRIES=0 ;; esac
if [ "$RETRIES" -gt 0 ]; then
    printf 'connections retried after "too many connections": %d' "$RETRIES"
    if [ "$RETRIES" -gt 20 ]; then
        printf '  -- %s\n' \
            "that is a lot; -j$JOBS may be more than this device's session limit feeds"
    else
        printf '\n'
    fi
fi

# What the pool failures turned out to be. This is the whole reason the re-run
# happens: a suite that fails alone is a bug the pool merely found earlier,
# while one that passes alone is a finding *about* concurrency, and the two want
# completely different next steps.
if [ -n "${FAILED_POOL:-}" ]; then
    printf '\nfailures found by the pool, re-run alone:\n'
    for name in $FAILED_POOL; do
        if [ -f "$RUNDIR/rerun/res/$name.res" ]; then
            read -r p f s secs rest < "$RUNDIR/rerun/res/$name.res"
            if [ "${f:-0}" -gt 0 ]; then
                printf '  %-14s fails alone too (%s) -- a bug, not a collision\n' \
                       "$name" "$f"
            else
                printf '  %-14s %s -- only fails under load; seed %s reproduces it\n' \
                       "$name" "$(_espix_red 'passes alone')" "$ESPIX_SEED"
            fi
        fi
    done
fi

if health=$(dev_health_check); then :; else
    printf '\n%s %s\n' "$(_espix_red 'device health after the run:')" "$health"
    N_FAIL=$((N_FAIL + 1))
fi

if [ -f "$RUNDIR/abort" ]; then
    N_FAIL=$((N_FAIL + 1))
fi

if [ "$N_FAIL" -gt 0 ]; then
    [ -n "$SUITES_FAILED" ] && printf 'failed suites:%s\n' "$SUITES_FAILED"
    [ "$ESPIX_JOBS" != 1 ] && \
        printf 'to reproduce this order: tests/run.sh --seed %s -j %s\n' \
               "$ESPIX_SEED" "$JOBS"
    exit 1
fi
printf 'all good\n'
exit 0
