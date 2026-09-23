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

# The project root, so a caller finds .espix/ no matter where it runs from.
espix_root_dir=$(cd "$(dirname "$0")/.." && pwd)

# The target this tree is configured for. .espix/active is written by
# tools/espix; ESPIX_TARGET overrides it for a one-off build.
espix_active_target() {
    if [ -n "${ESPIX_TARGET:-}" ]; then
        printf '%s' "$ESPIX_TARGET"
        return 0
    fi
    local t=""
    if [ -f "$espix_root_dir/.espix/active" ]; then
        t=$(tr -d ' \t\r\n' < "$espix_root_dir/.espix/active")
    fi
    printf '%s' "${t:-esp32s3}"
}

# A target IDF refuses without --preview. Keep in sync with IDF's PREVIEW_TARGETS
# (tools/idf_py_actions/constants.py).
espix_target_is_preview() {
    case "$1" in
        esp32s31|esp32h21|esp32h4|linux) return 0 ;;
        *) return 1 ;;
    esac
}

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
    espix_env_target=$(espix_active_target)
    printf 'IDF_PATH=%s\n' "$espix_idf_path"
    printf 'ESPIX_PYTHON=%s\n' "$espix_python"
    printf 'IDF_VERSION=%s\n' "$(idf_version "$espix_idf_path")"
    printf 'ESPIX_TARGET=%s\n' "$espix_env_target"
    # Exported, so a caller's child processes (build-apps.sh during a release,
    # for one) build for the same target rather than falling back to active.
    printf 'export IDF_TARGET=%s\n' "$espix_env_target"
    printf 'ESPIX_BUILD=%s\n' "$espix_root_dir/build-$espix_env_target"
    printf 'ESPIX_SDKCONFIG=%s\n' "$espix_root_dir/sdkconfig.$espix_env_target"
    printf 'ESPIX_LOADER_BUILD=%s\n' "$espix_root_dir/loader/build-$espix_env_target"
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

# ---------------------------------------------------------------------------
# Target and build layout.
#
# One tree holds several targets side by side: each gets its own sdkconfig and
# build directory, so switching is not a full rebuild and two configurations
# coexist. .espix/active names the target (tools/espix writes it); IDF_TARGET is
# exported so every project in the tree -- firmware, loader, apps -- agrees.
# ---------------------------------------------------------------------------
espix_target=$(espix_active_target)
export IDF_TARGET="$espix_target"

espix_args=()
if espix_target_is_preview "$espix_target"; then
    espix_args+=(--preview)
fi

# Which project is this? The firmware is the root project; the loader is its own
# under loader/; anything else (an app, the test app) manages its own build
# directory and sdkconfig, and only needs the target.
#
# The project directory is the -C/--project-dir argument when there is one, and
# the working directory otherwise -- build-apps.sh cds into the app and calls
# idf.sh with no -C at all, so looking only at the arguments sent every app
# build into the firmware's build directory and sdkconfig.
espix_project_dir="$PWD"
espix_has_build=0
espix_has_sdkconfig=0
espix_expect=""
for espix_a in "$@"; do
    if [ "$espix_expect" = project ]; then
        espix_project_dir="$espix_a"
        espix_expect=""
        continue
    fi
    if [ "$espix_expect" = define ]; then
        case "$espix_a" in SDKCONFIG=*) espix_has_sdkconfig=1 ;; esac
        espix_expect=""
        continue
    fi
    case "$espix_a" in
        -C|--project-dir) espix_expect=project ;;
        -Cloader|--project-dir=loader) espix_project_dir="$espix_root_dir/loader" ;;
        --project-dir=*) espix_project_dir="${espix_a#--project-dir=}" ;;
        -C*) espix_project_dir="${espix_a#-C}" ;;
        -B|--build-dir) espix_has_build=1 ;;
        -B*) espix_has_build=1 ;;
        -D|--define) espix_expect=define ;;
        -DSDKCONFIG=*|SDKCONFIG=*) espix_has_sdkconfig=1 ;;
    esac
done

espix_project_real=$(cd "$espix_project_dir" 2>/dev/null && pwd) \
    || espix_project_real="$espix_project_dir"
case "$espix_project_real" in
    "$espix_root_dir")        espix_project=main ;;
    "$espix_root_dir/loader") espix_project=loader ;;
    *)                        espix_project=other ;;
esac

if [ "$espix_project" = main ]; then
    espix_build_dir="$espix_root_dir/build-$espix_target"
    espix_sdkconfig="$espix_root_dir/sdkconfig.$espix_target"

    # A board is a defaults file that lands after the target's own defaults. It
    # only matters when sdkconfig is generated, which tools/espix arranges by
    # deleting it; an existing sdkconfig is authoritative.
    espix_board_file="$espix_root_dir/.espix/board-$espix_target"
    if [ -f "$espix_board_file" ]; then
        espix_board=$(head -n1 "$espix_board_file")
        if [ -n "$espix_board" ] && [ -f "$espix_root_dir/boards/$espix_board.conf" ]; then
            export SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$espix_board.conf"
        fi
    fi
elif [ "$espix_project" = loader ]; then
    espix_build_dir="$espix_root_dir/loader/build-$espix_target"
    espix_sdkconfig="$espix_root_dir/loader/sdkconfig.$espix_target"
else
    # An app or the test app: it has its own defaults. Do not leak the
    # firmware's board selection into a different project's defaults search.
    unset SDKCONFIG_DEFAULTS
fi

if [ "$espix_project" = main ] || [ "$espix_project" = loader ]; then
    [ "$espix_has_build" = 0 ] && espix_args+=(-B "$espix_build_dir")
    [ "$espix_has_sdkconfig" = 0 ] && espix_args+=(-D "SDKCONFIG=$espix_sdkconfig")
fi

# ${espix_args[@]+"..."} rather than "${espix_args[@]}": bash 3.2 treats an
# empty array as unbound under `set -u`.
exec "$espix_python" "$espix_idf_path/tools/idf.py" \
    ${espix_args[@]+"${espix_args[@]}"} "$@"
