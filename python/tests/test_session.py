# File/layer: python/tests/test_session.py | pytest
# Testing: HARNESS, PythonTest.test_session: the package against the
#          shared core on mock://, the simulated car, a replay folder,
#          the ELM327 emulator (havne-cli sim --elm327)
#
# Sessions and streams: values with their names and units, frames whole,
# a slow reader's drops counted and said once, a close from another
# thread ending the iteration, core's refusals as havne.Error, the
# caller's word passed to a clear, and a clear of the simulated car
# through core's gate, and the unsupported and absent signals. A session,
# sim or recording left unclosed is released when it's collected (a stream
# a script keeps keeps its session).

import contextlib
import gc
import json
import logging
import os
import subprocess
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


def test_the_vin_waits_for_core_to_have_it_in(monkeypatch):
    # The report stood in for (the mock identifies nothing): F190's VIN
    # beside a 09 02 still out is core's pick but not in yet
    def report(flags):
        ident = c.ecuio_channel_identity_t()
        ident.flags = flags
        ident.items = 2
        for i, (request, outcome, text) in enumerate(
                ((b"\x09\x02", c.ECUIO_IDENTITY_UNASKED, b""),
                 (b"\x22\xF1\x90", c.ECUIO_IDENTITY_ANSWERED, b"FAKEJ2534VIN00001"))):
            item = ident.list[i]
            for k, byte in enumerate(request):
                item.request[k] = byte
            item.length = len(request)
            item.outcome = outcome
            item.text = text
            item.text_length = len(text)
        return ident
    with havne.Session("mock://py") as s:
        assert s.vin(0) == ""
        monkeypatch.setattr(s, "identity", lambda channel=0: report(0))
        assert s.vin(0) == ""
        monkeypatch.setattr(s, "identity",
                            lambda channel=0: report(c.ECUIO_IDENTITY_VIN))
        assert s.vin(0) == "FAKEJ2534VIN00001"


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


def test_a_default_pick_is_named_pick_in_a_recording(tmp_path):
    """ECUIO_FOCUS_DEFAULT_PICK: a recording's header says "pick", not
    "focus"; with no signals it is core's INVALID_ARGUMENT"""
    with havne.Session("mock://py") as s:
        sid, (name, unit) = next(iter(s.signals.items()))
        with pytest.raises(havne.Error) as e:
            s.set_focus([], default_pick=True)
        assert e.value.name == "INVALID_ARGUMENT"
        assert "default pick" in e.value.detail
        s.set_focus([sid], default_pick=True)
        rec = s.record(tmp_path)
        folder = rec.folder
    assert ended(folder)
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        events = f.read()
    assert f"\npick  {name}\n" in events
    assert "\nfocus  " not in events


SESSIONS_IN_TURN = 20


@contextlib.contextmanager
def elm327_emulator():
    """The address of an ELM327 emulator (havne-cli sim --elm327 tcp:0),
    a session that asks its signals on every platform. Its later lines
    are read on a thread: a full pipe (Windows' holds 4 KiB) would hold
    the emulator up"""
    cli = os.environ.get("HAVNE_CLI", "")
    if not cli:
        pytest.skip("HAVNE_CLI names no havne-cli")
    emu = subprocess.Popen([cli, "sim", "mock://emu?values=canned", "--elm327", "tcp:0",
                            "--seconds", "120"], stdout=subprocess.PIPE, text=True)
    reader = None
    try:
        port = None
        for line in emu.stdout:
            j = json.loads(line)
            assert j["type"] != "error", j
            if j["type"] == "elm327":
                port = j["listen"].rsplit(":", 1)[1]
                break
        assert port, "the emulator said where it listens"
        reader = threading.Thread(target=emu.stdout.read, daemon=True)
        reader.start()
        yield f"elm327://127.0.0.1:{port}"
    finally:
        emu.terminate()
        if reader is None:
            emu.communicate(timeout=60)
        else:
            reader.join(timeout=60)
            emu.wait(timeout=60)
            emu.stdout.close()


def test_a_session_right_after_another_on_one_emulator_connects():
    """The emulator takes a host the moment the last one closed: a
    session made, closed and made again at once, as the cases on one
    emulator do"""
    with elm327_emulator() as address:
        for _ in range(SESSIONS_IN_TURN):
            with havne.Session(address) as s:
                assert s.signals


def test_the_ranked_signals_are_the_default_pick_havne_cli_makes():
    """ranked_signals(): cli/default_pick.h's rule, the app's and --pick
    ranked's: the set's ranked signals and those added, in the set's
    order, each once; a set that ranks none gives none, whatever is added,
    and so does a session that asks nothing"""
    with elm327_emulator() as address:
        with havne.Session(address + "?profile=obd2") as s:
            ranked = s.ranked_signals()
            assert len(ranked) == 14, "obd2's ranked signals"
            picked = [s.signals[i][0] for i in s.ranked_signals(["Fuel Level"])]
            assert picked[:9] == ["Engine Load", "Coolant Temp", "RPM", "Vehicle Speed",
                                  "Intake Air Temp", "Fuel Level", "Battery Voltage",
                                  "MAF", "Engine Oil Temp"]
            assert len(picked) == len(ranked) + 1
            assert s.ranked_signals(ranked) == ranked, "an added id already ranked is there once"
            with pytest.raises(KeyError):
                s.ranked_signals(["no such signal"])
            s.set_focus(ranked, default_pick=True)
    with havne.Session("mock://py?profile=obd2") as s:
        assert s.ranked_signals() == [], "the mock asks nothing, so it takes no pick"
        assert s.ranked_signals(["Fuel Level"]) == []
    with havne.Session("mock://py") as s:
        sid = next(iter(s.signals))
        assert s.ranked_signals() == []
        assert s.ranked_signals([sid]) == [], "an unranked set's pick is the whole set"


def test_a_ranked_text_signal_is_not_counted_in_the_pick():
    def file_of(temp_plot):
        temp = {"key": "temp", "name": "Temp", "read": {"service": "22", "number": "1E1C"},
                "field": {"byte": 0, "bits": 8}}
        if temp_plot:
            temp["plot"] = temp_plot
        return havne.SignalFile(json.dumps({
            "havne_signals": 1, "name": "Ranked text", "source": {"name": "a test"},
            "ecus": [{"id": "pcm", "rx": "7E8"}],
            "signals": [{"key": "part", "name": "Part", "kind": "text", "plot": 1,
                         "read": {"service": "22", "number": "F187"},
                         "field": {"byte": 0}}, temp]}))
    with elm327_emulator() as address:
        with havne.Session(address, signals=file_of(0)) as s:
            assert s.ranked_signals() == [], "a text rank alone ranks nothing"
        with havne.Session(address, signals=file_of(2)) as s:
            assert [s.signals[i][0] for i in s.ranked_signals()] == ["Temp"]


def test_a_focus_id_outside_a_uint16_or_not_an_int_is_a_value_error():
    with havne.Session("mock://py") as s:
        sid = next(iter(s.signals))
        for bad in (-1, 0x10000, 65536 + sid, True, 1.0, None):
            with pytest.raises(ValueError):
                s.set_focus([bad])
        s.set_focus([sid])


def test_a_session_that_asks_nothing_leaves_nothing_unasked():
    with havne.Session("mock://py") as s:
        assert s.unasked_signals() == []
        s.set_focus([next(iter(s.signals))])
        s.start()
        time.sleep(0.2)
        assert s.unasked_signals() == [], "the mock asks nothing"


def test_the_unasked_signals_follow_the_focus_on_a_session_that_asks():
    """Through the ELM327 emulator (havne-cli sim --elm327 tcp:0): a focus
    on engine speed leaves vehicle speed and Boost (whose MAP is parked)
    unasked, never engine speed itself; its end leaves none"""
    with elm327_emulator() as address:
        with havne.Session(address + "?profile=obd2") as s:
            ids = {name: sid for sid, (name, unit) in s.signals.items()}
            s.set_focus(["RPM"])
            s.start()

            def wait_for(done):
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    got = s.unasked_signals()
                    if done(got):
                        return got
                    time.sleep(0.05)
                return s.unasked_signals()

            unasked = wait_for(lambda got: len(got) > 0)
            assert ids["Vehicle Speed"] in unasked
            assert ids["Boost"] in unasked
            assert ids["RPM"] not in unasked
            assert unasked == sorted(unasked, key=list(s.signals).index), "the set's order"
            s.set_focus([])
            assert wait_for(lambda got: got == []) == []


def test_a_session_asks_no_extended_session_unless_the_uri_does():
    """ADR-0060: no ?session=03, no session state, as havne-cli writes no
    session line"""
    with havne.Session("mock://demo") as s:
        s.start()
        assert s.session_state(0) is None


def test_session_state_gives_a_refusal_its_code_and_nothing_else_does(monkeypatch):
    """A refused or abandoned session carries the module's NRC as two hex
    digits, as havne-cli's session line does; an entered one has none"""
    report = c.ecuio_channel_support_t()
    report.rx_id = 0x7E9
    report.generation = 1
    report.session_asked = 3
    report.session = 1
    report.session_state = c.ECUIO_SESSION_STATE_REFUSED
    report.session_nrc = 0x12
    report.session_entries = 2
    report.session_lost = 1
    with havne.Session("mock://demo") as s:
        monkeypatch.setattr(s, "support", lambda channel=0: report)
        state = s.session_state(0)
        assert state == ("0x7E9", "03", "01", "refused", "12", 2, 1)
        report.session_state = c.ECUIO_SESSION_STATE_ABANDONED
        assert s.session_state(0).state == "abandoned" and s.session_state(0).nrc == "12"
        report.session_state = c.ECUIO_SESSION_STATE_ENTERED
        report.session = 3
        entered = s.session_state(0)
        assert entered.state == "entered" and entered.session == "03" and entered.nrc is None


def test_a_session_with_nothing_to_judge_lists_no_unsupported_or_absent_signal():
    with havne.Session("mock://py") as s:
        assert s.unsupported_signals() == []
        assert s.absent_signals() == []


def test_the_demo_car_under_obd2_lists_the_signals_it_lacks():
    """mock:// under obd2 walks its demo car's PID bitmaps at create, so
    before start the signals the car doesn't answer are unsupported (PID 55
    among them) and engine and vehicle speed are not"""
    with havne.Session("mock://py?profile=obd2&diagnose=0") as s:
        ids = {name: sid for sid, (name, _) in s.signals.items()}
        lacking = s.unsupported_signals()
        assert 0 < len(lacking) < len(ids), "some, not all"
        assert ids["Rear O2 Short Trim B3"] in lacking
        assert ids["RPM"] not in lacking and ids["Vehicle Speed"] not in lacking
        assert lacking == sorted(lacking, key=list(s.signals).index), "the set's order"
        assert s.absent_signals() == []


def short_reply_replay(tmp_path, frames):
    """A replay folder (frames.csv alone, as the fixture) of frames answers
    to PID 55 that end after bank 1's byte"""
    folder = tmp_path / f"short-{frames}"
    folder.mkdir(exist_ok=True)
    rows = [f"{10000 * k},{10000 * k},0x7E8,0x0,41 55 80" for k in range(frames)]
    text = "\n".join(["host_us,timestamp_us,id,flags,data", *rows]) + "\n"
    (folder / "frames.csv").write_text(text, encoding="utf-8", newline="\n")
    return "replay://" + str(folder).replace("\\", "/") + "?profile=obd2&speed=max"


def test_a_field_past_every_reply_is_absent_once_enough_replies_have_ended(tmp_path):
    """A replay of PID 55 answered with bank 1's byte alone: bank 3's
    signal is absent after ECUIO_ABSENT_AFTER_FRAMES replies, not before,
    and not while no values stream is open"""
    n = c.ECUIO_ABSENT_AFTER_FRAMES
    for frames, opened, want in ((n - 1, True, False), (n, True, True), (n, False, False)):
        with havne.Session(short_reply_replay(tmp_path, frames)) as s:
            ids = {name: sid for sid, (name, _) in s.signals.items()}
            values = s.values(capacity=c.ECUIO_QUEUE_MAX_VALUES) if opened else None
            s.start()
            if values is not None:
                list(values)             # ends at the replay's last frame
            else:
                time.sleep(0.5)
            absent = s.absent_signals()
            assert (ids["Rear O2 Short Trim B3"] in absent) is want, (frames, opened, absent)
            assert ids["Rear O2 Short Trim B1"] not in absent, "bank 1's byte arrived"
            assert absent == sorted(absent, key=list(s.signals).index), "the set's order"


def test_a_list_that_grew_between_the_count_and_the_fill_is_read_again():
    """core says 3, then 5: the short fill is no answer. A stand-in for the
    getter, since no session grows its list between two calls on demand"""
    asks = []

    def getter(handle, ids, capacity, count):
        asks.append(capacity)
        count._obj.value = 3 if len(asks) == 1 else 5
        for i in range(min(capacity, count._obj.value) if ids else 0):
            ids[i] = 100 + i
        return 0

    class Stub:
        def _handle(self):
            return None

    assert havne.Session._listed(Stub(), getter, "stub") == [100, 101, 102, 103, 104]
    assert asks == [0, 3, 5]
