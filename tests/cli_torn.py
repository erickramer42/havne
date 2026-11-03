# File/layer: tests/cli_torn.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliReplayTest.ATornLastRowIsLeftOut): havne-cli
#          stream of a recording whose last row has no newline (a write
#          cut short, ADR-0049 section 7) plays the rows before it and
#          says so in the session line's replay object (rows_torn)
#
#   python tests/cli_torn.py <havne-cli>
#
# A scratch recording of three whole rows and a fourth cut mid-row
# replays at full speed. The stream must exit 0 at the replay's end, its
# session line's replay object holding frames 3 and rows_torn 1, and
# three frame lines; the same recording whole holds no rows_torn key.

import json
import os
import subprocess
import sys
import tempfile

HEADER = "host_us,timestamp_us,id,flags,data\n"
ROWS = "".join(f"{t},{t},0x7E8,0x0,41 0C 1F 40\n" for t in (0, 10000, 20000))


def stream(cli, text):
    folder = tempfile.mkdtemp(prefix="havne-cli-torn-")
    csv = os.path.join(folder, "frames.csv")
    try:
        with open(csv, "w", newline="\n") as f:
            f.write(HEADER + text)
        uri = "replay://" + folder.replace("\\", "/") + "?speed=max"
        r = subprocess.run([cli, "stream", uri], capture_output=True,
                           timeout=20)
    finally:
        try:
            os.remove(csv)
            os.rmdir(folder)
        except OSError:
            pass
    lines = [json.loads(l) for l in r.stdout.decode().splitlines() if l.strip()]
    return r, lines


def main():
    cli = sys.argv[1]
    ok = True
    r, lines = stream(cli, ROWS + "30000,30000,0x7E8,0x0,41 0C")
    if r.returncode != 0:
        print(f"FAIL  exit {r.returncode}: {r.stderr.decode(errors='replace')}")
        return 1
    session = next((l for l in lines if l.get("type") == "session"), {})
    replay = session.get("replay", {})
    frames = [l for l in lines if l.get("type") == "frame"]
    end = lines[-1] if lines else {}
    if replay.get("frames") != 3 or replay.get("rows_torn") != 1:
        print(f"FAIL  replay {replay}, want frames 3 and rows_torn 1")
        ok = False
    if len(frames) != 3:
        print(f"FAIL  {len(frames)} frame lines, want 3")
        ok = False
    if end.get("type") != "end" or end.get("reason") != "replay-end":
        print(f"FAIL  last line {end}, want the replay's end")
        ok = False
    r, lines = stream(cli, ROWS)
    session = next((l for l in lines if l.get("type") == "session"), {})
    if "rows_torn" in session.get("replay", {}):
        print(f"FAIL  a whole recording's replay {session.get('replay')}")
        ok = False
    if ok:
        print("ok    three rows played, rows_torn 1; none on a whole one")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
