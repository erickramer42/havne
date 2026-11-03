# File/layer: tests/recording_compare.py | test-only, Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest RecordingCompareTest.SelfTest);
#          then havne-cli's recording runs (CliRecordTest)
#
# Compares two recording folders (ADR-0049 section 9) on what doesn't
# depend on the run: frames.csv with host_us left out and timestamp_us
# rebased to the first frame; signals.csv and markers.csv without host_us;
# latency.csv without t_s; events.log's header without its times, core's
# version, the URI and the caller lines, and core's own lines (link,
# asleep, drop, and end's counts) without their times. Notes differ by
# front end and are left out. A difference names the file and the line.
#
#   python3 tests/recording_compare.py compare <actual> <golden> [--frames-from <frames.csv>]
#   python3 tests/recording_compare.py selftest
#
# The golden folder holds the files compared; --frames-from compares the
# actual frames.csv with another recording's instead (a replay's source),
# so a golden folder needn't carry a copy of its fixture. A file the
# actual folder has and the golden doesn't is a difference.
# Exit 0 on a match, 1 on a difference, 2 on an error.

import argparse
import re
import sys
import tempfile
from pathlib import Path

HEADER_SKIPPED = {"started", "utc", "core", "uri", "caller"}
CORE_KINDS = {"link", "asleep", "drop", "end"}
EVENT = re.compile(r"^\d\d:\d\d:\d\d\.\d{3}  \+\d+\.\d{3}  (\w+)  (.*)$")
END_COUNTS = re.compile(
    r"(\d+ frames, \d+ values, \d+ records dropped, \d+ notes dropped)$")


def text_lines(path):
    """The file's lines, its line endings and a byte-order mark dropped"""
    text = path.read_bytes().decode("utf-8")
    if text.startswith("\ufeff"):
        text = text[1:]
    return text.splitlines()


def frames(path):
    """frames.csv's rows without host_us, timestamp_us from the first row"""
    rows = text_lines(path)
    out = [rows[0].split(",", 1)[1]] if rows else []
    base = None
    for row in rows[1:]:
        host, stamp, rest = row.split(",", 2)
        stamp = int(stamp)
        if base is None:
            base = stamp
        out.append(f"{stamp - base},{rest}")
    return out


def without_first_column(path):
    """signals.csv, markers.csv and latency.csv without their time column"""
    return [row.split(",", 1)[1] if "," in row else row
            for row in text_lines(path)]


def events(path):
    """events.log's lines as compared: the header's, then core's own"""
    out = []
    lines = text_lines(path)
    i = 0
    while i < len(lines) and lines[i]:
        line = lines[i]
        key = line.split("  ", 1)[0]
        if i == 0 or key not in HEADER_SKIPPED:
            out.append(line)
        i += 1
    for line in lines[i:]:
        m = EVENT.match(line)
        if not m:
            continue
        kind, text = m.groups()
        if kind not in CORE_KINDS:
            continue
        if kind == "end":
            counts = END_COUNTS.search(text)
            text = counts.group(1) if counts else text
        out.append(f"{kind}  {text}")
    return out


READERS = {
    "frames.csv": frames,
    "signals.csv": without_first_column,
    "markers.csv": without_first_column,
    "latency.csv": without_first_column,
    "events.log": events,
}


def first_difference(name, actual, golden):
    for number, (a, g) in enumerate(zip(actual, golden), start=1):
        if a != g:
            return f"{name}:{number}: {a!r}, golden has {g!r}"
    if len(actual) != len(golden):
        return f"{name}: {len(actual)} lines, golden has {len(golden)}"
    return None


def compare(actual, golden, frames_from=None):
    """The differences between two folders, as messages; none on a match"""
    actual, golden = Path(actual), Path(golden)
    found = []
    expected = {p.name for p in golden.iterdir() if p.is_file()}
    if frames_from is not None:
        expected.add("frames.csv")
    for p in sorted(actual.iterdir()):
        if p.is_file() and p.name not in expected:
            found.append(f"{p.name}: in the recording, not in the golden folder")
    for name in sorted(expected):
        read = READERS.get(name)
        if read is None:
            found.append(f"{name}: no rule compares it")
            continue
        mine = actual / name
        if not mine.is_file():
            found.append(f"{name}: missing from the recording")
            continue
        theirs = Path(frames_from) if name == "frames.csv" and frames_from \
            else golden / name
        d = first_difference(name, read(mine), read(theirs))
        if d:
            found.append(d)
    return found


URI_PATH = re.compile(r"^(uri  [a-z0-9]+://)[^?]*", re.M)


def update(actual, golden):
    """The golden folder rewritten from a run: the files it compares, with
    LF line endings, and the URI's path (this machine's, which the compare
    leaves out) as <path>"""
    golden = Path(golden)
    golden.mkdir(parents=True, exist_ok=True)
    for p in golden.iterdir():
        if p.is_file():
            p.unlink()
    for p in Path(actual).iterdir():
        if p.is_file() and p.name in READERS and p.name != "frames.csv":
            text = p.read_bytes().decode("utf-8").replace("\r\n", "\n")
            if p.name == "events.log":
                text = URI_PATH.sub(r"\1<path>", text, count=1)
            (golden / p.name).write_bytes(text.encode("utf-8"))


def selftest():
    header = ("ecuio session recording, format 2 (ADR-0049)\n"
              "started  2026-11-02 10:00:00.000 (local time)\n"
              "utc  2026-11-02T16:00:00.000Z\n"
              "core  0.3.6\n"
              "uri  replay://C:/somewhere/run?speed=max\n"
              "profile  obd2\n"
              "frame flags  complete\n"
              "caller  havne-cli 0.3.6\n"
              "columns  local time, seconds\n\n")
    body = ("10:00:00.100  +0.100  link  vehicle not responding (state 8, 0 episodes)\n"
            "10:00:00.200  +0.200  note  something a front end said\n\n"
            "10:00:01.000  +1.000  end  havne-cli: replay-end: 2 frames, 1 values, "
            "0 records dropped, 0 notes dropped\n")
    files = {
        "frames.csv": "host_us,timestamp_us,id,flags,data\n"
                      "10,5000,0x7E8,0x100,41 0C 1F 40\n20,5100,0x7E8,0x0,41 0D 20\n",
        "signals.csv": "\ufeffhost_us,signal,value,unit\n10,RPM,2000,rpm\n",
        "events.log": header + body,
    }

    def folder(base, name, changes=None):
        d = Path(base) / name
        d.mkdir()
        for f, text in files.items():
            if changes and f in changes:
                text = changes[f](text)
            # As Windows writes them, CRLF
            (d / f).write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
        return d

    failures = []

    def expect(label, found, want_match):
        if bool(found) == want_match:
            failures.append(f"{label}: {'differs' if found else 'matches'}: {found}")

    with tempfile.TemporaryDirectory() as base:
        golden = folder(base, "golden")
        runs = {
            "the same": (None, True),
            "other host_us and stamps rebased": ({
                "frames.csv": lambda t: t.replace("10,5000", "77,9000")
                .replace("20,5100", "88,9100"),
                "signals.csv": lambda t: t.replace("10,RPM", "99,RPM")}, True),
            "other times, version, URI, caller and notes": ({
                "events.log": lambda t: t.replace("10:00", "11:30")
                .replace("0.3.6", "0.4.0").replace("C:/somewhere", "/home/x")
                .replace("something a front end said", "another")
                .replace("havne-cli: replay-end", "the app: closed")}, True),
            "a value": ({"signals.csv": lambda t: t.replace("2000", "2001")}, False),
            "a frame's flags": ({"frames.csv": lambda t: t.replace("0x100", "0x0")}, False),
            "a step between frames": ({"frames.csv": lambda t: t.replace("5100", "5200")}, False),
            "the profile": ({"events.log": lambda t: t.replace("obd2", "raw")}, False),
            "a link line": ({"events.log": lambda t: t.replace("state 8", "state 9")}, False),
            "the end's counts": ({"events.log": lambda t: t.replace("1 values", "2 values")}, False),
        }
        for i, (label, (changes, want)) in enumerate(runs.items()):
            expect(label, compare(folder(base, f"run{i}", changes), golden), want)
        extra = folder(base, "extra")
        (extra / "markers.csv").write_text("host_us,label\n5,lap\n")
        expect("an extra file", compare(extra, golden), False)
        missing = folder(base, "missing")
        (missing / "signals.csv").unlink()
        expect("a missing file", compare(missing, golden), False)
        # --frames-from: the golden folder without frames.csv
        lean = Path(base) / "lean"
        update(folder(base, "source"), lean)
        if (lean / "frames.csv").exists():
            failures.append("update copied frames.csv")
        kept = (lean / "events.log").read_bytes().decode("utf-8")
        if "\r" in kept or "somewhere" in kept or "uri  replay://<path>?" not in kept:
            failures.append("update kept a CR or the URI's path")
        expect("frames from the source",
               compare(folder(base, "run-lean"), lean,
                       frames_from=Path(base) / "source" / "frames.csv"), True)
    for f in failures:
        print(f"selftest: {f}")
    print("selftest: ok" if not failures else f"selftest: {len(failures)} failed")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("compare")
    c.add_argument("actual")
    c.add_argument("golden")
    c.add_argument("--frames-from")
    sub.add_parser("selftest")
    args = parser.parse_args()
    if args.command == "selftest":
        return selftest()
    try:
        found = compare(args.actual, args.golden, args.frames_from)
    except (OSError, ValueError, UnicodeDecodeError) as e:
        print(f"recording_compare: {e}", file=sys.stderr)
        return 2
    for f in found:
        print(f)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
