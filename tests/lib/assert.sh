# Assertions, and the counters behind them.
#
# Every assertion prints one line, pass or fail, so a run is readable as it
# happens rather than only at the end. Failures print what was expected and what
# arrived, because "assertion failed" without the values is a second debugging
# session.

# Named N_ rather than ESPIX_PASS/FAIL/SKIP: ESPIX_PASS is the device password
# in device.sh, and the first version of this file silently overwrote it with
# the pass count, so every login tried to authenticate as "0".
ESPIX_N_PASS=0
ESPIX_N_FAIL=0
ESPIX_N_SKIP=0
ESPIX_CURRENT_SUITE=""

_espix_green() { printf '\033[32m%s\033[0m' "$1"; }
_espix_red()   { printf '\033[31m%s\033[0m' "$1"; }
_espix_dim()   { printf '\033[2m%s\033[0m'  "$1"; }

espix_pass() {
    ESPIX_N_PASS=$((ESPIX_N_PASS + 1))
    printf '  %s %s\n' "$(_espix_green ok)" "$1"
}

espix_fail() {
    ESPIX_N_FAIL=$((ESPIX_N_FAIL + 1))
    printf '  %s %s\n' "$(_espix_red FAIL)" "$1"
    if [ $# -gt 1 ]; then
        shift
        local line
        for line in "$@"; do
            printf '       %s\n' "$line"
        done
    fi
}

espix_skip() {
    ESPIX_N_SKIP=$((ESPIX_N_SKIP + 1))
    printf '  %s %s\n' "$(_espix_dim skip)" "$1"
}

# assert_eq <what> <expected> <actual>
assert_eq() {
    if [ "$2" = "$3" ]; then
        espix_pass "$1"
    else
        espix_fail "$1" "expected: $2" "actual:   $3"
    fi
}

# assert_contains <what> <needle> <haystack>
assert_contains() {
    case "$3" in
        *"$2"*) espix_pass "$1" ;;
        *)      espix_fail "$1" "expected to contain: $2" "actual: $3" ;;
    esac
}

# assert_not_contains <what> <needle> <haystack>
assert_not_contains() {
    case "$3" in
        *"$2"*) espix_fail "$1" "expected NOT to contain: $2" "actual: $3" ;;
        *)      espix_pass "$1" ;;
    esac
}

# assert_status <what> <expected status> <command...>
#
# Runs the command locally; use with dev_run_status for a device command. The
# espix shell has no $?, so a status assertion always costs its own connection.
assert_status() {
    local what="$1" want="$2"; shift 2
    local got=0
    "$@" >/dev/null 2>&1 || got=$?
    if [ "$got" = "$want" ]; then
        espix_pass "$what"
    else
        espix_fail "$what" "expected status: $want" "actual status:   $got"
    fi
}

espix_suite_begin() {
    ESPIX_CURRENT_SUITE="$1"
    printf '\n=== %s ===\n' "$1"
}
