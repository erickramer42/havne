# File/layer: tests/schema_keys.py | test-only, Python 3.8+, no packages
# Testing: UNIT (ctest SchemaKeysTest.EachObjectsKeysAreItsLoaders)
#
#   python tests/schema_keys.py <source dir>
#
# Keeps schema/ in step with core's loaders (ADR-0051, ADR-0052): every
# only_keys(<object>, {<keys>}, <context>) call in the loaders is mapped
# below to the schema object it checks, and that object's properties are
# exactly those keys plus "notes", with "^x-" free and nothing else
# (additionalProperties false), as only_keys allows. A call the table
# doesn't map, an entry no call matches, and a schema object no call maps
# all fail, so a key added on either side alone fails. The loaders' word
# lists that a schema spells as an enum (kinds, groups, services, byte
# orders, extends, tiers) are compared too.

import json
import os
import re
import sys

LOADERS = ("core/src/signal_file.cpp", "core/src/plugin.cpp")
SCHEMAS = ("signal-file", "plugin-manifest", "plugin-fixtures")

# (loader, the call's context argument as written, its place among the
# calls with that context in that file, from 1) -> (schema, JSON pointer)
CALLS = {
    ("signal_file.cpp", '"an ECU"', 1): ("signal-file", "/$defs/ecu"),
    ("signal_file.cpp", "what + \"'s read\"", 1): ("signal-file", "/$defs/read"),
    # read_field: text and hex, then enum, then number
    ("signal_file.cpp", "what + \"'s field\"", 1): ("signal-file", "/$defs/textField"),
    ("signal_file.cpp", "what + \"'s field\"", 2): ("signal-file", "/$defs/enumField"),
    ("signal_file.cpp", "what + \"'s field\"", 3): ("signal-file", "/$defs/numberField"),
    ("signal_file.cpp", '"\\"source\\""', 1): ("signal-file", "/$defs/source"),
    ("signal_file.cpp", '"the file"', 1): ("signal-file", ""),
    # build: computed, then text and hex, then enum, then number
    ("signal_file.cpp", "what", 1): ("signal-file", "/$defs/computedSignal"),
    ("signal_file.cpp", "what", 2): ("signal-file", "/$defs/textSignal"),
    ("signal_file.cpp", "what", 3): ("signal-file", "/$defs/enumSignal"),
    ("signal_file.cpp", "what", 4): ("signal-file", "/$defs/numberSignal"),
    ("plugin.cpp", '"the manifest"', 1): ("plugin-manifest", ""),
    ("plugin.cpp", '"\\"requires\\""', 1): ("plugin-manifest", "/properties/requires"),
    ("plugin.cpp", '"a variant"', 1): ("plugin-manifest", "/$defs/variant"),
    ("plugin.cpp", '"a match"', 1): ("plugin-manifest", "/$defs/match"),
    ("plugin.cpp", '"a data file"', 1): ("plugin-manifest", "/$defs/dataFile"),
    ("plugin.cpp", '"a fixture file"', 1): ("plugin-fixtures", ""),
    ("plugin.cpp", '"a value fixture"', 1): ("plugin-fixtures", "/$defs/value"),
}

# (source, a pattern whose group 1 is one word of the list) -> (schema,
# pointer to the enum or const that spells it)
WORDS = [
    ("core/src/signal_file.cpp", r'e\.kind != "([a-z]+)"',
     ("signal-file", "/$defs/signal/allOf/0/properties/kind/enum")),
    ("core/src/signal_file.cpp", r'service->text == "([0-9A-F]+)"',
     ("signal-file", "/$defs/read/properties/service/enum")),
    ("core/src/signal_file.cpp", r'order->text [!=]= "([a-z]+)"',
     ("signal-file", "/$defs/order/enum")),
    ("core/src/signal_file.cpp", r'extends->text != "([a-z0-9]+)"',
     ("signal-file", "/$defs/extends/enum")),
    ("core/src/plugin.cpp", r't\.text != "([a-z]+)"',
     ("plugin-manifest", "/properties/tiers/items/const")),
]


def strip_comments(text):
    """C++ text with its comments blanked, strings kept"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            out.append("\n" * text.count("\n", i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def call_args(text, start):
    """The top-level arguments of the call whose '(' is at start"""
    args = []
    depth = 0
    i = start
    begin = start + 1
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
            if depth == 0:
                args.append(text[begin:i])
                return args
        elif c == "," and depth == 1:
            args.append(text[begin:i])
            begin = i + 1
        i += 1
    raise ValueError("a call with no closing parenthesis")


def loader_calls(root):
    """Each only_keys call: (file, context, place) -> (keys, line)"""
    found = {}
    for rel in LOADERS:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            text = strip_comments(f.read())
        name = os.path.basename(rel)
        places = {}
        for m in re.finditer(r"\bonly_keys\s*\(", text):
            if re.search(r"\bvoid\s+$", text[:m.start()]):
                continue  # the definition
            args = call_args(text, m.end() - 1)
            line = text.count("\n", 0, m.start()) + 1
            if len(args) != 3 or not args[1].strip().startswith("{"):
                raise ValueError(f"{name} line {line}: an only_keys call "
                                 f"this test can't read: {args}")
            keys = re.findall(r'"((?:[^"\\]|\\.)*)"', args[1])
            context = " ".join(args[2].split())
            places[context] = places.get(context, 0) + 1
            found[(name, context, places[context])] = (keys, line)
    return found


def pointer(doc, ptr):
    node = doc
    for part in ptr.split("/")[1:]:
        part = part.replace("~1", "/").replace("~0", "~")
        node = node[int(part)] if isinstance(node, list) else node[part]
    return node


def object_schemas(node, ptr=""):
    """Every pointer to a subschema of type object with properties"""
    found = []
    if isinstance(node, dict):
        if node.get("type") == "object" and "properties" in node:
            found.append(ptr)
        for k, v in node.items():
            found += object_schemas(v, ptr + "/" + k.replace("~", "~0").replace("/", "~1"))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            found += object_schemas(v, f"{ptr}/{i}")
    return found


def main():
    if len(sys.argv) != 2:
        print("usage: schema_keys.py <source dir>")
        return 2
    root = sys.argv[1]
    schemas = {}
    for s in SCHEMAS:
        with open(os.path.join(root, "schema", s + ".schema.json"), encoding="utf-8") as f:
            schemas[s] = json.load(f)
    failures = []

    calls = loader_calls(root)
    for key, (keys, line) in sorted(calls.items()):
        name, context, place = key
        if key not in CALLS:
            failures.append(f"{name} line {line}: only_keys(..., {context}) "
                            f"(call {place} with that context) maps to no "
                            f"schema object: add it to CALLS")
            continue
        schema, ptr = CALLS[key]
        where = f"{schema}.schema.json#{ptr}"
        obj = pointer(schemas[schema], ptr)
        props = set(obj.get("properties", {}))
        want = set(keys) | {"notes"}
        if props != want:
            only_loader = sorted(want - props)
            only_schema = sorted(props - want)
            failures.append(f"{name} line {line} vs {where}: keys only the "
                            f"loader takes {only_loader}, only the schema "
                            f"takes {only_schema}")
        if obj.get("additionalProperties") is not False:
            failures.append(f"{where}: additionalProperties isn't false")
        if set(obj.get("patternProperties", {})) != {"^x-"}:
            failures.append(f"{where}: patternProperties isn't \"^x-\" alone")
        if obj.get("properties", {}).get("notes") != {
                "$ref": ("#" if schema == "signal-file" else
                         "signal-file.schema.json#") + "/$defs/notes"}:
            failures.append(f"{where}: \"notes\" isn't the shared notes")
    for key, (schema, ptr) in sorted(CALLS.items()):
        if key not in calls:
            failures.append(f"CALLS maps {key} to {schema}.schema.json#{ptr}, "
                            f"but no only_keys call has that context and place")

    mapped = {(s, p) for s, p in CALLS.values()}
    for s in SCHEMAS:
        for ptr in object_schemas(schemas[s]):
            if (s, ptr) not in mapped:
                failures.append(f"{s}.schema.json#{ptr} is an object with "
                                f"properties that no only_keys call maps to")

    # only_keys's own rule: "notes" a string, "x-" keys skipped
    with open(os.path.join(root, LOADERS[0]), encoding="utf-8") as f:
        sf = f.read()
    body = re.search(r"void only_keys\(.*?\n\}", sf, re.S)
    if body is None or 'm.key == "notes"' not in body.group(0) \
            or 'compare(0, 2, "x-")' not in body.group(0):
        failures.append("only_keys doesn't allow \"notes\" and \"x-\" keys "
                        "as the schemas' notes and ^x- do: change both")

    words = list(WORDS)
    with open(os.path.join(root, "core/src/signals.cpp"), encoding="utf-8") as f:
        names = re.search(r"kNames\[\] = \{(.*?)\};", f.read(), re.S)
    groups = [g.lower() for g in re.findall(r'"([^"]*)"', names.group(1))] if names else []
    for src, pattern, (schema, ptr) in words:
        with open(os.path.join(root, src), encoding="utf-8") as f:
            have = set(re.findall(pattern, strip_comments(f.read())))
        spelled = pointer(schemas[schema], ptr)
        spelled = set(spelled) if isinstance(spelled, list) else {spelled}
        if not have:
            failures.append(f"{src}: nothing matches {pattern}: the loader "
                            f"moved, so find its list again")
        elif have != spelled:
            failures.append(f"{src}'s {sorted(have)} vs {schema}.schema.json#"
                            f"{ptr}'s {sorted(spelled)}")
    spelled = set(pointer(schemas["signal-file"], "/$defs/group/enum"))
    if not groups or set(groups) != spelled:
        failures.append(f"core/src/signals.cpp's groups {groups} vs "
                        f"signal-file.schema.json#/$defs/group/enum's {sorted(spelled)}")

    for f in failures:
        print("FAIL  " + f)
    if failures:
        return 1
    print(f"ok  {len(calls)} only_keys calls, each its schema object's keys; "
          f"{len(words) + 1} word lists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
