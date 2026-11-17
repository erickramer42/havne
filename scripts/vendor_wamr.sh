#!/usr/bin/env bash
# File/layer: scripts/vendor_wamr.sh | Git Bash or Linux, local tooling
# Testing: none (git and diff; --check is run by hand after a pin moves);
#          ctest's VendoredTest checks the folder against the list it writes
#
# third_party/wamr from WAMR's repository at the pinned tag: the files
# Havne compiles and the headers beside them, byte for byte, then the
# patches in third_party/wamr-patches in name order. The folder holds
# nothing of Havne's own, so --check can compare it whole. Each file's
# hash goes into third_party/wamr.sha256 (scripts/vendored_check.py), which
# a test compares with the folder in every build, with no network.
#
#   scripts/vendor_wamr.sh            writes third_party/wamr and the list
#   scripts/vendor_wamr.sh --check    exits 1 when the folder differs from
#                                     the tag with its patches, or the list
#                                     from the folder
#
# Moving the pin: change tag and commit below, run it, build, and update
# the version in third_party/README.md, the notices and the SBOM's
# component (packaging/core-components.json).

set -euo pipefail

url=https://github.com/bytecodealliance/wasm-micro-runtime
tag=WAMR-2.4.5
commit=25bd7eb63e828e4bd242cc9b38d260b4b31c6605

# What is kept: the sources third_party/wamr.cmake lists and the headers
# beside them. Two headers of the AOT side come too (wasm_memory.c
# includes them whatever is built); nothing else of AOT, JIT, WASI, the
# libc shims or the other ports
keep=(
    LICENSE
    core/config.h
    core/version.h
    core/iwasm/include
    core/iwasm/common
    core/iwasm/aot/aot_runtime.h
    core/iwasm/compilation/aot.h
    core/iwasm/interpreter
    core/shared/utils
    core/shared/mem-alloc
    core/shared/platform/include
    core/shared/platform/common/posix
    core/shared/platform/common/memory
    core/shared/platform/windows
    core/shared/platform/linux
    core/shared/platform/darwin
    core/shared/platform/android
)
# Left out of those folders, with every SConscript and .cmake file (other
# build systems' lists): the fast interpreter and the small loader (never
# compiled), assembly (the C form of invokeNative is used), the GC, the
# wasm-c-api layer and the files only WASI needs
drop=(
    core/iwasm/common/gc
    'core/iwasm/common/arch/*.s'
    'core/iwasm/common/arch/*.S'
    'core/iwasm/common/arch/*.asm'
    core/iwasm/common/wasm_application.c
    core/iwasm/common/wasm_c_api.c
    core/iwasm/interpreter/wasm_interp_fast.c
    core/iwasm/interpreter/wasm_mini_loader.c
    core/shared/utils/uncommon
    core/shared/platform/common/posix/posix_file.c
    core/shared/platform/common/posix/posix_clock.c
    core/shared/platform/common/posix/posix_socket.c
    core/shared/platform/windows/win_file.c
    core/shared/platform/windows/win_socket.c
    core/shared/platform/windows/win_clock.c
)

check=0
[ "${1:-}" = "--check" ] && check=1
root=$(cd "$(dirname "$0")/.." && pwd)
python=$(command -v python3 || command -v python) || {
    echo "vendor_wamr.sh: needs Python 3 for the list of hashes" >&2
    exit 1
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# autocrlf off: a Windows Git would otherwise hand over CRLF copies
git -c core.autocrlf=false -c advice.detachedHead=false clone -q --depth 1 \
    --branch "$tag" "$url" "$work/src"
got=$(git -C "$work/src" rev-parse HEAD)
[ "$got" = "$commit" ] || {
    echo "vendor_wamr.sh: $tag is $got, not the pinned $commit" >&2
    exit 1
}

mkdir "$work/out"
(cd "$work/src" && tar -cf - "${keep[@]}") | tar -xf - -C "$work/out"
find "$work/out" -type f \( -name SConscript -o -name '*.cmake' \) -delete
for d in "${drop[@]}"; do
    rm -rf "$work/out"/$d   # unquoted: an entry may hold a wildcard
done
for p in "$root"/third_party/wamr-patches/*.patch; do
    [ -f "$p" ] || continue
    # autocrlf off again: a Windows Git would write the patched file as CRLF
    git -c core.autocrlf=false -C "$work/out" apply --whitespace=nowarn "$p"
done

if [ $check = 1 ]; then
    if ! diff -r "$work/out" "$root/third_party/wamr" > "$work/diff.txt"; then
        head -40 "$work/diff.txt"
        echo "FAIL  third_party/wamr differs from $tag with its patches" >&2
        exit 1
    fi
    echo "ok    third_party/wamr is $tag ($commit) with its patches"
    "$python" "$root/scripts/vendored_check.py" check
    exit $?
fi
rm -rf "$root/third_party/wamr"
mkdir -p "$root/third_party"
mv "$work/out" "$root/third_party/wamr"
echo "ok    third_party/wamr written from $tag ($commit): $(find "$root/third_party/wamr" -type f | wc -l) files"
"$python" "$root/scripts/vendored_check.py" write
