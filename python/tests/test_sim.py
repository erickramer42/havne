# File/layer: python/tests/test_sim.py | pytest
# Testing: HARNESS, PythonTest.test_sim: the simulated ECU asked through
#          the package with no bus (ecuio_sim_ask, ADR-0055)
#
# The engine ECU's answers to a script's requests: engine speed, the VIN
# (the sim's made-up default), a functional request, an ID no ECU hears,
# a refusal, and the request counts; a module's timing on each reply;
# a module behind the simulated gateway, unlocked by a key the script
# works out from the transform sim.h states, and the gateway's view.

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


def test_each_module_answers_its_own_codes():
    with havne.Sim.unbound("mock://sim?modules=2&codes=refused@7E9") as sim:
        (codes,) = sim.ask(b"\x19\x02\xaf")
        assert codes.id == 0x7E8
        assert codes.data[:3] == b"\x59\x02\xff"
        assert len(codes.data) == 3 + 4 * 4
        assert b"\x03\x01\x00\x08" in codes.data   # P0301-00, stored in J1979 too
        (refused,) = sim.ask(b"\x19\x02\xaf", to=0x7E1)
        assert refused.data == b"\x7f\x19\x11"


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


def made_up_key(seed_reply):
    """The key request for a 67 <level> <seed> reply: the seed reversed,
    each byte XORed with a letter of HAVNE (sim.h)"""
    seed = seed_reply[2:]
    return bytes([0x27, seed_reply[1] + 1]) + bytes(
        b ^ ord("HAVNE"[i % 5]) for i, b in enumerate(reversed(seed)))


def test_a_script_unlocks_the_gateway_and_reads_the_module_behind_it():
    uri = ("mock://sim?modules=2&gateway=6F0:6F8&behind=7E9&sa=key"
           "&route=reads:gated,19:refuse.31@7E9")
    d100 = bytes([0x22, 0xD1, 0x00])
    with havne.Sim.unbound(uri) as sim:
        before = sim.gateway
        assert (before.request_id, before.reply_id) == (0x6F0, 0x6F8)
        assert before.routes["reads"] == "gated" and before.now["reads"] == "refuse"
        assert before.routes["write"] == "forward"
        assert not before.unlocked and before.unlocked_level == 0
        (o,) = before.overrides
        assert (o.reply_id, o.service, o.cls, o.route, o.nrc) == (0x7E9, 0x19, None,
                                                                  "refuse", 0x31)

        (locked,) = sim.ask(d100, to=0x7E1, at_us=0)
        assert locked.id == 0x7E9 and locked.data == bytes([0x7F, 0x22, 0x33])
        assert locked.flags & havne._ecuio.ECUIO_SIM_REPLY_GATEWAY
        assert sim.ask(bytes([0x10, 0x03]), to=0x6F0, at_us=0)[0].data[0] == 0x50
        (seed,) = sim.ask(bytes([0x27, 0x01]), to=0x6F0, at_us=0)
        (unlocked,) = sim.ask(made_up_key(seed.data), to=0x6F0, at_us=0)
        assert unlocked.data == bytes([0x67, 0x02])

        (answer,) = sim.ask(d100, to=0x7E1, at_us=0)
        assert answer.data == bytes([0x62, 0xD1, 0x00, 0x82]) and answer.flags == 0
        after = sim.gateway
        assert after.unlocked and after.extended and after.unlocked_level == 1
        assert after.now["reads"] == "forward" and after.routes["reads"] == "gated"
        assert (after.refused, after.forwarded, after.seeds, after.unlocks) == (1, 1, 1, 1)


def test_a_sim_with_no_gateway_says_so():
    with havne.Sim.unbound() as sim:
        try:
            sim.gateway
        except havne.Error as e:
            assert e.name == "INVALID_ARGUMENT"
        else:
            raise AssertionError("a gateway's view with no gateway")
