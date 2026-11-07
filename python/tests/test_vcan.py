# File/layer: python/tests/test_vcan.py | pytest
# Testing: HARNESS, PythonTest.test_vcan (Linux, vcan0): v0.6.0's
#          Done-when, "a Python script opens SocketCAN, decodes PIDs and
#          prints the values"
#
# The simulated ECU answers on vcan0 as the engine ECU (havne-cli sim's),
# and a script opens socketcan://vcan0 under obd2, reads the decoded PIDs
# from a values stream and prints them, exactly as a user's script would.
# Skips without Linux or vcan0, and fails instead under
# ECUIO_VCAN_REQUIRED=1, as the C++ vcan suites do (CI's linux-build).

import os
import sys
import time

import pytest

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


def test_a_script_opens_socketcan_decodes_pids_and_prints_them(capsys):
    assert why is None, why
    wanted = {"RPM", "Vehicle Speed", "Coolant Temp"}
    seen = {}
    with havne.Sim("mock://sim?values=canned", interface=IFACE):
        with havne.Session(f"socketcan://{IFACE}?profile=obd2") as s:
            values = s.values()
            s.start()
            until = time.monotonic() + 20
            while not wanted <= seen.keys() and time.monotonic() < until:
                for v in values.read(0.2):
                    if v.name in wanted:
                        seen[v.name] = v
                        print(f"{v.name}: {v.value:g} {v.unit}")
    assert wanted <= seen.keys(), f"decoded only {sorted(seen)}"
    assert seen["RPM"].value == 2000          # the canned engine speed
    assert seen["RPM"].id == 0x7E8
    out = capsys.readouterr().out
    assert "RPM: 2000 RPM" in out
