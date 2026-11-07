#!/usr/bin/env bash
# scripts/verify.sh: build both trees and run the tests, one summary line
# per step. Full output goes to Testing/verify/<step>.log; on a failure
# the relevant lines are printed and the script stops.
#
# Usage: scripts/verify.sh [--tests-only | --build-only] [--no-cross]
#                          [--no-devices] [-R <ctest regex>]
#
#   win32       build-win32's surrogate and fake DLLs (Windows, when
#               build-win32 exists). First, because the app build copies
#               the Win32 surrogate next to havne.exe.
#   build       the build tree. Both builds pass --parallel (MSBuild
#               builds projects side by side; the root CMakeLists.txt
#               adds /MP for a project's files): a clean build of build/
#               takes about a quarter of the time it does one at a time
#   ctest       the suite, or the -R subset (an -R matching nothing fails),
#               HAVNE_VERIFY_JOBS cases at a time (default 8: the suite
#               took 34 s at 8 against 260 s one at a time on the dev
#               machine; 1 runs them one at a time). On Windows, ECUIO_EXPECT_J2534_DEVICES=1 makes the
#               discovery test demand a device from the registry, since
#               the dev machine has a Pass-Thru driver installed;
#               --no-devices for a machine without one
#   cross-arch  J2534Routing, J2534Surrogate, J2534HealthTest and
#               J2534ConnectTest, against the x86 surrogate and fake
#               (Windows, needs build-win32; skipped with -R). Its line
#               names each case that skipped. Fails if the env-gated
#               routing case skips, since that means the paths were wrong;
#               any other skip is reported, not failed (the surrogate's
#               session-log case skips while another checkout's run holds
#               its fixed log name). Then, where build-win32 has the
#               shared core (HAVNE_SHARED_CORE), its own c_abi_shared and
#               ExportsTest.TheLibraryExportsTheDeclarations: the x86 DLL's
#               export table, undecorated names included, which CI never
#               builds; and PythonTest.*, the havne package against the
#               x86 core, where build-win32 was configured with a 32-bit
#               Python (the main checkout's build-py32-venv) and so
#               registers them; the line adds "x86 core N/M", M being 2
#               plus those.
#
# --build-only runs win32 and build and stops: fresh binaries without the
# suite, for a machine that only runs them (the laptop at the car).
#
# The trees must already be configured (docs/development.md).
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/verify
mkdir -p "$LOGDIR"

tests_only=0
build_only=0
cross=1
devices=1
regex=""
while [ $# -gt 0 ]; do
    case "$1" in
        --tests-only) tests_only=1 ;;
        --build-only) build_only=1 ;;
        --no-cross) cross=0 ;;
        --no-devices) devices=0 ;;
        -R) regex="${2:-}"; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
if [ "$build_only" = 1 ] && { [ "$tests_only" = 1 ] || [ -n "$regex" ]; }; then
    echo "--build-only runs no tests: it takes neither --tests-only nor -R" >&2
    exit 2
fi

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

# Summarizes a ctest log: "<passed>/<total> passed, <n> skipped".
ctest_summary() {
    local log=$1 total failed skipped
    total=$(grep -oE 'out of [0-9]+' "$log" | tail -n 1 | grep -oE '[0-9]+')
    failed=$(grep -oE '[0-9]+ tests? failed' "$log" | tail -n 1 | grep -oE '^[0-9]+')
    skipped=$(grep -cE '^[[:space:]]+[0-9]+ - .*\(Skipped\)$' "$log")
    total=${total:-0}; failed=${failed:-0}
    echo "$((total - failed))/$total passed, $skipped skipped"
}

# The cases a ctest log lists as skipped, one name a line
skipped_cases() {
    sed -nE 's/^[[:space:]]+[0-9]+ - (.*) \(Skipped\)[[:space:]]*$/\1/p' "$1"
}

ctest_failed() {
    line "$1" FAIL "$dur" "$(ctest_summary "$LOGDIR/$1.log"); see $LOGDIR/$1.log"
    sed -n '/The following tests FAILED:/,$p' "$LOGDIR/$1.log" | head -n 25
    grep -E 'No tests were found' "$LOGDIR/$1.log"
    exit 1
}

if [ "$tests_only" = 0 ]; then
    if [ "$have_win32" = 1 ]; then
        run win32 cmake --build build-win32 --config Release --parallel \
            --target ecuio-j2534-surrogate fake_j2534 fake_j2534_legacy
        [ "$rc" = 0 ] || build_failed win32
        # The x86 shared core, once the build above has configured the
        # option (an older tree's cache gets it there), and the x86
        # havne-cli, whose ELM327 emulator PythonTest.test_read_pids runs
        if grep -q '^HAVNE_SHARED_CORE:BOOL=ON' build-win32/CMakeCache.txt; then
            win32_dur=$dur
            run win32-shared cmake --build build-win32 --config Release --parallel \
                --target ecuio test_c_abi_shared havne-cli
            [ "$rc" = 0 ] || build_failed win32-shared
            cat "$LOGDIR/win32-shared.log" >> "$LOGDIR/win32.log"
            dur=$((win32_dur + dur))
        fi
        line win32 ok "$dur" "$(warnings win32) warnings"
    fi
    run build cmake --build build --config Release --parallel
    [ "$rc" = 0 ] || build_failed build
    line build ok "$dur" "$(warnings build) warnings"
fi
[ "$build_only" = 1 ] && exit 0

if [ "$windows" = 1 ] && [ "$devices" = 1 ]; then
    export ECUIO_EXPECT_J2534_DEVICES=1
fi
jobs=${HAVNE_VERIFY_JOBS:-8}
if [ -n "$regex" ]; then
    run ctest ctest --test-dir build -C Release -j "$jobs" --output-on-failure --no-tests=error -R "$regex"
else
    run ctest ctest --test-dir build -C Release -j "$jobs" --output-on-failure --no-tests=error
fi
[ "$rc" = 0 ] || ctest_failed ctest
line ctest ok "$dur" "$(ctest_summary "$LOGDIR/ctest.log")"

if [ "$cross" = 1 ] && [ "$have_win32" = 1 ] && [ -z "$regex" ]; then
    ECUIO_SURROGATE_EXE="$(cygpath -m "$PWD")/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe"
    ECUIO_FAKE_DLL="$(cygpath -m "$PWD")/build-win32/tests/Release/fake_j2534.dll"
    export ECUIO_SURROGATE_EXE ECUIO_FAKE_DLL
    run cross-arch ctest --test-dir build -C Release -j "$jobs" --output-on-failure \
        --no-tests=error -R "J2534Routing|J2534Surrogate|J2534(Health|Connect)Test\."
    [ "$rc" = 0 ] || ctest_failed cross-arch
    summary=$(ctest_summary "$LOGDIR/cross-arch.log")
    skips=$(skipped_cases "$LOGDIR/cross-arch.log" | paste -sd ',' - | sed 's/,/, /g')
    if skipped_cases "$LOGDIR/cross-arch.log" \
        | grep -qxF J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips; then
        line cross-arch FAIL "$dur" "$summary ($skips): the routing case skipped, so the x86 paths are wrong"
        exit 1
    fi
    # The x86 shared core's two checks, and the Python package's tests
    # where a 32-bit Python registered them, in build-win32's own ctest
    x86=""
    if grep -q '^HAVNE_SHARED_CORE:BOOL=ON' build-win32/CMakeCache.txt; then
        cross_dur=$dur
        py32=$(ctest --test-dir build-win32 -C Release -N -R '^PythonTest\.' 2>/dev/null \
            | sed -n 's/^Total Tests: //p' | tr -d '\r')
        want=$((2 + ${py32:-0}))
        run cross-arch-core ctest --test-dir build-win32 -C Release --output-on-failure \
            --no-tests=error -R '^(c_abi_shared|ExportsTest\.TheLibraryExportsTheDeclarations|PythonTest\..*)$'
        dur=$((cross_dur + dur))
        [ "$rc" = 0 ] || ctest_failed cross-arch-core
        x86="; x86 core $(ctest_summary "$LOGDIR/cross-arch-core.log" | cut -d' ' -f1)"
        [ "$x86" = "; x86 core $want/$want" ] || {
            line cross-arch FAIL "$dur" "$summary$x86: the x86 shared core's $want checks didn't all run"
            exit 1
        }
    fi
    line cross-arch ok "$dur" "$summary$x86${skips:+: $skips}"
fi
