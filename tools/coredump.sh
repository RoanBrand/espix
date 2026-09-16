#!/usr/bin/env bash
#
# Read the core dump off the board, then decode it.
#
#   tools/coredump.sh [file]        default: coredump.bin
#
# Why this is not `idf.py coredump-info`, which is what the make target used to
# call, and which failed every other time:
#
#   * It reads *exactly* the dump's length -- 37280 bytes, say -- which is not a
#     multiple of the flash block size, and esptool 5.4 fails on the short final
#     block: "Corrupt data, expected 0x1000 bytes but received 0x9d", or "invalid
#     head of packet". Reading the whole partition is aligned and does not care.
#   * It reads at 460800. This link drops bytes at that speed and fails at the
#     last block; 115200 has not failed once, and the read takes seconds either
#     way.
#   * It decodes as it reads, so a failure in either half loses both. The bytes
#     are kept here instead, and a decode can be re-run with no port at all.
#
# The port is checked first, through tools/port-holder.sh: two readers on one
# cu.* device split the byte stream, and "corrupt data" is usually that.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-coredump.bin}"

# espix's coredump partition: 0x410000/0x10000 is partitions/esp32s3-16mb.csv,
# and the 8MB board is 0x310000. Overridable rather than parsed, because reading
# the wrong offset would look exactly like corruption.
off="${ESPIX_COREDUMP_OFF:-0x410000}"
size="${ESPIX_COREDUMP_SIZE:-0x10000}"
baud="${ESPIX_COREDUMP_BAUD:-115200}"

port="${PORT:-${ESPIX_PORT:-}}"
if [ -z "$port" ]; then
    port=$("$here/port.sh") || exit 1
fi

"$here/port-holder.sh" "$port" || {
    echo "coredump: $port is in use (above); stop that reader first" >&2
    exit 1
}

# The IDF virtualenv carries esptool; the system python usually does not.
if [ -x "$here/idf.sh" ]; then
    eval "$(bash "$here/idf.sh" --env 2>/dev/null)" 2>/dev/null || true
fi
py="${ESPIX_PYTHON:-python3}"

echo "coredump: reading $size bytes at $off from $port at $baud..." >&2
if ! "$py" -m esptool -p "$port" -b "$baud" read-flash "$off" "$size" "$out"; then
    echo "coredump: the read failed and nothing was decoded" >&2
    exit 1
fi

echo "coredump: bytes are in $out; decoding from the file..." >&2
"$here/idf.sh" coredump-info -c "$out"

echo "coredump: keep $out for a second look: tools/idf.sh coredump-info -c $out" >&2
echo "coredump: and erase it on the device with 'coredump erase'" >&2
