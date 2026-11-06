#!/usr/bin/env python3
# File/layer: fuzz/seeds.py | Python 3, no packages; CI's fuzz job, local runs
# Testing: FUZZ: CI's fuzz job, each harness started from what it writes
#
# Writes the seed inputs of the harnesses that read a structured format
# into <out>/<harness>/, one file each, from the files the tree commits:
# the signal-file and OBDb fixtures and the example plugin. Each seed is
# in the harness's own form (fuzz/README.md): the OBDb harness's first
# byte picks its options, the plugin harness's first byte its form, with
# the pieces cut at 0x1E bytes. Started from these, a 60 s run reaches
# the loaders behind the JSON reader; from nothing, it barely leaves the
# reader. Built from the fixtures, never copies of them, so the seeds
# follow the formats as the fixtures do.
#
#   python3 fuzz/seeds.py corpus
#   ./build-fuzz/fuzz/fuzz_signal_file corpus/fuzz_signal_file -max_total_time=60

import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEP = b"\x1e"


def read(path):
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read()


def write(out, harness, name, data):
    folder = os.path.join(out, harness)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)


def main(argv):
    if len(argv) != 2:
        print("usage: seeds.py <out folder>", file=sys.stderr)
        return 2
    out = argv[1]

    signal_files = sorted(glob.glob(os.path.join(ROOT, "tests/fixtures/signal-files/*.json")))
    obdb_sets = sorted(glob.glob(os.path.join(ROOT, "tests/fixtures/obdb/*.json")))

    plugin = "plugins/example-sim"
    manifest_bytes = read(plugin + "/manifest.json")
    manifest = json.loads(manifest_bytes)
    data_files = []  # each variant's files, in the order the loader asks for them
    for variant in manifest["variants"]:
        for path in variant["files"]:
            if path not in data_files:
                data_files.append(path)
    fixtures = [read(plugin + "/" + p) for p in manifest.get("fixtures", [])]

    # The JSON reader: every JSON file above, as it is
    every = [os.path.relpath(p, ROOT).replace(os.sep, "/") for p in signal_files + obdb_sets]
    every += [plugin + "/manifest.json"] + [plugin + "/" + p for p in data_files]
    every += [plugin + "/" + p for p in manifest.get("fixtures", [])]
    for path in every:
        write(out, "fuzz_json", path.replace("/", "_"), read(path))

    # The signal-file loader: the fixtures, and the plugin's data files
    # (signal files with no name or source, refused late)
    for path in signal_files:
        write(out, "fuzz_signal_file", os.path.basename(path), read(os.path.relpath(path, ROOT)))
    for path in data_files:
        write(out, "fuzz_signal_file", "plugin_" + os.path.basename(path), read(plugin + "/" + path))

    # The OBDb import: no year extending obd2 (0x00), and alone in 2019
    # (0x80 | (2019 - 1990))
    for path in obdb_sets:
        data = read(os.path.relpath(path, ROOT))
        name = os.path.basename(path)
        write(out, "fuzz_obdb_import", "default_" + name, b"\x00" + data)
        write(out, "fuzz_obdb_import", "alone_2019_" + name, bytes([0x80 | 29]) + data)

    # The plugin loader: the folder (an odd first byte; the manifest, its
    # files in turn, then a value fixture) and the packed form (an even
    # first byte; the files inlined, no fixtures key, then a fixture)
    after = SEP + fixtures[0] if fixtures else b""
    folder = b"\x01" + manifest_bytes
    for path in data_files:
        folder += SEP + read(plugin + "/" + path)
    write(out, "fuzz_plugin_manifest", "folder_example-sim", folder + after)
    packed = {k: v for k, v in manifest.items() if k != "fixtures"}
    packed["files"] = {p: json.loads(read(plugin + "/" + p)) for p in data_files}
    text = json.dumps(packed, ensure_ascii=False, indent=1).encode("utf-8")
    write(out, "fuzz_plugin_manifest", "packed_example-sim", b"\x00" + text + after)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
