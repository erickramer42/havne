import importlib.resources
import json
import math
import os

import pytest

import havne

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRUTH = os.path.join(ROOT, "tests", "fixtures", "discovery", "sim-enhanced-truth.json")


def catalog():
    text = importlib.resources.files(havne).joinpath("data/wanted-list.json").read_text("utf-8")
    return json.loads(text)


def signals(doc):
    return [s for p in doc["packs"] for s in p["signals"]]


def span(enc):
    lo_raw = -(1 << (enc["bits"] - 1)) if enc["signed"] else 0
    hi_raw = (1 << (enc["bits"] - 1)) - 1 if enc["signed"] else (1 << enc["bits"]) - 1
    a = lo_raw * enc["scale"] + enc["offset"]
    b = hi_raw * enc["scale"] + enc["offset"]
    return min(a, b), max(a, b)


def test_the_catalog_takes_its_schema():
    jsonschema = pytest.importorskip("jsonschema")
    with open(os.path.join(ROOT, "schema", "wanted-list.schema.json"), encoding="utf-8") as f:
        schema = json.load(f)
    jsonschema.Draft202012Validator(schema).validate(catalog())
    bad = catalog()
    bad["packs"][0]["signals"][0]["class"] = "no-such-class"
    assert not jsonschema.Draft202012Validator(schema).is_valid(bad)
    bad = catalog()
    bad["packs"][0]["signals"][0]["units"] = "RPM"
    assert not jsonschema.Draft202012Validator(schema).is_valid(bad)


def test_the_three_packs_come_first():
    assert [p["key"] for p in catalog()["packs"]][:3] == ["tuning", "transmission", "health"]


def test_each_name_it_uses_is_declared_and_each_key_is_once():
    doc = catalog()
    keys = [s["key"] for s in signals(doc)]
    assert len(keys) == len(set(keys))
    for s in signals(doc):
        assert set(s["recognizers"]) <= set(doc["recognizers"]), s["key"]
        assert set(s["conditions"]) <= set(doc["conditions"]), s["key"]
        if "pair_of" in s:
            assert s["pair_of"] in keys and s["pair_of"] != s["key"], s["key"]


def test_a_signal_that_needs_an_oracle_names_one():
    needing = [s for s in signals(catalog()) if s.get("needs_oracle")]
    assert {"knock_retard", "intake_cam", "lambda", "rail_pressure", "boost_target", "fan_duty",
            "injected_quantity"} <= {s["key"] for s in needing}
    for s in needing:
        assert f"oracle.{s['key']}" in s["references"], s["key"]


def test_each_range_is_ordered_and_each_encoding_reaches_it():
    for s in signals(catalog()):
        lo, hi = s["range"]
        assert lo < hi, s["key"]
        for enc in s["encodings"]:
            a, b = span(enc)
            assert a < hi and b > lo, (s["key"], enc)


def test_a_load_pull_is_never_asked_for_on_a_road():
    words = catalog()["conditions"]["load_pull"]
    assert "dyno" in words and "closed course" in words


def same(a, b):
    return math.isclose(a, b, rel_tol=1e-9, abs_tol=1e-12)


def test_the_sims_fields_are_the_catalogs_and_use_its_encodings():
    by_key = {s["key"]: s for s in signals(catalog())}
    with open(TRUTH, encoding="utf-8") as f:
        truth = json.load(f)
    wanted = 0
    for did in truth["dids"]:
        for field in did["fields"]:
            if field["wanted"] is None:
                assert field["kind"] in ("counter", "noise", "static", "checksum")
                continue
            wanted += 1
            s = by_key[field["wanted"]]
            assert any(e["bits"] == field["bits"] and e["signed"] == field["signed"] and
                       same(e["scale"], field["scale"]) and same(e["offset"], field["offset"])
                       for e in s["encodings"]), field["key"]
            assert s["unit"] == field["unit"], field["key"]
    assert wanted > 20
