#!/usr/bin/env bash
#
# Build and run every host unit-test app: components/*/test_apps/host.
#
#   make unit
#
# Each is a small IDF project for the linux target, so it runs here in seconds
# with no board, and nothing in it is linked into the firmware. It tests the
# pure logic a component keeps in files of its own; tests/ is still where the
# running system is tested.

set -eu
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

for app in "$root"/components/*/test_apps/host; do
    [ -f "$app/CMakeLists.txt" ] || continue
    name="${app#"$root"/components/}"
    log="$app/unit.log"

    if [ ! -f "$app/sdkconfig" ] ||
       ! grep -q '^CONFIG_IDF_TARGET="linux"' "$app/sdkconfig"; then
        ESPIX_TARGET=linux "$root/tools/idf.sh" -C "$app" set-target linux \
            > "$log" 2>&1 || { echo "unit: $name: set-target failed; see $log" >&2; status=1; continue; }
    fi
    if ! ESPIX_TARGET=linux "$root/tools/idf.sh" -C "$app" build >> "$log" 2>&1; then
        echo "unit: $name: build failed; see $log" >&2
        status=1
        continue
    fi

    elf=$(ls "$app"/build/*.elf | head -1)
    echo "unit: $name"
    "$elf" || status=1
done

exit $status
