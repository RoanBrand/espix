#!/usr/bin/env bash
# The board's serial port, or a refusal.
#
# Deliberately refuses to choose when several match: picking one and being wrong
# means flashing the wrong board, which is worse than being asked.
set -u

case "$(uname -s)" in
    Darwin) pattern='/dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*' ;;
    *)      pattern='/dev/ttyUSB* /dev/ttyACM*' ;;
esac

found=""
count=0
for p in $pattern; do
    [ -e "$p" ] || continue
    found="$p"
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "port.sh: no serial port found (looked for: $pattern)" >&2
    echo "port.sh: pass PORT=/dev/... to say which" >&2
    exit 1
fi
if [ "$count" -gt 1 ]; then
    echo "port.sh: several serial ports match; pass PORT=/dev/... to choose:" >&2
    for p in $pattern; do [ -e "$p" ] && echo "  $p" >&2; done
    exit 1
fi

printf '%s' "$found"
