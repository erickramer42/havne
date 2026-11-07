# File/layer: python/tests/test_declarations.py | pytest
# Testing: UNIT, PythonTest.test_declarations (ADR-0053 section 7)
#
# The package's declarations against the headers, with no library
# loaded: _ecuio.py is what scripts/ctypes_gen.py makes from them today;
# every function the shared core exports (scripts/ecuio_exports.py) is
# declared; and each layout the headers pin with a static_assert (a
# struct's size, a member's offset) holds for the ctypes types on the
# platform running, so a type mapped wrong fails here, not in a script.

import ctypes
import importlib.util
import os
import re
import sys

from conftest import ROOT

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import ctypes_gen  # noqa: E402
import ecuio_exports  # noqa: E402


def load_declarations():
    path = os.path.join(ROOT, "python", "havne", "_ecuio.py")
    spec = importlib.util.spec_from_file_location("havne_ecuio_alone", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


E = load_declarations()
HEADERS = [os.path.join(ROOT, "core", "include", "ecuio", h) for h in E.HEADERS]


def test_the_declarations_are_the_headers_today():
    with open(os.path.join(ROOT, "python", "havne", "_ecuio.py"), encoding="utf-8") as f:
        committed = f.read()
    assert committed == ctypes_gen.generate(), "run scripts/ctypes_gen.py"


def test_every_exported_function_is_declared_and_nothing_else():
    assert sorted(E.FUNCTIONS) == sorted(ecuio_exports.declared(HEADERS))


def test_the_headers_are_the_installed_list():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import api_check
    assert list(E.HEADERS) == api_check.installed_headers(ROOT)


ASSERT = re.compile(r"(?<!_)static_assert\s*\((.*?),\s*\"[^\"]*\"\s*\)\s*;", re.S)


def translated(expr):
    """A static_assert's condition as Python over the ctypes types"""
    t = " ".join(expr.split())
    t = re.sub(r"sizeof\s*\(\s*void\s*\*\s*\)", "ctypes.sizeof(ctypes.c_void_p)", t)
    t = re.sub(r"sizeof\s*\(\s*(\w+)\s*\)", r"ctypes.sizeof(E.\1)", t)
    # getattr: a member may be a Python keyword (ecuio_plugin_value_t's def)
    t = re.sub(r"offsetof\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)", r"getattr(E.\1, '\2').offset", t)
    t = t.replace("||", " or ").replace("&&", " and ")
    t = re.sub(r"\b(\d+)[uU]\b", r"\1", t)
    t = re.sub(r"\b(ECUIO_\w+)\b", r"E.\1", t)
    return t


def test_every_pinned_layout_holds_for_the_ctypes_types():
    checked, wrong = 0, []
    for path in HEADERS:
        with open(path, encoding="utf-8") as f:
            source = f.read()
        for m in ASSERT.finditer(source):
            expr = translated(m.group(1))
            ok = eval(expr, {"ctypes": ctypes, "E": E})  # noqa: S307
            checked += 1
            if not ok:
                wrong.append(f"{os.path.basename(path)}: {' '.join(m.group(1).split())}")
    assert not wrong, "\n".join(wrong)
    assert checked > 100, f"only {checked} asserts read: the pattern lost some"


def test_a_frame_is_core_s_frame():
    assert ctypes.sizeof(E.ecuio_frame_t) == 4120
    assert E.ecuio_frame_t.data.offset == 16
    assert ctypes.sizeof(E.ecuio_queue_value_t) == 160
