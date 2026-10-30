#!/usr/bin/env bash
# File/layer: scripts/abi-check.sh | Linux, bash (CI's abi job; WSL)
# Testing: run by .github/workflows/ci.yml's abi job on every push, and
#          by hand in WSL (docs/development.md, "The C ABI check")
#
# Holds the C ABI to its promises (ADR-0001): the core at HEAD against
# a release's, with libabigail. Each tree's core-only build (debug info,
# position independent) has its static archive linked whole into a
# shared object that exports only the ecuio_ functions, as an FFI
# consumer would load it; abidw dumps the functions and the types the
# public headers (core/include) declare, and abidiff compares the two.
#   - An incompatible change fails: a function removed or its signature
#     changed, a struct field moved or retyped, a struct's size changed,
#     an enum value renumbered.
#   - An addition passes and is reported: a new function, an enum value
#     appended, a field taking a reserved one's place at the same size.
# A deliberate break (before 1.0 a minor version may make one,
# CHANGELOG.md) goes in abi/suppressions.abignore with its reason, until
# the next release becomes the base.
#
# Usage: scripts/abi-check.sh [base]
#   base  a tag or commit (default: the newest vX.Y.Z tag, rc tags
#         left out), or a directory holding that tree's sources
# Output in build-abi/: base.abi, head.abi, report.txt. Exit 0 on no
# change or additions only, 1 on an incompatible change, 2 on an error.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build-abi
SUPPRESSIONS=$ROOT/abi/suppressions.abignore

die() { echo "abi-check: $*" >&2; exit 2; }
for t in abidw abidiff cmake ninja; do
    command -v "$t" > /dev/null || die "no $t (Ubuntu: sudo apt install abigail-tools cmake ninja-build)"
done

base=${1:-}
mkdir -p "$OUT"
if [ -n "$base" ] && [ -d "$base" ]; then
    base_src=$(cd "$base" && pwd)
    base_name=$(basename "$base_src")
else
    if [ -z "$base" ]; then
        base=$(git -C "$ROOT" describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' --exclude '*-rc*' 2>/dev/null) \
            || die "no vX.Y.Z tag to compare with (fetch the tags: git fetch --tags)"
    fi
    git -C "$ROOT" rev-parse --verify --quiet "$base^{commit}" > /dev/null || die "no commit $base"
    base_src=$OUT/base-src
    rm -rf "$base_src" && mkdir -p "$base_src"
    git -C "$ROOT" archive "$base" | tar -x -C "$base_src"
    base_name=$base
fi

# Only the C ABI is exported, as a shared core would export it
cat > "$OUT/exports.map" <<'EOF'
{ global: ecuio_*; local: *; };
EOF

# <source dir> <name>: the core-only tree, its .so and its ABI dump
dump() {
    local src=$1 name=$2 tree=$OUT/$2
    # A base before v0.3.3 knows the option only by its name before the
    # rename, QTTUNE_BUILD_CORE_ONLY; a newer tree ignores it
    cmake -S "$src" -B "$tree" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TESTING=OFF \
        -DHAVNE_BUILD_CORE_ONLY=ON -DQTTUNE_BUILD_CORE_ONLY=ON \
        --no-warn-unused-cli > "$tree.configure.log" 2>&1 \
        || { tail -n 20 "$tree.configure.log"; die "$name: configure failed"; }
    cmake --build "$tree" --target ecuio-core > "$tree.build.log" 2>&1 \
        || { tail -n 20 "$tree.build.log"; die "$name: build failed"; }
    local lib
    lib=$(find "$tree" -name 'libecuio-core.a' | head -n 1)
    [ -n "$lib" ] || die "$name: no libecuio-core.a"
    "${CXX:-c++}" -shared -o "$tree/libecuio-core.so" \
        -Wl,--whole-archive "$lib" -Wl,--no-whole-archive \
        -Wl,--version-script="$OUT/exports.map" -pthread
    abidw --headers-dir "$src/core/include" --drop-private-types --no-show-locs \
        --out-file "$OUT/$name.abi" "$tree/libecuio-core.so"
    # The exported functions: the dump's declarations also hold C++
    # library ones, as many as the libabigail version keeps
    echo "dumped $name: $(grep -c "<elf-symbol name='ecuio_[^']*' type='func-type'.*is-defined='yes'" "$OUT/$name.abi") exported functions"
}

dump "$base_src" base
dump "$ROOT" head

# Harmless changes (an enum value appended, among them) are left out of
# the report, as abidiff does by default
args=(--no-show-locs)
[ -f "$SUPPRESSIONS" ] && args+=(--suppressions "$SUPPRESSIONS")
set +e
abidiff "${args[@]}" "$OUT/base.abi" "$OUT/head.abi" > "$OUT/report.txt" 2>&1
status=$?
set -e
# abidiff's exit status is a bit field: 1 error, 2 usage, 4 a change,
# 8 an incompatible change, which it sets for a removed symbol only. A
# struct's layout or an enum value changed is a "Changed" function or
# variable in the summary, so the summary decides: anything removed or
# changed fails, additions pass
if (( status & 3 )); then
    cat "$OUT/report.txt"
    die "abidiff failed ($status)"
fi
broken=$(grep -E 'changes summary:' "$OUT/report.txt" \
    | grep -oE '[1-9][0-9]* (Removed|Changed)' || true)
# A changed function can be a struct growing into its reserved room,
# which the ABI allows; abi_report.py tells that from a break
if [ -n "$broken" ] && ! (( status & 8 )) && python3 "$ROOT/scripts/abi_report.py" "$OUT/report.txt" > "$OUT/growth.txt"; then
    cat "$OUT/report.txt" "$OUT/growth.txt"
    echo "ok    the C ABI grew since $base_name, into reserved room"
    exit 0
fi
if (( status & 8 )) || [ -n "$broken" ]; then
    [ -s "$OUT/growth.txt" ] && cat "$OUT/growth.txt"
    cat "$OUT/report.txt"
    echo "FAIL  an incompatible C ABI change since $base_name (report: build-abi/report.txt)"
    exit 1
fi
if (( status & 4 )); then
    cat "$OUT/report.txt"
    echo "ok    the C ABI grew since $base_name, compatibly"
else
    echo "ok    the C ABI is unchanged since $base_name"
fi
