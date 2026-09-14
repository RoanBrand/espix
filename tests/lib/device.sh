# Talking to the device: one long-lived SSH session, one-shot commands for
# exit statuses, file transfer, and the health check that runs between suites.

: "${ESPIX_HOST:=192.168.110.55}"
: "${ESPIX_USER:=esp}"
: "${ESPIX_PASS:=espix}"
: "${ESPIX_PYTHON:=python3}"
: "${ESPIX_PORT:=}"

# Which worker this shell is: 0 in a serial run, 1..N under the pool. Every
# path this file names on the *device* carries it, because two workers running
# at once share one filesystem and a fixed name is a collision waiting for a
# fast enough machine.
: "${ESPIX_WORKER:=0}"

# The run's scratch directory, shared by the runner and its workers. Empty in a
# plain serial run, in which case the cross-worker locks below are no-ops --
# there is nothing to coordinate with.
: "${ESPIX_RUNDIR:=}"

# The run has given up.
#
# Written by the monitor when the device reboots or dumps core, and read by
# every call that would otherwise wait on a device that is not there. Without
# it a panic four minutes into a run cost another 577 seconds: each remaining
# suite still opened a session (60s login timeout), and every assertion in it
# still spent a 30s command deadline and four refusal retries before failing.
#
# A file rather than a variable, because the workers are separate processes and
# nothing assigned in one is visible in another.
dev_aborted() {
    [ -n "$ESPIX_RUNDIR" ] && [ -f "$ESPIX_RUNDIR/abort" ]
}

dev_abort() {   # <reason>
    [ -n "$ESPIX_RUNDIR" ] || return 0
    printf '%s\n' "$1" > "$ESPIX_RUNDIR/abort" 2>/dev/null
    return 0
}

# Let transfers overlap. Off by default: concurrent SFTP transfers are a
# documented, pre-existing fault (docs/KNOWN-ISSUES.md), so the default run
# serialises them rather than going red on a bug it is not testing. Turn it on
# to hunt that bug deliberately.
: "${ESPIX_OVERLAP_TRANSFERS:=0}"

ESPIX_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEV_SESSION_DIR=""
DEV_SESSION_PID=""
DEV_PROMPT=""

# Set when the shared session is known to be gone. Once it is, every dev_run
# returns DEV_DEAD rather than "".
#
# Why a sentinel *value* rather than an exit status: suites use dev_run inside
# `$( )` and compare the string, so `assert_eq "..." "" "$(dev_run ...)"` never
# looks at a status. A dead session used to answer "" to everything, which is
# exactly what several of those assertions expect -- so losing the session made
# them pass. All seven empty-expecting assertions in the tree are in
# 15-streams.sh, the suite covering the stream split, so the failure mode was
# aimed squarely at the work it was meant to verify.
DEV_SESSION_DEAD=""
DEV_DEAD="<<<dead-session>>>"

# ---------------------------------------------------------------- session ---
#
# session.py holds one login for a whole suite, because a login costs a key
# exchange plus PBKDF2 at 20 000 iterations. FIFOs rather than a coprocess:
# `coproc` is bash 4, and macOS ships 3.2.

dev_session_start() {
    # Nothing to log in to once the run has given up; see dev_aborted().
    if dev_aborted; then
        DEV_SESSION_DEAD=yes
        DEV_PROMPT=""
        return 1
    fi

    DEV_SESSION_DIR=$(espix_mktemp_dir)
    mkfifo "$DEV_SESSION_DIR/in" "$DEV_SESSION_DIR/out"

    "$ESPIX_PYTHON" "$ESPIX_LIB_DIR/session.py" \
        --host "$ESPIX_HOST" --user "$ESPIX_USER" --password "$ESPIX_PASS" \
        --login-timeout "$ESPIX_LOGIN_TIMEOUT" \
        < "$DEV_SESSION_DIR/in" > "$DEV_SESSION_DIR/out" \
        2> "$DEV_SESSION_DIR/err" &
    DEV_SESSION_PID=$!

    # Order matters: opening a FIFO blocks until the other end is open, and
    # python has both by now.
    exec 9> "$DEV_SESSION_DIR/in"
    exec 8< "$DEV_SESSION_DIR/out"

    # First thing session.py emits is the prompt it synced on, so a suite can
    # assert on the sigil without having to see one in command output (dev_run
    # strips them, and should).
    DEV_SESSION_DEAD=""
    DEV_PROMPT=""

    local line
    while IFS= read -r line <&8; do
        case "$line" in
            '<<<ESPIX-PROMPT '*)
                DEV_PROMPT="${line#<<<ESPIX-PROMPT }"
                DEV_PROMPT="${DEV_PROMPT%>>>}"
                break ;;
            '<<<ESPIX-ERROR '*)
                break ;;
        esac
    done

    # No prompt means no session. This used to fall out of the loop on EOF and
    # carry on with DEV_PROMPT empty, so the suite ran its whole way through
    # against a login that never happened.
    if [ -z "$DEV_PROMPT" ]; then
        DEV_SESSION_DEAD=yes
        printf '  %s could not open a session to %s\n' \
               "$(_espix_red FAIL)" "$ESPIX_HOST" >&2
        if [ -s "$DEV_SESSION_DIR/err" ]; then
            sed 's/^/       /' "$DEV_SESSION_DIR/err" >&2
        fi
        return 1
    fi
    return 0
}

dev_session_stop() {
    [ -n "$DEV_SESSION_PID" ] || return 0
    exec 9>&- 2>/dev/null
    exec 8<&- 2>/dev/null
    wait "$DEV_SESSION_PID" 2>/dev/null
    rm -rf "$DEV_SESSION_DIR"
    DEV_SESSION_PID=""
    DEV_SESSION_DIR=""
}

# dev_run <command> -- run it in the shared session, echo its output.
#
# No exit status: espix's shell has no $?, so use dev_status when the number is
# the thing being asserted.
dev_run() {
    # SIGPIPE ignored, and the write checked, because dev_run is almost always
    # called as `$(dev_run ...)` -- which is a subshell, and that has two
    # consequences that between them made a lost session invisible:
    #
    #  - Writing to the FIFO after session.py is gone killed the subshell with
    #    SIGPIPE (status 141) before a single line below could run, and command
    #    substitution renders a killed subshell as the empty string. Which is
    #    precisely what several assertions in 15-streams.sh expect, so losing
    #    the session made them pass. Measured, not surmised: the first version
    #    of this guard did not work for exactly this reason.
    #  - Nothing assigned in here outlives the call, so "the session is dead"
    #    cannot be remembered between calls. Each one has to find out for
    #    itself, which is why the check is on the write rather than on a flag.
    if [ -n "$DEV_SESSION_DEAD" ] || [ -z "$DEV_SESSION_PID" ] || dev_aborted; then
        printf '%s' "$DEV_DEAD"
        return 1
    fi

    local line out="" closed="" why=""

    # The trap is scoped to the write, in its own subshell, rather than set for
    # the whole function: `trap '' PIPE` here would otherwise persist in the
    # *calling* shell whenever dev_run is used outside `$( )`, quietly changing
    # how every later pipeline in the run behaves. The write is the only place
    # SIGPIPE can arrive -- everything after it reads.
    if ! ( trap '' PIPE; printf '%s\n' "$1" >&9 ) 2>/dev/null; then
        printf '  %s session gone before: %s\n' "$(_espix_red FAIL)" "$1" >&2
        printf '%s' "$DEV_DEAD"
        return 1
    fi
    # Frame markers come from session.py; read to the closing one.
    while IFS= read -r line <&8; do
        case "$line" in
            '<<<ESPIX-CMD '*)   continue ;;
            '<<<ESPIX-END '*)   closed=yes; break ;;
            '<<<ESPIX-ERROR '*) why="${line#<<<ESPIX-ERROR }"; why="${why%>>>}" ;;
            *) out="$out$line
" ;;
        esac
    done

    # An unclosed frame means the session went away mid-command -- session.py
    # timed out, or the connection dropped. Returning the accumulated output
    # (usually nothing) would report that as a command which printed nothing.
    if [ -z "$closed" ]; then
        DEV_SESSION_DEAD=yes
        printf '  %s session lost while running: %s\n' \
               "$(_espix_red FAIL)" "$1" >&2
        [ -n "$why" ] && printf '       %s\n' "$why" >&2
        printf '%s' "$DEV_DEAD"
        return 1
    fi

    # A framed error with a closed frame: the command itself was refused (an
    # unsupported `;`, say). The session is still good.
    if [ -n "$why" ]; then
        printf '%s' "$DEV_DEAD"
        return 1
    fi

    printf '%s' "$out" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

# ---------------------------------------------------------------- one-shot ---

# The askpass dance, in one place. SSH_ASKPASS_REQUIRE=force makes ssh take the
# password from the helper *instead of* the terminal -- right for these one-shot
# calls, and wrong for an interactive session, where it answers the prompt with
# whatever it was told before expect gets a look in. session.py unsets all three
# for exactly that reason.
DEV_ASKPASS=""

dev_askpass_init() {
    local dir
    dir=$(espix_mktemp_dir)
    chmod 700 "$dir"
    DEV_ASKPASS="$dir/askpass.sh"
    # The password goes in the file rather than through the environment: ssh
    # runs this helper itself, and one less thing has to survive that hop. The
    # trailing newline matters -- without it ssh waits for the rest of the line.
    printf '#!/bin/sh\necho %s\n' "$ESPIX_PASS" > "$DEV_ASKPASS"
    chmod 700 "$DEV_ASKPASS"
}

dev_askpass_cleanup() {
    [ -n "$DEV_ASKPASS" ] && rm -rf "$(dirname "$DEV_ASKPASS")"
    DEV_ASKPASS=""
}

# Deliberately no -o LogLevel=ERROR anywhere below: it suppresses the one line
# that explains a dropped connection, and reading rc=255 with empty stderr has
# already sent this project down a wrong path once.
DEV_SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"

# How long session.py waits for a login, as against for a command.
#
# They used to be the same 25 seconds. Separating them is a budget correction
# and not, as first supposed, the cure for anything: a login costs a key
# exchange plus PBKDF2 at 20 000 iterations and gets slower when several happen
# together, while a command that takes 25 seconds is a fault and should still be
# reported as one.
#
# Measured on this device, because the guess it replaced was wrong and worth
# recording as wrong. One login on an idle device: 3986 ms. Five simultaneous
# logins: 8710, 10843, 11312, 11649, 11653 ms. A login while four other
# connections stream `top -b`: 4307, 4114, 4040 ms -- that load is not the
# expensive part; the concurrent key exchanges are.
#
# So the worst measured case is under twelve seconds against a budget of
# twenty-five, which means the intermittent "could not open a session" seen
# while building the pool -- once in ten rounds, then not once in twelve -- is
# NOT explained by this. It is still open. The budget goes up because half of
# it was already being used on the ordinary path, not because doing so fixed
# that.
: "${ESPIX_LOGIN_TIMEOUT:=60}"

# ConnectTimeout bounds the *connect* and nothing after it, so a device that
# accepts a connection and then never answers held a run open indefinitely --
# dev_health_begin calls dev_once twice before the first suite, with no output
# to say what it was waiting for. These are the whole-command deadlines.
: "${ESPIX_SSH_TIMEOUT:=30}"
: "${ESPIX_SCP_TIMEOUT:=90}"

# --------------------------------------------------- the one-shot connection budget ---
#
# How many connections *besides* the long-lived sessions may be open at once.
#
# The device allows CONFIG_ESPIX_SSH_MAX_SESSIONS of them, eight. A parallel run
# already holds one per worker plus one for the health monitor, so at -j 4 that
# is five, and every dev_status, dev_once, scp and sftp wants a sixth. Several
# suites reaching for one at the same moment take the device past eight, and it
# refuses -- correctly, and with a clear message, but a refused `dev_status`
# reads as `exit status 255` and a refused `ssh app < file` as an empty answer.
# A run measured five such failures across two suites, all of them "passes
# alone".
#
# Retrying is not enough on its own: the retry sits in _dev_ssh below and is
# bounded, and a busy patch outlasts it. So the host rations them instead, and
# keeps the retry as the backstop for whatever slips through -- including the
# person watching `top` from another window, whom the arithmetic cannot see.
#
# K token directories, because mkdir is atomic and bash 3.2 has nothing better.
: "${ESPIX_ONESHOT_MAX:=2}"

_dev_oneshot_acquire() {
    [ -n "$ESPIX_RUNDIR" ] || return 0

    local waited=0 i
    while :; do
        i=1
        while [ "$i" -le "$ESPIX_ONESHOT_MAX" ]; do
            if mkdir "$ESPIX_RUNDIR/oneshot.$i.lock" 2>/dev/null; then
                DEV_ONESHOT_TOKEN=$i
                return 0
            fi
            i=$((i + 1))
        done
        sleep 0.2
        waited=$((waited + 1))
        if [ "$waited" -gt 300 ]; then     # a minute: go anyway
            DEV_ONESHOT_TOKEN=""
            return 0
        fi
    done
}

_dev_oneshot_release() {
    [ -n "$ESPIX_RUNDIR" ] || return 0
    [ -n "${DEV_ONESHOT_TOKEN:-}" ] || return 0
    rmdir "$ESPIX_RUNDIR/oneshot.$DEV_ONESHOT_TOKEN.lock" 2>/dev/null
    DEV_ONESHOT_TOKEN=""
    return 0
}

DEV_ONESHOT_TOKEN=""

# ----------------------------------------------------------- the transfer lock ---
#
# One transfer in flight at a time across the whole run.
#
# Concurrent SFTP transfers break, and have since long before there was a
# parallel runner (docs/KNOWN-ISSUES.md; proven pre-existing by a control with
# an ordinary file). Running suites in parallel makes that reachable by
# accident, and a default run that goes red on a known bug it is not testing
# teaches everyone to ignore the colour.
#
# So it is a guard rather than a bottleneck -- today exactly one pool suite
# transfers -- and ESPIX_OVERLAP_TRANSFERS removes it for anyone who wants the
# bug rather than the run.
#
# A directory, because mkdir is atomic on APFS and on ext4 and bash 3.2 has no
# other portable primitive. The wait is bounded and then simply takes the lock:
# a worker killed mid-transfer would otherwise wedge every later one, and a
# stuck run that says nothing is worse than an overlapping transfer.
_dev_xfer_lock() {
    [ -n "$ESPIX_RUNDIR" ] || return 0
    [ "$ESPIX_OVERLAP_TRANSFERS" = 1 ] && return 0

    local waited=0
    while ! mkdir "$ESPIX_RUNDIR/xfer.lock" 2>/dev/null; do
        sleep 0.2
        waited=$((waited + 1))
        if [ "$waited" -gt 900 ]; then      # three minutes
            printf 'transfer lock held for 180s; taking it\n' \
                >> "$ESPIX_RUNDIR/notes" 2>/dev/null
            return 0
        fi
    done
    return 0
}

_dev_xfer_unlock() {
    [ -n "$ESPIX_RUNDIR" ] || return 0
    [ "$ESPIX_OVERLAP_TRANSFERS" = 1 ] && return 0
    rmdir "$ESPIX_RUNDIR/xfer.lock" 2>/dev/null
    return 0
}

# ------------------------------------------------------------ refused, or gone ---
#
# ssh exits 255 and says "Connection closed by <host> port 22" both when the
# device refuses a connection for capacity and when one genuinely drops. The
# difference matters: the first is contention and should be retried, the second
# is a fault and must not be papered over.
#
# espix says which, and says it on the wire before the version string -- RFC
# 4253 4.2 allows exactly that, which is how OpenSSH reports "Exceeded
# MaxStartups". OpenSSH does not show such a line without -v, so read it from a
# bare socket instead. Confirmed on the device:
#
#   b'espix: too many connections (8 of 8 in use)\r\n'
#
# The probe costs a TCP connection and no session slot: a refused connection
# never becomes one.
# The greeting espix sends before its version string, read from a bare socket.
# Empty when the server has nothing to say, which is the ordinary case.
dev_banner() {
    "$ESPIX_PYTHON" - "$ESPIX_HOST" <<'PY' 2>/dev/null
import socket, sys
try:
    s = socket.create_connection((sys.argv[1], 22), 5)
    s.settimeout(5)
    sys.stdout.write(s.recv(200).decode("utf-8", "replace"))
    s.close()
except Exception:
    pass
PY
}

_dev_refused_for_capacity() {
    case "$(dev_banner)" in
        *"too many connections"*) return 0 ;;
        *)                        return 1 ;;
    esac
}

# Retries are counted rather than hidden. A run that needed a few is healthy
# contention; a run that needed dozens is a worker count the device cannot feed,
# and the report says so.
_dev_note_retry() {
    [ -n "$ESPIX_RUNDIR" ] || return 0
    printf 'w%s\n' "$ESPIX_WORKER" >> "$ESPIX_RUNDIR/retries" 2>/dev/null
    return 0
}

DEV_SSH_MAX_RETRIES=4

# The environment and the deadline, in one place.
#
# espix_timeout is handed the ssh *binary*, through env, and never a shell
# function: `"$@" &` on a function gives back a subshell pid, and killing that
# leaves ssh alive holding one of espix's four connection slots. run.sh's
# preflight did exactly that, and the orphans accumulate until the device
# answers nobody. See the note in portable.sh.
_dev_ssh_once() {
    espix_timeout "$ESPIX_SSH_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS "$ESPIX_USER@$ESPIX_HOST" "$@"
}

# Retried only when the device itself says it is full; every other 255 is
# returned as it stands. A refused connection produced no output, so retrying
# cannot duplicate any.
_dev_ssh() {
    local rc tries=0
    dev_aborted && return 255
    _dev_oneshot_acquire
    while :; do
        _dev_ssh_once "$@"
        rc=$?
        if [ "$rc" != 255 ] \
           || [ "$tries" -ge "$DEV_SSH_MAX_RETRIES" ] \
           || dev_aborted \
           || ! _dev_refused_for_capacity; then
            _dev_oneshot_release
            return $rc
        fi
        tries=$((tries + 1))
        _dev_note_retry
        sleep 2
    done
}

_dev_scp_once() {
    espix_timeout "$ESPIX_SCP_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        scp $DEV_SSH_OPTS "$@"
}

_dev_scp() {
    local rc tries=0
    dev_aborted && return 255
    _dev_oneshot_acquire
    _dev_xfer_lock
    while :; do
        _dev_scp_once "$@"
        rc=$?
        if [ "$rc" = 255 ] && [ "$tries" -lt "$DEV_SSH_MAX_RETRIES" ] \
           && ! dev_aborted && _dev_refused_for_capacity; then
            tries=$((tries + 1))
            _dev_note_retry
            sleep 2
            continue
        fi
        break
    done
    _dev_xfer_unlock
    _dev_oneshot_release
    return $rc
}

# dev_ssh_raw <command> [args...] -- own connection, output unfiltered.
#
# For the handful of assertions that have to see exactly what the *client*
# printed -- which stream a diagnostic arrived on, what `2>/dev/null` swallows --
# and for the ones that pipe a file into a process's stdin.
#
# It exists because 15-streams.sh and 45-throughput.sh had each grown their own
# copy of the same four lines, and a private copy of a connection helper is a
# copy that does not take the one-shot budget, does not retry a refusal, and
# does not stop when the run has aborted. Two of the three failures that made a
# parallel run red were exactly that.
dev_ssh_raw() {
    _dev_ssh "$@"
}

# dev_status <command> -- run it in its own connection, return its exit status.
dev_status() {
    _dev_ssh "$1" >/dev/null 2>&1
}

# Everything below prints the command's output with ssh's host-key notice
# filtered out, and returns the *command's* status.
#
# Not a pipeline into grep, which is how the first version was written: a
# transfer that succeeds silently produces no output, grep finds nothing and
# exits 1, and the helper reports failure for a copy that worked perfectly.
# Under `set -o pipefail` it is worse still.
_dev_filter() {
    printf '%s' "$1" | grep -v '^Warning: Permanently added' || true
}

# dev_once <command> -- own connection, echo output, discard status.
dev_once() {
    local out
    out=$(_dev_ssh "$1" 2>&1)
    _dev_filter "$out"
}

dev_push() {
    local out rc
    out=$(_dev_scp "$1" "$ESPIX_USER@$ESPIX_HOST:$2" 2>&1)
    rc=$?
    _dev_filter "$out"
    return $rc
}

dev_pull() {
    local out rc
    out=$(_dev_scp "$ESPIX_USER@$ESPIX_HOST:$1" "$2" 2>&1)
    rc=$?
    _dev_filter "$out"
    return $rc
}

# dev_sftp <batchfile> -- BatchMode=no is not optional: `sftp -b` turns batch
# mode on, and batch mode disables password authentication, so the whole thing
# fails at the login with a message about permissions that looks like the
# device refusing you.
dev_sftp() {
    local rc
    dev_aborted && return 255
    _dev_oneshot_acquire
    _dev_xfer_lock
    espix_timeout "$ESPIX_SCP_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        sftp $DEV_SSH_OPTS -o BatchMode=no -b "$1" \
        "$ESPIX_USER@$ESPIX_HOST" 2>&1
    rc=$?
    _dev_xfer_unlock
    _dev_oneshot_release
    return $rc
}

# dev_capture <remote command> <local file>
#
# Writes on the device and fetches the file, rather than reading a long answer
# back over the channel. Output big enough to matter is exactly what stresses
# the transport being tested, so reading it inline makes the transport part of
# the measurement.
dev_capture() {
    # Per worker: two of these running at once would otherwise each fetch the
    # other's file, and the comparison that follows would be of the wrong thing
    # rather than obviously broken.
    local remote="/tmp/.espix-capture.$ESPIX_WORKER"

    dev_once "$1 > $remote" >/dev/null
    dev_pull "$remote" "$2" >/dev/null
    dev_once "rm $remote" >/dev/null

    # Empty is a failure, not a result. Two things produce it and both are
    # silent: a command that wrote nothing, and -- the one that caught me -- a
    # *loaded app*, whose output cannot be redirected at all. espix points an
    # app's stdout at the session rather than the redirect FILE, deliberately
    # (see the note in espix_proc/exec.c: the FILE is closed when the command
    # returns and a backgrounded app would outlive it).
    #
    # Without this check, comparing two captured app outputs compares two empty
    # files and reports that they match.
    if [ ! -s "$2" ]; then
        echo "dev_capture: '$1' produced nothing -- a loaded app's output" \
             "cannot be redirected to a file; read it over the channel" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------- console ---

# dev_console_run <cmd> [<cmd>...] -- run commands on the serial console.
#
# One python process per call rather than a long-lived one like the SSH side:
# the console suite is small, there is only one port so nothing can overlap
# anyway, and a second FIFO pair to manage would cost more than it saves. Pass
# several commands in one call if it matters.
# One console.py for the whole suite, started on first use.
#
# It used to be one process per command, and that was the cause of the console
# suite's long-standing flakiness rather than a symptom of it. Closing and
# reopening the port ends the device's console session; espix starts another,
# which probes the terminal for its cursor position -- and console.py is the
# thing that answers that probe, so a probe landing between two invocations has
# nobody to answer it, times out ("terminal does not answer cursor queries"),
# and the next console.py syncs against a session that is mid-probe rather than
# at a prompt. dmesg caught it in the act: `console session on uart` appearing
# in the middle of a run that never rebooted.
#
# Holding the port for the suite removes the open/close cycle entirely, which is
# the same reason session.py is held for the SSH side.
DEV_CONSOLE_DIR=""
DEV_CONSOLE_PID=""

dev_console_start() {
    [ -n "$ESPIX_PORT" ] || return 1
    [ -z "$DEV_CONSOLE_PID" ] || return 0

    DEV_CONSOLE_DIR=$(espix_mktemp_dir)
    mkfifo "$DEV_CONSOLE_DIR/in" "$DEV_CONSOLE_DIR/out"

    "$ESPIX_PYTHON" "$ESPIX_LIB_DIR/console.py" --port "$ESPIX_PORT" \
        < "$DEV_CONSOLE_DIR/in" > "$DEV_CONSOLE_DIR/out" \
        2> "$DEV_CONSOLE_DIR/err" &
    DEV_CONSOLE_PID=$!

    # Order matters: opening a FIFO blocks until the other end is open.
    exec 7> "$DEV_CONSOLE_DIR/in"
    exec 6< "$DEV_CONSOLE_DIR/out"
    return 0
}

dev_console_stop() {
    [ -n "$DEV_CONSOLE_PID" ] || return 0
    exec 7>&- 2>/dev/null
    exec 6<&- 2>/dev/null
    wait "$DEV_CONSOLE_PID" 2>/dev/null
    rm -rf "$DEV_CONSOLE_DIR"
    DEV_CONSOLE_PID=""
    DEV_CONSOLE_DIR=""
}

dev_console_run() {
    [ -n "$ESPIX_PORT" ] || { echo "dev_console_run: no serial port"; return 1; }

    if [ -z "$DEV_CONSOLE_PID" ] && ! dev_console_start; then
        echo "dev_console_run: could not open $ESPIX_PORT"
        return 1
    fi

    # SIGPIPE ignored around the write for the same reason dev_run does it: this
    # runs inside `$( )`, and writing to a FIFO whose reader has gone kills that
    # subshell before any guard can report why, which command substitution then
    # renders as an empty string.
    local cmd line out="" closed=""
    for cmd in "$@"; do
        if ! ( trap '' PIPE; printf '%s\n' "$cmd" >&7 ) 2>/dev/null; then
            echo "dev_console_run: the console session is gone"
            DEV_CONSOLE_PID=""
            return 1
        fi

        closed=""
        while IFS= read -r line <&6; do
            case "$line" in
                '<<<ESPIX-CMD '*) continue ;;
                '<<<ESPIX-END '*) closed=yes; break ;;
                *) out="$out$line
" ;;
            esac
        done

        if [ -z "$closed" ]; then
            # EOF without a closing frame: console.py died mid-command. Its
            # stderr is the only place that says why, so print it rather than
            # returning silence.
            echo "dev_console_run: the console stopped answering"
            [ -s "$DEV_CONSOLE_DIR/err" ] && sed 's/^/       /' "$DEV_CONSOLE_DIR/err"
            DEV_CONSOLE_PID=""
            return 1
        fi
    done

    printf '%s' "$out" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
    return 0
}

# --------------------------------------------------------------- test app ---

DEV_TESTAPP="/home/$ESPIX_USER/testapp"
DEV_TESTAPP_SHA="/home/$ESPIX_USER/.testapp.sha"

# Is the test app on the device? A read, and nothing else.
#
# The suites used to call dev_testapp_sync() themselves -- four of them did --
# which was fine while they ran one after another and is a race as soon as they
# do not: two workers deciding to push the same path at the same moment. The
# push now happens once, in run.sh's preflight, before any worker exists. What
# is left for a suite is this question, which is safe to ask from anywhere.
dev_testapp_present() {
    case "$(dev_run "ls -l $DEV_TESTAPP")" in
        *"no such file"*|*"No such file"*|"$DEV_DEAD"|'') return 1 ;;
        *) return 0 ;;
    esac
}

# Put the test app on the device, but only when the one there is not the one we
# just built. espix has no checksum command among its 48, so the hash travels
# with the binary in a sidecar and `cat` does the comparison.
#
# Called once from the runner's preflight; see dev_testapp_present() above for
# why not from the suites.
dev_testapp_sync() {
    local local_elf="$1" want have

    [ -f "$local_elf" ] || { echo "no test app at $local_elf"; return 1; }
    want=$(espix_sha256 "$local_elf")
    have=$(dev_run "cat $DEV_TESTAPP_SHA" 2>/dev/null)

    if [ "$have" = "$want" ]; then
        # Confirm the binary is actually there: a sidecar can outlive its
        # binary if someone deleted one and not the other.
        case "$(dev_run "ls -l $DEV_TESTAPP")" in
            *"no such file"*|*"No such file"*) ;;
            *) return 0 ;;
        esac
    fi

    # Binary first, sidecar second. An interrupted copy then leaves a stale
    # sidecar and the next run copies again -- self-healing, where the other
    # order leaves a wrong binary vouched for by a correct hash.
    dev_push "$local_elf" "$DEV_TESTAPP" >/dev/null || return 1

    local tmp
    tmp=$(espix_mktemp_dir)
    printf '%s\n' "$want" > "$tmp/sha"
    dev_push "$tmp/sha" "$DEV_TESTAPP_SHA" >/dev/null
    rm -rf "$tmp"
    return 0
}

# ----------------------------------------------------------------- health ---
#
# Asked after every suite, because the most expensive bug in this project's
# history looked like a WiFi assert and an ipc0 fault and was actually a
# filesystem recursion -- found only once somebody asked the device how it was.
# Nobody remembers to ask, so the runner does.

DEV_HEALTH_REASON=""
DEV_HEALTH_MINUTES=-1
DEV_HEALTH_CONNS=1
DEV_HEALTH_WDT=0
DEV_HEALTH_HEAPMIN=-1

# espix prints one of three shapes (espix_kernel/kernel.c):
#   up N min          up H:MM          up N days, H:MM
#
# The 10# prefixes are not decoration. `up 5:08` gives mins="08", and bash reads
# a leading zero as octal, where 8 is not a digit -- so the arithmetic failed
# outright with "value too great for base" on any run that happened to start in
# the first nine minutes of an hour. It printed one line to stderr and returned
# nothing, which the reboot check then compared against.
# Turned into minutes so a *decrease* can be spotted, which is the only
# dependable sign of a reboot: comparing the reset reason alone misses the
# common case, since two software reboots in a row read identically.
#
# Minute granularity, so a reboot and recovery inside the same minute could
# slip past. Suites take tens of seconds, and the core-dump check covers the
# crash case regardless.
_dev_parse_uptime() {
    local line="$1" days hours mins

    #
    # A capture that fails answers -1, never 0. This mattered:
    #
    #     _dev_parse_uptime "min free internal since boot: 106 K"   ->  0
    #
    # That is a line from `free`, and under load a session can hand one query
    # another command's output -- the framing slip 50-console already documents
    # for the serial console. It matched *min*, the `^up N min` capture came back
    # empty, ${mins:-0} made it zero, and dev_health_check read a device that had
    # been up 24 minutes as freshly rebooted. Three findings invented from one
    # mis-framed reply: reset-reason-changed, rebooted, coredump-unrecognised.
    #
    # Which is the lesson already written down a few functions below, about the
    # core-dump check: a check that invents findings is worse than no check,
    # because the next real one gets waved away too. -1 means "no answer" and
    # every caller already guards on it.
    case "$line" in
        *day*)
            days=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\) day.*/\1/p')
            hours=$(printf '%s' "$line" | sed -n 's/.*, \([0-9]*\):[0-9]*.*/\1/p')
            mins=$(printf '%s' "$line" | sed -n 's/.*:\([0-9]*\),.*/\1/p')
            [ -n "$mins" ] || mins=$(printf '%s' "$line" | sed -n 's/.*:\([0-9]*\).*/\1/p')
            if [ -z "$days" ] || [ -z "$hours" ] || [ -z "$mins" ]; then
                printf '%s' "-1"
            else
                printf '%s' "$(( 10#$days * 1440 + 10#$hours * 60 + 10#$mins ))"
            fi ;;
        *min*)
            mins=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\) min.*/\1/p')
            if [ -z "$mins" ]; then
                printf '%s' "-1"
            else
                printf '%s' "$(( 10#$mins ))"
            fi ;;
        *:*)
            hours=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\):[0-9]*.*/\1/p')
            mins=$(printf '%s' "$line" | sed -n 's/^up [0-9]*:\([0-9]*\).*/\1/p')
            if [ -z "$hours" ] || [ -z "$mins" ]; then
                printf '%s' "-1"
            else
                printf '%s' "$(( 10#$hours * 60 + 10#$mins ))"
            fi ;;
        *)
            printf '%s' "-1" ;;
    esac
}

# Ask through the shared session when there is one, and pay for a connection
# only when there is not.
#
# The health questions used to be one-shot logins without exception, three of
# them before the first suite and four after every one -- roughly three minutes
# of a ten-minute run spent logging in to ask how things are. They are ordinary
# read-only commands and there is nothing about them that needs its own
# connection.
_dev_ask() {
    local out
    if [ -n "$DEV_SESSION_PID" ] && [ -z "$DEV_SESSION_DEAD" ]; then
        out=$(dev_run "$1")
        if [ "$out" != "$DEV_DEAD" ]; then
            printf '%s' "$out"
            return 0
        fi
    fi
    dev_once "$1"
}

dev_uptime_minutes() {
    _dev_parse_uptime "$(_dev_ask 'uptime')"
}

# Everything below parses one `uptime` line rather than fetching its own.
#
# Not a tidy-up: fetching separately is a race, and it bit. dev_health_begin took
# the reset reason, the minutes and the watchdog count in three calls, each of
# which is a login -- and on this device a login is ~3.5s. A watchdog trigger
# landing between the first and third made the baseline disagree with itself:
# the reason said "6 warnings", the count said 7, and the end-of-run check then
# reported a *reset reason change* that had not happened while missing the
# watchdog event that had. One read, parsed three ways, cannot do that.
_dev_parse_reason() {   # <uptime output>
    # Stops at the first comma so the watchdog suffix is not swallowed into the
    # reason -- "power-on, 7 watchdog warnings" is not a reset reason.
    printf '%s' "$1" | sed -n 's/.*last reset: \([^,]*\).*/\1/p'
}

dev_health_reason() {
    _dev_parse_reason "$(_dev_ask 'uptime')"
}

# Task watchdog triggers since the device booted, from the tail of `uptime`:
#
#     up 5 min, last reset: power-on, 2 watchdog warnings
#
# Absent when the count is zero, which is why this answers 0 rather than failing
# on a line that does not have it -- a healthy device and an old firmware look
# the same here, and neither is a finding.
#
# Read from `uptime` rather than from `dmesg` deliberately. The klog ring is
# CONFIG_ESPIX_KLOG_LINES entries and four workers churn it in well under a
# minute, so a ten-second poll can arrive after the evidence has scrolled away.
# The count on the device is cumulative and cannot be missed by a late reader.
_dev_parse_wdt() {      # <uptime output>
    local n
    n=$(printf '%s' "$1" | sed -n 's/.*, \([0-9][0-9]*\) watchdog warning.*/\1/p')
    case "${n:-0}" in
        ''|*[!0-9]*) echo 0 ;;
        *)           echo "$n" ;;
    esac
}

dev_wdt_count() {
    _dev_parse_wdt "$(_dev_ask 'uptime')"
}

# Which task was holding a core, from the parenthesised tail of `uptime`:
#
#     up 5 min, last reset: power-on, 1 watchdog warning (app:testapp)
#
# Empty when the device reports no name, which older firmware does not.
_dev_parse_wdt_task() {     # <uptime output>
    printf '%s' "$1" | sed -n 's/.*watchdog warning[s]* (\([^)]*\)).*/\1/p'
}

# The internal heap's low-water mark, from `free`:
#
#     min free internal since boot: 0 K
#
# Cumulative since boot and never recovers, which is what makes two reads enough
# where sampling would not be: a run that dipped to nothing for a tenth of a
# second and recovered looks perfectly healthy in every other number, and this is
# the only one that remembers. Taken at the start and the end, the pair says
# whether *this* run is what pushed it down.
#
# It earned its place. A run under heap poisoning failed three assertions in ways
# that read as three unrelated bugs -- an ESP_ERR_NO_MEM spawning an app, a
# session dying mid-command with status 255, and 55-sessions' 20K floor -- and
# all three were one fact: the internal heap had reached zero. Nothing in the
# run's own output said so. Finding it meant going and asking the board by hand
# afterwards, which only happens when somebody already suspects it.
#
# -1, never 0, when there is no answer. Zero is a real and alarming reading here,
# so a failed parse that returned it would invent the very finding this exists to
# report. That mistake has been made in this file before, in _dev_parse_uptime.
_dev_parse_free_min() {     # <free output>
    local n
    n=$(printf '%s' "$1" |
        sed -n 's/.*min free internal since boot: *\([0-9][0-9]*\) *K.*/\1/p' |
        head -1)
    case "${n:-}" in
        ''|*[!0-9]*) printf '%s' "-1" ;;
        *)           printf '%s' "$n" ;;
    esac
}

# Tasks that have exited and whose stacks IDLE has not freed yet.
#
# Worth reporting beside any heap figure, because it is the difference between
# "this memory is gone" and "this memory has not come back yet". ps lists these:
# uxTaskGetSystemState() walks xTasksWaitingTermination as eDeleted, which
# cmd_sys.c prints as 'D'. 55-sessions read that column as a live session once
# and reported reclamation running late as a 12K leak.
#
# -1 and never 0 on a failed parse, for the reason _dev_parse_free_min gives
# below: zero is the reassuring answer here, so inventing it on a bad read hides
# exactly what this is for.
_dev_parse_tasks_deleted() {    # <ps output>
    local n
    case "${1:-}" in
        ''|"$DEV_DEAD") printf '%s' "-1"; return ;;
    esac
    # A ps listing has its header; anything without it is not one.
    case "$1" in
        *NAME*) ;;
        *) printf '%s' "-1"; return ;;
    esac
    n=$(printf '%s' "$1" | awk '$3 == "D"' | grep -c .)
    case "${n:-}" in
        ''|*[!0-9]*) printf '%s' "-1" ;;
        *)           printf '%s' "$n" ;;
    esac
}

dev_tasks_deleted() {
    _dev_parse_tasks_deleted "$(_dev_ask 'ps')"
}

# Internal heap "used" and block count, as one pair, from a `free` reading.
#
# Two numbers because they answer different questions about the same growth: a
# run that adds 4K in one block and a run that adds 4K in forty are not the same
# bug, and the block count is already sitting in the same row.
#
# "-1 -1" and never "0 0" on a failed parse, for the reason _dev_parse_free_min
# spells out above -- a zero here reads as a real and dramatic finding, so a
# parser that invents one on a bad line is worse than no parser.
_dev_parse_free_used() {    # <free output> -> "<usedK> <blocks>"
    local line used blocks
    line=$(printf '%s' "$1" | sed -n 's/^internal  *//p' | head -1)
    used=$(printf '%s' "$line" | awk '{print $2}')
    blocks=$(printf '%s' "$line" | awk '{print $5}')
    case "${used:-}${blocks:-}" in
        ''|*[!0-9]*) printf '%s' "-1 -1" ;;
        *)           printf '%s %s' "$used" "$blocks" ;;
    esac
}

# Is a watchdog trigger espix's fault?
#
# WHY THIS DOES NOT FAIL A RUN, since restoring that is the obvious "fix".
#
# The watchdog fires when an IDLE task has not run for five seconds, which means
# a core was busy -- not that anything is stuck. It cannot tell saturation from
# a stall, and espix is a machine you can legitimately saturate.
#
# A yield does not help, which is the part that catches people out (it caught
# me): IDLE runs only when *nothing else* is ready, so blocking one busy task
# just hands the core to the next one. There is no sprinkling of vTaskDelay that
# makes IDLE run while there is real work queued.
#
# WHAT IT IS, now that the console has said so.
#
# Arithmetic never settled this -- two derivations from login cost were wrong
# (five concurrent logins is not 6s on one core, there are two; and 55-sessions
# opens SESS_BATCH=4 at a time, not eight). The answer came from the UART
# backtrace the watchdog ISR prints, captured with tools/serlog.sh. Three of four
# triggers in one session decoded to the same stack:
#
#     pbkdf2_sha256 (espix_auth/auth.c) -> hmac_half -> psa_hash_finish
#       -> esp_sha_hash_abort -> free() -> multi_heap_free
#
# PBKDF2 runs 20 000 iterations with no blocking call, and IDF's PSA SHA driver
# allocates and frees an internal DMA buffer inside psa_hash_clone()/abort() on
# every one of them -- around 40 000 heap operations per login, with up to eight
# logins in flight. So it is saturation by a real workload, exactly as the
# paragraphs above reason, and now with the code named.
#
# Two limits on that, kept because they are easy to overstate. The fourth trigger
# decoded to the *wifi* task in pm_tbtt_process -> esp_phy_enable, the modem-sleep
# wake path, not PBKDF2 at all. And the ISR prints CPU 0's backtrace, which is not
# necessarily the core that starved. Three of four is strong; it is not all four.
#
# None of which changes the verdict: worth printing, not worth failing on -- and
# this project already wrote down why, about the connection-count check a few
# functions below:
#
#     A check that fires on correct behaviour gets ignored, which costs more
#     than the check is worth.
#
# So run.sh prints every trigger with its task and the suites in flight, and the
# exit status is left to the assertions. If a *new* name starts appearing there,
# that is the signal this exists for.
#
# A task named "app:something" is a user program using the CPU it was given.
# tests/suites/35-signals.sh runs `testapp sig spin` on purpose -- a compute loop
# with no blocking call, which the suite asserts must work -- so a run that
# failed on that would be failing on its own test design. Anything else means
# espix held a core for five seconds, which is a real finding.
#
# Unknown counts as espix's: a name we cannot read is not a name we can excuse.
# Both cores are reported, so this has to look at each name rather than the
# string as a whole -- "app:testapp, IDLE1" and "IDLE0, app:testapp" are the same
# finding, and a `case app:*` on the whole string only recognises the first.
#
# IDLE names are dropped before judging: a core sitting in IDLE is not what
# starved anything, it is the other core's report.
_dev_wdt_is_ours() {        # <task names, comma separated>
    local rest="${1:-}" name found=0

    [ -n "$rest" ] || return 0      # no name reported: assume ours

    while [ -n "$rest" ]; do
        name=${rest%%,*}
        case "$rest" in *,*) rest=${rest#*,} ;; *) rest="" ;; esac
        name=$(printf '%s' "$name" | sed 's/^ *//; s/ *$//')

        case "$name" in
            IDLE*|'') continue ;;   # the other core, not the culprit
            app:*)    found=1 ;;    # a program using the CPU it was given
            *)        return 0 ;;   # an espix task held a core: ours
        esac
    done

    # Only apps named, or only IDLE. The former is not a fault; the latter is
    # unexplained, and unexplained counts as ours.
    [ "$found" = 1 ] && return 1
    return 0
}

dev_health_begin() {
    local up
    up=$(_dev_ask 'uptime')

    DEV_HEALTH_REASON=$(_dev_parse_reason "$up")
    DEV_HEALTH_MINUTES=$(_dev_parse_uptime "$up")

    # Relative to whatever the device had already, for the same reason the
    # connection count is: a trigger from before this run started is not this
    # run's finding. Parsed from the same line as the two above so the three
    # cannot describe different moments -- see _dev_parse_reason.
    DEV_HEALTH_WDT=$(_dev_parse_wdt "$up")
    if [ "${DEV_HEALTH_WDT:-0}" -gt 0 ]; then
        printf '  %s %d watchdog warning(s) already recorded; counting from there\n' \
               "$(_espix_dim note:)" "$DEV_HEALTH_WDT"
    fi

    # The heap low-water mark is cumulative since boot and never recovers, so
    # without a baseline the first run that touches zero makes every later run
    # on that boot report zero too. That is the failure mode this file already
    # names twice: a line that is always there stops being read, and the run
    # where it *first* went red is the one that mattered.
    DEV_HEALTH_HEAPMIN=$(_dev_parse_free_min "$(_dev_ask 'free')")
    if [ "${DEV_HEALTH_HEAPMIN:--1}" -eq 0 ]; then
        printf '  %s the internal heap has already been to zero on this boot\n' \
               "$(_espix_dim note:)"
    fi

    # Whatever is already connected before the run starts is somebody else's,
    # and the leak check below is measured against it rather than against zero.
    DEV_HEALTH_CONNS=$(_dev_ask 'ps' | grep -c 'sshd:conn')
    if [ "${DEV_HEALTH_CONNS:-0}" -gt 1 ]; then
        # "besides this one": the count includes the connection asking the
        # question, so a single session held elsewhere reads as 2 and looks
        # like somebody else has two open.
        printf '  %s %d connection(s) open besides this one; leak detection is relative to that\n' \
               "$(_espix_dim note:)" "$(( DEV_HEALTH_CONNS - 1 ))"
    fi
}

# The end-of-run check: did the device reboot, dump core, or leak connection
# tasks? Called once, after every worker and the monitor have stopped -- which
# is the only moment the connection count means anything, since a running pool
# holds one session per worker by design.
#
# The reboot and core-dump halves are watched continuously by dev_monitor_run()
# while the run is in progress; this is the backstop for anything that happened
# between its last poll and the end.
dev_health_check() {
    local reason mins cores conns wdt wdt_task up problems=""

    up=$(_dev_ask 'uptime')
    reason=$(_dev_parse_reason "$up")
    if [ -n "$DEV_HEALTH_REASON" ] && [ "$reason" != "$DEV_HEALTH_REASON" ]; then
        problems="$problems reset-reason-changed('$DEV_HEALTH_REASON'->'$reason')"
    fi

    mins=$(_dev_parse_uptime "$up")
    if [ "$DEV_HEALTH_MINUTES" -ge 0 ] && [ "$mins" -ge 0 ] \
       && [ "$mins" -lt "$DEV_HEALTH_MINUTES" ]; then
        problems="$problems rebooted(uptime ${DEV_HEALTH_MINUTES}min->${mins}min)"
    fi
    [ "$mins" -ge 0 ] && DEV_HEALTH_MINUTES=$mins

    # Matched on what a stored dump actually prints -- "core dump: N bytes at
    # flash 0x..." -- rather than on the absence of "no core dump stored".
    #
    # The absence test was wrong in the direction that costs the most: an
    # unanswered query is empty, empty does not contain "no core dump", and the
    # run then reports a core dump that is not there. It did exactly that once,
    # on a device that answered "no core dump stored" to the very next question.
    # A check that invents findings is worse than no check, because the next
    # real one gets waved away too.
    cores=$(_dev_ask 'coredump')
    case "$cores" in
        *"core dump: "*)  problems="$problems core-dump-present" ;;
        *"no core dump"*) ;;
        '')               problems="$problems coredump-unanswered" ;;
        *)                problems="$problems coredump-unrecognised" ;;
    esac

    # The watchdog backstop. Cumulative on the device, so this one read catches
    # every trigger in the run including any the monitor's cadence straddled --
    # the monitor exists to say *which suites were running*, not to be the only
    # thing that notices.
    # Reported by run.sh, and deliberately *not* a failure. See the note on
    # _dev_wdt_is_ours() for why: the trigger is saturation, not a stall, and
    # the runner must not go red on the machine doing its job.
    #
    # Still read here rather than dropped, because the count is what proves a
    # trigger happened at all -- the monitor only sees the ones its ten-second
    # poll catches, and this is cumulative on the device.
    wdt=$(_dev_parse_wdt "$up")
    if [ "${wdt:-0}" -gt "${DEV_HEALTH_WDT:-0}" ]; then
        wdt_task=$(_dev_parse_wdt_task "$up")
        printf 'watchdog %d %s\n' "$(( wdt - DEV_HEALTH_WDT ))" \
               "${wdt_task:-unknown}" > "${ESPIX_RUNDIR:-/tmp}/watchdog.total" \
               2>/dev/null || true
    fi

    # How close the internal heap came to nothing. Reported, never fatal, for
    # the same reason as the watchdog: a run may legitimately push the device
    # hard, and a check that goes red on that gets ignored. See
    # _dev_parse_free_min() for what this costs to miss.
    #
    # Through the shared session, so it is one command and not a login.
    local heap_min
    heap_min=$(_dev_parse_free_min "$(_dev_ask 'free')")
    if [ "${heap_min:--1}" -ge 0 ]; then
        # Both numbers, so run.sh can say whether this run is what pushed it
        # down or whether it was already there.
        printf '%s %s\n' "$heap_min" "${DEV_HEALTH_HEAPMIN:--1}" \
               > "${ESPIX_RUNDIR:-/tmp}/heapmin" 2>/dev/null || true
    fi

    # Connection tasks: told apart by persistence and by *growth*, not by count.
    #
    # One is the connection asking the question. A second is usually the
    # previous one still in teardown -- close_gracefully drains until the peer
    # hangs up, bounded by PARTIAL_READ_TIMEOUT_MS at five seconds -- so
    # back-to-back commands routinely show two, and complaining about that would
    # cry wolf on every run. Looking again after the teardown window separates
    # the two: a closing connection is gone by then and a stranded one is not.
    #
    # Counting was still wrong, though, and in the direction that matters: a
    # person watching `top` from another window is indistinguishable from a
    # stranded task by count alone, so an entirely healthy run failed twelve
    # times over with the message admitting it did not know
    # ("someone-logged-in?"). A check that fires on correct behaviour gets
    # ignored, which costs more than the check is worth.
    #
    # The baseline from dev_health_begin() fixes that without weakening it: what
    # is watched for is a connection count *above what was already there*, which
    # is what a leak looks like and what a spectator does not.
    local base="${DEV_HEALTH_CONNS:-1}"
    [ "$base" -lt 1 ] && base=1

    conns=$(_dev_ask 'ps' | grep -c 'sshd:conn')
    if [ "$conns" -gt "$base" ]; then
        sleep 7
        conns=$(_dev_ask 'ps' | grep -c 'sshd:conn')
        if [ "$conns" -gt "$base" ]; then
            problems="$problems sshd-conn-tasks-held=$conns(was $base at start)"
        fi
    fi

    if [ -n "$problems" ]; then
        printf '%s' "$problems"
        return 1
    fi
    return 0
}


# ---------------------------------------------------------------- monitor ---
#
# The health check used to run after every suite, and it cost four one-shot
# logins each time -- reset reason, uptime, coredump, ps. At roughly four
# seconds a login and twelve suites that is around three minutes of a ten-minute
# run spent asking the device how it feels.
#
# It also cannot work under a pool. Its leak test counts `sshd:conn` tasks, and
# with N workers holding a session each the count it reads is the runner itself.
#
# So it becomes a background subshell with one *persistent* session, polling
# every ten seconds. Every question then costs no login at all, the answers are
# timestamped, and attribution improves rather than degrades: instead of "the
# suite that just finished", a reboot is attributed to whichever suites were
# actually running when it happened, which is what the .cur files below record.
#
# The connection-leak test moves to the end of the run, where the count is
# meaningful again -- see dev_health_check, which run.sh now calls once.

DEV_MONITOR_PERIOD=10

_dev_monitor_running() {    # <rundir> -- the suites in flight right now
    local dir="$1" running="" f name

    for f in "$dir"/w*.cur; do
        [ -f "$f" ] || continue
        name=$(cut -d' ' -f1 "$f" 2>/dev/null)
        [ -n "$name" ] && running="$running $name"
    done
    [ -n "$running" ] || running=" (nothing recorded as running)"
    printf '%s' "$running"
}

_dev_monitor_alert() {      # <rundir> <reason>
    local dir="$1" reason="$2" running

    running=$(_dev_monitor_running "$dir")

    printf '%s\nsuites running at the time:%s\n' "$reason" "$running" \
        > "$dir/health.alert"

    # And stop everything else waiting on a device that is not answering. The
    # alert is for the report; this is for the twelve minutes of timeouts that
    # would otherwise follow it.
    dev_abort "$reason"
}

# Poll until <rundir>/stop appears. Intended to be backgrounded.
dev_monitor_run() {
    local dir="$1"
    # Seeded from the run's baseline, not from this loop's first sample.
    #
    # It used to start at -1 with a `>= 0` guard, which silently swallowed the
    # first poll -- so anything firing between dev_health_begin() taking the
    # baseline and the monitor's first question was counted by the end-of-run
    # check and never attributed to a suite. That window is several seconds
    # wide, since the monitor opens its own session first while four workers
    # open theirs, and it is exactly when a pile-up is most likely.
    local up cores mins last=-1 i wdt wdt_task
    local wdt_last="${DEV_HEALTH_WDT:-0}"

    if ! dev_session_start >/dev/null 2>&1; then
        printf 'monitor could not open a session\n' > "$dir/health.log"
        return 1
    fi

    while [ ! -f "$dir/stop" ]; do
        up=$(dev_run 'uptime')
        if [ "$up" = "$DEV_DEAD" ]; then
            # Losing the monitor's own session is not by itself a device fault,
            # so it reconnects rather than crying wolf. But it must keep trying
            # for longer than a reboot takes, and this is why: the first version
            # gave up after one attempt two seconds later, which is exactly when
            # a rebooting device is not yet accepting connections. So the one
            # event the monitor exists to catch was also the one event that
            # silently switched it off, and the run carried on with nobody
            # watching. The panic that taught it this was found afterwards, in
            # the end-of-run health check, which is far too late to stop
            # anything.
            dev_session_stop
            i=0
            while [ "$i" -lt 12 ] && [ ! -f "$dir/stop" ]; do
                sleep 5
                if dev_session_start >/dev/null 2>&1; then
                    break
                fi
                i=$((i + 1))
            done
            if [ -z "$DEV_PROMPT" ]; then
                [ -f "$dir/stop" ] || _dev_monitor_alert "$dir" \
                    "the device stopped answering and did not come back in 60s"
                break
            fi
            continue
        fi

        # Watchdog triggers, from the `uptime` already in hand -- no extra
        # command and no extra login.
        #
        # Recorded, not alerted. A core dump means the device rebooted and every
        # assertion after it is meaningless, so that aborts; a watchdog warning
        # means a core was held too long, which is a real defect but leaves the
        # run's remaining assertions perfectly valid. Cutting the run short here
        # would throw away good evidence to report something the end-of-run
        # check catches anyway. What only the monitor can add is *which suites
        # were running at the time*, so that is what it writes down.
        wdt=$(_dev_parse_wdt "$up")
        if [ "${wdt:-0}" -gt "$wdt_last" ]; then
            wdt_task=$(_dev_parse_wdt_task "$up")
            printf '%s|%d|%s|%s\n' "$(date +%s)" "$(( wdt - wdt_last ))" \
                   "${wdt_task:-unknown}" \
                   "$(_dev_monitor_running "$dir")" >> "$dir/watchdog.log"
        fi
        [ "${wdt:-0}" -ge 0 ] && wdt_last=$wdt

        cores=$(dev_run 'coredump')
        mins=$(_dev_parse_uptime "$up")
        printf '%s|%s|%s\n' "$(date +%s)" "$up" \
               "$(printf '%s' "$cores" | head -1)" >> "$dir/health.log"

        # Positive evidence only; see the note in dev_health_check. Stopping a
        # whole run on a query that merely failed to answer would be the same
        # mistake with a bigger blast radius.
        case "$cores" in
            *"core dump: "*)
                _dev_monitor_alert "$dir" "a core dump appeared during the run"
                break ;;
        esac

        if [ "$mins" -ge 0 ] && [ "$last" -ge 0 ] && [ "$mins" -lt "$last" ]; then
            _dev_monitor_alert "$dir" \
                "the device rebooted during the run (uptime ${last}min -> ${mins}min)"
            break
        fi
        [ "$mins" -ge 0 ] && last=$mins

        # Sleep in slices so `stop` is noticed promptly; a monitor that takes
        # ten seconds to notice the run ended holds a session for ten seconds
        # that the final leak check is about to count.
        i=0
        while [ $i -lt $((DEV_MONITOR_PERIOD * 2)) ] && [ ! -f "$dir/stop" ]; do
            sleep 0.5
            i=$((i + 1))
        done
    done

    dev_session_stop
    return 0
}
