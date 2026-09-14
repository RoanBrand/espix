#!/usr/bin/env bash
#
# Ask the device one thing, over SSH, and print what it says.
#
#   tools/esp.sh uptime
#   tools/esp.sh 'ps'
#   tools/esp.sh free
#   ESPIX_HOST=10.0.0.5 tools/esp.sh uname -a
#
# WHY THIS EXISTS, and it is not convenience.
#
# There was no supported way to ask the board a single question outside the test
# suite, so the gap kept getting filled with a hand-rolled `expect` script
# driving `ssh` through a pty. That script worked for a while and then silently
# stopped completing the password step -- it reached the prompt, sent nothing
# useful, and sat there.
#
# What that cost was not the script. It was an hour spent concluding, from the
# script's own silence, that *the device* had wedged: SSH accepted connections,
# finished key exchange, and then never authenticated. The evidence all agreed,
# because the evidence was all downstream of the same broken client. The board
# was reflashed, soft-rebooted, hard-reset and re-keyed on the strength of it.
# One `tests/run.sh --suite 00-smoke` -- fifteen seconds, and it passed 15/15 --
# would have ended it at the start.
#
# So this uses the same login path the suite does: SSH_ASKPASS with a helper
# script, via tests/lib/device.sh, which is the arrangement that is actually
# exercised on every run. Nothing hand-rolled, nothing that can rot separately
# from the thing that proves it works.
#
# And when it cannot get in, it says what to try next rather than just failing,
# because "the device is broken" and "my client is broken" look identical from
# here and only one of them is usually true.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPIX_ROOT="$(cd "$here/.." && pwd)"
ESPIX_TEST_DIR="$ESPIX_ROOT/tests"
export ESPIX_ROOT

if [ "$#" -eq 0 ]; then
    sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi

. "$ESPIX_TEST_DIR/lib/portable.sh"
. "$ESPIX_TEST_DIR/lib/assert.sh"
. "$ESPIX_TEST_DIR/lib/device.sh"

dev_askpass_init
trap 'dev_askpass_cleanup' EXIT

# _dev_ssh rather than dev_once, because dev_once throws the exit status away
# and the status is what separates the two cases below.
out=$(_dev_ssh "$*" 2>&1); rc=$?
out=$(printf '%s' "$out" | grep -v '^Warning: Permanently added' || true)

# ssh answers 255 for its own failures -- refused, timed out, not authenticated
# -- and anything else is the remote command's own status. Only the first is a
# problem with getting in.
#
# Keyed on that and not on empty output, which was the first version and was
# wrong within the hour: `cat /dev/null` returns nothing and is entirely
# correct, and the tool called it a failed login. A diagnostic that fires on
# correct behaviour is the thing this file exists to complain about.
if [ "$rc" -eq 255 ]; then
    printf 'esp.sh: could not reach %s@%s (ssh exit 255): %s\n' \
           "$ESPIX_USER" "$ESPIX_HOST" "${out:-no message}" >&2
    printf 'esp.sh: that is what a failed login looks like too. To tell a device\n' >&2
    printf 'esp.sh: problem from a client one, run the suite'"'"'s own login path:\n\n' >&2
    printf '    ./tests/run.sh --suite 00-smoke\n\n' >&2
    printf 'esp.sh: if that passes, the device is fine and the problem is here.\n' >&2
    exit 1
fi

[ -n "$out" ] && printf '%s\n' "$out"
exit "$rc"
