# File/layer: python/tests/test_conditions.py | pytest
# Testing: HARNESS, PythonTest.test_conditions: a condition file through
#          the package (ADR-0063)
#
# tests/fixtures/conditions/example.json from its path and from its text,
# field for field; a refused file's line, column and words; each
# condition resolved against obd2, the mock, no signals and a signal file
# (tests/fixtures/signals/extras.json), against havne-cli's own lines
# where the tree has it; a set left unclosed and collected, and one
# closed.

import gc
import json
import os
import subprocess

import pytest

import havne

from conftest import ROOT, SIGNALS

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
