#!/usr/bin/env bash
#
# Fetch what the Doom app builds from: the portable engine, and nothing else.
# It is not vendored -- this script is the only place the tree names it, and a
# build whose pin already matches does nothing.
#
# Run by apps/doom/CMakeLists.txt before project(), so the sources exist by the
# time the engine component globs them. Safe to run by hand.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tp="$here/third_party"
mkdir -p "$tp"

ENGINE_REPO="https://github.com/ozkl/doomgeneric.git"
# The commit this app was built and tested against, so a new upstream cannot
# change the code under us without the pin changing too.
ENGINE_REV="dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284"

# The engine. The rev file is what makes a second configure cheap: no network,
# no checkout.
stamp="$tp/doomgeneric.rev"
if [ ! -d "$tp/doomgeneric/.git" ]; then
    echo "doom: cloning doomgeneric..."
    git clone -q "$ENGINE_REPO" "$tp/doomgeneric"
fi
if [ "$(cat "$stamp" 2>/dev/null || true)" != "$ENGINE_REV" ]; then
    echo "doom: checking out $ENGINE_REV..."
    git -C "$tp/doomgeneric" fetch -q origin "$ENGINE_REV" 2>/dev/null || \
        git -C "$tp/doomgeneric" fetch -q origin
    git -C "$tp/doomgeneric" checkout -q "$ENGINE_REV"
    echo "$ENGINE_REV" > "$stamp"
fi

# The game data is deliberately NOT here, and not packaged either. It is 4 MB of
# shareware that is not ours to ship, it barely fits beside the kernel on a
# 16 MB board, and a release image that carries one game's data is a release
# image that carries everyone's. The URL and its digest live in the release
# image at fsroot/etc/apps/doom.conf, and the launcher fetches it on the first
# launch; this script only builds the program.
