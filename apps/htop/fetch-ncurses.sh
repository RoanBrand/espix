#!/usr/bin/env bash
#
# Fetch ncurses 6.4, the terminal library htop draws with. Pinned to the commit
# the tag names, which is what the release tarball is cut from and what carries
# a generated configure script.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tp="$here/third_party"
mkdir -p "$tp"

REPO="https://github.com/mirror/ncurses.git"
REV="79b9071f2be20a24c7be031655a5638f6032f29f"   # v6.4
DIR="$tp/ncurses"

if [ ! -d "$DIR/.git" ]; then
    echo "ncurses: cloning..."
    git clone -q "$REPO" "$DIR"
fi
if [ "$(git -C "$DIR" rev-parse HEAD 2>/dev/null || true)" != "$REV" ]; then
    echo "ncurses: checking out ${REV:0:12}..."
    git -C "$DIR" fetch -q origin "$REV" 2>/dev/null || git -C "$DIR" fetch -q origin
    git -C "$DIR" checkout -q "$REV"
fi
echo "ncurses: $REV in $DIR"
