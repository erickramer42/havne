# File/layer: tests/tidy_ratchet_selftest.py | test-only, Python 3.8+, no packages
# Testing: UNIT, over made-up size.txt and baseline files (ctest TidyRatchetTest.SelfTest)
#
# Runs scripts/tidy-size-ratchet.sh over fixtures and checks its exit code:
# a clean run, a function the baseline doesn't list, a repeated name with too
# few baseline lines, a size line the script can't read, a stale line and
# the update that removes one; a listed function that grew in lines or in
# complexity, one that shrank (passes, and the update lowers its numbers) and
# one past a threshold its line lists it within. Skips (77) where no bash
# is found.
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "tidy-size-ratchet.sh")


def find_bash():
    # On Windows PATH's bash can be WSL's: take Git's beside git.exe
    if os.name == "nt":
        git = shutil.which("git")
        d = os.path.dirname(git) if git else ""
        while d and d != os.path.dirname(d):
            cand = os.path.join(d, "bin", "bash.exe")
            if os.path.isfile(cand):
                return cand
            d = os.path.dirname(d)
        return None
    return shutil.which("bash")


def warn(path, line, name, kind="size"):
    msg = ("exceeds recommended size/complexity thresholds [readability-function-size]"
           if kind == "size" else
           "has cognitive complexity of 50 (threshold 40) [readability-function-cognitive-complexity]")
    return "%s:%d:1: warning: function '%s' %s" % (path, line, name, msg)


def note(path, line, n):
    return ("%s:%d:1: note: %d lines including whitespace and comments (threshold 80)"
            % (path, line, n))


def run_one(bash, size_lines, base_lines, files, update=0, notes=None):
    if notes is None:
        # Each size warning's note, 90 lines, as clang-tidy prints it
        notes = [note(m.group(1), int(m.group(2)), 90)
                 for m in (re.match(r"([^:]+):(\d+):\d+: warning: function .* exceeds", x)
                           for x in size_lines) if m]
    with tempfile.TemporaryDirectory() as d:
        for f in files:
            os.makedirs(os.path.dirname(os.path.join(d, f)) or d, exist_ok=True)
            open(os.path.join(d, f), "w").write("\n")
        for name, lines in (("size.txt", size_lines), ("base.txt", base_lines),
                            ("checked.txt", files), ("notes.txt", notes)):
            with open(os.path.join(d, name), "w", newline="\n") as fh:
                fh.write("".join(x + "\n" for x in lines))
        p = subprocess.run([bash, SCRIPT, "size.txt", "base.txt", "checked.txt", "0", str(update),
                            "notes.txt"],
                           cwd=d, capture_output=True, text=True)
        base_after = open(os.path.join(d, "base.txt"), newline="").read().splitlines()
        return p.returncode, p.stdout + p.stderr, base_after


# Each bash spawn costs seconds on Windows, so the cases run twice: a first
# pass records every distinct call, the calls run on a pool, and the second
# pass reads their results.
_calls = {}
_recording = True


def run(bash, size_lines, base_lines, files, update=0, notes=None):
    key = repr((size_lines, base_lines, files, update, notes))
    if _recording:
        _calls.setdefault(key, (bash, size_lines, base_lines, files, update, notes))
        return 0, "", []
    return _calls[key]


def main():
    global _recording
    bash = find_bash()
    if not bash:
        print("no bash found")
        return 77
    _recording = True
    cases(bash)
    todo = dict(_calls)
    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 2)) as pool:
        results = list(pool.map(lambda a: run_one(*a), todo.values()))
    _calls.clear()
    _calls.update(zip(todo.keys(), results))
    _recording = False
    return cases(bash)


def cases(bash):
    f = "core/src/a.cpp"
    fails = []

    def expect(label, got, want, out=""):
        if got != want:
            fails.append("%s: exit %s, wanted %s\n%s" % (label, got, want, out))

    rc, out, _ = run(bash, [warn(f, 10, "run")], ["%s\trun\t90\t-" % f], [f])
    expect("a listed function passes", rc, 0, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 10, "run", "cx")],
                     ["%s\trun\t90\t50" % f], [f])
    expect("size and complexity on one function pass with one line", rc, 0, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "go")], ["%s\trun\t90\t-" % f], [f])
    expect("an unlisted function fails", rc, 1, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "run")], ["%s\trun\t90\t-" % f], [f])
    expect("a second function of a listed name fails", rc, 1, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "run")],
                     ["%s\trun\t90\t-" % f, "%s\trun\t90\t-" % f], [f])
    expect("a repeated name with a line for each passes", rc, 0, out)
    rc, out, _ = run(bash, [warn("E:/src/" + f, 10, "go")], [], [f])
    expect("a line it cannot read fails", rc, 1, out)
    lam = ("%s:10:5: warning: lambda has cognitive complexity of 50 (threshold 40) "
           "[readability-function-cognitive-complexity]" % f)
    rc, out, _ = run(bash, [warn(f, 10, "run"), lam], ["%s\trun\t90\t-" % f], [f])
    expect("a lambda's line fails as unreadable", rc, 1, out)
    rc, out, _ = run(bash, [], ["%s\trun\t90\t-" % f], [f])
    expect("a stale baseline line fails", rc, 1, out)
    rc, out, base = run(bash, [warn(f, 10, "run")],
                        ["%s\trun\t90\t-" % f, "%s\trun\t90\t-" % f], [f], update=1)
    expect("update drops one of two lines", rc, 0, out)
    expect("update leaves the other line", base, ["%s\trun\t90\t-" % f], str(base))

    # Measured numbers: a listed function may stay or shrink, never grow
    def sized(lines, cx=None):
        w = [warn(f, 10, "run")] if lines else []
        if cx:
            w.append(warn(f, 10, "run", "cx").replace("of 50", "of %d" % cx))
        return w, ([note(f, 10, lines)] if lines else [])

    def line(lines, cx):
        return "%s	run	%s	%s" % (f, lines, cx)

    w, n = sized(96)
    rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=n)
    expect("a listed function at its lines passes", rc, 0, out)
    rc, out, _ = run(bash, w, [line(97, "-")], [f], notes=n)
    expect("a listed function under its lines passes", rc, 0, out)
    rc, out, _ = run(bash, w, [line(95, "-")], [f], notes=n)
    expect("a listed function over its lines fails", rc, 1, out)
    w, n = sized(0, 52)
    rc, out, _ = run(bash, w, [line("-", 52)], [f], notes=n)
    expect("a listed function at its complexity passes", rc, 0, out)
    rc, out, _ = run(bash, w, [line("-", 51)], [f], notes=n)
    expect("a listed function over its complexity fails", rc, 1, out)
    w, n = sized(96, 52)
    rc, out, _ = run(bash, w, [line(96, 52)], [f], notes=n)
    expect("both measures at their numbers pass", rc, 0, out)
    rc, out, _ = run(bash, w, [line(96, 51)], [f], notes=n)
    expect("one measure over with the other at its number fails", rc, 1, out)
    rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=n)
    expect("a function past a threshold its line lists it within fails", rc, 1, out)
    # A size warning with no note can't be judged: it fails, never reads as 0 lines
    w, n = sized(96)
    rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=[])
    expect("a size warning with no note fails", rc, 1, out)
    rc, out, base = run(bash, w, [line(96, "-")], [f], update=1, notes=[])
    expect("update with a missing note fails", rc, 1, out)
    expect("update with a missing note keeps the recorded number", base, [line(96, "-")], str(base))
    rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=[note(f, 11, 96)])
    expect("a note of another line does not join", rc, 1, out)
    # A root or a drive letter left on the note's path still joins it, in
    # either case of the letter; another file's note doesn't
    for root in ("E:/work/tree/", "e:/work/tree/"):
        rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=[note(root + f, 10, 96)])
        expect("a note under %s joins" % root, rc, 0, out)
        rc, out, _ = run(bash, w, [line(95, "-")], [f], notes=[note(root + f, 10, 96)])
        expect("a joined note under %s still judges growth" % root, rc, 1, out)
    rc, out, _ = run(bash, w, [line(96, "-")], [f], notes=[note("E:/work/tree/core/src/b.cpp", 10, 96)])
    expect("a note of another file does not join", rc, 1, out)
    w, n = sized(90)
    rc, out, base = run(bash, w, [line(96, 52)], [f], update=1, notes=n)
    expect("a shrunk function that is still listed passes", rc, 0, out)
    expect("update lowers the lines and drops the complexity it is within",
           base, [line(90, "-")], str(base))
    w, n = sized(96)
    rc, out, base = run(bash, w, [line(96, "-")], [f], update=1, notes=n)
    expect("update at the same numbers changes nothing", base, [line(96, "-")], str(base))
    rc, out, base = run(bash, w, [line(80, "-")], [f], update=1, notes=n)
    expect("update never raises a number", base, [line(80, "-")], str(base))

    # A run that lowers one function and finds another grown writes the grown
    # one back at its old number
    rc, out, base = run(bash, [warn(f, 10, "run"), warn(f, 40, "go")],
                        ["%s	run	96	-" % f, "%s	go	85	-" % f], [f], update=1,
                        notes=[note(f, 10, 90), note(f, 40, 99)])
    expect("a grown function fails beside a shrunk one", rc, 1, out)
    expect("update keeps the grown number and lowers the shrunk",
           base, ["%s	go	85	-" % f, "%s	run	90	-" % f], str(base))

    if _recording:
        return 0
    for m in fails:
        print("FAIL " + m)
    print("%d failure(s)" % len(fails) if fails else "ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
