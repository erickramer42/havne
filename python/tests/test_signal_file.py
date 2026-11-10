# File/layer: python/tests/test_signal_file.py | pytest
# Testing: HARNESS, PythonTest.test_signal_file: a script's own signals,
#          as a signal file (ADR-0053 section 5), through the package
#
# tests/fixtures/signals/extras.json (extends obd2, a computed RPM in
# thousands) decoding the replay fixture, from its path and from its text;
# a refused file's line and column; a recording opened through the file
# it's given; the file recorded with its session; each signal's gauge as
# havne.Gauge, a level not given None, and None for a signal with no
# gauge or no such signal.

import os

import pytest

import havne

from conftest import REPLAY, SIGNALS, replay_uri


def test_a_signal_file_decodes_a_session():
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    assert extras.profile.startswith("user.") and extras.extends == "obd2"
    with havne.Session(replay_uri("?speed=max"), signals=extras) as s:
        assert "profile=" + extras.profile in s.uri
        values = s.values(capacity=havne.c.ECUIO_QUEUE_MAX_VALUES)
        s.start()
        got = list(values)
    rpm = {v.timestamp_us: v.value for v in got if v.name == "RPM"}
    krpm = [v for v in got if v.name == "RPM in Thousands"]
    assert krpm, "the computed signal"
    assert max(v.value for v in krpm) == pytest.approx(1.26125)
    road = [v for v in got if v.name == "Road Speed"]
    assert road and road[0].unit == "km/h"
    assert len(rpm) > 0


def test_a_signal_file_from_its_text():
    with open(os.path.join(SIGNALS, "extras.json"), encoding="utf-8") as f:
        extras = havne.SignalFile(f.read())
    assert extras.name == "Desk extras"


def test_a_refused_file_says_where():
    with pytest.raises(havne.Error) as e:
        havne.SignalFile('{"havne_signals": 1,\n "name": 7}')
    assert e.value.name == "INVALID_ARGUMENT"
    assert "line 2" in e.value.detail


def test_a_recording_opens_through_the_file_it_is_given():
    # Two files of one name share a profile, and the registry keeps the
    # later; the recording decodes through the one it's given
    path = os.path.join(SIGNALS, "extras.json")
    first = havne.SignalFile(path=path)
    with open(path, encoding="utf-8") as f:
        later = havne.SignalFile(f.read().replace("RPM in Thousands", "RPM in Kilos"))
    assert later.profile == first.profile
    with havne.Recording(REPLAY, signals=first) as r:
        names = [s.name for s in r.signals]
    assert "RPM in Thousands" in names and "RPM in Kilos" not in names
    with havne.Recording(REPLAY, signals=later) as r:
        assert "RPM in Kilos" in [s.name for s in r.signals]


def test_a_recording_takes_a_profile_or_a_file_not_both():
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    with pytest.raises(ValueError):
        havne.Recording(REPLAY, profile="obd2", signals=extras)


def test_a_session_s_recording_holds_its_signal_file(tmp_path):
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    with havne.Session(replay_uri("?speed=max"), signals=extras) as s:
        rec = s.record(tmp_path)
        values = s.values()
        s.start()
        for _ in values:
            pass
        folder = rec.folder
    with open(os.path.join(folder, "signals.json"), "rb") as f:
        with open(os.path.join(SIGNALS, "extras.json"), "rb") as g:
            assert f.read() == g.read()


def test_a_signal_file_says_each_signal_s_gauge():
    gauges = havne.SignalFile(path=os.path.join(SIGNALS, "gauges.json"))
    assert gauges.gauge("coolant") == havne.Gauge(40.0, 130.0, None, 110.0)
    assert gauges.gauge("volts") == havne.Gauge(10.0, 16.0, 11.5, 15.25)
    assert gauges.gauge("half_volts") == havne.Gauge(5.0, 8.0, None, None)
    assert gauges.gauge("load") is None
    assert gauges.gauge("no_such_signal") is None


def test_a_gauge_the_field_cannot_reach_is_refused_naming_the_unit():
    with open(os.path.join(SIGNALS, "gauges.json"), encoding="utf-8") as f:
        text = f.read().replace('"min": 40, "max": 130, "warn_high": 110',
                                '"min": 300, "max": 400')
    with pytest.raises(havne.Error, match="another unit"):
        havne.SignalFile(text)
