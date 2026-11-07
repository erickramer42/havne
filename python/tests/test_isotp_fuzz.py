# File/layer: python/tests/test_isotp_fuzz.py | pytest
# Testing: HARNESS, PythonTest.test_isotp_fuzz (Linux, vcan0)
#
# python/examples/isotp_fuzz.py run as a user runs it, the simulated ECU
# in its own process: every case leaves the sim answering, a case replays
# alone by its seed and index, a bus with no ECU fails the run, an ECU
# that goes away mid-run stops it within a few probes, so does a second
# ECU already on the bus (the sim yields to it), and an
# interface that isn't vcan is refused, by the bus itself too. havne-cli's sim as the ECU is
# ExampleTest.isotp_fuzz_cli, outside pytest, for the sanitizer tree.

import io
import os
import sys
import threading
import time

import pytest

from conftest import ROOT, need_vcan, run_example

import havne

linux = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="SocketCAN needs Linux")


def test_every_case_leaves_the_sim_answering():
    iface = need_vcan()
    rc, out, err = run_example("isotp_fuzz.py", "--interface", iface, "--cases", "200",
                               timeout=170)
    assert rc == 0, out + err
    assert "isotp_fuzz: 200 cases, 0 failed (seed 1, " in out, out
    assert "FAIL" not in out


def test_a_case_replays_alone_by_its_seed_and_index():
    iface = need_vcan()
    rc, out, err = run_example("isotp_fuzz.py", "--interface", iface, "--seed", "3",
                               "--case", "37")
    assert rc == 0, out + err
    assert "isotp_fuzz: 1 case, 0 failed (seed 3, " in out, out


def test_a_bus_with_no_ecu_fails_the_run():
    iface = need_vcan()
    rc, out, _ = run_example("isotp_fuzz.py", "--interface", iface, "--ecu-running",
                             "--cases", "1")
    assert rc == 1
    assert f"isotp_fuzz: no ECU answers 01 0C on {iface}" in out


def test_an_ecu_that_goes_away_stops_the_run_at_once():
    iface = need_vcan()
    sim = havne.Sim("mock://sim?values=canned", interface=iface)
    timer = threading.Timer(2.0, sim.close)        # mid-run: 200 cases take about 8 s
    timer.start()
    try:
        started = time.monotonic()
        rc, out, _ = run_example("isotp_fuzz.py", "--interface", iface, "--ecu-running",
                                 "--cases", "200")
        took = time.monotonic() - started
    finally:
        timer.join()
        sim.close()
    assert rc == 1
    assert "isotp_fuzz: the ECU stopped answering (3 probes in a row)" in out, out
    assert "200 cases" not in out, out
    assert took < 40, took                         # 200 dead probes would take 300 s and more


def test_a_second_ecu_on_the_bus_stops_the_run_with_words():
    iface = need_vcan()
    with havne.Sim("mock://sim?values=canned", interface=iface):   # an ECU already there
        rc, out, _ = run_example("isotp_fuzz.py", "--interface", iface, "--cases", "200")
    assert rc == 1
    assert "another ECU answers on the bus, so the sim went silent" in out, out
    assert "isotp_fuzz: the fuzzer needs the bus to itself" in out, out
    assert "200 cases" not in out, out


@linux
def test_an_interface_that_isnt_vcan_is_refused():
    rc, out, err = run_example("isotp_fuzz.py", "--interface", "lo", "--cases", "1")
    assert rc == 2 and out == "", out
    assert err.startswith("isotp_fuzz: ") and "vcan" in err, err


@linux
def test_the_bus_itself_refuses_an_interface_that_isnt_vcan():
    # A script importing the fuzzer never reaches main()'s check
    sys.path.insert(0, os.path.join(ROOT, "python", "examples"))
    try:
        import isotp_fuzz
    finally:
        sys.path.pop(0)
    with pytest.raises(ValueError, match="vcan only"):
        isotp_fuzz.Bus("lo")
    with pytest.raises(ValueError, match="vcan only"):
        isotp_fuzz.fuzz("lo", 1, 1, out=io.StringIO())
