# File/layer: python/tests/test_version.py | pytest
# Testing: UNIT, PythonTest.test_version (ADR-0053 sections 3 and 6, its
#          appendix)
#
# The package's version is the project's, and the library it loads must
# be one it was built for: 0.x on major.minor, 1.0 on major, the patch
# free. The refusals run in a child Python, since the package loads its
# library once, at import. On Windows the surrogates' variables come from
# the library's folder unless the caller set them.

import os
import re
import subprocess
import sys

from conftest import ROOT

import havne
from havne import _lib


def child(code, **env):
    run = subprocess.run([sys.executable, "-B", "-c", code], capture_output=True, text=True,
                         env={**os.environ, "PYTHONPATH": os.path.join(ROOT, "python"), **env},
                         timeout=60)
    return run.returncode, run.stdout + run.stderr


def test_the_package_is_the_project_s_version():
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        project = re.search(r"project\(Havne\s+VERSION\s+([0-9.]+)", f.read()).group(1)
    assert havne.__version__ == project


def test_the_library_loaded_is_this_build_s():
    assert havne.core_version[:2] == _lib.package_version()[:2]
    assert os.path.samefile(havne.library_path, os.environ["HAVNE_LIBRARY"])


def test_before_one_the_minor_must_match_and_after_it_the_major():
    assert _lib.compatible((0, 6, 0), (0, 6, 3))
    assert not _lib.compatible((0, 6, 0), (0, 5, 1))
    assert not _lib.compatible((0, 6, 0), (0, 7, 0))
    assert not _lib.compatible((0, 6, 0), (1, 6, 0))
    assert _lib.compatible((1, 2, 0), (1, 9, 4))
    assert not _lib.compatible((1, 2, 0), (2, 0, 0))


def test_a_refusal_names_both_versions_and_the_library():
    words = _lib.version_words((0, 6, 0), (0, 5, 1), "/x/libecuio.so.0.5")
    assert words == ("havne 0.6.0 needs core 0.6.x; the library at /x/libecuio.so.0.5 "
                     "is core 0.5.1")
    assert _lib.version_words((1, 2, 0), (2, 0, 0), "p").startswith("havne 1.2.0 needs core 1.x;")


def test_a_package_of_another_version_refuses_the_library():
    # A stand-in _version, in place before the package's first import
    rc, out = child("import sys, types\n"
                    "v = types.ModuleType('havne._version'); v.__version__ = '0.99.0'\n"
                    "sys.modules['havne._version'] = v\n"
                    "import havne")
    assert rc != 0
    assert "LibraryVersionError" in out
    assert re.search(r"havne 0\.99\.0 needs core 0\.99\.x; the library at .* is core \d+\.\d+\.\d+",
                     out), out


def test_a_library_that_isn_t_there_is_named():
    rc, out = child("import havne", HAVNE_LIBRARY=os.path.join(ROOT, "no-such-ecuio.dll"))
    assert rc != 0
    assert "LibraryNotFound" in out and "no-such-ecuio.dll" in out


def test_with_no_library_named_it_looks_in_the_package_alone():
    rc, out = child("import havne", HAVNE_LIBRARY="")
    if _lib.package_library() is None:
        assert rc != 0 and "LibraryNotFound" in out and "HAVNE_LIBRARY" in out
    else:
        assert rc == 0, out


def test_the_surrogates_come_from_the_library_s_folder_unless_the_caller_names_them(
        tmp_path, monkeypatch):
    (x86, x86_name), (x64, x64_name) = _lib.SURROGATES
    for name in (x86_name, x64_name):
        (tmp_path / name).write_bytes(b"")
    callers = str(tmp_path / "elsewhere" / x86_name)
    monkeypatch.setenv(x86, callers)
    monkeypatch.delenv(x64, raising=False)
    _lib.set_surrogates(str(tmp_path / "empty"))      # no surrogates there: nothing set
    assert x64 not in os.environ
    _lib.set_surrogates(str(tmp_path))
    assert os.environ[x86] == callers                  # the caller's stays
    if sys.platform == "win32":
        assert os.environ[x64] == str(tmp_path / x64_name)
    else:
        assert x64 not in os.environ                   # the surrogates are Windows'
