# File/layer: tests/cli_record.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliRecordTest.*): havne-cli stream --record
#          (ADR-0049) from outside the process
#
#   python tests/cli_record.py replay <havne-cli> <fixture folder> <golden.jsonl> <golden folder> [--update]
#   python tests/cli_record.py marks <havne-cli>
#   python tests/cli_record.py fails <havne-cli>
#   python tests/cli_record.py signals <havne-cli> <fixture folder> <signal file>
#   python tests/cli_record.py hang <havne-cli> <fake_j2534.dll>    (Windows)
#
# replay: the fixture through obd2 to its last frame, recorded under
# --label; the stream's own lines (frames and values left to the other
# goldens) against golden.jsonl, the folder against the golden folder
# (recording_compare.py) and its frames.csv against the fixture's.
# marks: lines on standard input are markers, a mark line each, in order.
# fails: a folder that can't be made says why and exits 1 before any
# frame. hang: an interrupt while the session's close hangs still ends
# the recording whole, 2 s on. signals: the fixture recorded through a
# signal file holds it as signals.json with its hash, which info names,
# and a replay of that recording through its signals.json decodes the
# same values (ADR-0051 section 4). --update rewrites both goldens from
# the run.

import hashlib
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside the tests
import golden  # noqa: E402
import recording_compare  # noqa: E402

IGNORED = {"core", "folder"}


def run(cli, args, stdin=b""):
    done = subprocess.run([cli] + args, input=stdin, capture_output=True)
    rows = golden.parse(done.stdout.decode("utf-8"), "stdout")
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


def the_folder(root):
    found = [p for p in Path(root).iterdir() if p.is_dir()]
    if len(found) != 1:
        raise SystemExit(f"FAIL  {len(found)} folders in the recordings' root")
    return found[0]


def replay(cli, fixture, golden_lines, golden_folder, rewrite):
    with tempfile.TemporaryDirectory(prefix="havne-cli-record-") as root:
        uri = f"replay://{Path(fixture).as_posix()}?profile=obd2&speed=max"
        code, rows, err = run(cli, ["stream", uri, "--record", root,
                                    "--label", "golden"])
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        own = [r for r in rows if r.get("type") not in ("frame", "values")]
        folder = the_folder(root)
        if not folder.name.endswith(" golden"):
            print(f"FAIL  the folder's name {folder.name!r} has no label")
            return 1
        started = [r for r in own if r.get("type") == "recording"]
        if not started or Path(started[0]["folder"]).name != folder.name:
            print("FAIL  the recording line doesn't name the folder")
            return 1
        if rewrite:
            Path(golden_lines).write_text(golden.golden_text(own, IGNORED),
                                          encoding="utf-8", newline="\n")
            recording_compare.update(folder, golden_folder)
            print(f"ok    rewrote {golden_lines} and {golden_folder}")
            return 0
        expected = golden.parse(Path(golden_lines).read_text(encoding="utf-8"),
                                golden_lines)
        problems = golden.compare(own, expected, IGNORED)
        if golden.report(problems, golden_lines):
            return 1
        found = recording_compare.compare(
            folder, golden_folder, frames_from=Path(fixture) / "frames.csv")
        for f in found:
            print(f"FAIL  {f}")
        if found:
            print(f"      (rewrite it from this run with --update: {golden_folder})")
            return 1
        print(f"ok    the folder matches {golden_folder}")
        return 0


def marks(cli):
    typed = "lap 1\n\nsecond lap\r\n\xf8l".encode("utf-8")
    with tempfile.TemporaryDirectory(prefix="havne-cli-marks-") as root:
        code, rows, err = run(cli, ["stream", "mock://marks", "--count", "30",
                                    "--record", root], typed)
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        want = ["lap 1", "", "second lap", "\xf8l"]
        got = [r["label"] for r in rows if r.get("type") == "mark"]
        if got != want:
            print(f"FAIL  mark lines {got!r}, want {want!r}")
            return 1
        text = (the_folder(root) / "markers.csv").read_bytes().decode("utf-8")
        labels = [line.split(",", 1)[1] for line in text.splitlines()[1:]]
        if labels != want:
            print(f"FAIL  markers.csv's labels {labels!r}, want {want!r}")
            return 1
        closed = [r for r in rows if r.get("type") == "recording"
                  and r.get("state") == "closed"]
        if closed != [{"type": "recording", "state": "closed", "whole": True}]:
            print(f"FAIL  the closing line {closed!r}")
            return 1
        end = json.dumps(rows[-1], separators=(",", ":"))
        events = (the_folder(root) / "events.log").read_bytes().decode("utf-8")
        if f"  note  {end}" not in events:
            print(f"FAIL  events.log has no note of the end line {end}")
            return 1
    print("ok    each line typed is a marker")
    return 0


def fails(cli):
    with tempfile.TemporaryDirectory(prefix="havne-cli-fails-") as base:
        blocker = Path(base) / "a file"
        blocker.write_text("not a folder")
        code, rows, err = run(cli, ["stream", "mock://fails", "--count", "5",
                                    "--record", str(blocker / "under")])
        if code != 1:
            print(f"FAIL  exit {code}, want 1\n{err}")
            return 1
        types = [r.get("type") for r in rows]
        if types != ["start", "session", "recording"]:
            print(f"FAIL  lines {types!r}")
            return 1
        line = rows[2]
        if line.get("state") != "failed" or not line.get("reason"):
            print(f"FAIL  {json.dumps(line)}")
            return 1
        if str(blocker) in line["reason"] or base in line["reason"]:
            print(f"FAIL  the reason names the path: {line['reason']!r}")
            return 1
    print("ok    a folder that can't be made fails the stream")
    return 0


def hang(cli, dll):
    """Windows: the fake DLL holds its disconnect 6 s, and Ctrl+Break
    comes a second in; the recorder closes while still attached, 2 s on,
    so its files end whole before the session's close returns"""
    import os
    import signal
    env = dict(os.environ, ECUIO_FAKE_DISCONNECT_DELAY_MS="6000")
    with tempfile.TemporaryDirectory(prefix="havne-cli-hang-") as root:
        p = subprocess.Popen([cli, "stream", f"j2534://{dll}", "--record", root],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=env,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        time.sleep(1.5)
        try:
            p.send_signal(signal.CTRL_BREAK_EVENT)
        except OSError as e:
            p.kill()
            p.communicate()
            print(f"skip  no console to send Ctrl+Break through: {e}")
            return SKIP
        try:
            out, err = p.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            print("FAIL  still running 20 s after Ctrl+Break")
            return 1
        rows = golden.parse(out.decode("utf-8"), "stdout")
        own = [r for r in rows if r.get("type") not in ("frame", "values")]
        types = [(r.get("type"), r.get("state")) for r in own]
        if p.returncode != 0 or not own or own[-1].get("reason") != "interrupted":
            print(f"FAIL  exit {p.returncode}, lines {types}\n{err.decode()}")
            return 1
        if ("recording", "closed") not in types:
            print(f"FAIL  no closing recording line: {types}")
            return 1
        events = (the_folder(root) / "events.log").read_bytes().decode("utf-8")
        if "  end  interrupted: the session's close didn't finish: " not in events:
            print(f"FAIL  events.log's summary:\n{events[-400:]}")
            return 1
    print("ok    an interrupted close that hangs still ends the recording whole")
    return 0


def decoded(folder):
    """signals.csv's rows without their host_us: what was decoded"""
    rows = (folder / "signals.csv").read_text(encoding="utf-8-sig").splitlines()
    return [r.split(",", 1)[1] for r in rows[1:]]


def signals(cli, fixture, signal_file):
    want = Path(signal_file).read_bytes()
    sha = hashlib.sha256(want).hexdigest()
    with tempfile.TemporaryDirectory(prefix="havne-cli-signals-") as base:
        first_root = Path(base) / "first"
        uri = f"replay://{Path(fixture).as_posix()}?speed=max"
        code, _, err = run(cli, ["stream", uri, "--signals", signal_file,
                                 "--record", str(first_root)])
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        first = the_folder(first_root)
        if (first / "signals.json").read_bytes() != want:
            print("FAIL  signals.json isn't the signal file's bytes")
            return 1
        events = (first / "events.log").read_text(encoding="utf-8")
        if f"\nsignal file  signals.json,{sha}\n" not in events:
            print(f"FAIL  events.log names no signals.json {sha}")
            return 1
        code, rows, err = run(cli, ["info", str(first)])
        opened = [r for r in rows if r.get("type") == "opened"]
        if code != 0 or not opened or opened[0].get("signal_file") != {
                "sha256": sha, "present": True, "matches": True}:
            print(f"FAIL  info's opened line: {opened}\n{err}")
            return 1

        second_root = Path(base) / "second"
        again = f"replay://{first.as_posix()}?speed=max"
        code, _, err = run(cli, ["stream", again, "--signals",
                                 str(first / "signals.json"),
                                 "--record", str(second_root)])
        if code != 0:
            print(f"FAIL  the replay of the recording: exit {code}\n{err}")
            return 1
        second = the_folder(second_root)
        a, b = decoded(first), decoded(second)
        if len(a) < 100 or not any(",Road Speed," in "," + r for r in a):
            print(f"FAIL  the first recording decoded {len(a)} values, "
                  "none of the file's")
            return 1
        if a != b:
            at = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                      min(len(a), len(b)))
            print(f"FAIL  the replay decodes otherwise from row {at + 1} "
                  f"({len(a)} rows, then {len(b)})")
            return 1
        if (second / "signals.json").read_bytes() != want:
            print("FAIL  the replay's recording holds other bytes")
            return 1
    print("ok    a recording holds its signal file, and its replay decodes "
          "the same")
    return 0


SKIP = 77


def main():
    command, cli = sys.argv[1], sys.argv[2]
    if command == "hang":
        return hang(cli, sys.argv[3])
    if command == "replay":
        return replay(cli, sys.argv[3], sys.argv[4], sys.argv[5],
                      "--update" in sys.argv[6:])
    if command == "marks":
        return marks(cli)
    if command == "fails":
        return fails(cli)
    if command == "signals":
        return signals(cli, sys.argv[3], sys.argv[4])
    print(f"cli_record: no command {command!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
