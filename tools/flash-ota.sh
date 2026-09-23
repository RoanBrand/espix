#!/usr/bin/env bash
#
# Push the active target's firmware to the board over the network and install
# it: copy, adopt, reboot, wait. No cable.
#
#   make flash-ota
#   ESPIX_HOST=192.168.110.203 make flash-ota
#   ESPIX_NO_REBOOT=1 make flash-ota
#
# This is the dev loop the OTA work exists to enable: build, push, reboot, and
# never touch the UART. The interface used is whichever the board routes over,
# so a live cable sends the update over Ethernet and otherwise it goes over
# WiFi -- nothing here names an interface.
#
# The image is copied to /tmp and adopted from there with "upgrade --file",
# which puts it in /boot and selects the loader; the reboot installs it. The
# obvious alternative -- streaming it into the command's stdin -- does not work
# with this shell: a command that declares a stack of its own runs on a new task
# while the connection task waits on it (session.c run_on_own_task), and the
# connection task is the only reader of the wire, so nothing drains the channel.
#
# The address comes from the gitignored .espix/hosts, for this target,
# preferring a live cable to WiFi; ESPIX_HOST overrides it. tools/espix host
# edits that file.
#
# It uses the same login path the test suite does (SSH_ASKPASS via
# tests/lib/device.sh), so there is no second way in that can rot separately
# from the one exercised on every run.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPIX_ROOT="$(cd "$here/.." && pwd)"
export ESPIX_ROOT

. "$ESPIX_ROOT/tests/lib/portable.sh"
. "$ESPIX_ROOT/tests/lib/assert.sh"

eval "$("$ESPIX_ROOT/tools/idf.sh" --env)"
bin="$ESPIX_BUILD/espix.bin"
if [ ! -f "$bin" ]; then
    printf 'flash-ota: %s does not exist; build first\n' "$bin" >&2
    exit 1
fi

# Resolve the host before device.sh: an explicit ESPIX_HOST wins, otherwise
# the gitignored .espix/hosts for this target (tools/espix host), preferring a
# live cable to WiFi.
. "$ESPIX_ROOT/tools/hosts.sh"
if [ -z "${ESPIX_HOST:-}" ]; then
    ESPIX_HOST="$(espix_host_pick "$(espix_hosts_target)" || true)"
fi
export ESPIX_HOST

. "$ESPIX_ROOT/tests/lib/device.sh"

dev_askpass_init
trap 'dev_askpass_cleanup' EXIT

before="$(dev_once 'uname -v' 2>/dev/null || true)"

printf 'flash-ota: %s -> %s (%s)\n' "$bin" "$ESPIX_HOST" "$ESPIX_TARGET"
if ! dev_push "$bin" /tmp/espix.bin >/dev/null 2>&1; then
    printf 'flash-ota: could not copy the image to the board\n' >&2
    exit 1
fi

rc=0
dev_ssh_raw 'sudo upgrade --file /tmp/espix.bin' || rc=$?
dev_ssh_raw 'rm /tmp/espix.bin' >/dev/null 2>&1 || true

if [ "$rc" -ne 0 ]; then
    printf 'flash-ota: the install failed; the board is still running what it was\n' >&2
    exit 1
fi

if [ -n "${ESPIX_NO_REBOOT:-}" ]; then
    printf 'flash-ota: queued. Install it with:  tools/esp.sh reboot\n'
    exit 0
fi

printf 'flash-ota: rebooting to install it\n'
dev_ssh_raw 'sudo reboot' >/dev/null 2>&1 || true

printf 'flash-ota: waiting for %s' "$ESPIX_HOST"
back=""
i=0
while [ "$i" -lt 60 ]; do
    sleep 2
    i=$((i + 1))
    printf '.'
    if espix_host_up "$ESPIX_HOST"; then
        back=1
        break
    fi
done
printf '\n'

if [ -z "$back" ]; then
    printf 'flash-ota: %s did not come back; check the console\n' "$ESPIX_HOST" >&2
    exit 1
fi

after="$(dev_once 'uname -v' 2>/dev/null || true)"
if [ -n "$before" ] && [ "$before" = "$after" ]; then
    printf 'flash-ota: back, running the same build (%s)\n' "$after"
else
    printf 'flash-ota: installed and running %s\n' "${after:-an unknown build}"
fi
