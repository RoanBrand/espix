#!/usr/bin/env bash
#
# Build a small littlefs image from a directory, for the "storage" partition.
#
#   tools/make-fs-image.sh <source-dir> <output.bin>
#
# The image is sized to its contents, not to the partition. The kernel mounts the
# rootfs with grow_on_mount, which makes littlefs expand the image to the whole
# partition on its first mount -- so the image, and any release that carries it,
# stays small while the device still ends up with the full filesystem.
#
# The tool is the same littlefs-python the managed component uses, at the same
# version and in the same place (build/littlefs_py_venv); it is created here if
# no build has made it yet.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    sed -n '3,5p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
src="$1"
out="$2"

if [ ! -d "$src" ]; then
    printf 'make-fs-image: %s is not a directory\n' "$src" >&2
    exit 1
fi

venv="$root/build/littlefs_py_venv"
tool="$venv/bin/littlefs-python"
req="$root/managed_components/joltwallet__littlefs/image-building-requirements.txt"

if [ ! -x "$tool" ]; then
    eval "$("$root/tools/idf.sh" --env)"
    "$ESPIX_PYTHON" -m venv "$venv"
    "$venv/bin/pip" install -q -r "$req"
fi

# Content plus a margin, rounded up to 64 KiB, with a 256 KiB floor: littlefs
# wants some free blocks to work with, and the kernel grows the image on first
# mount regardless, so there is nothing to gain by making it larger.
bytes=$(du -sk "$src" | awk '{print $1 * 1024}')
size=$(( ((bytes * 2) / 65536 + 1) * 65536 ))
if [ "$size" -lt 262144 ]; then
    size=262144
fi

"$tool" create "$src" "$out" -v \
    --fs-size="$size" --name-max=64 --block-size=4096

printf 'make-fs-image: %s (%s bytes; grows to the partition on first mount)\n' \
    "$out" "$size"
