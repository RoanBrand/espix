# Talking to the device: one long-lived SSH session, one-shot commands for
# exit statuses, file transfer, and the health check that runs between suites.

: "${ESPIX_HOST:=192.168.110.55}"
: "${ESPIX_USER:=esp}"
: "${ESPIX_PASS:=espix}"
: "${ESPIX_PYTHON:=python3}"
: "${ESPIX_PORT:=}"

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
    DEV_SESSION_DIR=$(espix_mktemp_dir)
    mkfifo "$DEV_SESSION_DIR/in" "$DEV_SESSION_DIR/out"

    "$ESPIX_PYTHON" "$ESPIX_LIB_DIR/session.py" \
        --host "$ESPIX_HOST" --user "$ESPIX_USER" --password "$ESPIX_PASS" \
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
    if [ -n "$DEV_SESSION_DEAD" ] || [ -z "$DEV_SESSION_PID" ]; then
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

# ConnectTimeout bounds the *connect* and nothing after it, so a device that
# accepts a connection and then never answers held a run open indefinitely --
# dev_health_begin calls dev_once twice before the first suite, with no output
# to say what it was waiting for. These are the whole-command deadlines.
: "${ESPIX_SSH_TIMEOUT:=30}"
: "${ESPIX_SCP_TIMEOUT:=90}"

# The environment and the deadline, in one place.
#
# espix_timeout is handed the ssh *binary*, through env, and never a shell
# function: `"$@" &` on a function gives back a subshell pid, and killing that
# leaves ssh alive holding one of espix's four connection slots. run.sh's
# preflight did exactly that, and the orphans accumulate until the device
# answers nobody. See the note in portable.sh.
_dev_ssh() {
    espix_timeout "$ESPIX_SSH_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS "$ESPIX_USER@$ESPIX_HOST" "$@"
}

_dev_scp() {
    espix_timeout "$ESPIX_SCP_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        scp $DEV_SSH_OPTS "$@"
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
    espix_timeout "$ESPIX_SCP_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        sftp $DEV_SSH_OPTS -o BatchMode=no -b "$1" \
        "$ESPIX_USER@$ESPIX_HOST" 2>&1
}

# dev_capture <remote command> <local file>
#
# Writes on the device and fetches the file, rather than reading a long answer
# back over the channel. Output big enough to matter is exactly what stresses
# the transport being tested, so reading it inline makes the transport part of
# the measurement.
dev_capture() {
    dev_once "$1 > /tmp/.espix-capture" >/dev/null
    dev_pull /tmp/.espix-capture "$2" >/dev/null
    dev_once "rm /tmp/.espix-capture" >/dev/null

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
dev_console_run() {
    [ -n "$ESPIX_PORT" ] || { echo "dev_console_run: no serial port"; return 1; }
    local out rc
    out=$(printf '%s\n' "$@" \
          | "$ESPIX_PYTHON" "$ESPIX_LIB_DIR/console.py" --port "$ESPIX_PORT" 2>&1)
    rc=$?

    # Captured before the filter and returned explicitly, because the obvious
    # version returns sed's status instead -- always zero, so a console that
    # never answered looked like a command that returned nothing. dev_push had
    # this exact bug and it cost an afternoon.
    printf '%s' "$out" | sed -e '/^<<<ESPIX-/d'
    return $rc
}

# --------------------------------------------------------------- test app ---

DEV_TESTAPP="/home/$ESPIX_USER/testapp"
DEV_TESTAPP_SHA="/home/$ESPIX_USER/.testapp.sha"

# Put the test app on the device, but only when the one there is not the one we
# just built. espix has no checksum command among its 48, so the hash travels
# with the binary in a sidecar and `cat` does the comparison.
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

# espix prints one of three shapes (espix_kernel/kernel.c):
#   up N min          up H:MM          up N days, H:MM
# Turned into minutes so a *decrease* can be spotted, which is the only
# dependable sign of a reboot: comparing the reset reason alone misses the
# common case, since two software reboots in a row read identically.
#
# Minute granularity, so a reboot and recovery inside the same minute could
# slip past. Suites take tens of seconds, and the core-dump check covers the
# crash case regardless.
dev_uptime_minutes() {
    local line days hours mins
    line=$(dev_once 'uptime')

    case "$line" in
        *day*)
            days=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\) day.*/\1/p')
            hours=$(printf '%s' "$line" | sed -n 's/.*, \([0-9]*\):[0-9]*.*/\1/p')
            mins=$(printf '%s' "$line" | sed -n 's/.*:\([0-9]*\),.*/\1/p')
            [ -n "$mins" ] || mins=$(printf '%s' "$line" | sed -n 's/.*:\([0-9]*\).*/\1/p')
            printf '%s' "$(( ${days:-0} * 1440 + ${hours:-0} * 60 + ${mins:-0} ))" ;;
        *min*)
            mins=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\) min.*/\1/p')
            printf '%s' "${mins:-0}" ;;
        *:*)
            hours=$(printf '%s' "$line" | sed -n 's/^up \([0-9]*\):[0-9]*.*/\1/p')
            mins=$(printf '%s' "$line" | sed -n 's/^up [0-9]*:\([0-9]*\).*/\1/p')
            printf '%s' "$(( ${hours:-0} * 60 + ${mins:-0} ))" ;;
        *)
            printf '%s' "-1" ;;
    esac
}

dev_health_reason() {
    dev_once 'uptime' | sed -n 's/.*last reset: //p'
}

dev_health_begin() {
    DEV_HEALTH_REASON=$(dev_health_reason)
    DEV_HEALTH_MINUTES=$(dev_uptime_minutes)
}

# Returns non-zero and explains if the device rebooted, dumped core, or is
# leaking connection tasks.
dev_health_check() {
    local reason mins cores conns problems=""

    reason=$(dev_health_reason)
    if [ -n "$DEV_HEALTH_REASON" ] && [ "$reason" != "$DEV_HEALTH_REASON" ]; then
        problems="$problems reset-reason-changed('$DEV_HEALTH_REASON'->'$reason')"
    fi

    mins=$(dev_uptime_minutes)
    if [ "$DEV_HEALTH_MINUTES" -ge 0 ] && [ "$mins" -ge 0 ] \
       && [ "$mins" -lt "$DEV_HEALTH_MINUTES" ]; then
        problems="$problems rebooted(uptime ${DEV_HEALTH_MINUTES}min->${mins}min)"
    fi
    [ "$mins" -ge 0 ] && DEV_HEALTH_MINUTES=$mins

    cores=$(dev_once 'coredump')
    case "$cores" in
        *"no core dump"*) ;;
        *) problems="$problems core-dump-present" ;;
    esac

    # Connection tasks: told apart by persistence, not by counting.
    #
    # One is the connection asking the question. A second is usually the
    # previous one still in teardown -- close_gracefully drains until the peer
    # hangs up, bounded by PARTIAL_READ_TIMEOUT_MS at five seconds -- so
    # back-to-back commands routinely show two, and complaining about that would
    # cry wolf on every run.
    #
    # But KNOWN-ISSUES records that one sometimes does *not* clear, and simply
    # tolerating two made that invisible to the check meant to catch it. So look
    # again after the teardown window has passed: a closing connection is gone
    # by then and a stranded one is not. The extra wait only happens on the rare
    # path where the count is above one.
    conns=$(dev_once 'ps' | grep -c 'sshd:conn')
    if [ "$conns" -gt 1 ]; then
        sleep 7
        conns=$(dev_once 'ps' | grep -c 'sshd:conn')
        if [ "$conns" -gt 1 ]; then
            # "Something is holding connections", not a diagnosis: a person
            # logged in at another terminal looks exactly like a stranded task
            # from here, and reading one as the other has already put a wrong
            # claim into KNOWN-ISSUES once.
            problems="$problems sshd-conn-tasks-held=$conns(someone-logged-in?)"
        fi
    fi

    if [ -n "$problems" ]; then
        printf '%s' "$problems"
        return 1
    fi
    return 0
}
