#!/usr/bin/env bash
# scripts/tidy.sh: clang-tidy over core/ and surrogate/ with .clang-tidy's
# checks, one summary line; each file's output goes to
# Testing/tidy/<file>.log, and on a warning the warnings are printed and
# the script exits 1. CI's clang-tidy job runs it.
#
# Usage: scripts/tidy.sh [<tree>]     (default build-tidy)
#
# The tree is a Ninja (or Makefile) build with a compile database, which
# the Visual Studio generator doesn't write. On Windows, configure it from
# a Developer prompt (vcvars64), so the compile database names cl.exe:
#
#   cmake -B build-tidy -G Ninja -DHAVNE_BUILD_CORE_ONLY=ON \
#         -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#
# Nothing needs building first. clang-tidy is the pinned version in
# .github/clang-tidy-requirements.txt, found on PATH or named by
# $CLANG_TIDY. Another version finds other things, so the script refuses
# it unless CLANG_TIDY_ANY=1 (trying a new pin). To get the pinned one:
#
#   python -m venv tidyvenv
#   tidyvenv/Scripts/pip install --require-hashes -r .github/clang-tidy-requirements.txt
#   CLANG_TIDY=tidyvenv/Scripts/clang-tidy scripts/tidy.sh   # bin/ off Windows
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/tidy
TREE=${1:-build-tidy}
TIDY=${CLANG_TIDY:-clang-tidy}
DB=$TREE/compile_commands.json

if [ ! -f "$DB" ]; then
    echo "no $DB: configure the tree first (see the top of $0)" >&2
    exit 2
fi
if ! version=$("$TIDY" --version 2>/dev/null | grep -oE 'version [0-9.]+'); then
    echo "no clang-tidy at '$TIDY' (set CLANG_TIDY)" >&2
    exit 2
fi
pin=$(sed -nE 's/^clang-tidy==([0-9.]+).*/\1/p' .github/clang-tidy-requirements.txt)
if [ "$version" != "version $pin" ]; then
    if [ "${CLANG_TIDY_ANY:-0}" != 1 ]; then
        echo "clang-tidy at '$TIDY' is $version; CI pins $pin" \
            "(.github/clang-tidy-requirements.txt): see the top of $0," \
            "or CLANG_TIDY_ANY=1 to run it anyway" >&2
        exit 2
    fi
    echo "warning: clang-tidy $version, not CI's $pin" >&2
fi
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log

root=$(pwd -W 2>/dev/null || pwd)
# C and C++ sources in the source tree only: the database also lists
# generated files, such as the version resource in the tree's own
# surrogate/ folder, which clang-tidy can't read
files=$(grep -oE '"file": *"[^"]*"' "$DB" | sed -E 's/"file": *"(.*)"/\1/' |
    tr '\\' '/' | grep -E "/(core|surrogate)/" | grep -E '\.(c|cc|cpp|cxx)$' |
    grep -vF "/$TREE/" | sort -u)
if [ -z "$files" ]; then
    echo "no core/ or surrogate/ sources in $DB" >&2
    exit 2
fi
count=$(printf '%s\n' "$files" | wc -l | tr -d ' ')

start=$SECONDS
jobs=$(nproc 2>/dev/null || echo 4)
printf '%s\n' "$files" | xargs -P "$jobs" -I{} sh -c \
    '"$1" -p "$2" --quiet "$3" > "$4/$(basename "$3").log" 2>&1' \
    sh "$TIDY" "$TREE" {} "$LOGDIR"
rc=$?
dur=$((SECONDS - start))

# A header's warning repeats in every file that includes it: print once.
found=$(cat "$LOGDIR"/*.log | grep -E '(warning|error): ' | tr '\\' '/' |
    sed "s#^$root/##" | sort -u)
if [ -n "$found" ]; then
    n=$(printf '%s\n' "$found" | wc -l | tr -d ' ')
    what=warnings
    [ "$n" = 1 ] && what=warning
    printf 'tidy        FAIL  %4s s  %s files, %s %s (%s); see %s/\n' \
        "$dur" "$count" "$n" "$what" "$version" "$LOGDIR"
    printf '%s\n' "$found"
    exit 1
fi
if [ "$rc" != 0 ]; then
    # clang-tidy failed without a diagnostic: a crash, or a bad flag.
    printf 'tidy        FAIL  %4s s  clang-tidy exited %s; see %s/\n' \
        "$dur" "$rc" "$LOGDIR"
    exit 1
fi
printf 'tidy        ok    %4s s  %s files, no warnings (%s)\n' \
    "$dur" "$count" "$version"
