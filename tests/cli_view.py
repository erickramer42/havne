# File/layer: tests/cli_view.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliViewRunTest.*): havne-cli view (ADR-0049
#          section 5) from outside the process, on the two pulls in
#          tests/fixtures/recording/
#
#   python tests/cli_view.py once <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py agree <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py stats <havne-cli> <pull-early> <pull-late>
#
# once: over many zooms and alignments, a signal's columns count each of
# its samples once when the axis covers the recording (the default axis
# does): none lost where the axis starts before a recording, none counted
# twice where its first column is split. agree: aligned on the pull's
# rise, the two recordings' columns are equal wherever both have samples,
# in raw columns (under a second) and in whole-second ones (the
# fixtures' pulls are whole seconds apart: wider columns place whole
# seconds). stats: each --span's stats line, both ends counted, is a pass
# over the recording's own signals.csv: the samples, the first and last in
# force, the extremes and their first times, and the mean.

import csv
import json
import math
import os
import subprocess
import sys

SIGNALS = ["--signal", "RPM", "--signal", "Throttle", "--signal", "Coolant",
           "--signal", "VIN"]


def view(cli, folders, args):
    done = subprocess.run([cli, "view"] + folders + args, capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"FAIL  view {' '.join(args)} exited "
                         f"{done.returncode}: {done.stderr.decode()}")
    return [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()]


def once(cli, folders):
    alignments = [[], ["--align-on", "rise:RPM:4000"],
                  ["--align-on", "fall:RPM:3000", "--hysteresis", "100"],
                  ["--align-on", "marker:pull"], ["--align-on", "utc"]]
    zooms = ["1", "3", "7", "13", "40", "77", "400", "1000", "9999"]
    bad = 0
    for align in alignments:
        for n in zooms:
            args = align + ["--columns", n] + SIGNALS
            rows = view(cli, folders, args)
            samples = {(r["recording"], r["name"]): r["samples"]
                       for r in rows if r["type"] == "signal"}
            for r in rows:
                if r["type"] != "columns":
                    continue
                key = (r["recording"], r["signal"])
                if sum(r["samples"]) != samples[key]:
                    print(f"FAIL  {' '.join(args)}: recording {key[0]}'s "
                          f"{key[1]} counts {sum(r['samples'])} of "
                          f"{samples[key]} samples")
                    bad += 1
    if bad == 0:
        print(f"ok    {len(alignments) * len(zooms)} views count every "
              f"sample once")
    return 1 if bad else 0


def agree(cli, folders):
    bad = 0
    for span in [["--from", "-1", "--to", "1", "--columns", "20"],
                 ["--from", "-0.35", "--to", "0.45", "--columns", "16"],
                 ["--from", "-5", "--to", "10", "--columns", "15"],
                 ["--from", "-10", "--to", "15", "--columns", "5"]]:
        args = ["--align-on", "rise:RPM:4000", "--hysteresis", "500"] + span
        rows = view(cli, folders, args + SIGNALS)
        columns = [r for r in rows if r["type"] == "columns"]
        for name in ("RPM", "Throttle"):
            a, b = [r for r in columns if r["signal"] == name]
            pairs = [(x, y) for k in ("lowest", "highest")
                     for x, y in zip(a[k], b[k]) if None not in (x, y)]
            if not pairs or any(x != y for x, y in pairs):
                print(f"FAIL  {' '.join(span)}: {name} differs aligned: "
                      f"{a} against {b}")
                bad += 1
    if bad == 0:
        print("ok    the aligned pulls agree at every zoom")
    return 1 if bad else 0


def samples_of(folder):
    """Each number signal's (host_us, value) in signals.csv's order"""
    out = {}
    with open(os.path.join(folder, "signals.csv"), encoding="utf-8-sig",
              newline="") as f:
        for row in csv.DictReader(f):
            try:
                value = float(row["value"])
            except ValueError:
                continue
            out.setdefault(row["signal"], []).append(
                (int(row["host_us"]), value))
    return out


def by_hand(points, t0, t1):
    """What a stats line owes over [t0, t1]"""
    inside = [(t, v) for t, v in points if t0 <= t <= t1]
    before = [v for t, v in points if t <= t0]
    upto = [v for t, v in points if t <= t1]
    want = {"samples": len(inside),
            "first": before[-1] if before else None,
            "last": upto[-1] if upto else None}
    if inside:
        low = min(v for _, v in inside)
        high = max(v for _, v in inside)
        want.update(lowest=low, highest=high,
                    lowest_us=next(t for t, v in inside if v == low),
                    highest_us=next(t for t, v in inside if v == high),
                    mean=sum(v for _, v in inside) / len(inside),
                    magnitude=sum(abs(v) for _, v in inside) / len(inside))
    return want


def stats(cli, folders):
    spans = ["0:1", "0.1:0.1001", "2.5:7.25", "11:12.3", "0:15", "14.9:15"]
    bad = checked = 0
    for k, folder in enumerate(folders):
        points = samples_of(folder)
        args = ["--signal", "RPM", "--signal", "Throttle"]
        for s in spans:
            args += ["--span", s]
        rows = view(cli, [folder], args)
        for r in (r for r in rows if r["type"] == "stats"):
            t0, t1 = r["from_us"], r["to_us"]
            want = by_hand(points[r["signal"]], t0, t1)
            got_mean = r["mean"]
            for key in ("samples", "first", "last", "lowest", "highest",
                        "lowest_us", "highest_us"):
                if r[key] != want.get(key):
                    print(f"FAIL  recording {k} {r['signal']} {t0}-{t1}: "
                          f"{key} {r[key]}, by hand {want.get(key)}")
                    bad += 1
            if want["samples"] == 0:
                if got_mean is not None:
                    print(f"FAIL  {r['signal']} {t0}-{t1}: a mean of nothing")
                    bad += 1
            elif got_mean is None or not math.isclose(
                    got_mean, want["mean"], rel_tol=0,
                    abs_tol=1e-12 * want["magnitude"]):
                print(f"FAIL  {r['signal']} {t0}-{t1}: mean {got_mean}, by "
                      f"hand {want['mean']}")
                bad += 1
            checked += 1
    if bad == 0 and checked == len(folders) * len(spans) * 2:
        print(f"ok    {checked} stats lines are their samples")
        return 0
    return 1


def main(argv):
    modes = {"once": once, "agree": agree, "stats": stats}
    if len(argv) != 5 or argv[1] not in modes:
        print(__doc__ or "see the header", file=sys.stderr)
        return 2
    return modes[argv[1]](argv[2], argv[3:5])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
