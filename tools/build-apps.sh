#!/usr/bin/env bash
#
# Build every project under apps/ and stage the result into fsroot/bin/, so the
# rootfs image `make flash-fs` writes actually contains the apps.
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
# Normally the dev tree. A release sets ESPIX_APPS_STAGE to a clean directory so
# that only apps/ -- never whatever else is in the local fsroot -- is packaged.
stage_dir="${ESPIX_APPS_STAGE:-$root/fsroot/bin}"

# tools/idf.sh finds the SDK, puts the toolchain on PATH and sets the variables
# idf.py needs, so nothing has to be sourced first. It used to be duplicated
# here, badly: this script only knew how to use an environment somebody had
# already activated, and said so with an error rather than doing anything about
# it.
idf=("$root/tools/idf.sh")

# The target comes from the environment when the firmware build calls this
# (tools/idf.sh exports IDF_TARGET), and from .espix/active when run by hand.
target="${IDF_TARGET:-}"
if [ -z "$target" ]; then
    target=$(head -n1 "$root/.espix/active" 2>/dev/null || true)
fi
target="${target:-esp32s3}"

mkdir -p "$stage_dir"

# Staged ELFs are not target-neutral: an S31 image cannot load an S3 binary.
# Remember which target staged them and rebuild rather than call a foreign one
# up to date.
stamp="$stage_dir/.espix-target"
staged_target=""
[ -f "$stamp" ] && staged_target=$(head -n1 "$stamp" 2>/dev/null || true)
force=0
[ "$staged_target" = "$target" ] || force=1

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
    if [ "$force" = 0 ] && [ -f "$staged" ]; then
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
        have=""
        [ -f "sdkconfig" ] && have=$(grep '^CONFIG_IDF_TARGET=' sdkconfig 2>/dev/null | head -1 | cut -d'"' -f2)
        if [ "$have" != "$target" ]; then
            # A different target's sdkconfig is stale, and set-target's implicit
            # fullclean refuses a build dir left by a failed configure, so start
            # from nothing.
            rm -rf build
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
    printf '%s\n' "$target" > "$stamp"
    echo "build-apps: staged $name ($(wc -c < "$staged" | tr -d ' ') bytes)"
done
