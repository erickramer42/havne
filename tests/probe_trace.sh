#!/bin/sh
# File/layer: tests/probe_trace.sh | test-only, POSIX sh, Linux, root
# Testing: HARNESS (CI's linux-build, "USDT probes under bpftrace"): core's
#          probes fire in a running havne-cli (ADR-0034 §4)
#
#   sudo sh tests/probe_trace.sh <havne-cli>
#
# Attaches bpftrace to the three ecuio probes in the binary, streams 100
# mock:// frames through it, and expects one session_start, one
# session_close and at least 100 frames (core may dispatch a few more
# before the close lands). bpftrace prints each map at its exit as
# "@name: N"; the stream's JSON lines never start with "@".
set -eu
cli=$(readlink -f "$1")
out=$(bpftrace -e "
usdt:$cli:ecuio:session_start { @start = count(); }
usdt:$cli:ecuio:session_close { @close = count(); }
usdt:$cli:ecuio:frame { @frames = count(); }
" -c "$cli stream mock://probes --count 100")
count() {
    printf '%s\n' "$out" | sed -n "s/^@$1: //p"
}
start=$(count start)
close=$(count close)
frames=$(count frames)
echo "session_start ${start:-0}, session_close ${close:-0}, frame ${frames:-0}"
if [ "${start:-0}" != 1 ] || [ "${close:-0}" != 1 ] || [ "${frames:-0}" -lt 100 ]; then
    echo "probe_trace: expected 1, 1 and at least 100" >&2
    exit 1
fi
