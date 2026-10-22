#!/usr/bin/env bash
# scripts/verify.sh: build both trees and run the tests, one summary line
# per step. Full output goes to Testing/verify/<step>.log; on a failure
# the relevant lines are printed and the script stops.
#
# Usage: scripts/verify.sh [--tests-only] [--no-cross] [-R <ctest regex>]
#
#   win32       build-win32's surrogate and fake DLLs (Windows, when
#               build-win32 exists). First, because the app build copies
#               the Win32 surrogate next to qttune.exe.
#   build       the build tree
#   ctest       the suite, or the -R subset (an -R matching nothing fails)
#   cross-arch  J2534Routing|J2534Surrogate against the x86 surrogate and
#               fake (Windows, needs build-win32; skipped with -R). Fails
#               if the env-gated routing case skips, since that means the
#               paths were wrong.
#
# The trees must already be configured (docs/development.md).
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/verify
mkdir -p "$LOGDIR"

tests_only=0
cross=1
regex=""
while [ $# -gt 0 ]; do
    case "$1" in
        --tests-only) tests_only=1 ;;
        --no-cross) cross=0 ;;
        -R) regex="${2:-}"; shift ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
have_win32=0
[ "$windows" = 1 ] && [ -d build-win32 ] && have_win32=1

line() { printf '%-11s %-5s %4s s  %s\n' "$1" "$2" "$3" "$4"; }

# Runs "$@" into $LOGDIR/$name.log; sets rc and dur.
run() {
    local name=$1; shift
    local start=$SECONDS
    "$@" >"$LOGDIR/$name.log" 2>&1
    rc=$?
    dur=$((SECONDS - start))
}

build_failed() {
    line "$1" FAIL "$dur" "see $LOGDIR/$1.log"
    grep -E 'error [A-Z]+[0-9]+|fatal error|error:|CMake Error' "$LOGDIR/$1.log" \
        | sort -u | head -n 20
    exit 1
}

warnings() {
    grep -E 'warning [A-Z]+[0-9]+|warning:' "$LOGDIR/$1.log" | sort -u | wc -l | tr -d ' '
}

# Summarises a ctest log: "<passed>/<total> passed, <n> skipped".
ctest_summary() {
    local log=$1 total failed skipped
    total=$(grep -oE 'out of [0-9]+' "$log" | tail -n 1 | grep -oE '[0-9]+')
    failed=$(grep -oE '[0-9]+ tests? failed' "$log" | tail -n 1 | grep -oE '^[0-9]+')
    skipped=$(grep -cE '^[[:space:]]+[0-9]+ - .*\(Skipped\)$' "$log")
    total=${total:-0}; failed=${failed:-0}
    echo "$((total - failed))/$total passed, $skipped skipped"
}

ctest_failed() {
    line "$1" FAIL "$dur" "$(ctest_summary "$LOGDIR/$1.log"); see $LOGDIR/$1.log"
    sed -n '/The following tests FAILED:/,$p' "$LOGDIR/$1.log" | head -n 25
    grep -E 'No tests were found' "$LOGDIR/$1.log"
    exit 1
}

if [ "$tests_only" = 0 ]; then
    if [ "$have_win32" = 1 ]; then
        run win32 cmake --build build-win32 --config Release \
            --target qttune-j2534-surrogate fake_j2534 fake_j2534_legacy
        [ "$rc" = 0 ] || build_failed win32
        line win32 ok "$dur" "$(warnings win32) warnings"
    fi
    run build cmake --build build --config Release
    [ "$rc" = 0 ] || build_failed build
    line build ok "$dur" "$(warnings build) warnings"
fi

if [ -n "$regex" ]; then
    run ctest ctest --test-dir build -C Release --output-on-failure --no-tests=error -R "$regex"
else
    run ctest ctest --test-dir build -C Release --output-on-failure --no-tests=error
fi
[ "$rc" = 0 ] || ctest_failed ctest
line ctest ok "$dur" "$(ctest_summary "$LOGDIR/ctest.log")"

if [ "$cross" = 1 ] && [ "$have_win32" = 1 ] && [ -z "$regex" ]; then
    QTTUNE_SURROGATE_EXE="$(cygpath -m "$PWD")/build-win32/surrogate/Release/qttune-j2534-surrogate.exe"
    QTTUNE_FAKE_DLL="$(cygpath -m "$PWD")/build-win32/tests/Release/fake_j2534.dll"
    export QTTUNE_SURROGATE_EXE QTTUNE_FAKE_DLL
    run cross-arch ctest --test-dir build -C Release --output-on-failure \
        --no-tests=error -R "J2534Routing|J2534Surrogate"
    [ "$rc" = 0 ] || ctest_failed cross-arch
    summary=$(ctest_summary "$LOGDIR/cross-arch.log")
    case "$summary" in
        *", 0 skipped") line cross-arch ok "$dur" "$summary" ;;
        *) line cross-arch FAIL "$dur" "$summary: the routing case skipped, so the x86 paths are wrong"
           exit 1 ;;
    esac
fi
