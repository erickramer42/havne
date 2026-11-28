import glob
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schema_fixtures import *  # noqa: E402,F401,F403


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
        print("usage: schema_fixtures_run.py <source dir> [<havne-cli>]")
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
                 CONVERSATION + PAGES + PUBLISHER + OPERATIONS + KERNELS +
                 FLASHES)
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
