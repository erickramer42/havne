# File/layer: python/tests/test_conditions.py | pytest
# Testing: HARNESS, PythonTest.test_conditions: a condition file through
#          the package (ADR-0063)
#
# tests/fixtures/conditions/example.json from its path and from its text,
# field for field; a refused file's line, column and words; each
# condition resolved against obd2, the mock, no signals and a signal file
# (tests/fixtures/signals/extras.json), against havne-cli's own lines
# where the tree has it; a set left unclosed and collected, and one
# closed. Then a set in a session: the replay fixture's engine start
# judged under obd2, its events equal to havne-cli stream's lines; the
# states before a frame and across attaches; keys bound through the
# session's signal file; a plugin's variant resolved; an attach refused
# while a recording is open.

import gc
import json
import os
import subprocess

import pytest

import havne

from conftest import PLUGINS, ROOT, SIGNALS, replay_uri

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
    assert got[0] == havne.ConditionEvent(366063, "running", "off", False, False, 0.0, 0, 1)
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
