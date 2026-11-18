# File/layer: tests/cli_log.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliLogRunTest.*): havne-cli log (ADR-0056
#          section 7) from outside the process
#
#   python tests/cli_log.py demo <havne-cli>
#   python tests/cli_log.py fails <havne-cli>
#   python tests/cli_log.py replay <havne-cli> <fixture folder>
#   python tests/cli_log.py armed <havne-cli> <fixture folder>
#   python tests/cli_log.py interrupt <havne-cli>
#   python tests/cli_log.py closed-pipe <havne-cli>
#   python tests/cli_log.py focus-refused <havne-cli> <fake_j2534.dll>   (Windows)
#   python tests/cli_log.py pace <havne-cli>
#   python tests/cli_log.py pick <havne-cli>
#   python tests/cli_log.py pick-only <havne-cli>
#   python tests/cli_log.py session <havne-cli> <fake_j2534.dll> <golden> [--update]   (Windows)
#
# demo: mock://demo logged for 2 s with two signals picked, the log line's
# period shortened through HAVNE_CLI_LOG_EVERY_MS (set here): the lines
# are start, session, focus (the two picks in order), recording started,
# log lines (seconds climbing, bytes and values growing), recording closed
# whole and end (reason seconds, the folder); events.log carries the picks on its
# focus line; an export of the folder with no --signal picks exactly
# those two columns, in order, through that line; --layout long still
# writes every number signal.
# fails: a folder that can't be made ends the log before any frame, with
# the reason (never the path) and exit 1. replay: a replay at max ends at
# its last frame, reason replay-end, the recording whole. armed: --armed
# over the replay at max with a condition on engine speed: an armed line,
# window lines whose count climbs to two with each folder there, the
# closed and end lines saying two windows, the end naming the newest
# folder, each window ended whole with its trigger line and its frames
# from the firing's before; a file with no condition that records is
# refused at the start, exit 1, no folder made. interrupt:
# Ctrl+Break (Windows, exit 77 without a console) or SIGINT ends the
# log with reason interrupted, the recording closed whole and read back.
# closed-pipe: standard output closed under a running log, and under a
# stream --record, is a quiet exit 0 with the recording closed whole,
# its end line's why output-failed (the crash this pins: a null reason
# on that path). focus-refused: a focus the session can't ask (a
# ?poll= session, a pick that isn't the polled PID) is an error line
# after the session line, exit 3, no recording made. pace: --min-sweep over
# a simulated ELM327 on TCP: the same two signals logged for 3 s with no
# minimum and with 250 ms, the focus line and the header saying so only
# with one, and the wide export's sweep rows at most the time allows
# (a ceiling alone: load takes rows away from both runs, so it can't fail
# it), with at least one. pick, over the emulator's ELM327: --pick ranked
# under obd2 joins a --signal to the ranked ones in the set's order, its
# focus line saying pick ranked and events.log naming them on a pick
# line, never a focus line; under uds-generic (which ranks none) the
# names alone are a log's focus with its minimum, and with no names the
# whole set has none; on the mock (which asks nothing) no pick; stream
# --pick ranked writes its focus line after the session line, and a pick
# core refuses (?poll=0x2F) leaves the whole set asked, exit 0.

import csv
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside the tests
import golden  # noqa: E402

PICKS = ["Vehicle Speed", "RPM"]
# The keys a recording keeps PICKS under: the mock's 0x0C0 bytes 3 and 0-1
PICK_KEYS = ["0C0::3.0:8", "0C0::0.0:16"]
HEAD = "Time (ms),Vehicle Speed (km/h),RPM (RPM)"


def run(cli, args, env=None):
    done = subprocess.run([cli] + args, capture_output=True, env=env)
    rows = golden.parse(done.stdout.decode("utf-8"), "stdout")
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


class Emulator:
    """havne-cli sim's ELM327 on an OS-picked port, as a child"""

    def __init__(self, cli):
        self.p = subprocess.Popen(
            [cli, "sim", "mock://sim?values=canned", "--elm327", "tcp:0",
             "--seconds", "90"],
            stdout=subprocess.PIPE, text=True)
        self.port = None
        for line in self.p.stdout:
            j = json.loads(line)
            if j["type"] == "elm327":
                self.port = int(j["listen"].rsplit(":", 1)[1])
                return
        raise SystemExit("FAIL  the emulator wrote no elm327 line")

    def end(self):
        self.p.terminate()
        self.p.communicate(timeout=60)


def paced_rows(cli, port, root, floor):
    """A log of PACE_SIGNALS for 3 s: its focus line, its events.log and
    the number of the wide export's sweep rows"""
    uri = (f"elm327://127.0.0.1:{port}"
           "?profile=obd2&identify=0&diagnose=0&probe=0")
    args = ["log", uri, str(root), "--seconds", "3"]
    for name in PICKS:
        args += ["--signal", name]
    if floor:
        args += ["--min-sweep", str(floor)]
    code, rows, err = run(cli, args)
    if code != 0:
        raise SystemExit(f"FAIL  log exit {code}: {err} {rows}")
    focus = [r for r in rows if r["type"] == "focus"][0]
    folder = the_folder(root)
    events = (folder / "events.log").read_text("utf-8")
    out = Path(root) / "wide.csv"
    code, rows, err = run(cli, ["export", str(folder), str(out), "--format",
                                "csv", "--rows", "sweep"])
    if code != 0:
        raise SystemExit(f"FAIL  export exit {code}: {err} {rows}")
    with open(out, newline="", encoding="utf-8-sig") as f:
        table = [row for row in csv.reader(f) if row and not row[0].startswith("#")]
    return focus, events, len(table) - 1


def pace(cli):
    emulator = Emulator(cli)
    try:
        with tempfile.TemporaryDirectory() as free_root:
            focus, events, free = paced_rows(cli, emulator.port, free_root, 0)
            if "min_sweep_ms" in focus:
                raise SystemExit(f"FAIL  a focus with none says one: {focus}")
            if "min sweep" in events:
                raise SystemExit("FAIL  a log with none has a min sweep line")
        with tempfile.TemporaryDirectory() as root:
            focus, events, paced = paced_rows(cli, emulator.port, root, 250)
    finally:
        emulator.end()
    if focus.get("min_sweep_ms") != 250:
        raise SystemExit(f"FAIL  the focus line: {focus}")
    if "\nmin sweep  250 ms\n" not in events.replace("\r\n", "\n"):
        raise SystemExit("FAIL  the header has no min sweep line")
    # 3 s of log at most, 4 sweeps a second; the first row's sweep and the
    # log's own edges are worth a row or two, which the bound leaves. Only
    # the ceiling judges the floor: load takes rows from both runs alike
    if not 1 <= paced <= 15:
        raise SystemExit(f"FAIL  {paced} rows at 250 ms in 3 s")
    if free <= 15:
        print(f"note  the free run's {free} rows are too few to show the floor")
    print(f"ok    {free} rows free, {paced} at 250 ms")
    return 0


def the_folder(root):
    found = [p for p in Path(root).iterdir() if p.is_dir()]
    if len(found) != 1:
        raise SystemExit(f"FAIL  {len(found)} folders in the recordings' root")
    return found[0]


def demo(cli):
    env = dict(os.environ)
    env["HAVNE_CLI_LOG_EVERY_MS"] = "500"
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-") as root:
        args = ["log", "mock://demo", root, "--seconds", "2", "--label", "log"]
        for name in PICKS:
            args += ["--signal", name]
        code, rows, err = run(cli, args, env)
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        types = [r.get("type") for r in rows]
        head, tail = types[:4], types[4:]
        if head != ["start", "session", "focus", "recording"]:
            print(f"FAIL  the first lines {head!r}")
            return 1
        if len(tail) < 3 or tail[-2:] != ["recording", "end"] or \
                set(tail[:-2]) != {"log"}:
            print(f"FAIL  the lines after the recording's {tail!r}")
            return 1
        closed = rows[-2]
        if closed.get("state") != "closed" or closed.get("whole") is not True:
            print(f"FAIL  the closing line {json.dumps(closed)}")
            return 1
        if any(t in ("frame", "values") for t in types):
            print("FAIL  a log writes no frame or values lines")
            return 1
        focus = [s["name"] for s in rows[2]["signals"]]
        if focus != PICKS:
            print(f"FAIL  the focus line names {focus!r}, want {PICKS!r}")
            return 1
        if not all(isinstance(s.get("id"), int) for s in rows[2]["signals"]):
            print("FAIL  a focus line's signal has no id")
            return 1
        folder = the_folder(root)
        if rows[3].get("state") != "started" or \
                Path(rows[3].get("folder", "")).name != folder.name:
            print(f"FAIL  the recording line {json.dumps(rows[3])}")
            return 1
        logs = [r for r in rows if r.get("type") == "log"]
        if len(logs) < 2:
            print(f"FAIL  {len(logs)} log line(s) in 2 s at 500 ms")
            return 1
        seconds = [r["seconds"] for r in logs]
        if seconds != sorted(seconds) or seconds[-1] < 1:
            print(f"FAIL  the log lines' seconds {seconds!r}")
            return 1
        if logs[-1]["bytes"] <= 0 or logs[-1]["values"] <= 0:
            print(f"FAIL  the last log line {json.dumps(logs[-1])}")
            return 1
        if logs[-1]["bytes"] < logs[0]["bytes"] or \
                logs[-1]["values"] < logs[0]["values"]:
            print("FAIL  the recording shrank between log lines")
            return 1
        end = rows[-1]
        if end.get("reason") != "seconds" or \
                Path(end.get("folder", "")).name != folder.name:
            print(f"FAIL  the end line {json.dumps(end)}")
            return 1
        if end["values"] < logs[-1]["values"] or end["seconds"] < 2:
            print(f"FAIL  the end line's readout {json.dumps(end)}")
            return 1
        events = (folder / "events.log").read_bytes().decode("utf-8")
        want = "focus  " + ",".join(PICK_KEYS)
        if want not in events.splitlines():
            print(f"FAIL  events.log has no line {want!r}")
            return 1
        # The recording's note of the end is the stdout line without its
        # folder, and no path of the machine is in the log
        kept = {k: v for k, v in end.items() if k != "folder"}
        if f"  note  {json.dumps(kept, separators=(',', ':'))}" not in events:
            print("FAIL  events.log has no note of the end line")
            return 1
        for path in {str(root), Path(root).as_posix(), str(folder),
                     folder.as_posix(), json.dumps(str(root))[1:-1]}:
            if path in events:
                print(f"FAIL  events.log holds the path {path!r}")
                return 1

        out = Path(root) / "wide.csv"
        code, erows, err = run(cli, ["export", str(folder), str(out),
                                     "--format", "csv"])
        if code != 0:
            print(f"FAIL  export exit {code}\n{err}")
            return 1
        text = out.read_bytes().decode("utf-8")
        lines = text.split("\r\n")
        if lines[0] != HEAD:
            print(f"FAIL  the wide header {lines[0]!r}, want {HEAD!r}")
            return 1
        body = [l for l in lines[1:] if l]
        if len(body) < 10:
            print(f"FAIL  {len(body)} wide rows from 2 s of the mock")
            return 1
        for line in body:
            cells = next(csv.reader([line]))
            if len(cells) != 3 or not cells[0].isdigit():
                print(f"FAIL  a wide row {line!r}")
                return 1
        exported = [r for r in erows if r.get("type") == "exported"]
        if not exported or exported[0].get("signals") != 2:
            print(f"FAIL  the exported line {exported!r}")
            return 1

        long_out = Path(root) / "long.csv"
        code, erows, err = run(cli, ["export", str(folder), str(long_out),
                                     "--format", "csv", "--layout", "long"])
        if code != 0:
            print(f"FAIL  long export exit {code}\n{err}")
            return 1
        exported = [r for r in erows if r.get("type") == "exported"]
        if not exported or exported[0].get("signals", 0) <= 2:
            print(f"FAIL  the long form's exported line {exported!r}")
            return 1
    print("ok    the log records the picks and the export takes its columns")
    return 0


def lines_of(stdout):
    return golden.parse(stdout.decode("utf-8"), "stdout")


def whole(folder, why):
    """events.log's last line is the recorder's end line with the close's
    why: the files ended whole"""
    text = (folder / "events.log").read_bytes().decode("utf-8")
    last = [l for l in text.splitlines() if l.strip()][-1]
    if "end" not in last or why not in last:
        print(f"FAIL  events.log's last line {last!r}, want an end with {why!r}")
        return False
    return True


def fails(cli):
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-fails-") as base:
        blocker = Path(base) / "a file"
        blocker.write_text("not a folder")
        code, rows, err = run(cli, ["log", "mock://fails", str(blocker / "under"),
                                    "--seconds", "5"])
        if code != 1:
            print(f"FAIL  exit {code}, want 1\n{err}")
            return 1
        types = [r.get("type") for r in rows]
        if types != ["start", "session", "focus", "recording"]:
            print(f"FAIL  lines {types!r}")
            return 1
        line = rows[3]
        if line.get("state") != "failed" or not line.get("reason"):
            print(f"FAIL  {json.dumps(line)}")
            return 1
        if str(blocker) in line["reason"] or base in line["reason"]:
            print(f"FAIL  the reason names the path: {line['reason']!r}")
            return 1
    print("ok    a folder that can't be made ends the log with its reason")
    return 0


def replay(cli, fixture):
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-replay-") as root:
        uri = f"replay://{Path(fixture).as_posix()}?profile=obd2&speed=max"
        code, rows, err = run(cli, ["log", uri, root, "--seconds", "20"])
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        end = rows[-1]
        if end.get("type") != "end" or end.get("reason") != "replay-end":
            print(f"FAIL  the end line {json.dumps(end)}")
            return 1
        if end.get("values", 0) <= 0:
            print(f"FAIL  no values logged: {json.dumps(end)}")
            return 1
        if not whole(the_folder(root), "havne-cli: replay-end"):
            return 1
    print("ok    a replay ends the log at its last frame")
    return 0


ARMED = ('{"havne_conditions":1,"conditions":[{"key":"high","name":"High",'
         '"when":{"signal":["obd2.rpm"],"unit":"RPM","above":1000},'
         '"record":{"before":2,"after":1}}]}')
MARKED = ('{"havne_conditions":1,"conditions":[{"key":"high","name":"High",'
          '"when":{"signal":["obd2.rpm"],"unit":"RPM","above":1000},"mark":true}]}')


def armed(cli, fixture):
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-armed-") as base:
        uri = f"replay://{Path(fixture).as_posix()}?profile=obd2&speed=max"
        sets = Path(base) / "armed.json"
        sets.write_text(ARMED, encoding="utf-8")
        root = Path(base) / "windows"
        code, rows, err = run(cli, ["log", uri, str(root), "--conditions", str(sets),
                                    "--armed", "--seconds", "20"])
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        recording = [r for r in rows if r.get("type") == "recording"]
        if recording[0] != {"type": "recording", "state": "armed", "windows": 0}:
            print(f"FAIL  the first recording line {json.dumps(recording[0])}")
            return 1
        windows = [r for r in recording if r.get("state") == "window"]
        counts = [r["window"] for r in windows]
        if not windows or counts != sorted(set(counts)) or counts[-1] != 2:
            print(f"FAIL  the window lines {counts!r}")
            return 1
        for w in windows:
            if not Path(w["folder"]).is_dir():
                print(f"FAIL  a window line's folder isn't there: {w['folder']!r}")
                return 1
        closed = recording[-1]
        if closed.get("state") != "closed" or not closed.get("whole") or \
                closed.get("windows") != 2:
            print(f"FAIL  the closed line {json.dumps(closed)}")
            return 1
        end = rows[-1]
        folders = sorted(p for p in root.iterdir())
        if end.get("windows") != 2 or len(folders) != 2 or \
                Path(end.get("folder", "")) != folders[-1]:
            print(f"FAIL  the end line {json.dumps(end)}, folders {folders!r}")
            return 1
        for folder in folders:
            if not whole(folder, "the window ended"):
                return 1
            text = (folder / "events.log").read_text(encoding="utf-8")
            if "\ntrigger  high,2000 ms\n" not in text:
                print(f"FAIL  {folder.name} has no trigger line")
                return 1
            fired = [l for l in text.splitlines() if "  cond  high,on,fired," in l]
            t_fired = int(fired[0].split(",")[-1])
            with open(folder / "frames.csv", encoding="utf-8") as f:
                first = int(f.read().splitlines()[1].split(",")[1])
            if not t_fired - 2000000 <= first < t_fired:
                print(f"FAIL  {folder.name}'s first frame {first}, fired {t_fired}")
                return 1

        sets.write_text(MARKED, encoding="utf-8")
        refused = Path(base) / "refused"
        code, rows, err = run(cli, ["log", uri, str(refused), "--conditions", str(sets),
                                    "--armed"])
        failed = [r for r in rows if r.get("type") == "recording"]
        line = failed[-1] if failed else {}
        if code != 1 or line.get("state") != "failed" or \
                not line.get("reason", "").startswith("no condition records"):
            print(f"FAIL  a set that records nothing: exit {code}, {json.dumps(line)}")
            return 1
        if refused.exists() and any(refused.iterdir()):
            print("FAIL  a refused armed log made a folder")
            return 1
    print("ok    an armed log writes a folder per window and names each")
    return 0


def interrupt(cli):
    env = dict(os.environ)
    env["HAVNE_CLI_LOG_EVERY_MS"] = "500"
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-int-") as root:
        args = [cli, "log", "mock://interrupt", root, "--seconds", "30"]
        if os.name == "nt":
            p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=env,
                                 creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        else:
            p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=env)
        time.sleep(1.5)
        try:
            if os.name == "nt":
                p.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                p.send_signal(signal.SIGINT)
        except OSError as e:
            p.kill()
            p.communicate()
            print(f"skip  no console to send the interrupt through: {e}")
            return 77
        try:
            out, err = p.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            print("FAIL  still running 15 s after the interrupt")
            return 1
        rows = lines_of(out)
        if p.returncode != 0 or not rows or rows[-1].get("type") != "end" or \
                rows[-1].get("reason") != "interrupted":
            print(f"FAIL  exit {p.returncode}, last line {rows[-1:] !r}\n"
                  f"{err.decode('utf-8', 'replace')}")
            return 1
        closed = rows[-2]
        if closed.get("type") != "recording" or closed.get("whole") is not True:
            print(f"FAIL  the closing line {json.dumps(closed)}")
            return 1
        folder = the_folder(root)
        if not whole(folder, "havne-cli: interrupted"):
            return 1
        # It reads back: the export opens it through the reader
        out_csv = Path(root) / "back.csv"
        code, erows, err = run(cli, ["export", str(folder), str(out_csv),
                                     "--format", "csv", "--layout", "long"])
        exported = [r for r in erows if r.get("type") == "exported"]
        if code != 0 or not exported or exported[0].get("values", 0) <= 0:
            print(f"FAIL  the recording doesn't read back: exit {code}\n{err}")
            return 1
    print("ok    an interrupt ends the log whole and it reads back")
    return 0


def closed_pipe(cli):
    env = dict(os.environ)
    env["HAVNE_CLI_LOG_EVERY_MS"] = "200"
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-pipe-") as base:
        for name, args in (("log", ["log", "mock://pipe", str(Path(base) / "log"),
                                    "--seconds", "20"]),
                           ("stream --record", ["stream", "mock://pipe", "--record",
                                                str(Path(base) / "stream"),
                                                "--seconds", "20"])):
            p = subprocess.Popen([cli] + args, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                 env=env)
            first = p.stdout.readline()
            second = p.stdout.readline()
            if not first.startswith(b'{"type":"start"') or \
                    not second.startswith(b'{"type":"session"'):
                p.kill()
                p.communicate()
                print(f"FAIL  {name}: the first lines {first!r} {second!r}")
                return 1
            p.stdout.close()
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
                p.communicate()
                print(f"FAIL  {name}: still running 15 s after its reader left")
                return 1
            err = p.stderr.read().decode("utf-8", "replace")
            if p.returncode != 0:
                print(f"FAIL  {name}: exit {p.returncode} with the pipe closed\n{err}")
                return 1
            folder = the_folder(Path(base) / ("log" if name == "log" else "stream"))
            if not whole(folder, "havne-cli: output-failed"):
                return 1
    print("ok    a closed output ends log and stream --record quietly, the recording whole")
    return 0


def focus_refused(cli, dll):
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-focus-") as root:
        uri = f"j2534://{dll}?profile=obd2&poll=12"
        code, rows, err = run(cli, ["log", uri, root, "--signal", "Vehicle Speed",
                                    "--seconds", "5"])
        types = [r.get("type") for r in rows]
        if code != 3 or types != ["start", "session", "error"]:
            print(f"FAIL  exit {code}, lines {types!r}\n{err}")
            return 1
        if "focus" not in json.dumps(rows[2]):
            print(f"FAIL  the error line {json.dumps(rows[2])}")
            return 1
        if any(Path(root).iterdir()):
            print("FAIL  a recording was made")
            return 1
    print("ok    a focus the session can't ask is an error line, exit 3")
    return 0


def session_line(cli, dll, golden_file, update):
    """A uds-generic log with ?session=03 writes stream's session line: the
    last one is the engine ECU's entered session, as the golden holds it"""
    with tempfile.TemporaryDirectory(prefix="havne-cli-log-session-") as root:
        uri = (f"j2534://{dll}?profile=uds-generic&did=1E1C,D200&session=03"
               "&identify=0&diagnose=0")
        code, rows, err = run(cli, ["log", uri, root, "--signal", "DID 1E1C",
                                    "--seconds", "3"])
    if code != 0:
        print(f"FAIL  exit {code}: {err}")
        return 1
    # A module's session line is the one carrying ecu: the opening session
    # line (backend, profile, channels) shares its type
    types = [r.get("type") for r in rows]
    opening = [r for r in rows if r.get("type") == "session" and "ecu" not in r]
    if len(opening) != 1 or "backend" not in opening[0] or \
            types.index("session") != rows.index(opening[0]):
        print(f"FAIL  the opening session line: {opening!r}")
        return 1
    sessions = [i for i, r in enumerate(rows)
                if r.get("type") == "session" and "ecu" in r]
    if not sessions:
        print("FAIL  no module's session line")
        return 1
    last = rows[sessions[-1]]
    if sessions[0] < rows.index(opening[0]) or rows[-1].get("type") != "end":
        print(f"FAIL  the order of the lines {types!r}")
        return 1
    # The session line is the log's own: written before the end line, never
    # after it
    if types.index("end") < sessions[-1]:
        print("FAIL  a session line after the end")
        return 1
    if update:
        Path(golden_file).write_text(golden.golden_text([last], set()),
                                     encoding="utf-8", newline="\n")
        print(f"ok    wrote {golden_file}")
        return 0
    expected = golden.parse(Path(golden_file).read_text("utf-8"), golden_file)
    return golden.report(golden.compare([last], expected, set()), golden_file)


# obd2's ranked signals (kPlotOrder) in the set's order, Fuel Level
# (unranked) among them where the set holds it
OBD2_PICK = ["Engine Load", "Coolant Temp", "RPM", "Vehicle Speed",
             "Intake Air Temp", "Fuel Level", "Battery Voltage", "MAF",
             "Engine Oil Temp"]
# Their keys, which a recording's pick line lists: each PID's reply from
# 0x7E8, its value from byte 2
OBD2_PICK_KEYS = ["7E8:4104:2.0:8", "7E8:4105:2.0:8", "7E8:410C:2.0:16",
                  "7E8:410D:2.0:8", "7E8:410F:2.0:8", "7E8:412F:2.0:8",
                  "7E8:4142:2.0:16", "7E8:4110:2.0:16", "7E8:415C:2.0:8"]


def header_lines(folder, word):
    text = (folder / "events.log").read_bytes().decode("utf-8")
    return [l for l in text.splitlines() if l.startswith(word + "  ")]


def pick_run(cli, args):
    """A run of args; its exit, rows, standard error and the one folder made"""
    with tempfile.TemporaryDirectory(prefix="havne-cli-pick-") as root:
        code, rows, err = run(cli, [a.replace("<root>", root) for a in args])
        folders = [p for p in Path(root).iterdir() if p.is_dir()]
        header = {}
        if len(folders) == 1:
            header = {w: header_lines(folders[0], w) for w in ("pick", "focus")}
        return code, rows, err, header


def focus_of(rows):
    found = [r for r in rows if r.get("type") == "focus"]
    return found[0] if found else None


def pick(cli):
    emulator = Emulator(cli)
    try:
        return pick_cases(cli, f"elm327://127.0.0.1:{emulator.port}")
    finally:
        emulator.end()


def pick_cases(cli, elm):
    obd2 = elm + "?profile=obd2&identify=0&diagnose=0&probe=0"
    # A session that asks: the ranked signals and the names, a pick
    code, rows, err, header = pick_run(cli, [
        "log", obd2, "<root>", "--seconds", "1", "--pick", "ranked",
        "--signal", "Fuel Level"])
    focus = focus_of(rows)
    names = [s["name"] for s in (focus or {}).get("signals", [])]
    if code != 0 or focus is None or focus.get("pick") != "ranked" or \
            len(names) != 15 or names[:len(OBD2_PICK)] != OBD2_PICK:
        print(f"FAIL  the pick's focus line {focus}, exit {code}\n{err}")
        return 1
    if len(header["pick"]) != 1 or header["focus"] or \
            not header["pick"][0].startswith("pick  " + ",".join(OBD2_PICK_KEYS)):
        print(f"FAIL  the header's pick {header!r}")
        return 1

    # A profile that ranks none: the names alone, a log's focus with its
    # minimum; with no names the whole set, which takes no minimum
    uds = elm + "?profile=uds-generic&did=1E1C:u16,F190:text&identify=0&diagnose=0"
    code, rows, err, header = pick_run(cli, [
        "log", uds, "<root>", "--seconds", "1", "--pick", "ranked",
        "--signal", "DID 1E1C", "--min-sweep", "250"])
    focus = focus_of(rows)
    if code != 0 or "ranks no signal" not in err or focus is None or \
            [s["name"] for s in focus["signals"]] != ["DID 1E1C"] or \
            "pick" in focus or focus.get("min_sweep_ms") != 250:
        print(f"FAIL  the unranked names' focus {focus}, exit {code}, {err!r}")
        return 1
    if header["pick"] or len(header["focus"]) != 1:
        print(f"FAIL  the unranked names' header {header!r}")
        return 1
    code, rows, err, header = pick_run(cli, [
        "log", uds, "<root>", "--seconds", "1", "--pick", "ranked",
        "--min-sweep", "250"])
    focus = focus_of(rows)
    if code != 0 or "ranks no signal" not in err or focus is None or \
            focus["signals"] != [] or "pick" in focus or "min_sweep_ms" in focus:
        print(f"FAIL  the unranked whole set's focus {focus}, exit {code}, {err!r}")
        return 1
    if header["pick"] or header["focus"]:
        print(f"FAIL  a set asked whole names a pick or a focus {header!r}")
        return 1

    # The mock asks nothing: no pick, the names an ordinary focus
    code, rows, err, header = pick_run(cli, [
        "log", "mock://demo?profile=obd2", "<root>", "--seconds", "1",
        "--pick", "ranked", "--signal", "Fuel Level"])
    focus = focus_of(rows)
    if code != 0 or "asks no signal" not in err or focus is None or \
            [s["name"] for s in focus["signals"]] != ["Fuel Level"] or \
            "pick" in focus or header["pick"]:
        print(f"FAIL  the mock's log {focus} {header!r}, exit {code}, {err!r}")
        return 1
    code, rows, err, header = pick_run(cli, [
        "stream", "mock://demo?profile=obd2", "--count", "3", "--pick",
        "ranked", "--record", "<root>"])
    if code != 0 or "asks no signal" not in err or focus_of(rows) is not None \
            or header["pick"]:
        print(f"FAIL  the mock's stream {header!r}, exit {code}, {err!r}")
        return 1

    # stream on a session that asks: the ranked signals, a pick
    code, rows, err, header = pick_run(cli, [
        "stream", obd2, "--seconds", "1", "--pick", "ranked", "--record",
        "<root>"])
    types = [r.get("type") for r in rows]
    focus = focus_of(rows)
    if code != 0 or focus is None or len(focus["signals"]) != 14 or \
            types.index("focus") != types.index("session") + 1:
        print(f"FAIL  stream's lines {types!r}, exit {code}\n{err}")
        return 1
    if len(header["pick"]) != 1 or \
            not header["pick"][0].startswith("pick  " + OBD2_PICK_KEYS[0] + ","):
        print(f"FAIL  stream's recording names {header!r}")
        return 1

    # Through a signal file a signal the file keys itself (the later of two
    # at one address) has that key on the line, the others their address
    extras = Path(__file__).resolve().parent / "fixtures" / "signals" / "extras.json"
    code, rows, err, header = pick_run(cli, [
        "stream", elm + "?identify=0&diagnose=0&probe=0", "--signals",
        str(extras), "--seconds", "1", "--pick", "ranked"])
    keys = {s["name"]: s["key"]
            for s in (focus_of(rows) or {}).get("signals", [])}
    if code != 0 or keys.get("Road Speed") != "road_speed" or \
            keys.get("RPM") != OBD2_PICK_KEYS[2]:
        print(f"FAIL  stream's keys through a signal file {keys!r}, "
              f"exit {code}\n{err}")
        return 1

    # A pick core refuses (?poll= asks none of the ranked ones) leaves the
    # whole set asked, as in the app, and the run goes on
    polled = elm + "?profile=obd2&poll=0x2F&identify=0&diagnose=0"
    code, rows, err, header = pick_run(cli, [
        "stream", polled, "--seconds", "1", "--pick", "ranked"])
    if code != 0 or "asking every signal: the focus leaves nothing" not in err \
            or focus_of(rows) is not None:
        print(f"FAIL  a refused pick: exit {code}, {err!r}")
        return 1
    print("ok    --pick ranked asks the ranked signals and names them a pick")
    return 0


def car_plugin(folder, plugin_id, plot):
    """A plugin folder whose one variant holds two data files of
    ECUIO_SIGNAL_FILE_MAX_SIGNALS signals, each read by 22 <DID> on its own
    ECU: a set past a rotation, so a session on it is asked only its picks
    (ADR-0065). plot gives every signal that rank, 0 none."""
    most = 1024
    folder = Path(folder)
    (folder / "data").mkdir(parents=True)
    for n, (ecu, name) in enumerate((("a", "A"), ("b", "B"))):
        sigs = []
        for i in range(most):
            s = {"key": f"{ecu}{i}", "name": f"Sig {name} {i}", "ecu": ecu,
                 "read": {"service": "22", "number": f"{0x1000 + i:04X}"},
                 "field": {"byte": 0}}
            if plot:
                s["plot"] = plot
            sigs.append(s)
        (folder / "data" / f"{ecu}.json").write_text(
            json.dumps({"havne_signals": 1, "signals": sigs}), "utf-8")
    (folder / "manifest.json").write_text(json.dumps({
        "havne_plugin": 1, "id": plugin_id, "name": "Car sized",
        "version": "1.0.0", "requires": {"core": "0.4.1"},
        "source": {"name": "test"}, "tiers": ["data"],
        "ecus": [{"id": "a", "rx": "7E8", "tx": "7E0"},
                 {"id": "b", "rx": "7E9", "tx": "7E1"}],
        "variants": [{"id": "base", "files": ["data/a.json", "data/b.json"]}],
    }), "utf-8")


def pick_only_cases(cli, elm):
    """A set past a rotation is asked only its picks: the words say so"""
    with tempfile.TemporaryDirectory(prefix="havne-cli-car-") as plugins:
        car_plugin(Path(plugins) / "plain", "carplain", 0)
        car_plugin(Path(plugins) / "ranked", "carranked", 1)
        plain = ["--plugin", str(Path(plugins) / "plain")]
        ranked = ["--plugin", str(Path(plugins) / "ranked")]
        tail = "&identify=0&diagnose=0"
        uri_plain = elm + "?profile=plugin.carplain.base" + tail
        uri_ranked = elm + "?profile=plugin.carranked.base" + tail

        info = [r for r in run(cli, ["stream", uri_plain, "--seconds", "1"]
                               + plain)[1] if r.get("type") == "session"]
        if len(info) != 1 or info[0].get("pick_only") is not True:
            print(f"FAIL  the session line names no pick_only: {info}")
            return 1

        code, rows, err, _ = pick_run(cli, [
            "stream", uri_plain, "--seconds", "1", "--pick", "ranked"] + plain)
        if code != 0 or focus_of(rows) is not None or \
                "the profile ranks no signal and is asked only what is " \
                "picked: --pick ranked asks none" not in err or \
                "every" in err:
            print(f"FAIL  an unranked pick-only stream: exit {code}, {err!r}")
            return 1

        code, rows, err, _ = pick_run(cli, [
            "stream", uri_ranked, "--seconds", "1", "--pick", "ranked"]
            + ranked)
        if code != 0 or focus_of(rows) is not None or \
                "asking no signal: " not in err or \
                "a session asks 1024 at a time" not in err or \
                "every" in err:
            print(f"FAIL  a refused pick-only pick: exit {code}, {err!r}")
            return 1

        code, rows, err, _ = pick_run(cli, [
            "stream", uri_plain, "--seconds", "1"] + plain)
        if code != 0 or focus_of(rows) is not None or \
                "nothing is asked (--pick ranked, or log --signal)" not in err:
            print(f"FAIL  a pick-only stream with no pick: exit {code}, "
                  f"{err!r}")
            return 1

        with tempfile.TemporaryDirectory(prefix="havne-cli-car-log-") as root:
            code, rows, err = run(cli, [
                "log", uri_plain, root, "--seconds", "1"] + plain)
            made = [p for p in Path(root).iterdir()]
            errors = [r for r in rows if r.get("type") == "error"]
            if code != 3 or made or len(errors) != 1 or \
                    "name signals with --signal or attach conditions with " \
                    "--conditions" not in err:
                print(f"FAIL  a pick-only log with no names: exit {code}, "
                      f"{err!r}, made {made}, {errors}")
                return 1

            # Attached conditions are asked by core: a log with no names
            # but a condition file isn't refused
            conds = Path(root) / "conditions.json"
            conds.write_text(json.dumps({
                "havne_conditions": 1, "name": "Car",
                "conditions": [{"key": "hot", "name": "Hot",
                                "when": {"signal": ["key:a7"], "unit": "",
                                         "above": 1}}],
            }), "utf-8")
            code, rows, err = run(cli, [
                "log", uri_plain, str(Path(root) / "out"), "--seconds", "1",
                "--conditions", str(conds)] + plain)
            ends = [r for r in rows if r.get("type") == "end"]
            if code != 0 or "asked only what is picked" in err or \
                    len(ends) != 1:
                print(f"FAIL  a pick-only log with conditions: exit {code}, "
                      f"{err!r}, {rows[-3:]}")
                return 1

        code, rows, err, header = pick_run(cli, [
            "log", uri_plain, "<root>", "--seconds", "1", "--signal",
            "Sig A 7"] + plain)
        focus = focus_of(rows)
        if code != 0 or focus is None or \
                [s["name"] for s in focus["signals"]] != ["Sig A 7"] or \
                "pick" in focus or len(header["focus"]) != 1:
            print(f"FAIL  a pick-only log of one name: {focus}, exit {code}, "
                  f"{err!r}, {header!r}")
            return 1

        code, rows, err, header = pick_run(cli, [
            "log", uri_plain, "<root>", "--seconds", "1", "--pick",
            "ranked", "--signal", "Sig A 7"] + plain)
        focus = focus_of(rows)
        if code != 0 or focus is None or focus.get("pick") != "ranked" or \
                [s["name"] for s in focus["signals"]] != ["Sig A 7"] or \
                "--pick ranked adds none" not in err or \
                len(header["pick"]) != 1 or \
                not header["pick"][0].startswith("pick  7E8:621007:3.0:8"):
            print(f"FAIL  a pick-only log with --pick ranked: {focus}, "
                  f"exit {code}, {err!r}, {header!r}")
            return 1
    print("ok    a pick-only session says nothing is asked and is asked "
          "only its picks")
    return 0


def pick_only(cli):
    emulator = Emulator(cli)
    try:
        return pick_only_cases(cli, f"elm327://127.0.0.1:{emulator.port}")
    finally:
        emulator.end()


def main(argv):
    if len(argv) >= 3 and argv[1] == "demo":
        return demo(argv[2])
    if len(argv) >= 3 and argv[1] == "pick":
        return pick(argv[2])
    if len(argv) >= 3 and argv[1] == "pick-only":
        return pick_only(argv[2])
    if len(argv) >= 3 and argv[1] == "fails":
        return fails(argv[2])
    if len(argv) >= 4 and argv[1] == "replay":
        return replay(argv[2], argv[3])
    if len(argv) >= 4 and argv[1] == "armed":
        return armed(argv[2], argv[3])
    if len(argv) >= 3 and argv[1] == "interrupt":
        return interrupt(argv[2])
    if len(argv) >= 3 and argv[1] == "closed-pipe":
        return closed_pipe(argv[2])
    if len(argv) >= 4 and argv[1] == "focus-refused":
        return focus_refused(argv[2], argv[3])
    if len(argv) >= 5 and argv[1] == "session":
        return session_line(argv[2], argv[3], argv[4], "--update" in argv[5:])
    if len(argv) >= 3 and argv[1] == "pace":
        return pace(argv[2])
    print(__doc__ or "usage: cli_log.py <mode> <havne-cli> ...")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
