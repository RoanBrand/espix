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

# /boot accumulates one 3 MB kernel per install and nothing prunes them, so a
# few OTA cycles fill the rootfs and the *next* push fails for want of space --
# reported only as "could not copy the image to the board", with no mention of
# why. Drop every kernel but the running one before pushing, so the copy has
# room. The proper place for this is the loader, which is what installs them;
# until it does it, the dev loop does.
if [ -n "$before" ]; then
    keep="${before#\#}"
    # One rm per file, because tools/esp.sh execs a path rather than a shell:
    # a remote `for` is "for: command not found".
    dev_once 'ls /boot' 2>/dev/null | tr -d '\r' | while read -r f; do
        case "$f" in
            *"$keep"*) ;;
            espix-*.bin) dev_once "sudo rm /boot/$f" >/dev/null 2>&1 || true ;;
        esac
    done
fi

# Two more things live on the rootfs and are worth reclaiming before the copy:
# a stored core dump, which also keeps every unit in safe mode until it is
# erased, and the image from the previous install, still in /tmp because the
# loader needs it until the reboot that reclaims it. Both are the difference
# between a copy that fits and "could not copy the image to the board".
if [ -n "$before" ]; then
    dev_once 'sudo coredump erase' >/dev/null 2>&1 || true
    # One rm per file, for the same reason as /boot above: tools/esp.sh execs a
    # path, so a remote glob is not expanded.
    dev_once 'ls /tmp' 2>/dev/null | tr -d '\r' | while read -r f; do
        case "$f" in
            espix.bin) ;;                   # the one this run replaces
            *) dev_once "sudo rm /tmp/$f" >/dev/null 2>&1 || true ;;
        esac
    done
fi

# Now that the space has been reclaimed, say whether it is enough *before*
# pushing 3 MB at a board that cannot hold it -- a copy that fails for want of
# space reports the same thing as a dropped connection, and telling them apart
# afterwards has cost more than this check does.
#
# /boot cannot simply be wiped: the loader stalls on a missing known-good image
# ("the known-good image is missing from /boot"), so the running kernel stays.
# It is pruned properly after the reboot below, which is what keeps /boot from
# settling at two kernels and filling the rootfs.
if [ -n "$before" ]; then
    img_bytes=$(wc -c < "$bin" | tr -d ' ')
    avail_kb="$(dev_once 'df' 2>/dev/null | tr -d '\r' |
                awk '$NF == "/" { print $(NF - 2); exit }')"
    if [ -z "$avail_kb" ]; then
        printf 'flash-ota: could not read the free space on the board\n' >&2
        exit 1
    fi
    avail_bytes=$((avail_kb * 1024))
    if [ "$avail_bytes" -lt $((img_bytes + 65536)) ]; then
        printf 'flash-ota: not enough space on the board for the image\n' >&2
        printf '  image %s bytes, free %s bytes (%s kB)\n' \
               "$img_bytes" "$avail_bytes" "$avail_kb" >&2
        printf '  /boot and /tmp were just pruned, so this is the rootfs itself\n' >&2
        exit 1
    fi
fi

printf 'flash-ota: %s -> %s (%s)\n' "$bin" "$ESPIX_HOST" "$ESPIX_TARGET"
if ! dev_push "$bin" /tmp/espix.bin >/dev/null 2>&1; then
    printf 'flash-ota: could not copy the image to the board\n' >&2
    exit 1
fi

rc=0
dev_ssh_raw 'sudo upgrade --file /tmp/espix.bin' || rc=$?
# No rm here: the loader installs the image from /tmp, so it has to survive
# until the reboot. The kernel clears /tmp on boot, which is what reclaims it.

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

# The installed kernel is now the running one, so the kernel it replaced is
# dead weight in /boot -- and the pre-push prune could not remove it, because at
# that point it *was* the running one. Pruning here is what keeps /boot at one
# kernel rather than oscillating between one and two. The running image stays:
# the loader needs a known-good one.
if [ -n "$after" ]; then
    keep="${after#\#}"
    dev_once 'ls /boot' 2>/dev/null | tr -d '\r' | while read -r f; do
        case "$f" in
            *"$keep"*) ;;
            espix-*.bin) dev_once "sudo rm /boot/$f" >/dev/null 2>&1 || true ;;
        esac
    done
fi
