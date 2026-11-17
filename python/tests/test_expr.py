# File/layer: python/tests/test_expr.py | pytest
# Testing: HARNESS, PythonTest.test_expr: an expression against a loaded
#          signal file (ADR-0062), through the package
#
# tests/fixtures/signals/chain.json (a chain of computed signals) and
# extras.json (format 1, extends obd2): an expression checked, what it
# reads and the format it needs; a refused one's character and words; the
# name an expression reads each signal by; an expression worked out from
# values, with why when there is no value; the values' own refusals; a
# plugin's variants each judged in their own file; and havne-cli's
# signals expr over the same file saying the same.

import collections
import os

import pytest

import havne

from conftest import PLUGINS, SIGNALS, cli, line_of

# obd2's own IDs (signal_sets.h, which the package doesn't carry): its
# first ten signals, two of them by name, and its computed Boost
FIRST, RPM, SPEED, LAST, BOOST = 7001, 7003, 7004, 7010, 7011


def chain():
    return havne.SignalFile(path=os.path.join(SIGNALS, "chain.json"))


def extras():
    return havne.SignalFile(path=os.path.join(SIGNALS, "extras.json"))


def test_a_checked_expression_says_what_it_reads_and_the_format_it_needs():
    f = chain()
    # margin reads hottest, which reads the two numbers: three deep
    assert f.check_expr("margin / 2 + oil") == havne.ExprCheck(
        2, 3, ["margin", "oil"], ["coolant", "oil"])
    assert f.check_expr("oil - coolant") == havne.ExprCheck(1, 1, ["oil", "coolant"],
                                                             ["oil", "coolant"])
    old = extras()
    assert old.format == 1
    assert old.check_expr("obd2.rpm / 1000") == havne.ExprCheck(1, 1, ["obd2.rpm"], ["obd2.rpm"])
    # A call is format 2's: the file would need that to hold it
    assert old.check_expr("max(obd2.rpm, road_speed)").format == 2
    # Bytes are taken as text is
    assert f.check_expr(b"oil * 2").inputs == ["oil"]


def test_a_refused_expression_says_where_and_why():
    f = chain()
    with pytest.raises(havne.ExprRefused) as r:
        f.check_expr("max(coolant, oli)")
    assert r.value.at == 14
    assert r.value.detail == "the expression reads 'oli', which no signal is"
    assert r.value.name == "INVALID_ARGUMENT"
    assert isinstance(r.value, havne.Error)
    with pytest.raises(havne.ExprRefused) as r:
        f.check_expr("oil +")
    assert r.value.at == 6, "the end, where a value should be"
    # In hottest's place it would read itself through margin; as a new
    # signal it reads margin as any other
    with pytest.raises(havne.ExprRefused) as r:
        f.check_expr("margin + 1", key="hottest")
    assert r.value.at == 1 and "hottest" in r.value.detail
    assert f.check_expr("margin + 1").depth == 3
    assert f.check_expr("margin + 1", key="a_new_signal").depth == 3
    # A key that is no key is the call's refusal, not the expression's
    with pytest.raises(havne.Error) as e:
        f.check_expr("oil", key="No Key")
    assert type(e.value) is havne.Error and "a-z" in e.value.detail


def test_each_signal_has_the_name_an_expression_reads_it_by():
    f = extras()
    assert f.expr_ref("Road Speed") == "road_speed"
    assert f.expr_ref(RPM) == "obd2.rpm"
    assert f.expr_ref("RPM") == "obd2.rpm"
    value = collections.namedtuple("Value", "id name")(SPEED, "x")
    assert f.expr_ref(value) == "obd2.speed"
    # Each of obd2's first ten compiles by its name and is read as it
    for i in range(FIRST, LAST + 1):
        name = f.expr_ref(i)
        assert name.startswith("obd2.") and f.check_expr(name).inputs == [name]
    with pytest.raises(LookupError):
        f.expr_ref("No Such Signal")
    with pytest.raises(LookupError):
        f.expr_ref(1)
    # obd2's computed Boost is the set's, and no expression names it
    with pytest.raises(havne.Error, match="has no name in an expression"):
        f.expr_ref(BOOST)
    # A file that extends nothing has its keys alone
    assert chain().expr_ref("Margin to 130") == "margin"


def test_an_expression_is_worked_out_from_values():
    f = chain()
    # hottest 110, margin 20
    assert f.evaluate("margin / 2 + oil", {"coolant": 95, "oil": (110, 1_000_000)}) == \
        havne.Evaluation(120.0, None, None, None)
    # A computed signal's key is an expression: the file's own signal
    assert f.evaluate("hot", {"coolant": 120, "oil": 90}).value == 1.0
    assert f.evaluate("hot", {"coolant": 100, "oil": 90}).value == 0.0
    # A name the expression doesn't read is passed by
    assert f.evaluate("coolant * 2", {"coolant": 2, "oil": 5}).value == 4.0
    assert f.evaluate("hot", {"coolant": 120}) == havne.Evaluation(None, "missing", "oil", None)
    assert f.evaluate("hot", {}) == havne.Evaluation(None, "missing", "coolant", None)
    stale = havne.c.ECUIO_VALUE_STALE_US
    assert f.evaluate("hot", {"coolant": (120, stale + 1), "oil": 90}) == \
        havne.Evaluation(None, "stale", "coolant", stale)
    assert f.evaluate("hot", {"coolant": (120, stale), "oil": 90}).value == 1.0, \
        "a value at its bound counts"
    assert f.evaluate("coolant / oil", {"coolant": 1, "oil": 0}) == \
        havne.Evaluation(None, "no_number", None, None)
    # In a signal's place, as check_expr judges it
    with pytest.raises(havne.ExprRefused) as r:
        f.evaluate("margin + 1", {"coolant": 1, "oil": 1}, key="hottest")
    assert r.value.at == 1


def test_a_value_goes_to_a_number_by_a_name_an_expression_reads():
    f = extras()
    assert f.evaluate("obd2.rpm / 1000", {"obd2.pid_0c": 2500}).value == 2.5, \
        "any spelling core takes"
    with pytest.raises(ValueError, match="'bost', which no signal is"):
        f.evaluate("obd2.rpm", {"bost": 1})
    with pytest.raises(ValueError, match="the signal is computed"):
        f.evaluate("obd2.rpm", {"krpm": 1})
    with pytest.raises(ValueError, match="by its name"):
        f.evaluate("obd2.rpm", {"obd2.rpm + 1": 1})
    with pytest.raises(ValueError, match="two values, as 'obd2.rpm' and 'obd2.pid_0c'"):
        f.evaluate("obd2.rpm", {"obd2.rpm": 1, "obd2.pid_0c": 2})
    for age in (-1, 1.5, True, 2**64):
        with pytest.raises(ValueError, match="whole microseconds"):
            f.evaluate("obd2.rpm", {"obd2.rpm": (1, age)})
    with pytest.raises(havne.ExprRefused):
        f.evaluate("obd2.rpm +", {"obd2.rpm": 1})


def test_a_plugins_variant_is_judged_in_its_own_file():
    p = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    assert p.variants[:2] == ["base", "my2019"]
    # Only the later variant has the clutch's slip
    assert p.check_expr("clutch_slip / 2", variant="my2019").inputs == ["clutch_slip"]
    with pytest.raises(havne.ExprRefused) as r:
        p.check_expr("clutch_slip / 2", variant="base")
    assert r.value.at == 1
    with pytest.raises(havne.ExprRefused):
        p.check_expr("clutch_slip / 2")  # the first variant when none is named
    assert p.expr_ref("Clutch Slip", variant="my2019") == "clutch_slip"
    with pytest.raises(LookupError):
        p.expr_ref("Clutch Slip", variant="base")
    assert p.expr_ref("Trans Fluid Temp") == "trans_temp"
    assert p.expr_ref(RPM, variant="base") == "obd2.rpm"
    assert p.evaluate("trans_temp - obd2.coolant", {"trans_temp": 90, "obd2.coolant": 85},
                      variant="my2019") == havne.Evaluation(5.0, None, None, None)
    assert p.evaluate("clutch_slip", {}, variant="my2019") == \
        havne.Evaluation(None, "missing", "clutch_slip", None)


def test_havne_cli_says_the_same_of_an_expression():
    f = chain()
    path = os.path.join(SIGNALS, "chain.json")
    for expr, values in (("margin / 2 + oil", {"coolant": 95, "oil": (110, 1_000_000)}),
                         ("hot", {"coolant": (120, 3_500_000), "oil": 90}),
                         ("coolant / oil", {"coolant": 1, "oil": 0}),
                         ("hot", {"oil": 90})):
        given = []
        for name, value in values.items():
            value, age_us = value if isinstance(value, tuple) else (value, 0)
            given += ["--value", f"{name}={value}@{age_us / 1e6}"]
        lines = cli("signals", "expr", "--signals", path, expr, *given)
        checked = f.check_expr(expr)
        line = line_of(lines, "expr")
        assert line["ok"] and line["at"] is None
        assert (line["format"], line["depth"]) == (checked.format, checked.depth)
        assert [i["ref"] for i in line["inputs"]] == checked.inputs
        assert [i["ref"] for i in line["leaves"]] == checked.leaves
        got = f.evaluate(expr, values)
        line = line_of(lines, "value")
        assert (line["value"], line["why"], line["leaf"], line["bound_us"]) == tuple(got)
    lines = cli("signals", "expr", "--signals", path, "max(coolant, oli)", status=3)
    with pytest.raises(havne.ExprRefused) as r:
        f.check_expr("max(coolant, oli)")
    line = line_of(lines, "expr")
    assert (line["ok"], line["at"], line["detail"]) == (False, r.value.at, r.value.detail)
    # The names the CLI lists are the ones the package gives
    listed = cli("signals", "expr", "--signals", path)
    assert [x["ref"] for x in listed if x["type"] == "input"] == \
        [f.expr_ref(x["id"]) for x in listed if x["type"] == "input"]
    assert len(listed) == 6, "start and the file's five signals"
