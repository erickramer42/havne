# File/layer: python/tests/test_did_scan.py | pytest
# Testing: HARNESS, PythonTest.test_did_scan (the mock everywhere; Linux,
#          vcan0 for the sim)
#
# python/examples/did_scan.py run as a user runs it: core's DID scan
# (ADR-0020) through a session. The mock answers every DID itself; on
# vcan0 the simulated ECU's second module answers its own DIDs and the
# engine ECU refuses Service 0x22, and the unbound sim, asked each DID of
# the range, is the oracle for what the scan must print.

from conftest import need_vcan, run_example

import havne

FIRST, LAST = 0xD100, 0xD10F


def printed(out):
    """{(rx, did): (byte count, what)} of the scan's answer lines"""
    found = {}
    for line in out.splitlines():
        parts = line.split("  ", 3)
        if len(parts) == 4 and parts[2].split()[-1] in ("byte", "bytes"):
            found[(int(parts[0], 16), int(parts[1], 16))] = (int(parts[2].split()[0]), parts[3])
    return found


def test_the_mock_answers_every_did_of_the_range():
    rc, out, err = run_example("did_scan.py", "mock://scan", "--range", "F180-F18F",
                               "--seconds", "30")
    assert rc == 0, err
    found = printed(out)
    assert sorted(found) == [(0x7E8, d) for d in range(0xF180, 0xF190)]
    assert "7E8  found 16: the transport answers DIDs itself" in out


def test_a_bad_range_or_a_uri_with_a_profile_is_refused():
    for args in (["--range", "F1FF-F180"], ["--range", "F180"], ["--range", "F180-1FFFF"],
                 ["--ecu", "7G8"]):
        rc, _, err = run_example("did_scan.py", "mock://scan", *args)
        assert rc == 2 and err.startswith("did_scan: "), (args, err)
    rc, _, err = run_example("did_scan.py", "mock://scan?profile=obd2")
    assert rc == 2 and "sets the profile itself" in err, err


def test_a_module_answers_its_own_dids_and_the_engine_ecu_refuses_them():
    iface = need_vcan()
    expected = {}
    with havne.Sim.unbound("mock://sim?modules=2") as oracle:
        for rx, tx in ((0x7E8, 0x7E0), (0x7E9, 0x7E1)):
            for did in range(FIRST, LAST + 1):
                for r in oracle.ask(bytes([0x22, did >> 8, did & 0xFF]), to=tx):
                    if r.id == rx and r.data[:3] == bytes([0x62, did >> 8, did & 0xFF]):
                        expected[(rx, did)] = r.data[3:]
    assert expected and all(rx == 0x7E9 for rx, _ in expected)   # the engine refuses 0x22

    with havne.Sim("mock://sim?values=canned&modules=2", interface=iface):
        rc, out, err = run_example("did_scan.py", f"socketcan://{iface}",
                                   "--range", f"{FIRST:04X}-{LAST:04X}",
                                   "--ecu", "7E8", "--ecu", "7E9", "--seconds", "60")
    assert rc == 0, err
    found = printed(out)
    assert sorted(found) == sorted(expected)
    for key, data in expected.items():
        n, what = found[key]
        assert n == len(data), key
        body = data.rstrip(b"\0")
        if what.startswith('"'):
            assert what == f'"{body.decode("ascii")}"', key
        else:
            assert bytes.fromhex(what) == data, key
    size = LAST - FIRST + 1
    mine = len(expected)
    assert f"7E9  scanned {size} of {size}, found {mine}, refused {size - mine}" in out
    assert f"7E8  scanned {size} of {size}, found 0, refused {size}" in out
