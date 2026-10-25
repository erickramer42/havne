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
#               Testing/coverage/cobertura.xml (repo-relative paths, one
#               entry per file; CI uploads it to Codecov). Failing tests
#               are listed but don't stop
#               the report: the build is unoptimized and the tool attaches
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
rm -rf "$LOGDIR/html" "$LOGDIR/raw.xml" "$LOGDIR/cobertura.xml"
run coverage "$tool" --quiet --cover_children \
    --modules "$root\\$TREE" \
    --sources "$root\\core" --sources "$root\\surrogate" --sources "$root\\app" \
    --export_type "html:$root\\Testing\\coverage\\html" \
    --export_type "cobertura:$root\\Testing\\coverage\\raw.xml" \
    -- "$(cygpath -w "$(command -v ctest)")" "${ctest_args[@]}"

raw=$LOGDIR/raw.xml
xml=$LOGDIR/cobertura.xml
if [ ! -f "$raw" ]; then
    line coverage FAIL "$dur" "no report; see $LOGDIR/coverage.log"
    exit 1
fi
# The tool's report has a package per test binary, each listing the
# sources it links, with paths from the drive root. This rewrites it as
# one package of repo-relative files (what Codecov matches against the
# repo), each line's hits summed over the binaries, and prints the rate
# per area: a file's first two path parts (core/src, core/include), or
# its directory for a file one level down (app, surrogate).
prefix=$(cygpath -m "$PWD" | sed 's|^[A-Za-z]:/||')/
summary=$(awk -v prefix="$prefix" -v out="$xml" '
    match($0, /filename="[^"]*"/) {
        f = substr($0, RSTART + 10, RLENGTH - 11); gsub(/\\/, "/", f)
        if (tolower(substr(f, 1, length(prefix))) == tolower(prefix))
            f = substr(f, length(prefix) + 1)
    }
    match($0, /<line number="[0-9]+" hits="[0-9]+"/) {
        split(substr($0, RSTART, RLENGTH), q, "\"")
        hits[f][q[2] + 0] += q[4]
    }
    END {
        PROCINFO["sorted_in"] = "@ind_str_asc"
        for (f in hits) {
            n = split(f, p, "/"); area = (n > 2) ? p[1] "/" p[2] : p[1]
            for (l in hits[f]) {
                valid[f]++; avalid[area]++; tv++
                if (hits[f][l] > 0) { cov[f]++; acov[area]++; tc++ }
            }
        }
        rate = tv ? tc / tv : 0
        print "<?xml version=\"1.0\" encoding=\"utf-8\"?>" > out
        printf "<coverage line-rate=\"%.4f\" branch-rate=\"0\" lines-covered=\"%d\" lines-valid=\"%d\" branches-covered=\"0\" branches-valid=\"0\" complexity=\"0\" timestamp=\"%d\" version=\"0\">\n", rate, tc, tv, systime() > out
        print "  <sources><source>.</source></sources>" > out
        printf "  <packages><package name=\"qttune\" line-rate=\"%.4f\" branch-rate=\"0\" complexity=\"0\"><classes>\n", rate > out
        for (f in hits) {
            n = split(f, p, "/")
            printf "    <class name=\"%s\" filename=\"%s\" line-rate=\"%.4f\" branch-rate=\"0\" complexity=\"0\"><methods/><lines>\n", p[n], f, cov[f] / valid[f] > out
            PROCINFO["sorted_in"] = "@ind_num_asc"
            for (l in hits[f])
                printf "      <line number=\"%d\" hits=\"%d\"/>\n", l, hits[f][l] > out
            PROCINFO["sorted_in"] = "@ind_str_asc"
            print "    </lines></class>" > out
        }
        print "  </classes></package></packages>" > out
        print "</coverage>" > out
        printf "%.1f%% of lines (%d/%d)\n", 100 * rate, tc, tv
        for (a in avalid)
            printf "  %-16s %5.1f%%  %5d/%-5d\n", a, 100 * acov[a] / avalid[a], acov[a], avalid[a]
    }' "$raw")
tests=$(grep -oE '[0-9]+% tests passed.* out of [0-9]+' "$LOGDIR/coverage.log" | tail -n 1)
line coverage ok "$dur" "$(head -n 1 <<<"$summary"); ${tests:-no ctest summary}"
tail -n +2 <<<"$summary"
sed -n '/The following tests FAILED:/,$p' "$LOGDIR/coverage.log" | sed -n '2,25p'
echo "report: $LOGDIR/html/index.html"
