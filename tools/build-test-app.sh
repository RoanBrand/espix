#!/usr/bin/env bash
#
# Build the test app and stage it where both delivery routes expect it.
#
# Separate from build-apps.sh on purpose: that one globs apps/*/ and stages into
# fsroot/bin, which would put a test binary into every rootfs image. This one
# builds exactly tests/app and stages to fsroot/home/esp/, the same path the
# suite scp's to -- so `make fs` and the suite's own copy land in one place
# rather than two.
#
# Staging under a home directory is not only tidiness: espix's ownership rule
# gives a file to the account whose home contains it, and its mode rule sees ELF
# magic, so the binary arrives esp:esp and 0755 with no chown, no chmod and no
# attribute data in the image at all.

set -eu

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="$root/tests/app"
stage_dir="$root/fsroot/home/esp"
stage="$stage_dir/testapp"
target="${IDF_TARGET:-}"
if [ -z "$target" ]; then
    target=$(head -n1 "$root/.espix/active" 2>/dev/null || true)
fi
target="${target:-esp32s3}"

[ -f "$app/CMakeLists.txt" ] || { echo "build-test-app: no project at $app" >&2; exit 1; }

mkdir -p "$stage_dir"

# `idf.py elf` needs a Makefiles generator, which is what the apps use too.
have=""
[ -f "$app/sdkconfig" ] && have=$(grep '^CONFIG_IDF_TARGET=' "$app/sdkconfig" 2>/dev/null | head -1 | cut -d'"' -f2)
if [ "$have" != "$target" ]; then
    # A build directory left by a failed configure is not a CMake build
    # directory yet, and set-target's implicit fullclean refuses to touch it --
    # so a (re)configure has to start from nothing. A stale target's config is
    # the same situation for a different reason.
    rm -rf "$app/build"
    "$root/tools/idf.sh" -C "$app" -G 'Unix Makefiles' set-target "$target" \
        > "$app/build-test-app.log" 2>&1 \
        || { echo "build-test-app: set-target failed; see $app/build-test-app.log" >&2; exit 1; }
fi

if ! "$root/tools/idf.sh" -C "$app" elf >> "$app/build-test-app.log" 2>&1; then
    echo "build-test-app: build failed; see $app/build-test-app.log" >&2
    exit 1
fi

elf="$app/build/testapp.app.elf"
[ -f "$elf" ] || { echo "build-test-app: expected $elf" >&2; exit 1; }

cp "$elf" "$stage"
echo "build-test-app: staged $(basename "$stage") ($(wc -c < "$stage" | tr -d ' ') bytes)"
