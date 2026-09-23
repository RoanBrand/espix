#!/usr/bin/env bash
#
# Where each board is on the LAN, read from the gitignored .espix/hosts.
#
#   <model> <iface> <address>
#
# model: s3, s31 (esp32s3/esp32s31 also accepted)
# iface: eth, wifi, usb (free-form; preference is eth, then wifi, then usb)
#
# Shared by tools/flash-ota.sh, tests/lib/device.sh and anything else that
# needs to find a board, so there is one place an address is written down.
# ESPIX_HOST wins over all of it; ESPIX_IFACE pins an interface.
#
# Source this; do not run it.

ESPIX_HOSTS_ROOT="${ESPIX_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ESPIX_HOSTS_FILE="${ESPIX_HOSTS_FILE:-$ESPIX_HOSTS_ROOT/.espix/hosts}"

# Normalise a target name to the model key the file uses.
espix_host_model() {
    case "$1" in
        esp32s3|s3) printf 's3\n' ;;
        esp32s31|s31) printf 's31\n' ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# The model this tree builds for, from .espix/active or ESPIX_TARGET.
espix_hosts_target() {
    local t="${ESPIX_TARGET:-}"
    if [ -z "$t" ] && [ -f "$ESPIX_HOSTS_ROOT/.espix/active" ]; then
        t="$(tr -d ' \t\r\n' < "$ESPIX_HOSTS_ROOT/.espix/active")"
    fi
    [ -n "$t" ] || t=esp32s3
    espix_host_model "$t"
}

espix_host_ifaces() {
    if [ -n "${ESPIX_IFACE:-}" ]; then
        printf '%s\n' "$ESPIX_IFACE"
    else
        printf 'eth\nwifi\nusb\n'
    fi
}

espix_host_get() {  # <model> <iface>
    [ -f "$ESPIX_HOSTS_FILE" ] || return 0
    awk -v m="$(espix_host_model "$1")" -v i="$2" \
        '!/^[[:space:]]*#/ && NF >= 3 && $1 == m && $2 == i { print $3; exit }' \
        "$ESPIX_HOSTS_FILE"
}

espix_host_iface_of() {  # <model> <address> -> iface
    [ -f "$ESPIX_HOSTS_FILE" ] || return 0
    awk -v m="$(espix_host_model "$1")" -v a="$2" \
        '!/^[[:space:]]*#/ && NF >= 3 && $1 == m && $3 == a { print $2; exit }' \
        "$ESPIX_HOSTS_FILE"
}

# Every address for the model, most preferred first, one per line.
espix_host_candidates() {  # <model> [iface]
    local ifc
    if [ "$#" -ge 2 ] && [ -n "$2" ]; then
        espix_host_get "$1" "$2"
        return 0
    fi
    for ifc in $(espix_host_ifaces); do
        espix_host_get "$1" "$ifc"
    done
}

# Reachable on TCP 22? If no probe tool exists, say yes so the first configured
# address is used rather than none.
#
# Two spellings, because the *connect* timeout is what matters and the flag is
# not the same everywhere: macOS nc needs -G (plain -w bounds only the idle read,
# so a black-holed address hangs north of a minute), while GNU/OpenBSD nc has no
# -G and bounds the connect with -w. Try each.
espix_host_up() {  # <address>
    if command -v nc >/dev/null 2>&1; then
        nc -z -G 2 -w 2 "$1" 22 >/dev/null 2>&1 && return 0
        nc -z -w 2 "$1" 22 >/dev/null 2>&1 && return 0
        return 1
    fi
    return 0
}

# The best address to use: the first candidate that answers, else the most
# preferred configured one, so a caller can report a real address.
espix_host_pick() {  # <model>
    local c
    for c in $(espix_host_candidates "$1"); do
        if espix_host_up "$c"; then printf '%s\n' "$c"; return 0; fi
    done
    for c in $(espix_host_candidates "$1"); do
        printf '%s\n' "$c"; return 0
    done
    return 1
}

# Add or replace a line, preserving the rest of the file. An address of '-'
# removes the entry.
espix_host_set() {  # <model> <iface> <address>
    local m tmp
    m="$(espix_host_model "$1")"
    mkdir -p "$(dirname "$ESPIX_HOSTS_FILE")"
    tmp="$ESPIX_HOSTS_FILE.tmp.$$"
    if [ -f "$ESPIX_HOSTS_FILE" ]; then
        awk -v m="$m" -v i="$2" \
            '/^[[:space:]]*#/ { print; next } NF < 3 || $1 != m || $2 != i { print }' \
            "$ESPIX_HOSTS_FILE" > "$tmp" || return 1
    fi
    if [ "$3" != "-" ]; then
        printf '%s %s %s\n' "$m" "$2" "$3" >> "$tmp"
    fi
    mv "$tmp" "$ESPIX_HOSTS_FILE"
}
