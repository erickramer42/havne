#!/usr/bin/env bash
# File/layer: tests/package_check.sh | packaging check, Linux (CI's linux-packages)
# Testing: HARNESS — this is the test: the core packages as a user gets
#          them (ADR-0035 §6). Needs sudo, gdb, pkg-config and a C compiler.
#
# Installs the deb and its -dbgsym with apt, then:
#   - runs havne-cli from PATH against mock://
#   - builds tests/package/consumer.c through find_package(ecuio) and
#     through pkg-config, and runs each
#   - unpacks the tarball in a fresh folder and builds the consumer
#     against it by CMAKE_PREFIX_PATH (it must be relocatable)
#   - checks gdb reads havne-cli's debug info, found by build ID
# Removes both packages at the end.
#
# Usage: tests/package_check.sh <out dir> <version>   (from the repository root)

set -euo pipefail

die() { echo "package_check: $*" >&2; exit 1; }
[ $# -eq 2 ] || die "usage: package_check.sh <out dir> <version>"
out=$(cd "$1" && pwd) version=$2
deb=$out/havne-core_${version}_amd64.deb
dbgdeb=$out/havne-core-dbgsym_${version}_amd64.deb
tarball=$out/havne-core-$version-linux-x86_64.tar.gz
for f in "$deb" "$dbgdeb" "$tarball"; do [ -f "$f" ] || die "no $f"; done
work=$(mktemp -d)
trap 'sudo apt-get remove -y havne-core havne-core-dbgsym > /dev/null 2>&1 || true; rm -rf "$work"' EXIT

sudo apt-get install -y "$deb" "$dbgdeb" > /dev/null
[ "$(command -v havne-cli)" = /usr/bin/havne-cli ] || die "havne-cli isn't /usr/bin/havne-cli"
lines=$(havne-cli stream mock://package --count 20 | grep -c '"type": *"frame"')
[ "$lines" -eq 20 ] || die "havne-cli streamed $lines frames, not 20"
echo "havne-cli: 20 frames"

cmake -S tests/package -B "$work/cmake" > /dev/null
cmake --build "$work/cmake" > /dev/null
"$work/cmake/consumer" | sed 's/^/find_package: /'

# shellcheck disable=SC2046  # pkg-config's flags are words
cc -std=c11 -o "$work/pc-consumer" tests/package/consumer.c $(pkg-config --cflags --libs ecuio)
"$work/pc-consumer" | sed 's/^/pkg-config: /'

mkdir "$work/elsewhere"
tar -xzf "$tarball" -C "$work/elsewhere"
prefix=$work/elsewhere/havne-core-$version-linux-x86_64
sudo apt-get remove -y havne-core-dbgsym havne-core > /dev/null
cmake -S tests/package -B "$work/tar" -DCMAKE_PREFIX_PATH="$prefix" > /dev/null
grep -q "ecuio_DIR:PATH=$prefix/lib/cmake/ecuio" "$work/tar/CMakeCache.txt" \
    || die "find_package didn't take the tarball's package"
cmake --build "$work/tar" > /dev/null
"$work/tar/consumer" | sed 's/^/tarball: /'
"$prefix/bin/havne-cli" stream mock://package --count 1 > /dev/null || die "the tarball's havne-cli doesn't run"

sudo apt-get install -y "$deb" "$dbgdeb" > /dev/null
# Captured, then matched: grep -q leaving a pipe early would end gdb
# with SIGPIPE, which pipefail reads as a failure. ^main, not ^main$:
# gdb matches a C++ function by its signature, main(int, char**).
# debuginfod off before the file loads (-iex): the debug file must come
# from the -dbgsym package, not a download
found=$(gdb -batch -iex 'set debuginfod enabled off' -ex 'info functions ^main' \
    /usr/bin/havne-cli 2>&1 || true)
grep -q 'File .*cli/main\.cpp:' <<< "$found" \
    || { echo "$found"; die "gdb found no debug info for havne-cli"; }
echo "gdb: havne-cli's debug info found by build ID"
