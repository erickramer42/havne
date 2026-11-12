#!/usr/bin/env bash
# scripts/verify.sh: build both trees and run the tests, one summary line
# per step. Full output goes to Testing/verify/<step>.log; on a failure
# the relevant lines are printed and the script stops.
#
# Usage: scripts/verify.sh [--tests-only | --build-only] [--no-cross]
#                          [--no-devices] [-R <ctest regex> | --suite <names>]
#        scripts/verify.sh --changed [base] [--no-devices]
#        either with --dry-run: print the plan and run nothing
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
# --suite Bridge,Qml,c_abi runs the named suites: BridgeTest.*, QmlTest.*
# and c_abi, a single test. A name that matches no test fails.
#
# --changed [base] picks the steps from what changed since base (default
# the merge-base with origin/dev), uncommitted and untracked files
# included, by scripts/verify_scope.py's table: docs and markdown only run
# nothing, QML only runs build and QmlTest.*, and anything else, or a
# change it can't read, is the full run.
#
# Anything less than the full run prints what it skipped and ends with a
# "not a full verify" line, and says whether clang-tidy applies. A full
# run that ran every step writes Testing/verify/full-run.stamp (HEAD's
# SHA, whether the tree stayed clean, and the main ctest step's passed,
# skipped and total), which a PR's preflight reads.
#
# The trees must already be configured (docs/development.md).
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/verify
STAMP=$LOGDIR/full-run.stamp
mkdir -p "$LOGDIR"

usage_error() { echo "$1" >&2; exit 2; }

tests_only=0
build_only=0
cross=1
devices=1
regex=""
have_suite=0
suites=""
changed=0
base=""
dry=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tests-only) tests_only=1 ;;
        --build-only) build_only=1 ;;
        --no-cross) cross=0 ;;
        --no-devices) devices=0 ;;
        -R) regex="${2:-}"; shift ;;
        --suite) have_suite=1; suites="${2:-}"; shift ;;
        --changed)
            changed=1
            case "${2:-}" in ""|-*) ;; *) base=$2; shift ;; esac
            ;;
        --dry-run) dry=1 ;;
        -h|--help) sed -n '2,/^set -u$/{/^set -u$/!p}' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
if [ "$build_only" = 1 ] && { [ "$tests_only" = 1 ] || [ -n "$regex" ] || [ "$have_suite" = 1 ]; }; then
    usage_error "--build-only runs no tests: it takes neither --tests-only, -R nor --suite"
fi
if [ -n "$regex" ] && [ "$have_suite" = 1 ]; then
    usage_error "-R and --suite both pick the tests: use one"
fi
if [ "$changed" = 1 ] && { [ "$tests_only" = 1 ] || [ "$build_only" = 1 ] || [ "$cross" = 0 ] \
        || [ -n "$regex" ] || [ "$have_suite" = 1 ]; }; then
    usage_error "--changed picks the steps itself: it takes only a base, --no-devices and --dry-run"
fi

windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
have_win32=0
[ "$windows" = 1 ] && [ -d build-win32 ] && have_win32=1

python_cmd() {
    local p
    for p in python python3; do
        "$p" -I -c '' 2>/dev/null && { echo "$p"; return 0; }
    done
    return 1
}

# Sets plan, plan_reason, tidy and tidy_why for the paths changed since $1
classify() {
    local py tracked untracked out key value
    plan=""
    py=$(python_cmd) || return 1
    # --no-renames lists a moved file's old path too
    tracked=$(git -c core.quotepath=off diff --name-only --no-renames "$1" --) || return 1
    untracked=$(git -c core.quotepath=off ls-files --others --exclude-standard) || return 1
    out=$(printf '%s\n%s\n' "$tracked" "$untracked" | "$py" -I scripts/verify_scope.py classify) || return 1
    while IFS='=' read -r key value; do
        value=${value%$'\r'}
        case "$key" in
            plan) plan=$value ;;
            reason) plan_reason=$value ;;
            tidy) tidy=$value ;;
            tidy_why) tidy_why=$value ;;
        esac
    done <<< "$out"
    [ -n "$plan" ]
}

suite_each=""
if [ "$have_suite" = 1 ]; then
    py=$(python_cmd) || usage_error "--suite needs Python 3"
    out=$("$py" -I scripts/verify_scope.py suite "$suites") || exit 2
    out=$(printf '%s' "$out" | tr -d '\r')
    regex=${out%%$'\n'*}
    suite_each=${out#*$'\n'}
fi

# What runs. scope says why this isn't the full run; empty on the full run
do_win32=1
do_build=1
do_tests=1
do_cross=1
scope=""
plan=""
plan_reason=""
tidy=""
tidy_why=""
add_scope() { scope="${scope:+$scope, }$1"; }

if [ "$changed" = 1 ]; then
    if [ -n "$base" ]; then
        git rev-parse -q --verify "$base^{commit}" > /dev/null \
            || usage_error "--changed: no commit named $base"
    else
        base=$(git merge-base HEAD origin/dev 2> /dev/null)
    fi
    if [ -z "$base" ] || ! classify "$base"; then
        plan=full
        plan_reason="the changed files can't be read (no origin/dev, or no Python)"
    fi
    case "$plan" in
        none)
            do_win32=0; do_build=0; do_tests=0; do_cross=0
            add_scope "$plan_reason"
            ;;
        qml)
            do_win32=0; do_cross=0
            regex='^QmlTest\.'
            add_scope "$plan_reason"
            ;;
    esac
fi
[ "$tests_only" = 1 ] && { do_win32=0; do_build=0; add_scope "--tests-only"; }
[ "$build_only" = 1 ] && { do_tests=0; do_cross=0; add_scope "--build-only"; }
[ "$cross" = 0 ] && { do_cross=0; add_scope "--no-cross"; }
if [ "$have_suite" = 1 ]; then
    do_cross=0
    add_scope "--suite $suites"
elif [ "$changed" = 0 ] && [ -n "$regex" ]; then
    do_cross=0
    add_scope "-R $regex"
fi
[ "$windows" = 1 ] && [ "$devices" = 0 ] && add_scope "--no-devices"

# What a scoped run left out, for its last line
skipped=""
add_skipped() { skipped="${skipped:+$skipped, }$1"; }
[ "$have_win32" = 1 ] && [ "$do_win32" = 0 ] && add_skipped win32
[ "$do_build" = 0 ] && add_skipped build
if [ "$do_tests" = 0 ]; then
    add_skipped "the suite"
elif [ -n "$regex" ]; then
    add_skipped "the suite outside $regex"
fi
[ "$have_win32" = 1 ] && [ "$do_cross" = 0 ] && add_skipped cross-arch
[ "$windows" = 1 ] && [ "$devices" = 0 ] && add_skipped "the J2534 device check"

note() { printf '%-11s %s\n' "$1" "$2"; }

tidy_line() {
    if [ -z "$tidy" ]; then
        local b
        b=$(git merge-base HEAD origin/dev 2> /dev/null)
        [ -n "$b" ] && classify "$b" || { note tidy "unknown: no origin/dev to compare with"; return; }
    fi
    if [ "$tidy" = yes ]; then note tidy "applies: $tidy_why"
    else note tidy "doesn't apply: $tidy_why"; fi
}

if [ "$changed" = 1 ]; then
    if [ "$plan" = full ]; then note scope "full run: $plan_reason"
    else note scope "$plan: $plan_reason"; fi
elif [ -n "$scope" ]; then
    note scope "$scope"
fi

if [ "$dry" = 1 ]; then
    run_or_skip() { if [ "$2" = 1 ]; then note "$1" "run${3:+: $3}"; else note "$1" skip; fi; }
    [ "$have_win32" = 1 ] && run_or_skip win32 "$do_win32"
    run_or_skip build "$do_build"
    run_or_skip ctest "$do_tests" "${regex:-the whole suite}"
    [ "$have_win32" = 1 ] && run_or_skip cross-arch "$do_cross"
    { [ -n "$scope" ] || [ "$changed" = 1 ]; } && tidy_line
    [ -n "$scope" ] && note skipped "${skipped:-nothing}"
    echo "dry run: nothing ran"
    exit 0
fi

# The last word of any scoped run, a failed one included
finish_scoped() {
    tidy_line
    echo "not a full verify: skipped ${skipped:-nothing}; run scripts/verify.sh before a PR"
}
if [ -n "$scope" ]; then
    trap finish_scoped EXIT
elif [ "$changed" = 1 ]; then
    tidy_line
fi

# A failed or partial full run must not leave an earlier run's stamp behind
full=0
if [ -z "$scope" ] && [ "$have_win32" = 1 ]; then
    full=1
    rm -f "$STAMP"
    head_start=$(git rev-parse HEAD)
    dirty_start=$(git status --porcelain)
fi

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

# A ctest log's counts: "<passed> <total> <skipped>"
ctest_counts() {
    local log=$1 total failed skipped
    total=$(grep -oE 'out of [0-9]+' "$log" | tail -n 1 | grep -oE '[0-9]+')
    failed=$(grep -oE '[0-9]+ tests? failed' "$log" | tail -n 1 | grep -oE '^[0-9]+')
    skipped=$(grep -cE '^[[:space:]]+[0-9]+ - .*\(Skipped\)$' "$log")
    total=${total:-0}; failed=${failed:-0}
    echo "$((total - failed)) $total $skipped"
}

# Summarizes a ctest log: "<passed>/<total> passed, <n> skipped".
ctest_summary() {
    local passed total skipped
    read -r passed total skipped <<< "$(ctest_counts "$1")"
    echo "$passed/$total passed, $skipped skipped"
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

if [ "$do_build" = 1 ]; then
    if [ "$do_win32" = 1 ] && [ "$have_win32" = 1 ]; then
        # The x86 shared core and havne-cli (whose ELM327 emulator
        # PythonTest.test_read_pids runs) join the one build call when the
        # cache has the option; an older tree's cache gets it from the
        # first call's configure, so those targets follow in a second
        win32_targets="ecuio-j2534-surrogate fake_j2534 fake_j2534_legacy"
        shared_listed=0
        if grep -q '^HAVNE_SHARED_CORE:BOOL=ON' build-win32/CMakeCache.txt; then
            win32_targets="$win32_targets ecuio test_c_abi_shared havne-cli"
            shared_listed=1
        fi
        # shellcheck disable=SC2086
        run win32 cmake --build build-win32 --config Release --parallel             --target $win32_targets
        [ "$rc" = 0 ] || build_failed win32
        if [ "$shared_listed" = 0 ] && grep -q '^HAVNE_SHARED_CORE:BOOL=ON' build-win32/CMakeCache.txt; then
            win32_dur=$dur
            run win32-shared cmake --build build-win32 --config Release --parallel                 --target ecuio test_c_abi_shared havne-cli
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
[ "$do_tests" = 0 ] && exit 0

if [ "$windows" = 1 ] && [ "$devices" = 1 ]; then
    export ECUIO_EXPECT_J2534_DEVICES=1
fi
jobs=${HAVNE_VERIFY_JOBS:-8}
if [ -n "$suite_each" ]; then
    registered=$(ctest --test-dir build -C Release -N | sed -nE 's/^ *Test +#[0-9]+: //p' | tr -d '\r')
    while IFS=$'\t' read -r name one; do
        grep -qE "$one" <<< "$registered" || usage_error "--suite: no test in build matches $name"
    done <<< "$suite_each"
fi
if [ -n "$regex" ]; then
    run ctest ctest --test-dir build -C Release -j "$jobs" --output-on-failure --no-tests=error -R "$regex"
else
    run ctest ctest --test-dir build -C Release -j "$jobs" --output-on-failure --no-tests=error
fi
[ "$rc" = 0 ] || ctest_failed ctest
line ctest ok "$dur" "$(ctest_summary "$LOGDIR/ctest.log")"

if [ "$do_cross" = 1 ] && [ "$have_win32" = 1 ]; then
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

if [ "$full" = 1 ]; then
    clean=1
    { [ -n "$dirty_start" ] || [ -n "$(git status --porcelain)" ] \
        || [ "$(git rev-parse HEAD)" != "$head_start" ]; } && clean=0
    # The main ctest step's counts, so a preflight can see a tree that
    # registered fewer tests than a release had
    read -r passed total skipped <<< "$(ctest_counts "$LOGDIR/ctest.log")"
    printf 'sha=%s\nclean=%s\ndate=%s\npassed=%s\nskipped=%s\ntotal=%s\n' \
        "$head_start" "$clean" "$(date '+%Y-%m-%d %H:%M:%S %z')" \
        "$passed" "$skipped" "$total" > "$STAMP"
    if [ "$clean" = 1 ]; then note stamp "${head_start:0:7}, tree clean"
    else note stamp "${head_start:0:7}, tree not clean: no use to a PR's preflight"; fi
fi
