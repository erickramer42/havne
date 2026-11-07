# File/layer: python/tests/test_session.py | pytest
# Testing: HARNESS, PythonTest.test_session: the package against the
#          shared core on mock://, the simulated car
#
# Sessions and streams: values with their names and units, frames whole,
# a slow reader's drops counted and said once, a close from another
# thread ending the iteration, core's refusals as havne.Error, the
# caller's word passed to a clear, and a clear of the simulated car
# through core's gate.

import logging
import threading
import time

import pytest

import havne
from havne import c


def take(stream, n, seconds=10):
    out, until = [], time.monotonic() + seconds
    while len(out) < n and time.monotonic() < until and not stream.ended:
        out += stream.read(0.1)
    return out


def test_values_come_with_their_names_and_units():
    with havne.Session("mock://py") as s:
        assert "RPM" in [n for n, _ in s.signals.values()]
        values = s.values()
        s.start()
        got = take(values, 40)
    assert len(got) >= 40
    rpm = [v for v in got if v.name == "RPM"]
    assert rpm and rpm[0].unit == "RPM"
    assert all(v.text is None for v in got if v.kind not in (c.ECUIO_VALUE_TEXT, c.ECUIO_VALUE_HEX))
    times = [v.timestamp_us for v in got]
    assert times == sorted(times)


def test_frames_come_whole():
    with havne.Session("mock://py") as s:
        frames = s.frames()
        s.start()
        got = take(frames, 5)
    assert len(got) >= 5
    f = got[0]
    assert f.flags & c.ECUIO_FRAME_FLAG_FD
    assert len(f.data) == 16   # the mock's telemetry frame, CAN FD's 16 bytes
    assert isinstance(f.data, bytes)


def test_a_slow_reader_s_drops_are_counted_and_said_once_each(caplog):
    with havne.Session("mock://py") as s:
        frames = s.frames(capacity=2)
        s.start()
        time.sleep(0.5)   # the mock sends a frame every 50 ms: the queue fills
        with caplog.at_level(logging.WARNING, logger="havne"):
            frames.read(0)
            first = frames.dropped
            frames.read(0)   # nothing new dropped: nothing said
    assert first > 0
    said = [r for r in caplog.records if "dropped so far" in r.getMessage()]
    assert len(said) == 1
    assert str(first) in said[0].getMessage()


def test_a_close_from_another_thread_ends_the_iteration():
    s = havne.Session("mock://py")
    values = s.values()
    s.start()
    seen = []
    reader = threading.Thread(target=lambda: seen.extend(v for v in values))
    reader.start()
    time.sleep(0.4)
    t = time.monotonic()
    s.close()
    reader.join(5)
    assert not reader.is_alive(), "the iteration ended"
    assert time.monotonic() - t < 5
    assert seen
    assert values.ended == "session closed"


def test_core_s_refusals_are_errors_with_their_name_and_detail():
    with pytest.raises(havne.Error) as e:
        havne.Session("mock://py?flavor=red")
    assert e.value.name == "BAD_URI"
    assert e.value.status == c.ECUIO_ERR_BAD_URI
    assert "flavor" in e.value.detail
    with pytest.raises(havne.Error) as e:
        havne.Session("mock://py?profile=no-such-profile")
    assert e.value.name == "UNKNOWN_PROFILE"


def test_a_stream_opens_only_before_the_start():
    with havne.Session("mock://py") as s:
        s.start()
        with pytest.raises(havne.Error) as e:
            s.values()
        assert e.value.name == "INVALID_ARGUMENT"


def test_the_info_says_the_mock():
    with havne.Session("mock://py") as s:
        assert s.info.backend == c.ECUIO_BACKEND_MOCK
        assert s.diagnostics(0).flags & c.ECUIO_DIAG_COMPLETE


def test_a_clear_needs_the_caller_s_word():
    with havne.Session("mock://py") as s:
        with pytest.raises(ValueError):
            s.clear(0)


def test_a_clear_passes_the_caller_s_word_for_the_engine_and_the_speed(monkeypatch):
    # The flags alone, the call stood in for: core's gate itself is the
    # case below's and ClearTest's
    from havne import session as module
    calls = []
    monkeypatch.setattr(module.lib, "ecuio_session_clear_diagnostics_with",
                        lambda handle, channel, generation, flags: calls.append(flags) or 0)
    with havne.Session("mock://py") as s:
        for running in (False, True):
            for moving in (False, True):
                s.clear(0, confirm=True, engine_running=running, moving=moving)
    assert calls == [0, c.ECUIO_CLEAR_ALLOW_MOVING, c.ECUIO_CLEAR_ALLOW_RUNNING,
                     c.ECUIO_CLEAR_ALLOW_RUNNING | c.ECUIO_CLEAR_ALLOW_MOVING]


def test_a_clear_of_the_simulated_car_goes_through_core_s_gate():
    # The simulated car only: its engine is stopped for the first 15 s
    # after the connect, so core's gate lets 04 out
    with havne.Session("mock://py?profile=obd2") as s:
        s.start()
        before = s.diagnostics(0)
        assert before.lists[c.ECUIO_DIAG_STORED].reported > 0
        s.clear(0, confirm=True)
        until = time.monotonic() + 10
        while time.monotonic() < until and s.diagnostics(0).clear == c.ECUIO_CLEAR_NONE:
            time.sleep(0.05)
        after = s.diagnostics(0)
        assert after.clear == c.ECUIO_CLEAR_CLEARED
        until = time.monotonic() + 10
        while time.monotonic() < until and not s.diagnostics(0).flags & c.ECUIO_DIAG_COMPLETE:
            time.sleep(0.05)
        assert s.diagnostics(0).lists[c.ECUIO_DIAG_STORED].reported == 0
