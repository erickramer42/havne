# File/layer: scripts/api_check.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest ApiCheckTest.SelfTest), over
#          header pairs written to show each rule; run by
#          scripts/abi-check.sh
#
# The C API's source half of the ABI check (ADR-0001). libabigail
# compares the binaries, so it misses a change that keeps the layout: a
# field renamed, or uint32_t becoming int32_t, leaves every offset and
# size as it was and changes what a consumer's source means. This
# compares the installed headers (core/CMakeLists.txt's install list) of
# a release and of HEAD as declarations, so comments and formatting
# never count. It fails when, since the release:
#   - a struct's member is renamed, retyped or gone, except one named
#     reserved (the room a struct grows into);
#   - an enumerator or a #define is gone or has another value;
#   - a function's, a callback's or a typedef's types change (parameter
#     names don't count), or one is gone;
#   - an installed header is gone.
# Additions pass. Where a member sits and how big a struct is stays
# libabigail's (scripts/abi-check.sh). A deliberate change goes in
# abi/api_changes.txt, one name a line with its reason, until the next
# release becomes the base.
#
#   python3 scripts/api_check.py <base tree> <head tree>   exit 0: compatible, 1: not, 2: error
#   python3 scripts/api_check.py selftest

import os
import re
import sys

RESERVED = re.compile(r"reserved", re.IGNORECASE)
IDENT = re.compile(r"[A-Za-z_]\w*$")
TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"'          # a string
    r"|'(?:\\.|[^'\\])*'"         # a character
    r"|\.\.\."                    # varargs
    r"|[A-Za-z_]\w*"              # an identifier or keyword
    r"|\d[\w.]*"                  # a number, suffix included
    r"|\S"                        # any other single character
)
# The words a parameter's type is made of: a parameter ending in one has
# no name to drop ("unsigned int", "void")
TYPE_WORDS = {"void", "char", "short", "int", "long", "float", "double",
              "signed", "unsigned", "const", "volatile", "struct", "enum",
              "union", "_Bool"}


def strip_comments(text):
    """The text with its comments as spaces; strings and characters kept"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " if "\n" not in text[i:j] else "\n" * text[i:j].count("\n"))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_lines(text):
    """(preprocessor lines, the rest's tokens), continuations joined"""
    text = strip_comments(text).replace("\\\r\n", " ").replace("\\\n", " ")
    directives, code = [], []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            directives.append(" ".join(TOKEN.findall(line.lstrip()[1:])))
        else:
            code.append(line)
    return directives, TOKEN.findall("\n".join(code))


def macros(directives):
    """{name: its definition's tokens}, from the #define lines"""
    found = {}
    for d in directives:
        m = re.match(r"define\s+([A-Za-z_]\w*)(.*)$", d)
        if m:
            name, rest = m.group(1), m.group(2).strip()
            # A function-like macro's parameters belong to it: no space
            # between the name and "(" in the source, which the tokens
            # lose, so keep whatever follows as its text
            found[name] = rest
    return found


def top_level(tokens):
    """Each declaration at file scope, as tokens; extern "C" blocks opened"""
    decls, cur, depth, transparent = [], [], 0, []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "extern" and i + 2 < len(tokens) and tokens[i + 1] == '"C"' and tokens[i + 2] == "{":
            transparent.append(depth)
            i += 3
            continue
        if t == "{":
            depth += 1
        elif t == "}":
            if transparent and transparent[-1] == depth:
                transparent.pop()
                i += 1
                continue
            depth -= 1
        if t == ";" and depth == 0:
            if cur:
                decls.append(cur)
            cur = []
        else:
            cur.append(t)
        i += 1
    if cur:
        decls.append(cur)
    return decls


def split_at(tokens, sep):
    """tokens split at sep outside brackets"""
    parts, cur, depth = [], [], 0
    for t in tokens:
        if t in "([{":
            depth += 1
        elif t in ")]}":
            depth -= 1
        if t == sep and depth == 0:
            parts.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        parts.append(cur)
    return parts


def braced(tokens, start):
    """(the tokens inside the braces opening at start, the index after them)"""
    depth = 0
    for j in range(start, len(tokens)):
        if tokens[j] == "{":
            depth += 1
        elif tokens[j] == "}":
            depth -= 1
            if depth == 0:
                return tokens[start + 1:j], j + 1
    raise ValueError("unbalanced braces")


def member_name(tokens):
    """A member's or parameter's name: the last identifier before any
    array brackets or bit-field width"""
    toks = list(tokens)
    if ":" in toks:
        toks = toks[:toks.index(":")]
    while toks and toks[-1] == "]":
        toks = toks[:len(toks) - toks[::-1].index("[") - 1]
    return toks[-1] if toks and IDENT.match(toks[-1]) else None


def without_param_names(tokens):
    """A declaration's text with each parameter's name left out"""
    out, i = [], 0
    while i < len(tokens):
        if tokens[i] == "(":
            # the innermost parameter lists: parentheses holding no "("
            depth, j = 0, i
            for j in range(i, len(tokens)):
                if tokens[j] == "(":
                    depth += 1
                elif tokens[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
            inner = tokens[i + 1:j]
            if "(" in inner:
                out.append("(")
                out.extend(without_param_names(inner))
                out.append(")")
            else:
                params = []
                for p in split_at(inner, ","):
                    name = member_name(p)
                    if (name and len(p) >= 2 and name not in TYPE_WORDS
                            and p[0] != "*" and not (len(p) == 2 and p[0] in ("struct", "enum", "union"))):
                        k = len(p) - 1 - p[::-1].index(name)
                        p = p[:k] + p[k + 1:]
                    params.append(" ".join(p))
                out.append("(" + ", ".join(params) + ")")
            i = j + 1
        else:
            out.append(tokens[i])
            i += 1
    return " ".join(out)


def declarations(text):
    """What a header declares: {kind: {name: what to compare}}"""
    directives, tokens = split_lines(text)
    found = {"macro": macros(directives), "struct": {}, "enum": {},
             "typedef": {}, "function": {}}
    for d in top_level(tokens):
        if d[0] in ("static_assert", "_Static_assert"):
            continue
        typedef = d[0] == "typedef"
        body = d[1:] if typedef else d
        if body and body[0] in ("struct", "union", "enum") and "{" in body:
            kind = body[0]
            inner, after = braced(body, body.index("{"))
            tag = body[1] if body[1] != "{" else None
            alias = body[after] if typedef and after < len(body) else None
            if kind == "enum":
                values = [" ".join(e) for e in split_at(inner, ",") if e]
                found["enum"][alias or tag] = values
            else:
                members = [m for m in split_at(inner, ";") if m]
                found["struct"][tag or alias] = [
                    (member_name(m), " ".join(m)) for m in members]
                if typedef and alias and tag:
                    found["typedef"][alias] = f"{kind} {tag}"
        elif typedef:
            text = without_param_names(body)
            m = re.search(r"\(\s*\*\s*(\w+)\s*\)", " ".join(body))
            name = m.group(1) if m else member_name(body)
            found["typedef"][name] = text
        elif "(" in d:
            name = d[d.index("(") - 1]
            found["function"][name] = without_param_names(d)
    return found


def struct_findings(name, old, new):
    """old's members outside the reserved room, each in new as it was,
    in order; members new adds may come between them"""
    findings, j = [], 0
    kept = [m for m in new]
    for member, text in old:
        if member and RESERVED.search(member):
            continue
        k = next((x for x in range(j, len(kept)) if kept[x][1] == text), None)
        if k is None:
            same = next((t for n, t in kept if n == member), None)
            now = f" (now '{same}')" if same else ""
            findings.append(f"struct {name}: member '{text}' is renamed, retyped or gone{now}")
        else:
            j = k + 1
    return findings


def compare(old, new):
    """Findings: each a change old's consumers would see in new"""
    findings = []
    for name, value in old["macro"].items():
        if name not in new["macro"]:
            findings.append(f"#define {name} is gone")
        elif new["macro"][name] != value:
            findings.append(f"#define {name} was '{value}', is '{new['macro'][name]}'")
    for name, members in old["struct"].items():
        if name not in new["struct"]:
            findings.append(f"struct {name} is gone")
        else:
            findings.extend(struct_findings(name, members, new["struct"][name]))
    for name, values in old["enum"].items():
        if name not in new["enum"]:
            findings.append(f"enum {name} is gone")
            continue
        for v in values:
            if v not in new["enum"][name]:
                findings.append(f"enum {name}: '{v}' is renamed, renumbered or gone")
    for kind in ("typedef", "function"):
        for name, text in old[kind].items():
            if name not in new[kind]:
                findings.append(f"{kind} {name} is gone")
            elif new[kind][name] != text:
                findings.append(f"{kind} {name} was '{text}', is '{new[kind][name]}'")
    return findings


def installed_headers(tree):
    """The headers core/CMakeLists.txt installs, by file name"""
    with open(os.path.join(tree, "core", "CMakeLists.txt"), encoding="utf-8") as f:
        cmake = f.read()
    return sorted(set(re.findall(r"include/ecuio/(\w+\.h)\b", cmake)))


def tree_declarations(tree, headers):
    """{header: its declarations}"""
    out = {}
    for h in headers:
        path = os.path.join(tree, "core", "include", "ecuio", h)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                out[h] = declarations(f.read())
    return out


def allowed(tree):
    """The names abi/api_changes.txt lets change, each line '<name> <reason>'"""
    path = os.path.join(tree, "abi", "api_changes.txt")
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        return {line.split()[0] for line in f
                if line.strip() and not line.lstrip().startswith("#")}


def check(base, head):
    headers = installed_headers(base)
    old = tree_declarations(base, headers)
    new = tree_declarations(head, headers)
    ok_names = allowed(head)
    findings = []
    for h in headers:
        if h not in old:
            continue
        if h not in new:
            findings.append(f"{h} is gone")
            continue
        for f in compare(old[h], new[h]):
            names = set(re.findall(r"\w+", f))
            if not names & ok_names:
                findings.append(f"{h}: {f}")
    return findings


# Self-test

BASE = """
/* A header in the house style */
#ifndef ECUIO_T_H
#define ECUIO_T_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ECUIO_T_MAX 4u   /* a bound */
#define ECUIO_T_FLAG_A 0x1u
typedef enum {
    ECUIO_T_OK = 0,
    ECUIO_T_BAD = 1    /* the last */
} ecuio_t_status_t;
typedef struct ecuio_t_item {
    uint32_t flags;    /* ECUIO_T_FLAG_* */
    char name[32];
    uint8_t kind;
    uint8_t reserved[7];
} ecuio_t_item_t;
typedef struct ecuio_t_session ecuio_t_session_t;
typedef void (*ecuio_t_callback_t)(const ecuio_t_item_t* item, void* user);
ecuio_t_status_t ecuio_t_open(const char* uri, ecuio_t_session_t** out);
uint32_t ecuio_t_count(const ecuio_t_session_t* session);
#if defined(__cplusplus)
static_assert(sizeof(ecuio_t_item_t) == 44, "ABI");
#else
_Static_assert(sizeof(ecuio_t_item_t) == 44, "ABI");
#endif
#ifdef __cplusplus
}
#endif
#endif
"""


def selftest():
    def mutant(old, new):
        assert BASE.count(old) == 1, old
        return BASE.replace(old, new)

    cases = [
        ("no change passes", BASE, True),
        ("comments and formatting only pass", BASE.replace("/* a bound */", "")
         .replace("uint32_t flags;    /* ECUIO_T_FLAG_* */", "uint32_t   flags;")
         .replace("ecuio_t_status_t ecuio_t_open(const char* uri, ecuio_t_session_t** out);",
                  "ecuio_t_status_t ecuio_t_open(const char *uri,\n    ecuio_t_session_t **out);"), True),
        ("a member renamed fails", mutant("uint32_t flags;", "uint32_t options;"), False),
        ("a member's sign changed fails", mutant("uint32_t flags;", "int32_t flags;"), False),
        ("an array resized fails", mutant("char name[32];", "char name[31];"), False),
        ("a member gone fails", mutant("    uint8_t kind;\n", ""), False),
        ("the reserved room taken passes", mutant("uint8_t reserved[7];",
                                                  "uint8_t group;\n    uint8_t reserved[6];"), True),
        ("the reserved room taken whole passes", mutant("uint8_t reserved[7];",
                                                        "uint8_t a;\n    uint8_t b[6];"), True),
        ("an enumerator appended passes", mutant("ECUIO_T_BAD = 1    /* the last */",
                                                 "ECUIO_T_BAD = 1,\n    ECUIO_T_LATE = 2"), True),
        ("an enumerator renumbered fails", mutant("ECUIO_T_BAD = 1", "ECUIO_T_BAD = 2"), False),
        ("an enumerator renamed fails", mutant("ECUIO_T_BAD = 1", "ECUIO_T_WRONG = 1"), False),
        ("a macro's value changed fails", mutant("#define ECUIO_T_MAX 4u", "#define ECUIO_T_MAX 8u"), False),
        ("a macro gone fails", mutant("#define ECUIO_T_FLAG_A 0x1u\n", ""), False),
        ("a macro added passes", mutant("#define ECUIO_T_FLAG_A 0x1u",
                                        "#define ECUIO_T_FLAG_A 0x1u\n#define ECUIO_T_FLAG_B 0x2u"), True),
        ("a parameter renamed passes", mutant("const char* uri,", "const char* text,"), True),
        ("a parameter retyped fails", mutant("const char* uri,", "char* uri,"), False),
        ("a return type changed fails", mutant("uint32_t ecuio_t_count", "int32_t ecuio_t_count"), False),
        ("a function gone fails", mutant(
            "uint32_t ecuio_t_count(const ecuio_t_session_t* session);\n", ""), False),
        ("a function added passes", mutant(
            "uint32_t ecuio_t_count(const ecuio_t_session_t* session);",
            "uint32_t ecuio_t_count(const ecuio_t_session_t* session);\n"
            "void ecuio_t_close(ecuio_t_session_t* session);"), True),
        ("a callback's parameter renamed passes", mutant("void* user);", "void* context);"), True),
        ("a callback's parameter retyped fails", mutant("void* user);", "const void* user);"), False),
        ("an opaque type renamed fails", mutant("typedef struct ecuio_t_session ecuio_t_session_t;",
                                                "typedef struct ecuio_t_link ecuio_t_session_t;"), False),
        ("a static assert changed passes", BASE.replace("== 44", "== 48"), True),
    ]
    failed = 0
    old = declarations(BASE)
    for name, text, expect in cases:
        findings = compare(old, declarations(text))
        if (not findings) != expect:
            failed += 1
            print(f"FAIL  {name}: {findings or 'no finding'}")
    # A name in abi/api_changes.txt is let through, nothing else
    findings = compare(old, declarations(mutant("uint32_t flags;", "uint32_t options;")))
    names = set(re.findall(r"\w+", findings[0])) if findings else set()
    if not (len(findings) == 1 and "ecuio_t_item" in names):
        failed += 1
        print(f"FAIL  a finding names its struct: {findings}")
    print(f"{len(cases) + 1 - failed}/{len(cases) + 1} cases passed")
    return 0 if failed == 0 else 1


def main(argv):
    if len(argv) == 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) != 3:
        print(__doc__ or "usage: api_check.py <base tree> <head tree> | selftest", file=sys.stderr)
        return 2
    try:
        findings = check(argv[1], argv[2])
    except (OSError, ValueError) as e:
        print(f"api_check: {e}", file=sys.stderr)
        return 2
    for f in findings:
        print(f"changed  {f}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
