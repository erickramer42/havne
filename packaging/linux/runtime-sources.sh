#!/usr/bin/env bash
# File/layer: packaging/linux/runtime-sources.sh | release packaging, Linux
# Testing: run by .github/workflows/release.yml's publish job, and by hand
#          (docs/development.md, "Linux: the AppImage")
#
# Fetches the source the AppImage's runtime asks to travel with it, each
# checked against its SHA-256: the runtime links libfuse statically, so
# LGPL-2.1 section 6 wants libfuse's source and that of the runtime it is
# linked into (with its libfuse patch and build scripts) offered beside
# the AppImage. Each release attaches both (THIRD_PARTY_NOTICES.txt's
# runtime section names them).
#   <out dir>/fuse-3.15.0.tar.xz             libfuse's own release
#   <out dir>/type2-runtime-<release>.tar.gz  the runtime at its commit
# The runtime's release and commit are make-appimage.sh's, which fails a
# pack when the two differ. The runtime's tarball is GitHub's archive of
# that commit: if GitHub ever packs it differently its SHA-256 changes,
# and this fails rather than attach another file.
#
# Usage: packaging/linux/runtime-sources.sh [out dir]   (default dist)
# Run from the repository root.

set -euo pipefail

die() { echo "runtime-sources: $*" >&2; exit 1; }

[ $# -le 1 ] || die "usage: runtime-sources.sh [out dir]"
out=${1:-dist}

runtime_release=20251108
runtime_full=dd6cebedcbddde9c82f89b011e8e1d40b6e43868
runtime_sum=f5fec23be76e50e2445ed2d018bac49b367490fc483c62c4637e99ec705d27ba
fuse_version=3.15.0
fuse_sum=70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0

mkdir -p "$out"
# Downloads $2 to $1: curl with retries, then, for a GitHub release asset
# URL, the API through gh (the asset URLs redirect to a host that answers
# 504 for long stretches while the API path keeps working). The caller
# checks the SHA-256 either way.
download() {
    local dest=$1 url=$2 owner repo tag name
    curl -fsSL --retry 3 --retry-delay 5 --retry-all-errors -o "$dest" "$url" && return 0
    if command -v gh >/dev/null 2>&1 \
        && [[ $url =~ ^https://github\.com/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$ ]]; then
        owner=${BASH_REMATCH[1]} repo=${BASH_REMATCH[2]} tag=${BASH_REMATCH[3]} name=${BASH_REMATCH[4]}
        echo "$0: curl failed for $url; trying gh release download" >&2
        gh release download "$tag" -R "$owner/$repo" -p "$name" -O "$dest" --clobber && return 0
    fi
    die "couldn't download $url"
}
fetch() {
    local name=$1 url=$2 sum=$3
    download "$out/$name.part" "$url"
    echo "$sum  $out/$name.part" | sha256sum -c --status \
        || { rm -f "$out/$name.part"; die "$name's SHA-256 isn't the pinned one"; }
    mv "$out/$name.part" "$out/$name"
    echo "$name $sum"
}
fetch "fuse-$fuse_version.tar.xz" \
    "https://github.com/libfuse/libfuse/releases/download/fuse-$fuse_version/fuse-$fuse_version.tar.xz" \
    "$fuse_sum"
fetch "type2-runtime-$runtime_release.tar.gz" \
    "https://github.com/AppImage/type2-runtime/archive/$runtime_full.tar.gz" \
    "$runtime_sum"
