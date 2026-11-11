# File/layer: python/tests/test_recording_aids.py | pytest
# Testing: UNIT, PythonTest.test_recording_aids: a span's statistics, a
#          step and the stretches through the package
#
# Recording.stats over the pull fixture against Python's own reading of
# its signals.csv (a closed span, both ends inside a second); step to
# each side and past the ends; role_signal's picks; stretches over the
# cruise fixture with core's rules, a level of the caller's, a range cut
# at its edge and a role named; then the refusals.

import csv
import math
import os

import pytest

import havne
from conftest import ROOT

RECORDINGS = os.path.join(ROOT, "tests", "fixtures", "recording")
PULL = os.path.join(RECORDINGS, "pull-early")
CRUISE = os.path.join(RECORDINGS, "cruise")


def samples_of(folder, signal):
    """[(host_us, value)] of the signal's rows in signals.csv"""
    with open(os.path.join(folder, "signals.csv"), encoding="utf-8-sig", newline="") as f:
        return [(int(r["host_us"]), float(r["value"]))
                for r in csv.DictReader(f) if r["signal"] == signal]


def test_a_spans_statistics_are_its_samples():
    rows = samples_of(PULL, "RPM")
    t0, t1 = 3_250_000, 17_100_000
    inside = [(t, v) for t, v in rows if t0 <= t <= t1]
    assert inside[-1][0] == t1, "the span's end lands on a sample, which it counts"
    with havne.Recording(PULL) as r:
        rpm, throttle = r.stats(["RPM", 1], t0, t1)
    values = [v for _, v in inside]
    assert rpm.samples == len(inside)
    assert rpm.first == [v for t, v in rows if t <= t0][-1]
    assert rpm.last == values[-1]
    assert rpm.lowest == min(values) and rpm.highest == max(values)
    assert rpm.lowest_us == next(t for t, v in inside if v == min(values))
    assert rpm.highest_us == next(t for t, v in inside if v == max(values))
    assert math.isclose(rpm.mean, sum(values) / len(values), rel_tol=1e-12)
    assert throttle.samples == sum(1 for t, _ in samples_of(PULL, "Throttle") if t0 <= t <= t1)


def test_a_text_signal_and_a_span_before_its_first_sample():
    with havne.Recording(PULL) as r:
        vin, rpm = r.stats(["VIN", "RPM"], 0, 50_000)
    assert vin.samples == 0 and math.isnan(vin.mean)
    assert rpm.samples == 0, "RPM starts at 100 ms"
    assert math.isnan(rpm.first) and math.isnan(rpm.lowest)
    assert rpm.lowest_us is None and rpm.highest_us is None


def test_a_step_goes_a_sample_either_way():
    rows = samples_of(PULL, "RPM")
    with havne.Recording(PULL) as r:
        assert r.step("RPM", rows[3][0]) == rows[4][0]
        assert r.step("RPM", rows[3][0], forward=False) == rows[2][0]
        assert r.step("RPM", rows[3][0] + 1, forward=False) == rows[3][0]
        assert r.step("RPM", rows[-1][0]) is None
        assert r.step("RPM", rows[0][0], forward=False) is None
        assert r.step("RPM", 0) == rows[0][0]


def test_each_role_has_its_signal_or_none():
    with havne.Recording(PULL) as r:
        assert r.role_signal("rpm").name == "RPM"
        assert r.role_signal("throttle").name == "Throttle"
        assert r.role_signal("speed") is None
        assert r.role_signal("load") is None
    with havne.Recording(CRUISE) as r:
        assert r.role_signal("speed").name == "Vehicle Speed"


def test_the_cruises_are_found_with_cores_rules_and_the_callers():
    with havne.Recording(CRUISE) as r:
        found = r.stretches("cruises")
        assert [(s.kind, s.signal, s.from_us, s.to_us) for s in found] == [
            ("cruise", "Vehicle Speed", 0, 11_901_000),
            ("cruise", "Vehicle Speed", 12_000_000, 24_901_000)]
        assert all(s.flags == 0 for s in found)
        assert math.isclose(found[0].start, 80.4, rel_tol=1e-12)
        assert found[0].start == found[0].end, "a cruise's mean in both"

        # A floor of 82 km/h leaves the first out; a longer hold both
        assert [s.from_us for s in r.stretches("cruises", cruise_floor=82.0)] == [12_000_000]
        assert r.stretches("cruises", cruise_hold_us=15_000_000) == []

        # A range ending inside the second cuts it at its edge
        cut = r.stretches("all", 0, 23_000_000)
        assert [s.to_us for s in cut] == [11_901_000, 23_000_000]
        assert cut[1].flags == havne.c.ECUIO_RECORDING_STRETCH_CUT_END

        # A role named in place of core's pick: no pull finds a cruise's
        # throttle, and the speed is the same signal
        named = r.stretches(roles={"speed": "Vehicle Speed", "throttle": "Throttle"})
        assert named == found
        assert r.stretches("pulls") == [], "22 % throttle is no pull"


def test_the_pull_is_found_by_engine_speed_without_a_speed_signal():
    with havne.Recording(PULL) as r:
        (pull,) = r.stretches()
    assert pull.kind == "pull" and pull.signal == "RPM"
    assert pull.flags == havne.c.ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED
    assert pull.end > pull.start


def test_what_core_cant_answer_is_refused():
    with havne.Recording(PULL) as r:
        with pytest.raises(havne.Error) as e:
            r.stats(["RPM"], 5_000_000, 5_000_000)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(pull_level=-1.0)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(roles={"throttle": "VIN"})
        assert e.value.name == "INVALID_ARGUMENT", "a text signal plays no role"
        with pytest.raises(KeyError):
            r.stats(["Boost"], 0, 1_000_000)
        with pytest.raises(ValueError):
            r.role_signal("brake")
        with pytest.raises(ValueError):
            r.stretches("pull")
        with pytest.raises(ValueError):
            r.stretches(roles={"brake": "RPM"})
        with pytest.raises(TypeError):
            r.stretches(pull_speed=1.0)
