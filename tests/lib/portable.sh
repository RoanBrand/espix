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
espix_timeout() {
    local secs="$1"; shift
    local pid rc start

    "$@" &
    pid=$!
    start=$SECONDS

    while kill -0 "$pid" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$secs" ]; then
            # Braces with stderr closed: bash announces "Terminated: 15" for a
            # killed background job, and that lands in the middle of test output
            # looking like a failure.
            {
                kill -TERM "$pid" 2>/dev/null
                sleep 1
                kill -KILL "$pid" 2>/dev/null
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
