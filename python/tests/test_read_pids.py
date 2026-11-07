# File/layer: python/tests/test_read_pids.py | pytest
# Testing: HARNESS, PythonTest.test_read_pids: v0.6.0's Done-when on
#          every platform, the same script as test_vcan's
#
# python/examples/read_pids.py, run as a user runs it, against mock://
# and against the ELM327 emulator (havne-cli sim --elm327 tcp:0, the
# simulated car behind ELM327's text on loopback, ADR-0055) through core's
# own elm327:// session. ctest names havne-cli in HAVNE_CLI; without it
# the emulator's case skips.

import json
import os
import subprocess

import pytest

from conftest import run_example

CLI = os.environ.get("HAVNE_CLI", "")


def printed(out):
    return {line.split(": ", 1)[0]: line for line in out.splitlines()}


def test_the_script_reads_the_mock():
    rc, out, err = run_example("read_pids.py", "mock://read-pids")
    assert rc == 0, err
    assert set(printed(out)) == {"RPM", "Vehicle Speed", "Coolant Temp"}


def test_the_script_reads_the_elm327_emulator():
    if not CLI:
        pytest.skip("HAVNE_CLI names no havne-cli")
    emu = subprocess.Popen([CLI, "sim", "mock://emu?values=canned", "--elm327", "tcp:0",
                            "--seconds", "120"], stdout=subprocess.PIPE, text=True)
    try:
        port = None
        for line in emu.stdout:
            j = json.loads(line)
            assert j["type"] != "error", j
            if j["type"] == "elm327":
                host, port = j["listen"].rsplit(":", 1)
                assert host == "127.0.0.1", j
                break
        assert port, "the emulator said where it listens"
        rc, out, err = run_example("read_pids.py", f"elm327://127.0.0.1:{port}",
                                   "--seconds", "40")
    finally:
        emu.terminate()
        emu.communicate(timeout=60)
    assert rc == 0, err
    lines = printed(out)
    assert lines.get("RPM") == "RPM: 2000 RPM"            # the canned engine speed
    assert {"Vehicle Speed", "Coolant Temp"} <= lines.keys()
