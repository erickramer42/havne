# File/layer: scripts/ecuio_exports.py | Python 3.8+, no packages
# Testing: UNIT, ExportsTest.SelfTest (scripts/exports_check.py's
#          self-test runs its cases); INTEGRATION, ExportsTest.
#          TheLibraryExportsTheDeclarations, over the library built
#          from its lists; run by scripts/abi-check.sh
#
# The shared core's export list (ADR-0053 and its appendix "the shared
# core as built"): every function the public headers declare, and
# nothing else. The headers are core/CMakeLists.txt's
# ECUIO_PUBLIC_HEADERS, the list it installs, and they're read as
# scripts/api_check.py reads them, as declarations. So a function a
# header declares is exported the day it's declared, and a function no
# public header declares (internal.h's seams and test hooks, the
# surrogate client's) never is. The one list goes out in each linker's
# form:
#   ecuio.exports  one name a line, sorted (the check's and the Python
#                  package's list)
#   ecuio.def      MSVC's module definition (link.exe /DEF)
#   ecuio.map      GNU ld's version script (--version-script)
#   ecuio.exp      Apple ld's exported symbols list (-exported_symbols_list)
# A file whose text is unchanged isn't rewritten, so a build that
# regenerates the lists relinks only when they change.
#
#   python3 scripts/ecuio_exports.py <out dir> <header> ...
#   python3 scripts/ecuio_exports.py --tree <source tree> <out dir>
#       (the tree's installed headers: abi-check.sh's base and head)
# Exit 0, or 2 on an error (a header missing, a declared name without
# the ecuio_ prefix, no names at all).

import os
import sys

# No __pycache__ in the source tree: the build and ctest run this from it
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import api_check  # noqa: E402

PREFIX = "ecuio_"


def declared(headers):
    """The functions the headers declare: {name: its header's file name}"""
    names = {}
    for path in headers:
        with open(path, encoding="utf-8") as f:
            found = api_check.declarations(f.read())["function"]
        for name in found:
            names[name] = os.path.basename(path)
    return names


def tree_headers(tree):
    """A source tree's installed headers, as paths"""
    return [os.path.join(tree, "core", "include", "ecuio", h)
            for h in api_check.installed_headers(tree)]


def forms(names):
    """{file name: its text}, each linker's form of the sorted names"""
    names = sorted(names)
    return {
        "ecuio.exports": "".join(f"{n}\n" for n in names),
        "ecuio.def": "LIBRARY ecuio\nEXPORTS\n" + "".join(f"    {n}\n" for n in names),
        "ecuio.map": "{\n  global:\n" + "".join(f"    {n};\n" for n in names)
                     + "  local:\n    *;\n};\n",
        "ecuio.exp": "".join(f"_{n}\n" for n in names),
    }


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main(argv):
    args = argv[1:]
    try:
        if args[:1] == ["--tree"] and len(args) == 3:
            headers, out = tree_headers(args[1]), args[2]
        elif len(args) >= 2 and not args[0].startswith("-"):
            out, headers = args[0], args[1:]
        else:
            print("usage: ecuio_exports.py <out dir> <header> ... | --tree <source tree> <out dir>",
                  file=sys.stderr)
            return 2
        names = declared(headers)
    except (OSError, ValueError) as e:
        print(f"ecuio_exports: {e}", file=sys.stderr)
        return 2
    wrong = sorted(n for n in names if not n.startswith(PREFIX))
    if wrong:
        print(f"ecuio_exports: declared without the {PREFIX} prefix: "
              + ", ".join(f"{n} ({names[n]})" for n in wrong), file=sys.stderr)
        return 2
    if not names:
        print("ecuio_exports: the headers declare no functions", file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    for name, text in forms(names).items():
        write_if_changed(os.path.join(out, name), text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
