# File/layer: tests/cli_ctrl_break.py | test-only, Python 3.8+, Windows
# Testing: HARNESS (ctest CliSignalTest.CtrlBreakEndsTheStreamCleanly):
#          the console's Ctrl+Break ends a running havne-cli stream
#          cleanly (ADR-0031 §5), through the handler Ctrl+C and a closed
#          console take too
#
#   python tests/cli_ctrl_break.py <havne-cli> <replay uri>
#
# Each stream, a mock:// one and a replay, starts in a process group of
# its own, so the event reaches it alone (Ctrl+C can't be sent to a
# group, Ctrl+Break can), runs a second and gets Ctrl+Break. It must exit
# 0 with an end line whose reason is "interrupted" as its last line,
# after some frames. Exit 77 (ctest's skip) where this process has no
# console to send the event through.

import signal
import subprocess
import sys
import time

SKIP = 77


def main():
    cli, replay = sys.argv[1], sys.argv[2]
    for uri in ("mock://ctrlbreak", replay):
        p = subprocess.Popen([cli, "stream", uri], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        time.sleep(1)
        try:
            p.send_signal(signal.CTRL_BREAK_EVENT)
        except OSError as e:
            p.kill()
            p.communicate()
            print(f"skip  no console to send Ctrl+Break through: {e}")
            return SKIP
        try:
            out, _ = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            print(f"FAIL  {uri}: still running 10 s after Ctrl+Break")
            return 1
        lines = out.decode("utf-8").splitlines()
        last = lines[-1] if lines else ""
        frames = sum('"type":"frame"' in line for line in lines)
        name = uri.split(":")[0]
        if p.returncode != 0:
            print(f"FAIL  {name}: exit {p.returncode}, not 0")
            return 1
        if not last.startswith('{"type":"end","reason":"interrupted"'):
            print(f"FAIL  {name}: the last line isn't an interrupted end: {last}")
            return 1
        if frames < 5:
            print(f"FAIL  {name}: only {frames} frames in a second")
            return 1
        print(f"ok    {name}: exit 0 after {frames} frames, {last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
