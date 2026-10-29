# File/layer: tests/cli_signal.sh | test-only, POSIX sh
# Testing: HARNESS (ctest CliSignalTest): SIGINT and SIGTERM end a
#          running havne-cli stream cleanly (ADR-0031 §5)
#
#   sh tests/cli_signal.sh <havne-cli>
#
# For each signal: a mock:// stream runs for a second, gets the signal,
# and must exit 0 with an end line whose reason is "interrupted" as its
# last line, after some frames.

set -u
cli=$1
out=$(mktemp)
trap 'rm -f "$out"' EXIT

for sig in INT TERM; do
    "$cli" stream mock://signal > "$out" &
    pid=$!
    sleep 1
    kill -s "$sig" "$pid"
    wait "$pid"
    code=$?
    last=$(tail -n 1 "$out")
    frames=$(grep -c '"type":"frame"' "$out")
    if [ "$code" -ne 0 ]; then
        echo "FAIL  SIG$sig: exit $code, not 0"
        exit 1
    fi
    case $last in
        *'"type":"end","reason":"interrupted"'*) ;;
        *) echo "FAIL  SIG$sig: the last line isn't an interrupted end: $last"; exit 1 ;;
    esac
    if [ "$frames" -lt 5 ]; then
        echo "FAIL  SIG$sig: only $frames frames in a second"
        exit 1
    fi
    echo "ok    SIG$sig: exit 0 after $frames frames, $last"
done
