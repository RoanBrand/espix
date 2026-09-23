#!/usr/bin/env bash
#
# Cut one release that serves every target, in one command.
#
#   make release-all
#   tools/release-all.sh --dry-run
#
# release.sh builds and publishes one target per run; running it for each means
# the last run's merged manifest and notes cover all of them. This loops the
# targets in RELEASE_TARGETS (default: esp32s3 esp32s31).
set -eu

dry=""
if [ "${1:-}" = "--dry-run" ]; then
    dry="--dry-run"
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

# Every buildable target. The P4 is planned, so it is not here.
targets="${RELEASE_TARGETS:-esp32s3 esp32s31}"

for t in $targets; do
    printf 'release-all: %s\n' "$t"
    ESPIX_TARGET="$t" "$root/tools/release.sh" $dry
done
