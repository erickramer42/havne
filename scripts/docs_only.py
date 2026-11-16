# File/layer: scripts/docs_only.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest DocsOnlyTest.SelfTest); run by
#          ci.yml's scope job and by release.yml's CI gate
#
# Whether a diff is docs alone, judged from the changed paths and modes and
# never from a commit message, and the light check CI runs when it is.
#
#   python3 -P scripts/docs_only.py diff [--lint] <git diff arguments>
#   python3 -P scripts/docs_only.py classify < paths    NUL-separated paths
#   python3 -P scripts/docs_only.py lint                relative links in the docs
#   python3 -P scripts/docs_only.py selftest
#
# diff and classify print docs_only=true|false and reason=<why>, and exit 0
# whatever the answer: a git failure, an empty diff and an unreadable mode
# all read false, so a caller that gets nothing back runs everything.
# --lint adds the check below when the answer is true; its failures exit 1.
#
# The list is closed and narrower than scripts/verify_scope.py's NONE table,
# which also takes a Markdown file anywhere outside tests/ and everything
# under docs/ (its logs and recordings too): local verify only skips steps,
# while this list decides that CI runs none. Every path accepted here is
# NONE there (the self-test checks it), never the other way round. Allowed:
#   - README.md, STATUS.md, CHANGELOG.md, CONTRIBUTING.md, SECURITY.md and
#     CODE_OF_CONDUCT.md at the top. Not DISCLAIMER.md: the Windows zip
#     ships it.
#   - Markdown anywhere under docs/ (docs/releases/*.md included).
#   - Images under docs/images/: png, gif, jpg, jpeg.
# Only an added, modified or deleted regular file (mode 100644) counts: a
# symlink, an executable, a submodule or a type change is not docs.

import importlib.util
import os
import re
import subprocess
import sys

TOP_LEVEL = ("README.md", "STATUS.md", "CHANGELOG.md", "CONTRIBUTING.md",
             "SECURITY.md", "CODE_OF_CONDUCT.md")
PART = r"[A-Za-z0-9_][A-Za-z0-9_. -]*"
DOCS_MD = re.compile(r"docs(/" + PART + r")*/[A-Za-z0-9_][A-Za-z0-9_. -]*\.md")
DOCS_IMAGE = re.compile(r"docs/images(/" + PART + r")*/[A-Za-z0-9_][A-Za-z0-9_. -]*\.(png|gif|jpe?g)")
REGULAR = ("000000", "100644")


def is_docs_path(path):
    if not path or ".." in path.split("/") or "\\" in path or path.startswith("/"):
        return False
    if path in TOP_LEVEL:
        return True
    return bool(DOCS_MD.fullmatch(path) or DOCS_IMAGE.fullmatch(path))


def judge(paths):
    """(docs_only, reason) for repo-relative paths"""
    if not paths:
        return False, "nothing changed"
    for path in paths:
        if not is_docs_path(path):
            return False, path + " is outside the docs list"
    n = len(paths)
    return True, "only docs changed (" + (f"{n} file" if n == 1 else f"{n} files") + ")"


def parse_raw(data):
    """paths from `git diff --raw -z --no-renames`, or (None, why) for any doubt"""
    tokens = data.split("\0")
    if tokens and tokens[-1] == "":
        tokens.pop()
    paths = []
    i = 0
    while i < len(tokens):
        meta = tokens[i]
        fields = meta.split(" ")
        if not meta.startswith(":") or len(fields) != 5 or i + 1 >= len(tokens):
            return None, "an unreadable diff entry"
        old_mode, new_mode, status = fields[0][1:], fields[1], fields[4]
        if status not in ("A", "M", "D"):
            return None, tokens[i + 1] + " has status " + status
        if old_mode not in REGULAR or new_mode not in REGULAR:
            return None, tokens[i + 1] + " is not a plain file (mode 100644)"
        paths.append(tokens[i + 1])
        i += 2
    return paths, ""


def classify_raw(data):
    paths, why = parse_raw(data)
    if paths is None:
        return False, why
    return judge(paths)


def git_diff(args):
    cmd = ["git", "diff", "--raw", "-z", "--no-renames", "--no-ext-diff",
           "--no-textconv", "--no-abbrev"] + args + ["--"]
    done = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if done.returncode != 0:
        return None, "git diff failed: " + done.stderr.decode("utf-8", "replace").strip()
    return done.stdout.decode("utf-8", "surrogateescape"), ""


def link_targets(text):
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"`[^`\n]*`", "", text)
    for m in re.finditer(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", text):
        target = m.group(1)
        if re.match(r"[A-Za-z][A-Za-z0-9+.-]*:|#|<", target):
            continue
        path = target.split("#")[0]
        if path:
            yield target, path


def lint(root="."):
    """Every docs file is UTF-8 and every relative link in it names a file that exists"""
    listed = subprocess.run(["git", "ls-files", "-z"], cwd=root, stdout=subprocess.PIPE, check=True)
    fails = []
    for rel in listed.stdout.decode("utf-8", "surrogateescape").split("\0"):
        if not rel.endswith(".md") or not is_docs_path(rel):
            continue
        try:
            with open(os.path.join(root, rel), "rb") as handle:
                text = handle.read().decode("utf-8")
        except (OSError, UnicodeDecodeError) as error:
            fails.append(f"{rel}: unreadable ({error})")
            continue
        for target, path in link_targets(text):
            where = os.path.normpath(os.path.join(root, os.path.dirname(rel), path))
            if not os.path.exists(where):
                fails.append(f"{rel}: the link {target} names no file")
    return fails


def report(docs_only, reason):
    print("docs_only=" + ("true" if docs_only else "false"))
    print("reason=" + reason)


def verify_scope():
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_scope.py")
    spec = importlib.util.spec_from_file_location("verify_scope", here)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def selftest():
    fails = []

    def check(what, got, want):
        if got != want:
            fails.append(f"{what}: got {got!r}, want {want!r}")

    docs = ["README.md", "STATUS.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md",
            "CODE_OF_CONDUCT.md", "docs/ci.md", "docs/releases/v0.9.0.md", "docs/adr/0065-x.md",
            "docs/bench-sessions/2026-10-20/notes.md", "docs/images/live.png",
            "docs/images/sub/dash-1.gif", "docs/images/a.jpg", "docs/images/a.jpeg"]
    other = ["DISCLAIMER.md", "LICENSE", "CMakeLists.txt", "docs/CMakeLists.txt", "docs/x.txt",
             "docs/bench-sessions/2026-10-20/wire.log", "docs/images/a.svg", "docs/images/a.png.exe",
             "docs/.hidden.md", "docs/../core/a.md", "docs//a.md", "docs/a.md/b.py",
             "tests/fixtures/sample.md", "tests/README.md", "schema/README.md", "plugins/README.md",
             "core/notes.md", "app/qml/Main.qml", "scripts/docs_only.py", ".github/workflows/ci.yml",
             "packaging/windows/README.txt", "packaging/README.md", "README.markdown", "readme.md",
             "docs\\a.md", "/docs/a.md", "docs/a.md ", "", "docs/images/a.PNG", "DOCS/a.md"]
    for path in docs:
        check("docs " + path, is_docs_path(path), True)
    for path in other:
        check("not docs " + repr(path), is_docs_path(path), False)

    check("empty", judge([])[0], False)
    check("docs", judge(["docs/a.md", "README.md"]), (True, "only docs changed (2 files)"))
    check("one", judge(["docs/a.md"])[1], "only docs changed (1 file)")
    check("one stray", judge(["docs/a.md", "core/a.cpp"]), (False, "core/a.cpp is outside the docs list"))

    def raw(*entries):
        return "".join(f":{o} {n} {'0' * 40} {'1' * 40} {s}\0{p}\0" for o, n, s, p in entries)

    check("raw docs", classify_raw(raw(("100644", "100644", "M", "docs/a.md")))[0], True)
    check("raw added and deleted", classify_raw(raw(("000000", "100644", "A", "docs/a.md"),
                                                    ("100644", "000000", "D", "docs/b.md")))[0], True)
    check("raw empty", classify_raw("")[0], False)
    check("raw symlink", classify_raw(raw(("100644", "120000", "M", "docs/a.md")))[0], False)
    check("raw executable", classify_raw(raw(("100644", "100755", "M", "docs/a.md")))[0], False)
    check("raw submodule", classify_raw(raw(("160000", "160000", "M", "docs/a.md")))[0], False)
    check("raw type change", classify_raw(raw(("100644", "120000", "T", "docs/a.md")))[0], False)
    check("raw rename", classify_raw(raw(("100644", "100644", "R100", "docs/a.md")))[0], False)
    check("raw code beside docs", classify_raw(raw(("100644", "100644", "M", "docs/a.md"),
                                                   ("100644", "100644", "M", "core/a.cpp")))[0], False)
    check("raw torn", classify_raw(":100644 100644 aa bb M\0")[0], False)
    check("raw no meta", classify_raw("docs/a.md\0")[0], False)

    check("link targets", [t for t, _ in link_targets(
        "[a](b.md) [c](https://x/y) [d](#e) [f](g.md#h) `[i](j.md)` [k](l.md \"t\")\n```\n[m](n.md)\n```")],
        ["b.md", "g.md#h", "l.md"])

    # Everything accepted here is NONE in verify_scope's table, a path outside
    # its docs rows included
    scope = verify_scope()
    for path in docs:
        check("verify_scope agrees on " + path, scope.classify_path(path), scope.NONE)

    for line in fails:
        print("FAIL", line, file=sys.stderr)
    print(f"docs_only selftest: {len(fails)} failed")
    return 1 if fails else 0


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command == "selftest" and len(argv) == 2:
        return selftest()
    if command == "classify" and len(argv) == 2:
        paths = [p for p in sys.stdin.buffer.read().decode("utf-8", "surrogateescape").split("\0") if p]
        report(*judge(paths))
        return 0
    if command == "lint" and len(argv) == 2:
        fails = lint()
        for line in fails:
            print("FAIL", line, file=sys.stderr)
        print(f"docs lint: {len(fails)} problem(s)")
        return 1 if fails else 0
    if command == "diff" and len(argv) > 2:
        args = argv[2:]
        want_lint = args[0] == "--lint"
        if want_lint:
            args = args[1:]
        if not args:
            print("usage: docs_only.py diff [--lint] <git diff arguments>", file=sys.stderr)
            return 2
        data, why = git_diff(args)
        docs_only, reason = (False, why) if data is None else classify_raw(data)
        report(docs_only, reason)
        if docs_only and want_lint:
            fails = lint()
            for line in fails:
                print("FAIL", line, file=sys.stderr)
            return 1 if fails else 0
        return 0
    print("usage: docs_only.py diff [--lint] <git diff args> | classify | lint | selftest", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
