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
#   python tests/cli_view.py shifted <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py instant <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py past <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py named <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py end <havne-cli> <pull-twice> <pull-early>
#   python tests/cli_view.py interrupt <havne-cli> <pull-twice> <pull-early>
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
# many zooms, started and aligned on chosen pulls, each column's count,
# range and mean are a pass over signals.csv: by the reader's rule (a
# column under a second its own samples; a wider one the whole seconds
# that start in it, the first taking the second across its start), or in
# an aligned view with columns of a second or more the samples in the
# column's own microseconds, and a later recording's delta is its mean
# less the first's wherever both have one. deltas: a
# delta is taken only between a signal of one name in one unit (a copy of
# the first recording with its speed relabeled mph has none for it), and
# never from a recording's own line when a signal is named twice.
# shifted: a copy of the first recording with every sample half a second
# later, aligned on the same pull, differs from it in no column at any
# zoom. instant: a span that ends on a recording's first microsecond
# holds its sample there. past: a pull number past a recording's pulls
# leaves that recording off the axis while the other is drawn. named:
# --stretches
# takes pulls, cruises or all after it as its word, so a folder of that
# name is given as ./all. end: a recording whose last sample sits
# exactly at its length, viewed in column counts its length divides by,
# has that sample in the last column: counted once, at every zoom,
# started or aligned. interrupt: a view held at its first search's ask
# (the stretches, a pull to align on, a level's rise) and interrupted ends
# with the interrupted end line and exit 1, and writes nothing the search
# would have given.

import csv
import json
import math
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

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
            elif t == t1:
                out[n - 1].append(v)  # the axis's end, in the last column
        else:
            begins = t // second * second
            if begins >= start // second * second and begins < t1:
                out[skip + (max(begins, start) - start) // col].append(v)
            elif t == t1:
                out[n - 1].append(v)
    return out


def own_windows_by_hand(points, axis, offset):
    """Each axis column's samples of one signal in the column's own
    microseconds of the recording's time"""
    col = axis["column_us"]
    n = axis["columns"]
    out = []
    for c in range(n):
        lo = axis["from_us"] + c * col - offset
        hi = lo + col - (0 if c == n - 1 else 1)  # the last holds the end
        out.append([v for t, v in points if lo <= t <= hi])
    return out


def means(cli, folders):
    alignments = [[], ["--align-on", "pull:1"], ["--align-on", "pull:2,1"],
                  ["--align-on", "utc"]]
    zooms = ["1", "3", "7", "13", "40", "77", "400", "1000"]
    bad, checked, deltas, wide = columns_checked(cli, folders, alignments,
                                                 zooms)
    if bad == 0 and checked > 6000 and deltas > 500 and wide >= 30:
        print(f"ok    {checked} columns' means are their samples', {deltas} "
              f"deltas the means' difference, {wide} lines of an aligned "
              f"view's wide columns among them")
        return 0
    print(f"FAIL  {bad} wrong of {checked} columns and {deltas} deltas")
    return 1


def columns_checked(cli, folders, alignments, zooms):
    """Every columns line of each view against signals.csv by hand: what
    was wrong, the columns and deltas checked, the wide aligned lines"""
    points = [samples_of(f) for f in folders]
    bad = checked = deltas = wide = 0
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
                # An aligned view's wide columns hold the column's own
                # window, not the seconds a query places there
                own = bool(align) and axis["column_us"] >= 1000000
                if own:
                    hand = own_windows_by_hand(points[k][r["signal"]], axis,
                                               offset.get(k, 0))
                wide += own
                want = [sum(c) / len(c) if c else None for c in hand]
                what = f"{' '.join(args)}: recording {k}'s {r['signal']}"
                if r["samples"] != [len(c) for c in hand]:
                    print(f"FAIL  {what} counts {r['samples']}, by hand "
                          f"{[len(c) for c in hand]}")
                    bad += 1
                for key, pick in (("lowest", min), ("highest", max)):
                    extremes = [pick(c) if c else None for c in hand]
                    if r[key] != extremes:
                        print(f"FAIL  {what} {key} {r[key]}, by hand "
                              f"{extremes}")
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
    return bad, checked, deltas, wide


def end(cli, folders):
    """A sample exactly at the axis's end is in the last column"""
    peak = 9999
    zooms = ["1", "4", "8", "40", "400", "1000"]
    with tempfile.TemporaryDirectory() as scratch:
        copy = os.path.join(scratch, "to-its-end")
        os.mkdir(copy)
        with open(os.path.join(folders[0], "events.log"), "rb") as f:
            events = f.read()
        with open(os.path.join(copy, "events.log"), "wb") as f:
            f.write(events)
        with open(os.path.join(folders[0], "signals.csv"), "rb") as f:
            rows = f.read()
        length = next(r for r in view(cli, [folders[0]], ["--columns", "1"])
                      if r["type"] == "opened")["duration_us"]
        if any(length % int(n) for n in zooms):
            print(f"FAIL  {length} us doesn't divide by every zoom")
            return 1
        with open(os.path.join(copy, "signals.csv"), "wb") as f:
            f.write(rows + b"%d,RPM,%d,rpm\n" % (length, peak))
        bad, checked, deltas, wide = columns_checked(
            cli, [copy, copy],
            [[], ["--align-on", "pull:1"], ["--align-on", "utc"]], zooms)
        ends = 0
        for align in ([], ["--align-on", "pull:1"]):
            for n in zooms:
                rows = view(cli, [copy], align + ["--columns", n, "--signal",
                                                  "RPM"])
                axis = next(r for r in rows if r["type"] == "axis")
                total = next(r["samples"] for r in rows
                             if r["type"] == "signal" and r["name"] == "RPM")
                (line,) = [r for r in rows if r["type"] == "columns"]
                where = f"{' '.join(align)} --columns {n}"
                if axis["to_us"] - axis["from_us"] != length:
                    print(f"FAIL  {where}: the axis isn't the recording's "
                          f"length: {axis}")
                    bad += 1
                if sum(line["samples"]) != total:
                    print(f"FAIL  {where}: {sum(line['samples'])} of {total} "
                          f"samples counted")
                    bad += 1
                if line["highest"][-1] != peak or any(
                        h == peak for h in line["highest"][:-1]):
                    print(f"FAIL  {where}: the sample at the end isn't in "
                          f"the last column alone: {line['highest']}")
                    bad += 1
                ends += 1
    if bad == 0 and checked > 2000 and wide >= 8 and ends == 12:
        print(f"ok    a sample at the axis's end is in the last column in "
              f"{ends} views, {checked} columns by hand")
        return 0
    print(f"FAIL  {bad} wrong; {checked} columns, {wide} wide lines, {ends} "
          f"views")
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


def shifted_copy(folder, scratch, by_us):
    """The recording again, every sample by_us later"""
    copy = os.path.join(scratch, "later")
    os.mkdir(copy)
    with open(os.path.join(folder, "events.log"), "rb") as f:
        events = f.read()
    with open(os.path.join(copy, "events.log"), "wb") as f:
        f.write(events)
    with open(os.path.join(folder, "signals.csv"), "rb") as f:
        head, *rows = f.read().split(b"\n")
    with open(os.path.join(copy, "signals.csv"), "wb") as f:
        f.write(head + b"\n")
        for row in rows:
            if row.strip():
                t, rest = row.split(b",", 1)
                f.write(b"%d,%s\n" % (int(t) + by_us, rest))
    return copy


def shifted(cli, folders):
    bad = zeros = wide = 0
    with tempfile.TemporaryDirectory() as scratch:
        copy = shifted_copy(folders[0], scratch, 500000)
        for n in ["1", "3", "8", "13", "40", "77", "400"]:
            args = ["--align-on", "pull:1", "--columns", n, "--signal", "RPM",
                    "--signal", "Throttle", "--signal", "Vehicle Speed"]
            rows = view(cli, [folders[0], copy], args)
            offsets = [r["offset_us"] for r in rows if r["type"] == "align"]
            if len(offsets) != 2 or offsets[1] - offsets[0] != -500000:
                print(f"FAIL  the copy isn't half a second later: {offsets}")
                return 1
            axis = next(r for r in rows if r["type"] == "axis")
            wide += axis["column_us"] >= 1000000
            theirs = [r for r in rows
                      if r["type"] == "columns" and r["recording"] == 1]
            if len(theirs) != 3:
                print(f"FAIL  --columns {n}: the copy's lines {theirs}")
                return 1
            for r in theirs:
                if r.get("delta_from") != 0:
                    print(f"FAIL  --columns {n}: {r['signal']} has no delta")
                    bad += 1
                    continue
                off = [(c, d) for c, d in enumerate(r["delta"])
                       if d is not None and abs(d) > 1e-9]
                zeros += sum(d is not None for d in r["delta"])
                if off:
                    print(f"FAIL  --columns {n}: {r['signal']} differs from "
                          f"itself half a second later, (column, delta) "
                          f"{off[:6]}")
                    bad += 1
    if bad == 0 and zeros > 500 and wide >= 4:
        print(f"ok    {zeros} deltas of a recording against itself half a "
              f"second later are 0, {wide} zooms in columns of a second or "
              f"more")
        return 0
    print(f"FAIL  {bad} lines differ; {zeros} deltas, {wide} wide zooms")
    return 1


def instant(cli, folders):
    bad = 0
    # A span ending on the first microsecond: Vehicle Speed's sample at 0
    # is inside it, RPM's at 1 ms and Throttle's at 2 ms aren't
    points = samples_of(folders[0])
    rows = view(cli, [folders[0]],
                ["--from", "-1", "--span", "-1:0", "--signal",
                 "Vehicle Speed", "--signal", "RPM", "--signal", "Throttle"])
    lines = {r["signal"]: r for r in rows if r["type"] == "stats"}
    if sorted(lines) != ["RPM", "Throttle", "Vehicle Speed"]:
        print(f"FAIL  the span's stats lines: {lines}")
        return 1
    if not any(t == 0 for t, _ in points["Vehicle Speed"]):
        print("FAIL  the first recording has no speed sample at 0")
        return 1
    for name, r in lines.items():
        want = by_hand(points[name], r["from_us"], r["to_us"])
        # A span from before the recording's start has no value in force
        # at its start
        want["first"] = None
        for key in ("samples", "first", "last", "lowest", "highest",
                    "lowest_us", "highest_us", "mean"):
            if r[key] != want.get(key):
                print(f"FAIL  {name} over -1:0: {key} {r[key]}, by hand "
                      f"{want.get(key)}")
                bad += 1
    if lines["Vehicle Speed"]["samples"] != 1:
        print(f"FAIL  the sample at 0 isn't counted: {lines['Vehicle Speed']}")
        bad += 1
    if lines["RPM"]["samples"] != 0 or lines["Throttle"]["samples"] != 0:
        print(f"FAIL  a sample after 0 is counted in a span ending at 0")
        bad += 1

    # An axis that ends on the first microsecond: the sample there is in
    # the last column, in columns under a second and of a second or more
    for span in (["--from", "-1", "--to", "0"], ["--from", "-8", "--to", "0"]):
        rows = view(cli, [folders[0]], span + [
            "--columns", "4", "--signal", "Vehicle Speed", "--signal", "RPM"])
        got = {r["signal"]: r for r in rows if r["type"] == "columns"}
        speed = got.get("Vehicle Speed", {})
        if speed.get("samples") != [0, 0, 0, 1] or \
                speed.get("lowest") != [None, None, None, 50] or \
                speed.get("mean") != [None, None, None, 50]:
            print(f"FAIL  {' '.join(span)}: the sample at the axis's end, "
                  f"the recording's first: {speed}")
            bad += 1
        if got.get("RPM", {}).get("samples") != [0, 0, 0, 0]:
            print(f"FAIL  {' '.join(span)}: a sample past the axis's end "
                  f"is counted: {got.get('RPM')}")
            bad += 1

    # A text signal's sample at 0 is counted, and counted is all it is
    with tempfile.TemporaryDirectory() as scratch:
        copy = os.path.join(scratch, "noted")
        os.mkdir(copy)
        with open(os.path.join(folders[0], "events.log"), "rb") as f:
            events = f.read()
        with open(os.path.join(copy, "events.log"), "wb") as f:
            f.write(events)
        with open(os.path.join(folders[0], "signals.csv"), "rb") as f:
            head, first, rest = f.read().split(b"\n", 2)
        with open(os.path.join(copy, "signals.csv"), "wb") as f:
            f.write(head + b"\n" + first + b"\n0,Note,as it left,\n" + rest)
        rows = view(cli, [copy], ["--from", "-1", "--span", "-1:0",
                                  "--signal", "Note"])
    (note,) = [r for r in rows if r["type"] == "stats"]
    if note["samples"] != 1 or any(
            note[key] is not None
            for key in ("first", "last", "lowest", "highest", "lowest_us",
                        "highest_us", "mean")):
        print(f"FAIL  a text signal's sample at 0: {note}")
        bad += 1
    if bad == 0:
        print("ok    a span ending at 0 holds the sample there and no later "
              "one")
    return 1 if bad else 0


def past(cli, folders):
    bad = 0
    # The first recording has two pulls: its third is none, so it has no
    # place on the axis, and the second recording's first is still drawn
    rows = view(cli, folders, ["--align-on", "pull:3,1", "--columns", "4",
                               "--signal", "RPM"])
    aligns = {r["recording"]: r for r in rows if r["type"] == "align"}
    columns = [r for r in rows if r["type"] == "columns"]
    none = aligns.get(0, {})
    if none.get("found") is not False or none.get("host_us") is not None \
            or none.get("offset_us") is not None:
        print(f"FAIL  a third pull was found in two: {none}")
        bad += 1
    if aligns.get(1, {}).get("found") is not True:
        print(f"FAIL  the second recording's first pull: {aligns.get(1)}")
        bad += 1
    if [r["recording"] for r in columns] != [1]:
        print(f"FAIL  the columns lines' recordings: "
              f"{[r['recording'] for r in columns]}")
        bad += 1
    elif "delta" in columns[0] or "delta_from" in columns[0]:
        print(f"FAIL  a delta from a recording that isn't drawn: {columns[0]}")
        bad += 1
    # The second pull is there: the count is asked before the pulls
    rows = view(cli, folders, ["--align-on", "pull:2,1", "--columns", "4",
                               "--signal", "RPM"])
    found = [r["found"] for r in rows if r["type"] == "align"]
    if found != [True, True]:
        print(f"FAIL  pull:2,1 found {found}")
        bad += 1
    if bad == 0:
        print("ok    a pull past a recording's pulls leaves it off the axis, "
              "the other drawn")
    return 1 if bad else 0


def named(cli, folders):
    """--stretches' word against a folder of that name"""
    bad = 0
    with tempfile.TemporaryDirectory() as scratch:
        os.mkdir(os.path.join(scratch, "all"))
        for name in ("events.log", "signals.csv"):
            with open(os.path.join(folders[0], name), "rb") as f:
                text = f.read()
            with open(os.path.join(scratch, "all", name), "wb") as f:
                f.write(text)

        def run(args):
            return subprocess.run([cli, "view"] + args, capture_output=True,
                                  cwd=scratch)

        done = run(["--stretches", "all"])
        if done.returncode != 2 or done.stdout or \
                b"view needs a recording's folder" not in done.stderr:
            print(f"FAIL  --stretches all with a folder all: exit "
                  f"{done.returncode}, {done.stderr.decode()}")
            bad += 1
        for args, kinds in ((["--stretches", "./all"], {"pull", "cruise"}),
                            (["--stretches", "all", "./all"],
                             {"pull", "cruise"}),
                            (["--stretches", "pulls", "all"], {"pull"}),
                            (["./all", "--stretches"], {"pull", "cruise"})):
            done = run(args)
            rows = [json.loads(l)
                    for l in done.stdout.decode("utf-8").splitlines()]
            got = {r["kind"] for r in rows if r["type"] == "stretch"}
            # The fixture has pulls and no cruise: the roles line says
            # which kinds were asked by what it searched with
            asked = [r for r in rows if r["type"] == "roles"]
            if done.returncode != 0 or len(asked) != 1 or \
                    not got or not got <= kinds:
                print(f"FAIL  view {' '.join(args)}: exit {done.returncode}, "
                      f"kinds {got}, {done.stderr.decode()}")
                bad += 1
    if bad == 0:
        print("ok    --stretches takes its word first: a folder named all "
              "is ./all")
    return 1 if bad else 0


# The first ask's hold (HAVNE_CLI_EXPORT_HOLD_MS, tests only): long enough
# that an end within half of it is the interrupt's
HOLD_MS = 20000
HELD_WAIT_S = 30


def interrupted_view(cli, args):
    """view args held at its first search's ask, then interrupted: None, a
    failure's words, or "skip" with no console"""
    env = dict(os.environ)
    env["HAVNE_CLI_EXPORT_HOLD_MS"] = str(HOLD_MS)
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.DEVNULL, "env": env}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([cli, "view"] + args, **kw)
    held = []
    reader = threading.Thread(target=lambda: held.append(p.stderr.readline()))
    reader.start()
    reader.join(HELD_WAIT_S)
    if reader.is_alive() or not held or \
            not held[0].startswith(b"havne-cli: holding the view's"):
        p.kill()
        _, err = p.communicate()
        reader.join()
        return (f"didn't hold at the view's first ask within {HELD_WAIT_S} "
                f"s: exit {p.returncode}, "
                f"{(held[0] if held else b'') + err!r}")
    try:
        p.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt"
                      else signal.SIGINT)
    except OSError as e:
        p.kill()
        p.communicate()
        print(f"skip  no console to send the interrupt through: {e}")
        return "skip"
    sent = time.monotonic()
    try:
        out, err = p.communicate(timeout=HOLD_MS / 2000)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return f"still running {HOLD_MS / 2000:.0f} s after the interrupt"
    took = time.monotonic() - sent
    lines = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    if p.returncode != 1 or not lines or \
            lines[-1] != {"type": "end", "reason": "interrupted"}:
        return (f"exit {p.returncode}, lines {lines}, "
                f"{err.decode('utf-8', 'replace')}")
    found = [l["type"] for l in lines
             if l["type"] in ("align", "stretch", "axis", "columns", "error")]
    if found:
        return f"an interrupted view wrote {found}"
    print(f"ok    view {' '.join(args[1:])}: interrupted, ended {took:.2f} s "
          f"after it")
    return None


def interrupt(cli, folders):
    """Each of view's searches interrupted at its first ask"""
    bad = 0
    for args in ([folders[0], "--stretches", "pulls"],
                 [folders[0], "--align-on", "pull:1"],
                 [folders[0], "--align-on", "rise:RPM:3000"]):
        why = interrupted_view(cli, args)
        if why == "skip":
            return 77
        if why is not None:
            print(f"FAIL  view {' '.join(args[1:])}: {why}")
            bad += 1
    return 1 if bad else 0


def main(argv):
    modes = {"once": once, "agree": agree, "stats": stats, "means": means,
             "deltas": deltas, "shifted": shifted, "instant": instant,
             "past": past, "named": named, "end": end,
             "interrupt": interrupt}
    if len(argv) != 5 or argv[1] not in modes:
        print(__doc__ or "see the header", file=sys.stderr)
        return 2
    return modes[argv[1]](argv[2], argv[3:5])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
