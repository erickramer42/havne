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
    ("format 3", SIGNAL_FILE, ["havne_signals"], 3, False),
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
    ("format 3 of a plugin", PACKED, ["havne_plugin"], 3, False),
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

# A format 2 plugin with the conversation tier (ADR-0066). The module is
# the least one the guest ABI takes: its six exports, each returning a
# constant, and one page of memory (fuzz/seeds.py's wasm_module() wrote it)
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
        cases = list(CASES) + FORMAT2 + CONDITION_CASES + CONVERSATION
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
