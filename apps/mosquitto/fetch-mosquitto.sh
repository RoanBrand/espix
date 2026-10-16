#!/usr/bin/env bash
#
# Fetch what the Mosquitto app builds from: the broker's own source, and
# nothing else. It is not vendored -- this script is the only place the tree
# names it, and a checkout whose pin already matches does nothing.
#
# Run by apps/mosquitto/CMakeLists.txt before project(), so the sources exist
# by the time the component globs them. Safe to run by hand.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tp="$here/third_party"
mkdir -p "$tp"

# Eclipse Mosquitto, the reference MQTT broker. Version 2.1.2.
REPO="https://github.com/eclipse-mosquitto/mosquitto.git"
REV="83f2620f0b955b89aadce32cceb96a8e81950f5e"

stamp="$tp/mosquitto.rev"
if [ ! -d "$tp/mosquitto/.git" ]; then
    echo "mosquitto: cloning upstream..."
    git clone -q "$REPO" "$tp/mosquitto"
fi
if [ "$(cat "$stamp" 2>/dev/null || true)" != "$REV" ]; then
    echo "mosquitto: checking out ${REV:0:12}..."
    git -C "$tp/mosquitto" fetch -q origin "$REV" 2>/dev/null || \
        git -C "$tp/mosquitto" fetch -q origin
    git -C "$tp/mosquitto" checkout -q "$REV"
    echo "$REV" > "$stamp"
fi
