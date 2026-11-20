# File/layer: tests/cli_transfer_import.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliTransferRunTest.csv-import, .csv-guess):
#          havne-cli import csv (ADR-0054) from outside the process
#
#   python tests/cli_transfer_import.py csv-import <havne-cli> <csv fixtures>
#   python tests/cli_transfer_import.py csv-guess <havne-cli> <csv fixtures>
#
# csv-import: the made-up loggers' CSVs imported by their column maps and
# viewed (each signal's samples and range, the UTC start or null); a
# missing map, a map core refuses and a folder already there each exit 3
# with words, leaving nothing made; an interval logger-a is slower than
# says so in the estimate and the export. csv-guess: the same CSVs with no
# map: --print-map prints each logger's guessed-map.json byte for byte,
# and the import writes the guess's line (that map) before its own; a time
# column named so, of whole numbers with no unit, reads as ms with a note
# and --time-unit gives another; one with fractions is refused until
# --time-unit gives one; logger-c, a tuner log's shape (padded cells, a
# trailing comma, a sparse counter), imports with its notes, its text
# trimmed but a quoted cell kept, and its wide export keeps the counter,
# which paces no sweep; logger-d, whose rows with no time (one mid-file, a
# last one of padding) are left out, counted in the imported line and said
# on stderr; a file with no header is refused, exit 3 with words and
# nothing made.

import csv
import json
import os
import subprocess
import sys
import tempfile

from cli_transfer_kit import one, partial_left, run, same


LOGGERS = [
    # name, lines, values, columns ignored, utc start, duration, each
    # signal (name, text, samples, lowest, highest)
    ("logger-a", 11, 22, 1, None, 1750000,
     [("RPM", False, 8, 780.5, 3420.25), ("Engine Load", False, 7, 12, 88.5),
      ("Lambda", False, 7, 0.86, 1.98)]),
    ("logger-b", 9, 16, 2, 1793883802250000, 3500000,
     [("Vehicle Speed", False, 8, 0, 48.5), ("Gear", True, 8, None, None)]),
]


def csv_import(cli, fixtures):
    with tempfile.TemporaryDirectory() as d:
        for name, lines, values, ignored, utc, duration, signals in LOGGERS:
            base = os.path.join(fixtures, name)
            log = os.path.join(base, "log.csv")
            cmap = os.path.join(base, "map.json")
            rec = os.path.join(d, name)
            imported = one(run(cli, ["import", "csv", log, rec, "--map", cmap]),
                           "imported")
            want = {"type": "imported", "format": "csv", "folder": rec,
                    "lines": lines, "values": values,
                    "columns_ignored": ignored, "rows_no_time": 0,
                    "utc_start_us": utc, "duration_us": duration}
            same(f"{name}'s imported line", want, imported)
            if sorted(os.listdir(rec)) != ["events.log", "signals.csv"]:
                raise SystemExit(f"FAIL  {name} made {os.listdir(rec)}")
            viewed = run(cli, ["view", rec, "--columns", "4"])
            opened = one(viewed, "opened")
            if (opened["utc_start_us"], opened["duration_us"]) != (utc,
                                                                    duration):
                raise SystemExit(f"FAIL  {name} opened {opened}")
            got = [(s["name"], s["text"], s["samples"], s.get("lowest"),
                    s.get("highest"))
                   for s in viewed if s["type"] == "signal"]
            same(f"{name}'s signals", signals, got)
        # Refused, each exit 3 with words and no folder: no map file, a map
        # core refuses, a folder already there
        a = os.path.join(fixtures, "logger-a")
        log = os.path.join(a, "log.csv")
        rec = os.path.join(d, "refused")
        error = one(run(cli, ["import", "csv", log, rec, "--map",
                              os.path.join(d, "none.json")], 3), "error")
        if error["detail"] != ("the column map can't be read: there's no "
                               "such file"):
            raise SystemExit(f"FAIL  a missing map said {error}")
        bad = os.path.join(d, "bad.json")
        with open(bad, "w", newline="\n") as f:
            f.write('{"havne_column_map": 1, "shade": "red"}\n')
        error = one(run(cli, ["import", "csv", log, rec, "--map", bad], 3),
                    "error")
        if "'shade'" not in error["detail"]:
            raise SystemExit(f"FAIL  an unknown key said {error}")
        if os.path.exists(rec) or partial_left(rec):
            raise SystemExit("FAIL  a refused import left a folder")
        there = os.path.join(d, "logger-a")
        error = one(run(cli, ["import", "csv", log, there, "--map",
                              os.path.join(a, "map.json")], 3), "error")
        if sorted(os.listdir(there)) != ["events.log", "signals.csv"]:
            raise SystemExit(f"FAIL  the folder there changed: {error}")
        slower_than_its_intervals(cli, there, d)
    print(f"ok    {len(LOGGERS)} loggers' CSVs imported and viewed; a missing "
          "map, a refused map and a folder already there refused; an "
          "interval logger-a is slower than said so")
    return 0


def slower_than_its_intervals(cli, rec, d):
    """logger-a holds about four rows per second: an interval of 100 or
    250 ms gives no fewer rows than the sweep's, which the estimate and
    the export say, and one of 1000 ms thins them, which neither says"""
    sized = one(run(cli, ["export", rec, "--format", "csv", "--estimate"]),
                "estimate")
    flagged = [s["row_ms"] for s in sized["sizes"]
               if s.get("no_fewer_than_sweep")]
    if flagged != [100, 250]:
        raise SystemExit(f"FAIL  the estimate flagged {flagged}: {sized}")
    for rows, says in [("100", True), ("1000", False)]:
        alone = one(run(cli, ["export", rec, "--format", "csv", "--estimate",
                              "--rows", rows]), "estimate")
        if ([s["row_ms"] for s in alone["sizes"]] != [int(rows)] or
                alone["sizes"][0].get("no_fewer_than_sweep", False) != says):
            raise SystemExit(f"FAIL  --rows {rows}'s estimate {alone}")
        out = os.path.join(d, f"logger-a-{rows}.csv")
        done = subprocess.run([cli, "export", rec, out, "--format", "csv",
                               "--rows", rows], capture_output=True,
                              stdin=subprocess.DEVNULL)
        lines = [json.loads(l) for l in
                 done.stdout.decode("utf-8").splitlines()]
        said = done.stderr.decode("utf-8").replace("\r", "")
        exported = one(lines, "exported")
        if done.returncode != 0 or exported["rows"] != alone["sizes"][0]["rows"]:
            raise SystemExit(f"FAIL  --rows {rows} exported {lines} {said}")
        line = (f"havne-cli: every {rows} ms gives no fewer rows than every "
                "sweep: the recording is slower than that\n")
        if exported.get("no_fewer_than_sweep", False) != says or \
                (said == line) != says or (not says and said):
            raise SystemExit(f"FAIL  --rows {rows} said {exported} {said!r}")


GUESSED = [
    # name, values imported with the guessed map (each column's samples)
    ("logger-a", 25),
    ("logger-b", 27),
    # a row with no time and a text among the three columns' first rows
    ("logger-e", 12),
]


def printed(cli, args, want=0):
    """--print-map's stdout as bytes and its stderr, which aren't lines"""
    done = subprocess.run([cli] + args, capture_output=True,
                          stdin=subprocess.DEVNULL)
    if done.returncode != want:
        raise SystemExit(f"FAIL  {' '.join(args)} exited {done.returncode}, "
                         f"not {want}: {done.stderr.decode()}")
    return done.stdout, done.stderr.decode("utf-8")


def committed(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r", b"")


def wide_columns(path):
    """A wide CSV's header and each column's cells, its comment line
    left out"""
    with open(path, newline="", encoding="utf-8") as f:
        rows = [r for r in csv.reader(f) if r and not r[0].startswith("#")]
    head = rows[0]
    return head, {h: [r[i] for r in rows[1:]] for i, h in enumerate(head)}


def logger_c(cli, fixtures, d):
    """A tuner log's shape (fixture A): a space after each comma, a
    trailing comma, a bare ms Time, a sparse counter of " " blanks, a
    padded text column with one quoted cell"""
    base = os.path.join(fixtures, "logger-c")
    log = os.path.join(base, "log.csv")
    want = committed(os.path.join(base, "guessed-map.json"))
    out, err = printed(cli, ["import", "csv", log, "--print-map"])
    if out != want or err:
        raise SystemExit(f"FAIL  logger-c's printed map: {out!r} {err}")
    rec = os.path.join(d, "logger-c")
    lines = run(cli, ["import", "csv", log, rec])
    guessed = one(lines, "guessed")
    same("logger-c's guessed map", json.loads(want), guessed["map"])
    for note in ["the time column 'Time' has no unit: read as ms; change it "
                 "if it isn't",
                 "'Flux counter' holds a value in 3 of 200 rows read: a "
                 "wide export by sweep holds its newest value in the rows "
                 "the other columns set"]:
        if note not in guessed["notes"]:
            raise SystemExit(f"FAIL  logger-c's notes lack {note!r}: "
                             f"{guessed['notes']}")
    imported = one(lines, "imported")
    if (imported["values"], imported["columns_ignored"],
            imported["duration_us"]) != (420 * 14 + 6, 1, 121521000):
        raise SystemExit(f"FAIL  logger-c imported {imported}")
    # The padded text arrives trimmed, the one quoted cell as written
    with open(os.path.join(rec, "signals.csv"), newline="",
              encoding="utf-8-sig") as f:
        modes = [r[2] for r in csv.reader(f) if r[1:2] == ["Mode"]]
    if (len(modes) != 420 or modes.count("OL ") != 1 or
            set(modes) != {"OL", "CL", "OL "}):
        raise SystemExit(f"FAIL  logger-c's modes {sorted(set(modes))}")
    # Its wide export keeps the sparse counter; far sparser than the rest,
    # it paces no sweep, so by sweep there is a row for each of the log's
    for rows, count in [(None, 420), ("300", 401)]:
        out_csv = os.path.join(d, f"logger-c-{rows or 'sweep'}.csv")
        args = ["export", rec, out_csv, "--format", "csv"]
        if rows:
            args += ["--rows", rows]
        exported = one(run(cli, args), "exported")
        head, cells = wide_columns(out_csv)
        flux = [c for c in cells.get("Flux counter", []) if c != ""]
        # a grid row repeats a cell's latest value: each value's run once
        runs = [v for k, v in enumerate(flux) if k == 0 or flux[k - 1] != v]
        if (exported["signals"] != 14 or "Flux counter" not in head or
                runs != ["1", "2", "3", "4", "5", "6"] or
                exported["rows"] != count):
            raise SystemExit(f"FAIL  logger-c exported {exported} {head} "
                             f"{flux}")
    # 10 s to 20 s holds one counter value: with no interval of its own it
    # is still sparse, so the range keeps a row for each of its 34 log rows
    out_csv = os.path.join(d, "logger-c-range.csv")
    exported = one(run(cli, ["export", rec, out_csv, "--format", "csv",
                             "--from", "10", "--to", "20"]), "exported")
    head, cells = wide_columns(out_csv)
    flux = [c for c in cells.get("Flux counter", []) if c != ""]
    if exported["rows"] != 34 or not flux or set(flux) != {"1"}:
        raise SystemExit(f"FAIL  logger-c's range exported {exported} {flux}")


def logger_d(cli, fixtures, d):
    """Rows with no time, one mid-file holding values and a last one of
    padding: the guess notes them, the import leaves them out, counts
    them in its line and says so on stderr; a file where no row has a
    time is refused both ways"""
    base = os.path.join(fixtures, "logger-d")
    log = os.path.join(base, "log.csv")
    want = committed(os.path.join(base, "guessed-map.json"))
    out, err = printed(cli, ["import", "csv", log, "--print-map"])
    if out != want or err:
        raise SystemExit(f"FAIL  logger-d's printed map: {out!r} {err}")
    rec = os.path.join(d, "logger-d")
    done = subprocess.run([cli, "import", "csv", log, rec],
                          capture_output=True, stdin=subprocess.DEVNULL)
    lines = [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()]
    said = done.stderr.decode("utf-8").replace("\r", "")
    if done.returncode != 0:
        raise SystemExit(f"FAIL  logger-d exited {done.returncode}: {said}")
    note = ("2 of the 10 rows read have no time (the first on line 5): the "
            "import leaves such a row out")
    guessed = one(lines, "guessed")
    if note not in guessed["notes"]:
        raise SystemExit(f"FAIL  logger-d's notes lack {note!r}: "
                         f"{guessed['notes']}")
    imported = one(lines, "imported")
    if (imported["rows_no_time"], imported["values"], imported["lines"],
            imported["duration_us"]) != (2, 24, 11, 1740000):
        raise SystemExit(f"FAIL  logger-d imported {imported}")
    if said != "havne-cli: 2 rows with no time left out\n":
        raise SystemExit(f"FAIL  logger-d said {said!r}")
    with open(os.path.join(rec, "events.log"), encoding="utf-8") as f:
        events = f.read()
    if ("import  rows left out: 2 with no time (the first on line 5)\n"
            not in events):
        raise SystemExit(f"FAIL  logger-d's events.log: {events}")
    # The untimed row's values are nowhere: 8 samples a signal
    viewed = run(cli, ["view", rec, "--columns", "4"])
    samples = {s["name"]: s["samples"] for s in viewed
               if s["type"] == "signal"}
    if samples != {"Engine speed": 8, "Widget pressure": 8,
                   "Gizmo state": 8}:
        raise SystemExit(f"FAIL  logger-d's signals {samples}")
    # One such row is said as one, through a map as through the guess
    single = os.path.join(d, "single.csv")
    with open(single, "w", newline="\n") as f:
        f.write("time (s),A\n0,1\n,2\n1,3\n")
    done = subprocess.run([cli, "import", "csv", single,
                           os.path.join(d, "single")],
                          capture_output=True, stdin=subprocess.DEVNULL)
    said = done.stderr.decode("utf-8").replace("\r", "")
    if done.returncode != 0 or said != ("havne-cli: 1 row with no time left "
                                        "out\n"):
        raise SystemExit(f"FAIL  one row with no time said {said!r}")
    # A file with none says nothing on stderr
    done = subprocess.run([cli, "import", "csv",
                           os.path.join(fixtures, "logger-b", "log.csv"),
                           os.path.join(d, "logger-b-quiet")],
                          capture_output=True, stdin=subprocess.DEVNULL)
    if done.returncode != 0 or done.stderr:
        raise SystemExit(f"FAIL  a file with every time said {done.stderr!r}")
    # No row with a time: the import's words from the guess too
    untimed = os.path.join(d, "untimed.csv")
    with open(untimed, "w", newline="\n") as f:
        f.write("time (s),A\n,1\n,2\n")
    for args in [[], ["--time-unit", "s"]]:
        error = one(run(cli, ["import", "csv", untimed,
                              os.path.join(d, "untimed")] + args, 3), "error")
        if error["detail"] != "no row below the header has a time":
            raise SystemExit(f"FAIL  no time said {error}")
    out, err = printed(cli, ["import", "csv", untimed, "--print-map"], 3)
    if out or err.replace("\r", "") != ("havne-cli: no row below the header "
                                        "has a time\n"):
        raise SystemExit(f"FAIL  no time printed {out!r} {err!r}")
    if os.path.exists(os.path.join(d, "untimed")):
        raise SystemExit("FAIL  a file with no time left a folder")


def csv_guess(cli, fixtures):
    with tempfile.TemporaryDirectory() as d:
        for name, values in GUESSED:
            base = os.path.join(fixtures, name)
            log = os.path.join(base, "log.csv")
            want = committed(os.path.join(base, "guessed-map.json"))
            # --print-map is core's slice byte for byte, which
            # CsvGuessTest.TheMadeUpLoggersAreGuessedAndImported pins too
            out, err = printed(cli, ["import", "csv", log, "--print-map"])
            if out != want or err:
                raise SystemExit(f"FAIL  {name}'s printed map: {out!r} {err}")
            # With no map: the guess on a line, then the import with it
            rec = os.path.join(d, name)
            lines = run(cli, ["import", "csv", log, rec])
            if [l["type"] for l in lines] != ["start", "guessed", "imported"]:
                raise SystemExit(f"FAIL  {name} wrote {lines}")
            guessed = one(lines, "guessed")
            same(f"{name}'s guessed map", json.loads(want), guessed["map"])
            if not all(c["used"] for c in guessed["columns"]):
                raise SystemExit(f"FAIL  {name} left a column out: {guessed}")
            imported = one(lines, "imported")
            if (imported["values"], imported["columns_ignored"]) != (values, 0):
                raise SystemExit(f"FAIL  {name} imported {imported}")
            if sorted(os.listdir(rec)) != ["events.log", "signals.csv"]:
                raise SystemExit(f"FAIL  {name} made {os.listdir(rec)}")
        # A time named so, of whole numbers with no unit: read as ms
        # with a note, printed so, and --time-unit still gives another
        plain = os.path.join(d, "plain.csv")
        with open(plain, "w", newline="\n") as f:
            f.write("time,A\n0,1\n1,2\n")
        rec = os.path.join(d, "plain")
        lines = run(cli, ["import", "csv", plain, rec])
        guessed = one(lines, "guessed")
        if (guessed["map"]["time"].get("unit") != "ms" or
                "the time column 'time' has no unit: read as ms; change it "
                "if it isn't" not in guessed["notes"]):
            raise SystemExit(f"FAIL  a bare time guessed {guessed}")
        imported = one(lines, "imported")
        if (imported["values"], imported["duration_us"]) != (2, 1000):
            raise SystemExit(f"FAIL  a bare time imported {imported}")
        out, err = printed(cli, ["import", "csv", plain, "--print-map"])
        if json.loads(out)["time"]["unit"] != "ms" or err:
            raise SystemExit(f"FAIL  a bare time printed {out!r} {err}")
        out, err = printed(cli, ["import", "csv", plain, "--print-map",
                                 "--time-unit", "s"])
        if json.loads(out)["time"]["unit"] != "s" or err:
            raise SystemExit(f"FAIL  --time-unit printed {out!r} {err}")
        imported = one(run(cli, ["import", "csv", plain,
                                 os.path.join(d, "plain-s"), "--time-unit",
                                 "s"]), "imported")
        if (imported["values"], imported["duration_us"]) != (2, 1000000):
            raise SystemExit(f"FAIL  --time-unit imported {imported}")
        # A time with fractions may be seconds: refused without
        # --time-unit, printed with a word on stderr, imported with one
        fraction = os.path.join(d, "fraction.csv")
        with open(fraction, "w", newline="\n") as f:
            f.write("time,A\n0.0,1\n0.5,2\n")
        rec = os.path.join(d, "fraction")
        lines = run(cli, ["import", "csv", fraction, rec], 3)
        if "unit" in one(lines, "guessed")["map"]["time"]:
            raise SystemExit(f"FAIL  a unit came from nowhere: {lines}")
        error = one(lines, "error")
        if "--time-unit" not in error["detail"] or os.path.exists(rec):
            raise SystemExit(f"FAIL  no unit said {error}")
        out, err = printed(cli, ["import", "csv", fraction, "--print-map"])
        if "unit" in json.loads(out)["time"] or "--time-unit" not in err:
            raise SystemExit(f"FAIL  no unit printed {out!r} {err}")
        imported = one(run(cli, ["import", "csv", fraction, rec,
                                 "--time-unit", "s"]), "imported")
        if (imported["values"], imported["duration_us"]) != (2, 500000):
            raise SystemExit(f"FAIL  --time-unit imported {imported}")
        logger_c(cli, fixtures, d)
        logger_d(cli, fixtures, d)
        # ISO 8601 times with no offset: no --time-unit gives them a unit,
        # so the refusal and stderr name a map, never --time-unit
        zoneless = os.path.join(d, "zoneless.csv")
        with open(zoneless, "w", newline="\n") as f:
            f.write("timestamp,A\n2026-11-05T22:03:22.250,1\n"
                    "2026-11-05T22:03:22.750,2\n")
        rec = os.path.join(d, "zoneless")
        error = one(run(cli, ["import", "csv", zoneless, rec, "--time-unit",
                              "ms"], 3), "error")
        if ("--time-unit" in error["detail"] or "--map" not in error["detail"]
                or os.path.exists(rec)):
            raise SystemExit(f"FAIL  a zoneless time said {error}")
        out, err = printed(cli, ["import", "csv", zoneless, "--print-map"])
        if "unit" in json.loads(out)["time"] or "--time-unit" in err or "zone" not in err:
            raise SystemExit(f"FAIL  a zoneless time printed {out!r} {err}")
        # A file with no header: core's words, exit 3, nothing printed
        numbers = os.path.join(d, "numbers.csv")
        with open(numbers, "w", newline="\n") as f:
            f.write("1,2\n3,4\n")
        error = one(run(cli, ["import", "csv", numbers,
                              os.path.join(d, "none")], 3), "error")
        if not error["detail"].startswith("no row within the file's first"):
            raise SystemExit(f"FAIL  no header said {error}")
        out, err = printed(cli, ["import", "csv", numbers, "--print-map"], 3)
        if out or not err.startswith("havne-cli: no row within"):
            raise SystemExit(f"FAIL  no header printed {out!r} {err}")
        if os.path.exists(os.path.join(d, "none")):
            raise SystemExit("FAIL  a refused guess left a folder")
    print(f"ok    {len(GUESSED) + 2} loggers' maps guessed, printed as "
          "committed and imported, logger-c's wide export with its sparse "
          "counter, logger-d's rows with no time left out and counted; "
          "a whole-number time read as ms, a fractional one refused; "
          "a file with no header "
          "refused")
    return 0




if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "csv-import":
        sys.exit(csv_import(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 4 and sys.argv[1] == "csv-guess":
        sys.exit(csv_guess(sys.argv[2], sys.argv[3]))
    print(__doc__ or "usage: cli_transfer_import.py csv-import|csv-guess <havne-cli> <argument>")
    sys.exit(2)
