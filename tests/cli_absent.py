# File/layer: tests/cli_absent.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliReplayTest.AnAbsentFieldIsNamed): havne-cli
#          stream --values writes an absent line naming the signals the
#          session's frames come without (ecuio_session_get_absent_signals,
#          ADR-0039 §6)
#
#   python tests/cli_absent.py <havne-cli>
#
# A scratch recording of the test car's PID 55 replies, bank 1's byte
# alone (docs/bench-sessions/2026-10-31-elm327-vehicle.md), replays under
# obd2 at full speed. The stream must exit 0 with one absent line naming
# bank 3's field (11000 + 0x55 * 8 + 1) and nothing else, and its bank 1
# values. Where the line falls among the frames depends on the main
# thread's drain, so its place isn't checked.

import json
import os
import subprocess
import sys
import tempfile

ROWS = 6
HEADER = "host_us,timestamp_us,id,flags,data\n"
BANK1 = 11000 + 0x55 * 8
BANK3 = BANK1 + 1


def main():
    cli = sys.argv[1]
    folder = tempfile.mkdtemp(prefix="havne-cli-absent-")
    csv = os.path.join(folder, "frames.csv")
    try:
        with open(csv, "w", newline="\n") as f:
            f.write(HEADER)
            for i in range(ROWS):
                t = 10000 * i
                f.write(f"{t},{t},0x7E8,0x0,41 55 80\n")
        uri = "replay://" + folder.replace("\\", "/") + "?profile=obd2&speed=max"
        r = subprocess.run([cli, "stream", uri, "--values"],
                           capture_output=True, timeout=20)
    finally:
        try:
            os.remove(csv)
            os.rmdir(folder)
        except OSError:
            pass

    if r.returncode != 0:
        print(f"FAIL  exit {r.returncode}: {r.stderr.decode(errors='replace')}")
        return 1
    lines = [json.loads(l) for l in r.stdout.decode().splitlines() if l.strip()]
    absent = [l for l in lines if l.get("type") == "absent"]
    values = [v for l in lines if l.get("type") == "values"
              for v in l.get("values", [])]
    end = lines[-1] if lines else {}
    ok = True
    if [a.get("ids") for a in absent] != [[BANK3]]:
        print(f"FAIL  absent lines {absent}, want one naming {BANK3}")
        ok = False
    if not any(v.get("id") == BANK1 for v in values):
        print(f"FAIL  no value of bank 1 ({BANK1})")
        ok = False
    if end.get("type") != "end" or end.get("reason") != "replay-end":
        print(f"FAIL  last line {end}, want the replay's end")
        ok = False
    if ok:
        print(f"ok    one absent line naming {BANK3}, bank 1 decoded")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
