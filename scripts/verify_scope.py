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
# classify prints plan=full|areas|qml|none, reason=, tidy=yes|no and tidy_why=,
# and for plan=areas also regex= (the ctest -R that runs the areas' suites) and
# cross=yes|no (whether the x86 steps apply). Areas are the table below: a
# path in core/src, cli/, app/ or a test file maps to the groups of suites that
# read its output; a path the table doesn't name takes the full run. plan=qml
# also prints a regex= when every QML path is a tests/qml test file or kit:
# the QmlTest of each file that is, or derives from, a changed one.
# suite prints the regex, then one line per name: the name, a tab and that
# name's own regex, which verify.sh uses to refuse a name matching nothing.

import os
import re
import sys
import tempfile

FULL = "full"
QML = "qml"
NONE = "none"
AREAS = "areas"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RANK = {NONE: 0, QML: 1, AREAS: 2, FULL: 3}

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



# Suite groups: the first pattern a ctest suite name (the text before the first
# dot) matches names its group; a name matching none is "engine", so a new
# suite is run by every area that runs the engine.
GROUPS = [
    ("pkg", r"(Python|Wheel|Example|Notices|Sbom|Docs|Tidy|VerifyScope|Golden|RecordingCompare"
            r"|AbiReport|ApiCheck|PlainTextPin|VpicWmi|DtcTitles|Required|Exports|SchemaKeys"
            r"|SchemaFixtures|ProbeNotes|CtestIncludeNames|DiscoveryTimeout)\w*|c_abi\w*"),
    ("elm", r"(Elm327|SerialPorts|CliElm327)\w*"),
    ("j2534", r"(J2534\w*|FakeFixture)"),
    ("can", r"(SocketCan|IsoTp|Vcan|SimBus|SimCarBus|CliCan)\w*"),
    ("sandbox", r"(Sandbox|Wasm|Wamr|Vendored)\w*"),
    ("bridge", r"(BridgeTest|RecordingViewTest|RecordingExportTest|ViewerFramesTest"
               r"|ReadOnceTest|FrameFilterTest|ObdbImportTest)"),
    ("cli", r"Cli\w*"),
    ("qml", r"QmlTest"),
    ("files", r"(Recorder|ArmedRecorder|Recording\w*|Export\w*|CsvExport|FolderExport|Scrub\w*"
              r"|FramesRead|Import\w*|Csv\w*|Candump|CheckRules|Health\w*)Test"),
    ("signals", r"(Signal\w*|Computed\w*|Condition\w*|Plugin\w*|Base64|Json)Test"),
]
# every group but pkg: what a change to the engine can reach (pkg's suites read
# the public ABI and the packages, which a public header or a build file changes)
INNER = ["engine", "files", "signals", "elm", "j2534", "can", "bridge", "cli", "qml"]

# (path pattern, areas) in order, first match wins; an area is (groups, qml)
# where qml is "all", "none" or a word that a QmlTest's file must mention
def area(groups, qml="none", cross=False):
    return {"groups": groups, "qml": qml, "cross": cross}


ENGINE_WIDE = area(INNER, "all", True)
AREA_TABLE = [
    (r"core/src/elm327_|core/src/serial_ports\.", area(["elm", "bridge"], "elm327", True)),
    (r"core/src/j2534_|core/src/surrogate_log_names\.h|surrogate/[^/]*$",
     area(["j2534", "bridge"], "j2534", True)),
    (r"core/src/socketcan_", area(["can"], "none", True)),
    (r"core/src/(recording|recorder|export|import|csv|candump|pcapng|frames|frame_rows|scrub|health"
     r"|folder_files|dtc_titles)", area(["files", "bridge", "cli"], "all", True)),
    (r"core/src/(signal|computed|condition|plugin|json|base64)", area(["signals", "files", "bridge", "cli"], "all", True)),
    (r"core/src/(wasm_|guest_abi)", area(["sandbox", "signals", "bridge"], "none", True)),
    (r"core/src/(request_|obd2|uds_|pid_support|sim|mock_|isotp|frame_decoder|frame_reassembly|talk_seam"
     r"|security_|tier2|profile_|diagnostics|vin|health|host_clock|queue|pull_queue|precise_wait)",
     ENGINE_WIDE),
    (r"core/include/ecuio/internal\.h", ENGINE_WIDE),
    (r"cli/[^/]*\.(cpp|h)$", area(["cli"], "none", False)),
    (r"app/[^/]*\.(cpp|h|mm)$", area(["bridge", "qml"], "all", False)),
    (r"tests/test_elm327", area(["elm"])),
    (r"tests/test_cli_elm327|tests/fake_elm327", area(["elm", "bridge"])),
    (r"tests/(test_j2534|fake_j2534)", area(["j2534"])),
    (r"tests/test_(socketcan|vcan)", area(["can"])),
    (r"tests/test_bridge|tests/test_(one_store|store_)", area(["bridge"])),
    (r"tests/(test_cli|cli_[a-z0-9_]+\.(py|sh|cmake))", area(["cli"])),
    (r"tests/test_(sandbox|wamr|wasm)", area(["sandbox"])),
    (r"tests/test_(signal|computed|condition|plugin)", area(["signals"])),
    (r"tests/test_(recording|recorder|armed|export|csv|import|candump|frames_read|health|scrub)",
     area(["files"])),
    (r"tests/test_(obd2|pacing|request|sim_|session_control|security|key_window|engine_talk|scan_retry|tier2|pick_only)",
     area(["engine"])),
]


def area_of(path):
    """The area a path maps to, or None (a path the table doesn't name)"""
    for pattern, spec in AREA_TABLE:
        if re.match(pattern, path):
            return spec
    return None


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def suite_universe(root):
    """The ctest suite names the tree registers, read from the test sources"""
    names = set()
    tests = os.path.join(root, "tests")
    for fn in os.listdir(tests):
        if fn.startswith("test_") and fn.endswith(".cpp"):
            names.update(re.findall(r"^\s*TEST(?:_F|_P)?\(\s*(\w+)\s*,", read(root, "tests/" + fn), re.M))
    cm = read(root, "tests/CMakeLists.txt")
    names.update(re.findall(r"add_test\(\s*NAME\s+(\w+)[.\s$]", cm))
    names.add("QmlTest")
    return names


def group_of(name):
    for group, pattern in GROUPS:
        if re.fullmatch(pattern, name):
            return group
    return "engine"


def qml_files(root, word):
    """The QmlTest names (file stems) whose file mentions word"""
    d = os.path.join(root, "tests", "qml")
    return sorted(fn[4:-4] for fn in os.listdir(d) if fn.startswith("tst_") and fn.endswith(".qml")
                  and word.lower() in read(root, "tests/qml/" + fn).lower())


def areas_regex(root, specs):
    """(ctest -R regex, suite count, needs cross) for the union of the areas"""
    groups, qml_all, words, cross = set(), False, set(), False
    for spec in specs:
        groups.update(spec["groups"])
        cross = cross or spec["cross"]
        if spec["qml"] == "all":
            qml_all = True
        elif spec["qml"] != "none":
            words.add(spec["qml"])
    suites = sorted(n for n in suite_universe(root) if group_of(n) in groups and n != "QmlTest"
                    and not n.startswith("c_abi"))
    # lower-case names are whole tests, not suites (c_abi, c_abi_shared)
    whole = sorted(n for n in suite_universe(root) if group_of(n) in groups and n.startswith("c_abi"))
    parts = []
    if suites:
        parts.append("(" + "|".join(suites) + r")\.")
    if whole:
        parts.append("(" + "|".join(whole) + ")$")
    qml = sorted({s for w in words for s in qml_files(root, w)})
    if "qml" in groups or qml_all:
        parts.append(r"QmlTest\.")
    elif qml:
        parts.append(r"QmlTest\.(" + "|".join(qml) + ")$")
    return "^(" + "|".join(parts) + ")", cross


def qml_tests_named(root, paths):
    """The QmlTest names to run when every QML path is a test file under
    tests/qml (tst_<name>.qml, or a kit a test file derives from); None when
    any path is app QML or another file, which no table maps to its tests."""
    d = os.path.join(root, "tests", "qml")
    files = {fn: read(root, "tests/qml/" + fn) for fn in os.listdir(d) if fn.endswith(".qml")}
    stems, kits = set(), set()
    for p in paths:
        base = p[len("tests/qml/"):] if p.startswith("tests/qml/") else ""
        if base not in files:
            return None
        if base.startswith("tst_"):
            stems.add(base[4:-4])
        elif base.endswith("Kit.qml"):
            kits.add(base[:-4])
        else:
            return None
    # a kit reaches the test files that derive from it, and the kits that do
    seen = set()
    while kits - seen:
        kit = (kits - seen).pop()
        seen.add(kit)
        for fn, text in files.items():
            if re.search(r"\b" + kit + r"\s*\{", text):
                if fn.startswith("tst_"):
                    stems.add(fn[4:-4])
                elif fn.endswith("Kit.qml"):
                    kits.add(fn[:-4])
    return sorted(stems)


def plural(count, noun):
    return f"{count} {noun}" if count == 1 else f"{count} {noun}s"


def plan(paths, root=None):
    """(plan, reason, regex, cross) for the changed paths; regex and cross are for AREAS"""
    if not paths:
        return NONE, "nothing changed", "", False
    kinds = [classify_path(p) for p in paths]
    worst = max(kinds, key=RANK.get)
    if worst == NONE:
        return NONE, "only docs and markdown changed (" + plural(len(paths), "file") + ")", "", False
    if worst == QML:
        reason = "only QML changed, or QML and docs (" + plural(len(paths), "file") + ")"
        try:
            names = qml_tests_named(root or ROOT, [p for p, k in zip(paths, kinds) if k == QML])
        except OSError:
            names = None
        if names:
            return QML, reason + ": runs " + plural(len(names), "QmlTest"), \
                "^QmlTest\\.(" + "|".join(names) + ")$", False
        return QML, reason, "", False
    first = next(p for p, k in zip(paths, kinds) if k == FULL)
    if is_build_file(first):
        return FULL, first + " changes the build", "", False
    if any(is_build_file(p) for p in paths):
        return FULL, next(p for p in paths if is_build_file(p)) + " changes the build", "", False
    specs = []
    for p, k in zip(paths, kinds):
        if k == FULL:
            spec = area_of(p)
            if spec is None:
                return FULL, p + " is outside the table of areas", "", False
            specs.append(spec)
        elif k == QML:
            specs.append(area(["qml"], "all"))
    try:
        regex, cross = areas_regex(root or ROOT, specs)
    except OSError:
        return FULL, "the suites can't be read from the tests", "", False
    return AREAS, "areas of " + plural(len(paths), "changed file"), regex, cross


def tidy(paths):
    """(applies, why): clang-tidy's inputs are C and C++, its config and the build"""
    hits = [p for p in paths
            if p.endswith(CPP) or p in TIDY_CONFIG or is_build_file(p)]
    if hits:
        return "yes", hits[0] + " changed (" + plural(len(hits), "C++ or build file") + ")"
    return "no", "no C++ changed"


def classify_text(text, root=None):
    # By line feed alone: splitlines() also splits on what a path may hold
    lines = (line.rstrip("\r") for line in text.split("\n"))
    paths = list(dict.fromkeys(line for line in lines if line))
    kind, reason, regex, cross = plan(paths, root)
    applies, why = tidy(paths)
    extra = ""
    if kind == AREAS:
        extra = f"regex={regex}\ncross={'yes' if cross else 'no'}\n"
    elif kind == QML and regex:
        extra = f"regex={regex}\n"
    return f"plan={kind}\nreason={reason}\n{extra}tidy={applies}\ntidy_why={why}\n"


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
          "core/a.cpp is outside the table of areas")
    check("a build file says so", plan(["app/CMakeLists.txt"])[1], "app/CMakeLists.txt changes the build")
    check("one file", plan(["docs/a.md"])[1], "only docs and markdown changed (1 file)")
    check("two files", plan(["docs/a.md", "b.md"])[1], "only docs and markdown changed (2 files)")

    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    universe = suite_universe(root)
    for name in ["CoreTest", "BridgeTest", "Elm327SessionTest", "QmlTest", "CliGoldenTest", "c_abi"]:
        check("universe has " + name, name in universe, True)

    def areas(*paths):
        kind, _, regex, cross = plan(list(paths), root)
        return kind, regex, cross

    kind, regex, cross = areas("core/src/elm327_worker.cpp")
    check("elm327 is areas", kind, AREAS)
    check("elm327 runs its suites", bool(re.search(regex, "Elm327SessionTest.X")), True)
    check("elm327 runs the bridge", bool(re.search(regex, "BridgeTest.X")), True)
    check("elm327 skips j2534", bool(re.search(regex, "J2534HealthTest.X")), False)
    check("elm327 skips python", bool(re.search(regex, "PythonTest.x")), False)
    check("elm327 takes the cross-arch steps", cross, True)
    check("elm327 takes only the QML that names it", bool(re.search(regex, "QmlTest.dashboard")), False)
    kind, regex, cross = areas("cli/cmd_log.cpp")
    check("cli is areas without cross", (kind, cross), (AREAS, False))
    check("cli runs the cli", bool(re.search(regex, "CliLogRunTest.x")), True)
    check("cli skips the bridge", bool(re.search(regex, "BridgeTest.x")), False)
    kind, regex, cross = areas("core/src/request_engine.h")
    check("the engine runs the bridge and QML", all(re.search(regex, n) for n in
          ["BridgeTest.x", "QmlTest.home", "Elm327SessionTest.x", "RequestEngineTest.x"]), True)
    check("the engine skips python and the sandbox", any(re.search(regex, n) for n in
          ["PythonTest.x", "SandboxTest.x", "c_abi"]), False)
    check("a public header is full", areas("core/include/ecuio/core.h")[0], FULL)
    check("an unlisted core file is full", areas("core/src/core.cpp")[0], FULL)
    check("an unlisted test file is full", areas("tests/fake_ecu.h")[0], FULL)
    check("one unlisted path makes it full", areas("cli/cmd_log.cpp", "core/src/core.cpp")[0], FULL)
    check("a build file makes it full", areas("cli/cmd_log.cpp", "cli/CMakeLists.txt")[0], FULL)
    kind, regex, cross = areas("app/sessionbridge.cpp", "app/qml/A.qml")
    check("app C++ with QML", (kind, cross, bool(re.search(regex, "QmlTest.home"))), (AREAS, False, True))
    check("the QML words come from the files", bool(qml_files(root, "elm327")), True)

    tests_dir = os.path.join(root, "tests", "qml")
    some = sorted(fn for fn in os.listdir(tests_dir) if fn.startswith("tst_") and fn.endswith(".qml"))[0]
    kind, _, regex, cross = plan(["tests/qml/" + some], root)
    check("one QML test file runs its own test", (kind, regex, cross), (QML, r"^QmlTest\.(" + some[4:-4] + ")$", False))
    check("a QML test is anchored", bool(re.search(regex, "QmlTest." + some[4:-4] + "_more")), False)
    kit = "PhoneKit"
    users = sorted(fn[4:-4] for fn in os.listdir(tests_dir) if fn.startswith("tst_")
                   and re.search(r"\b" + kit + r"\s*\{", read(root, "tests/qml/" + fn)))
    check("a kit runs the files that derive from it", plan(["tests/qml/" + kit + ".qml"], root)[2],
          r"^QmlTest\.(" + "|".join(users) + ")$")
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "tests", "qml"))
        for fn, text in [("AKit.qml", "TestCase {}"), ("BKit.qml", "AKit { id: t }"),
                         ("tst_one.qml", "BKit {}"), ("tst_two.qml", "AKit {\n}"), ("tst_three.qml", "TestCase {}")]:
            with open(os.path.join(tmp, "tests", "qml", fn), "w", encoding="utf-8") as f:
                f.write(text)
        check("a kit reaches a kit's files", qml_tests_named(tmp, ["tests/qml/AKit.qml"]), ["one", "two"])
        check("a test file alone", qml_tests_named(tmp, ["tests/qml/tst_three.qml"]), ["three"])
        check("a kit nobody derives from", qml_tests_named(tmp, ["tests/qml/tst_three.qml", "tests/qml/BKit.qml"]),
              ["one", "three"])
    check("app QML runs every QML test", plan(["app/qml/Main.qml", "tests/qml/" + some], root)[2:], ("", False))
    check("a QML file that isn't a test or kit runs every QML test", plan(["tests/qml/helper.qml"], root)[2], "")
    check("a QML test and docs", plan(["docs/a.md", "tests/qml/" + some], root)[0], QML)
    check("the text names the regex", classify_text("tests/qml/" + some + "\n", root).split("\n")[2],
          "regex=" + r"^QmlTest\." + "(" + some[4:-4] + ")$")

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
