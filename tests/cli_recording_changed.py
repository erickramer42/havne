# File/layer: tests/cli_recording_changed.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliReplayTest.ACutRecordingEndsTheStream):
#          a replay whose recording is cut short under it stops early
#          (ECUIO_ERR_RECORDING_CHANGED), and havne-cli stream ends with
#          it rather than waiting for a last frame that never comes
#          (ADR-0031 §5)
#
#   python tests/cli_recording_changed.py <havne-cli>
#
# A scratch recording of 2000 rows a millisecond apart plays at speed 1;
# half a second in, the file is cut to its first 100 rows, behind the
# worker. The stream must exit 0 within 10 s with a health line of code
# 16 and, last, an end line whose reason is "recording-changed", short of
# the 2000 frames. Frames the worker sent before it stopped may follow the
# health line.

import os
import subprocess
import sys
import tempfile
import time

ROWS = 2000
KEPT = 100
HEADER = "host_us,timestamp_us,id,flags,data\n"


def row(i):
    t = 5000000 + 1000 * i
    return f"{t},{t},0x7E8,0x0,41 0C 1F 40\n"


def main():
    cli = sys.argv[1]
    folder = tempfile.mkdtemp(prefix="havne-cli-cut-")
    csv = os.path.join(folder, "frames.csv")
    try:
        with open(csv, "w", newline="\n") as f:
            f.write(HEADER + "".join(row(i) for i in range(ROWS)))
        kept = len(HEADER) + sum(len(row(i)) for i in range(KEPT))
        uri = "replay://" + folder.replace("\\", "/")
        p = subprocess.Popen([cli, "stream", uri], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        time.sleep(0.5)
        with open(csv, "r+b") as f:
            f.truncate(kept)
        try:
            out, err = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            print("FAIL  still running 10 s after the recording was cut")
            return 1
    finally:
        try:
            os.remove(csv)
            os.rmdir(folder)
        except OSError:
            pass

    lines = out.decode("utf-8").splitlines()
    last = lines[-1] if lines else ""
    frames = sum('"type":"frame"' in line for line in lines)
    health = [i for i, line in enumerate(lines)
              if line.startswith('{"type":"health","code":16,')]
    if p.returncode != 0:
        print(f"FAIL  exit {p.returncode}, not 0: {err.decode('utf-8')}")
        return 1
    if not last.startswith('{"type":"end","reason":"recording-changed"'):
        print(f"FAIL  the last line isn't a recording-changed end: {last}")
        return 1
    if not health:
        print("FAIL  no health line of code 16 before the end line")
        return 1
    if frames >= ROWS:
        print(f"FAIL  all {frames} frames played: the cut came too late")
        return 1
    print(f"ok    exit 0 after {frames} of {ROWS} frames, {last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
