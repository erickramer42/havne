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

def test_a_scan_in_the_extended_session_reports_its_session():
    """ADR-0060: the fake's ECU answers D200 only in its extended session,
    so the range's one find and the report's session say it was entered"""
    uri = ("j2534://" + FAKE.replace("\\", "/") +
           "?profile=uds-generic&did=1E1C&did=D200-D203&session=03"
           "&identify=0&diagnose=0")
    with havne.Session(uri) as s:
        s.start()
        until = time.monotonic() + 15
        report = s.support(0)
        while (not report.flags & havne.c.ECUIO_SUPPORT_SCAN_COMPLETE and
               time.monotonic() < until):
            time.sleep(0.05)
            report = s.support(0)
        assert report.flags & havne.c.ECUIO_SUPPORT_SCAN_COMPLETE
        assert report.session_asked == 3
        assert report.session == 3
        assert report.session_state == havne.c.ECUIO_SESSION_STATE_ENTERED
        assert report.flags & havne.c.ECUIO_SUPPORT_EXTENDED
        assert report.session_entries == 1
        assert report.scan_found == 1, "D200, answered in the session alone"



def test_session_state_says_the_session_in_havne_clis_words():
    """ADR-0060: the module's session as words, the fields of the session
    line havne-cli stream writes for the same URI; none without ?session=03"""
    from conftest import cli
    uri = ("j2534://" + FAKE.replace("\\", "/") +
           "?profile=uds-generic&did=1E1C&did=D200-D203&session=03"
           "&identify=0&diagnose=0")
    with havne.Session(uri) as s:
        s.start()
        until = time.monotonic() + 15
        while (not s.support(0).flags & havne.c.ECUIO_SUPPORT_SCAN_COMPLETE and
               time.monotonic() < until):
            time.sleep(0.05)
        state = s.session_state(0)
    assert state is not None
    assert state.state == "entered" and state.session == "03" and state.nrc is None
    assert state.entries == 1 and state.lost == 0
    lines = cli("stream", uri, "--until-scanned")
    ours = {k: v for k, v in state._asdict().items()}
    theirs = [x for x in lines if x.get("type") == "session"][-1]
    assert {k: theirs[k] for k in ours if k != "ecu"} ==         {k: v for k, v in ours.items() if k != "ecu"}
    assert theirs["ecu"] == state.ecu
    plain = uri.replace("&session=03", "")
    with havne.Session(plain) as s:
        s.start()
        assert s.session_state(0) is None
