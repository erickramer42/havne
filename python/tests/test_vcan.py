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

from conftest import need_vcan, run_example

import havne


def test_a_script_opens_socketcan_decodes_pids_and_prints_them():
    iface = need_vcan()
    with havne.Sim("mock://sim?values=canned", interface=iface):
        rc, out, err = run_example("read_pids.py", f"socketcan://{iface}?profile=obd2")
    assert rc == 0, err
    printed = out.splitlines()
    assert "RPM: 2000 RPM" in printed                     # the canned engine speed
    assert any(line.startswith("Vehicle Speed: ") for line in printed)
    assert any(line.startswith("Coolant Temp: ") for line in printed)
