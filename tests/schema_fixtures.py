# File/layer: tests/schema_fixtures.py | test-only, Python 3.8+, jsonschema
# Testing: HARNESS (ctest SchemaFixturesTest.TheSchemasAndTheLoadersAgree):
#          schema/ through the jsonschema package, and havne-cli as a child
#
#   python tests/schema_fixtures.py <source dir> [<havne-cli>]
#
# schema/ against the files core's loaders read (ADR-0051, ADR-0052,
# ADR-0063, ADR-0066):
# - every committed signal file, condition file, plugin manifest (a
#   folder's or packed), a folder plugin's data files and value-fixture
#   files validate;
# - each file in tests/fixtures/schema/ (and tests/fixtures/signals/
#   refused.json) fails validation at the rule its table entry names, and
#   with havne-cli given, its check refuses the file in the words named;
# - edge cases, each one change to a file both read, get the same verdict
#   from the schema and, with havne-cli given, from its check.
# Exits 77 (ctest's skip) when jsonschema can't be imported, unless
# ECUIO_SCHEMA_REQUIRED=1 (CI's core-and-tests), which fails instead.

import base64
import copy
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile

SKIP = 77

# Refused files (relative to tests/fixtures): the schema, the keyword of
# the rule that refuses it, and words of havne-cli's refusal
REFUSED = {
    "schema/signal-unknown-key.json":
        ("signal-file", "additionalProperties", "unknown key 'units' in signal 'road_speed'"),
    "schema/signal-wrong-type.json":
        ("signal-file", "type", "decimals is a string, not a number"),
    "schema/signal-missing-key.json":
        ("signal-file", "required", "the file needs a \"source\""),
    "schema/signal-bad-kind.json":
        ("signal-file", "enum", "kind 'float' isn't number, text, hex, enum or computed"),
    "schema/plugin-unknown-key.json":
        ("plugin-manifest", "additionalProperties", "unknown key 'matches' in the manifest"),
    "schema/plugin-missing-key.json":
        ("plugin-manifest", "required", "the manifest needs \"tiers\""),
    "signals/refused.json":
        ("signal-file", "additionalProperties", "unknown key 'scael' in signal 'pedal'"),
    "schema/condition-unknown-key.json":
        ("condition-file", "additionalProperties", "unknown key 'wen' in condition 'revs'"),
    "schema/condition-wrong-type.json":
        ("condition-file", "type", "condition 'revs''s for is a string, not a number"),
    "schema/condition-missing-key.json":
        ("condition-file", "required", "condition 'revs' needs a \"when\""),
    "schema/condition-bad-word.json":
        ("condition-file", "enum", "link 'down' isn't not_ok, timeout"),
    "conditions/refused.json":
        ("condition-file", "additionalProperties",
         "unknown key 'abov' in condition 'pull''s when"),
}

DELETE = object()

SIGNAL_FILE = {
    "havne_signals": 1,
    "name": "Schema edge",
    "source": {"name": "Written by hand for Havne's tests"},
    "ecus": [{"id": "pcm", "rx": "7E8"}],
    "signals": [
        {"key": "rpm_raw", "name": "RPM", "read": {"service": "01", "number": "0C"},
         "field": {"byte": 0, "bits": 16}, "scale": 0.25, "unit": "r/min"},
        {"key": "gear", "name": "Gear", "kind": "enum",
         "read": {"service": "22", "number": "F411"}, "field": {"byte": 0},
         "values": {"0": "Park", "1": "First"}},
        {"key": "part", "name": "Part", "kind": "text",
         "read": {"service": "22", "number": "F187"}, "field": {"byte": 0}},
        {"key": "half_rpm", "name": "Half RPM", "kind": "computed",
         "expr": "rpm_raw / 2", "max_age": {"rpm_raw": 1}},
    ],
}

PACKED = {
    "havne_plugin": 1,
    "id": "edge",
    "name": "Schema edge plugin",
    "version": "1.0.0",
    "requires": {"core": "0.1.0"},
    "source": {"name": "Havne's tests"},
    "tiers": ["data"],
    "match": [{"wmi": ["ZZZ"], "years": [2015, 2030]}],
    "ecus": [{"id": "pcm", "rx": "7E8"}],
    "variants": [{"id": "base", "files": ["a.json"]}],
    "files": {
        "a.json": {
            "havne_signals": 1,
            "signals": [
                {"key": "oil_temp", "name": "Oil Temp", "read": {"service": "22", "number": "F40D"},
                 "field": {"byte": 0}, "offset": -40, "unit": "deg C"},
            ],
        },
    },
}

CONDITION_FILE = {
    "havne_conditions": 1,
    "name": "Schema edge",
    "checks": {"gap": 1, "rate_drop": 10, "settle": 2},
    "conditions": [
        {"key": "hot", "name": "Hot",
         "when": {"signal": ["obd2.coolant"], "unit": "deg C", "above": 110, "clear": 105,
                  "max_age": 10},
         "for": 2, "clear_for": 1,
         "alert": {"level": "danger", "sound": True, "banner": True},
         "record": {"before": 5, "after": 5}, "mark": True},
        {"key": "both", "name": "Both",
         "when": {"all": [{"link": "not_ok"}, {"check": "gap"},
                          {"signal": ["key:gear", "id:1001"], "equals": 1}]}},
    ],
}

def ecus(n):
    """n modules: the engine ECU, then made-up 11-bit pairs"""
    return [{"id": "pcm", "rx": "7E8"}] + [
        {"id": "m%d" % i, "rx": "%03X" % (0x640 + i), "tx": "%03X" % (0x600 + i)}
        for i in range(1, n)]


S0 = ["signals", 0]
S1 = ["signals", 1]
S3 = ["signals", 3]
F0 = ["files", "a.json"]

# (what, the base, the path changed, its value or DELETE, accepted)
CASES = [
    ("the base signal file", SIGNAL_FILE, [], None, True),
    ("a 29-bit standard rx with no tx", SIGNAL_FILE, ["ecus", 0, "rx"], "18DAF110", True),
    ("a standard rx with leading zeros", SIGNAL_FILE, ["ecus", 0, "rx"], "000007E8", True),
    ("a lower-case rx", SIGNAL_FILE, ["ecus", 0, "rx"], "7e8", True),
    ("an rx of 0", SIGNAL_FILE, ["ecus", 0, "rx"], "000", False),
    ("an rx past 1FFFFFFF", SIGNAL_FILE, ["ecus", 0, "rx"], "20000000", False),
    ("an rx of 9 digits", SIGNAL_FILE, ["ecus", 0, "rx"], "0000007E8", False),
    ("an rx that isn't hex", SIGNAL_FILE, ["ecus", 0, "rx"], "7G8", False),
    ("an rx the standard gives no tx", SIGNAL_FILE, ["ecus", 0, "rx"], "7B8", False),
    ("an ECU id with a dash", SIGNAL_FILE, ["ecus", 0, "id"], "p-cm", False),
    ("32 ECUs", SIGNAL_FILE, ["ecus"], ecus(32), True),
    ("33 ECUs", SIGNAL_FILE, ["ecus"], ecus(33), False),
    ("a plugin of 32 ECUs", PACKED, ["ecus"], ecus(32), True),
    ("a plugin of 33 ECUs", PACKED, ["ecus"], ecus(33), False),
    ("a file name with no letter or digit", SIGNAL_FILE, ["name"], "--", False),
    ("a file name of 64 bytes", SIGNAL_FILE, ["name"], "n" * 64, False),
    ("a gauge with both levels", SIGNAL_FILE, S0 + ["gauge"],
     {"min": 0, "max": 8000, "warn_low": 500, "warn_high": 6500}, True),
    ("a gauge on a computed signal", SIGNAL_FILE, S3 + ["gauge"], {"min": 0, "max": 4000}, True),
    ("a gauge with no max", SIGNAL_FILE, S0 + ["gauge"], {"min": 0}, False),
    ("a gauge with a key it doesn't take", SIGNAL_FILE, S0 + ["gauge"],
     {"min": 0, "max": 1, "high": 1}, False),
    ("a gauge whose min is text", SIGNAL_FILE, S0 + ["gauge"], {"min": "0", "max": 1}, False),
    ("a gauge on an enum", SIGNAL_FILE, S1 + ["gauge"], {"min": 0, "max": 1}, False),
    ("a gauge on text", SIGNAL_FILE, ["signals", 2, "gauge"], {"min": 0, "max": 1}, False),
    ("a key with a dash", SIGNAL_FILE, S0 + ["key"], "rpm-raw", False),
    ("a key of 32 bytes", SIGNAL_FILE, S0 + ["key"], "k" * 32, False),
    ("a name a formula starts", SIGNAL_FILE, S0 + ["name"], "=RPM", False),
    ("a unit a formula starts", SIGNAL_FILE, S0 + ["unit"], "-", False),
    ("an empty unit", SIGNAL_FILE, S0 + ["unit"], "", True),
    ("a unit of 16 bytes", SIGNAL_FILE, S0 + ["unit"], "u" * 16, False),
    ("decimals 3", SIGNAL_FILE, S0 + ["decimals"], 3, True),
    ("decimals 4", SIGNAL_FILE, S0 + ["decimals"], 4, False),
    ("plot 256", SIGNAL_FILE, S0 + ["plot"], 256, False),
    ("group Engine in capitals", SIGNAL_FILE, S0 + ["group"], "Engine", False),
    ("group oxygen sensors", SIGNAL_FILE, S0 + ["group"], "oxygen sensors", True),
    ("scale 0", SIGNAL_FILE, S0 + ["scale"], 0, False),
    ("12 bits big-endian", SIGNAL_FILE, S0 + ["field", "bits"], 12, False),
    ("a bit big-endian", SIGNAL_FILE, S0 + ["field", "bit"], 1, False),
    ("12 bits little-endian", SIGNAL_FILE, S0 + ["field"],
     {"byte": 0, "bits": 12, "bit": 2, "order": "le"}, True),
    ("33 bits", SIGNAL_FILE, S0 + ["field"], {"byte": 0, "bits": 33, "order": "le"}, False),
    ("a byte order of \"BE\"", SIGNAL_FILE, S0 + ["field", "order"], "BE", False),
    ("a PID's last byte", SIGNAL_FILE, S0 + ["field"], {"byte": 4093}, True),
    ("a DID's byte past its last", SIGNAL_FILE, S1 + ["field", "byte"], 4093, False),
    ("signed on an enum", SIGNAL_FILE, S1 + ["field", "signed"], True, False),
    ("service 02", SIGNAL_FILE, S0 + ["read", "service"], "02", False),
    ("a PID of 4 digits", SIGNAL_FILE, S0 + ["read", "number"], "0C0C", False),
    ("a DID of 2 digits", SIGNAL_FILE, S1 + ["read", "number"], "F4", False),
    ("an enum value with a leading zero", SIGNAL_FILE, S1 + ["values"], {"01": "One"}, False),
    ("an enum label of 28 bytes", SIGNAL_FILE, S1 + ["values"], {"0": "L" * 28}, False),
    ("an enum label a formula starts", SIGNAL_FILE, S1 + ["values"], {"0": "@park"}, False),
    ("an enum with no values", SIGNAL_FILE, S1 + ["values"], DELETE, False),
    ("a scale on an enum", SIGNAL_FILE, S1 + ["scale"], 2, False),
    ("a unit on a text signal", SIGNAL_FILE, ["signals", 2, "unit"], "x", False),
    ("an expression reading no signal", SIGNAL_FILE, S3 + ["expr"], "2 * 3", False),
    ("an expression with %", SIGNAL_FILE, S3 + ["expr"], "rpm_raw % 2", False),
    ("an expression of 257 characters", SIGNAL_FILE, S3 + ["expr"],
     "rpm_raw" + " " * 250, False),
    ("a max age of 0.05 s", SIGNAL_FILE, S3 + ["max_age", "rpm_raw"], 0.05, False),
    ("a max age of 60 s", SIGNAL_FILE, S3 + ["max_age", "rpm_raw"], 60, True),
    ("a field on a computed signal", SIGNAL_FILE, S3 + ["field"], {"byte": 0}, False),
    ("a source with no name", SIGNAL_FILE, ["source"], {"url": "https://example.org"}, False),
    ("an empty source url", SIGNAL_FILE, ["source", "url"], "", False),
    ("notes that aren't text", SIGNAL_FILE, ["notes"], 1, False),
    ("a tool's own key", SIGNAL_FILE, S0 + ["x-tool"], {"any": [1]}, True),
    ("extends something other than obd2", SIGNAL_FILE, ["extends"], "uds", False),
    ("format 3", SIGNAL_FILE, ["havne_signals"], 3, True),
    ("format 4", SIGNAL_FILE, ["havne_signals"], 4, False),
    ("needs in format 1", SIGNAL_FILE, ["needs"], [], False),
    ("an ECU's name", SIGNAL_FILE, ["ecus", 0, "name"], "Engine", True),
    ("an ECU's name of 63 bytes", SIGNAL_FILE, ["ecus", 0, "name"], "n" * 63, True),
    ("an ECU's name of 64 bytes", SIGNAL_FILE, ["ecus", 0, "name"], "n" * 64, False),
    ("an empty ECU name", SIGNAL_FILE, ["ecus", 0, "name"], "", False),
    ("an ECU's name that starts a formula", SIGNAL_FILE, ["ecus", 0, "name"], "=1", False),
    ("a $schema key", SIGNAL_FILE, ["$schema"], "signal-file.schema.json", False),
    ("the base packed plugin", PACKED, [], None, True),
    ("an id with a dash first", PACKED, ["id"], "-edge", False),
    ("an id of 32 bytes", PACKED, ["id"], "e" * 32, False),
    ("version 65535.0.0", PACKED, ["version"], "65535.0.0", True),
    ("version 65536.0.0", PACKED, ["version"], "65536.0.0", False),
    ("a version with a leading zero", PACKED, ["version"], "01.0.0", False),
    ("a version of two parts", PACKED, ["version"], "1.0", False),
    ("a version of 16 characters", PACKED, ["version"], "65535.65535.6553", False),
    ("a required core with a suffix", PACKED, ["requires", "core"], "0.1.0-rc1", False),
    ("a lineage of 32 hex digits", PACKED, ["lineage"], "0123456789abcdef" * 2, True),
    ("a lineage of 31 digits", PACKED, ["lineage"], "a" * 31, False),
    ("a lineage of 33 digits", PACKED, ["lineage"], "a" * 33, False),
    ("a lineage in upper case", PACKED, ["lineage"], "A" * 32, False),
    ("a lineage that is a number", PACKED, ["lineage"], 7, False),
    ("a tier named twice", PACKED, ["tiers"], ["data", "data"], False),
    ("a tier this core doesn't run", PACKED, ["tiers"], ["data", "write"], False),
    ("the conversation tier in format 1", PACKED, ["tiers"], ["data", "conversation"], False),
    ("a conversation in format 1", PACKED, ["conversation"],
     {"code": {"file": "a.wasm", "sha256": "0" * 64}, "ecus": ["pcm"],
      "services": [{"sid": "22"}]}, False),
    ("a binary file in format 1", PACKED, ["files", "guest.wasm"],
     {"kind": "wasm", "base64": ""}, False),
    ("format 2 with the data tier alone", PACKED, ["havne_plugin"], 2, True),
    ("format 3 of a plugin", PACKED, ["havne_plugin"], 3, True),
    ("format 4 of a plugin", PACKED, ["havne_plugin"], 4, False),
    ("a manufacturer code with an I", PACKED, ["match", 0, "wmi"], ["ZZI"], False),
    ("a manufacturer code of 6", PACKED, ["match", 0, "wmi"], ["ZZ9ABC"], True),
    ("a manufacturer code of 4", PACKED, ["match", 0, "wmi"], ["ZZZZ"], False),
    ("a model year of 1979", PACKED, ["match", 0, "years"], [1979, 2000], False),
    ("model years 2039 to 2039", PACKED, ["match", 0, "years"], [2039, 2039], True),
    ("three model years", PACKED, ["match", 0, "years"], [2015, 2020, 2030], False),
    ("a variant id of 25 bytes", PACKED, ["variants", 0, "id"], "v" * 25, False),
    ("a variant with no files", PACKED, ["variants", 0, "files"], [], False),
    ("fixtures in a packed plugin", PACKED, ["fixtures"], ["f.json"], False),
    ("a data file with its own ECUs", PACKED, F0 + ["ecus"], [{"id": "pcm", "rx": "7E8"}], False),
    ("a data file with a name", PACKED, F0 + ["name"], "Oil", True),
    ("a data file with a source", PACKED, F0 + ["source"], {"name": "Elsewhere"}, True),
]

C0 = ["conditions", 0]
W0 = C0 + ["when"]
T2 = ["conditions", 1, "when", "all", 2]

CONDITION_CASES = [
    ("the base condition file", CONDITION_FILE, [], None, True),
    ("no conditions", CONDITION_FILE, ["conditions"], [], True),
    ("65 conditions", CONDITION_FILE, ["conditions"],
     [{"key": "c%d" % n, "name": "C%d" % n, "when": {"link": "timeout"}} for n in range(65)],
     False),
    ("no conditions key", CONDITION_FILE, ["conditions"], DELETE, False),
    ("no file name", CONDITION_FILE, ["name"], DELETE, True),
    ("a file name of 64 bytes", CONDITION_FILE, ["name"], "n" * 64, False),
    ("an empty file name", CONDITION_FILE, ["name"], "", False),
    ("format 2", CONDITION_FILE, ["havne_conditions"], 2, False),
    ("a $schema key", CONDITION_FILE, ["$schema"], "condition-file.schema.json", False),
    ("a tool's own key", CONDITION_FILE, C0 + ["x-tool"], {"any": [1]}, True),
    ("notes that aren't text", CONDITION_FILE, W0 + ["notes"], 1, False),
    ("a gap of 0", CONDITION_FILE, ["checks", "gap"], 0, False),
    ("a gap of 3600 s", CONDITION_FILE, ["checks", "gap"], 3600, True),
    ("a gap of 3601 s", CONDITION_FILE, ["checks", "gap"], 3601, False),
    ("a rate drop of 1", CONDITION_FILE, ["checks", "rate_drop"], 1, False),
    ("a rate drop of 1.5", CONDITION_FILE, ["checks", "rate_drop"], 1.5, True),
    ("a rate drop of 1001", CONDITION_FILE, ["checks", "rate_drop"], 1001, False),
    ("a settle below 0", CONDITION_FILE, ["checks", "settle"], -1, False),
    ("a check it doesn't take", CONDITION_FILE, ["checks", "late"], 1, False),
    ("a key with a capital", CONDITION_FILE, C0 + ["key"], "Hot", False),
    ("a key of 32 bytes", CONDITION_FILE, C0 + ["key"], "k" * 32, False),
    ("a name of 40 bytes", CONDITION_FILE, C0 + ["name"], "n" * 40, True),
    ("a name of 41 bytes", CONDITION_FILE, C0 + ["name"], "n" * 41, False),
    ("a name a formula starts", CONDITION_FILE, C0 + ["name"], "=Hot", False),
    ("no name", CONDITION_FILE, C0 + ["name"], DELETE, False),
    ("enabled false", CONDITION_FILE, C0 + ["enabled"], False, True),
    ("enabled as a number", CONDITION_FILE, C0 + ["enabled"], 0, False),
    ("a hold of 600 s", CONDITION_FILE, C0 + ["for"], 600, True),
    ("a hold of 601 s", CONDITION_FILE, C0 + ["for"], 601, False),
    ("a clear hold below 0", CONDITION_FILE, C0 + ["clear_for"], -0.5, False),
    ("an alert with no level", CONDITION_FILE, C0 + ["alert", "level"], DELETE, False),
    ("an alert level of high", CONDITION_FILE, C0 + ["alert", "level"], "high", False),
    ("an alert with a level alone", CONDITION_FILE, C0 + ["alert"], {"level": "warning"}, True),
    ("a sound that isn't true or false", CONDITION_FILE, C0 + ["alert", "sound"], "on", False),
    ("an empty record", CONDITION_FILE, C0 + ["record"], {}, True),
    ("60 s before", CONDITION_FILE, C0 + ["record", "before"], 60, True),
    ("61 s before", CONDITION_FILE, C0 + ["record", "before"], 61, False),
    ("601 s after", CONDITION_FILE, C0 + ["record", "after"], 601, False),
    ("mark false", CONDITION_FILE, C0 + ["mark"], False, True),
    ("mark as text", CONDITION_FILE, C0 + ["mark"], "yes", False),
    ("no when", CONDITION_FILE, C0 + ["when"], DELETE, False),
    ("an empty when", CONDITION_FILE, W0, {}, False),
    ("all and any together", CONDITION_FILE, W0,
     {"all": [{"link": "timeout"}], "any": [{"link": "timeout"}]}, False),
    ("an any of one term", CONDITION_FILE, W0, {"any": [{"check": "overflow"}]}, True),
    ("an all of no terms", CONDITION_FILE, W0, {"all": []}, False),
    ("an all of five terms", CONDITION_FILE, W0,
     {"all": [{"link": "timeout"}] * 5}, False),
    ("an any inside an all", CONDITION_FILE, W0,
     {"all": [{"any": [{"link": "timeout"}]}]}, False),
    ("a term of two kinds", CONDITION_FILE, W0, {"link": "timeout", "check": "gap"}, False),
    ("a link state it doesn't know", CONDITION_FILE, W0, {"link": "down"}, False),
    ("a check it doesn't know", CONDITION_FILE, W0, {"check": "asleep"}, False),
    ("a unit on a link term", CONDITION_FILE, W0, {"link": "timeout", "unit": "V"}, False),
    ("a signal that isn't a list", CONDITION_FILE, W0 + ["signal"], "obd2.coolant", False),
    ("no references", CONDITION_FILE, W0 + ["signal"], [], False),
    ("nine references", CONDITION_FILE, W0 + ["signal"], ["id:%d" % n for n in range(9)], False),
    ("eight references", CONDITION_FILE, W0 + ["signal"], ["id:%d" % n for n in range(8)], True),
    ("a reference by name", CONDITION_FILE, W0 + ["signal"], ["rpm"], False),
    ("an id with a leading zero", CONDITION_FILE, W0 + ["signal"], ["id:07"], False),
    ("an id of six digits", CONDITION_FILE, W0 + ["signal"], ["id:100000"], False),
    ("an id of 65535", CONDITION_FILE, W0 + ["signal"], ["id:65535"], True),
    ("an id of 65536", CONDITION_FILE, W0 + ["signal"], ["id:65536"], False),
    ("an id of 70000", CONDITION_FILE, W0 + ["signal"], ["id:70000"], False),
    ("a key with a capital in it", CONDITION_FILE, W0 + ["signal"], ["key:Boost"], False),
    ("a reference given twice", CONDITION_FILE, W0 + ["signal"], ["id:1", "id:1"], False),
    ("above and below together", CONDITION_FILE, W0 + ["below"], 90, False),
    ("no comparison", CONDITION_FILE, W0 + ["above"], DELETE, False),
    ("above with no unit", CONDITION_FILE, W0 + ["unit"], DELETE, False),
    ("above with an empty unit", CONDITION_FILE, W0 + ["unit"], "", True),
    ("a unit of 16 bytes", CONDITION_FILE, W0 + ["unit"], "u" * 16, False),
    ("a level that is text", CONDITION_FILE, W0 + ["above"], "110", False),
    ("equals with no unit", CONDITION_FILE, T2 + ["equals"], 0, True),
    ("differs with a unit", CONDITION_FILE, T2,
     {"signal": ["key:gear"], "differs": 1, "unit": "V"}, True),
    ("a clear with equals", CONDITION_FILE, T2 + ["clear"], 0, False),
    ("a max age of 0.05 s", CONDITION_FILE, W0 + ["max_age"], 0.05, False),
    ("a max age of 60 s", CONDITION_FILE, W0 + ["max_age"], 60, True),
    ("a hold inside a term", CONDITION_FILE, W0 + ["for"], 1, False),
]

# Paths in the package, each as a packed file's name and its variant's
PATHS = [
    ("dir/a.json", True),
    ("./a.json", False),
    ("dir/../a.json", False),
    ("dir//a.json", False),
    ("/a.json", False),
    ("dir/", False),
    ("a\\b.json", False),
    ("a:b.json", False),
    ("a b.json", False),
    ("..a.json", True),
    ("p" * 127, True),
    ("p" * 128, False),
]


def changed(base, path, value):
    doc = copy.deepcopy(base)
    if not path:
        return doc
    node = doc
    for part in path[:-1]:
        node = node[part]
    if value is DELETE:
        del node[path[-1]]
    else:
        node[path[-1]] = value
    return doc


def keywords(errors):
    """Every failing rule's keyword, the context's (oneOf, anyOf) too"""
    found = set()
    for e in errors:
        found.add(e.validator)
        found |= keywords(e.context)
    return found


# Format 2 files (ADR-0062), each one change or two to the base: a call, a
# comparison and a chain take the schema's widened characters. The loader
# alone checks which format a form needs, so no case here is a format 1 file
# using one
F2 = changed(SIGNAL_FILE, ["havne_signals"], 2)
F2_CHAIN = copy.deepcopy(F2)
F2_CHAIN["signals"].append({"key": "quarter_rpm", "name": "Quarter RPM", "kind": "computed",
                            "expr": "half_rpm / 2"})
FORMAT2 = [
    ("format 2", SIGNAL_FILE, None, F2, True),
    ("a call in format 2", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "max(rpm_raw, 100)"), True),
    ("a comparison in format 2", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "if(rpm_raw >= 1000, rpm_raw, 0)"), True),
    ("a truth joined in format 2", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "if(all(rpm_raw > 1, rpm_raw != 5), 1, 0)"), True),
    ("a computed signal reading another in format 2", SIGNAL_FILE, None, F2_CHAIN, True),
    ("a character no format has", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "if(rpm_raw > 1, 1; 0)"), False),
    ("a question mark in an expression", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "rpm_raw ? 1 : 0"), False),
    ("a format 2 expression of 257 characters", SIGNAL_FILE, None,
     changed(F2, S3 + ["expr"], "max(rpm_raw, 1)" + " " * 250), False),
]

# Format 3 (ADR-0065 section 9): a file that needs another's signal
F3_NEEDS = changed(SIGNAL_FILE, ["havne_signals"], 3)
F3_NEEDS["needs"] = ["oil_t"]
F3_NEEDS["signals"].append({"key": "oil_margin", "name": "Oil margin", "kind": "computed",
                            "expr": "130 - oil_t"})
FORMAT3 = [
    ("a format 3 file with a need an expression reads", SIGNAL_FILE, None, F3_NEEDS, True),
    ("a need that is no key", SIGNAL_FILE, None,
     changed(F3_NEEDS, ["needs"], ["Oil-T"]), False),
    ("a need given twice", SIGNAL_FILE, None,
     changed(F3_NEEDS, ["needs"], ["oil_t", "oil_t"]), False),
    ("needs that is no list", SIGNAL_FILE, None,
     changed(F3_NEEDS, ["needs"], "oil_t"), False),
    ("needs in format 2", SIGNAL_FILE, None,
     changed(F3_NEEDS, ["havne_signals"], 2), False),
]

# A merged set's written form (ADR-0065 section 9): "parts", each
# signal's "part" and the keys it "replaced"
MERGED = changed(SIGNAL_FILE, ["havne_signals"], 3)
MERGED["source"] = {"name": "A merge of 2 parts"}
MERGED["parts"] = [
    {"profile": "plugin.edge.base", "name": "Schema edge plugin", "format": 2,
     "plugin": {"id": "edge", "version": "1.0.0", "variant": "base"},
     "source": {"name": "Havne's tests", "license": "CC-BY-SA-4.0"}},
    {"profile": "user.schema-edge", "name": "Schema edge", "format": 3,
     "source": {"name": "Written by hand for Havne's tests"}},
]
for i, s in enumerate(MERGED["signals"]):
    s["part"] = 0 if i < 2 else 1
MERGED["signals"][0]["replaced"] = ["old_rpm"]
MERGED["ecus"][0]["part"] = 0
P0 = ["parts", 0]
F3 = changed(SIGNAL_FILE, ["havne_signals"], 3)
MERGED_SET = [
    ("the base merged set", SIGNAL_FILE, None, MERGED, True),
    ("parts in format 2", SIGNAL_FILE, None, changed(MERGED, ["havne_signals"], 2), False),
    ("parts that list none", SIGNAL_FILE, None, changed(MERGED, ["parts"], []), False),
    ("17 parts", SIGNAL_FILE, None,
     changed(MERGED, ["parts"],
             [{"profile": "user.p%d" % i, "name": "P", "format": 3, "source": {"name": "s"}}
              for i in range(17)]), False),
    ("needs in a merged set", SIGNAL_FILE, None, changed(MERGED, ["needs"], ["oil_t"]), False),
    ("a part with no profile", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["profile"], DELETE), False),
    ("a part's profile in capitals", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["profile"], "Plugin.Edge"), False),
    ("a merged set as a part", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["profile"], "merged.0123456789abcdef"), False),
    ("a part with no name", SIGNAL_FILE, None, changed(MERGED, P0 + ["name"], DELETE), False),
    ("a part's name of 64 bytes", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["name"], "n" * 64), False),
    ("a part of format 4", SIGNAL_FILE, None, changed(MERGED, P0 + ["format"], 4), False),
    ("a part with no source", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["source"], DELETE), False),
    ("a part with a key it doesn't take", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["owner"], "me"), False),
    ("a part with no plugin", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["plugin"], DELETE), True),
    ("a part's plugin with no variant", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["plugin", "variant"], DELETE), False),
    ("a part's plugin version of two parts", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["plugin", "version"], "1.0"), False),
    ("a part's plugin id with a dash first", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["plugin", "id"], "-edge"), False),
    ("a part's plugin with a key it doesn't take", SIGNAL_FILE, None,
     changed(MERGED, P0 + ["plugin", "hash"], "00"), False),
    ("a merged set's source with a url", SIGNAL_FILE, None,
     changed(MERGED, ["source", "url"], "https://example.org"), False),
    ("a merged set's source with a license", SIGNAL_FILE, None,
     changed(MERGED, ["source", "license"], "CC-BY-SA-4.0"), False),
    ("a merged set's signal with no part", SIGNAL_FILE, None,
     changed(MERGED, S0 + ["part"], DELETE), False),
    ("a signal's part of 16", SIGNAL_FILE, None, changed(MERGED, S0 + ["part"], 16), False),
    ("a signal's part that is text", SIGNAL_FILE, None,
     changed(MERGED, S0 + ["part"], "0"), False),
    ("a replaced key that is no key", SIGNAL_FILE, None,
     changed(MERGED, S0 + ["replaced"], ["Old-RPM"]), False),
    ("a replaced key given twice", SIGNAL_FILE, None,
     changed(MERGED, S0 + ["replaced"], ["old_rpm", "old_rpm"]), False),
    ("replaced that is no list", SIGNAL_FILE, None,
     changed(MERGED, S0 + ["replaced"], "old_rpm"), False),
    ("a replaced key on a computed signal", SIGNAL_FILE, None,
     changed(MERGED, S3 + ["replaced"], ["older"]), True),
    ("an ECU's part of 16", SIGNAL_FILE, None,
     changed(MERGED, ["ecus", 0, "part"], 16), False),
    ("a signal's part in a file with no parts", SIGNAL_FILE, None,
     changed(F3, S0 + ["part"], 0), False),
    ("a replaced key in a file with no parts", SIGNAL_FILE, None,
     changed(F3, S0 + ["replaced"], ["old_rpm"]), False),
    ("an ECU's part in a file with no parts", SIGNAL_FILE, None,
     changed(F3, ["ecus", 0, "part"], 0), False),
    ("a data file's signal with a part", PACKED, F0 + ["signals", 0, "part"], 0, False),
    ("a data file's signal with a replaced key", PACKED, F0 + ["signals", 0, "replaced"],
     ["old"], False),
    ("a data file with parts", PACKED, F0 + ["parts"], MERGED["parts"], False),
    ("a manifest's ECU with a part", PACKED, ["ecus", 0, "part"], 0, False),
]

# A format 2 plugin with the conversation tier (ADR-0066). The module is
# the least one the guest ABI takes: its six exports, each returning a
# constant, and one page of memory (fuzz/seeds_wasm.py's wasm_module() wrote it)
MODULE_B64 = ("AGFzbQEAAAABDgNgAAF/YAJ/fwF/YAAAAwcGAAABAAECBQQBAQEBB0IGCWVjdWlvX2FiaQAADGVj"
              "dWlvX2J1ZmZlcgABBWJlZ2luAAIMbmV4dF9yZXF1ZXN0AAMGYWNjZXB0AAQDZW5kAAUKHgYEAEEB"
              "CwUAQYAICwQAQQALBABBAAsEAEEACwIACw==")
PACKED2 = changed(PACKED, ["havne_plugin"], 2)
PACKED2["tiers"] = ["data", "conversation"]
PACKED2["files"]["guest.wasm"] = {"kind": "wasm", "base64": MODULE_B64}
PACKED2["conversation"] = {
    "code": {"file": "guest.wasm",
             "sha256": hashlib.sha256(base64.b64decode(MODULE_B64)).hexdigest()},
    "budget": 100000,
    "ecus": ["pcm"],
    "services": [{"sid": "10", "sub": ["03"]}, {"sid": "22"},
                 {"sid": "27", "sub": ["01", "02"]}],
    "security": [{"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4}],
}
CV = ["conversation"]
SV = CV + ["services"]
S27 = SV + [2, "sub"]
L0 = CV + ["security", 0]
B0 = ["files", "guest.wasm"]
CONVERSATION = [
    ("the base conversation plugin", PACKED2, [], None, True),
    ("the conversation tier alone", PACKED2, ["tiers"], ["conversation"], False),
    ("the conversation tier with no conversation", PACKED2, CV, DELETE, False),
    ("a conversation with no such tier", PACKED2, ["tiers"], ["data"], False),
    ("a conversation key it doesn't take", PACKED2, CV + ["writes"], [], False),
    ("no budget", PACKED2, CV + ["budget"], DELETE, True),
    ("a budget of 0", PACKED2, CV + ["budget"], 0, False),
    ("a budget of 1", PACKED2, CV + ["budget"], 1, True),
    ("a budget of 1000000", PACKED2, CV + ["budget"], 1000000, True),
    ("a budget of 1000001", PACKED2, CV + ["budget"], 1000001, False),
    ("no conversation ECUs", PACKED2, CV + ["ecus"], [], False),
    ("no ecus key in the conversation", PACKED2, CV + ["ecus"], DELETE, False),
    ("a conversation ECU listed twice", PACKED2, CV + ["ecus"], ["pcm", "pcm"], False),
    ("no services", PACKED2, SV, [], False),
    ("no services key", PACKED2, SV, DELETE, False),
    ("17 services", PACKED2, SV, [{"sid": "22"}] * 17, False),
    ("a service that writes", PACKED2, SV + [1, "sid"], "2E", False),
    ("a routine", PACKED2, SV + [1, "sid"], "31", False),
    ("a service in lower case", PACKED2, SV + [1, "sid"], "2a", False),
    ("the periodic read", PACKED2, SV + [1, "sid"], "2A", True),
    ("the trouble-code read", PACKED2, SV + [1, "sid"], "19", True),
    ("a read with sub-functions", PACKED2, SV + [1, "sub"], ["01"], False),
    ("a session with no sub-function", PACKED2, SV + [0, "sub"], DELETE, False),
    ("the programming session", PACKED2, SV + [0, "sub"], ["02"], False),
    ("the default session", PACKED2, SV + [0, "sub"], ["01"], False),
    ("no sub-functions listed", PACKED2, SV + [0, "sub"], [], False),
    ("tester present", PACKED2, SV + [1], {"sid": "3E", "sub": ["00"]}, True),
    ("tester present with no reply", PACKED2, SV + [1], {"sid": "3E", "sub": ["80"]}, False),
    ("defining by identifier and clearing", PACKED2, SV + [1],
     {"sid": "2C", "sub": ["01", "03"]}, True),
    ("defining by memory address", PACKED2, SV + [1], {"sid": "2C", "sub": ["02"]}, False),
    ("a service key it doesn't take", PACKED2, SV + [1, "did"], "F190", False),
    ("security access 00", PACKED2, S27, ["01", "02", "00"], False),
    ("security access 5F", PACKED2, S27, ["01", "02", "5F"], False),
    ("security access 60", PACKED2, S27, ["01", "02", "60"], False),
    ("security access 7E", PACKED2, S27, ["01", "02", "7E"], True),
    ("security access 7F", PACKED2, S27, ["01", "02", "7F"], False),
    ("security access with bit 7", PACKED2, S27, ["01", "02", "81"], False),
    ("a sub-function in lower case", PACKED2, S27, ["01", "02", "0a"], False),
    ("a sub-function of one digit", PACKED2, S27, ["01", "02", "3"], False),
    ("a sub-function listed twice", PACKED2, S27, ["01", "02", "02"], False),
    ("14 sub-functions", PACKED2, S27, ["%02X" % n for n in range(1, 15)], True),
    ("15 sub-functions", PACKED2, S27, ["%02X" % n for n in range(1, 16)], False),
    ("no security levels", PACKED2, CV + ["security"], DELETE, True),
    ("an even security level", PACKED2, L0 + ["level"], "02", False),
    ("security level 5F", PACKED2, L0 + ["level"], "5F", False),
    ("security level 7F", PACKED2, L0 + ["level"], "7F", False),
    ("a security level that is a number", PACKED2, L0 + ["level"], 1, False),
    ("a security level with no ECU", PACKED2, L0 + ["ecu"], DELETE, False),
    ("a security level with no seed length", PACKED2, L0 + ["seed_bytes"], DELETE, False),
    ("a seed of 0 bytes", PACKED2, L0 + ["seed_bytes"], 0, False),
    ("a seed of 255 bytes", PACKED2, L0 + ["seed_bytes"], 255, True),
    ("a seed of 256 bytes", PACKED2, L0 + ["seed_bytes"], 256, False),
    ("a key of 0 bytes", PACKED2, L0 + ["key_bytes"], 0, False),
    ("a key of 58 bytes", PACKED2, L0 + ["key_bytes"], 58, True),
    ("a key of 59 bytes", PACKED2, L0 + ["key_bytes"], 59, False),
    ("a delay of 999 ms", PACKED2, L0 + ["delay_ms"], 999, False),
    ("a delay of 1000 ms", PACKED2, L0 + ["delay_ms"], 1000, True),
    ("a delay of an hour", PACKED2, L0 + ["delay_ms"], 3600000, True),
    ("a delay past an hour", PACKED2, L0 + ["delay_ms"], 3600001, False),
    ("a key window of 49 ms", PACKED2, L0 + ["key_window_ms"], 49, False),
    ("a key window of 50 ms", PACKED2, L0 + ["key_window_ms"], 50, True),
    ("a key window of 4000 ms", PACKED2, L0 + ["key_window_ms"], 4000, True),
    ("a key window of 4001 ms", PACKED2, L0 + ["key_window_ms"], 4001, False),
    ("a key window with a fraction", PACKED2, L0 + ["key_window_ms"], 100.5, False),
    ("a security level key it doesn't take", PACKED2, L0 + ["algorithm"], "xor", False),
    ("code with no file", PACKED2, CV + ["code", "file"], DELETE, False),
    ("code with no hash", PACKED2, CV + ["code", "sha256"], DELETE, False),
    ("a hash in upper case", PACKED2, CV + ["code", "sha256"], "A" * 64, False),
    ("a hash of 63 digits", PACKED2, CV + ["code", "sha256"], "a" * 63, False),
    ("code outside the package", PACKED2, CV + ["code", "file"], "../guest.wasm", False),
    ("a binary file of another kind", PACKED2, B0 + ["kind"], "elf", False),
    ("a binary file with no bytes", PACKED2, B0 + ["base64"], DELETE, False),
    ("a binary file key it doesn't take", PACKED2, B0 + ["sha256"], "0" * 64, False),
    ("base64 with a line break", PACKED2, B0 + ["base64"],
     MODULE_B64[:76] + "\n" + MODULE_B64[76:], False),
    ("base64 in the URL alphabet", PACKED2, B0 + ["base64"],
     MODULE_B64.replace("/", "_"), False),
    ("base64 with no padding", PACKED2, B0 + ["base64"], MODULE_B64.rstrip("="), False),
    ("base64 with padding bits set", PACKED2, B0 + ["base64"],
     MODULE_B64[:-3] + "x==", False),
]

# A Dashboard page a packed plugin carries (ADR-0052's appendix on pages):
# core's envelope, which the schema's pageFile states
PAGE = {
    "format": 2, "form": "phone", "files": [],
    "pages": [{"name": "Engine", "cells": [
        {"kind": "digital", "w": 3, "h": 9, "label": "Oil temp", "refs": ["key:oil_temp"],
         "unit": "deg C", "options": {"from": 0, "to": 160}}]}],
}
PACKED_PAGE = changed(PACKED, ["pages"], ["p/phone.json"])
PACKED_PAGE["files"]["p/phone.json"] = PAGE
PG = ["files", "p/phone.json"]
SEVENTEEN = changed(PACKED, ["pages"], [f"p/{n}.json" for n in range(17)])
SEVENTEEN["files"].update({f"p/{n}.json": changed(PAGE, ["pages", 0, "name"], f"Engine {n}")
                           for n in range(17)})
PAGES = [
    ("a page", PACKED_PAGE, [], None, True),
    ("a page for the desktop", PACKED_PAGE, PG + ["form"], "desktop", True),
    ("a page with no form", PACKED_PAGE, PG + ["form"], DELETE, False),
    ("a page for a tablet", PACKED_PAGE, PG + ["form"], "tablet", False),
    ("a page of format 1", PACKED_PAGE, PG + ["format"], 1, False),
    ("a page with no format", PACKED_PAGE, PG + ["format"], DELETE, False),
    ("a page file with no page", PACKED_PAGE, PG + ["pages"], [], False),
    ("a page file with two pages", PACKED_PAGE, PG + ["pages"], PAGE["pages"] * 2, False),
    ("a page naming a signal file", PACKED_PAGE, PG + ["files"],
     [{"name": "x", "sha256": "0" * 64}], False),
    ("a page with no files key", PACKED_PAGE, PG + ["files"], DELETE, True),
    ("a page key the envelope doesn't list", PACKED_PAGE, PG + ["theme"], "dark", True),
    ("no pages listed", PACKED_PAGE, ["pages"], [], False),
    ("a page listed twice", PACKED_PAGE, ["pages"], ["p/phone.json", "p/phone.json"], False),
    ("16 pages", changed(SEVENTEEN, ["pages"], SEVENTEEN["pages"][:16]), ["files", "p/16.json"],
     DELETE, True),
    ("17 pages", SEVENTEEN, [], None, False),
]

# Format 3's publisher (ADR-0066's appendix of 2026-11-23): required with
# the conversation tier, format 3's alone, each text at its bound. The
# loader's plain-text rule (no control or invisible character) is its own.
PUB = {"name": "Made-up Tools", "contact": "https://example.com/help",
       "license": "MIT", "terms": "Use at your own risk.\nNo warranty."}
PACKED3 = changed(PACKED2, ["havne_plugin"], 3)
PACKED3["publisher"] = PUB
DATA3 = changed(PACKED, ["havne_plugin"], 3)
PB = ["publisher"]
PUBLISHER = [
    ("a format 3 conversation plugin", PACKED3, [], None, True),
    ("format 3's conversation tier with no publisher", PACKED3, PB, DELETE, False),
    ("a publisher in format 2", PACKED2, PB, PUB, False),
    ("a publisher in format 1", PACKED, PB, PUB, False),
    ("a format 3 data plugin with no publisher", DATA3, [], None, True),
    ("a format 3 data plugin with a publisher", DATA3, PB, PUB, True),
    ("format 4", PACKED3, ["havne_plugin"], 4, False),
    ("a signature", PACKED3, ["signature"], {"alg": "ed25519"}, False),
    ("a publisher key it doesn't take", PACKED3, PB + ["url"], "u", False),
    ("a publisher that isn't an object", PACKED3, PB, "Made-up Tools", False),
] + [
    (f"no publisher {key}", PACKED3, PB + [key], DELETE, False)
    for key in ("name", "contact", "license", "terms")
] + [
    (f"a publisher {key} of {n} bytes", PACKED3, PB + [key], "x" * n, 0 < n <= most)
    for key, most in (("name", 127), ("contact", 255), ("license", 63), ("terms", 65536))
    for n in (0, 1, most, most + 1)
]

# Format 3's operation tier (ADR-0066 section 4): plans of read and
# upload steps against one of the conversation's ECUs. Every address and
# identifier is made up. The rules only the loader holds (an ECU and a
# level the conversation declares, ids given once, ranges apart, what
# fits a step's address and size bytes) are PluginOperationTest's.
READ = {"step": "read", "name": "cal", "session": "extended", "address": "00010000",
        "bytes": 4096, "address_bytes": 4, "size_bytes": 2, "chunk": 256}
UPLOAD = {"step": "upload", "name": "boot", "session": "extended", "address": "00020000",
          "bytes": 4096, "address_bytes": 4, "size_bytes": 4, "dfi": "00"}
PACKED4 = changed(PACKED3, ["tiers"], ["data", "conversation", "operation"])
PACKED4["operations"] = [{
    "id": "copy-cal", "kind": "dump", "title": "Calibration copy", "ecu": "pcm",
    "level": "01", "ident": [{"did": "D220", "role": "software"}],
    "steps": [READ, UPLOAD],
}]


def many(n, make):
    return [make(i) for i in range(n)]


def op_n(i):
    return changed(PACKED4["operations"][0], ["id"], f"copy-{i}")


def step_n(i):
    return dict(READ, name=f"s{i}", address=f"{0x00010000 + 16 * i:08X}", bytes=16, chunk=16)


def ident_n(i):
    return {"did": f"D2{0x20 + i:02X}", "role": "software"}


OPS = ["operations"]
OP = OPS + [0]
ID = OP + ["ident", 0]
RD = OP + ["steps", 0]
UP = OP + ["steps", 1]
OPERATIONS = [
    ("a plugin with an operation", PACKED4, [], None, True),
    ("the operation tier with no operations", PACKED4, OPS, DELETE, False),
    ("operations with no such tier", PACKED4, ["tiers"], ["data", "conversation"], False),
    ("the operation tier in format 2", PACKED4, ["havne_plugin"], 2, False),
    ("the operation tier with no conversation tier", PACKED4, ["tiers"],
     ["data", "operation"], False),
    ("operations that is an object", PACKED4, OPS, {}, False),
    ("no operations in the list", PACKED4, OPS, [], False),
    ("8 operations", PACKED4, OPS, many(8, op_n), True),
    ("9 operations", PACKED4, OPS, many(9, op_n), False),
    ("an operation key it doesn't take", PACKED4, OP + ["covers"], [], False),
    ("an operation id in upper case", PACKED4, OP + ["id"], "Copy", False),
    ("an operation id of 31 characters", PACKED4, OP + ["id"], "c" * 31, True),
    ("an operation id of 32 characters", PACKED4, OP + ["id"], "c" * 32, False),
    ("an operation id ending in a hyphen", PACKED4, OP + ["id"], "copy-", False),
    ("an operation kind this Havne doesn't run", PACKED4, OP + ["kind"], "flash", False),
    ("an operation with no kind", PACKED4, OP + ["kind"], DELETE, False),
    ("an operation with no title", PACKED4, OP + ["title"], DELETE, False),
    ("an empty operation title", PACKED4, OP + ["title"], "", False),
    ("an operation title of 63 bytes", PACKED4, OP + ["title"], "t" * 63, True),
    ("an operation title of 64 bytes", PACKED4, OP + ["title"], "t" * 64, False),
    ("an operation with no ecu", PACKED4, OP + ["ecu"], DELETE, False),
    ("an operation ecu that is a number", PACKED4, OP + ["ecu"], 1, False),
    ("an operation with no level", PACKED4, OP + ["level"], DELETE, False),
    ("an operation level of one digit", PACKED4, OP + ["level"], "1", False),
    ("an operation level that is a number", PACKED4, OP + ["level"], 1, False),
    ("an operation with no ident", PACKED4, OP + ["ident"], DELETE, True),
    ("8 identifiers", PACKED4, OP + ["ident"], many(8, ident_n), True),
    ("9 identifiers", PACKED4, OP + ["ident"], many(9, ident_n), False),
    ("an identifier in lower case", PACKED4, ID + ["did"], "d220", False),
    ("an identifier of three digits", PACKED4, ID + ["did"], "D22", False),
    ("an identifier with no did", PACKED4, ID + ["did"], DELETE, False),
    ("an identifier role it doesn't take", PACKED4, ID + ["role"], "serial", False),
    ("an identifier with no role", PACKED4, ID + ["role"], DELETE, False),
    ("an identifier key it doesn't take", PACKED4, ID + ["bytes"], 4, False),
    ("an operation with no steps", PACKED4, OP + ["steps"], DELETE, False),
    ("no steps in the list", PACKED4, OP + ["steps"], [], False),
    ("64 steps", PACKED4, OP + ["steps"], many(64, step_n), True),
    ("65 steps", PACKED4, OP + ["steps"], many(65, step_n), False),
    ("a step kind this Havne doesn't run", PACKED4, RD + ["step"], "download", False),
    ("a step with no kind", PACKED4, RD + ["step"], DELETE, False),
    ("a step name with an underscore", PACKED4, RD + ["name"], "cal_1", False),
    ("a step with no name", PACKED4, RD + ["name"], DELETE, False),
    ("a step in a session it doesn't read in", PACKED4, RD + ["session"], "safety", False),
    ("a plan in the programming session", PACKED4, OP + ["steps"],
     [dict(READ, session="programming"), dict(UPLOAD, session="programming")], True),
    ("a programming plan left by a reset", PACKED4, OP,
     dict(PACKED4["operations"][0], steps=[dict(READ, session="programming")],
          leave="reset"), True),
    ("a leave it doesn't take", PACKED4, OP + ["leave"], "exit", False),
    ("a step with no session", PACKED4, RD + ["session"], DELETE, False),
    ("a step address of three digits", PACKED4, RD + ["address"], "100", False),
    ("a step address of ten digits", PACKED4, RD + ["address"], "0000010000", False),
    ("a step address in lower case", PACKED4, RD + ["address"], "0001000a", False),
    ("a step address that is a number", PACKED4, RD + ["address"], 65536, False),
    ("a step of no bytes", PACKED4, RD + ["bytes"], 0, False),
    ("a step past the image's bound", PACKED4, RD + ["bytes"], 67108865, False),
    ("a step with no bytes", PACKED4, RD + ["bytes"], DELETE, False),
    ("a step of no address bytes", PACKED4, RD + ["address_bytes"], 0, False),
    ("a step of five address bytes", PACKED4, RD + ["address_bytes"], 5, False),
    ("a step of no size bytes", PACKED4, RD + ["size_bytes"], 0, False),
    ("a step of five size bytes", PACKED4, RD + ["size_bytes"], 5, False),
    ("a read with no chunk", PACKED4, RD + ["chunk"], DELETE, False),
    ("a read chunk of 0", PACKED4, RD + ["chunk"], 0, False),
    ("a read chunk of 4094", PACKED4, RD + ["chunk"], 4094, True),
    ("a read chunk of 4095", PACKED4, RD + ["chunk"], 4095, False),
    ("a read with a dfi", PACKED4, RD + ["dfi"], "00", False),
    ("an upload with a chunk", PACKED4, UP + ["chunk"], 256, False),
    ("an upload with no dfi", PACKED4, UP + ["dfi"], DELETE, True),
    ("an upload that is compressed", PACKED4, UP + ["dfi"], "10", False),
    ("a step key it doesn't take", PACKED4, RD + ["erase"], True, False),
]

# A kernel read: a made-up helper downloaded to a made-up RAM window and
# started, then the range it reads. The rules only the loader holds (the
# download inside the window, the window apart from the ranges, the
# routine among the start list, the helper's SHA-256 and its file's kind)
# are PluginOperationTest's.
HELPER = bytes((i * 7 + 3) % 256 for i in range(300))
DOWNLOAD = {"step": "download", "name": "helper", "session": "programming",
            "address": "00F00000", "address_bytes": 4, "size_bytes": 4,
            "file": "kernel.bin", "sha256": hashlib.sha256(HELPER).hexdigest()}
ROUTINE = {"step": "routine", "name": "start", "session": "programming", "routine": "0201"}
PACKED5 = changed(PACKED4, ["files", "kernel.bin"],
                  {"kind": "bin", "base64": base64.b64encode(HELPER).decode()})
PACKED5["operations"] = [{
    "id": "kernel-read", "kind": "kernel_read", "title": "Through the helper",
    "ecu": "pcm", "level": "01",
    "kernel": {"ram": {"address": "00F00000", "bytes": 16384}, "start": ["0201"],
               "stays": True,
               "functional": [{"request": "10 82"}, {"request": "3E 80", "every_ms": 2000}]},
    "steps": [DOWNLOAD, ROUTINE, dict(UPLOAD, name="code", address="00080000", session="programming")],
}]
KR = OPS + [0]
KN = KR + ["kernel"]
FN = KN + ["functional"]
KERNELS = [
    ("a kernel read", PACKED5, [], None, True),
    ("a kernel read with no kernel", PACKED5, KN, DELETE, False),
    ("a dump with a kernel", PACKED5, KR + ["kind"], "dump", False),
    ("a kernel key it doesn't take", PACKED5, KN + ["flash"], [], False),
    ("a kernel with no ram", PACKED5, KN + ["ram"], DELETE, False),
    ("a kernel with no start", PACKED5, KN + ["start"], DELETE, False),
    ("a kernel's start of no routines", PACKED5, KN + ["start"], [], False),
    ("a kernel's start in lower case", PACKED5, KN + ["start"], ["020a"], False),
    ("a kernel with no stays", PACKED5, KN + ["stays"], DELETE, False),
    ("a kernel's stays that is a word", PACKED5, KN + ["stays"], "yes", False),
    ("a kernel with no functional requests", PACKED5, FN, DELETE, True),
    ("a functional reset", PACKED5, FN + [0, "request"], "11 81", False),
    ("a functional request answered", PACKED5, FN + [0, "request"], "10 02", False),
    ("a 3E 80 with no every_ms", PACKED5, FN + [1, "every_ms"], DELETE, False),
    ("a 10 82 with an every_ms", PACKED5, FN + [0, "every_ms"], 2000, False),
    ("a 3E 80 every 4500 ms", PACKED5, FN + [1, "every_ms"], 4500, True),
    ("a 3E 80 every 4501 ms", PACKED5, FN + [1, "every_ms"], 4501, False),
    ("a functional request to an address", PACKED5, FN + [0, "to"], "physical", False),
    ("a kernel read with no routine", PACKED5, KR + ["steps", 1], dict(UPLOAD), False),
    ("a download in the extended session", PACKED5, KR + ["steps", 0, "session"],
     "extended", False),
    ("a download with no sha256", PACKED5, KR + ["steps", 0, "sha256"], DELETE, False),
    ("a download key it doesn't take", PACKED5, KR + ["steps", 0, "dfi"], "00", False),
    ("a routine of three digits", PACKED5, KR + ["steps", 1, "routine"], "201", False),
]


class Cli:
    def __init__(self, exe, scratch):
        self.exe = exe
        self.scratch = scratch
        self.count = 0

    def check(self, kind, doc=None, path=None):
        """(accepted, the refusal's detail) from signals or plugins check"""
        if path is None:
            self.count += 1
            path = os.path.join(self.scratch, f"case{self.count}.json")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(doc, f, ensure_ascii=False, indent=1)
        word = {"signal-file": "signals", "condition-file": "conditions"}.get(kind, "plugins")
        r = subprocess.run([self.exe, word, "check", path], capture_output=True, timeout=30)
        detail = ""
        for line in r.stdout.decode("utf-8", errors="replace").splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("type") == "error":
                detail = record.get("detail", "")
        if r.returncode not in (0, 3):
            raise RuntimeError(f"havne-cli {word} check exited {r.returncode}: "
                               f"{r.stderr.decode(errors='replace')}")
        return r.returncode == 0, detail


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: schema_fixtures.py <source dir> [<havne-cli>]")
        return 2
    try:
        import jsonschema
        from referencing import Registry, Resource
    except ImportError as e:
        why = f"jsonschema can't be imported ({e}): pip install -r .github/jsonschema-requirements.txt"
        if os.environ.get("ECUIO_SCHEMA_REQUIRED") == "1":
            print("FAIL  " + why + ", and ECUIO_SCHEMA_REQUIRED is set")
            return 1
        print("SKIP  " + why)
        return SKIP

    root = sys.argv[1]
    fixtures = os.path.join(root, "tests", "fixtures")
    schemas = {}
    for name in ("signal-file", "plugin-manifest", "plugin-fixtures", "condition-file"):
        with open(os.path.join(root, "schema", name + ".schema.json"), encoding="utf-8") as f:
            schemas[name] = json.load(f)
        jsonschema.Draft202012Validator.check_schema(schemas[name])
    registry = Registry().with_resources(
        (s["$id"], Resource.from_contents(s)) for s in schemas.values())
    validators = {name: jsonschema.Draft202012Validator(s, registry=registry)
                  for name, s in schemas.items()}
    validators["data-file"] = jsonschema.Draft202012Validator(
        {"$ref": schemas["plugin-manifest"]["$id"] + "#/$defs/dataFile"}, registry=registry)
    validators["page-file"] = jsonschema.Draft202012Validator(
        {"$ref": schemas["plugin-manifest"]["$id"] + "#/$defs/pageFile"}, registry=registry)

    failures = []
    checked = 0

    def load(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)

    def validate(kind, path, doc=None):
        nonlocal checked
        checked += 1
        errors = list(validators[kind].iter_errors(load(path) if doc is None else doc))
        rel = os.path.relpath(path, root).replace("\\", "/")
        for e in errors:
            where = "/".join(str(p) for p in e.absolute_path)
            failures.append(f"{rel} at /{where}: {e.message}")

    refused = {os.path.normpath(os.path.join(fixtures, p)) for p in REFUSED}

    # Signal files
    for pattern in ("tests/fixtures/signal-files/*.json", "tests/fixtures/signals/*.json",
                    "tests/golden/obdb/*.json", "examples/*.json"):
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            if os.path.normpath(path) not in refused:
                validate("signal-file", path)

    # Condition files
    for path in sorted(glob.glob(os.path.join(fixtures, "conditions", "*.json"))):
        if os.path.normpath(path) not in refused:
            validate("condition-file", path)

    # Plugins: folders with their data and fixture files, and packed ones
    for path in sorted(glob.glob(os.path.join(root, "plugins", "*", "manifest.json")) +
                       glob.glob(os.path.join(fixtures, "plugins", "*", "manifest.json"))):
        validate("plugin-manifest", path)
        manifest = load(path)
        folder = os.path.dirname(path)
        data = {f for v in manifest.get("variants", []) for f in v.get("files", [])}
        for rel in sorted(data):
            validate("data-file", os.path.join(folder, rel))
        for rel in manifest.get("fixtures", []):
            validate("plugin-fixtures", os.path.join(folder, rel))
        for rel in manifest.get("pages", []):
            validate("page-file", os.path.join(folder, rel))
    for path in sorted(glob.glob(os.path.join(fixtures, "plugins", "*.json"))):
        validate("plugin-manifest", path)

    cli = None
    scratch = tempfile.mkdtemp(prefix="havne-schema-")
    if len(sys.argv) == 3:
        cli = Cli(sys.argv[2], scratch)
    try:
        # The refused files, each at its rule and in the loader's words
        listed = {os.path.normpath(p) for p in
                  glob.glob(os.path.join(fixtures, "schema", "*.json"))}
        for p in sorted(listed - refused):
            failures.append(f"{os.path.relpath(p, root)} has no entry in REFUSED")
        for rel, (kind, keyword, words) in sorted(REFUSED.items()):
            path = os.path.join(fixtures, rel)
            checked += 1
            found = keywords(validators[kind].iter_errors(load(path)))
            if keyword not in found:
                failures.append(f"tests/fixtures/{rel}: the schema refuses it by "
                                f"{sorted(found) or 'nothing'}, not {keyword}")
            if cli is not None:
                ok, detail = cli.check(kind, path=path)
                if ok or words not in detail:
                    failures.append(f"tests/fixtures/{rel}: havne-cli says "
                                    f"{'it reads' if ok else repr(detail)}, not {words!r}")

        # Edge cases: one verdict from both
        cases = (list(CASES) + FORMAT2 + FORMAT3 + MERGED_SET + CONDITION_CASES +
                 CONVERSATION + PAGES + PUBLISHER + OPERATIONS + KERNELS)
        for p, accepted in PATHS:
            doc = changed(PACKED, ["variants", 0, "files"], [p])
            doc["files"] = {p: PACKED["files"]["a.json"]}
            cases.append((f"the path {p[:24]!r}", PACKED, None, doc, accepted))
        for what, base, path, value, accepted in cases:
            doc = value if path is None else changed(base, path, value)
            kind = ("signal-file" if base is SIGNAL_FILE else
                    "condition-file" if base is CONDITION_FILE else "plugin-manifest")
            checked += 1
            valid = validators[kind].is_valid(doc)
            if valid != accepted:
                failures.append(f"{what}: the schema {'takes' if valid else 'refuses'} it")
            if cli is not None:
                ok, detail = cli.check(kind, doc)
                if ok != accepted:
                    failures.append(f"{what}: havne-cli {'reads' if ok else 'refuses'} it"
                                    f"{'' if ok else ': ' + detail}")
    finally:
        for f in glob.glob(os.path.join(scratch, "*.json")):
            os.remove(f)
        os.rmdir(scratch)

    for f in failures:
        print("FAIL  " + f)
    if failures:
        return 1
    print(f"ok  {checked} files and cases, "
          f"{'each checked by havne-cli too' if cli else 'the schema alone (no havne-cli)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
