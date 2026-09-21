#!/usr/bin/env bash
#
# Cut a release: tag v<version.txt>, build it, and publish the kernel image and
# the OTA manifest to GitHub, where the device's default update URL points.
#
#   make release
#   tools/release.sh --dry-run     # do everything except push and gh
#
# The tag is made *before* the build because espix calls a build a release only
# when the built commit is exactly tagged and clean (espix_kernel's CMakeLists);
# the tag is removed again if the build fails, so a failed release leaves no
# trace. gh does the GitHub side and is already authenticated.
#
# The manifest's url points at the tag's asset, which is what the device
# downloads; the release's own body is generated from the commits.

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

# owner/repo from the push URL, so this is not tied to one checkout.
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

git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null &&
    die "tag $tag already exists"

printf 'release: tagging %s\n' "$tag"
git -C "$root" tag -a "$tag" -m "espix $ver"
trap 'git -C "$root" tag -d "$tag" >/dev/null 2>&1 || true' ERR

# Reconfigure, not merely build: the release flag is decided when CMake runs, so
# a build directory configured before the tag existed would still say "dev".
printf 'release: building the kernel\n'
( cd "$root" && tools/idf.sh reconfigure >/dev/null && tools/idf.sh build )

printf 'release: writing the manifest\n'
( cd "$root" && tools/ota-manifest.sh \
    "https://github.com/$slug/releases/download/$tag" )

trap - ERR

if [ "$dry" = 1 ]; then
    printf 'release: dry run; would push %s and create the GitHub release\n' "$tag"
    printf 'release: local tag %s is left in place; delete it to retry\n' "$tag"
    exit 0
fi

printf 'release: pushing %s\n' "$tag"
git -C "$root" push origin "$tag"

printf 'release: creating the GitHub release\n'
gh release create "$tag" \
    --repo "$slug" \
    --title "espix $ver" \
    --generate-notes \
    "$root/build/espix.bin" \
    "$root/build/espix-ota.json"

printf 'release: %s is up. Devices ask:\n' "$tag"
printf 'release:   https://github.com/%s/releases/latest/download/espix-ota.json\n' "$slug"
