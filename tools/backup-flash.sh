#!/usr/bin/env bash
#
# Back up a board's whole flash, before changing anything on it.
#
#   tools/backup-flash.sh [PORT]
#
# The name comes from the chip's own MAC, which is unique per board and is also
# what espix uses to name the device on the network, so two boards never
# collide. A .txt beside the image records the MAC, the flash size, the date and
# the SHA-256, and the read is verified by a second pass over the flash.
#
#   ESPIX_BACKUP_DIR   where to write (default: ~/S31-backups)
#   BAUD               serial speed (default 460800)
#
# Reads the active target from .espix/active; run `tools/espix target s31`
# first. esptool on a preview target gets --no-stub.

set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
outdir="${ESPIX_BACKUP_DIR:-$HOME/S31-backups}"
baud="${BAUD:-230400}"

target="$(tr -d ' \t\r\n' < "$root/.espix/active" 2>/dev/null || true)"
target="${target:-esp32s3}"
preview=""

if [ "$#" -ge 1 ]; then
    port="$1"
else
    port="${PORT:-}"
fi
if [ -z "$port" ]; then
    port="$("$here/port.sh")" || exit 1
fi

eval "$("$here/idf.sh" --env)"
py="${ESPIX_PYTHON:-python3}"

mkdir -p "$outdir"

read_mac="$("$py" -m esptool --chip "$target" $preview -p "$port" -b "$baud" read-mac 2>&1)"
mac="$(printf '%s\n' "$read_mac" | sed -n 's/^MAC:[[:space:]]*//p' | head -1 | tr -d ' \r\n:')"
if [ -z "$mac" ]; then
    printf 'backup: no MAC came back; esptool said:\n%s\n' "$read_mac" >&2
    exit 1
fi

flash_id="$("$py" -m esptool --chip "$target" $preview -p "$port" -b "$baud" flash-id 2>&1)"
size="$(printf '%s\n' "$flash_id" | sed -n 's/^Detected flash size: //p' | head -1)"
[ -n "$size" ] || size="16MB"

base="$outdir/${target}-${mac}"
image="$base.bin"

printf 'backup: %s, %s -> %s\n' "$target" "$size" "$image"
"$py" -m esptool --chip "$target" $preview -p "$port" -b "$baud" \
    read-flash 0 "${size%MB}M" "$image"

printf 'backup: verifying (a second pass over the flash)...\n'
"$py" -m esptool --chip "$target" $preview -p "$port" -b "$baud" \
    verify-flash 0 "$image"

sha="$(shasum -a 256 "$image" | awk '{print $1}')"
{
    printf 'file:    %s\n' "$image"
    printf 'chip:    %s\n' "$target"
    printf 'mac:     %s\n' "$mac"
    printf 'flash:   %s\n' "$size"
    printf 'sha256:  %s\n' "$sha"
    printf 'date:    %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$base.txt"
printf 'backup: %s\n' "$image"
