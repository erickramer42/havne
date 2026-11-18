# File/layer: tests/cli_keys.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliKeysRunTest.*): havne-cli names a signal by
#          its key or by a name one signal has (ADR-0065 section 9)
#
#   python tests/cli_keys.py recording <havne-cli>
#   python tests/cli_keys.py log <havne-cli>
#   python tests/cli_keys.py file <havne-cli>
#
# recording: a format 4 recording written here, two signals named RPM on
# two modules and one under its author's key. view shows a signal named by
# its key or by a name one signal has, each line that names it giving its
# key; a name two signals have is usage (exit 2, nothing on stdout) in
# core's words with the recording's place. export takes a column by its
# key, counts only a text no signal has as missing, and refuses the name
# two signals have. log: a mock session logged by a signal's key and by
# another's name asks those two, its focus line and the recording's focus
# line give their keys, and the key and name of one signal are a pick
# named twice. file: the replay fixture logged through
# tests/fixtures/signals/extras.json takes a computed signal, and the
# later of two signals at one address, by their author's keys, and view
# --frames gives those keys for the recording's own signals.json and for
# a --signals file; a mock session logged through plugins/example-sim
# takes its computed signal by key, and view --frames --plugin gives it.

import json
import subprocess
import sys
import tempfile
from pathlib import Path

RPM1 = "7E8:410C:2.0:16"
RPM2 = "7E9:410C:2.0:16"
SEVERAL = (f"2 signals are named 'RPM': name one by its key ({RPM1}, {RPM2})")


def run(cli, args):
    done = subprocess.run([cli] + args, capture_output=True)
    rows = [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()]
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


def write_recording(folder):
    folder.mkdir()
    (folder / "events.log").write_bytes(
        b"ecuio session recording, format 4 (ADR-0049)\n"
        b"started  2026-11-05 12:00:00.000 (local time)\n"
        b"utc  2026-11-05T18:00:00.000Z\ncore  0.9.0\n"
        b"uri  mock://x\nprofile  test\nframe flags  complete\n"
        + f"signal  {RPM1},RPM,number,rpm,0,1,1\n".encode()
        + f"signal  {RPM2},RPM,number,rpm,0,2,1\n".encode()
        + b"signal  boost,Boost,number,kPa,0,3,2\n"
        b"columns  local time, seconds since the recording started\n\n"
        b"12:00:03.000  +3.000  end  the test ended: 0 frames, 6 values, "
        b"0 records dropped, 0 notes dropped\n")
    (folder / "signals.csv").write_bytes(
        b"\xef\xbb\xbfhost_us,signal,value,unit\n"
        + f"1000000,{RPM1},800,rpm\n1000000,{RPM2},900,rpm\n".encode()
        + b"1000000,boost,12,kPa\n"
        + f"2000000,{RPM1},810,rpm\n2000000,{RPM2},910,rpm\n".encode()
        + b"2000000,boost,13,kPa\n")


def recording(cli):
    bad = 0

    def fail(what):
        nonlocal bad
        print(f"FAIL  {what}")
        bad += 1

    with tempfile.TemporaryDirectory(prefix="havne-cli-keys-") as root:
        folder = Path(root) / "rec"
        write_recording(folder)
        rec = str(folder)

        # Every signal line gives its key; a pick by key shows that signal
        code, rows, err = run(cli, ["view", rec, "--signal", RPM2,
                                    "--signal", "Boost", "--columns", "2",
                                    "--span", "0:3"])
        signals = {(r["name"], r["key"]) for r in rows if r["type"] == "signal"}
        columns = [(r["signal"], r["key"], sum(r["samples"]))
                   for r in rows if r["type"] == "columns"]
        stats = [(r["signal"], r["key"], r["highest"])
                 for r in rows if r["type"] == "stats"]
        if code != 0 or signals != {("RPM", RPM1), ("RPM", RPM2),
                                    ("Boost", "boost")}:
            fail(f"view's signal lines: exit {code}, {signals}, {err!r}")
        if columns != [("RPM", RPM2, 2), ("Boost", "boost", 2)]:
            fail(f"view's columns by key and by name: {columns}")
        if stats != [("RPM", RPM2, 910), ("Boost", "boost", 13)]:
            fail(f"view's stats by key and by name: {stats}")

        # An author's key and the name are one signal
        by_key = run(cli, ["view", rec, "--signal", "boost", "--columns", "2"])
        by_name = run(cli, ["view", rec, "--signal", "Boost", "--columns", "2"])
        if by_key[0] != 0 or by_key[1] != by_name[1]:
            fail(f"view by an author's key: {by_key}")

        # A name two signals have: usage, in core's words
        for args in (["--signal", "RPM"],
                     ["--signal", RPM1, "--align-on", "rise:RPM:805"],
                     ["--stretches", "pulls", "--role", "rpm=RPM"]):
            code, rows, err = run(cli, ["view", rec] + args)
            if code != 2 or rows or f"recording 0: {SEVERAL}" not in err:
                fail(f"view {' '.join(args)}: exit {code}, {rows}, {err!r}")
        code, rows, err = run(cli, ["view", rec, "--signal", RPM1,
                                    "--align-on", f"rise:{RPM1}:805"])
        aligned = [r for r in rows if r["type"] == "align"]
        if code != 0 or len(aligned) != 1 or not aligned[0].get("found"):
            fail(f"view aligned on a signal by its key: exit {code}, {err!r}, "
                 f"{[r['type'] for r in rows]}")
        code, rows, err = run(cli, ["view", rec, "--signal", "Nothing"])
        if code != 2 or rows or "no recording has a signal 'Nothing'" not in err:
            fail(f"view of a text no signal has: exit {code}, {err!r}")

        # export: a column by its key; only a text no signal has is missing
        out = str(Path(root) / "by-key.csv")
        code, rows, err = run(cli, ["export", rec, out, "--format", "csv",
                                    "--signal", RPM2, "--signal", "Boost",
                                    "--signal", "Nothing"])
        done = [r for r in rows if r["type"] == "exported"]
        head = Path(out).read_text("utf-8").splitlines()[0] if done else ""
        if code != 0 or len(done) != 1 or done[0]["missing"] != 1 or \
                head != "Time (ms),RPM (rpm),Boost (kPa),Nothing" or \
                "'Nothing'" not in err:
            fail(f"export by key: exit {code}, {done}, {head!r}, {err!r}")
        refused = str(Path(root) / "refused.csv")
        code, rows, err = run(cli, ["export", rec, refused, "--format", "csv",
                                    "--signal", "RPM"])
        errors = [r for r in rows if r["type"] == "error"]
        if code == 0 or len(errors) != 1 or SEVERAL not in json.dumps(errors) \
                or Path(refused).exists():
            fail(f"export of a name two signals have: exit {code}, {rows}")
    return 1 if bad else 0


def header_lines(folder, word):
    lines = (folder / "events.log").read_text("utf-8").splitlines()
    return [l for l in lines if l.startswith(word + "  ")]


def log(cli):
    bad = 0

    def fail(what):
        nonlocal bad
        print(f"FAIL  {what}")
        bad += 1

    rpm, speed = "0C0::0.0:16", "0C0::3.0:8"
    with tempfile.TemporaryDirectory(prefix="havne-cli-keys-") as root:
        code, rows, err = run(cli, ["log", "mock://demo", root, "--seconds", "1",
                                    "--signal", rpm, "--signal",
                                    "Vehicle Speed"])
        focus = [r for r in rows if r["type"] == "focus"]
        picks = [(s["name"], s["key"]) for s in focus[0]["signals"]] \
            if focus else []
        folders = [p for p in Path(root).iterdir() if p.is_dir()]
        if code != 0 or picks != [("RPM", rpm), ("Vehicle Speed", speed)]:
            fail(f"log by a key and a name: exit {code}, {picks}, {err!r}")
        elif len(folders) != 1 or \
                header_lines(folders[0], "focus") != [f"focus  {rpm},{speed}"]:
            fail(f"the recording's focus line: "
                 f"{[header_lines(f, 'focus') for f in folders]}")

    with tempfile.TemporaryDirectory(prefix="havne-cli-keys-") as root:
        for names, words in (
                (["RPM", rpm], f"--signal names '{rpm}' twice"),
                (["0C0::0.0:17"], "no signal with the key or name '0C0::0.0:17'")):
            args = [a for n in names for a in ("--signal", n)]
            code, rows, err = run(cli, ["log", "mock://demo", root,
                                        "--seconds", "1"] + args)
            if code != 2 or rows or words not in err or \
                    any(Path(root).iterdir()):
                fail(f"log {' '.join(names)}: exit {code}, {rows}, {err!r}")
    return 1 if bad else 0


def file_keys(rows):
    """The signal lines' keys for the two signals the file keys itself"""
    return [r["key"] for r in rows if r["type"] == "signal"
            and r["name"] in ("Road Speed", "RPM in Thousands")]


def file(cli):
    bad = 0

    def fail(what):
        nonlocal bad
        print(f"FAIL  {what}")
        bad += 1

    here = Path(__file__).resolve().parent
    replay = (here / "fixtures" / "replay" / "2026-10-25-engine-start").as_posix()
    extras = str(here / "fixtures" / "signals" / "extras.json")
    with tempfile.TemporaryDirectory(prefix="havne-cli-keys-") as root:
        # A computed signal, and the later of two at one address, are
        # picked by their author's keys, which the focus line and the
        # recording's focus line give
        code, rows, err = run(cli, ["log", f"replay://{replay}?speed=max", root,
                                    "--signals", extras, "--signal",
                                    "road_speed", "--signal", "krpm",
                                    "--signal", "RPM"])
        focus = [r for r in rows if r["type"] == "focus"]
        picks = [(s["name"], s["key"]) for s in focus[0]["signals"]] \
            if focus else []
        folders = [p for p in Path(root).iterdir() if p.is_dir()]
        if code != 0 or len(folders) != 1 or \
                picks != [("Road Speed", "road_speed"),
                          ("RPM in Thousands", "krpm"), ("RPM", RPM1)]:
            fail(f"log through a signal file: exit {code}, {picks}, {err!r}")
            return 1
        if header_lines(folders[0], "focus") != [f"focus  road_speed,krpm,{RPM1}"]:
            fail(f"its focus line: {header_lines(folders[0], 'focus')}")

        # Through its frames a recording's signals have the file's keys:
        # its own signals.json, and a --signals file over a replay
        code, rows, err = run(cli, ["view", str(folders[0]), "--frames",
                                    "--signal", "road_speed", "--signal",
                                    "krpm", "--columns", "2"])
        if code != 0 or file_keys(rows) != ["road_speed", "krpm"]:
            fail(f"view --frames of the recording: exit {code}, "
                 f"{file_keys(rows)}, {err!r}")
        code, rows, err = run(cli, ["view", replay, "--frames", "--signals",
                                    extras, "--signal", "road_speed",
                                    "--signal", "krpm", "--columns", "2"])
        if code != 0 or file_keys(rows) != ["road_speed", "krpm"]:
            fail(f"view --frames --signals: exit {code}, "
                 f"{file_keys(rows)}, {err!r}")

        # A --plugin variant's file gives its keys too: a mock session
        # logged through the example plugin, then viewed through the
        # plugin with the recording's own signals.json taken away
        plugin = str(here.parent / "plugins" / "example-sim")
        base = "plugin.example-sim.base"
        made = Path(root) / "plugin"
        made.mkdir()
        code, rows, err = run(cli, ["log", f"mock://x?profile={base}",
                                    str(made), "--plugin", plugin,
                                    "--seconds", "1", "--signal", "boost"])
        focus = [r for r in rows if r["type"] == "focus"]
        picks = [(s["name"], s["key"]) for s in focus[0]["signals"]] \
            if focus else []
        folders = [p for p in made.iterdir() if p.is_dir()]
        if code != 0 or len(folders) != 1 or picks != [("Boost", "boost")]:
            fail(f"log through a plugin: exit {code}, {picks}, {err!r}")
            return 1
        (folders[0] / "signals.json").unlink()
        code, rows, err = run(cli, ["view", str(folders[0]), "--frames",
                                    "--plugin", plugin, "--profile", base,
                                    "--signal", "boost", "--columns", "2"])
        keys = [r["key"] for r in rows
                if r["type"] == "signal" and r["name"] == "Boost"]
        if code != 0 or keys != ["boost"]:
            fail(f"view --frames --plugin: exit {code}, {keys}, {err!r}")
    return 1 if bad else 0


def main(argv):
    modes = {"recording": recording, "log": log, "file": file}
    if len(argv) != 3 or argv[1] not in modes:
        print("usage: cli_keys.py recording|log|file <havne-cli>",
              file=sys.stderr)
        return 2
    return modes[argv[1]](argv[2])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
