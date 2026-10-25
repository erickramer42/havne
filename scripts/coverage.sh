#!/usr/bin/env bash
# scripts/coverage.sh: line coverage of the first-party C++ (core/,
# surrogate/, app/) over the whole ctest suite, Windows only. One summary
# line per step, as in verify.sh; logs and reports go to Testing/coverage/.
#
# Usage: scripts/coverage.sh [--tests-only] [--no-devices] [-R <ctest regex>]
#
#   configure   build-coverage, once, if it doesn't exist: the app and the
#               tests, RelWithDebInfo with /Od /Ob0, so there are PDBs and
#               no inlining to hide lines. Release Qt, so no debug kit
#   build       build-coverage, RelWithDebInfo
#   coverage    ctest under OpenCppCoverage with --cover_children, so every
#               test process and the x64 surrogate child it spawns count.
#               Reports: Testing/coverage/html/index.html and
#               Testing/coverage/cobertura.xml (the format Codecov and
#               Coveralls take). Failing tests are listed but don't stop
#               the report: the build is unoptimised and the tool attaches
#               to every process, so a timing case can miss its bounds
#
# Not counted: QML/JS (OpenCppCoverage sees only native code), the x86
# surrogate (the cross-arch run isn't covered), the tests and the fake
# DLLs. OPENCPPCOVERAGE names the tool if it isn't on PATH or in
# Program Files; QT_PREFIX the Qt kit (default C:/Qt/6.12.0/msvc2022_64).
set -u
cd "$(dirname "$0")/.." || exit 2
LOGDIR=Testing/coverage
TREE=build-coverage
mkdir -p "$LOGDIR"

tests_only=0
devices=1
regex=""
while [ $# -gt 0 ]; do
    case "$1" in
        --tests-only) tests_only=1 ;;
        --no-devices) devices=0 ;;
        -R) regex="${2:-}"; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "coverage.sh runs on Windows only (OpenCppCoverage)" >&2; exit 2 ;;
esac

tool=${OPENCPPCOVERAGE:-$(command -v OpenCppCoverage || true)}
[ -n "$tool" ] || tool="/c/Program Files/OpenCppCoverage/OpenCppCoverage.exe"
if [ ! -x "$tool" ]; then
    echo "OpenCppCoverage not found: winget install OpenCppCoverage.OpenCppCoverage, or set OPENCPPCOVERAGE" >&2
    exit 2
fi

line() { printf '%-11s %-5s %4s s  %s\n' "$1" "$2" "$3" "$4"; }

# Runs "$@" into $LOGDIR/$name.log; sets rc and dur. MSYS path conversion
# is off: it rewrites /Od into a path and mangles html:<dir>.
run() {
    local name=$1; shift
    local start=$SECONDS
    MSYS_NO_PATHCONV=1 "$@" >"$LOGDIR/$name.log" 2>&1
    rc=$?
    dur=$((SECONDS - start))
}

build_failed() {
    line "$1" FAIL "$dur" "see $LOGDIR/$1.log"
    grep -E 'error [A-Z]+[0-9]+|fatal error|error:|CMake Error' "$LOGDIR/$1.log" \
        | sort -u | head -n 20
    exit 1
}

if [ "$tests_only" = 0 ]; then
    if [ ! -f "$TREE/CMakeCache.txt" ]; then
        run configure cmake -B "$TREE" \
            -DCMAKE_PREFIX_PATH="${QT_PREFIX:-C:/Qt/6.12.0/msvc2022_64}" \
            "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/Zi /Od /Ob0 /DNDEBUG" \
            "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/Zi /Od /Ob0 /DNDEBUG"
        [ "$rc" = 0 ] || build_failed configure
        line configure ok "$dur" "$TREE"
    fi
    run build cmake --build "$TREE" --config RelWithDebInfo --parallel
    [ "$rc" = 0 ] || build_failed build
    line build ok "$dur" ""
fi

if [ "$devices" = 1 ]; then
    export ECUIO_EXPECT_J2534_DEVICES=1
fi
root=$(cygpath -w "$PWD")
ctest_args=(--test-dir "$TREE" -C RelWithDebInfo --output-on-failure --no-tests=error)
[ -n "$regex" ] && ctest_args+=(-R "$regex")
rm -rf "$LOGDIR/html"
run coverage "$tool" --quiet --cover_children \
    --modules "$root\\$TREE" \
    --sources "$root\\core" --sources "$root\\surrogate" --sources "$root\\app" \
    --export_type "html:$root\\Testing\\coverage\\html" \
    --export_type "cobertura:$root\\Testing\\coverage\\cobertura.xml" \
    -- "$(cygpath -w "$(command -v ctest)")" "${ctest_args[@]}"

xml=$LOGDIR/cobertura.xml
if [ ! -f "$xml" ]; then
    line coverage FAIL "$dur" "no report; see $LOGDIR/coverage.log"
    exit 1
fi
# The report has a package per test binary, each listing the sources it
# links, so a line counts as covered when any binary hit it. An area is
# a file's first two path parts under the repo (core/src, core/include),
# or its directory for a file one level down (app, surrogate).
summary=$(awk '
    match($0, /filename="[^"]*"/) {
        f = substr($0, RSTART + 10, RLENGTH - 11); gsub(/\\/, "/", f)
        sub(/^.*qttune\//, "", f)
        n = split(f, p, "/"); area = (n > 2) ? p[1] "/" p[2] : p[1]
    }
    match($0, /<line number="[0-9]+" hits="[0-9]+"/) {
        split(substr($0, RSTART, RLENGTH), q, "\"")
        key = f ":" q[2]
        if (!(key in seen)) { seen[key] = 1; valid[area]++; tv++ }
        if (q[4] > 0 && !(key in hit)) { hit[key] = 1; cov[area]++; tc++ }
    }
    END {
        printf "%.1f%% of lines (%d/%d)\n", tv ? 100 * tc / tv : 0, tc, tv
        for (a in valid)
            printf "  %-16s %5.1f%%  %5d/%-5d\n", a, 100 * cov[a] / valid[a], cov[a], valid[a] | "sort"
    }' "$xml")
tests=$(grep -oE '[0-9]+% tests passed.* out of [0-9]+' "$LOGDIR/coverage.log" | tail -n 1)
line coverage ok "$dur" "$(head -n 1 <<<"$summary"); ${tests:-no ctest summary}"
tail -n +2 <<<"$summary"
sed -n '/The following tests FAILED:/,$p' "$LOGDIR/coverage.log" | sed -n '2,25p'
echo "report: $LOGDIR/html/index.html"
