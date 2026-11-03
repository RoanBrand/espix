#!/usr/bin/env bash
#
# Fetch htop's release tarball, pinned. Not vendored: this script is the only
# place the tree names the version, and a checkout whose pin already matches
# does nothing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tp="$here/third_party"
mkdir -p "$tp"

VER="3.5.3"
SHA="a8b164386494cb85bb255a415a3f5f80afe7a0c4491da5d113b3a0f951087e65"
TAR="$tp/htop-$VER.tar.xz"
DIR="$tp/htop"

if [ -d "$DIR" ] && [ -f "$DIR/configure" ]; then
    exit 0
fi

if [ ! -f "$TAR" ] || ! echo "$SHA  $TAR" | shasum -a 256 -c - >/dev/null 2>&1; then
    echo "htop: fetching $VER..."
    curl -fsSL -o "$TAR" "https://github.com/htop-dev/htop/releases/download/$VER/htop-$VER.tar.xz"
fi
echo "$SHA  $TAR" | shasum -a 256 -c -

rm -rf "$DIR"
mkdir -p "$DIR"
tar xJf "$TAR" -C "$DIR" --strip-components=1
echo "htop: $VER in $DIR"
