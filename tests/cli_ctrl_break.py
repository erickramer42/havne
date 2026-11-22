# File/layer: tests/cli_ctrl_break.py | test-only, Python 3.8+, Windows
# Testing: HARNESS (ctest CliSignalTest.CtrlBreakEndsTheStreamCleanly):
#          the console's Ctrl+Break ends a running havne-cli stream
#          cleanly (ADR-0031 section 5), through the handler Ctrl+C and a closed
#          console take too
#
#   python tests/cli_ctrl_break.py <havne-cli> <replay uri>
#
# Each stream, a mock:// one and a replay, starts in a process group of
# its own, so the event reaches it alone (Ctrl+C can't be sent to a
# group, Ctrl+Break can) and gets Ctrl+Break once its fifth frame has been
# read (the handler is installed before the first frame can flow, so the
# frames prove it, whatever the machine's load). It must exit 0 with an
# end line whose reason is "interrupted" as its last line, after some
# frames. Exit 77 (ctest's skip) where this process has no
# console to send the event through.

import signal
import subprocess
import sys
import threading

SKIP = 77
FRAMES_BEFORE_BREAK = 5
START_BOUND_S = 30


class Pipe:
    """A child's pipe read on a thread: its lines, and an event set at each
    frame line and at the end of the stream"""

    def __init__(self, stream):
        self.lines = []
        self.frames = 0
        self.ended = threading.Event()
        self.enough = threading.Event()
        self._thread = threading.Thread(target=self._read, args=(stream,),
                                        daemon=True)
        self._thread.start()

    def _read(self, stream):
        for raw in stream:
            line = raw.decode("utf-8").rstrip()
            self.lines.append(line)
            if '"type":"frame"' in line:
                self.frames += 1
                if self.frames >= FRAMES_BEFORE_BREAK:
                    self.enough.set()
        self.ended.set()
        self.enough.set()

    def join(self):
        self._thread.join(10)


def main():
    cli, replay = sys.argv[1], sys.argv[2]
    for uri in ("mock://ctrlbreak", replay):
        p = subprocess.Popen([cli, "stream", uri], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        out_pipe, err_pipe = Pipe(p.stdout), Pipe(p.stderr)
        name = uri.split(":")[0]
        out_pipe.enough.wait(START_BOUND_S)
        if out_pipe.frames < FRAMES_BEFORE_BREAK:
            p.kill()
            p.wait()
            print(f"FAIL  {name}: no frames before the break "
                  f"({out_pipe.frames} in {START_BOUND_S} s)")
            return 1
        try:
            p.send_signal(signal.CTRL_BREAK_EVENT)
        except OSError as e:
            p.kill()
            p.wait()
            print(f"skip  no console to send Ctrl+Break through: {e}")
            return SKIP
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            print(f"FAIL  {uri}: still running 10 s after Ctrl+Break")
            return 1
        out_pipe.join()
        err_pipe.join()
        lines = out_pipe.lines
        last = lines[-1] if lines else ""
        frames = out_pipe.frames
        if p.returncode != 0:
            print(f"FAIL  {name}: exit {p.returncode}, not 0")
            return 1
        if not last.startswith('{"type":"end","reason":"interrupted"'):
            print(f"FAIL  {name}: the last line isn't an interrupted end: {last}")
            return 1
        if frames < 5:
            print(f"FAIL  {name}: only {frames} frames")
            return 1
        print(f"ok    {name}: exit 0 after {frames} frames, {last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
