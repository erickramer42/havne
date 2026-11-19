# File/layer: python/tests/test_sim.py | pytest
# Testing: HARNESS, PythonTest.test_sim: the simulated ECU asked through
#          the package with no bus (ecuio_sim_ask, ADR-0055)
#
# The engine ECU's answers to a script's requests: engine speed, the VIN
# (the sim's made-up default), a functional request, an ID no ECU hears,
# a refusal, and the request counts; a module's timing on each reply.

import havne


def test_the_engine_ecu_answers_a_script_s_requests():
    with havne.Sim.unbound("mock://sim?values=canned") as sim:
        (rpm,) = sim.ask(bytes([0x01, 0x0C]))
        assert rpm.id == 0x7E8
        assert rpm.data[:2] == b"\x41\x0c"
        assert (rpm.data[2] * 256 + rpm.data[3]) / 4 == 2000   # the canned reading
        (vin,) = sim.ask(b"\x09\x02")
        assert vin.data[:3] == b"\x49\x02\x01"
        assert vin.data[3:] == b"ZZZFAKE11M0000001"
        assert [r.id for r in sim.ask(b"\x01\x0c", to=0x7DF)] == [0x7E8]
        assert sim.ask(b"\x01\x0c", to=0x123) == []
        (refused,) = sim.ask(b"\x22\x7f\x22")
        assert refused.data == b"\x7f\x22\x31"
        assert sim.stats.requests >= 4


def test_each_reply_carries_its_module_s_timing():
    with havne.Sim.unbound("mock://sim?delay=2000:2000&fc=4000&cf=5000") as sim:
        (rpm,) = sim.ask(bytes([0x01, 0x0C]))
        assert (rpm.delay_us, rpm.fc_us, rpm.cf_us) == (2000, 4000, 5000)
    with havne.Sim.unbound() as sim:
        (rpm,) = sim.ask(bytes([0x01, 0x0C]))
        assert (rpm.delay_us, rpm.fc_us, rpm.cf_us) == (0, 0, 0)


def test_a_bad_request_is_refused():
    with havne.Sim.unbound() as sim:
        try:
            sim.ask(b"")
        except havne.Error as e:
            assert e.name == "INVALID_ARGUMENT"
        else:
            raise AssertionError("an empty request was taken")
