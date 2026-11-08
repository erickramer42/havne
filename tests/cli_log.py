# File/layer: tests/cli_log.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliLogRunTest.*): havne-cli log (ADR-0056
#          section 7) from outside the process
#
#   python tests/cli_log.py demo <havne-cli>
#   python tests/cli_log.py fails <havne-cli>
#   python tests/cli_log.py replay <havne-cli> <fixture folder>
#   python tests/cli_log.py interrupt <havne-cli>
#   python tests/cli_log.py closed-pipe <havne-cli>
#   python tests/cli_log.py focus-refused <havne-cli> <fake_j2534.dll>   (Windows)
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
# its last frame, reason replay-end, the recording whole. interrupt:
# Ctrl+Break (Windows, exit 77 without a console) or SIGINT ends the
# log with reason interrupted, the recording closed whole and read back.
# closed-pipe: standard output closed under a running log, and under a
# stream --record, is a quiet exit 0 with the recording closed whole,
# its end line's why output-failed (the crash this pins: a null reason
# on that path). focus-refused: a focus the session can't ask (a
# ?poll= session, a pick that isn't the polled PID) is an error line
# after the session line, exit 3, no recording made.

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
HEAD = "Time (ms),Vehicle Speed (km/h),RPM (RPM)"


def run(cli, args, env=None):
    done = subprocess.run([cli] + args, capture_output=True, env=env)
    rows = golden.parse(done.stdout.decode("utf-8"), "stdout")
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


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
        want = "focus  " + ",".join(PICKS)
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


def main(argv):
    if len(argv) >= 3 and argv[1] == "demo":
        return demo(argv[2])
    if len(argv) >= 3 and argv[1] == "fails":
        return fails(argv[2])
    if len(argv) >= 4 and argv[1] == "replay":
        return replay(argv[2], argv[3])
    if len(argv) >= 3 and argv[1] == "interrupt":
        return interrupt(argv[2])
    if len(argv) >= 3 and argv[1] == "closed-pipe":
        return closed_pipe(argv[2])
    if len(argv) >= 4 and argv[1] == "focus-refused":
        return focus_refused(argv[2], argv[3])
    print(__doc__ or "usage: cli_log.py <mode> <havne-cli> ...")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
