# Differences between macOS and Linux, in one place.
#
# Targets bash 3.2, which is what macOS ships and will keep shipping: Apple
# froze it at the last GPLv2 release. So no associative arrays, no mapfile, no
# ${var^^}. A bash-4-ism works on Linux and fails here, which is the worst kind
# of portability bug -- green for the author, red for everyone else.

# Run a command with a deadline. There is no `timeout` on macOS and no
# `gtimeout` unless coreutils is installed; assuming otherwise has already cost
# this project a wrong conclusion.
#
# Returns the command's status, or 124 if it was killed for running long --
# matching GNU timeout so callers can test for it.
#
# The kill goes to the process *group*, and that is not a refinement.
#
# `"$@" &` gives back the pid of a subshell whenever the argument is a shell
# function -- and the first caller of this, run.sh's preflight, passed
# `dev_status`, which is one. Signalling that pid killed the subshell and left
# its `ssh` child running. espix accepts four connections; each orphan holds one
# for as long as the machine is up, so they accumulate silently across runs
# until the device answers nobody and every suite hangs with no output.
#
# That is not a hypothesis about this code: three such orphans were found alive
# on this machine -- `uptime`, `coredump` and a bad command, hours apart -- while
# investigating exactly that symptom, and reading it as device flakiness cost a
# whole session. `set -m` puts the child in its own process group so the
# negative pid reaches everything it started.
espix_timeout() {
    local secs="$1"; shift
    local pid rc start

    # Keep the caller's standard input.
    #
    # Backgrounding with `&` gives the job /dev/null for stdin, so wrapping a
    # command in this quietly severed it -- which broke
    # `_ssh 'prog' < file` in 15-streams.sh the moment that helper gained a
    # deadline. The app read zero bytes and it read exactly like a firmware
    # bug; the same command by hand worked perfectly. A saved descriptor is the
    # only way back, because an explicit `0<&0` on an async command is applied
    # *after* the /dev/null substitution and so re-duplicates /dev/null.
    # Braces round the exec, and they are not decoration: `exec` with no
    # command makes its redirections *permanent*, so `exec 7<&0 2>/dev/null`
    # silences the shell's stderr for good -- which swallowed ssh's own
    # diagnostics and broke the one assertion that checks a client's `2>&1`.
    # Grouping scopes the 2>/dev/null to the group while fd 7 still lands on
    # the shell.
    local have_stdin=""
    if { exec 7<&0; } 2>/dev/null; then
        have_stdin=yes
    fi

    # Job control off again immediately: leaving it on changes how later
    # background jobs in the same shell report themselves.
    set -m
    if [ -n "$have_stdin" ]; then
        "$@" <&7 &
    else
        "$@" &
    fi
    pid=$!
    set +m
    [ -n "$have_stdin" ] && { exec 7<&-; } 2>/dev/null
    start=$SECONDS

    while kill -0 "$pid" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$secs" ]; then
            # Braces with stderr closed: bash announces "Terminated: 15" for a
            # killed background job, and that lands in the middle of test output
            # looking like a failure.
            #
            # Group first, then the pid alone as a fallback -- if `set -m` did
            # not take (a shell built without job control), the negative form
            # fails and the child would otherwise survive the timeout entirely.
            {
                kill -TERM "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null
                sleep 1
                kill -KILL "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
                wait "$pid"
            } 2>/dev/null
            return 124
        fi
        sleep 0.2
    done

    wait "$pid"
    rc=$?
    return $rc
}

# SHA-256 of a file, however this machine spells it.
espix_sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        echo "portable.sh: no shasum or sha256sum" >&2
        return 1
    fi
}

# mktemp differs in its template handling; this form works on both.
espix_mktemp_dir() {
    mktemp -d "${TMPDIR:-/tmp}/espix-test.XXXXXX"
}

# grep over a file that may contain NUL bytes -- a serial capture always does,
# and without -a grep calls it binary and prints nothing, which reads exactly
# like "the thing I searched for is absent".
espix_grep() {
    grep -a "$@"
}
