#!/usr/bin/env bash
#
# Write <build>/espix-ota-<board>.json, the manifest a release publishes next to
# its image.
#
#   tools/ota-manifest.sh <release-asset-base-url> [build-dir]
#
# e.g.
#   tools/ota-manifest.sh https://github.com/RoanBrand/espix/releases/download/v0.3.1
#
# The board ("s3-n16r8") is read from the image's own generated header, so it is
# the board the binary was actually built for -- target, flash and PSRAM size --
# and the manifest, the asset name and the URL the device asks for all agree
# without anyone keeping a list.
#
# The build id is the same content hash the device reports as "uname -v", read
# straight out of the app descriptor in espix.bin -- the first nine hex digits of
# app_elf_sha256 at offset 0xB0. No toolchain and no second build, and it cannot
# disagree with what the running image will say about itself.
#
# The version is version.txt, which is also what ESP-IDF puts in the descriptor.

set -u

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    sed -n '3,7p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
if [ "$#" -eq 2 ]; then
    build="$2"
else
    build="$root/build"
fi
bin="$build/espix.bin"

if [ ! -f "$bin" ]; then
    printf 'ota-manifest: %s does not exist; build first\n' "$bin" >&2
    exit 1
fi

header="$build/esp-idf/espix_kernel/espix_version.h"
sdkconfig_h="$build/config/sdkconfig.h"
if [ ! -f "$header" ]; then
    printf 'ota-manifest: %s not found; build first\n' "$header" >&2
    exit 1
fi

board=$(sed -n 's/^#define ESPIX_BOARD "\(.*\)"$/\1/p' "$header" | head -1)
chip=$(sed -n 's/^#define CONFIG_IDF_TARGET "\(.*\)"$/\1/p' "$sdkconfig_h" | head -1)
if [ -z "$board" ]; then
    printf 'ota-manifest: no ESPIX_BOARD in %s\n' "$header" >&2
    exit 1
fi

ver=$(tr -d ' \t\r\n' < "$root/version.txt")
sha=$(dd if="$bin" bs=1 skip=176 count=32 2>/dev/null |
      od -An -tx1 -v | tr -d ' \n' | cut -c1-9)

if command -v sha256sum >/dev/null 2>&1; then
    img_sha=$(sha256sum "$bin" | awk '{print $1}')
else
    img_sha=$(shasum -a 256 "$bin" | awk '{print $1}')
fi

if [ -z "$ver" ] || [ -z "$sha" ]; then
    printf 'ota-manifest: could not read version.txt or the app descriptor\n' >&2
    exit 1
fi

out="$build/espix-ota-$board.json"
cat > "$out" <<EOF
{
  "name": "espix",
  "version": "$ver",
  "build": "$sha",
  "sha256": "$img_sha",
  "chip": "$chip",
  "board": "$board",
  "url": "$1/espix-$board-ota.bin"
}
EOF

printf 'ota-manifest: %s\n' "$out"
cat "$out"
