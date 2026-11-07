# File/layer: tests/schema_fixtures.py | test-only, Python 3.8+, jsonschema
# Testing: HARNESS (ctest SchemaFixturesTest.TheSchemasAndTheLoadersAgree):
#          schema/ through the jsonschema package, and havne-cli as a child
#
#   python tests/schema_fixtures.py <source dir> [<havne-cli>]
#
# schema/ against the files core's loaders read (ADR-0051, ADR-0052):
# - every committed signal file, plugin manifest (a folder's or packed),
#   a folder plugin's data files and value-fixture files validate;
# - each file in tests/fixtures/schema/ (and tests/fixtures/signals/
#   refused.json) fails validation at the rule its table entry names, and
#   with havne-cli given, its check refuses the file in the words named;
# - edge cases, each one change to a file both read, get the same verdict
#   from the schema and, with havne-cli given, from its check.
# Exits 77 (ctest's skip) when jsonschema can't be imported, unless
# ECUIO_SCHEMA_REQUIRED=1 (CI's core-and-tests), which fails instead.

import copy
import glob
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
    ("a file name with no letter or digit", SIGNAL_FILE, ["name"], "--", False),
    ("a file name of 64 bytes", SIGNAL_FILE, ["name"], "n" * 64, False),
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
    ("format 2", SIGNAL_FILE, ["havne_signals"], 2, False),
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
    ("a tier named twice", PACKED, ["tiers"], ["data", "data"], False),
    ("a tier this core doesn't run", PACKED, ["tiers"], ["conversation"], False),
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
        word = "signals" if kind == "signal-file" else "plugins"
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
    for name in ("signal-file", "plugin-manifest", "plugin-fixtures"):
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
                    "tests/golden/obdb/*.json"):
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            if os.path.normpath(path) not in refused:
                validate("signal-file", path)

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
        cases = list(CASES)
        for p, accepted in PATHS:
            doc = changed(PACKED, ["variants", 0, "files"], [p])
            doc["files"] = {p: PACKED["files"]["a.json"]}
            cases.append((f"the path {p[:24]!r}", PACKED, None, doc, accepted))
        for what, base, path, value, accepted in cases:
            doc = value if path is None else changed(base, path, value)
            kind = "signal-file" if base is SIGNAL_FILE else "plugin-manifest"
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
