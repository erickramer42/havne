# File/layer: python/tests/test_session.py | pytest
# Testing: HARNESS, PythonTest.test_session: the package against the
#          shared core on mock://, the simulated car
#
# Sessions and streams: values with their names and units, frames whole,
# a slow reader's drops counted and said once, a close from another
# thread ending the iteration, core's refusals as havne.Error, the
# caller's word passed to a clear, and a clear of the simulated car
# through core's gate. A session, sim or recording left unclosed is
# released when it's collected (a stream a script keeps keeps its session).

import gc
import logging
import os
import threading
import time
import weakref

import pytest

import havne
from havne import c
from havne._core import lib

from conftest import REPLAY


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


def iterated_on_a_thread(stream, per_value=0.0):
    """The stream iterated on a thread of its own, sleeping per_value
    after each value: (the thread, what it saw)"""
    seen = []

    def run():
        for v in stream:
            seen.append(v)
            time.sleep(per_value)

    reader = threading.Thread(target=run, daemon=True)
    reader.start()
    return reader, seen


def test_a_stream_iterated_after_its_close_ends():
    s = havne.Session("mock://py")
    values = s.values()
    s.start()
    time.sleep(0.3)
    s.close()
    assert values.ended == "session closed", "said at the close, before any read"
    reader, _ = iterated_on_a_thread(values)
    reader.join(5)
    assert not reader.is_alive(), "the iteration ended"
    assert values.ended == "session closed"
    assert values.read() == []


def test_a_close_while_the_reader_is_in_its_loop_body_ends_the_iteration():
    s = havne.Session("mock://py")
    values = s.values()
    s.start()
    reader, seen = iterated_on_a_thread(values, per_value=0.05)
    assert take_until(lambda: seen, 5), "values arrive"
    s.close()
    reader.join(5)
    assert not reader.is_alive(), "the iteration ended"
    assert values.ended == "session closed"


def take_until(cond, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if cond():
            return True
        time.sleep(0.02)
    return False


def ended(folder):
    """Whether the recording's events.log has its end line: the
    recorder closed, after its session's close and destroy"""
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        return any(line.split("  ")[2:3] == ["end"] for line in f.read().splitlines())


def test_an_unclosed_session_collected_is_closed_in_close_s_order(tmp_path):
    # A notebook cell run again drops the last session unclosed: its
    # worker stops and its recording ends when it's collected
    s = havne.Session("mock://py")
    values = s.values()
    rec = s.record(tmp_path)
    s.start()
    assert take(values, 1), "the mock's values"
    folder, gone = rec.folder, weakref.ref(s)
    del s, values, rec
    gc.collect()
    assert gone() is None, "nothing the package holds keeps the session"
    assert ended(folder), "the recording ended when its session was collected"


def test_a_stream_a_script_keeps_keeps_its_session(tmp_path):
    s = havne.Session("mock://py")
    values = s.values()
    rec = s.record(tmp_path)
    s.start()
    folder, gone = rec.folder, weakref.ref(s)
    del s, rec
    gc.collect()
    assert gone() is not None and not ended(folder)
    assert take(values, 1), "the session still runs"
    del values
    gc.collect()
    assert gone() is None and ended(folder)


def counted(monkeypatch, name):
    """Each call of lib's name counted, then made"""
    real, calls = getattr(lib, name), []

    def counting(*args):
        calls.append(args)
        return real(*args)

    monkeypatch.setattr(lib, name, counting)
    return calls


def test_an_unclosed_sim_and_recording_collected_are_released(monkeypatch):
    destroyed = counted(monkeypatch, "ecuio_sim_destroy")
    closed = counted(monkeypatch, "ecuio_recording_close")
    sim, recording = havne.Sim.unbound(), havne.Recording(REPLAY, profile="obd2")
    del sim, recording
    gc.collect()
    assert (len(destroyed), len(closed)) == (1, 1)
    with havne.Sim.unbound() as sim, havne.Recording(REPLAY, profile="obd2") as recording:
        pass
    del sim, recording
    gc.collect()
    assert (len(destroyed), len(closed)) == (2, 2), "a closed one isn't released twice"


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


def test_a_recorder_opened_after_start_counts_from_then(tmp_path):
    # The analysis log (ADR-0056 section 3): a recorder on a running
    # session holds the frames from its open, its host_us from then,
    # while the mock's timestamp_us counts from the start
    with havne.Session("mock://py") as s:
        values = s.values()
        s.start()
        assert take(values, 1), "the mock's values"
        time.sleep(0.3)
        rec = s.record(tmp_path)
        with pytest.raises(havne.Error) as e:
            s.record(tmp_path)
        assert e.value.name == "INVALID_ARGUMENT", "one recorder at a time"
        folder = rec.folder
        # What was queued before the attach drains first; a value stamped
        # well past the newest of those came from a frame dispatched
        # after it, which the recording holds
        at_attach = max([v.timestamp_us for v in values.read(0)] + [0])
        newest, until = at_attach, time.monotonic() + 5
        while newest <= at_attach + 200_000 and time.monotonic() < until:
            for v in values.read(0.1):
                newest = max(newest, v.timestamp_us)
        assert newest > at_attach + 200_000, "values from after the attach"
    assert ended(folder)
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        assert "\nlink  healthy (state 0, 0 episodes)\n" in f.read()
    with open(os.path.join(folder, "frames.csv"), encoding="utf-8") as f:
        rows = f.read().splitlines()[1:]
    assert len(rows) >= 2
    host_us, timestamp_us = (int(x) for x in rows[0].split(",")[:2])
    assert timestamp_us - host_us >= 200_000, "the session was older than the recording"


def test_a_focus_takes_ids_or_names_and_refuses_a_stranger():
    with havne.Session("mock://py") as s:
        sid, (name, unit) = next(iter(s.signals.items()))
        s.set_focus([sid])
        s.set_focus([name])
        s.set_focus([])
        with pytest.raises(KeyError):
            s.set_focus(["no such signal"])
        with pytest.raises(havne.Error) as e:
            s.set_focus([65000])
        assert e.value.name == "INVALID_ARGUMENT"
        assert "65000" in e.value.detail
        s.start()
        s.set_focus([sid])


def test_a_focus_takes_a_minimum_time_between_sweeps():
    with havne.Session("mock://py") as s:
        sid = next(iter(s.signals))
        s.set_focus([sid], min_sweep_ms=250)
        s.set_focus([sid], min_sweep_ms=60000)
        s.set_focus([sid], min_sweep_ms=0)
        for bad in (-1, 1.5, True, None, 0x100000000):
            with pytest.raises(ValueError):
                s.set_focus([sid], min_sweep_ms=bad)
        with pytest.raises(havne.Error) as e:
            s.set_focus([sid], min_sweep_ms=60001)
        assert e.value.name == "INVALID_ARGUMENT"
        assert "60000" in e.value.detail
        with pytest.raises(havne.Error) as e:
            s.set_focus([], min_sweep_ms=250)
        assert e.value.name == "INVALID_ARGUMENT"
        assert "focus" in e.value.detail


def test_a_focus_id_outside_a_uint16_or_not_an_int_is_a_value_error():
    with havne.Session("mock://py") as s:
        sid = next(iter(s.signals))
        for bad in (-1, 0x10000, 65536 + sid, True, 1.0, None):
            with pytest.raises(ValueError):
                s.set_focus([bad])
        s.set_focus([sid])
