# File/layer: python/tests/test_conditions.py | pytest
# Testing: HARNESS, PythonTest.test_conditions: a condition file through
#          the package (ADR-0063)
#
# tests/fixtures/conditions/example.json from its path and from its text,
# field for field; a refused file's line, column and words; a file past
# core's bound read to a byte past it alone; each condition resolved
# against obd2, the mock, no signals and a signal file
# (tests/fixtures/signals/extras.json), against havne-cli's own lines
# where the tree has it; a set left unclosed and collected, and one
# closed. Then a set in a session: the replay fixture's engine start
# judged under obd2, its events equal to havne-cli stream's lines; the
# states before a frame and across attaches; keys bound through the
# session's signal file; a plugin's variant resolved; an attach refused
# while a recording is open. Then a recording's set and events
# (tests/fixtures/recording/conditions) against events.log read here and
# havne-cli view's lines, a held line from a ranged folder export, a copy
# changed, gone, too long or refused, a list cut at core's most, a cond
# line that doesn't parse, the replay above recorded, and a recorder's
# state, read while another thread closes it and when its read fails.

import ctypes
import gc
import hashlib
import json
import os
import shutil
import subprocess
import threading
import time

import pytest

import havne
from havne._core import lib

from conftest import FINDINGS, PLUGINS, ROOT, SIGNALS, replay_uri

CONDITIONS = os.path.join(ROOT, "tests", "fixtures", "conditions")
EXAMPLE = os.path.join(CONDITIONS, "example.json")
DESK = os.path.join(CONDITIONS, "desk.json")


def test_the_example_reads_field_for_field():
    s = havne.ConditionSet(path=EXAMPLE)
    assert s.name == "Daily driver"
    assert s.checks == havne.Checks(1000000, 10.0, 2000000)
    assert [c.key for c in s.conditions] == ["coolant_high", "pull", "link_fault",
                                             "module_slow"]
    hot, pull, link, slow = s.conditions
    assert hot == havne.Condition(
        "coolant_high", "Coolant high", True, "all",
        (havne.Term("signal", ("obd2.coolant",), "°C", "above", 110.0, 105.0, 10000000),),
        2000000, 0, havne.Alert("danger", True, True), None, True)
    assert pull.terms == (
        havne.Term("signal", ("obd2.throttle",), "%", "above", 80.0, 70.0, None),
        havne.Term("signal", ("id:1001", "id:7003"), "RPM", "above", 2500.0, None, None))
    assert pull.for_us == 500000 and pull.alert is None
    assert pull.record == havne.Record(5000000, 5000000) and pull.mark
    assert link.terms == (havne.Term("link", (), None, "not_ok", None, None, None),)
    assert link.record == havne.Record(10000000, 10000000) and not link.mark
    assert slow.terms == (havne.Term("check", (), None, "rate_drop", None, None, None),)
    assert slow.alert == havne.Alert("warning", False, True)
    assert repr(s) == "ConditionSet('Daily driver', 4 conditions)"


def test_a_set_from_its_text_and_from_bytes():
    with open(EXAMPLE, "rb") as f:
        data = f.read()
    from_bytes = havne.ConditionSet.load(data)
    from_text = havne.ConditionSet(data.decode("utf-8"))
    assert from_bytes.conditions == from_text.conditions
    assert from_bytes.conditions == havne.ConditionSet(path=EXAMPLE).conditions
    with pytest.raises(ValueError):
        havne.ConditionSet()
    with pytest.raises(ValueError):
        havne.ConditionSet("{}", path=EXAMPLE)


def test_what_a_file_leaves_out_is_none():
    s = havne.ConditionSet('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                           '"enabled":false,"when":{"any":[{"signal":["key:gear"],'
                           '"differs":2}]},"record":{}}]}')
    assert s.name == "" and s.checks == havne.Checks(None, None, None)
    c, = s.conditions
    assert not c.enabled and c.join == "any" and c.alert is None and not c.mark
    assert c.record == havne.Record(0, 0)
    assert c.terms == (havne.Term("signal", ("key:gear",), None, "differs", 2.0, None, None),)
    assert havne.ConditionSet('{"havne_conditions":1,"conditions":[]}').conditions == []


def test_a_refused_file_says_where():
    with pytest.raises(havne.Error) as e:
        havne.ConditionSet(path=os.path.join(CONDITIONS, "refused.json"))
    assert e.value.name == "INVALID_ARGUMENT"
    assert e.value.detail == ("line 10, column 17: unknown key 'abov' in condition "
                              "'pull''s when")
    with pytest.raises(havne.Error) as e:
        havne.ConditionSet("")
    assert e.value.name == "INVALID_ARGUMENT" and "line 1" in e.value.detail


def test_a_file_past_cores_bound_is_read_no_further(tmp_path, monkeypatch):
    bound = havne._ecuio.ECUIO_CONDITION_FILE_MAX_BYTES
    empty = b'{"havne_conditions":1,"conditions":[]}'
    at = tmp_path / "at.json"
    at.write_bytes(empty + b" " * (bound - len(empty)))
    assert havne.ConditionSet(path=str(at)).conditions == []
    over = tmp_path / "over.json"
    over.write_bytes(empty + b" " * (4 * bound))
    asked = []

    def spy(*args, **kwargs):
        f = open(*args, **kwargs)
        read = f.read

        def bounded(*n):
            asked.append(n)
            return read(*n)
        f.read = bounded
        return f
    monkeypatch.setattr(havne.conditions, "open", spy, raising=False)
    with pytest.raises(havne.Error) as e:
        havne.ConditionSet(path=str(over))
    assert e.value.name == "INVALID_ARGUMENT", e.value.detail
    assert asked == [(bound + 1,)]


def test_the_example_resolves_against_a_profile():
    s = havne.ConditionSet(path=EXAMPLE)
    obd2 = s.resolve("obd2")
    assert obd2 == [
        havne.Binding(True, None, None, (havne.Resolved("obd2.coolant", 7002),)),
        havne.Binding(True, None, None, (havne.Resolved("obd2.throttle", 7006),
                                         havne.Resolved("id:7003", 7003))),
        havne.Binding(True, None, None, (None,)),
        havne.Binding(True, None, None, (None,)),
    ]
    mock = s.resolve("mock")
    assert mock[0] == havne.Binding(False, "no_signal", 0, (None,))
    assert mock[1] == havne.Binding(False, "no_signal", 0,
                                    (None, havne.Resolved("id:1001", 1001)))
    assert mock[2].available and mock[3].available
    for none in (s.resolve(), s.resolve("raw")):
        assert [b.why for b in none] == ["no_signal", "no_signal", None, None]
    with pytest.raises(havne.Error) as e:
        s.resolve("obd3")
    assert e.value.name == "UNKNOWN_PROFILE"


def test_a_set_resolves_against_a_signal_file():
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    s = havne.ConditionSet(path=DESK)
    found = dict(zip((c.key for c in s.conditions), s.resolve(extras)))
    assert found["fast"].available
    assert found["fast"].terms[0].reference == "key:road_speed"
    assert found["stopped_hot"].available
    assert [t.reference for t in found["stopped_hot"].terms] == ["key:standstill",
                                                                 "key:trans_temp"]
    assert found["revs"].terms[1] == havne.Resolved("obd2.rpm", 7003)
    assert (found["wrong_unit"].why, found["wrong_unit"].why_term) == ("unit", 0)
    assert found["wrong_unit"].terms[0] is not None, "the signal is still named"
    assert found["text"].why == "not_a_number"
    assert found["ordered_enum"].why == "enum_order"
    assert found["elsewhere"] == havne.Binding(False, "no_signal", 1, (None, None))
    assert found["off"] == havne.Binding(False, "disabled", None, (None,))
    # The file's keys name nothing in obd2's own set
    assert not any(b.available for b in s.resolve("obd2")[:2])


def test_resolving_is_havne_clis_own():
    cli = os.environ.get("HAVNE_CLI")
    if not cli:
        pytest.skip("no havne-cli in this tree (HAVNE_CLI)")
    out = subprocess.run([cli, "conditions", "check", DESK, "--signals",
                          os.path.join(SIGNALS, "extras.json")],
                         capture_output=True, check=True, timeout=60).stdout
    lines = [json.loads(line) for line in out.decode("utf-8").splitlines()]
    defs = [line for line in lines if line["type"] == "condition_def"]
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    s = havne.ConditionSet(path=DESK)
    assert [d["key"] for d in defs] == [c.key for c in s.conditions]
    for d, c, b in zip(defs, s.conditions, s.resolve(extras)):
        assert (d["name"], d["enabled"], d["join"]) == (c.name, c.enabled, c.join)
        assert (d["for_us"], d["clear_for_us"], d["mark"]) == (c.for_us, c.clear_for_us,
                                                               c.mark)
        assert (d["available"], d["why"], d["why_term"]) == (b.available, b.why, b.why_term)
        for dt, t, r in zip(d["terms"], c.terms, b.terms):
            assert dt["kind"] == t.kind
            if t.kind != "signal":
                assert dt[t.kind] == t.test
                continue
            assert (dt["signal"], dt["unit"], dt["test"], dt["level"], dt["clear"],
                    dt["max_age_us"]) == (list(t.signal), t.unit, t.test, t.level, t.clear,
                                          t.max_age_us)
            got = dt["resolved"]
            assert (got and (got["reference"], got["id"])) == (r and tuple(r))


def counted(monkeypatch, name):
    """How often lib.<name> is called from here on, as a one-item list"""
    from havne._core import lib
    calls = [0]
    real = getattr(lib, name)

    def call(*args):
        calls[0] += 1
        return real(*args)

    monkeypatch.setattr(lib, name, call)
    return calls


def test_a_set_is_freed_once_closed_or_collected(monkeypatch):
    gc.collect()  # the sets the cases before left behind
    frees = counted(monkeypatch, "ecuio_condition_set_free")
    s = havne.ConditionSet(path=EXAMPLE)
    s.close()
    s.close()
    assert frees[0] == 1
    assert s.conditions[0].key == "coolant_high", "what it read stays"
    with pytest.raises(ValueError):
        s.resolve("obd2")
    left = havne.ConditionSet(path=EXAMPLE)
    del left
    gc.collect()
    assert frees[0] == 2


REPLAY_SET = os.path.join(CONDITIONS, "replay.json")


def test_a_replay_fires_its_conditions_as_havne_cli_says():
    with havne.Session(replay_uri("?profile=obd2&speed=max")) as s:
        events = s.conditions(capacity=havne.c.ECUIO_QUEUE_MAX_CONDITIONS)
        s.set_conditions(havne.ConditionSet(path=REPLAY_SET))
        before = s.condition_states()
        s.start()
        got = list(events)
        after = s.condition_states()
    assert events.ended == "stream ended" and events.dropped == 0
    assert [(b.key, b.state, b.stale, b.available, b.why) for b in before] == [
        ("running", "off", True, True, None), ("turning", "off", True, True, None),
        ("rpm_fresh", "off", True, True, None), ("link", "off", False, False, "no_link")]
    assert all(b.generation == 1 and b.fired == 0 for b in before)
    # The first judgment, the engine's start, and the hold's end at its own time
    assert got[0] == havne.ConditionEvent(366063, "running", "off", False, False, 0.0, 0, 1, 0)
    fired = [e for e in got if e.fired]
    assert [e.key for e in fired[:3]] == ["rpm_fresh", "running", "turning"]
    running = next(e for e in fired if e.key == "running")
    turning = next(e for e in fired if e.key == "turning")
    assert (running.timestamp_us, running.value) == (3806218, 943.25)
    assert turning.timestamp_us == 4451493, "a second after the level was crossed"
    assert any(e.stale for e in got if e.key == "rpm_fresh")
    assert not any(e.key == "link" for e in got)
    by_key = {a.key: a for a in after}
    assert by_key["running"].fired == sum(1 for e in fired if e.key == "running")
    assert by_key["running"].since_us == [e for e in got if e.key == "running"][-1].timestamp_us

    cli = os.environ.get("HAVNE_CLI")
    if not cli:
        pytest.skip("no havne-cli in this tree (HAVNE_CLI)")
    out = subprocess.run([cli, "stream", replay_uri("?profile=obd2&speed=max"),
                          "--conditions", REPLAY_SET],
                         capture_output=True, check=True, timeout=120).stdout
    lines = [json.loads(line) for line in out.decode("utf-8").splitlines()]
    said = [(r["t_us"], r["key"], r["state"], r["fired"], r["stale"], r["value"], r["term"])
            for r in lines if r["type"] == "condition"]
    assert said == [tuple(e)[:7] for e in got]
    listed = next(r for r in lines if r["type"] == "conditions")
    assert [(c["key"], c["available"], c["why"]) for c in listed["conditions"]] == [
        (b.key, b.available, b.why) for b in before]


def test_each_attach_is_a_generation_with_its_own_keys():
    with havne.Session("mock://conditions") as s:
        assert s.condition_states() == []
        s.set_conditions(havne.ConditionSet(path=REPLAY_SET))
        first = s.condition_states()
        assert [c.key for c in first] == ["running", "turning", "rpm_fresh", "link"]
        assert [c.why for c in first] == [None, "no_signal", "no_signal", "no_link"]
        assert first[0].generation == 1
        s.set_conditions(None)
        assert s.condition_states() == []
        s.set_conditions(havne.ConditionSet(
            '{"havne_conditions":1,"conditions":[{"key":"idle","name":"Idle",'
            '"when":{"signal":["id:1001"],"unit":"RPM","below":500}}]}'))
        third, = s.condition_states()
        assert (third.key, third.generation, third.available) == ("idle", 3, True)
        closed = havne.ConditionSet(path=REPLAY_SET)
        closed.close()
        with pytest.raises(ValueError):
            s.set_conditions(closed)
        assert s.condition_states()[0].generation == 3, "a refused attach changes nothing"


def test_keys_bind_through_the_sessions_signal_file():
    extras = havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))
    with havne.Session(replay_uri("?speed=max"), signals=extras) as s:
        s.set_conditions(havne.ConditionSet(path=DESK))
        states = {c.key: c for c in s.condition_states()}
    assert states["fast"].available and states["stopped_hot"].available
    assert states["wrong_unit"].why == "unit" and states["text"].why == "not_a_number"
    assert states["elsewhere"].why == "no_signal", "what the set settles comes first"
    assert states["off"].why == "disabled"


def test_a_set_resolves_against_a_plugins_variant():
    plugin = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    s = havne.ConditionSet(path=os.path.join(CONDITIONS, "plugin.json"))
    newest = dict(zip((c.key for c in s.conditions), s.resolve(plugin, "my2019")))
    assert all(b.available for b in newest.values())
    assert newest["slipping"].terms[0].reference == "key:clutch_slip"
    first = dict(zip((c.key for c in s.conditions), s.resolve(plugin)))
    assert first == dict(zip((c.key for c in s.conditions), s.resolve(plugin, "base")))
    assert (first["slipping"].why, first["slipping"].why_term) == ("no_signal", 0)
    assert first["oil_hot"].available and first["boosted"].available
    with pytest.raises(ValueError):
        s.resolve(plugin, "my2020")


def test_an_attach_is_refused_while_a_recording_is_open(tmp_path):
    with havne.Session("mock://conditions") as s:
        s.set_conditions(havne.ConditionSet(path=REPLAY_SET))
        recorder = s.record(str(tmp_path))
        with pytest.raises(havne.Error) as e:
            s.set_conditions(None)
        assert e.value.name == "IN_USE" and "recording" in e.value.detail
        assert len(s.condition_states()) == 4
        assert list(s._condition_keys) == [1], "a refused attach leaves no keys"
        recorder.close()
        s.set_conditions(None)
        assert s.condition_states() == []


ONE = ('{"havne_conditions":1,"conditions":[{"key":"%s","name":"One",'
       '"when":{"signal":["id:1001"],"unit":"RPM","below":500}}]}')


def test_an_attachs_keys_are_in_place_before_core_can_judge(monkeypatch):
    """A reader on another thread may pop the attach's first event before
    set_conditions returns: its key must be there by then"""
    from havne._core import lib

    real = lib.ecuio_session_set_conditions
    seen = []
    with havne.Session("mock://conditions") as s:
        def attach(*args):
            seen.append(dict(s._condition_keys))
            return real(*args)

        monkeypatch.setattr(lib, "ecuio_session_set_conditions", attach)
        s.set_conditions(havne.ConditionSet(ONE % "first"))
        s.set_conditions(havne.ConditionSet(ONE % "second"))
    assert seen == [{1: ("first",)}, {1: ("first",), 2: ("second",)}]


def test_a_session_keeps_the_newest_attaches_keys_alone():
    with havne.Session("mock://conditions") as s:
        kept = s.KEPT_GENERATIONS
        for n in range(kept + 3):
            s.set_conditions(havne.ConditionSet(ONE % ("k%d" % n)))
        assert sorted(s._condition_keys) == list(range(4, kept + 4))
        state, = s.condition_states()
        assert (state.key, state.generation) == ("k%d" % (kept + 2), kept + 3)


def test_the_states_are_whole_though_an_attach_grows_the_set_between_looks(monkeypatch):
    from havne._core import lib

    real = lib.ecuio_session_get_condition_states
    calls = []
    with havne.Session("mock://conditions") as s:
        s.set_conditions(havne.ConditionSet(ONE % "first"))

        def states(*args):
            rc = real(*args)
            calls.append(args[2])
            if len(calls) == 1:
                s.set_conditions(havne.ConditionSet(path=REPLAY_SET))
            return rc

        monkeypatch.setattr(lib, "ecuio_session_get_condition_states", states)
        got = s.condition_states()
    assert [c.key for c in got] == ["running", "turning", "rpm_fresh", "link"]
    assert {c.generation for c in got} == {2}
    assert calls == [0, 1, 4], "asked again with room for what it was told"


def test_an_interrupt_after_core_attached_keeps_the_attach(monkeypatch):
    """Ctrl+C can land once core has answered and before the answer is
    checked: the set is attached, so its keys and its count stay"""
    import havne.session

    real = havne.session.check

    def check(rc, call):
        if call == "ecuio_session_set_conditions" and rc == 0:
            raise KeyboardInterrupt
        return real(rc, call)

    with havne.Session("mock://conditions") as s:
        monkeypatch.setattr(havne.session, "check", check)
        with pytest.raises(KeyboardInterrupt):
            s.set_conditions(havne.ConditionSet(ONE % "first"))
        monkeypatch.setattr(havne.session, "check", real)
        state, = s.condition_states()
        assert (state.key, state.generation) == ("first", 1)
        s.set_conditions(havne.ConditionSet(ONE % "second"))
        state, = s.condition_states()
        assert (state.key, state.generation) == ("second", 2)


RECORDED = os.path.join(ROOT, "tests", "fixtures", "recording", "conditions")


def cond_lines(folder):
    """events.log's cond lines by Python alone, in a RecordedConditionEvent's order"""
    out = []
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        for line in f:
            if "  cond  " not in line:
                continue
            key, state, flags, value, term, host_us, t_us = (
                line.rstrip("\n").split("  cond  ", 1)[1].split(","))
            words = flags.split()
            out.append((int(host_us), int(t_us), key, state, "fired" in words,
                        "stale" in words, "held" in words, float(value), int(term)))
    return out


def test_a_recordings_set_and_events_are_read():
    with havne.Recording(RECORDED) as r:
        with open(os.path.join(RECORDED, "conditions.json"), "rb") as f:
            copy = f.read()
        assert r.conditions == havne.RecordedConditions(
            hashlib.sha256(copy).hexdigest(), True, True, 7, 0, False, 0)
        events = r.condition_events()
        assert [tuple(ev) for ev in events] == cond_lines(RECORDED)
        assert events[2] == havne.RecordedConditionEvent(
            7251000, 7250000, "coolant_high", "on", True, False, False, 111.0, 0)
        assert events[-1].stale and not events[-1].fired
        loaded = r.condition_set()
    assert (loaded.name, [c.key for c in loaded.conditions]) == (
        "Fixture", ["coolant_high", "rpm_high"])

    cli = os.environ.get("HAVNE_CLI")
    if not cli:
        pytest.skip("no havne-cli in this tree (HAVNE_CLI)")
    out = subprocess.run([cli, "view", RECORDED, "--columns", "3"], capture_output=True,
                         check=True, timeout=120).stdout
    lines = [json.loads(line) for line in out.decode("utf-8").splitlines()]
    said = [(r["host_us"], r["t_us"], r["key"], r["state"], r["fired"], r["stale"], r["held"],
             r["value"], r["term"]) for r in lines if r["type"] == "condition"]
    assert said == [tuple(ev) for ev in events]


def test_a_cut_list_says_so(monkeypatch):
    """Past ECUIO_RECORDING_MAX_CONDITION_EVENTS cond lines core keeps the
    first that many and says so in the info's flags (a million lines a
    case can't write: the flag is set on core's answer)"""
    real = lib.ecuio_recording_get_info

    def cut(r, out):
        rc = real(r, out)
        out._obj.flags |= havne.c.ECUIO_RECORDING_INFO_CONDITIONS_CUT
        return rc

    monkeypatch.setattr(lib, "ecuio_recording_get_info", cut)
    with havne.Recording(RECORDED) as r:
        assert r.conditions.cut and r.conditions.events == 7


def test_a_long_list_stops_at_cores_most(monkeypatch):
    """condition_events() asks for no event past the most core keeps"""
    monkeypatch.setattr(havne.c, "ECUIO_RECORDING_MAX_CONDITION_EVENTS", 3)
    with havne.Recording(RECORDED) as r:
        assert r.conditions.events == 7
        assert [tuple(ev) for ev in r.condition_events()] == cond_lines(RECORDED)[:3]


def test_a_cond_line_that_doesnt_parse_is_counted_and_left_out(tmp_path):
    torn = tmp_path / "torn"
    shutil.copytree(RECORDED, torn)
    log = torn / "events.log"
    whole = "cond  rpm_high,off,,4720,0,13376000,13375000"
    data = log.read_bytes().decode("utf-8")
    assert data.count(whole) == 1
    log.write_bytes(data.replace(whole, "cond  rpm_high,off,,4720,0,13376000").encode("utf-8"))
    with havne.Recording(torn) as r:
        assert (r.conditions.events, r.conditions.lines_skipped) == (6, 1)
        assert [tuple(ev) for ev in r.condition_events()] == [
            ev for ev in cond_lines(RECORDED) if ev[0] != 13376000]


def test_the_events_a_recorder_lost_are_counted(tmp_path):
    """The recorder's drop line for events it had no room for, summed"""
    with havne.Recording(RECORDED) as r:
        assert r.conditions.events_lost == 0
        assert r.conditions._fields[-1] == "events_lost"
    lost = tmp_path / "lost"
    shutil.copytree(RECORDED, lost)
    log = lost / "events.log"
    data = log.read_bytes().decode("utf-8")
    fired = "09:00:12.876  +12.876  cond  rpm_high,on,fired,5400,0,12876000,12875000"
    assert data.count(fired) == 1
    drop = "09:00:12.876  +12.876  drop  the recording lost %s: the disk fell behind"
    log.write_bytes(data.replace(fired, drop % "3 condition events").encode("utf-8"))
    with havne.Recording(lost) as r:
        assert (r.conditions.events, r.conditions.events_lost) == (6, 3)
        assert r.conditions.lines_skipped == 0
    again = data.replace(fired, drop % "3 condition events" + "\n" + drop % "1 condition event")
    log.write_bytes(again.encode("utf-8"))
    with havne.Recording(lost) as r:
        assert r.conditions.events_lost == 4


def test_a_range_opens_with_the_states_held(tmp_path):
    """A folder export from 10 s holds coolant_high ON then as a held line"""
    out = tmp_path / "from-10s"
    havne.export_folder(RECORDED, str(out), from_us=10_000_000)
    with havne.Recording(out) as r:
        events = r.condition_events()
    assert events[0] == havne.RecordedConditionEvent(
        10000000, 9999000, "coolant_high", "on", False, False, True, 111.0, 0)
    assert [ev.held for ev in events].count(True) == 1


def test_a_copy_that_changed_or_went_is_refused_and_none_is_none(tmp_path):
    changed = tmp_path / "changed"
    shutil.copytree(RECORDED, changed)
    with open(changed / "conditions.json", "ab") as f:
        f.write(b" ")
    with havne.Recording(changed) as r:
        assert (r.conditions.present, r.conditions.matches) == (True, False)
        with pytest.raises(ValueError, match="doesn't match its hash"):
            r.condition_set()
        assert len(r.condition_events()) == 7, "the events are read all the same"
    os.remove(changed / "conditions.json")
    with havne.Recording(changed) as r:
        assert r.conditions.sha256 is not None and not r.conditions.present
        with pytest.raises(ValueError, match="is missing"):
            r.condition_set()

    with havne.Recording(FINDINGS) as r:
        assert r.conditions == havne.RecordedConditions(None, False, False, 0, 0, False, 0)
        assert r.condition_events() == []
        assert r.condition_set() is None


def copy_with_conditions(tmp_path, name, data):
    """The fixture copied with conditions.json replaced and the header naming its hash"""
    folder = tmp_path / name
    shutil.copytree(RECORDED, folder)
    old = hashlib.sha256((folder / "conditions.json").read_bytes()).hexdigest()
    (folder / "conditions.json").write_bytes(data)
    log = folder / "events.log"
    text = log.read_bytes().decode("utf-8")
    assert text.count(old) == 1
    log.write_bytes(text.replace(old, hashlib.sha256(data).hexdigest()).encode("utf-8"))
    return folder


def test_a_matching_copy_the_loader_refuses_raises_its_words(tmp_path):
    folder = copy_with_conditions(tmp_path, "refused", b'{"havne_conditions": 2}')
    with havne.Recording(folder) as r:
        assert (r.conditions.present, r.conditions.matches) == (True, True)
        with pytest.raises(havne.Error) as raised:
            r.condition_set()
    assert not isinstance(raised.value, ValueError)


def test_a_copy_past_the_loaders_bound_says_so(tmp_path):
    folder = copy_with_conditions(
        tmp_path, "long", b" " * (havne.c.ECUIO_CONDITION_FILE_MAX_BYTES + 1))
    with havne.Recording(folder) as r:
        with pytest.raises(ValueError, match="longer than a set of conditions can be"):
            r.condition_set()


def test_a_recorded_replay_holds_the_events_the_session_gave(tmp_path):
    """The replay test_a_replay_fires_its_conditions_as_havne_cli_says
    judges, recorded: its cond lines are the session's events, the copy
    is the set, and the recorder lost none"""
    with havne.Session(replay_uri("?profile=obd2&speed=max")) as s:
        events = s.conditions(capacity=havne.c.ECUIO_QUEUE_MAX_CONDITIONS)
        s.set_conditions(havne.ConditionSet(path=REPLAY_SET))
        recorder = s.record(str(tmp_path))
        folder = recorder.folder
        assert recorder.state().state == "active"
        s.start()
        got = list(events)
    state = recorder.state()
    assert state.state == "detached", "read by the close, after the session's destroy"
    assert (state.records_dropped, state.conditions_lost, state.reason) == (0, 0, None)
    assert recorder.close() is None, "closed already, by the session's close"
    with havne.Recording(folder) as r:
        recorded = r.condition_events()
        assert r.conditions.matches
        loaded = r.condition_set()
    assert [(ev.timestamp_us, ev.key, ev.state, ev.fired, ev.stale, ev.value, ev.term)
            for ev in recorded] == [tuple(ev)[:7] for ev in got]
    assert not any(ev.held for ev in recorded)
    hosts = [ev.host_us for ev in recorded]
    assert hosts == sorted(hosts), "a replay at max writes no event before the frame before it"
    assert [c.key for c in loaded.conditions] == ["running", "turning", "rpm_fresh", "link"]


def test_a_recorders_state_is_cores(tmp_path, monkeypatch):
    with havne.Session("mock://conditions") as s:
        recorder = s.record(str(tmp_path))

        def failed(_r, out):
            st = out._obj
            st.state = havne.c.ECUIO_RECORDING_FAILED
            st.frame_rows, st.value_rows, st.records_dropped = 5, 6, 7
            st.notes_dropped, st.conditions_lost = 8, 9
            st.reason = b"the disk is full"
            return 0

        monkeypatch.setattr(lib, "ecuio_recorder_get_state", failed)
        assert recorder.state() == havne.RecorderState(
            "failed", 5, 6, 7, 8, "the disk is full", 9)
        monkeypatch.undo()
        assert recorder.state() == havne.RecorderState("active", 0, 0, 0, 0, None, 0)
        closed = recorder.close("done")
        assert closed.state == "active"
        assert recorder.state() == closed, "the state the close read"
        assert recorder.close() is None


def test_a_close_whose_state_cant_be_read_still_closes(tmp_path, monkeypatch):
    with havne.Session("mock://conditions") as s:
        recorder = s.record(str(tmp_path))
        folder = recorder.folder
        monkeypatch.setattr(lib, "ecuio_recorder_get_state",
                            lambda _r, _out: havne.c.ECUIO_ERR_INVALID_ARGUMENT)
        closes = counted(monkeypatch, "ecuio_recorder_close")
        with pytest.raises(havne.Error):
            recorder.close("done")
        assert closes == [1]
        assert recorder.close() is None and recorder.state() is None
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        assert "  end  done" in f.read()


def poll_while_closed(recorder):
    """Reads recorder.state() on a thread until close() returns, then once
    more: the reads, any exception, and close()'s answer"""
    reads, failed, stop, started = [], [], threading.Event(), threading.Event()

    def poll():
        try:
            while not stop.is_set():
                reads.append(recorder.state())
                started.set()
            reads.append(recorder.state())
        except Exception as err:  # noqa: BLE001 (reported by the case)
            failed.append(err)
            started.set()

    t = threading.Thread(target=poll)
    t.start()
    assert started.wait(10)
    closed = recorder.close()
    stop.set()
    t.join(10)
    assert not t.is_alive()
    return reads, failed, closed


def test_a_state_polled_while_another_thread_closes_is_whole(tmp_path):
    for run in range(20):
        with havne.Session("mock://conditions") as s:
            reads, failed, closed = poll_while_closed(s.record(str(tmp_path / str(run))))
        assert failed == [] and closed is not None
        assert all(isinstance(st, havne.RecorderState) for st in reads)
        assert reads[-1] == closed, "after the close, the state it read"


def test_a_close_waits_for_a_state_read_under_way(tmp_path, monkeypatch):
    """A state read held 50 ms inside core: the close waits it out, so no
    read meets the freed handle (each read checks, then calls core)"""
    real_state, real_close = lib.ecuio_recorder_get_state, lib.ecuio_recorder_close
    freed, met_freed = set(), []

    def handle(r):
        return ctypes.cast(r, ctypes.c_void_p).value

    def slow_state(r, out):
        time.sleep(0.05)
        if handle(r) in freed:
            met_freed.append(handle(r))
            return havne.c.ECUIO_ERR_INVALID_ARGUMENT
        return real_state(r, out)

    def close(r, why):
        freed.add(handle(r))
        return real_close(r, why)

    monkeypatch.setattr(lib, "ecuio_recorder_get_state", slow_state)
    monkeypatch.setattr(lib, "ecuio_recorder_close", close)
    for run in range(5):
        with havne.Session("mock://conditions") as s:
            reads, failed, closed = poll_while_closed(s.record(str(tmp_path / str(run))))
        freed.clear()
        assert (met_freed, failed) == ([], [])
        assert reads[-1] == closed


ARMED = ('{"havne_conditions":1,"conditions":[{"key":"high","name":"High",'
         '"when":{"signal":["obd2.rpm"],"unit":"RPM","above":1000},'
         '"record":{"before":2,"after":1}}]}')


def _windows(root):
    """Each window folder under root, by name, with its frames' session times"""
    out = []
    for name in sorted(os.listdir(root)):
        with open(os.path.join(root, name, "frames.csv"), encoding="utf-8") as f:
            rows = f.read().splitlines()[1:]
        out.append([int(r.split(",")[1]) for r in rows])
    return out


def test_an_armed_recorder_writes_havne_clis_windows(tmp_path):
    root = tmp_path / "py"
    with havne.Session(replay_uri("?profile=obd2&speed=max")) as s:
        events = s.conditions(capacity=havne.c.ECUIO_QUEUE_MAX_CONDITIONS)
        s.set_conditions(havne.ConditionSet(ARMED))
        r = s.record(str(root), armed=True)
        assert r.state().state == "armed" and r.folder == ""
        s.start()
        got = [e for e in events if e.key == "high"]
    state = r.state()   # the session's close closed it
    fired = [e for e in got if e.fired]
    assert state.windows == len(fired) == 2
    assert state.preroll_short == 0 and state.records_dropped == 0
    assert state.frame_rows + state.frames_passed == 7074
    windows = _windows(root)
    assert [len(w) for w in windows] == [151, 99]
    # Each window from its firing's before, in session time
    for w, e in zip(windows, fired):
        assert e.timestamp_us - 2000000 <= w[0] < e.timestamp_us

    cli = os.environ.get("HAVNE_CLI")
    if not cli:
        pytest.skip("no havne-cli in this tree (HAVNE_CLI)")
    sets = tmp_path / "armed.json"
    sets.write_text(ARMED, encoding="utf-8")
    out = subprocess.run([cli, "log", replay_uri("?profile=obd2&speed=max"),
                          str(tmp_path / "cli"), "--conditions", str(sets), "--armed"],
                         capture_output=True, check=True, timeout=120).stdout
    lines = [json.loads(line) for line in out.decode("utf-8").splitlines()]
    assert lines[-1]["windows"] == 2
    assert _windows(tmp_path / "cli") == windows


def test_an_armed_recorder_needs_a_condition_that_records(tmp_path):
    with havne.Session("mock://armed") as s:
        with pytest.raises(havne.Error) as e:
            s.record(str(tmp_path), armed=True)
        assert e.value.name == "INVALID_ARGUMENT"
        assert e.value.detail.startswith("no condition records")
        s.set_conditions(havne.ConditionSet(ONE % "plain"))
        with pytest.raises(havne.Error) as e:
            s.record(str(tmp_path), armed=True)
        assert e.value.detail.startswith("no condition records")
        s.record(str(tmp_path)).close()
    assert len(os.listdir(tmp_path)) == 1, "only the plain recording made a folder"
