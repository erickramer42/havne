# File/layer: scripts/verify_scope.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest VerifyScopeTest.SelfTest); run by
#          scripts/verify.sh
#
# Which steps a change needs (verify.sh --changed) and which ctest regex
# names a list of suites (verify.sh --suite). The table is conservative:
# a path it doesn't name takes the full run, so a new kind of file costs
# time and never coverage. Only the full run counts as a verify for a PR.
#
#   python3 scripts/verify_scope.py classify < paths      one path per line
#   python3 scripts/verify_scope.py suite Bridge,Qml,c_abi
#   python3 scripts/verify_scope.py selftest
#
# classify prints plan=full|qml|none, reason=, tidy=yes|no and tidy_why=.
# suite prints the regex, then one line per name: the name, a tab and that
# name's own regex, which verify.sh uses to refuse a name matching nothing.

import os
import re
import sys

FULL = "full"
QML = "qml"
NONE = "none"
RANK = {NONE: 0, QML: 1, FULL: 2}

CPP = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc")
TIDY_CONFIG = (".clang-tidy", ".github/clang-tidy-requirements.txt")


def is_build_file(path):
    return os.path.basename(path) == "CMakeLists.txt" or path.endswith(".cmake")


def classify_path(path):
    """The steps one repo-relative path needs: FULL, QML or NONE"""
    if is_build_file(path):
        return FULL
    if path.startswith("docs/"):
        return NONE
    # tests/ can hold a fixture that a test reads, whatever its extension
    if path.endswith(".md") and not path.startswith("tests/"):
        return NONE
    # A new QML file also needs app/CMakeLists.txt, which takes the full run
    if path.endswith(".qml") and path.startswith(("app/qml/", "tests/qml/")):
        return QML
    return FULL


def plural(count, noun):
    return f"{count} {noun}" if count == 1 else f"{count} {noun}s"


def plan(paths):
    """(plan, reason) for the changed paths"""
    if not paths:
        return NONE, "nothing changed"
    kinds = [classify_path(p) for p in paths]
    worst = max(kinds, key=RANK.get)
    if worst == NONE:
        return NONE, "only docs and markdown changed (" + plural(len(paths), "file") + ")"
    if worst == QML:
        return QML, "only QML changed, or QML and docs (" + plural(len(paths), "file") + ")"
    first = next(p for p, k in zip(paths, kinds) if k == FULL)
    if is_build_file(first):
        return FULL, first + " changes the build"
    return FULL, first + " is outside the table of docs and QML"


def tidy(paths):
    """(applies, why): clang-tidy's inputs are C and C++, its config and the build"""
    hits = [p for p in paths
            if p.endswith(CPP) or p in TIDY_CONFIG or is_build_file(p)]
    if hits:
        return "yes", hits[0] + " changed (" + plural(len(hits), "C++ or build file") + ")"
    return "no", "no C++ changed"


def classify_text(text):
    # By line feed alone: splitlines() also splits on what a path may hold
    lines = (line.rstrip("\r") for line in text.split("\n"))
    paths = list(dict.fromkeys(line for line in lines if line))
    kind, reason = plan(paths)
    applies, why = tidy(paths)
    return f"plan={kind}\nreason={reason}\ntidy={applies}\ntidy_why={why}\n"


def suite_name(name):
    """(test-name stem, whether it is a whole test's name) for one --suite name"""
    if not re.fullmatch(r"[A-Za-z0-9_]+", name):
        raise ValueError(f"'{name}' isn't a suite name (-R takes a regex)")
    # c_abi and c_abi_shared are single tests, not gtest suites with cases
    if name.islower():
        return name, True
    if name.endswith(("Test", "Fixture")):
        return name, False
    return name + "Test", False


def alternatives(names):
    return names[0] if len(names) == 1 else "(" + "|".join(names) + ")"


def suite_regex(spec):
    """(the regex for every name, [(name, its own regex)])"""
    names = list(dict.fromkeys(n.strip() for n in spec.split(",")))
    suites, whole, each = [], [], []
    for name in names:
        stem, is_whole = suite_name(name)
        (whole if is_whole else suites).append(stem)
        each.append((name, "^" + stem + ("$" if is_whole else r"\.")))
    dotted = alternatives(suites) + r"\." if suites else ""
    exact = alternatives(whole) + "$" if whole else ""
    if dotted and exact:
        return "^(" + dotted + "|" + exact + ")", each
    return "^" + (dotted or exact), each


def selftest():
    fails = []

    def check(what, got, want):
        if got != want:
            fails.append(f"{what}: got {got!r}, want {want!r}")

    for path, want in [
        ("docs/testing.md", NONE),
        ("docs/bench-sessions/2026-10-20/wire.log", NONE),
        ("docs/CMakeLists.txt", FULL),
        ("README.md", NONE),
        ("plugins/README.md", NONE),
        ("core/notes.md", NONE),
        ("tests/fixtures/sample.md", FULL),
        ("app/qml/Main.qml", QML),
        ("tests/qml/tst_phone.qml", QML),
        ("app/qml/CMakeLists.txt", FULL),
        ("tests/qml/helper.js", FULL),
        ("app/other/Thing.qml", FULL),
        ("Main.qml", FULL),
        ("core/src/session.cpp", FULL),
        ("core/include/ecuio/core.h", FULL),
        ("surrogate/main.cpp", FULL),
        ("CMakeLists.txt", FULL),
        ("tests/notices_match.cmake", FULL),
        ("scripts/verify.sh", FULL),
        ("LICENSE", FULL),
        (".github/workflows/ci.yml", FULL),
        ("app/sessionbridge.cpp", FULL),
    ]:
        check(path, classify_path(path), want)

    check("no paths", plan([])[0], NONE)
    check("docs only", plan(["docs/a.md", "README.md"])[0], NONE)
    check("QML only", plan(["app/qml/A.qml", "tests/qml/tst_a.qml"])[0], QML)
    check("QML and docs", plan(["docs/a.md", "app/qml/A.qml"])[0], QML)
    check("docs then QML then C++", plan(["docs/a.md", "app/qml/A.qml", "core/a.cpp"])[0], FULL)
    check("C++ first", plan(["core/a.cpp", "docs/a.md"])[0], FULL)
    check("full names its path", plan(["docs/a.md", "core/a.cpp"])[1],
          "core/a.cpp is outside the table of docs and QML")
    check("a build file says so", plan(["app/CMakeLists.txt"])[1], "app/CMakeLists.txt changes the build")
    check("one file", plan(["docs/a.md"])[1], "only docs and markdown changed (1 file)")
    check("two files", plan(["docs/a.md", "b.md"])[1], "only docs and markdown changed (2 files)")

    for paths, want in [
        (["docs/a.md"], "no"),
        (["app/qml/A.qml"], "no"),
        ([], "no"),
        (["scripts/verify.sh"], "no"),
        (["core/a.cpp"], "yes"),
        (["core/a.c"], "yes"),
        (["core/include/ecuio/core.h"], "yes"),
        (["app/CMakeLists.txt"], "yes"),
        (["cmake/x.cmake"], "yes"),
        ([".clang-tidy"], "yes"),
        ([".github/clang-tidy-requirements.txt"], "yes"),
        (["docs/a.md", "tests/a.hpp"], "yes"),
    ]:
        check(f"tidy {paths}", tidy(paths)[0], want)

    check("the text", classify_text("app/qml/A.qml\n\napp/qml/A.qml\r\ndocs/b.md\n"),
          "plan=qml\nreason=only QML changed, or QML and docs (2 files)\ntidy=no\ntidy_why=no C++ changed\n")
    check("the text of nothing", classify_text(""),
          "plan=none\nreason=nothing changed\ntidy=no\ntidy_why=no C++ changed\n")
    check("a form feed and a next line are inside a path",
          classify_text("docs/a\x0cb.md\ndocs/c\x85d.md\n").split("\n")[1],
          "reason=only docs and markdown changed (2 files)")
    check("a path's trailing space is its own",
          classify_text("README.md \n").split("\n")[0], "plan=full")

    for spec, want in [
        ("Bridge,Qml", r"^(BridgeTest|QmlTest)\."),
        ("Bridge", r"^BridgeTest\."),
        ("BridgeTest", r"^BridgeTest\."),
        ("FakeFixture", r"^FakeFixture\."),
        ("c_abi", r"^c_abi$"),
        ("c_abi,c_abi_shared", r"^(c_abi|c_abi_shared)$"),
        ("Bridge,c_abi", r"^(BridgeTest\.|c_abi$)"),
        ("Bridge,Qml,c_abi,c_abi_shared", r"^((BridgeTest|QmlTest)\.|(c_abi|c_abi_shared)$)"),
        (" Bridge , Qml ", r"^(BridgeTest|QmlTest)\."),
        ("Bridge,Bridge", r"^BridgeTest\."),
    ]:
        check(spec, suite_regex(spec)[0], want)
    check("each name's regex", suite_regex("Bridge,c_abi")[1],
          [("Bridge", r"^BridgeTest\."), ("c_abi", r"^c_abi$")])

    for spec in ["", "Bridge,", ",Qml", "Bridge,,Qml", "Bridge.Foo", "^Bridge", "Br idge"]:
        try:
            got = suite_regex(spec)
        except ValueError:
            continue
        fails.append(f"'{spec}' was accepted: {got!r}")

    for line in fails:
        print("FAIL", line, file=sys.stderr)
    print(f"verify_scope selftest: {len(fails)} failed")
    return 1 if fails else 0


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command == "selftest" and len(argv) == 2:
        return selftest()
    if command == "classify" and len(argv) == 2:
        data = sys.stdin.buffer.read().decode("utf-8", "replace")
        sys.stdout.write(classify_text(data))
        return 0
    if command == "suite" and len(argv) == 3:
        try:
            regex, each = suite_regex(argv[2])
        except ValueError as error:
            print(f"verify_scope: {error}", file=sys.stderr)
            return 2
        print(regex)
        for name, one in each:
            print(f"{name}\t{one}")
        return 0
    print("usage: verify_scope.py classify | suite <names> | selftest", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
