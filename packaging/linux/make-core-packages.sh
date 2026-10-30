#!/usr/bin/env bash
# File/layer: packaging/linux/make-core-packages.sh | release packaging, Linux x86_64
# Testing: run by .github/workflows/ci.yml's linux-packages job (twice,
#          under reprotest) and release.yml's linux job; tests/
#          package_check.sh installs and uses what it makes
#
# Builds core and havne-cli from the source tree and packs them
# (ADR-0035 §2–4), into the out dir:
#   havne-core_<version>_amd64.deb                  /usr: havne-cli, the
#       static archive, the public headers, a CMake package, ecuio.pc
#   havne-core-dbgsym_<version>_amd64.deb           havne-cli's debug info
#   havne-core-<version>-linux-x86_64.tar.gz        the same tree, relocatable
#   havne-core-<version>-linux-x86_64-debug.tar.gz  the debug info, to unpack under /
# and beside the deb and the tarball, <file>.components.json, their
# component lists for the release's SBOM (packaging/sbom.py).
# Built with -g, then havne-cli's debug info split off by its GNU build
# ID (/usr/lib/debug/.build-id/xx/rest.debug) and the binary linked to
# it; the archive keeps its own. Reproducible: the same commit and
# toolchain give the same bytes on any machine, in any folder
#   - paths: -ffile-prefix-map maps the source and build folders away
#   - time: SOURCE_DATE_EPOCH (default the commit's time) for every
#     file's time, dpkg-deb's and tar's
#   - order, owners and modes set, never the build machine's
# Checks: the USDT probes are in (ADR-0034 §4), the build ID and debug
# link match, no internal header ships, and no file holds a build path.
#
# Usage: packaging/linux/make-core-packages.sh <version> [build dir] [out dir]
#   build dir  default build-packages, configured afresh each run
#   out dir    default dist
# Environment:
#   SOURCE_DATE_EPOCH  default: git log -1 --format=%ct
# Needs cmake, ninja, gcc, binutils, dpkg-dev and systemtap-sdt-dev.
# Run from the repository root.

set -euo pipefail

die() { echo "make-core-packages: $*" >&2; exit 1; }

[ $# -ge 1 ] && [ $# -le 3 ] || die "usage: make-core-packages.sh <version> [build dir] [out dir]"
version=$1 build=${2:-build-packages} out=${3:-dist}
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+)?$ ]] || die "$version is not X.Y.Z or X.Y.Z-<suffix>"
[ -f core/include/ecuio/core.h ] || die "run from the repository root"
for t in cmake ninja objcopy strip readelf dpkg-deb dpkg-shlibdeps tar gzip strings; do
    command -v "$t" > /dev/null || die "no $t (Ubuntu: sudo apt install cmake ninja-build binutils dpkg-dev)"
done

# 0.3.4-rc1 sorts after 0.3.4 to dpkg; 0.3.4~rc1 sorts before it
debver=${version/-/\~}
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) || die "no SOURCE_DATE_EPOCH and no git history"
fi
export SOURCE_DATE_EPOCH
export LC_ALL=C TZ=UTC
umask 022

src=$(pwd -P)
rm -rf "$build"
mkdir -p "$build" "$out"
build=$(cd "$build" && pwd -P)
out=$(cd "$out" && pwd -P)

# The later map wins where both match: the build folder may sit inside
# the source tree
map="-ffile-prefix-map=$src=. -ffile-prefix-map=$build=build"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DHAVNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=OFF \
    -DCMAKE_C_FLAGS="-g $map" -DCMAKE_CXX_FLAGS="-g $map" > "$build/configure.log" \
    || { cat "$build/configure.log"; die "configure failed"; }
grep -q '^ECUIO_HAVE_SDT:INTERNAL=1$' "$build/CMakeCache.txt" \
    || die "no <sys/sdt.h>: the packages carry the USDT probes (sudo apt install systemtap-sdt-dev)"
cmake --build "$build" > "$build/build.log" || { cat "$build/build.log"; die "build failed"; }

stage=$build/stage
debroot=$stage/havne-core
dbgroot=$stage/havne-core-dbgsym
tarname=havne-core-$version-linux-x86_64
tarroot=$stage/$tarname
cmake --install "$build" --prefix "$debroot/usr" > /dev/null
cmake --install "$build" --prefix "$tarroot" > /dev/null

# havne-cli's debug info, by build ID (ADR-0034 §2)
cli=$build/cli/havne-cli
id=$(readelf -n "$cli" | sed -n 's/^ *Build ID: *\([0-9a-f]*\)$/\1/p')
[ "${#id}" -ge 16 ] || die "havne-cli has no GNU build ID"
dbgrel=usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug
mkdir -p "$dbgroot/$(dirname "$dbgrel")"
objcopy --only-keep-debug --compress-debug-sections "$cli" "$dbgroot/$dbgrel"
stripped=$stage/havne-cli
cp "$cli" "$stripped"
strip --strip-debug --strip-unneeded "$stripped"
objcopy --add-gnu-debuglink="$dbgroot/$dbgrel" "$stripped"
for root in "$debroot/usr" "$tarroot"; do cp "$stripped" "$root/bin/havne-cli"; done

readelf -n "$stripped" | grep -q "Build ID: $id" || die "the stripped havne-cli lost its build ID"
probes=$(readelf -n "$stripped" | grep -c 'Provider: ecuio' || true)
[ "$probes" -ge 3 ] || die "the stripped havne-cli has $probes USDT probes, not 3"
readelf --debug-dump=info "$stripped" 2>/dev/null | grep -q DW_TAG_compile_unit \
    && die "the stripped havne-cli still holds debug info"
readelf -S "$stripped" | grep -q '\.gnu_debuglink' || die "havne-cli has no debug link"

for f in internal.h j2534_ptypes.h j2534_surrogate.h surrogate_protocol.h signal_sets.h; do
    [ ! -e "$debroot/usr/include/ecuio/$f" ] || die "internal header $f is in the package"
done

# Runtime dependencies, from the libraries havne-cli links
mkdir -p "$stage/shlibs/debian"
printf 'Source: havne-core\n\nPackage: havne-core\nArchitecture: amd64\n' > "$stage/shlibs/debian/control"
depends=$(cd "$stage/shlibs" && dpkg-shlibdeps -O -e"$stripped" 2> /dev/null | sed -n 's/^shlibs:Depends=//p')
[ -n "$depends" ] || die "dpkg-shlibdeps found no dependencies for havne-cli"

maintainer="Havne <https://github.com/erickramer42/havne/issues>"
homepage=https://github.com/erickramer42/havne
installed_size() { du -sk --apparent-size "$1" | cut -f1; }

mkdir -p "$debroot/DEBIAN" "$dbgroot/DEBIAN"
cat > "$debroot/DEBIAN/control" <<EOF
Package: havne-core
Version: $debver
Architecture: amd64
Maintainer: $maintainer
Installed-Size: $(installed_size "$debroot/usr")
Depends: $depends
Section: devel
Priority: optional
Homepage: $homepage
Description: Havne's ECU reading core and command-line client
 havne-cli streams a session's frames and values as JSON lines, and
 puts a simulated car on a virtual CAN bus. The static library, its
 public C headers, a CMake package (find_package(ecuio), ecuio::core)
 and a pkg-config file (ecuio) let another program use the same core.
EOF
cat > "$dbgroot/DEBIAN/control" <<EOF
Package: havne-core-dbgsym
Source: havne-core
Version: $debver
Architecture: amd64
Maintainer: $maintainer
Installed-Size: $(installed_size "$dbgroot/usr")
Depends: havne-core (= $debver)
Section: debug
Priority: optional
Auto-Built-Package: debug-symbols
Build-Ids: $id
Description: debug symbols for havne-core
EOF

# Modes, times and order are the tree's own, never the machine's
for root in "$debroot" "$dbgroot" "$tarroot"; do
    find "$root" -type d -exec chmod 0755 {} +
    find "$root" -type f -exec chmod 0644 {} +
    find "$root" -path '*/bin/*' -type f -exec chmod 0755 {} +
    find "$root" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
done

# No file may name the folders it was built in. The debug file's
# sections are compressed, so the unstripped binary stands in for it.
leaks=$(grep -rlF -e "$src" -e "$build" "$cli" "$debroot" "$dbgroot" "$tarroot" || true)
[ -z "$leaks" ] || die "build paths in: $leaks"

deb=$out/havne-core_${version}_amd64.deb
dbgdeb=$out/havne-core-dbgsym_${version}_amd64.deb
tarball=$out/$tarname.tar.gz
dbgtar=$out/$tarname-debug.tar.gz
dpkg-deb --root-owner-group -Zxz --build "$debroot" "$deb" > /dev/null
dpkg-deb --root-owner-group -Zxz --build "$dbgroot" "$dbgdeb" > /dev/null

tarc() {   # <archive> <dir> <member>...
    local archive=$1 dir=$2; shift 2
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
        --format=gnu -C "$dir" -cf - "$@" | gzip -9n > "$archive"
}
tarc "$tarball" "$stage" "$tarname"
tarc "$dbgtar" "$dbgroot" usr

# Component lists for the release's SBOM (packaging/sbom.py): nothing is
# bundled, so each lists the libraries it needs, as Depends names them
deps_json=$(echo "$depends" | tr ',' '\n' | sed -n 's/^ *\([^ ]*\) *(\(.*\)) *$/\1\t\2/p;s/^ *\([^ (]*\) *$/\1\t/p' \
    | while IFS=$'\t' read -r name ver; do
        printf '{"name": "%s", "version": "%s", "purl": "pkg:deb/ubuntu/%s?arch=amd64"}\n' "$name" "$ver" "$name"
      done | paste -sd, -)
[ -n "$deps_json" ] || die "no dependencies parsed from: $depends"
for pair in "$deb INSTALL" "$tarball ARCHIVE"; do
    file=${pair% *} purpose=${pair#* }
    printf '{"download": "%s", "purpose": "%s", "depends": [%s]}\n' \
        "$(basename "$file")" "$purpose" "$deps_json" > "$file.components.json"
done

echo "make-core-packages: $version, build ID $id, $probes USDT probes; Depends: $depends"
(cd "$out" && sha256sum "$(basename "$deb")" "$(basename "$dbgdeb")" \
    "$(basename "$tarball")" "$(basename "$dbgtar")")
