#!/usr/bin/env python3
# File/layer: scripts/filelength_check.py | repo tooling, Python 3.8+, no packages
# Testing: UNIT, over made-up trees (ctest FileLengthTest.SelfTest); the real
# baseline is judged by scripts/tidy.sh
#
# The file-length ratchet: a tracked source file stays under 1,000 lines
# (the source-size rule). scripts/filelength-baseline.txt lists the files
# already past it, "<path><TAB><lines>" each. A run fails on
#   - a file past 1,000 lines that the baseline doesn't list,
#   - a listed file longer than its number,
#   - a listed line that no longer applies (the file is gone, now within the
#     limit, or exempt).
# A listed file can only shrink: --update lowers a number that shrank and
# removes a line that no longer applies; it never adds a line or raises a
# number.
#
# Exempt, as the size rule says: data tables, generated files and the public
# headers (EXEMPT below); third_party/ is not ours.
#
# Usage: filelength_check.py [--update]      judges the repo this script is in
#        filelength_check.py selftest
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

LIMIT = 1000
SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".qml", ".py", ".sh")
EXEMPT_PREFIXES = ("third_party/", "core/include/ecuio/")
EXEMPT_FILES = (
    "core/src/dtc_titles_table.h",
    "core/src/vin_wmi_table.h",
    "python/havne/_ecuio.py",
)
BASELINE = "scripts/filelength-baseline.txt"


def exempt(path):
    return path in EXEMPT_FILES or path.startswith(EXEMPT_PREFIXES)


def count_lines(full):
    with open(full, "rb") as fh:
        data = fh.read()
    n = data.count(b"\n")
    return n + (1 if data and not data.endswith(b"\n") else 0)


def tracked(root):
    out = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("filelength: git ls-files failed: " + out.stderr.strip())
    return [p for p in out.stdout.splitlines() if p.endswith(SUFFIXES)]


def read_baseline(path):
    rows = {}
    if not os.path.isfile(path):
        return rows
    with open(path, newline="") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            rows[parts[0]] = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    return rows


def judge(root, files, base):
    """Returns (problems, lowered, stale): the failures, the listed files whose
    number can drop (path -> lines) and the lines that no longer apply."""
    problems, lowered, stale = [], {}, []
    measured = {}
    for p in files:
        if exempt(p):
            continue
        n = count_lines(os.path.join(root, p))
        if n > LIMIT:
            measured[p] = n
    for p, n in sorted(measured.items()):
        if p not in base:
            problems.append("%s: %d lines, past %d and not in %s (split it, a commit of moves)"
                            % (p, n, LIMIT, BASELINE))
        elif n > base[p]:
            problems.append("%s: grew from %d to %d lines (a listed file only shrinks)"
                            % (p, base[p], n))
        elif n < base[p]:
            lowered[p] = n
    for p in sorted(base):
        if p not in measured:
            why = "exempt" if exempt(p) else ("no such file" if not os.path.isfile(os.path.join(root, p))
                                              else "within %d lines" % LIMIT)
            stale.append("%s: listed at %d lines, no longer applies (%s)" % (p, base[p], why))
    return problems, lowered, stale


def write_baseline(path, header, rows):
    with open(path, "w", newline="\n") as fh:
        fh.write(header)
        for p in sorted(rows):
            fh.write("%s\t%d\n" % (p, rows[p]))


def header_of(path):
    if not os.path.isfile(path):
        return ""
    with open(path, newline="") as fh:
        return "".join(l.replace("\r\n", "\n") for l in fh if l.startswith("#"))


def run(root, update):
    base_path = os.path.join(root, BASELINE)
    base = read_baseline(base_path)
    problems, lowered, stale = judge(root, tracked(root), base)
    if update and (lowered or stale):
        rows = {p: lowered.get(p, n) for p, n in base.items()
                if not any(s.startswith(p + ":") for s in stale)}
        write_baseline(base_path, header_of(base_path), rows)
        print("filelength  lowered %d number(s), removed %d line(s) in %s"
              % (len(lowered), len(stale), BASELINE))
        stale = []
    for msg in problems + stale:
        print("filelength  FAIL  " + msg)
    if problems or stale:
        if stale:
            print("  remove a stale line (scripts/tidy.sh --update-size-baseline does)")
        return 1
    return 0


def selftest():
    if shutil.which("git") is None:
        print("no git found")
        return 77
    fails = []

    def expect(label, got, want):
        if got != want:
            fails.append("%s: got %r, wanted %r" % (label, got, want))

    def tree(files, base_lines):
        d = tempfile.mkdtemp()
        subprocess.run(["git", "init", "-q", d], check=True)
        for p, n in files.items():
            full = os.path.join(d, p)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", newline="\n") as fh:
                fh.write("x\n" * n)
        subprocess.run(["git", "-C", d, "add", "-A"], check=True)
        os.makedirs(os.path.join(d, "scripts"), exist_ok=True)
        with open(os.path.join(d, BASELINE), "w", newline="\n") as fh:
            fh.write("# header\n" + "".join("%s\t%d\n" % kv for kv in base_lines.items()))
        return d

    def verdict(files, base_lines, update=False):
        d = tree(files, base_lines)
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = run(d, update)
        return rc, read_baseline(os.path.join(d, BASELINE)), buf.getvalue()

    rc, _, out = verdict({"a/x.cpp": 1000}, {})
    expect("a file at the limit passes", rc, 0)
    rc, _, out = verdict({"a/x.cpp": 1001}, {})
    expect("a new file past the limit fails", rc, 1)
    rc, _, out = verdict({"a/x.cpp": 1500}, {"a/x.cpp": 1500})
    expect("a listed file at its number passes", rc, 0)
    rc, _, out = verdict({"a/x.cpp": 1501}, {"a/x.cpp": 1500})
    expect("a listed file that grew fails", rc, 1)
    rc, _, out = verdict({"a/x.cpp": 1400}, {"a/x.cpp": 1500})
    expect("a listed file that shrank passes", rc, 0)
    rc, base, out = verdict({"a/x.cpp": 1400}, {"a/x.cpp": 1500}, update=True)
    expect("update lowers the number", base, {"a/x.cpp": 1400})
    rc, base, out = verdict({"a/x.cpp": 1600}, {"a/x.cpp": 1500}, update=True)
    expect("update never raises a number", (rc, base), (1, {"a/x.cpp": 1500}))
    rc, _, out = verdict({"a/x.cpp": 900}, {"a/x.cpp": 1500})
    expect("a listed file within the limit is stale", rc, 1)
    rc, base, out = verdict({"a/x.cpp": 900}, {"a/x.cpp": 1500}, update=True)
    expect("update removes a stale line", (rc, base), (0, {}))
    rc, _, out = verdict({"a/x.cpp": 10}, {"a/gone.cpp": 1500})
    expect("a listed file that is gone is stale", rc, 1)
    rc, _, out = verdict({"core/include/ecuio/api.h": 3000}, {})
    expect("a public header is exempt", rc, 0)
    rc, _, out = verdict({"core/src/dtc_titles_table.h": 3000, "python/havne/_ecuio.py": 3000}, {})
    expect("a data table and a generated file are exempt", rc, 0)
    rc, _, out = verdict({"third_party/w/x.c": 3000}, {})
    expect("third_party is exempt", rc, 0)
    rc, _, out = verdict({"a/x.txt": 3000, "a/y.md": 3000}, {})
    expect("a non-source file is not judged", rc, 0)
    rc, _, out = verdict({"a/x.qml": 1001, "a/y.py": 1001, "a/z.sh": 1001, "a/w.h": 1001}, {})
    expect("QML, Python, shell and headers are judged", out.count("FAIL"), 4)
    rc, _, out = verdict({"core/include/ecuio/api.h": 3000}, {"core/include/ecuio/api.h": 3000})
    expect("a listed exempt file is stale", rc, 1)

    for m in fails:
        print("FAIL " + m)
    print("%d failure(s)" % len(fails) if fails else "ok")
    return 1 if fails else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        return selftest()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return run(root, "--update" in sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
