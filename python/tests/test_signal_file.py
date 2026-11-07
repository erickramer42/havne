# File/layer: python/tests/test_signal_file.py | pytest
# Testing: HARNESS, PythonTest.test_signal_file: a script's own signals,
#          as a signal file (ADR-0053 section 5), through the package
#
# tests/fixtures/signals/extras.json (extends obd2, a computed RPM in
# thousands) decoding the replay fixture, from its path and from its text;
# a refused file's line and column; the file recorded with its session.

import os

import pytest

import havne

from conftest import SIGNALS, replay_uri


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
