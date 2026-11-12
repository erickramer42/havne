# File/layer: tests/cli_view.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliViewRunTest.*): havne-cli view (ADR-0049
#          section 5) from outside the process, on the two pulls in
#          tests/fixtures/recording/
#
#   python tests/cli_view.py once <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py agree <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py stats <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py means <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py deltas <havne-cli> <pull-twice> <pull-early>
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
# force, the extremes and their first times, and the mean. means: over
# many zooms, started and aligned on chosen pulls, each column's count and
# mean are a pass over signals.csv by the reader's rule (a column under a
# second its own samples; a wider one the whole seconds that start in it,
# the first taking the second across its start), and a later recording's
# delta is its mean less the first's wherever both have one. deltas: a
# delta is taken only between a signal of one name in one unit (a copy of
# the first recording with its speed relabeled mph has none for it), and
# never from a recording's own line when a signal is named twice.

import csv
import json
import math
import os
import subprocess
import sys
import tempfile

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


def columns_by_hand(points, axis, offset):
    """Each axis column's samples of one signal, by the reader's rule"""
    col = axis["column_us"]
    n = axis["columns"]
    second = 1000000
    t0 = axis["from_us"] - offset
    t1 = axis["to_us"] - offset
    out = [[] for _ in range(n)]
    if t1 <= 0:
        return out
    skip = -(t0 // col) if t0 < 0 else 0
    start = t0 + skip * col
    split = 0
    if skip > 0 and start > 0:
        # The column across the recording's start, a query of its own
        split = start if col < second else start // second * second
    for t, v in points:
        if t < split:
            out[skip - 1].append(v)
        elif col < second:
            if start <= t < t1:
                out[skip + (t - start) // col].append(v)
        else:
            begins = t // second * second
            if begins >= start // second * second and begins < t1:
                out[skip + (max(begins, start) - start) // col].append(v)
    return out


def means(cli, folders):
    alignments = [[], ["--align-on", "pull:1"], ["--align-on", "pull:2,1"],
                  ["--align-on", "utc"]]
    zooms = ["1", "3", "7", "13", "40", "77", "400", "1000"]
    points = [samples_of(f) for f in folders]
    bad = checked = deltas = 0
    for align in alignments:
        for n in zooms:
            args = align + ["--columns", n, "--signal", "RPM", "--signal",
                            "Throttle"]
            rows = view(cli, folders, args)
            axis = next(r for r in rows if r["type"] == "axis")
            offset = {r["recording"]: r["offset_us"]
                      for r in rows if r["type"] == "align"}
            first = {}
            for r in (r for r in rows if r["type"] == "columns"):
                k = r["recording"]
                hand = columns_by_hand(points[k][r["signal"]], axis,
                                       offset.get(k, 0))
                want = [sum(c) / len(c) if c else None for c in hand]
                what = f"{' '.join(args)}: recording {k}'s {r['signal']}"
                if r["samples"] != [len(c) for c in hand]:
                    print(f"FAIL  {what} counts {r['samples']}, by hand "
                          f"{[len(c) for c in hand]}")
                    bad += 1
                for c, (got, w) in enumerate(zip(r["mean"], want)):
                    if (got is None) != (w is None) or (
                            w is not None and not math.isclose(
                                got, w, rel_tol=1e-12, abs_tol=1e-9)):
                        print(f"FAIL  {what} column {c}: mean {got}, by hand "
                              f"{w}")
                        bad += 1
                    checked += 1
                if r["signal"] not in first:
                    first[r["signal"]] = (k, r["mean"])
                    if "delta" in r or "delta_from" in r:
                        print(f"FAIL  {what}: a delta from itself")
                        bad += 1
                    continue
                base, theirs = first[r["signal"]]
                if r.get("delta_from") != base or "delta" not in r:
                    print(f"FAIL  {what}: no delta from recording {base}")
                    bad += 1
                    continue
                for c, (d, mine, other) in enumerate(
                        zip(r["delta"], r["mean"], theirs)):
                    w = None if None in (mine, other) else mine - other
                    if (d is None) != (w is None) or (
                            w is not None and not math.isclose(
                                d, w, rel_tol=1e-12, abs_tol=1e-9)):
                        print(f"FAIL  {what} column {c}: delta {d}, the "
                              f"means' {w}")
                        bad += 1
                    deltas += w is not None
    if bad == 0 and checked > 6000 and deltas > 500:
        print(f"ok    {checked} columns' means are their samples', {deltas} "
              f"deltas the means' difference")
        return 0
    print(f"FAIL  {bad} wrong of {checked} columns and {deltas} deltas")
    return 1


def deltas(cli, folders):
    bad = 0

    def lines(rows, recording, signal):
        return [r for r in rows if r["type"] == "columns"
                and r["recording"] == recording and r["signal"] == signal]

    # The first recording again, its speed's unit mph in place of km/h
    with tempfile.TemporaryDirectory() as scratch:
        copy = os.path.join(scratch, "in-mph")
        os.mkdir(copy)
        for name in ("events.log", "signals.csv"):
            with open(os.path.join(folders[0], name), "rb") as f:
                text = f.read()
            if b"km/h" not in text:
                print(f"FAIL  {name} of the first recording has no km/h")
                bad += 1
            with open(os.path.join(copy, name), "wb") as f:
                f.write(text.replace(b"km/h", b"mph"))
        rows = view(cli, [folders[0], copy],
                    ["--columns", "4", "--signal", "Vehicle Speed",
                     "--signal", "RPM"])
    (speed,) = lines(rows, 1, "Vehicle Speed")
    (rpm,) = lines(rows, 1, "RPM")
    if "delta" in speed or "delta_from" in speed:
        print(f"FAIL  mph less km/h was taken as a delta: {speed}")
        bad += 1
    if "mean" not in speed or speed["mean"] != lines(rows, 0, "Vehicle Speed")[0]["mean"]:
        print(f"FAIL  the relabeled speed lost its means: {speed}")
        bad += 1
    if rpm.get("delta_from") != 0 or any(d != 0 for d in rpm.get("delta", [1])):
        print(f"FAIL  RPM in rpm both sides has no delta of 0: {rpm}")
        bad += 1

    # A signal named twice: no delta from its own recording's line, and
    # the second recording's two lines each take one from the first
    rows = view(cli, folders, ["--align-on", "pull:1", "--columns", "6",
                               "--signal", "RPM", "--signal", "RPM"])
    own = lines(rows, 0, "RPM")
    theirs = lines(rows, 1, "RPM")
    if len(own) != 2 or any("delta" in r or "delta_from" in r for r in own):
        print(f"FAIL  a recording took a delta from itself: {own}")
        bad += 1
    if len(theirs) != 2 or any(r.get("delta_from") != 0 for r in theirs):
        print(f"FAIL  the second recording's lines lack their delta: {theirs}")
        bad += 1
    if bad == 0:
        print("ok    a delta is between one name in one unit in two recordings")
    return 1 if bad else 0


def main(argv):
    modes = {"once": once, "agree": agree, "stats": stats, "means": means,
             "deltas": deltas}
    if len(argv) != 5 or argv[1] not in modes:
        print(__doc__ or "see the header", file=sys.stderr)
        return 2
    return modes[argv[1]](argv[2], argv[3:5])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
