# File/layer: python/tests/test_declarations.py | pytest
# Testing: UNIT, PythonTest.test_declarations (ADR-0053 section 7)
#
# The package's declarations against the headers, with no library
# loaded: _ecuio.py is what scripts/ctypes_gen.py makes from them today;
# every function the shared core exports (scripts/ecuio_exports.py) is
# declared, text core hands over as a pointer to free, every macro (a
# limit, a string; another kind fails the script); and each layout the
# headers pin with a static_assert (a struct's size, a member's offset)
# holds for the ctypes types on the platform running, so a type mapped
# wrong fails here, not in a script.

import ctypes
import importlib.util
import os
import re
import shutil
import sys

import pytest

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


def test_text_core_hands_over_stays_a_pointer():
    # A c_char_p would come back as a Python copy, and the pointer core
    # must free with it: owned text is POINTER(c_char)
    owned = ctypes.POINTER(ctypes.c_char)
    assert E.FUNCTIONS["ecuio_obdb_free"] == (None, [owned])
    assert E.FUNCTIONS["ecuio_obdb_import"][1][5] is ctypes.POINTER(owned)
    # Borrowed text stays c_char_p: a const char** out-parameter, a
    # caller's buffer, a const char* result
    assert E.ecuio_plugin_read_t._argtypes_[2] is ctypes.POINTER(ctypes.c_char_p)
    assert E.FUNCTIONS["ecuio_plugin_variant"][1][2] is ctypes.c_char_p
    assert E.FUNCTIONS["ecuio_last_error_detail"][0] is ctypes.c_char_p


def test_every_macro_is_declared():
    # <stdint.h>'s limits and string literals too
    assert E.ECUIO_RECORDING_NONE == 2**64 - 1
    assert E.ECUIO_TIMING_COLUMNS.startswith("t_s,ecu,request,")
    assert "," in E.ECUIO_TIMING_COLUMNS and '"' not in E.ECUIO_TIMING_COLUMNS


def made_up_tree(tmp_path, line):
    """core's CMakeLists and public headers with one line added to vin.h"""
    include = tmp_path / "core" / "include" / "ecuio"
    shutil.copytree(os.path.join(ROOT, "core", "include", "ecuio"), include)
    shutil.copy(os.path.join(ROOT, "core", "CMakeLists.txt"), tmp_path / "core")
    vin = include / "vin.h"
    body = vin.read_text(encoding="utf-8")
    at = body.rindex("#endif")
    vin.write_text(body[:at] + line + "\n" + body[at:], encoding="utf-8")
    return str(tmp_path)


def test_a_macro_that_is_neither_a_number_nor_a_string_fails_the_script(tmp_path):
    tree = made_up_tree(tmp_path, "#define ECUIO_MADE_UP (sizeof(int))")
    with pytest.raises(ctypes_gen.Unknown) as e:
        ctypes_gen.generate(tree)
    assert "ECUIO_MADE_UP" in str(e.value)


def test_a_limit_and_a_string_macro_are_declared(tmp_path):
    tree = made_up_tree(tmp_path, '#define ECUIO_MADE_UP_MIN (INT32_MIN + 1)\n'
                                  '#define ECUIO_MADE_UP_TEXT "a,b" "c"')
    text = ctypes_gen.generate(tree)
    assert f"ECUIO_MADE_UP_MIN = {-(2**31) + 1}" in text.splitlines()
    assert "ECUIO_MADE_UP_TEXT = 'a,bc'" in text.splitlines()


def test_an_owned_result_and_its_release_are_pointers(tmp_path):
    tree = made_up_tree(tmp_path, "char* ecuio_made_up_text(const char* name);\n"
                                  "void ecuio_made_up_free(char* text);")
    lines = ctypes_gen.generate(tree).splitlines()
    assert "    'ecuio_made_up_text': (POINTER(c_char), [c_char_p]),  # vin.h" in lines
    assert "    'ecuio_made_up_free': (None, [POINTER(c_char)]),  # vin.h" in lines


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
