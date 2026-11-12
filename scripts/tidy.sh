#!/usr/bin/env bash
# scripts/tidy.sh: clang-tidy over core/, surrogate/ and cli/, and app/ in a
# tree with Qt, with .clang-tidy's checks, one summary line; each file's
# output goes to Testing/tidy/<file>.log, and on a warning the warnings
# are printed and the script exits 1. CI's clang-tidy job runs it.
#
# Usage: scripts/tidy.sh [<tree>]               (default build-tidy)
#        scripts/tidy.sh --qt <kit> [<tree>]    (default build-tidy-qt)
#        HAVNE_TIDY_SHARD=<k>/<n> scripts/tidy.sh ...   (every n-th file from the k-th)
#
# The tree is a Ninja (or Makefile) build with a compile database, which
# the Visual Studio generator doesn't write, configured where cl.exe is
# the compiler. On Windows outside a Developer prompt, the script enters
# one itself (vswhere's newest VC tools, vcvars64), so the configure and
# clang-tidy see one MSVC's headers; and it configures a missing tree:
#
#   cmake -B build-tidy -G Ninja -DHAVNE_BUILD_CORE_ONLY=ON \
#         -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#
# With --qt, the tree is the whole project against that Qt kit (for
# example C:/Qt/6.12.0/msvc2022_64), so app/ is checked too, and the
# script generates the app's moc output (the *_autogen targets) before
# clang-tidy reads the sources; nothing else is built. The app's
# Android-only sources (btstream.cpp, androidsession.cpp) aren't in a
# desktop tree's database, so they aren't checked.
#
# Otherwise nothing needs building first. clang-tidy is the pinned version in
# .github/clang-tidy-requirements.txt: $CLANG_TIDY, else the venv below,
# else PATH's. Another version finds other things, so the script refuses
# it unless CLANG_TIDY_ANY=1 (trying a new pin). To get the pinned one
# (build-tidy-venv/ is git-ignored with the trees):
#
#   python -m venv build-tidy-venv
#   build-tidy-venv/Scripts/pip install --require-hashes -r .github/clang-tidy-requirements.txt
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/tidy
QT_KIT=
if [ "${1:-}" = --qt ]; then
    QT_KIT=${2:?"--qt takes the Qt kit's folder"}
    shift 2
    TREE=${1:-build-tidy-qt}
else
    TREE=${1:-build-tidy}
fi
DB=$TREE/compile_commands.json

# A Developer environment on Windows: vcvars64's variables, read through
# a cmd of its own and taken into this shell (PATH converted)
if [ -z "${VCINSTALLDIR:-}" ] && command -v cmd.exe > /dev/null 2>&1; then
    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    vcvars=$("$vswhere" -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -find 'VC\Auxiliary\Build\vcvars64.bat' 2> /dev/null | tr -d '\r' | head -n 1)
    if [ -z "$vcvars" ]; then
        echo "no Developer environment, and vswhere finds no VC tools: run from a Developer prompt" >&2
        exit 2
    fi
    bat=$(mktemp --suffix=.cmd)
    # vcvars64 runs vswhere through PATH, which Git Bash's doesn't hold
    printf '@set "PATH=%%PATH%%;%s"\r\n@call "%s" > nul\r\n@set\r\n' \
        "$(cygpath -w "$(dirname "$vswhere")")" "$vcvars" > "$bat"
    env_out=$(cmd.exe //d //c "$(cygpath -w "$bat")" | tr -d '\r')
    rm -f "$bat"
    while IFS='=' read -r name value; do
        case "$name" in
            PATH|Path) PATH="$(cygpath -u -p "$value"):$PATH"; export PATH ;;
            INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VCToolsInstallDir|VCToolsVersion|WindowsSdkDir|WindowsSDKVersion|UCRTVersion|UniversalCRTSdkDir|VSINSTALLDIR|VSCMD_ARG_TGT_ARCH|Platform)
                export "$name=$value" ;;
        esac
    done <<< "$env_out"
    [ -n "${VCINSTALLDIR:-}" ] || { echo "vcvars64 ($vcvars) set no VCINSTALLDIR" >&2; exit 2; }
fi

if [ -n "$QT_KIT" ] && [ ! -f "$DB" ]; then
    echo "...   configuring $TREE (Ninja, Qt at $QT_KIT, a compile database)" >&2
    cmake -B "$TREE" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DCMAKE_PREFIX_PATH="$QT_KIT" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null || {
        echo "configuring $TREE failed: see the top of $0" >&2; exit 2; }
elif [ ! -f "$DB" ]; then
    echo "...   configuring $TREE (Ninja, core only, a compile database)" >&2
    cmake -B "$TREE" -G Ninja -DHAVNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=OFF \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null || {
        echo "configuring $TREE failed: see the top of $0" >&2; exit 2; }
fi
# The app's sources include their moc output, which only a build writes
if [ -n "$QT_KIT" ]; then
    autogen=$(ninja -C "$TREE" -t targets all | grep -oE '^[A-Za-z0-9_]+_autogen:' |
        tr -d ':' | sort -u)
    if [ -z "$autogen" ]; then
        echo "no *_autogen targets in $TREE: was it configured with Qt?" >&2
        exit 2
    fi
    # shellcheck disable=SC2086
    cmake --build "$TREE" --target $autogen > /dev/null || {
        echo "generating the moc output in $TREE failed" >&2; exit 2; }
fi

if [ -n "${CLANG_TIDY:-}" ]; then TIDY=$CLANG_TIDY
elif [ -x build-tidy-venv/Scripts/clang-tidy.exe ]; then TIDY=build-tidy-venv/Scripts/clang-tidy.exe
elif [ -x build-tidy-venv/bin/clang-tidy ]; then TIDY=build-tidy-venv/bin/clang-tidy
else TIDY=clang-tidy
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
# C and C++ sources in the source tree only (app/ in a tree with Qt): the database also lists
# generated files, such as the version resource in the tree's own
# surrogate/ folder, which clang-tidy can't read
files=$(grep -oE '"file": *"[^"]*"' "$DB" | sed -E 's/"file": *"(.*)"/\1/' |
    tr '\\' '/' | grep -E "/(core|surrogate|cli|app)/" | grep -E '\.(c|cc|cpp|cxx)$' |
    grep -vF "/$TREE/" | sort -u)
if [ -z "$files" ]; then
    echo "no core/, surrogate/, cli/ or app/ sources in $DB" >&2
    exit 2
fi
# HAVNE_TIDY_SHARD=<k>/<n> checks every n-th file from the k-th (k from 0),
# so n runners together check the whole tree
if [ -n "${HAVNE_TIDY_SHARD:-}" ]; then
    shard_k=${HAVNE_TIDY_SHARD%/*}
    shard_n=${HAVNE_TIDY_SHARD#*/}
    case "$shard_k$shard_n" in *[!0-9]*|"") shard_k=x ;; esac
    if [ "$shard_k" = x ] || [ "$shard_n" -lt 1 ] || [ "$shard_k" -ge "$shard_n" ]; then
        echo "HAVNE_TIDY_SHARD='$HAVNE_TIDY_SHARD': want <k>/<n>, k below n" >&2
        exit 2
    fi
    files=$(printf '%s
' "$files" | awk -v k="$shard_k" -v n="$shard_n" '(NR - 1) % n == k')
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
