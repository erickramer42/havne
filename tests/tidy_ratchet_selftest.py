# File/layer: tests/tidy_ratchet_selftest.py | test-only, Python 3.8+, no packages
# Testing: UNIT, over made-up size.txt and baseline files (ctest TidyRatchetTest.SelfTest)
#
# Runs scripts/tidy-size-ratchet.sh over fixtures and checks its exit code:
# a clean run, a function the baseline doesn't list, a repeated name with too
# few baseline lines, a size line the script can't read, a stale line and
# the update that removes one. Skips (77) where no bash is found.
import os
import shutil
import subprocess
import sys
import tempfile

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


def run(bash, size_lines, base_lines, files, update=0):
    with tempfile.TemporaryDirectory() as d:
        for f in files:
            os.makedirs(os.path.dirname(os.path.join(d, f)) or d, exist_ok=True)
            open(os.path.join(d, f), "w").write("\n")
        for name, lines in (("size.txt", size_lines), ("base.txt", base_lines),
                            ("checked.txt", files)):
            with open(os.path.join(d, name), "w", newline="\n") as fh:
                fh.write("".join(x + "\n" for x in lines))
        p = subprocess.run([bash, SCRIPT, "size.txt", "base.txt", "checked.txt", "0", str(update)],
                           cwd=d, capture_output=True, text=True)
        base_after = open(os.path.join(d, "base.txt"), newline="").read().splitlines()
        return p.returncode, p.stdout + p.stderr, base_after


def main():
    bash = find_bash()
    if not bash:
        print("no bash found")
        return 77
    f = "core/src/a.cpp"
    fails = []

    def expect(label, got, want, out=""):
        if got != want:
            fails.append("%s: exit %s, wanted %s\n%s" % (label, got, want, out))

    rc, out, _ = run(bash, [warn(f, 10, "run")], ["%s\trun" % f], [f])
    expect("a listed function passes", rc, 0, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 10, "run", "cx")], ["%s\trun" % f], [f])
    expect("size and complexity on one function pass with one line", rc, 0, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "go")], ["%s\trun" % f], [f])
    expect("an unlisted function fails", rc, 1, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "run")], ["%s\trun" % f], [f])
    expect("a second function of a listed name fails", rc, 1, out)
    rc, out, _ = run(bash, [warn(f, 10, "run"), warn(f, 90, "run")],
                     ["%s\trun" % f, "%s\trun" % f], [f])
    expect("a repeated name with a line for each passes", rc, 0, out)
    rc, out, _ = run(bash, [warn("E:/src/" + f, 10, "go")], [], [f])
    expect("a line it cannot read fails", rc, 1, out)
    lam = ("%s:10:5: warning: lambda has cognitive complexity of 50 (threshold 40) "
           "[readability-function-cognitive-complexity]" % f)
    rc, out, _ = run(bash, [warn(f, 10, "run"), lam], ["%s\trun" % f], [f])
    expect("a lambda's line fails as unreadable", rc, 1, out)
    rc, out, _ = run(bash, [], ["%s\trun" % f], [f])
    expect("a stale baseline line fails", rc, 1, out)
    rc, out, base = run(bash, [warn(f, 10, "run")],
                        ["%s\trun" % f, "%s\trun" % f], [f], update=1)
    expect("update drops one of two lines", rc, 0, out)
    expect("update leaves the other line", base, ["%s\trun" % f], str(base))

    for m in fails:
        print("FAIL " + m)
    print("%d failure(s)" % len(fails) if fails else "ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
