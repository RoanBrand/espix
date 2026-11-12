#!/usr/bin/env bash
#
# Build every project under apps/ and stage the result into fsroot/bin/, so the
# rootfs image `make flash-fs` writes actually contains the apps.
#
# An app is a POSIX program: a plain CMake project built with tools/espix-cc
# (cmake/espix-uapp.cmake), into apps/<name>/build-<target>/. One that is an IDF
# project instead (neopixel, on IDF's drivers) is built by idf.py. The firmware
# build calls this script so a fresh clone ends up with a populated /bin.
#
#   ./tools/build-apps.sh              # all apps
#   ./tools/build-apps.sh hello        # just one
#
# Skip it entirely with `idf.py -DESPIX_BUILD_APPS=OFF build`, which is worth
# doing when iterating on the firmware alone: even an up-to-date app costs a few
# seconds of idf.py startup, and the Arduino one pulls a large dependency the
# first time.
#
# An app that needs data it does not ship declares it in `apps/<name>/appdata.conf`,
# and this stages it beside the binary's etc/ -- not into /bin, because it is
# configuration the launcher reads rather than a program. It lives with the app
# so that a release, which stages apps/ into a clean directory and never the
# developer's fsroot, cannot ship a binary without the manifest that explains
# its data.
#

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apps_dir="$root/apps"
# Normally the dev tree. A release sets ESPIX_APPS_STAGE to a clean directory so
# that only apps/ -- never whatever else is in the local fsroot -- is packaged.
stage_dir="${ESPIX_APPS_STAGE:-$root/fsroot/bin}"
# One compiler per CPU: a bare `make -j` starts one per source file, which runs out of RAM.
jobs="${ESPIX_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# tools/idf.sh finds the SDK, puts the toolchain on PATH and sets the variables
# idf.py needs, so nothing has to be sourced first. It used to be duplicated
# here, badly: this script only knew how to use an environment somebody had
# already activated, and said so with an error rather than doing anything about
# it.
idf=("$root/tools/idf.sh")

# The target comes from the environment when the firmware build calls this
# (tools/idf.sh exports IDF_TARGET), and from .espix/active when run by hand.
target="${IDF_TARGET:-${ESPIX_TARGET:-}}"
if [ -z "$target" ]; then
    target=$(head -n1 "$root/.espix/active" 2>/dev/null || true)
fi
target="${target:-esp32s3}"

mkdir -p "$stage_dir"

# Where the manifests go: the etc/ beside this stage root, so one rule covers
# the development fsroot/bin and a release's clean factory root alike.
manifest_dir="$(dirname "$stage_dir")/etc/apps"

# Idempotent, and deliberately not part of the "is it newer" decision below: a
# manifest edited without touching a source line is a cheap copy here, and a
# rootfs whose etc/apps was deleted still gets one back.
stage_manifest() {
    if [ -f "$app/appdata.conf" ]; then
        mkdir -p "$manifest_dir"
        cp "$app/appdata.conf" "$manifest_dir/$name.conf"
    else
        rm -f "$manifest_dir/$name.conf"
    fi
}

# An IDF project (neopixel): idf.py, against the kernel's ABI constants.
build_idf_app() {
    local elf="$app/build/$name.app.elf"
    # Per-app build environment, for anything that cannot be expressed in
    # sdkconfig.defaults -- see apps/neopixel/build.env.
    if [ -f "$app/build.env" ]; then
        # shellcheck disable=SC1090
        set -a; . "$app/build.env"; set +a
    fi
    # The app must be configured with the kernel's ABI constants, not its own
    # project defaults; see tools/app-abi.defaults.
    local fw_hz
    fw_hz=$(grep -m1 '^CONFIG_FREERTOS_HZ=' "$root/sdkconfig.defaults" 2>/dev/null | cut -d= -f2)
    (
        cd "$app"
        # Applied after the app's own defaults, so the kernel's constants win.
        export ESPIX_APP_DEFAULTS="$app/sdkconfig.defaults;$root/tools/app-abi.defaults"
        # Another target's build dir and sdkconfig load on neither: start from nothing.
        if [ "$force" = 1 ]; then
            rm -rf build sdkconfig
        fi
        # `idf.py elf` needs the Makefiles generator, which set-target creates.
        local have="" app_hz="" app_libc=""
        if [ -f "sdkconfig" ]; then
            have=$(grep '^CONFIG_IDF_TARGET=' sdkconfig 2>/dev/null | head -1 | cut -d'"' -f2)
            app_hz=$(grep -m1 '^CONFIG_FREERTOS_HZ=' sdkconfig 2>/dev/null | cut -d= -f2)
            app_libc=$(grep -c '^CONFIG_LIBC_PICOLIBC=y' sdkconfig 2>/dev/null || true)
        fi
        if [ "$have" != "$target" ] || [ "$app_libc" != 1 ] \
           || { [ -n "$fw_hz" ] && [ "$app_hz" != "$fw_hz" ]; }; then
            rm -rf build sdkconfig
            "${idf[@]}" -G 'Unix Makefiles' set-target "$target" > build.log 2>&1 \
                || { echo "build-apps: $name: set-target failed; see $app/build.log" >&2; exit 1; }
        fi
        rm -f "$elf"
        "${idf[@]}" elf >> build.log 2>&1 \
            || { echo "build-apps: $name: build failed; see $app/build.log" >&2; exit 1; }
    )
    [ -f "$elf" ] || { echo "build-apps: $name: expected $elf, which the build did not produce" >&2; exit 1; }
    cp "$elf" "$staged"
}

# Staged ELFs are not target-neutral: an S31 image cannot load an S3 binary.
# Each app's stamp names the target that staged *it*, checked per app below.

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

    # An app may name the targets it supports, one per line; neopixel needs an
    # Arduino variant the S31 does not have, so it is S3-only. Skipping it, and
    # clearing a binary staged for another target, keeps an image from carrying
    # an app the board cannot load.
    if [ -f "$app/targets" ] && ! grep -qx "$target" "$app/targets"; then
        rm -f "$stage_dir/$name" "$stage_dir/.espix-target-$name"
        stage_manifest
        echo "build-apps: $name: not for $target (apps/$name/targets)"
        continue
    fi

    staged="$stage_dir/$name"
    stamp="$stage_dir/.espix-target-$name"
    idf_app=0
    grep -q 'project.cmake' "$app/CMakeLists.txt" && idf_app=1

    # The target that staged *this* app. One shared stamp used to let an app
    # built for a new target mark every foreign binary current.
    force=1
    [ -f "$stamp" ] && [ "$(head -n1 "$stamp" 2>/dev/null || true)" = "$target" ] && force=0

    # Skip when the staged binary is newer than every source that feeds it: the
    # app's own, and the runtime it links (libespix, its libc). -newer is portable
    # in a way that `find -newermt` and stat(1) are not.
    if [ "$force" = 0 ] && [ -f "$staged" ]; then
        newer=$(find "$app" -type f -not -path "*/build*" -not -path "*/managed_components/*" \
                    -newer "$staged" -print -quit 2>/dev/null || true)
        if [ -z "$newer" ]; then
            newer=$(find "$root/user/libespix" "$root/user/include" "$root/user/crt0.S" "$root/user/uapp.ld" \
                        "$root/components/espix_proc/include/espix_syscall.h" "$root/tools/espix-cc" \
                        "$root/cmake/espix-uapp.cmake" "$root/cmake/espix-app.cmake" \
                        "$root/build-libc-$target/split/libc-app.a" "$root/build-libc-$target/split/abi-note.o" \
                        -type f -newer "$staged" -print -quit 2>/dev/null || true)
        fi
        if [ -z "$newer" ]; then
            stage_manifest
            echo "build-apps: $name is up to date"
            continue
        fi
    fi

    echo "build-apps: building $name"
    if [ "$idf_app" = 0 ]; then
        bu="$app/build-$target"
        out="$bu/$name"
        rm -f "$out"            # relinked: CMake cannot see libespix or libc change under espix-cc
        # Nor the compiler's own headers: a new espix-cc or libc means compiling everything again.
        built="$bu/.espix-built"
        if [ -f "$built" ] && [ -n "$(find "$root/tools/espix-cc" "$root/cmake/espix-uapp.cmake" \
                "$root/build-libc-$target/include/picolibc.h" -newer "$built" -print -quit 2>/dev/null)" ]; then
            rm -rf "$bu"
        fi
        { cmake -S "$app" -B "$bu" -DCMAKE_TOOLCHAIN_FILE="$root/cmake/espix-uapp.cmake" \
              -DESPIX_TARGET="$target" > "$app/build.log" 2>&1 \
          && cmake --build "$bu" -j "$jobs" >> "$app/build.log" 2>&1; } \
            || { echo "build-apps: $name: build failed; see $app/build.log" >&2; exit 1; }
        [ -f "$out" ] || { echo "build-apps: $name: expected $out, which the build did not produce" >&2; exit 1; }
        touch "$built"
        # Staged without debug info; the build keeps it for addr2line.
        "$root/tools/espix-cc" --target="$target" --strip "$out" "$staged"
    else
        build_idf_app
    fi
    printf '%s\n' "$target" > "$stamp"
    stage_manifest
    echo "build-apps: staged $name ($(wc -c < "$staged" | tr -d ' ') bytes)"
done

# Every app shares one /lib/libc.so (tools/build-libc.sh), this target's.
if [ -f "$root/build-libc-$target/split/libc.so.staged" ]; then
    mkdir -p "$root/fsroot/lib"
    cp "$root/build-libc-$target/split/libc.so.staged" "$root/fsroot/lib/libc.so"
    printf '%s\n' "$target" > "$root/fsroot/lib/.espix-target-libc.so"
fi
