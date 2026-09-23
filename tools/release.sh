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

# A release is cut for whichever target tools/espix has selected: each target
# has its own build directory and sdkconfig, so nothing is shared but the tag.
# Run it once per target to publish several boards under one version.
eval "$(tools/idf.sh --env)"
build="$ESPIX_BUILD"
sdkconfig="$ESPIX_SDKCONFIG"
loader_build="$ESPIX_LOADER_BUILD"
printf 'release: target %s\n' "$ESPIX_TARGET"

# dependencies.lock carries a per-target `target:` field that IDF 6.1 ignores,
# so building one target dirties it for the next. That is not a source change
# and must not block a release -- release-all builds several in a row.
dirty=$(git -C "$root" status --porcelain | grep -v 'dependencies\.lock$' || true)
if [ -n "$dirty" ]; then
    printf 'release: working tree is dirty; commit first -- a release is the tagged commit:\n' >&2
    printf '%s\n' "$dirty" >&2
    exit 1
fi

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

header="$build/esp-idf/espix_kernel/espix_version.h"
board=$(sed -n 's/^#define ESPIX_BOARD "\(.*\)"$/\1/p' "$header" | head -1)
[ -n "$board" ] || die "no ESPIX_BOARD in $header; did the build fail?"
model=$(printf '%s' "$board" | cut -d- -f1)
kmodel=$(sed -n 's/^#define CONFIG_IDF_TARGET "\(.*\)"$/\1/p' "$build/config/sdkconfig.h" | head -1)

# The S31 reserves its first two flash sectors, so its bootloader lands at
# 0x2000 in a merged image too; the image is still written at offset 0.
boot_off=0x0
[ "$kmodel" = esp32s31 ] && boot_off=0x2000
printf 'release: board %s\n' "$board"

# Assets are filed by board and target, so a release can hold several without
# the names colliding. The loader is per target, not per board: it is built
# without PSRAM and finds partitions by label, so one serves every S3 module.
img="$build/espix-$model-ota.bin"
cp "$build/espix.bin" "$img"

loader="$build/espix-loader-$model.bin"
cp "$loader_build/espix_loader.bin" "$loader"

printf 'release: writing the manifest\n'
( cd "$root" && tools/ota-manifest.sh \
    "https://github.com/$slug/releases/download/$tag" "$build" )
single="$build/espix-ota.json"
[ -f "$single" ] || die "no manifest was written"

# One release can hold several boards. Merge every target manifest already in
# the tree, rewriting each URL to this tag, so the published espix-ota.json
# serves every board without a second URL list to keep in sync. A target that
# was released earlier and not rebuilt still contributes its entry.
mkdir -p "$build/release"
manifest="$build/release/espix-ota.json"
"$ESPIX_PYTHON" "$root/tools/ota-merge.py" "$manifest" \
    "https://github.com/$slug/releases/download/$tag" \
    "$root"/build-*/espix-ota.json
[ -f "$manifest" ] || die "could not merge the manifests"

# The rootfs a release carries is built from apps/ into a clean directory, never
# from the local fsroot/: that tree is a development convenience and may hold a
# test app or a personal wifi.conf, neither of which belongs in a release.
factory_root="$build/factory-fsroot"
rm -rf "$factory_root"
mkdir -p "$factory_root/bin"
( cd "$root" && ESPIX_APPS_STAGE="$factory_root/bin" tools/build-apps.sh >/dev/null )
factory_fs="$build/factory-storage.bin"
( cd "$root" && tools/make-fs-image.sh "$factory_root" "$factory_fs" >/dev/null )

# One-file flash images, offsets from the same partition table sdkconfig selects.
# The minimal one omits the rootfs: the kernel formats the partition and grows it
# itself on first boot, but /bin stays empty. The full one carries a small seed
# filesystem (grown on first mount) with the stock apps, and rewrites storage --
# it is the factory image, not the one to flash over a live device.
csv=$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' "$sdkconfig")
off_of() {
    awk -F, -v n="$1" '$1 == n { gsub(/ /, "", $4); print $4 }' "$root/$csv"
}
flashargs=$(head -1 "$build/flash_args")
fm=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-mode \([a-z]*\).*/\1/p')
ff=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-freq \([0-9a-z]*\).*/\1/p')
fs=$(printf '%s' "$flashargs" | sed -n 's/.*--flash-size \([0-9A-Za-z]*\).*/\1/p')

minimal="$build/espix-$model-minimal.bin"
full="$build/espix-$model-full.bin"
merge() {
    out="$1"
    shift
    eval "$(cd "$root" && tools/idf.sh --env)"
    "$ESPIX_PYTHON" -m esptool --chip "$kmodel" merge-bin -o "$out" \
        --flash-mode "$fm" --flash-freq "$ff" --flash-size "$fs" "$@" >/dev/null
}
printf 'release: merging flash images\n'
merge "$minimal" \
    "$boot_off" "$build/bootloader/bootloader.bin" \
    "0x8000" "$build/partition_table/partition-table.bin" \
    "0xf000" "$build/ota_data_initial.bin" \
    "$(off_of ota_0)" "$loader" \
    "$(off_of ota_1)" "$build/espix.bin"
merge "$full" \
    "$boot_off" "$build/bootloader/bootloader.bin" \
    "0x8000" "$build/partition_table/partition-table.bin" \
    "0xf000" "$build/ota_data_initial.bin" \
    "$(off_of ota_0)" "$loader" \
    "$(off_of ota_1)" "$build/espix.bin" \
    "$(off_of storage)" "$factory_fs"

notes="$build/release-notes.md"
# One set of notes for the whole release, generated from the merged manifest, so
# a later run that adds a board regenerates them to cover it (see the release
# edit below).
"$ESPIX_PYTHON" "$root/tools/release-notes.py" "$manifest" "$ver" > "$notes"

trap - ERR

# The loader is in both merged images and is never delivered over OTA, so it is
# not published on its own; `make flash-loader` writes it when it needs a cable update.
assets="$img $manifest $minimal $full"

if [ "$dry" = 1 ]; then
    printf 'release: dry run; would push %s and publish:\n' "$tag"
    for a in $assets; do printf '  %10s  %s\n' "$(wc -c < "$a")" "$a"; done
    printf 'release: notes:\n\n'
    cat "$notes"
    printf '\nrelease: local tag %s is left in place; delete it to retry\n' "$tag"
    exit 0
fi

printf 'release: pushing %s\n' "$tag"
git -C "$root" push origin "$tag"

if gh release view "$tag" --repo "$slug" >/dev/null 2>&1; then
    printf 'release: adding to the existing release\n'
    # Regenerate the body too: a later run's merged manifest may cover boards the
    # first run did not, and upload alone leaves the notes describing one.
    gh release edit "$tag" --repo "$slug" --notes-file "$notes"
    gh release upload "$tag" --repo "$slug" --clobber $assets
else
    printf 'release: creating the GitHub release\n'
    gh release create "$tag" --repo "$slug" --title "espix $ver" \
        --notes-file "$notes" $assets
fi

printf 'release: %s is up. Devices ask:\n' "$tag"
printf 'release:   https://github.com/%s/releases/latest/download/espix-ota.json\n' \
    "$slug"
