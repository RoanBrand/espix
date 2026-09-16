#!/usr/bin/env bash
# The board's serial port, or a refusal.
#
# Deliberately refuses to choose when several match: picking one and being wrong
# means flashing the wrong board, which is worse than being asked.
set -u

case "$(uname -s)" in
    # usbserial* without the dash: some adapters enumerate as cu.usbserial and
    # others as cu.usbserial-210, and two patterns matching one file would make
    # this script refuse a port that is perfectly fine -- it counts matches.
    # wchusbserial covers the CH340/CH341 family, which is what most cheap
    # adapters are, and which the earlier list missed entirely.
    Darwin) pattern='/dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*' ;;
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
    echo "port.sh: 'ls /dev/cu.*' shows what the system has, if any USB serial" >&2
    echo "port.sh: adapter is plugged in at all" >&2
    echo "port.sh: pass PORT=/dev/... to say which" >&2
    exit 1
fi
if [ "$count" -gt 1 ]; then
    echo "port.sh: several serial ports match; pass PORT=/dev/... to choose:" >&2
    for p in $pattern; do [ -e "$p" ] && echo "  $p" >&2; done
    exit 1
fi

# And refuse a port somebody else already has.
#
# Two readers on one cu.* device split the byte stream between them -- macOS
# allows it, and the only symptom is a capture or a flash with holes in it -- so
# the check is worth making here, where every caller that is about to use the
# port comes through. The check itself is tools/port-holder.sh, which asks the
# exclusive tty.* node for the same UART and names the holders with lsof.
#
# Refused rather than resolved: a reader is usually a capture somebody started on
# purpose, and quietly killing it is a worse surprise than being asked.
if ! "$(dirname "$0")/port-holder.sh" "$found"; then
    printf 'port.sh: %s is already open (above); stop the reader first,\n' "$found" >&2
    printf 'port.sh: or choose another port with PORT=/dev/...\n' >&2
    exit 1
fi

printf '%s' "$found"
