# Running suites several at a time, and showing what is happening while it does.
#
# Two things are wanted from this, and the second matters more than the first.
# Speed, because a login costs a key exchange plus PBKDF2 at 20 000 iterations
# and a serial run spends most of its time paying that. And coverage: unrelated
# suites running at once put the device under a shape of load nothing else here
# produces -- several sessions, several shells, transfers beside them -- so the
# pool is itself a test. Suites are handed out in a *random* order for the same
# reason, so that successive runs try different combinations.
#
# Random order costs reproducibility unless the seed comes back, so the seed is
# printed at the top of every run and accepted by --seed. A failure that only
# happens under one interleaving is worth little if it cannot be run again.
#
# bash 3.2, like everything else here: no associative arrays, no coproc. The
# queue and the locks are directories, because mkdir is atomic on APFS and on
# ext4 and there is no other portable primitive to hand.

# ---------------------------------------------------------------- ordering ---

# Deterministic shuffle from a seed.
#
# Not $RANDOM: bash seeds it per implementation, so one seed gives a different
# order under bash 3.2 and bash 5 -- and a seed that does not replay is worse
# than no seed at all, because it looks like it should. awk's srand() has the
# same problem across mawk, gawk and BWK awk. Python's Mersenne Twister is
# stable across versions and platforms, and python is already a hard dependency.
pool_shuffle() {    # <seed> <item>...
    local seed="$1"; shift
    "$ESPIX_PYTHON" -c '
import random, sys
seed = int(sys.argv[1])
items = sys.argv[2:]
random.seed(seed)
random.shuffle(items)
print("\n".join(items))
' "$seed" "$@"
}

# What a suite needs to itself, declared in its own header. See tests/README.md.
#   none        runs beside anything
#   console     the one serial port
#   exclusive   the whole device -- measurements, and anything that saturates it
pool_resources() {  # <suite file>
    local r
    r=$(sed -n 's/^# RESOURCES: *//p' "$1" | head -1)
    printf '%s' "${r:-none}"
}

# ------------------------------------------------------------------- queue ---

_pool_lock() {      # <dir> <name>
    local waited=0
    while ! mkdir "$1/$2.lock" 2>/dev/null; do
        sleep 0.05
        waited=$((waited + 1))
        [ "$waited" -gt 1200 ] && return 0      # a minute: break in rather than hang
    done
    return 0
}

_pool_unlock() { rmdir "$1/$2.lock" 2>/dev/null; return 0; }

# Take the next suite, or print nothing when there are none left.
#
# Dynamic rather than a static split across workers: suite durations differ by
# an order of magnitude here, and a static split leaves three workers idle while
# the fourth finishes the long one.
_pool_pop() {       # <dir>
    local dir="$1" name=""

    _pool_lock "$dir" queue
    if [ -s "$dir/queue" ]; then
        name=$(head -1 "$dir/queue")
        sed '1d' "$dir/queue" > "$dir/queue.rest" 2>/dev/null
        mv -f "$dir/queue.rest" "$dir/queue"
    fi
    _pool_unlock "$dir" queue

    printf '%s' "$name"
}

# ------------------------------------------------------------ running one ----

# Run one suite in this shell, with its output captured and its tally recorded.
# Shared by the pool workers and by the serial phases, so a suite behaves the
# same however it was scheduled and the report reads the same either way.
#
# <dir>/out/<name>.log   everything the suite printed
# <dir>/res/<name>.res   "<pass> <fail> <skip> <seconds> <how>"
pool_run_suite() {  # <dir> <name> <how: pool|serial|rerun>
    local dir="$1" name="$2" how="$3"
    local file="$ESPIX_TEST_DIR/suites/$name.sh"
    local t0=$SECONDS

    ESPIX_PROGRESS="$dir/w$ESPIX_WORKER.prog"
    export ESPIX_PROGRESS
    rm -f "$ESPIX_PROGRESS" "$ESPIX_PROGRESS.fail"
    printf '%s %s\n' "$name" "$(date +%s)" > "$dir/w$ESPIX_WORKER.cur"

    espix_counters_reset

    # Braces, not a subshell: the counters have to survive to be written out
    # below, and a subshell would take them with it. Which also rules out
    # `| tee`, the obvious way to both capture and show a suite's output.
    #
    # So a serial run does not capture at all -- it prints as it goes, exactly
    # as this runner always has, because watching a suite fail in real time is
    # most of why anyone runs one suite on its own. The pool captures, because
    # four suites interleaving their lines is not readable by anyone.
    if [ "${POOL_CAPTURE:-1}" = 1 ]; then
        {
            espix_suite_begin "$name"
            if dev_session_start; then
                . "$file"
            else
                espix_fail "$name: could not open a session"
            fi
            dev_session_stop
        } > "$dir/out/$name.log" 2>&1
    else
        {
            espix_suite_begin "$name"
            if dev_session_start; then
                . "$file"
            else
                espix_fail "$name: could not open a session"
            fi
            dev_session_stop
        } 2>&1
    fi

    # The console is held for a suite, not for a run, and the worker that
    # opened it is about to move on to something else. Without this, console.py
    # keeps the serial port after the suite ends -- and in a pool the worker
    # simply exits, so nothing else would ever close it.
    dev_console_stop

    printf '%s %s %s\n' "$(espix_counters_line)" "$((SECONDS - t0))" "$how" \
        > "$dir/res/$name.res"
    rm -f "$dir/w$ESPIX_WORKER.cur"
    return 0
}

# ----------------------------------------------------------------- workers ---

pool_worker() {     # <dir> <n>
    local dir="$1" name

    ESPIX_WORKER="$2"
    export ESPIX_WORKER

    while [ ! -f "$dir/stop" ]; do
        name=$(_pool_pop "$dir")
        [ -n "$name" ] || break
        pool_run_suite "$dir" "$name" pool
    done
    return 0
}

# -------------------------------------------------------------------- grid ---
#
# Only when stdout is a terminal. Piped or redirected -- CI, `make test | tee`,
# a log file -- the grid is meaningless and its cursor movement corrupts what is
# written, so the runner streams each suite's log on completion instead.

POOL_GRID_LINES=0
POOL_CELL_W=32

_pool_cols() {
    local w="" c
    command -v tput >/dev/null 2>&1 && w=$(tput cols 2>/dev/null)
    case "$w" in ''|*[!0-9]*) w=80 ;; esac
    c=$(( (w - 2) / (POOL_CELL_W + 2) ))
    [ "$c" -lt 1 ] && c=1
    printf '%s' "$c"
}

# One worker's cell: what it is running, for how long, and its tally so far.
_pool_cell() {      # <dir> <n>
    local dir="$1" n="$2" name="" started="" el="-" p=0 f=0 s=0 rest=

    if [ -f "$dir/w$n.cur" ]; then
        name=$(cut -d' ' -f1 "$dir/w$n.cur" 2>/dev/null)
        started=$(cut -d' ' -f2 "$dir/w$n.cur" 2>/dev/null)
        case "$started" in
            ''|*[!0-9]*) el="-" ;;
            *) el="$(( $(date +%s) - started ))s" ;;
        esac
    fi
    if [ -f "$dir/w$n.prog" ]; then
        read -r p f s rest < "$dir/w$n.prog" 2>/dev/null
    fi
    case "$p" in ''|*[!0-9]*) p=0 ;; esac
    case "$f" in ''|*[!0-9]*) f=0 ;; esac

    if [ -z "$name" ]; then
        # Deliberately uncoloured: the cells are aligned by counting printable
        # characters, and an escape sequence is not one of those.
        printf '%-3s %-15.15s %4s %6s' "w$n" "(idle)" "" ""
    elif [ "$f" -gt 0 ]; then
        printf '%-3s %-15.15s %4s %3s ok %s' "w$n" "$name" "$el" "$p" \
               "$(_espix_red "$f fail")"
    else
        printf '%-3s %-15.15s %4s %3s ok' "w$n" "$name" "$el" "$p"
    fi
}

# Move back over the last frame and draw another. \033[K on every line, because
# a shorter line must not leave the tail of a longer one behind it.
pool_grid_draw() {  # <dir> <jobs> <total> <start seconds>
    local dir="$1" jobs="$2" total="$3" start="$4"
    local cols n line lines=0 done_n queued p f s rest tp=0 tf=0 ts=0 res cell

    [ -t 1 ] || return 0

    if [ "$POOL_GRID_LINES" -gt 0 ]; then
        printf '\033[%dA' "$POOL_GRID_LINES"
    fi

    cols=$(_pool_cols)
    n=1
    line=""
    while [ "$n" -le "$jobs" ]; do
        # Padded after colouring, so the escape codes do not eat the width. The
        # cell is padded to POOL_CELL_W of *printable* text by _pool_cell's own
        # %-15.15s fields; this just separates the columns.
        cell=$(_pool_cell "$dir" "$n")
        line="$line$cell   "
        if [ $(( n % cols )) -eq 0 ] || [ "$n" -eq "$jobs" ]; then
            printf '\033[K  %s\n' "$line"
            lines=$((lines + 1))
            line=""
        fi
        n=$((n + 1))
    done

    done_n=0
    for res in "$dir"/res/*.res; do
        [ -f "$res" ] || continue
        done_n=$((done_n + 1))
        read -r p f s rest < "$res" 2>/dev/null
        tp=$((tp + ${p:-0})); tf=$((tf + ${f:-0})); ts=$((ts + ${s:-0}))
    done

    # Suites still running count too. Without this the totals said "0 fail"
    # while a cell above it was showing one in red, which reads as the tally
    # disagreeing with the thing it is a tally of.
    n=1
    while [ "$n" -le "$jobs" ]; do
        if [ -f "$dir/w$n.cur" ] && [ -f "$dir/w$n.prog" ]; then
            read -r p f s rest < "$dir/w$n.prog" 2>/dev/null
            tp=$((tp + ${p:-0})); tf=$((tf + ${f:-0})); ts=$((ts + ${s:-0}))
        fi
        n=$((n + 1))
    done
    queued=0
    [ -f "$dir/queue" ] && queued=$(grep -c . "$dir/queue" 2>/dev/null)
    case "$queued" in ''|*[!0-9]*) queued=0 ;; esac

    printf '\033[K\n'
    printf '\033[K  queued %s  done %s/%s   %s ok  %s fail  %s skip   %ss\n' \
        "$queued" "$done_n" "$total" "$tp" "$tf" "$ts" "$((SECONDS - start))"
    lines=$((lines + 2))

    POOL_GRID_LINES=$lines
    return 0
}

# Leave the terminal as we found it.
pool_grid_end() {
    [ -t 1 ] || return 0
    POOL_GRID_LINES=0
    return 0
}
