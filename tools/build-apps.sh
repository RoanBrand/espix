#!/usr/bin/env bash
#
# Build every project under apps/ and stage the result into fsroot/bin/, so the
# rootfs image `idf.py storage-flash` writes actually contains the apps.
#
# Apps are separate ESP-IDF projects, not components of the firmware: they build
# to relocatable ELFs that espix loads at runtime, which is why `idf.py build`
# at the top level does not produce them. The firmware build calls this script
# so a fresh clone ends up with a populated /bin instead of an empty one.
#
#   ./tools/build-apps.sh              # all apps
#   ./tools/build-apps.sh hello        # just one
#
# Skip it entirely with `idf.py -DESPIX_BUILD_APPS=OFF build`, which is worth
# doing when iterating on the firmware alone: even an up-to-date app costs a few
# seconds of idf.py startup, and the Arduino one pulls a large dependency the
# first time.
#
# Requires an activated IDF environment, which the firmware build already has.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apps_dir="$root/apps"
stage_dir="$root/fsroot/bin"

# Locating idf.py is not as simple as looking on PATH. A plain `. export.sh`
# puts it there, but some setups (including esp-idf's own activate scripts)
# define idf.py as a *shell function* instead, which a child process cannot see.
# Falling back to invoking it through IDF's python covers both.
if command -v idf.py >/dev/null 2>&1; then
    idf=(idf.py)
elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf.py" ]; then
    if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
        idf=("$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py")
    else
        idf=(python3 "$IDF_PATH/tools/idf.py")
    fi
else
    echo "build-apps: cannot find idf.py; activate the ESP-IDF environment first" >&2
    exit 1
fi

target="${IDF_TARGET:-esp32s3}"
mkdir -p "$stage_dir"

# Named apps, or everything that looks like a project.
if [ $# -gt 0 ]; then
    names=("$@")
else
    names=()
    for d in "$apps_dir"/*/; do
        [ -f "${d}CMakeLists.txt" ] && names+=("$(basename "$d")")
    done
fi

for name in "${names[@]}"; do
    app="$apps_dir/$name"
    if [ ! -f "$app/CMakeLists.txt" ]; then
        echo "build-apps: $name: not an app project" >&2
        exit 1
    fi

    staged="$stage_dir/$name"
    elf="$app/build/$name.app.elf"

    # Skip when the staged binary is newer than every source that feeds it.
    # Without this, every firmware build pays for an app build that has nothing
    # to do. -newer is portable in a way that `find -newermt` and stat(1) are
    # not, macOS and Linux disagreeing on both.
    if [ -f "$staged" ]; then
        newer=$(find "$app" -type f \
                    -not -path "*/build/*" \
                    -not -path "*/managed_components/*" \
                    -newer "$staged" -print -quit 2>/dev/null || true)
        if [ -z "$newer" ]; then
            echo "build-apps: $name is up to date"
            continue
        fi
    fi

    # Per-app build environment, for anything that cannot be expressed in
    # sdkconfig.defaults -- see apps/neopixel/build.env.
    if [ -f "$app/build.env" ]; then
        # shellcheck disable=SC1090
        set -a; . "$app/build.env"; set +a
    fi

    echo "build-apps: building $name"
    (
        cd "$app"
        # `idf.py elf` is only available under the Makefiles generator, and
        # set-target is what creates the generator and sdkconfig in the first
        # place, so it runs once per app rather than on every build.
        if [ ! -f "sdkconfig" ]; then
            "${idf[@]}" -G 'Unix Makefiles' set-target "$target" > build-apps.log 2>&1 \
                || { echo "build-apps: $name: set-target failed; see $app/build-apps.log" >&2; exit 1; }
        fi
        "${idf[@]}" elf >> build-apps.log 2>&1 \
            || { echo "build-apps: $name: build failed; see $app/build-apps.log" >&2; exit 1; }
    )

    if [ ! -f "$elf" ]; then
        echo "build-apps: $name: expected $elf, which the build did not produce" >&2
        exit 1
    fi

    cp "$elf" "$staged"
    echo "build-apps: staged $name ($(wc -c < "$staged" | tr -d ' ') bytes)"
done
