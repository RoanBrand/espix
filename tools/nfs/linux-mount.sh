#!/bin/sh
#
# Mount espix's NFS export from a real Linux client and use it.
#
#   tools/nfs/linux-mount.sh            # read: mount, list, stat, read a file
#   tools/nfs/linux-mount.sh rw         # write: create, copy, rename, remove
#
# A kernel client is the test that matters -- it decodes replies with its own
# XDR, so a field in the wrong place is an error here and not in the server's
# own tool. It runs in a container because the Linux that is available on a Mac
# is Docker's; the kernel has nfs built in and --network host puts it on the LAN.
#
# ESPIX_HOST and ESPIX_EXPORT override the defaults.
#
set -u

HOST="${ESPIX_HOST:-192.168.110.203}"
EXPORT="${ESPIX_EXPORT:-/mnt/sda1}"
MODE="${1:-ro}"

if [ "${1:-}" != "--inside" ]; then
    # An absolute path: docker reads a relative one as a volume name.
    here=$(cd "$(dirname "$0")" && pwd)
    self="$here/$(basename "$0")"
    exec docker run --rm --privileged --network host -v "$self:/t.sh:ro" \
         redis:7-alpine sh /t.sh --inside "$HOST" "$EXPORT" "$MODE"
fi

HOST="$2"; EXPORT="$3"; MODE="$4"
apk add --no-cache nfs-utils >/dev/null 2>&1
mkdir -p /mnt/x
mount -t nfs "$HOST:$EXPORT" /mnt/x || { echo "mount failed"; exit 1; }
mount | grep /mnt/x | sed 's/^/  /'

echo "--- listing"
ls -ln /mnt/x | head -3

echo "--- reading a file the device already had"
f=$(ls /mnt/x | grep -v '^\._' | head -1)
if [ -n "$f" ] && [ -f "/mnt/x/$f" ]; then
    echo "  $f: $(wc -c < "/mnt/x/$f") bytes"
fi

if [ "$MODE" = "rw" ]; then
    d=/mnt/x/nfstest-cli
    mkdir -p "$d" || echo "  mkdir failed"
    echo "--- writing 2 MiB and comparing hashes"
    dd if=/dev/urandom of=/tmp/src.bin bs=4096 count=512 status=none
    sum=$(sha256sum < /tmp/src.bin | cut -d' ' -f1)
    start=$(date +%s)
    cp /tmp/src.bin "$d/copy.bin"
    echo "  copy took $(( $(date +%s) - start )) s"
    sum2=$(sha256sum < "$d/copy.bin" | cut -d' ' -f1)
    if [ "$sum" = "$sum2" ]; then echo "  identical"; else echo "  MISMATCH"; fi

    echo "--- writing a small file and reading it back"
    printf 'espix over nfs\n' > "$d/note.txt"
    cat "$d/note.txt" | sed 's/^/  /'

    echo "--- rename"
    mv "$d/note.txt" "$d/renamed.txt" && echo "  renamed"
    ls -ln "$d" | sed 's/^/  /'

    echo "--- links, which this filesystem has not got"
    ln -s target "$d/sym" 2>&1 && echo "  symlink created (unexpected)" || echo "  symlink refused"
    ln "$d/copy.bin" "$d/hard" 2>&1 && echo "  hard link created (unexpected)" || echo "  hard link refused"

    echo "--- a directory with something in it is not removable"
    rmdir "$d" 2>&1 && echo "  rmdir removed it (unexpected)" || echo "  rmdir refused"

    echo "--- remove"
    rm -f "$d/copy.bin" "$d/renamed.txt"
    ls -ln "$d" | sed 's/^/  /'
fi

echo "--- unmount"
umount.nfs /mnt/x 2>/dev/null || umount /mnt/x
echo "  unmounted"
