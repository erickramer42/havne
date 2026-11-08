#!/usr/bin/env python3
# File/layer: fuzz/seeds.py | Python 3, no packages; CI's fuzz job, local runs
# Testing: FUZZ: CI's fuzz job, each harness started from what it writes
#
# Writes the seed inputs of the harnesses that read a structured format
# into <out>/<harness>/, one file each, from the files the tree commits:
# the signal-file and OBDb fixtures, the example plugin and the recording
# fixtures; the ELM327 emulator's, the conversations its hosts open with;
# candump's, a log's lines in each form; and the CSV import's, each
# made-up logger's map on one line before its file. Each seed is in the harness's own form (fuzz/README.md): the
# JSON harness's first byte picks its bounds, the OBDb harness's its
# options, the plugin harness's its form, the recording and export
# harnesses' a folder ('F'), with the pieces cut at 0x1E bytes. Started from
# these, a 60 s run reaches the loaders behind the JSON reader; from
# nothing, it barely leaves the reader. Built from the fixtures, never
# copies of them, so the seeds follow the formats as the fixtures do.
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

    # The signal-file fixtures, the CLI's among them (two ECUs, an enum on
    # a PID, extends; a misspelled key the loader refuses)
    signal_files = sorted(glob.glob(os.path.join(ROOT, "tests/fixtures/signal-files/*.json")) +
                          glob.glob(os.path.join(ROOT, "tests/fixtures/signals/*.json")))
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

    # The JSON reader: every JSON file above, whole after the bounds
    # byte (0x1F: depth 32, the loaders' bound, and no size bound)
    every = [os.path.relpath(p, ROOT).replace(os.sep, "/") for p in signal_files + obdb_sets]
    every += [plugin + "/manifest.json"] + [plugin + "/" + p for p in data_files]
    every += [plugin + "/" + p for p in manifest.get("fixtures", [])]
    for path in every:
        write(out, "fuzz_json", path.replace("/", "_"), b"\x1f" + read(path))

    # The signal-file loader: the fixtures, and the plugin's data files
    # (signal files with no name or source, refused late)
    for path in signal_files:
        folder = os.path.basename(os.path.dirname(path))
        write(out, "fuzz_signal_file", folder + "_" + os.path.basename(path),
              read(os.path.relpath(path, ROOT)))
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

    # The recording and export harnesses: a folder, 'F' then events.log,
    # signals.csv, markers.csv and frames.csv cut at 0x1E, each opened
    # through signals.csv and through its frames, or exported and
    # reported. The findings fixture (a listener's wire frames, every
    # part), the packed replies (frames alone), and frames written for
    # the ISO-TP paths: a packed reply in a First and a Consecutive Frame
    # with the tester's Flow Control between, two senders' First Frames
    # on one ID, and a message whose Consecutive Frame comes the next
    # second. The export's own: an engine ECU's identification and
    # diagnostics, whole and on the wire, and a DID echoing its VIN
    findings = "tests/fixtures/recording/findings/"
    folder_seeds = {}
    folder_seeds["findings"] = b"F" + SEP.join(
        read(findings + n) for n in
        ("events.log", "signals.csv", "markers.csv", "frames.csv"))
    folder_seeds["packed-replies"] = (
        b"F" + SEP + SEP + SEP + read("tests/fixtures/replay/packed-replies/frames.csv"))
    header = b"host_us,timestamp_us,id,flags,data\n"
    wire = header + (
        b"100000,100000,0x7DF,0x0,02 01 0C 00 00 00 00 00\n"
        b"110000,110000,0x7E8,0x0,04 41 0C 1A F8 00 00 00\n"
        b"200000,200000,0x7E8,0x0,10 0A 41 0C 1B 58 0D 32\n"
        b"201000,201000,0x7E0,0x0,30 00 00 00 00 00 00 00\n"
        b"210000,210000,0x7E8,0x0,21 05 7B 0F 41 00 00 00\n"
        b"300000,300000,0x7E8,0x0,10 0A 41 0C 11 11 0D 01\n"
        b"310000,310000,0x7E8,0x0,10 0A 41 0C 22 22 0D 02\n"
        b"320000,320000,0x7E8,0x0,21 05 7B 0F 41 00 00 00\n"
        b"990000,990000,0x7E8,0x0,10 0A 41 0C 1A 00 0D 32\n"
        b"1005000,1005000,0x7E8,0x0,21 05 7B 0F 41 00 00 00\n"
        b"1500000,1500000,0x7E8,0x100,41 0C 1A 0D 32\n")
    events = (b"ecuio session recording, format 2 (ADR-0049)\n"
              b"profile  obd2\n\n")
    folder_seeds["wire-frames"] = b"F" + events + SEP + SEP + SEP + wire
    for name, seed in folder_seeds.items():
        write(out, "fuzz_recording", name, seed)
        write(out, "fuzz_export", name, seed)
    vin = b"ZZZFAKE11M0000001"
    ecu = header + (
        b"100000,100000,0x7E8,0x100,49 02 01 " + b" ".join(b"%02X" % c for c in vin) + b"\n"
        b"200000,200000,0x7E8,0x100,41 01 83 07 E5 00\n"
        b"300000,300000,0x7E8,0x100,43 02 03 01 04 20\n"
        b"400000,400000,0x7E8,0x0,10 14 62 D8 01 5A 5A 5A\n"
        b"401000,401000,0x7E0,0x0,30 00 00 00 00 00 00 00\n"
        b"410000,410000,0x7E8,0x0,21 46 41 4B 45 31 31 4D\n"
        b"420000,420000,0x7E8,0x0,22 30 30 30 30 30 30 31\n"
        b"1500000,1500000,0x7E8,0x100,41 0C 1A F8\n")
    events = (b"ecuio session recording, format 2 (ADR-0049)\n"
              b"uri  elm327://COM3?profile=obd2\n"
              b"profile  obd2\n"
              b"frame flags  complete\n\n")
    signals = b"\xef\xbb\xbfhost_us,signal,value,unit\n1500000,RPM,1726,RPM\n"
    write(out, "fuzz_export", "engine-ecu",
          b"F" + events + SEP + signals + SEP + SEP + ecu)

    # The ELM327 emulator: a mode byte (bit 0 a second module, bit 1 the
    # faults), then what a host sends, CR-ended: python-OBD's opening,
    # Havne's setup (core/src/elm327_transport.cpp) and a VIN read, each
    # in every mode
    hosts = {
        "python-obd": ["ATZ", "ATE0", "ATH1", "ATL0", "AT RV", "ATSP0", "0100",
                       "ATDPN", "0120", "0140", "0902", "010C", "03"],
        "havne": ["ATZ", "ATI", "ATE0", "ATL0", "ATS0", "ATH1", "ATD0", "ATCAF1",
                  "ATCFC1", "ATSP6", "ATAT1", "ATST19", "ATSH7E0", "ATCRA7E8",
                  "ATRV", "STI", "01001", "01201", "09022", "010C1", "0101",
                  "03", "ATSH7E1", "ATCRA7E9", "22D1011", "ATMA", " "],
        "vin": ["ATZ", "ATE0", "ATH0", "ATS1", "0902", "ATH1", "ATD1", "0902",
                "", "0902", "ATSP7", "0902", "ATSP8", "0902"],
    }
    for name, lines in hosts.items():
        text = "".join(line + "\r" for line in lines).encode("ascii")
        for mode in range(4):
            write(out, "fuzz_elm327_emulator", "%s_mode%d" % (name, mode),
                  bytes([mode]) + text)

    # candump -L's lines (ADR-0054 section 4), one of each form the parser
    # reads: classic, 29-bit, a DLC past 8, remote, CAN FD, an error
    # frame, candump -x's direction; and a request, its reply segmented,
    # the tester's Flow Control
    write(out, "fuzz_candump", "forms",
          b"(1700000000.250000) can0 7DF#0201000000000000\n"
          b"(1700000000.254000) can0 18DAF110#0441057B\n"
          b"(1700000000.255000) can0 123#1122334455667788_C\n"
          b"(1700000000.256000) can0 123#R\n"
          b"(1700000000.257000) can0 123#R8_9\n"
          b"(1700000000.258000) vcan0 456##3AABBCCDDEEFF.0011\n"
          b"(1700000000.259000) can0 20000004#0000080000000000\n"
          b"(1700000000.260000) can0 7E0#02010C T\n"
          b"(1700000000.261000) can0 7E8#04410C1AF8 R\r\n")
    write(out, "fuzz_candump", "segmented",
          b"(1.000000) can0 7E0#0209020000000000\n"
          b"(1.004000) can0 7E8#1014490201414243\n"
          b"(1.004500) can0 7E0#3000000000000000\n"
          b"(1.005000) can0 7E8#2144454647484950\n"
          b"(1.005500) can0 7E8#2251525354555657\n")

    # The CSV import: the first line a column map's JSON, the rest the file
    for cmap_path in sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "import",
                                                   "csv", "*", "map.json"))):
        folder = os.path.dirname(cmap_path)
        with open(cmap_path, encoding="utf-8") as f:
            cmap = json.dumps(json.load(f), separators=(",", ":")).encode("utf-8")
        with open(os.path.join(folder, "log.csv"), "rb") as f:
            log = f.read()
        write(out, "fuzz_csv_import", os.path.basename(folder), cmap + b"\n" + log)
        # The guess over the same file, with no unit and with one given
        write(out, "fuzz_csv_import", os.path.basename(folder) + "-guess",
              b"guess\n" + log)
        write(out, "fuzz_csv_import", os.path.basename(folder) + "-guess-ms",
              b"guess ms\n" + log)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
