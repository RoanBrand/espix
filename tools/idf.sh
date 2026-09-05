#!/usr/bin/env bash
#
# Find an ESP-IDF and run idf.py from it.
#
#   tools/idf.sh build
#   tools/idf.sh -p /dev/ttyUSB0 flash
#   eval "$(tools/idf.sh --env)"     # IDF_PATH / ESPIX_PYTHON, for a caller
#
# Why this exists at all: `idf.py` is often a *shell function* rather than a
# program -- esp-idf's own activate scripts define it that way -- and a function
# is invisible to any child process, which includes every make recipe. Finding
# the SDK ourselves and calling its idf.py through its own interpreter works
# whether or not anything has been sourced.
#
# Targets bash 3.2: macOS ships it and will not ship newer (GPLv3), so no
# associative arrays and no mapfile. See tests/README.md.

set -u

espix_idf_want_major=6
espix_idf_want_minor=1

warn() { printf 'idf.sh: %s\n' "$*" >&2; }
die()  { printf 'idf.sh: %s\n' "$*" >&2; exit 1; }

# Does this look like an ESP-IDF checkout?
idf_valid() {
    [ -n "${1:-}" ] && [ -f "$1/tools/idf.py" ]
}

# The version an IDF checkout calls itself, e.g. "v6.1" or "v6.1-beta1".
idf_version() {
    local dir="$1" v=""
    if [ -f "$dir/version.txt" ]; then
        v=$(tr -d ' \t\r\n' < "$dir/version.txt")
    fi
    if [ -z "$v" ]; then
        v=$(git -C "$dir" describe --tags 2>/dev/null | head -1)
    fi
    printf '%s' "$v"
}

# 6.1.x, and nothing else. Written as a glob rather than a version parse
# because the only question is whether the minimum espix needs is met, and
# main/idf_component.yml already refuses anything older with a clearer message.
idf_version_ok() {
    case "$1" in
        v$espix_idf_want_major.$espix_idf_want_minor|\
        v$espix_idf_want_major.$espix_idf_want_minor.*|\
        v$espix_idf_want_major.$espix_idf_want_minor-*) return 0 ;;
        *) return 1 ;;
    esac
}

# A pre-release: -beta, -rc, -dev. Usable, but only when nothing else is.
idf_is_prerelease() {
    case "$1" in *-*) return 0 ;; *) return 1 ;; esac
}

# Print "version<TAB>path" for every 6.1.x install found, stable first.
idf_candidates() {
    local stable="" pre="" d v
    for d in "$HOME"/.espressif/*/esp-idf "$HOME"/esp/*/esp-idf \
             "$HOME"/esp/esp-idf /opt/esp-idf; do
        idf_valid "$d" || continue
        v=$(idf_version "$d")
        [ -n "$v" ] || continue
        idf_version_ok "$v" || continue
        if idf_is_prerelease "$v"; then
            pre="$pre$v	$d
"
        else
            stable="$stable$v	$d
"
        fi
    done
    # Highest version within each group; stable outranks any pre-release.
    printf '%s' "$stable" | grep -v '^$' | sort -Vr
    printf '%s' "$pre"    | grep -v '^$' | sort -Vr
}

espix_find_idf() {
    if idf_valid "${IDF_PATH:-}"; then
        printf '%s' "$IDF_PATH"
        return 0
    fi
    if [ -n "${IDF_PATH:-}" ]; then
        die "IDF_PATH is set to '$IDF_PATH', which has no tools/idf.py"
    fi

    local line ver dir
    line=$(idf_candidates | head -1)
    if [ -z "$line" ]; then
        printf 'idf.sh: no ESP-IDF v%s.%s found.\n' \
               "$espix_idf_want_major" "$espix_idf_want_minor" >&2
        printf 'idf.sh: looked in ~/.espressif/*/esp-idf, ~/esp/*/esp-idf, /opt/esp-idf\n' >&2
        printf 'idf.sh: set IDF_PATH=... to point at one.\n' >&2
        exit 1
    fi

    ver=${line%%	*}
    dir=${line#*	}
    if idf_is_prerelease "$ver"; then
        warn "using $ver -- a pre-release, because no stable v$espix_idf_want_major.$espix_idf_want_minor is installed"
    fi
    printf '%s' "$dir"
}

# The interpreter that carries esptool and pyserial. Convention is
# ~/.espressif/tools/python/<tag>/venv/bin/python; fall back to python3, which
# is enough to run idf.py but not to drive a serial port.
espix_find_python() {
    local idf="$1" ver p
    if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
        printf '%s' "$IDF_PYTHON_ENV_PATH/bin/python"
        return 0
    fi
    ver=$(idf_version "$idf")
    p="$HOME/.espressif/tools/python/$ver/venv/bin/python"
    if [ -x "$p" ]; then
        printf '%s' "$p"
        return 0
    fi
    command -v python3 2>/dev/null || printf 'python3'
}

espix_idf_path=$(espix_find_idf) || exit 1
espix_python=$(espix_find_python "$espix_idf_path")

if [ "${1:-}" = "--env" ]; then
    printf 'IDF_PATH=%s\n' "$espix_idf_path"
    printf 'ESPIX_PYTHON=%s\n' "$espix_python"
    printf 'IDF_VERSION=%s\n' "$(idf_version "$espix_idf_path")"
    exit 0
fi

export IDF_PATH="$espix_idf_path"

# Put the toolchain on PATH and set the variables idf.py expects.
#
# The EIM installer ships a per-version activate script with an `-e` mode that
# prints its environment as KEY=value lines -- a documented interface for
# exactly this, and far better than the alternatives. Sourcing it does not work:
# a sourced script sees the *caller's* positional parameters, so our "build"
# lands in its $1, it decides it was executed rather than sourced, and calls
# exit -- which kills this script silently, mid-run, with no output.
#
# Deriving it by hand does not work either, which was the previous attempt:
# idf.py needs IDF_PYTHON_ENV_PATH, IDF_TOOLS_PATH *and* ESP_IDF_VERSION, and
# missing any one produces a different confusing failure -- a venv that does not
# exist, a missing constraints file, and a TypeError inside the component
# manager respectively. Let the installer say what it needs.
espix_activate="${IDF_TOOLS_PATH:-$HOME/.espressif/tools}/activate_idf_$(idf_version "$espix_idf_path").sh"

if [ -x "$espix_activate" ]; then
    espix_env=$("$espix_activate" -e 2>/dev/null)
    if [ -n "$espix_env" ]; then
        # SYSTEM_PATH is the caller's PATH, which the script splits out; putting
        # the tools first and the system after is what activation does.
        espix_sys_path=$(printf '%s\n' "$espix_env" | sed -n 's/^SYSTEM_PATH=//p')
        while IFS= read -r pair; do
            case "$pair" in
                ''|SYSTEM_PATH=*) continue ;;
                PATH=*) export PATH="${pair#PATH=}:${espix_sys_path:-$PATH}" ;;
                *=*) export "$pair" ;;
            esac
        done <<EOF
$espix_env
EOF
    fi
elif [ -f "$espix_idf_path/export.sh" ]; then
    # Classic install.sh layout. Sourcing is the only interface it offers, so
    # the positional parameters are cleared first to keep it from taking our
    # arguments as its own.
    set --
    set +u
    # shellcheck disable=SC1091
    . "$espix_idf_path/export.sh" >/dev/null 2>&1 || true
    set -u
fi

export IDF_PATH="$espix_idf_path"

if ! command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1 && \
   ! command -v riscv32-esp-elf-gcc >/dev/null 2>&1; then
    warn "no cross-compiler on PATH after activation; the build will fail at the compiler check"
fi

exec "$espix_python" "$espix_idf_path/tools/idf.py" "$@"
