#!/usr/bin/env bash
#
# Write build/espix-ota.json, the manifest a release publishes next to its image.
#
#   tools/ota-manifest.sh <release-asset-base-url>
#
# e.g.
#   tools/ota-manifest.sh https://github.com/RoanBrand/espix/releases/download/v0.3.1
#
# The build id is the same content hash the device reports as "uname -v", read
# straight out of the app descriptor in build/espix.bin -- the first nine hex
# digits of app_elf_sha256 at offset 0xB0. No toolchain and no second build, and
# it cannot disagree with what the running image will say about itself.
#
# The version is version.txt, which is also what ESP-IDF puts in the descriptor,
# so the manifest and the image agree by construction.

set -u

if [ "$#" -ne 1 ]; then
    sed -n '3,7p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
bin="$root/build/espix.bin"

if [ ! -f "$bin" ]; then
    printf 'ota-manifest: %s does not exist; build first\n' "$bin" >&2
    exit 1
fi

ver=$(tr -d ' \t\r\n' < "$root/version.txt")
sha=$(dd if="$bin" bs=1 skip=176 count=32 2>/dev/null |
      od -An -tx1 -v | tr -d ' \n' | cut -c1-9)

if [ -z "$ver" ] || [ -z "$sha" ]; then
    printf 'ota-manifest: could not read version.txt or the app descriptor\n' >&2
    exit 1
fi

out="$root/build/espix-ota.json"
cat > "$out" <<EOF
{
  "name": "espix",
  "version": "$ver",
  "build": "$sha",
  "chip": "esp32s3",
  "url": "$1/espix.bin"
}
EOF

printf 'ota-manifest: %s\n' "$out"
cat "$out"
