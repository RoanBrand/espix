#!/usr/bin/env bash
#
# Cut (or refresh) a release: tag v<version.txt>, build it, and publish the
# kernel, the OTA manifest, the loader and one-file flash images to GitHub.
#
#   make release
#   tools/release.sh --dry-run     # everything except push and gh
#
# The tag is made *before* the build because espix calls a build a release only
# when the built commit is exactly tagged and clean (espix_kernel's CMakeLists).
# If the tag already points at HEAD, the release is refreshed instead of
# recreated -- which is how a new board joins an existing version without moving
# the version or disturbing devices already on it. Bump version.txt when the
# kernel changes; leave it alone when only a board is added.
#
# gh does the GitHub side and is already authenticated.

set -eu

dry=0
if [ "$#" -ge 1 ] && [ "$1" = "--dry-run" ]; then
    dry=1
fi

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

die() { printf 'release: %s\n' "$*" >&2; exit 1; }

ver=$(tr -d ' \t\r\n' < "$root/version.txt")
[ -n "$ver" ] || die "version.txt is empty"
tag="v$ver"

remote=$(git -C "$root" remote get-url origin 2>/dev/null) \
    || die "no origin remote"
slug=$(printf '%s' "$remote" |
       sed -e 's#^git@github.com:##' -e 's#^https://github.com/##' -e 's#\.git$##')
case "$slug" in
    */*) ;;
    *) die "cannot parse a github owner/repo from '$remote'" ;;
esac

case "$(git -C "$root" status --porcelain)" in
    "") ;;
    *) die "working tree is dirty; commit first -- a release is the tagged commit" ;;
esac

# A tag for this version either does not exist yet (a new release) or points at
# HEAD (adding a board to one). Anything else means version.txt was not bumped.
created=0
if git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    [ "$(git -C "$root" rev-parse "$tag^{commit}")" = \
      "$(git -C "$root" rev-parse HEAD)" ] \
        || die "tag $tag points at another commit; bump version.txt"
    printf 'release: refreshing %s\n' "$tag"
else
    printf 'release: tagging %s\n' "$tag"
    git -C "$root" tag -a "$tag" -m "espix $ver"
    created=1
fi
trap 'if [ "$created" = 1 ]; then
          git -C "$root" tag -d "$tag" >/dev/null 2>&1 || true
      fi' ERR

# Reconfigure, not merely build: the release flag is decided when CMake runs, so
# a build directory configured before the tag existed would still say "dev".
printf 'release: building the kernel and loader\n'
( cd "$root" && tools/idf.sh reconfigure >/dev/null && tools/idf.sh build )
( cd "$root" && tools/idf.sh -C loader build >/dev/null )

header="$root/build/esp-idf/espix_kernel/espix_version.h"
board=$(sed -n 's/^#define ESPIX_BOARD "\(.*\)"$/\1/p' "$header" | head -1)
[ -n "$board" ] || die "no ESPIX_BOARD in $header; did the build fail?"
model=$(printf '%s' "$board" | cut -d- -f1)
kmodel=$(sed -n 's/^#define CONFIG_IDF_TARGET "\(.*\)"$/\1/p' "$root/build/config/sdkconfig.h" | head -1)
printf 'release: board %s\n' "$board"

# Assets are filed by board and target, so a release can hold several without
# the names colliding. The loader is per target, not per board: it is built
# without PSRAM and finds partitions by label, so one serves every S3 module.
img="$root/build/espix-$board.bin"
cp "$root/build/espix.bin" "$img"

loader="$root/build/espix-loader-$model.bin"
cp "$root/loader/build/espix_loader.bin" "$loader"

printf 'release: writing the manifest\n'
( cd "$root" && tools/ota-manifest.sh \
    "https://github.com/$slug/releases/download/$tag" )
manifest="$root/build/espix-ota-$board.json"
[ -f "$manifest" ] || die "no manifest was written"

# The rootfs a release carries is built from apps/ into a clean directory, never
# from the local fsroot/: that tree is a development convenience and may hold a
# test app or a personal wifi.conf, neither of which belongs in a release.
factory_root="$root/build/factory-fsroot"
rm -rf "$factory_root"
mkdir -p "$factory_root/bin"
( cd "$root" && ESPIX_APPS_STAGE="$factory_root/bin" tools/build-apps.sh >/dev/null )
factory_fs="$root/build/factory-storage.bin"
( cd "$root" && tools/make-fs-image.sh "$factory_root" "$factory_fs" >/dev/null )

# One-file flash images, offsets from the same partition table sdkconfig selects.
# The minimal one omits the rootfs: the kernel formats the partition and grows it
# itself on first boot, but /bin stays empty. The full one carries a small seed
# filesystem (grown on first mount) with the stock apps, and rewrites storage --
# it is the factory image, not the one to flash over a live device.
csv=$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' "$root/sdkconfig")
off_of() {
    awk -F, -v n="$1" '$1 == n { gsub(/ /, "", $4); print $4 }' "$root/$csv"
}
flashargs=$(head -1 "$root/build/flash_args")
fm=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-mode \([a-z]*\).*/\1/p')
ff=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-freq \([0-9a-z]*\).*/\1/p')
fs=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-size \([0-9A-Za-z]*\).*/\1/p')

minimal="$root/build/espix-$board-minimal.bin"
full="$root/build/espix-$board-full.bin"
merge() {
    out="$1"
    shift
    eval "$(cd "$root" && tools/idf.sh --env)"
    "$ESPIX_PYTHON" -m esptool --chip "$kmodel" merge-bin -o "$out" \
        --flash-mode "$fm" --flash-freq "$ff" --flash-size "$fs" "$@" >/dev/null
}
printf 'release: merging flash images\n'
merge "$minimal" \
    "0x0"    "$root/build/bootloader/bootloader.bin" \
    "0x8000" "$root/build/partition_table/partition-table.bin" \
    "0xf000" "$root/build/ota_data_initial.bin" \
    "$(off_of ota_0)" "$root/build/espix.bin" \
    "$(off_of ota_1)" "$loader"
merge "$full" \
    "0x0"    "$root/build/bootloader/bootloader.bin" \
    "0x8000" "$root/build/partition_table/partition-table.bin" \
    "0xf000" "$root/build/ota_data_initial.bin" \
    "$(off_of ota_0)" "$root/build/espix.bin" \
    "$(off_of ota_1)" "$loader" \
    "$(off_of storage)" "$factory_fs"

notes="$root/build/release-notes.md"
cat > "$notes" <<EOF
espix $ver

Flashing a board for the first time
-----------------------------------

Download espix-$board-minimal.bin and write it at offset 0:

    esptool.py --chip $kmodel -p <port> write_flash 0x0 espix-$board-minimal.bin

That is the whole system, and it makes its own filesystem on first boot. It has
no stock apps in /bin; take espix-$board-full.bin instead if you want those.

  espix-$board-minimal.bin   bootloader, partition table, kernel, loader -- smallest
  espix-$board-full.bin      the same plus the stock apps; reflashes everything
  espix-loader-$model.bin    the loader alone, for a cable update
  espix-$board.bin           the kernel, as delivered over OTA

The rootfs in the full image holds only the stock applications; a development
tree's test app and local configuration are never packaged. Flashing it replaces
whatever is on the device.

Updating an espix already on the network
----------------------------------------

    sudo upgrade

It reads espix-ota-$board.json from this release.
EOF

trap - ERR

assets="$img $manifest $loader $minimal $full"

if [ "$dry" = 1 ]; then
    printf 'release: dry run; would push %s and publish:\n' "$tag"
    for a in $assets; do printf '  %10s  %s\n' "$(wc -c < "$a")" "$a"; done
    printf 'release: local tag %s is left in place; delete it to retry\n' "$tag"
    exit 0
fi

printf 'release: pushing %s\n' "$tag"
git -C "$root" push origin "$tag"

if gh release view "$tag" --repo "$slug" >/dev/null 2>&1; then
    printf 'release: adding to the existing release\n'
    gh release upload "$tag" --repo "$slug" --clobber $assets
else
    printf 'release: creating the GitHub release\n'
    gh release create "$tag" --repo "$slug" --title "espix $ver" \
        --notes-file "$notes" $assets
fi

printf 'release: %s is up. Devices ask:\n' "$tag"
printf 'release:   https://github.com/%s/releases/latest/download/espix-ota-%s.json\n' \
    "$slug" "$board"
