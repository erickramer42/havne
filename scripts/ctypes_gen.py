# File/layer: scripts/ctypes_gen.py | Python 3.8+, no packages
# Testing: UNIT, PythonTest.test_declarations (python/tests): the
#          committed python/havne/_ecuio.py equals this script's output,
#          and every struct's layout equals the headers' static asserts
#
# The havne package's declarations (ADR-0053 section 1): every public
# header's functions with their argument and result types, the structs
# and the callback typedefs as ctypes types, the enumerators and the
# ECUIO_ macros (numbers, <stdint.h>'s limits among them, decimal
# literals, and string literals) as constants, written to python/havne/_ecuio.py.
# The headers are core/CMakeLists.txt's ECUIO_PUBLIC_HEADERS, read as
# scripts/api_check.py reads them, so the package declares exactly what
# the shared core exports (scripts/ecuio_exports.py).
#
#   python3 scripts/ctypes_gen.py            writes python/havne/_ecuio.py
#   python3 scripts/ctypes_gen.py --check    exit 1 when the file differs
#
# C to ctypes: the fixed-width integers, char, int, size_t, double and
# _Bool as their ctypes types; an enum as c_int (every ABI here gives an
# enum int's size); char* as c_char_p, void* as c_void_p, any other T* as
# POINTER(T); an array as T * N, its N from the headers' macros; an
# opaque handle as an empty Structure; a function pointer as CFUNCTYPE;
# an anonymous union member as a Union of its own, named <struct>_u<N>,
# in the struct's _anonymous_, so its members read as the struct's.
# Text core hands the caller to own (a non-const char** out-parameter, a
# non-const char* result) and a release function's char* are
# POINTER(c_char), never c_char_p: ctypes turns a c_char_p into a Python
# copy, and the pointer core must free would be lost. A construct the map
# doesn't know, or an ECUIO_ macro that is neither a number nor a string,
# fails the script, naming it.

import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import api_check  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "python", "havne", "_ecuio.py")

BUILTIN = {
    "void": "None", "char": "c_char", "int": "c_int", "unsigned": "c_uint",
    "float": "c_float", "double": "c_double", "size_t": "c_size_t",
    "_Bool": "c_bool", "bool": "c_bool",
    "int8_t": "c_int8", "uint8_t": "c_uint8", "int16_t": "c_int16",
    "uint16_t": "c_uint16", "int32_t": "c_int32", "uint32_t": "c_uint32",
    "int64_t": "c_int64", "uint64_t": "c_uint64",
}


# <stdint.h>'s limits, which a macro may name
STDINT = {
    "INT8_MIN": -(1 << 7), "INT8_MAX": (1 << 7) - 1, "UINT8_MAX": (1 << 8) - 1,
    "INT16_MIN": -(1 << 15), "INT16_MAX": (1 << 15) - 1, "UINT16_MAX": (1 << 16) - 1,
    "INT32_MIN": -(1 << 31), "INT32_MAX": (1 << 31) - 1, "UINT32_MAX": (1 << 32) - 1,
    "INT64_MIN": -(1 << 63), "INT64_MAX": (1 << 63) - 1, "UINT64_MAX": (1 << 64) - 1,
}

# A release function, as core.md's export rule names one
RELEASE = re.compile(r"_(free|close|destroy|unregister)$")


class Unknown(ValueError):
    pass


def int_value(text, macros, seen=()):
    """A macro's text as an integer, or None: suffixes dropped, other
    macros put in, only arithmetic left"""
    t = text.strip()
    if not t:
        return None

    def name(m):
        n = m.group(0)
        if n in STDINT and n not in macros:
            return f"({STDINT[n]})"
        if n in seen or n not in macros:
            raise Unknown(n)
        v = int_value(macros[n], macros, seen + (n,))
        if v is None:
            raise Unknown(n)
        return f"({v})"

    try:
        t = re.sub(r"\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]+\b", r"\1", t)
        t = re.sub(r"\b[A-Za-z_]\w*\b", name, t)
    except Unknown:
        return None
    if not re.fullmatch(r"[0-9xXa-fA-F\s()+\-*/<>|&~]+", t):
        return None
    try:
        v = eval(t.replace("/", "//"), {"__builtins__": {}}, {})  # noqa: S307
    except (SyntaxError, TypeError, ZeroDivisionError, NameError):
        return None
    return v if isinstance(v, int) else None


def float_value(text):
    """A macro's text as a float when it is one decimal literal (a point,
    an exponent or both, no suffix), else None"""
    t = text.strip()
    if not re.fullmatch(r"\d+(\.\d*)?([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?", t):
        return None
    if "." not in t and "e" not in t.lower():
        return None
    return float(t)


def string_value(text):
    """A macro's text as a str when it is one or more adjacent string
    literals with no escapes, else None"""
    t = text.strip()
    if not re.fullmatch(r'"[^"\\\n]*"(\s*"[^"\\\n]*")*', t):
        return None
    return "".join(re.findall(r'"([^"]*)"', t))


class Model:
    """The headers' declarations, gathered across the public headers"""

    def __init__(self, tree=ROOT):
        self.headers = api_check.installed_headers(tree)
        self.macros, self.structs, self.enums = {}, {}, {}
        self.typedefs, self.functions = {}, {}
        for h in self.headers:
            with open(os.path.join(tree, "core", "include", "ecuio", h), encoding="utf-8") as f:
                d = api_check.declarations(f.read())
            for kind, into in (("macro", self.macros), ("struct", self.structs),
                               ("enum", self.enums), ("typedef", self.typedefs),
                               ("function", self.functions)):
                for k, v in d[kind].items():
                    into.setdefault(k, (h, v) if kind == "function" else v)
        # The struct each name means: a typedef's alias names its tag's members
        self.struct_of = {}
        for alias, text in self.typedefs.items():
            m = re.fullmatch(r"(struct|union) (\w+)(?: \w+)?", text)
            if m:
                self.struct_of[alias] = m.group(2)
        for tag in self.structs:
            self.struct_of.setdefault(tag, tag)
        self.opaque = sorted(a for a, tag in self.struct_of.items() if tag not in self.structs)
        self.callbacks = {n: t for n, t in self.typedefs.items() if "(" in t}

    # --- types -------------------------------------------------------------

    def base(self, word):
        if word in BUILTIN:
            return BUILTIN[word]
        if word in self.enums:
            return "c_int"
        if word in self.struct_of or word in self.callbacks:
            return word
        raise Unknown(f"the type '{word}'")

    def ctype(self, tokens):
        """A type's tokens (no name) as a ctypes expression"""
        words = [t for t in tokens if t not in ("const", "struct", "enum", "union", "volatile")]
        stars = words.count("*")
        words = [w for w in words if w != "*"]
        if words == ["unsigned", "int"]:
            words = ["unsigned"]
        if len(words) != 1:
            raise Unknown(f"the type '{' '.join(tokens)}'")
        word = words[0]
        if stars == 0:
            return self.base(word)
        if word == "char":
            if stars > 1 and not self.const_char(tokens):
                return self.pointers("POINTER(c_char)", stars - 1)  # owned
            return "c_char_p" if stars == 1 else self.pointers("c_char_p", stars - 1)
        if word == "void":
            return "c_void_p" if stars == 1 else self.pointers("c_void_p", stars - 1)
        return self.pointers(self.base(word), stars)

    @staticmethod
    def const_char(tokens):
        """The chars themselves const: const before the first star"""
        head = tokens[:tokens.index("*")] if "*" in tokens else tokens
        return "const" in head

    def owned_char(self, tokens):
        """A plain non-const char*: text core hands over, as a result or
        to a release function"""
        toks = list(tokens)
        if len(toks) == 3 and re.fullmatch(r"[A-Za-z_]\w*", toks[2]):
            toks = toks[:2]  # a parameter's name
        return toks == ["char", "*"]

    @staticmethod
    def pointers(t, n):
        for _ in range(n):
            t = f"POINTER({t})"
        return t

    def callback(self, ret_tokens, params_text):
        args = [] if params_text.strip() in ("", "void") else [
            self.param_type(p.split()) for p in split_params(params_text)]
        return f"CFUNCTYPE({', '.join([self.ctype(ret_tokens)] + args)})"

    def param_type(self, tokens):
        """A parameter's tokens, its name dropped if it has one"""
        toks = list(tokens)
        if len(toks) >= 2 and re.fullmatch(r"[A-Za-z_]\w*", toks[-1]) and toks[-1] not in BUILTIN \
                and toks[-1] not in ("const", "void") and toks[-1] not in self.struct_of \
                and toks[-1] not in self.enums and toks[-1] not in self.callbacks:
            toks = toks[:-1]
        return self.ctype(toks)

    def field(self, text):
        """A struct member's text as (name, ctypes expression)"""
        toks = text.split()
        m = re.fullmatch(r"(.+?) \( \* (\w+) \) \( (.*) \)", text)
        if m:   # an inline function pointer
            return m.group(2), self.callback(m.group(1).split(), m.group(3).replace(" , ", ", "))
        dims = []
        while toks and toks[-1] == "]":
            i = len(toks) - 1 - toks[::-1].index("[")
            n = int_value(" ".join(toks[i + 1:-1]), self.macros)
            if n is None:
                raise Unknown(f"the array size in '{text}'")
            dims.insert(0, n)
            toks = toks[:i]
        name, t = toks[-1], self.ctype(toks[:-1])
        for n in reversed(dims):
            t = f"({t} * {n})"
        return name, t

    def by_value(self, struct):
        """The structs a struct holds by value (not through a pointer)"""
        out = []
        for _, text in self.structs[self.struct_of[struct]]:
            toks = text.split()
            if "*" in toks:
                continue
            for t in toks:
                if t in self.struct_of and self.struct_of[t] in self.structs and t != struct:
                    out.append(t)
        return out


def split_params(text):
    out, depth, cur = [], 0, ""
    for c in text:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def generate(tree=ROOT):
    m = Model(tree)
    lines = [
        "# GENERATED by scripts/ctypes_gen.py from core's public headers",
        "# (core/CMakeLists.txt's ECUIO_PUBLIC_HEADERS): never edit; run the",
        "# script again. ADR-0053 section 1.",
        "# pylint: skip-file",
        "# flake8: noqa",
        "from ctypes import (CFUNCTYPE, POINTER, Structure, Union, c_bool, c_char, c_char_p,",
        "                    c_double, c_float, c_int, c_int8, c_int16, c_int32, c_int64,",
        "                    c_size_t, c_uint, c_uint8, c_uint16, c_uint32, c_uint64,",
        "                    c_void_p)",
        "",
        "HEADERS = (" + ", ".join(repr(h) for h in m.headers) + ",)",
        "",
        "# --- constants: the ECUIO_ macros, then the enumerators ---------------",
    ]
    for name in sorted(m.macros):
        if not name.startswith("ECUIO_") or name.endswith("_H"):
            continue
        v = int_value(m.macros[name], m.macros)
        if v is not None:
            lines.append(f"{name} = {v:#x}" if v > 255 else f"{name} = {v}")
            continue
        f = float_value(m.macros[name])
        if f is not None:
            lines.append(f"{name} = {f!r}")
            continue
        s = string_value(m.macros[name])
        if s is None:
            raise Unknown(f"the macro {name} ({m.macros[name].strip()[:60]}): "
                          "neither a number nor a string")
        lines.append(f"{name} = {s!r}")
    values = enum_values(m)
    for enum in sorted(m.enums):
        for e in m.enums[enum]:
            k = e.partition("=")[0].strip()
            lines.append(f"{k} = {values[k]}")
    lines += ["", "# --- the enums' types: an enum is an int ------------------------------"]
    for enum in sorted(m.enums):
        lines.append(f"{enum} = c_int")
    lines += ["", "# --- opaque handles and every struct, declared first -------------------"]
    names = sorted({a for a in m.struct_of if a.endswith("_t")} | set(m.opaque))
    for n in names:
        lines += [f"class {n}(Structure):", "    pass", ""]
    lines += ["# --- callbacks ---------------------------------------------------------"]
    for n in sorted(m.callbacks):
        mm = re.fullmatch(r"(.+?) \(\* ?\w+\) \((.*)\)", m.callbacks[n])
        if not mm:
            raise Unknown(f"the callback '{m.callbacks[n]}'")
        lines.append(f"{n} = {m.callback(mm.group(1).split(), mm.group(2))}")
    lines += ["", "# --- the structs' fields, each after the structs it holds ------------"]
    done, order = set(), []

    def visit(n, path=()):
        if n in done:
            return
        if n in path:
            raise Unknown(f"a struct holding itself: {n}")
        for dep in m.by_value(n):
            visit(dep, path + (n,))
        done.add(n)
        order.append(n)

    with_fields = [a for a in names if m.struct_of.get(a) in m.structs]
    for n in with_fields:
        visit(n)
    for n in order:
        fields, anonymous = [], []
        for _, text in m.structs[m.struct_of[n]]:
            toks = text.split()
            if toks[:1] != ["union"]:
                fields.append(m.field(text))
                continue
            if toks[1:2] != ["{"] or toks[-1] != "}":
                raise Unknown(f"the union in {n}: '{text}' (only an anonymous one)")
            members = [x.strip() for x in " ".join(toks[2:-1]).split(";") if x.strip()]
            u = f"{n}_u{len(anonymous)}"
            lines.append(f"class {u}(Union):")
            lines.append("    _fields_ = [")
            lines += [f"        ({name!r}, {t})," for name, t in (m.field(x) for x in members)]
            lines += ["    ]", ""]
            anonymous.append(f"_u{len(anonymous)}")
            fields.append((anonymous[-1], u))
        if anonymous:
            lines.append(f"{n}._anonymous_ = ({', '.join(repr(a) for a in anonymous)},)")
        lines.append(f"{n}._fields_ = [")
        lines += [f"    ({name!r}, {t})," for name, t in fields]
        lines.append("]")
    lines += ["", "# --- functions: (result, argument types), bound by bind() ------------",
              "FUNCTIONS = {"]
    for fname in sorted(m.functions):
        h, text = m.functions[fname]
        at = text.index(" " + fname + " (")
        ret = text[:at].split()
        inner = text[text.index("(", at) + 1:text.rindex(")")]
        params = [] if inner.strip() in ("", "void") else split_params(inner)
        args = [m.param_type(p.split()) for p in params]
        if params and RELEASE.search(fname) and m.owned_char(params[0].split()):
            args[0] = "POINTER(c_char)"  # the text core handed over
        result = "POINTER(c_char)" if m.owned_char(ret) else m.ctype(ret)
        lines.append(f"    {fname!r}: ({result}, [{', '.join(args)}]),  # {h}")
    lines += [
        "}",
        "",
        "",
        "def bind(lib):",
        '    """Each function\'s types on the loaded library; the library"""',
        "    for name, (restype, argtypes) in FUNCTIONS.items():",
        "        fn = getattr(lib, name)",
        "        fn.restype = restype",
        "        fn.argtypes = argtypes",
        "    return lib",
        "",
    ]
    return "\n".join(lines)


def enum_values(m):
    """{enumerator: its value}: given, or one past the one before"""
    out = {}
    for enum in m.enums.values():
        nxt = 0
        for e in enum:
            k, eq, v = e.partition("=")
            k = k.strip()
            n = int_value(v, {**m.macros, **{x: str(y) for x, y in out.items()}}) if eq else nxt
            if n is None:
                raise Unknown(f"the enumerator '{e}'")
            out[k] = n
            nxt = n + 1
    return out


def main(argv):
    try:
        text = generate()
    except (OSError, Unknown) as e:
        print(f"ctypes_gen: {e}", file=sys.stderr)
        return 2
    if argv[1:] == ["--check"]:
        with open(OUT, encoding="utf-8") as f:
            same = f.read() == text
        print("ok  _ecuio.py is the headers'" if same else
              "_ecuio.py differs from the headers: run scripts/ctypes_gen.py")
        return 0 if same else 1
    if argv[1:]:
        print("usage: ctypes_gen.py [--check]", file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
