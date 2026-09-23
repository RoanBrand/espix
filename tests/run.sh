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
    # Refuse a port somebody else already has, because --port makes this run a
    # *second* reader. macOS lets two processes share a cu.* device and they
    # split the byte stream between them, so both captures come out with holes --
    # and that is not hypothetical: a suite run made itself the second reader of
    # the capture that was hunting its panic, which cost the panic's reason for
    # the rest of the session. tools/serlog.sh has always refused in the other
    # direction; this is the direction that actually happened.
    #
    # Three-way status from the helper -- 0 free, 1 held, 2 no such port -- kept
    # apart here too, because "held" and "missing" want different advice.
    holder_rc=0
    "$ESPIX_ROOT/tools/port-holder.sh" "$ESPIX_PORT" || holder_rc=$?
    if [ "$holder_rc" = 1 ]; then
        fatal "$ESPIX_PORT is already held, above; run without --port to leave the console alone, or stop the reader first"
    fi
    [ "$holder_rc" = 0 ] || fatal "$ESPIX_PORT cannot be opened, above"

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

# --------------------------------------------------- is this the build? ---
#
# `make test` builds the test app and runs the suite. It does not flash. So the
# board runs whatever was last written to it, which after any `idf.py build` is
# not the tree in front of you -- and nothing in a green run would tell you.
#
# This is not a hypothetical tidiness check. A -j4 run panicked, and a day went
# into explaining a CacheError that turned out to be in an image built from an
# uncommitted working tree twenty-eight minutes older than build/: the board had
# `72354c6-dirty`, build/ had `b5f4232-dirty`. The core dump would not even
# decode. Every address resolved against the wrong ELF, and the only reason that
# was caught is that espcoredump compares SHAs and refused.
#
# The build identity is the first CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9) hex
# characters of app_elf_sha256 in the app description -- 0xB0 into the image, so
# it is readable from build/espix.bin with dd, no toolchain and no second build.
# It is a content hash, and that is the point: `-dirty` is the same string for
# every modified tree, so a rebuild-without-flash at the same commit could
# compare equal on the describe alone. The SHA cannot collide that way, and
# espix_build_id() reports exactly this string as `uname -v`.
_local_build_id() {
    local bin="$ESPIX_ROOT/build/espix.bin" sha
    [ -f "$bin" ] || return 1
    sha=$(dd if="$bin" bs=1 skip=176 count=32 2>/dev/null | od -An -tx1 -v |
          tr -d ' \n' | cut -c1-9)
    [ -n "$sha" ] || return 1
    printf '%s' "$sha"
}

# Over the preflight session when there is one, and over its own connection when
# there is not -- the check is worth a login on the rare path where the
# persistent session could not be opened, because that path is already degraded
# and is the last place to also be running the wrong image blind.
#
# `uname -v` is the Linux place for exactly this question ("which build of this
# kernel"), and it is asked for on its own rather than parsed out of `uname -a`,
# so the format of -a stays free to change.
_device_build_id() {
    local out
    if [ -n "${DEV_SESSION_PID:-}" ]; then
        out=$(dev_run 'uname -v')
    else
        out=$(dev_once 'uname -v')
    fi
    printf '%s' "$out" | sed -n 's/^#\([0-9a-f]\{9\}\)$/\1/p'
}

# Absent is not mismatched, and neither is unknown. A clean checkout has no
# build/, and a board running firmware from before espix reported its build id
# answers nothing -- refusing in either case would be a guard that fires on the
# wrong thing. Both are said out loud rather than passed over, because "the
# check did not run" and "the check passed" must not look alike.
check_build_matches() {
    local on_device on_disk
    on_disk=$(_local_build_id) || {
        printf '  %s no build/espix.bin; cannot check what the board is running\n' \
               "$(_espix_dim note:)"
        return 0
    }
    on_device=$(_device_build_id)

    if [ -z "$on_device" ]; then
        printf '  %s the board does not report a build id (firmware predates it);\n' \
               "$(_espix_dim note:)"
        printf '        cannot confirm it is running %s\n' "$on_disk"
        return 0
    fi
    [ "$on_device" = "$on_disk" ] && return 0

    printf '\n%s the board is running %s, build/ holds %s\n' \
           "$(_espix_red 'STALE FIRMWARE:')" "$on_device" "$on_disk" >&2
    printf 'A run against an image you did not build tells you about code that is\n' >&2
    printf 'not in front of you. Flash it first:\n\n    make flash\n\n' >&2
    return 1
}

# One session for the whole preflight: the health baseline and the test app,
# which between them used to cost four logins before a single assertion ran.
if dev_session_start; then
    check_build_matches || { dev_session_stop; exit 2; }
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
    check_build_matches || exit 2
    dev_health_begin
fi

START=$SECONDS
FAILED_POOL=""

# ------------------------------------------------------------ the phases ---

# What one suite did to the internal heap, measured only where it means
# something: this function is the single path for every phase that runs a suite
# ALONE -- --serial, the failed-suite re-run, and the quiet phase -- so a delta
# taken around the call has nobody else to share the blame with.
#
# The run already reported a low-water mark for the whole run, which says the
# heap got tight and not which suite tightened it. A steady 4K per run went
# unattributed for exactly that reason.
#
# Two `free` readings per suite, over the existing session rather than a fresh
# login: at ~2.7s of PBKDF2 each, logging in twice per suite would put a minute
# on every serial run for a diagnostic that is meant to be free.
#
# READ IT AS "what the heap did while this suite ran", not "what this suite
# allocated". The monitor session, the harness's own queries and anything else
# connected are inside the window too. First measurements: 30-proc reported
# +4K/+31 blocks on the first run after a boot and exactly nothing on the next,
# so a single reading is a warm-up cost as easily as a leak -- what makes it
# evidence is the same suite doing it twice.
_serial_heap_read() {
    _dev_parse_free_used "$(_dev_ask 'free')"
}

run_serial_list() {     # <how> <name>...
    local how="$1"; shift
    local name outdir="$RUNDIR" rc serial_pid waited
    local h0_used h0_blk h1_used h1_blk d_used d_blk

    [ "$how" = rerun ] && outdir="$RUNDIR/rerun"
    for name in "$@"; do
        [ -n "$name" ] || continue
        if [ "$POOL_CAPTURE" = 1 ]; then
            printf '  %s ' "$(_espix_dim "-> $name")"
        fi

        read -r h0_used h0_blk <<EOF
$(_serial_heap_read)
EOF

        # Deadlined, like everything in the pool.
        #
        # The pool's grid loop enforces ESPIX_SUITE_TIMEOUT and for a while
        # these phases did not, so a wedged suite hung the whole run with no
        # output: 50-console held the serial port for twenty-five minutes during
        # a re-run while the runner waited for a frame that was never coming. A
        # deadline covering three phases out of four is not a deadline.
        #
        # Backgrounded here rather than handed to espix_timeout, and that is not
        # a style choice. **espix_timeout does not nest** when the inner call
        # has a redirected stdin: wrap a function that runs
        # `espix_timeout ... cmd < file` and the inner command reads nothing at
        # all. It is reproducible in four lines and it cost this a run --
        # 45-throughput reported `sink: 0 bytes` and a stdin rate of zero, in a
        # suite that had passed minutes earlier. Backgrounding it directly, in
        # its own process group, is the shape the pool workers already use and
        # is known to keep stdin.
        set -m
        pool_run_suite "$outdir" "$name" "$how" &
        serial_pid=$!
        set +m

        rc=0
        waited=0
        while kill -0 "$serial_pid" 2>/dev/null; do
            if [ "$waited" -ge "$ESPIX_SUITE_TIMEOUT" ]; then
                { kill -KILL "-$serial_pid" 2>/dev/null \
                    || kill -KILL "$serial_pid" 2>/dev/null; } 2>/dev/null
                rc=124
                break
            fi
            sleep 1
            waited=$((waited + 1))
        done
        wait "$serial_pid" 2>/dev/null || true

        if [ "$rc" = 124 ]; then
            printf '0 1 0 %s timeout\n' "$ESPIX_SUITE_TIMEOUT" \
                > "$outdir/res/$name.res"
            printf '  %s %s: exceeded %ss and was killed\n' \
                "$(_espix_red FAIL)" "$name" "$ESPIX_SUITE_TIMEOUT" \
                >> "$outdir/out/$name.log"
            [ "$POOL_CAPTURE" = 1 ] && printf 'killed after %ss\n' "$ESPIX_SUITE_TIMEOUT"
            dev_console_stop
            continue
        fi

        read -r h1_used h1_blk <<EOF
$(_serial_heap_read)
EOF

        # Silent unless the heap actually moved, so a line here means something.
        # Either reading being -1 means the device did not answer, which is not
        # a measurement of zero and must not be printed as one.
        d_used=0; d_blk=0
        if [ "${h0_used:--1}" -ge 0 ] && [ "${h1_used:--1}" -ge 0 ]; then
            d_used=$(( h1_used - h0_used ))
            d_blk=$(( h1_blk - h0_blk ))
        fi

        if [ "$POOL_CAPTURE" = 1 ]; then
            # Appended to the timing the grid already prints for this suite.
            if [ -f "$outdir/res/$name.res" ]; then
                read -r p f s secs rest < "$outdir/res/$name.res"
                printf '%ss, %s ok, %s fail, %s skip' "$secs" "$p" "$f" "$s"
            fi
            if [ "$d_used" -ne 0 ] || [ "$d_blk" -ne 0 ]; then
                printf ', heap %+dK %+d blocks' "$d_used" "$d_blk"
            fi
            printf '\n'
        elif [ "$d_used" -ne 0 ] || [ "$d_blk" -ne 0 ]; then
            # Serial prints suite output as it goes, so this needs its own line.
            printf '  %s\n' \
                "$(_espix_dim "$name: heap $(printf '%+dK %+d blocks' "$d_used" "$d_blk")")"
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

# Ask the device how it is *before* printing any of it.
#
# This has to come first, and it did not: dev_health_check() is what writes
# watchdog.total and heapmin, and the two blocks below read them. With the call
# left where it read most naturally -- after the reporting -- the watchdog.total
# fallback was dead code, so a trigger the monitor's ten-second poll straddled
# printed nothing at all, which is the one case that fallback exists for.
#
# Only the fetch moves. The printed order is unchanged: watchdog, then heap,
# then health.
health=""
health_ok=yes
if health=$(dev_health_check); then :; else health_ok=no; fi

# The watchdog, with the attribution only the monitor could supply.
#
# Reported, never fatal. The watchdog fires when a core was busy for five
# seconds, which on a machine you can legitimately saturate is not a fault --
# see the long note on _dev_wdt_is_ours() in tests/lib/device.sh, and resist
# restoring the failure without reading it.
#
# Two sources, because they know different things. The monitor's watchdog.log
# has the part that is gone once the run ends -- which suites were in flight --
# but only for triggers its ten-second poll happened to catch. watchdog.total is
# the device's own cumulative count, which cannot miss one but cannot say when.
# Prefer the detailed one; fall back to the count so a trigger is never silent.
if [ -s "$RUNDIR/watchdog.log" ]; then
    printf '\n%s\n' "$(_espix_red 'task watchdog fired during the run:')"
    ours=0
    while IFS='|' read -r when n task running; do
        [ -n "$when" ] || continue
        printf '  %s  %s trigger(s), task %s, while running:%s\n' \
               "$(date -r "$when" '+%H:%M:%S' 2>/dev/null ||
                  date -d "@$when" '+%H:%M:%S' 2>/dev/null || echo "$when")" \
               "$n" "${task:-unknown}" "$running"
        case "${task:-}" in app:*) ;; *) ours=1 ;; esac
    done < "$RUNDIR/watchdog.log"

    if [ "$ours" = 1 ]; then
        printf '  a core was held long enough to starve its IDLE task; nothing rebooted\n'
        printf '  (CONFIG_ESP_TASK_WDT_PANIC is off), but IDLE is what frees the stacks\n'
        printf '  of self-deleted tasks, so this defers reclamation. See docs/GOTCHAS.md.\n'
    else
        printf '  all of these name an app, i.e. a program using the CPU it was given --\n'
        printf '  35-signals runs a compute loop on purpose.\n'
    fi
elif [ -s "$RUNDIR/watchdog.total" ]; then
    # The monitor saw nothing, but the device counted one. It fired between
    # polls, or before the monitor's first question.
    read -r _ n task < "$RUNDIR/watchdog.total"
    printf '\n%s %s trigger(s), task %s -- between polls, so no suite attribution\n' \
           "$(_espix_red 'task watchdog fired during the run:')" \
           "$n" "${task:-unknown}"
fi

# How close the internal heap came to nothing. Printed every run, not only a bad
# one: the number is only useful as a trend, and a line that appears only when
# something is already wrong cannot establish one.
#
# The device's figure is cumulative since boot, so it is reported against the
# baseline dev_health_begin() took -- otherwise the first run to touch zero
# turns the line red for every later run on that boot, and the run that actually
# did it stops standing out.
#
# Reported, never fatal: the run is entitled to push the device hard. But when
# it does read zero, the run's other failures are usually not what they look
# like. An allocation that failed, a session that died mid-command and a memory
# floor assertion each say "bug" alone, and "the heap hit the wall" together.
if [ -s "$RUNDIR/heapmin" ]; then
    read -r heap_min heap_base < "$RUNDIR/heapmin"
    if [ "${heap_base:--1}" -ge 0 ] && [ "$heap_min" -lt "$heap_base" ]; then
        if [ "$heap_min" -le 0 ]; then
            printf '\n%s allocations were failing\n' \
                   "$(_espix_red 'the internal heap reached zero during this run --')"
        else
            printf '\ninternal heap low-water mark: fell to %sK this run (was %sK)\n' \
                   "$heap_min" "$heap_base"
        fi
    else
        printf '\ninternal heap low-water mark: %sK, unchanged by this run\n' \
               "$heap_min"
    fi
fi

# What was still awaiting reclamation when the heap was last read.
#
# Every heap figure above -- the per-suite deltas and the low-water mark -- is
# taken at a moment, and a task that has exited but whose stack IDLE has not
# freed is holding memory that is on its way back. Without this line those two
# cases read identically, which is how 12K of deferred reclamation was reported
# as a leak in 55-sessions.
_pending=$(dev_tasks_deleted 2>/dev/null || printf '%s' "-1")
case "${_pending:-}" in ''|*[!0-9-]*) _pending=-1 ;; esac
if [ "$_pending" -gt 0 ]; then
    printf '\n%d task(s) had exited but were not yet reclaimed when this was read;\n' \
           "$_pending"
    printf 'their stacks are still counted as used. See docs/GOTCHAS.md on IDLE.\n'
fi

if [ "$health_ok" = no ]; then
    printf '\n%s %s\n' "$(_espix_red 'device health after the run:')" "$health"
    N_FAIL=$((N_FAIL + 1))
fi

if [ -f "$RUNDIR/abort" ]; then
    N_FAIL=$((N_FAIL + 1))
fi

# Was anything reading this board's UART while the run happened?
#
# ESPIX_SERLOG names the log when `make test-panic` started one; a capture
# started by hand leaves its own pidfile in the working directory. Either counts.
_espix_serial_capture() {
    local log="${ESPIX_SERLOG:-serial.log}" pid
    [ -f "$log.pid" ] || return 1
    pid=$(cat "$log.pid" 2>/dev/null)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

# A CacheError panic names which of seven distinct faults fired, and it prints
# that to the UART and nowhere else -- the core dump keeps exccause 71, which is
# identical for all seven. So a panic caught with nothing reading the port has
# already lost the line that would identify it, and no amount of re-reading the
# dump afterwards gets it back.
#
# This says so at the point it happened, because the alternative is what the
# project has already done twice: diagnose a cache error by assuming the
# fallback reason, and record the assumption as a finding.
_espix_panic_seen=no
grep -qi 'core dump' "$RUNDIR/abort" 2>/dev/null && _espix_panic_seen=yes
case "${health:-}" in *coredump*|*panic*) _espix_panic_seen=yes ;; esac

if [ "$_espix_panic_seen" = yes ] && ! _espix_serial_capture; then
    printf '\n%s\n' "$(_espix_red 'the panic reason was not captured:')"
    printf '  Which of the seven cache faults fired goes to the UART only, and nothing\n'
    printf '  was reading the port during this run, so that line is gone. The dump keeps\n'
    printf '  exccause 71, which every one of them shares.\n'
    printf '  To catch the next one:  %s\n' "$(_espix_dim 'make test-panic')"
    printf '  (console suites skip -- serlog has to be the only reader, because macOS\n'
    printf '   cu.* devices are not exclusive and two readers split the bytes.)\n'
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
