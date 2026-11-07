# File/layer: python/examples/data/make_drive_loop.py | Python 3.11+, the examples' data
# Testing: run by hand; its output is checked by ExampleTest.notebook
#
# Makes drive-loop/, the recording ecu_states.ipynb reads: one loop of the
# mock's drive cycle (mock_drive_cycle.h), key-on to the engine's stop,
# recorded through core's recorder and cut down to the signals the
# notebook reads. The recording is synthetic, so it holds nothing of a
# car's; its header's times are fixed and its uri is mock://demo, so a
# second run differs only where the mock's timing does.
#
#   python make_drive_loop.py            # about 3 minutes: the loop runs in real time
#
# The mock's loop runs about 153 s from key-on to the engine's stop; the
# recording runs SECONDS, and the folder keeps rows up to the stop plus
# TAIL_US.

import argparse
import csv
import io
import os
import shutil
import sys
import tempfile
import time

import havne

SIGNALS = ("RPM", "Vehicle Speed", "Throttle Position", "Lambda")
SECONDS = 165.0
TAIL_US = 500_000
HERE = os.path.dirname(os.path.abspath(__file__))
BOM = "﻿"


def record(root, seconds):
    """mock://demo recorded for seconds: the recording's folder"""
    with havne.Session("mock://demo") as s:
        r = s.record(root)
        s.start()
        time.sleep(seconds)
        folder = r.folder
    r.close()
    return folder


def engine_stop(rows):
    """host_us of the first RPM under 100 after the engine ran (over 500)"""
    ran = False
    for us, name, value, _ in rows:
        if name != "RPM":
            continue
        rpm = float(value)
        ran = ran or rpm > 500
        if ran and rpm < 100:
            return us
    raise RuntimeError("the engine never stopped: record longer")


def cut(folder, out):
    """folder's SIGNALS up to the engine's stop, into out"""
    with open(os.path.join(folder, "signals.csv"), encoding="utf-8-sig", newline="") as f:
        rows = [(int(r[0]), r[1], r[2], r[3]) for r in list(csv.reader(f))[1:]]
    last = engine_stop(rows) + TAIL_US
    kept = [r for r in rows if r[1] in SIGNALS and r[0] <= last]
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        lines = f.read().splitlines()
    signal_lines = [x for x in lines if x.startswith("signal  ")
                    and x[len("signal  "):].split(",")[0] in SIGNALS]
    core = next(x for x in lines if x.startswith("core  "))
    columns = next(x for x in lines if x.startswith("columns  "))
    end = last / 1e6
    clock = f"00:{int(end) // 60:02d}:{end % 60:06.3f}"
    events = ["ecuio session recording, format 3 (ADR-0049)",
              "started  2026-01-01 00:00:00.000 (local time)",
              "utc  2026-01-01T00:00:00.000Z",
              core, "uri  mock://demo", "profile  mock", "frame flags  complete",
              *signal_lines,
              "caller  make_drive_loop.py",
              columns, "", "",
              f"{clock}  +{end:.3f}  end  one loop of the mock's drive cycle: "
              f"0 frames, {len(kept)} values, 0 records dropped, 0 notes dropped"]
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    with open(os.path.join(out, "events.log"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(events) + "\n")
    body = io.StringIO()
    w = csv.writer(body, lineterminator="\n")
    w.writerow(["host_us", "signal", "value", "unit"])
    w.writerows(kept)
    with open(os.path.join(out, "signals.csv"), "w", encoding="utf-8", newline="") as f:
        f.write(BOM + body.getvalue())
    return len(kept), last


def main(argv=None):
    p = argparse.ArgumentParser(description="one loop of the mock's drive cycle, recorded")
    p.add_argument("--seconds", type=float, default=SECONDS)
    p.add_argument("--out", default=os.path.join(HERE, "drive-loop"))
    args = p.parse_args(argv)
    root = tempfile.mkdtemp(prefix="havne-drive-loop-")
    try:
        folder = record(root, args.seconds)
        n, last = cut(folder, args.out)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(args.out, x)) for x in os.listdir(args.out))
    print(f"{args.out}: {n} values over {last / 1e6:.1f} s, {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
