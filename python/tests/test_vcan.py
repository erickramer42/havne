# File/layer: python/tests/test_vcan.py | pytest
# Testing: HARNESS, PythonTest.test_vcan (Linux, vcan0): v0.6.0's
#          Done-when, "a Python script opens SocketCAN, decodes PIDs and
#          prints the values"
#
# The simulated ECU answers on vcan0 as the engine ECU (havne-cli sim's),
# and python/examples/read_pids.py, the Done-when's script, opens
# socketcan://vcan0 under obd2 and prints the decoded PIDs, run as a user
# runs it. Skips without Linux or vcan0, and fails instead under
# ECUIO_VCAN_REQUIRED=1, as the C++ vcan suites do (CI's linux-build).

import os
import sys

import pytest

from conftest import run_example

import havne

IFACE = os.environ.get("ECUIO_VCAN_IF", "vcan0")


def vcan_up():
    if not sys.platform.startswith("linux"):
        return "SocketCAN needs Linux"
    try:
        with open(f"/sys/class/net/{IFACE}/operstate", encoding="ascii") as f:
            state = f.read().strip()
    except OSError:
        return f"no {IFACE} (the linux skill's vcan, or ip link add {IFACE} type vcan)"
    return None if state in ("up", "unknown") else f"{IFACE} is {state}"


why = vcan_up()
if why and os.environ.get("ECUIO_VCAN_REQUIRED") != "1":
    pytest.skip(why, allow_module_level=True)


def test_a_script_opens_socketcan_decodes_pids_and_prints_them():
    assert why is None, why
    with havne.Sim("mock://sim?values=canned", interface=IFACE):
        rc, out, err = run_example("read_pids.py", f"socketcan://{IFACE}?profile=obd2")
    assert rc == 0, err
    printed = out.splitlines()
    assert "RPM: 2000 RPM" in printed                     # the canned engine speed
    assert any(line.startswith("Vehicle Speed: ") for line in printed)
    assert any(line.startswith("Coolant Temp: ") for line in printed)
