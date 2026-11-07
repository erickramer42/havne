# File/layer: scripts/exports_check.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest ExportsTest.SelfTest);
#          INTEGRATION, ExportsTest.TheLibraryExportsTheDeclarations and
#          ExportsTest.EachAllocationHasItsRelease (tests/CMakeLists.txt,
#          where HAVNE_SHARED_CORE builds the library)
#
# The shared core's two promises (ADR-0053's appendix "the shared core
# as built"):
#   exports    the library exports exactly scripts/ecuio_exports.py's
#              list: every declared function, and no other symbol. The
#              declarations are read from the headers here, not from
#              the lists the library was linked with, so a list that
#              lost a name fails too. A
#              declared function core never defines fails (a consumer's
#              lookup would), and so does anything else the library
#              lets out (a C++ symbol, an internal seam), which a
#              consumer could come to depend on. The table is read with
#              the platform's own tool: dumpbin /exports (beside
#              link.exe), nm -D --defined-only (ELF), nm -gU (Mach-O,
#              each name's leading underscore dropped).
#   releasers  everything core hands out has its release in the same
#              header: a function whose non-const T** parameter (an
#              out-parameter core fills with something it made), or
#              whose non-const T* return, gives the caller a T needs a
#              function there named with free, close, destroy or
#              unregister (a word of its name) whose first parameter is
#              T*. A const T** or const T* is borrowed (a set the
#              registry keeps, a name core holds) and needs none.
#
#   python3 scripts/exports_check.py exports <library> <dumpbin|elf|macho> <tool> <header> ...
#   python3 scripts/exports_check.py releasers <header> ...
#   python3 scripts/exports_check.py selftest
# Exit 0: kept, 1: broken (each finding a line), 2: an error.

import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile

# No __pycache__ in the source tree: ctest runs this from it
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import api_check  # noqa: E402
import ecuio_exports  # noqa: E402

HEX8 = re.compile(r"[0-9A-Fa-f]{8}$")
RELEASE_WORD = re.compile(r"(^|_)(free|close|destroy|unregister)(_|$)")
# What the platform's linker adds to every library on its own, never a
# name of ours. None on ELF and PE so far; Mach-O's image header
MADE_BY_THE_LINKER = {"macho": {"_mh_dylib_header"}}


# --- exports ---------------------------------------------------------------

def dumpbin_names(text):
    """The names in `dumpbin /exports` output: the rows under its
    'ordinal hint RVA name' header, up to the Summary"""
    names, table = [], False
    for line in text.splitlines():
        t = line.split()
        if t[:4] == ["ordinal", "hint", "RVA", "name"]:
            table = True
        elif table and t[:1] == ["Summary"]:
            break
        elif table and len(t) >= 3 and t[0].isdigit():
            # a forwarded export has no RVA
            names.append(t[3] if len(t) >= 4 and HEX8.match(t[2]) else t[2])
    return names


def nm_names(text, macho):
    """The names in nm's output (`<value> <type> <name>`), a version
    suffix (name@VER) dropped, and on Mach-O the leading underscore"""
    names = []
    for line in text.splitlines():
        t = line.split()
        if len(t) < 2:
            continue
        name = t[-1].split("@")[0]
        if macho and name.startswith("_"):
            name = name[1:]
        names.append(name)
    return names


def exports_findings(exported, listed, fmt):
    """Findings: a listed name not exported, an exported one not listed"""
    exported = set(exported) - MADE_BY_THE_LINKER.get(fmt, set())
    listed = set(listed)
    return ([f"declared but not exported: {n}" for n in sorted(listed - exported)]
            + [f"exported but not declared: {n}" for n in sorted(exported - listed)])


def library_exports(library, fmt, tool):
    if fmt == "dumpbin":
        cmd = [tool, "/nologo", "/exports", library]
    elif fmt == "elf":
        cmd = [tool, "-D", "--defined-only", library]
    elif fmt == "macho":
        cmd = [tool, "-gU", library]
    else:
        raise ValueError(f"unknown format {fmt} (dumpbin, elf or macho)")
    run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    if run.returncode != 0:
        raise OSError(f"{' '.join(cmd)} failed ({run.returncode}): {run.stderr.strip()}")
    return dumpbin_names(run.stdout) if fmt == "dumpbin" else nm_names(run.stdout, fmt == "macho")


# --- releasers -------------------------------------------------------------

def signature(text, name):
    """(the return type's tokens, each parameter's tokens) of a
    declaration api_check.declarations wrote without parameter names"""
    at = text.index(" " + name + " (") if (" " + name + " (") in text else text.index(name)
    ret = text[:at].split()
    inner = text[text.index("(", at) + 1:text.rindex(")")]
    params = [p.split() for p in inner.split(", ")] if inner.strip() else []
    return ret, params


def handed_out(ret, params):
    """The types a function gives its caller to own: (T, where), from a
    non-const T** parameter or a non-const T* return"""
    out = []
    for i, p in enumerate(params, 1):
        if len(p) >= 3 and p[-2:] == ["*", "*"] and "const" not in p and p != ["void", "*", "*"]:
            out.append((" ".join(p[:-2]), f"its parameter {i}"))
    if len(ret) >= 2 and ret[-1] == "*" and ret[-2] != "*" and "const" not in ret and ret != ["void", "*"]:
        out.append((" ".join(ret[:-1]), "its return"))
    return out


def releaser_findings(header, functions):
    """Findings for one header: {name: its declaration's text}"""
    released = set()
    for name, text in functions.items():
        if RELEASE_WORD.search(name):
            _, params = signature(text, name)
            # T* first: "const T *" or "T * *" names another type than T
            if params and params[0][-1:] == ["*"]:
                released.add(" ".join(params[0][:-1]))
    findings = []
    for name, text in sorted(functions.items()):
        ret, params = signature(text, name)
        for t, where in handed_out(ret, params):
            if t not in released:
                findings.append(
                    f"{header}: {name} hands out a {t}* ({where}) that no function in "
                    f"{header} releases (a _free, _close, _destroy or _unregister taking {t}* first)")
    return findings


def check_releasers(headers):
    findings = []
    for path in headers:
        with open(path, encoding="utf-8") as f:
            functions = api_check.declarations(f.read())["function"]
        findings.extend(releaser_findings(os.path.basename(path), functions))
    return findings


# --- self-test -------------------------------------------------------------

HEADER = """
#ifndef ECUIO_T_H
#define ECUIO_T_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ecuio_t_thing ecuio_t_thing_t;
typedef struct ecuio_t_set ecuio_t_set_t;
ecuio_status_t ecuio_t_open(const char* uri, ecuio_t_thing_t** out_thing);
void ecuio_t_close(ecuio_t_thing_t* thing);
ecuio_status_t ecuio_t_text(const char* uri, char** out_text);
void ecuio_t_free_text(char* text);
ecuio_status_t ecuio_t_lookup(const char* name, const ecuio_t_set_t** out_set);
const char* ecuio_t_name(const ecuio_t_thing_t* thing);
#ifdef __cplusplus
}
#endif
#endif
"""

DUMPBIN = """
Dump of file ecuio.dll

File Type: DLL

  Section contains the following exports for ecuio.dll

    00000000 characteristics
    FFFFFFFF time date stamp
        0.00 version
           1 ordinal base
           3 number of functions
           3 number of names

    ordinal hint RVA      name

          1    0 00012A30 ecuio_init = ecuio_init
          2    1 00012B10 ecuio_shutdown
          3    2          ecuio_moved (forwarded to other.ecuio_moved)

  Summary

        1000 .data
"""

NM_ELF = """
0000000000012340 T ecuio_init
0000000000012400 T ecuio_shutdown@@ECUIO_0
"""

NM_MACHO = """
0000000000003f50 T _ecuio_init
0000000000003f90 T _ecuio_shutdown
0000000000000000 T __mh_dylib_header
"""


def selftest():
    failed, ran = [], []

    def expect(name, ok):
        ran.append(name)
        if not ok:
            failed.append(name)
            print(f"FAIL  {name}")

    two = ["ecuio_init", "ecuio_shutdown"]
    expect("dumpbin's table is read, a forwarded export too",
           dumpbin_names(DUMPBIN) == two + ["ecuio_moved"])
    expect("nm's ELF names lose their version", nm_names(NM_ELF, False) == two)
    expect("nm's Mach-O names lose their underscore",
           nm_names(NM_MACHO, True) == two + ["_mh_dylib_header"])
    expect("the same names pass", not exports_findings(two, two, "elf"))
    expect("Mach-O's image header isn't ours",
           not exports_findings(nm_names(NM_MACHO, True), two, "macho"))
    expect("a declared name not exported fails",
           exports_findings(["ecuio_init"], two, "elf") == ["declared but not exported: ecuio_shutdown"])
    expect("an exported name not declared fails",
           exports_findings(two + ["_ZN5ecuio4testEv"], two, "elf")
           == ["exported but not declared: _ZN5ecuio4testEv"])
    expect("the image header counts on ELF",
           exports_findings(two + ["_mh_dylib_header"], two, "elf") != [])

    def rel(text):
        return releaser_findings("t.h", api_check.declarations(text)["function"])

    def mutant(old, new):
        assert HEADER.count(old) == 1, old
        return HEADER.replace(old, new)

    expect("each handle with its release passes", rel(HEADER) == [])
    gone = rel(mutant("void ecuio_t_close(ecuio_t_thing_t* thing);\n", ""))
    expect("a handle without its release fails",
           len(gone) == 1 and "ecuio_t_open" in gone[0] and "ecuio_t_thing_t*" in gone[0])
    expect("a release taking a const T* doesn't count",
           len(rel(mutant("void ecuio_t_close(ecuio_t_thing_t* thing)",
                          "void ecuio_t_close(const ecuio_t_thing_t* thing)"))) == 1)
    expect("a release taking T* second doesn't count",
           len(rel(mutant("void ecuio_t_close(ecuio_t_thing_t* thing)",
                          "void ecuio_t_close(int how, ecuio_t_thing_t* thing)"))) == 1)
    expect("a release taking T** first doesn't count",
           any("ecuio_t_open" in f for f in rel(mutant("void ecuio_t_close(ecuio_t_thing_t* thing)",
                                                       "void ecuio_t_close(ecuio_t_thing_t** thing)"))))
    expect("a name without a release word doesn't count",
           len(rel(mutant("ecuio_t_close(", "ecuio_t_closed("))) == 1)
    expect("a string handed out needs its free",
           len(rel(mutant("void ecuio_t_free_text(char* text);\n", ""))) == 1)
    expect("a const T** is borrowed, the same T** without const isn't",
           rel(HEADER) == [] and len(rel(mutant("const ecuio_t_set_t** out_set",
                                                "ecuio_t_set_t** out_set"))) == 1)
    made = rel(mutant("const char* ecuio_t_name(", "ecuio_t_set_t* ecuio_t_make("))
    expect("a non-const T* return needs its release",
           len(made) == 1 and "its return" in made[0])
    expect("a const T* return is borrowed, the same T* without const isn't",
           len(rel(mutant("const char* ecuio_t_name(", "char* ecuio_t_name("))) == 0
           and len(rel(mutant("const char* ecuio_t_name(", "ecuio_t_set_t* ecuio_t_name("))) == 1)
    expect("a release in another header doesn't count",
           len(releaser_findings("t.h", {k: v for k, v in api_check.declarations(HEADER)["function"].items()
                                         if k != "ecuio_t_close"})) == 1)

    # ecuio_exports.py: its forms, and its refusals
    with tempfile.TemporaryDirectory() as tmp:
        h = os.path.join(tmp, "t.h")
        with open(h, "w", encoding="utf-8") as f:
            f.write(HEADER)
        out = os.path.join(tmp, "out")
        quiet = io.StringIO()
        rc = ecuio_exports.main(["ecuio_exports.py", out, h])
        names = ["ecuio_t_close", "ecuio_t_free_text", "ecuio_t_lookup", "ecuio_t_name",
                 "ecuio_t_open", "ecuio_t_text"]

        def read(n):
            with open(os.path.join(out, n), encoding="utf-8") as f:
                return f.read()

        expect("the list is every declared function, sorted",
               rc == 0 and read("ecuio.exports").split() == names)
        expect("the .def exports each name",
               rc == 0 and read("ecuio.def").split() == ["LIBRARY", "ecuio", "EXPORTS"] + names)
        expect("the version script lets each name out and nothing else",
               rc == 0 and read("ecuio.map").split()
               == ["{", "global:"] + [n + ";" for n in names] + ["local:", "*;", "};"])
        expect("Apple's list prefixes each name",
               rc == 0 and read("ecuio.exp").split() == ["_" + n for n in names])
        with open(h, "w", encoding="utf-8") as f:
            f.write(HEADER.replace("void ecuio_t_close(", "void t_close("))
        with contextlib.redirect_stderr(quiet):
            rc = ecuio_exports.main(["ecuio_exports.py", out, h])
        expect("a name without the prefix is refused", rc == 2 and "t_close" in quiet.getvalue())
        with open(h, "w", encoding="utf-8") as f:
            f.write("#define ECUIO_T 1\n")
        with contextlib.redirect_stderr(quiet):
            rc = ecuio_exports.main(["ecuio_exports.py", out, h])
        expect("a header declaring nothing is refused", rc == 2)

    print(f"{len(ran) - len(failed)}/{len(ran)} cases passed")
    return 0 if not failed else 1


def main(argv):
    try:
        if argv[1:2] == ["selftest"] and len(argv) == 2:
            return selftest()
        if argv[1:2] == ["exports"] and len(argv) >= 6:
            library, fmt, tool = argv[2:5]
            names = sorted(ecuio_exports.declared(argv[5:]))
            findings = exports_findings(library_exports(library, fmt, tool), names, fmt)
            what = f"{len(names)} declared functions exported, nothing else"
        elif argv[1:2] == ["releasers"] and len(argv) >= 3:
            findings = check_releasers(argv[2:])
            what = f"everything {len(argv) - 2} headers hand out has its release"
        else:
            print("usage: exports_check.py exports <library> <dumpbin|elf|macho> <tool> <header> ..."
                  " | releasers <header> ... | selftest", file=sys.stderr)
            return 2
    except (OSError, ValueError) as e:
        print(f"exports_check: {e}", file=sys.stderr)
        return 2
    for f in findings:
        print(f)
    if not findings:
        print(f"ok  {what}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
