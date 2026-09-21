#!/usr/bin/env bash
#
# Copy build/espix.bin to the board and queue it, over SSH. No cable.
#
#   make flash-ota
#   ESPIX_HOST=10.0.0.5 make flash-ota
#
# This is the dev loop the OTA work exists to enable: build, push, reboot, and
# never touch the UART.
#
# The image is copied to /tmp and adopted from there with "upgrade --file",
# which puts it in /boot and selects the loader; the next reboot installs it.
# The obvious alternative -- streaming it into the command's stdin -- does not
# work with this shell: a command that declares a stack of its own runs on a new
# task while the connection task waits on it (session.c run_on_own_task), and the
# connection task is the only reader of the wire, so nothing drains the channel.
#
# It still uses the same login path the test suite does (SSH_ASKPASS via
# tests/lib/device.sh), so there is no second way in that can rot separately
# from the one exercised on every run.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPIX_ROOT="$(cd "$here/.." && pwd)"
export ESPIX_ROOT

. "$ESPIX_ROOT/tests/lib/portable.sh"
. "$ESPIX_ROOT/tests/lib/assert.sh"
. "$ESPIX_ROOT/tests/lib/device.sh"

bin="$ESPIX_ROOT/build/espix.bin"
if [ ! -f "$bin" ]; then
    printf 'flash-ota: %s does not exist; build first\n' "$bin" >&2
    exit 1
fi

dev_askpass_init
trap 'dev_askpass_cleanup' EXIT

printf 'flash-ota: %s -> %s\n' "$bin" "$ESPIX_HOST"
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

printf 'flash-ota: queued. Install it with:  tools/esp.sh reboot\n'
