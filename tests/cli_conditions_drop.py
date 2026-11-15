# File/layer: tests/cli_conditions_drop.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliConditionsRunTest.ADroppedEventIsCountedOnTheEndLine):
#          havne-cli stream's conditions_dropped (ADR-0063)
#
#   python tests/cli_conditions_drop.py <havne-cli>
#
# A live stream whose reader stops reading drops lines, and a condition
# event whose line is dropped is counted on the end line. mock://demo
# idles from about 16 s, its engine speed hunting a few rpm on each tick;
# 64 conditions on it, levels 0.75 rpm apart across the idle's band, give
# many events a second. The line ring is at its floor (HAVNE_CLI_RING_BYTES)
# and the stream records: the recorder's ring is its own, so the
# recording's cond lines are every event the session gave. Read to 17 s
# of session time, stop reading for STALL_S, read to the end; then the
# lines received plus conditions_dropped are the cond lines, above 0, and
# each line received is one of them. The stall outlasts the ring's 32 KiB
# and the pipe's room at about 16 KB/s; Linux's pipe is cut to a page, as
# Windows' is (on Windows the 7 s drop about 200 of some 500 events).

import collections
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import fcntl
except ImportError:
    fcntl = None

SECONDS = 27
STALL_FROM_US = 17 * 1000 * 1000
STALL_S = 7.0
LEVELS = 64


def condition_file(folder):
    conditions = []
    for k in range(LEVELS):
        conditions.append({
            "key": "idle_%02d" % k,
            "name": "Idle %02d" % k,
            "when": {"signal": ["id:1001"], "unit": "RPM",
                     "above": 1405.875 + 0.75 * k},
        })
    path = os.path.join(folder, "idle.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"havne_conditions": 1, "name": "Idle levels",
                   "conditions": conditions}, f)
    return path


def cond_events(events_log):
    """(key, state, fired, stale, t_us) for each cond line"""
    out = []
    with open(events_log, encoding="utf-8") as f:
        for line in f:
            if "  cond  " not in line:
                continue
            fields = line.rstrip("\n").split("  cond  ", 1)[1].split(",")
            words = fields[2].split()
            out.append((fields[0], fields[1], "fired" in words,
                        "stale" in words, int(fields[6])))
    return out


def main():
    cli = os.path.abspath(sys.argv[1])
    scratch = tempfile.mkdtemp(prefix="havne-cli-conditions-drop-")
    try:
        named = condition_file(scratch)
        root = os.path.join(scratch, "recordings")
        os.mkdir(root)
        env = dict(os.environ, HAVNE_CLI_RING_BYTES="1")
        proc = subprocess.Popen(
            [cli, "stream", "mock://demo", "--values", "--conditions", named,
             "--record", root, "--seconds", str(SECONDS)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env)
        if fcntl is not None and hasattr(fcntl, "F_SETPIPE_SZ"):
            fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
        lines = []
        stalled = False
        for raw in proc.stdout:
            line = json.loads(raw)
            lines.append(line)
            if not stalled and line.get("t_us", 0) >= STALL_FROM_US:
                stalled = True
                time.sleep(STALL_S)
        err = proc.stderr.read().decode(errors="replace")
        proc.wait(timeout=60)
        end = [l for l in lines if l.get("type") == "end"]
        recording = [l for l in lines if l.get("type") == "recording"
                     and l.get("state") == "closed"]
        if proc.returncode != 0 or len(end) != 1 or len(recording) != 1 \
                or not stalled:
            print(f"FAIL  the run: exit {proc.returncode}, stalled {stalled},"
                  f" {len(end)} end and {len(recording)} recording lines: {err}")
            return 1
        end, recording = end[0], recording[0]
        ok = True
        if not recording.get("whole") or "conditions_lost" in recording:
            print(f"FAIL  the recording lost events: {recording}")
            ok = False
        folders = os.listdir(root)
        if len(folders) != 1:
            print(f"FAIL  {len(folders)} recordings")
            return 1
        truth = cond_events(os.path.join(root, folders[0], "events.log"))
        got = [(l["key"], l["state"], l["fired"], l["stale"], l["t_us"])
               for l in lines if l.get("type") == "condition"]
        dropped = end.get("conditions_dropped")
        print(f"      {len(got)} condition lines received, "
              f"conditions_dropped {dropped}, {len(truth)} cond lines "
              f"recorded; dropped_frames {end.get('dropped_frames')}, "
              f"dropped_values {end.get('dropped_values')}")
        if dropped is None:
            print("FAIL  the end line has no conditions_dropped")
            ok = False
        elif len(got) + dropped != len(truth):
            print(f"FAIL  {len(got)} received + {dropped} dropped is not "
                  f"the {len(truth)} recorded")
            ok = False
        elif dropped == 0:
            print("FAIL  no condition event was dropped: the stall filled "
                  "no ring")
            ok = False
        missing = collections.Counter(got) - collections.Counter(truth)
        if missing:
            print(f"FAIL  lines received that no cond line holds: "
                  f"{list(missing)[:5]}")
            ok = False
        if ok:
            print("ok    every event is a line received or counted dropped")
        return 0 if ok else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
