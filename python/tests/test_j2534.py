# File/layer: python/tests/test_j2534.py | pytest
# Testing: HARNESS, PythonTest.test_j2534 (Windows): the in-tree fake
#          J2534 DLL through the x64 surrogate, from Python
#
# Under Python the running executable is python.exe, so core finds its
# surrogate through ECUIO_SURROGATE_X64_EXE, or ECUIO_SURROGATE_EXE for
# build-win32's x86 fake under a 32-bit Python (ADR-0053 section 3):
# ctest sets the tree's one, as a wheel's package sets both from its own
# folder. The fake's ECU answers obd2 (j2534's default profile) and its
# identification.

import os
import struct
import sys
import time

import pytest

import havne

if sys.platform != "win32":
    pytest.skip("J2534 needs Windows", allow_module_level=True)

FAKE = os.environ.get("HAVNE_FAKE_J2534_DLL", "")
# the fake is the tree's own arch, and so is its surrogate
SURROGATE = "ECUIO_SURROGATE_X64_EXE" if struct.calcsize("P") == 8 else "ECUIO_SURROGATE_EXE"


def test_the_fake_dll_through_the_surrogate():
    assert FAKE and os.path.isfile(FAKE), "ctest names the fake DLL"
    assert os.path.isfile(os.environ.get(SURROGATE, "")), f"ctest names {SURROGATE}"
    with havne.Session("j2534://" + FAKE.replace("\\", "/")) as s:
        assert s.info.backend == havne.c.ECUIO_BACKEND_J2534_SURROGATE
        values = s.values()
        s.start()
        rpm = []
        until = time.monotonic() + 15
        while not rpm and time.monotonic() < until:
            rpm = [v for v in values.read(0.2) if v.name == "RPM"]
        until = time.monotonic() + 15
        while not s.vin(0) and time.monotonic() < until:
            time.sleep(0.05)
        assert rpm, "the fake's ECU answered 01 0C"
        assert s.vin(0) == "FAKEJ2534VIN00001"
