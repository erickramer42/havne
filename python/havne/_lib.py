# File/layer: python/havne/_lib.py | Python 3.11+, the havne package
# Testing: PythonTest.test_version, and every PythonTest through it
#
# Finds core's shared library, loads it, binds its declarations and checks
# its version (ADR-0053 sections 2a, 3 and 6, and its appendix "the havne
# package as built").
#
# Where it looks, in order, and nowhere else, so an older core elsewhere on
# the machine never loads in its place:
#   1. HAVNE_LIBRARY, a path to the library (the tests' and a developer's);
#   2. the package's own folder: the wheel's ecuio.dll, libecuio.so.<n> or
#      libecuio.dylib.
# On Windows the J2534 surrogates travel with the library: when they're
# beside it and the caller's environment doesn't name them, it sets
# ECUIO_SURROGATE_EXE and ECUIO_SURROGATE_X64_EXE to them before the load,
# which core reads at each spawn.
#
# The version: before 1.0 a minor release may change the C ABI (ADR-0048),
# so 0.x must match the package's major.minor; from 1.0 its major. A patch
# never changes it.

import ctypes
import glob
import os
import sys

from . import _ecuio
from ._version import __version__
from .errors import LibraryNotFound, LibraryVersionError

HERE = os.path.dirname(os.path.abspath(__file__))
SURROGATES = (("ECUIO_SURROGATE_EXE", "ecuio-j2534-surrogate.exe"),
              ("ECUIO_SURROGATE_X64_EXE", "ecuio-j2534-surrogate-x64.exe"))


def package_library(folder=HERE):
    """The library in the package's folder, or None"""
    if sys.platform == "win32":
        names = ["ecuio.dll"]
    elif sys.platform == "darwin":
        names = ["libecuio.dylib"] + sorted(glob.glob(os.path.join(folder, "libecuio.*.dylib")))
    else:
        names = sorted(glob.glob(os.path.join(folder, "libecuio.so.*"))) + ["libecuio.so"]
    for n in names:
        path = os.path.join(folder, n)
        if os.path.isfile(path):
            return path
    return None


def find_library():
    path = os.environ.get("HAVNE_LIBRARY", "")
    if path:
        if not os.path.isfile(path):
            raise LibraryNotFound(f"HAVNE_LIBRARY names {path}, which isn't a file")
        return os.path.abspath(path)
    path = package_library()
    if path is None:
        raise LibraryNotFound(
            f"havne {__version__} found no core library: set HAVNE_LIBRARY to the "
            f"shared core's path (ecuio.dll, libecuio.so, libecuio.dylib), or install "
            f"the havne wheel, which carries it in {HERE}")
    return path


def set_surrogates(folder):
    """The surrogates beside the library, unless the environment names them"""
    if sys.platform != "win32":
        return
    for var, name in SURROGATES:
        path = os.path.join(folder, name)
        if not os.environ.get(var) and os.path.isfile(path):
            os.environ[var] = path


def compatible(package, core):
    """Whether a core of version `core` (major, minor, patch) serves a
    package of version `package`: 0.x on major.minor, 1.0 on major"""
    if package[0] == 0:
        return core[:2] == package[:2]
    return core[0] == package[0]


def version_words(package, core, path):
    want = f"{package[0]}.{package[1]}.x" if package[0] == 0 else f"{package[0]}.x"
    return (f"havne {'.'.join(map(str, package))} needs core {want}; "
            f"the library at {path} is core {'.'.join(map(str, core))}")


def core_version(lib):
    major, minor, patch = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_uint32()
    lib.ecuio_version(ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch))
    return major.value, minor.value, patch.value


def package_version():
    return tuple(int(p) for p in __version__.split(".")[:3])


def load():
    path = find_library()
    set_surrogates(os.path.dirname(path))
    try:
        lib = ctypes.CDLL(path)
    except OSError as e:
        raise LibraryNotFound(f"core's library at {path} doesn't load: {e}") from e
    try:
        _ecuio.bind(lib)
    except AttributeError as e:
        raise LibraryVersionError(
            f"the library at {path} lacks a function havne {__version__} declares: {e}") from e
    core = core_version(lib)
    if not compatible(package_version(), core):
        raise LibraryVersionError(version_words(package_version(), core, path))
    return lib, path, core


lib, path, version = load()
