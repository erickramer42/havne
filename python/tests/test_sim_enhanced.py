import csv
import json
import os

import pytest

import havne

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRUTH = os.path.join(ROOT, "tests", "fixtures", "discovery", "sim-enhanced-truth.json")


def truth():
    with open(TRUTH, encoding="utf-8") as f:
        return json.load(f)


def field(did, key):
    return next(f for f in did["fields"] if f["key"] == key)


def decoded(data, f):
    body = data[3:]
    if f["order"] == "be":
        raw = int.from_bytes(body[f["byte"]:f["byte"] + f["bits"] // 8], "big", signed=f["signed"])
    else:
        n = (f["bit"] + f["bits"] + 7) // 8
        raw = int.from_bytes(body[f["byte"]:f["byte"] + n], "little") >> f["bit"]
        raw &= (1 << f["bits"]) - 1
        if f["signed"] and raw >= 1 << (f["bits"] - 1):
            raw -= 1 << f["bits"]
    return raw * f["scale"] + f["offset"]


SAMPLE = os.path.join(ROOT, "python", "examples", "data", "discovery-sample")


def coolant_by_time():
    """The sample's obd2 coolant (PID 05) as (host_us, deg C), from its signals.csv"""
    with open(os.path.join(SAMPLE, "signals.csv"), encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))[1:]
    return [(int(r[0]), float(r[2])) for r in rows if r[1] == "7E8:4105:2.0:8"]


def test_the_sample_holds_every_did_and_its_coolant_is_the_pids():
    dids = {d["did"]: d for d in truth()["dids"]}
    coolant = coolant_by_time()
    assert len(coolant) >= 100
    seen = {k: 0 for k in dids}
    close = compared = 0
    k = 0
    for f in havne.read_frames(SAMPLE):
        if f.data[:1] != b"\x62":
            continue
        key = f.data[1:3].hex().upper()
        assert key in dids and f.id == int(dids[key]["module"], 16)
        seen[key] += 1
        while k + 1 < len(coolant) and coolant[k + 1][0] <= f.host_us:
            k += 1
        if key == "FD00" and abs(coolant[k][0] - f.host_us) < 1_000_000:
            compared += 1
            close += abs(decoded(f.data, field(dids[key], "coolant")) - coolant[k][1]) <= 1.5
    assert min(seen.values()) >= 100, seen
    assert compared >= 100 and close >= 0.9 * compared, (close, compared)


def test_off_by_default_the_dids_are_refused():
    with havne.Sim.unbound("mock://sim?modules=2") as sim:
        (r,) = sim.ask(b"\x22\xfd\x00")
        assert r.data == b"\x7f\x22\x31"


def test_the_engine_dids_follow_the_drive_cycle_as_the_truth_reads_them():
    did = next(d for d in truth()["dids"] if d["did"] == "FD00")
    rpm = field(did, "rpm")
    seen = set()
    with havne.Sim.unbound("mock://sim?enhanced=1") as sim:
        for t in range(0, 60_000_000, 500_000):
            (pid,) = sim.ask(b"\x01\x0c", at_us=t)
            (r,) = sim.ask(b"\x22\xfd\x00", at_us=t)
            assert r.id == 0x7E8 and r.data[:3] == b"\x62\xfd\x00"
            want = int.from_bytes(pid.data[2:4], "big") / 4
            assert abs(decoded(r.data, rpm) - want) <= 0.25
            seen.add(round(want))
    assert len(seen) > 10


def test_the_transmission_module_answers_its_dids():
    with havne.Sim.unbound("mock://sim?enhanced=1") as sim:
        (r,) = sim.ask(b"\x22\xfd\x80", to=0x7E1)
        assert r.id == 0x7E9 and len(r.data) == 16
        (old,) = sim.ask(b"\x22\xd1\x00", to=0x7E1)
        assert old.data == b"\x62\xd1\x00\x82"


def test_a_prompt_bit_holds_what_a_test_gives_it():
    did = next(d for d in truth()["dids"] if d["did"] == "FD05")
    door = field(did, "door")
    with havne.Sim.unbound("mock://sim?enhanced=1") as sim:
        sim.set_value(door["id"], 1)
        (r,) = sim.ask(b"\x22\xfd\x05", at_us=30_000_000)
        assert decoded(r.data, door) == 1
        sim.set_value(door["id"], 0)
        (r,) = sim.ask(b"\x22\xfd\x05", at_us=30_100_000)
        assert decoded(r.data, door) == 0
        with pytest.raises(havne.Error):
            sim.set_value(door["id"], 2)
    with havne.Sim.unbound("mock://sim") as sim:
        with pytest.raises(havne.Error):
            sim.set_value(door["id"], 1)
