# File/layer: python/tests/test_recording_aids.py | pytest
# Testing: UNIT, PythonTest.test_recording_aids: a span's statistics, a
#          step and the stretches through the package
#
# Recording.stats over the pull fixture against Python's own reading of
# its signals.csv (a closed span, both ends inside a second); step to
# each side and past the ends; role_signal's picks; stretches over the
# cruise fixture with core's rules, a level of the caller's, a range cut
# at its edge and a role named; a road log's two pulls, the same ones
# havne-cli view --stretches lists, with a rise and a gap of the
# caller's; then the refusals. A cancel over the three searches
# (stretches, find_threshold, find_marker): stretches is asked once for
# each second of a recording about thirty seconds long, the two finds once
# (core's summaries pass a second by, so a find reads none here); a cancel
# never stopping them, stopping at an ask, raising, or a Ctrl+C.

import csv
import math
import os
import signal
import threading

import pytest

import havne
from conftest import ROOT, Asks, cli

RECORDINGS = os.path.join(ROOT, "tests", "fixtures", "recording")
PULL = os.path.join(RECORDINGS, "pull-early")
CRUISE = os.path.join(RECORDINGS, "cruise")
TWICE = os.path.join(RECORDINGS, "pull-twice")


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


def test_a_road_logs_pulls_are_its_two_and_not_its_stab():
    with havne.Recording(TWICE) as r:
        pulls = r.stretches("pulls")
        # Through an upshift whose lift is 925 ms, then in top gear
        assert [(s.kind, s.signal, s.from_us, s.to_us, s.start, s.end) for s in pulls] == [
            ("pull", "Vehicle Speed", 18_002_000, 25_000_000, 70.0, 128.0),
            ("pull", "Vehicle Speed", 31_002_000, 35_000_000, 140.0, 155.0)]
        assert all(s.flags == 0 for s in pulls)

        # The stab at 12 s climbs 180 rpm in all: a rise of 100 lets it in,
        # and a gain of 5 km/h doesn't, with engine speed in the log
        assert [s.from_us for s in r.stretches("pulls", pull_min_rise=100.0)] == [
            12_002_000, 18_002_000, 31_002_000]
        assert r.stretches("pulls", pull_min_gain=5.0) == pulls
        # A gap under the lift splits the first pull at it
        assert [s.from_us for s in r.stretches("pulls", pull_gap_us=900_000)] == [
            18_002_000, 22_427_000, 31_002_000]


def test_the_pulls_are_the_ones_havne_cli_lists():
    lines = cli("view", TWICE, "--stretches", "pulls", "--columns", 1)
    listed = [(x["from_us"], x["to_us"], x["start"], x["end"], x["signal"])
              for x in lines if x["type"] == "stretch"]
    with havne.Recording(TWICE) as r:
        assert listed == [(s.from_us, s.to_us, s.start, s.end, s.signal)
                          for s in r.stretches("pulls")]
        assert len(listed) == 2
        # And under the caller's rules, in each one's own words
        lines = cli("view", TWICE, "--stretches", "pulls", "--columns", 1,
                    "--stretch-level", "pull-rise=100", "--stretch-level", "pull-gap=0.9")
        assert [x["from_us"] for x in lines if x["type"] == "stretch"] == [
            s.from_us for s in r.stretches("pulls", pull_min_rise=100.0, pull_gap_us=900_000)]
        assert len([x for x in lines if x["type"] == "stretch"]) == 4
    # The same rules count the pulls --align-on pull: lines up on: with the
    # stab let in, the second pull is the one at 18 s
    lines = cli("view", TWICE, "--align-on", "pull:2", "--stretch-level", "pull-rise=100",
                "--columns", 1)
    (align,) = [x for x in lines if x["type"] == "align"]
    assert (align["found"], align["host_us"]) == (True, 18_002_000)


def test_a_recording_with_no_duration_has_no_stretches(tmp_path):
    # One row: its samples sit at one time, so the default range is empty
    log = tmp_path / "one.csv"
    log.write_text("time (s),Engine speed (rpm)\n0,800\n", encoding="utf-8", newline="\n")
    folder = tmp_path / "one"
    assert havne.import_csv(log, folder).duration_us == 0
    with havne.Recording(folder) as r:
        assert r.info.duration_us == 0
        assert r.stretches() == []
        assert r.stretches("pulls", pull_level=50.0) == []
        # A range the caller gives is still core's to judge
        with pytest.raises(havne.Error) as e:
            r.stretches(from_us=0, to_us=0)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(ValueError):
            r.stretches("pull")
        # And so are the rules, though the range holds nothing
        with pytest.raises(havne.Error) as e:
            r.stretches(pull_level=-1.0)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(cruise_floor=math.nan)
        assert e.value.name == "INVALID_ARGUMENT"
    # A start at or past the end of a recording that has a duration
    with havne.Recording(PULL) as r:
        assert r.stretches(from_us=r.info.duration_us) == []


def test_what_core_cant_answer_is_refused():
    with havne.Recording(PULL) as r:
        with pytest.raises(havne.Error) as e:
            r.stats(["RPM"], 5_000_000, 5_000_000)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(pull_level=-1.0)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(pull_min_rise=-1.0)
        assert e.value.name == "INVALID_ARGUMENT"
        with pytest.raises(havne.Error) as e:
            r.stretches(pull_min_gain=math.nan)
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


def stretches(r, cancel):
    return r.stretches(cancel=cancel)


def threshold(r, cancel):
    return r.find_threshold("RPM", 1e9, cancel=cancel)


def marker(r, cancel):
    return r.find_marker("no such marker", cancel=cancel)


def r_seconds(folder):
    with havne.Recording(folder) as r:
        return -(-r.info.duration_us // 1_000_000)


# (search, the ask the cases stop at, the asks a whole search makes at least)
SEARCHES = [(stretches, 3, 0), (threshold, 1, 1), (marker, 1, 1)]
IDS = ["stretches", "threshold", "marker"]


@pytest.mark.parametrize("search,at,whole", SEARCHES, ids=IDS)
def test_a_cancel_that_never_stops_a_search_gives_the_same_answer(search, at, whole):
    asks = Asks()
    with havne.Recording(PULL) as r:
        assert search(r, asks) == search(r, None)
    if search is stretches:
        # Two passes (the count, then the fill), each asking once at its
        # start and once before each second it reads
        assert asks.asked == 2 * (1 + r_seconds(PULL)), "once a second a pass"
    else:
        assert asks.asked >= whole


@pytest.mark.parametrize("search,at,whole", SEARCHES, ids=IDS)
def test_a_stop_ends_a_search_at_its_ask(search, at, whole):
    with havne.Recording(PULL) as r:
        asks = Asks(at)
        with pytest.raises(havne.Error) as e:
            search(r, asks)
        assert e.value.status == havne.c.ECUIO_ERR_CANCELED
        assert "canceled" in e.value.detail
        assert asks.asked == at, "it asked no further"
        search(r, None)  # the reader still answers


@pytest.mark.parametrize("search,at,whole", SEARCHES, ids=IDS)
def test_an_exception_in_a_searchs_cancel_reaches_the_caller(search, at, whole):
    with havne.Recording(PULL) as r:
        asks = Asks(at, RuntimeError("the caller's own"))
        with pytest.raises(RuntimeError, match="the caller's own"):
            search(r, asks)
        assert asks.asked == at
        asks = Asks(at, KeyboardInterrupt())
        with pytest.raises(KeyboardInterrupt):
            search(r, asks)
        assert asks.asked == at
        with pytest.raises(TypeError):
            search(r, 3)


@pytest.mark.parametrize("search,at,whole", SEARCHES, ids=IDS)
def test_a_ctrl_c_ends_a_search_at_its_next_ask(search, at, whole):
    # The search's handler stands in for Python's while core runs, and the
    # default is back after
    during = []

    def ctrl_c():
        during.append(signal.getsignal(signal.SIGINT))
        signal.raise_signal(signal.SIGINT)
        return False

    with havne.Recording(PULL) as r:
        asks = Asks(at, ctrl_c)
        with pytest.raises(KeyboardInterrupt):
            search(r, asks)
    assert asks.asked == at, "the flag ends it at the next ask without asking the cancel"
    assert during[0] is not signal.default_int_handler
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_a_search_off_the_main_thread_leaves_the_handler_alone():
    # Only the main thread may set a handler, and only it is interrupted
    asks = Asks()
    got = []
    with havne.Recording(PULL) as r:
        thread = threading.Thread(target=lambda: got.append(stretches(r, asks)))
        thread.start()
        thread.join()
        assert got == [stretches(r, None)]
    assert asks.asked > 2
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler
