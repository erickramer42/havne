#!/usr/bin/env python3
# File/layer: fuzz/seeds.py | Python 3, no packages; CI's fuzz job, local runs
# Testing: FUZZ: CI's fuzz job, each harness started from what it writes
#
# Writes the seed inputs of the harnesses that read a structured format
# into <out>/<harness>/, one file each, from the files the tree commits:
# the signal-file, condition-file and OBDb fixtures, the example plugin
# and the recording fixtures; the ELM327 emulator's, the conversations its
# hosts open with;
# candump's, a log's lines in each form; and the CSV import's, each
# made-up logger's map on one line before its file. Each seed is in the harness's own form (fuzz/README.md): the
# JSON harness's first byte picks its bounds, the OBDb harness's its
# options, the plugin harness's its form, the recording and export
# harnesses' a folder ('F', or '3' for format 3's first line), with the
# pieces cut at 0x1E bytes (a recording's conditions in a fifth), or the
# stretches' rules ('S') over two fixtures' samples. Started from
# these, a 60 s run reaches the loaders behind the JSON reader; from
# nothing, it barely leaves the reader. Built from the fixtures, never
# copies of them, so the seeds follow the formats as the fixtures do.
# fuzz_computed's are programs in the harness's built form (computed()),
# where the format 1 machine and this one would part if they ever
# differed: no coverage tells the fuzzer that a value went wrong.
#
#   python3 fuzz/seeds.py corpus
#   ./build-fuzz/fuzz/fuzz_signal_file corpus/fuzz_signal_file -max_total_time=60

import glob
import hashlib
import json
import os
import re
import struct
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


def signal_rows(path):
    """{signal: [(host_us, value)]} of a signals.csv's number rows"""
    rows = {}
    for line in read(path).decode("utf-8-sig").splitlines()[1:]:
        cells = line.split(",")
        try:
            rows.setdefault(cells[1], []).append((int(cells[0]), float(cells[2])))
        except (IndexError, ValueError):
            pass
    return rows


def stretch_seed(switches, gate, speed, measure):
    """fuzz_recording's stretches input: the switches, the default rules
    but the rise, 10 on the seeds' scale of rpm / 60 (indexes into the
    harness's tables), the whole range, then each
    signal's samples, at most 255, a time step in 10 ms and a value in
    halves under 120"""
    rules = bytes([7, 5, 8, 0, 6, 3, 4, 6, 14, 14])
    signals = [gate[:255], speed[:255], measure[:255]]
    out = b"S" + bytes([switches]) + rules + bytes([0, 255])
    out += bytes(len(s) for s in signals)
    for s in signals:
        last = 0
        for t, v in s:
            step = min((t - last) // 10000, 199)
            last += step * 10000
            out += bytes([step, max(0, min(239, round(v * 2)))])
    return out


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
    # Format 2's grammar (ADR-0062), which no fixture holds yet: each
    # function and comparison once, over four DIDs
    leaves = [{"key": k, "name": k.upper(), "read": {"service": "22", "number": "D10%d" % n},
               "field": {"byte": 0}} for n, k in enumerate("abcd")]
    exprs = ["min(a, b, c)", "max(a, b) - abs(c)", "clamp(a, 0, 100)",
             "sqrt(round(a) + floor(b) * ceil(c))",
             "if(all(a > b, b >= c, c < d), a, if(any(a <= 1, not(b == 2), c != 3), b, -c))"]
    grammar = {"havne_signals": 2, "name": "Grammar", "source": {"name": "Havne's seeds"},
               "ecus": [{"id": "pcm", "rx": "7E8"}],
               "signals": leaves + [{"key": "e%d" % n, "name": "E%d" % n, "kind": "computed",
                                     "expr": e} for n, e in enumerate(exprs)]}
    write(out, "fuzz_signal_file", "grammar_format2.json", json.dumps(grammar).encode())
    # A chain (ADR-0062) listed top first, so its depth order isn't the
    # set's: the harness's decode checks the batch keeps the set's
    chain_exprs = [("top", "mid * 2 + side"), ("side", "low - d"), ("mid", "low + b"),
                   ("low", "a + c")]
    chain = {"havne_signals": 2, "name": "Chain", "source": {"name": "Havne's seeds"},
             "ecus": [{"id": "pcm", "rx": "7E8"}],
             "signals": leaves + [{"key": k, "name": k.upper(), "kind": "computed", "expr": e}
                                  for k, e in chain_exprs]}
    write(out, "fuzz_signal_file", "chain_format2.json", json.dumps(chain).encode())

    # The condition-file loader (ADR-0063): each fixture alone, and again
    # before a signal file to resolve it against (cut at 0x1E). One of
    # them, every-key.json, holds every key the loader takes, each term
    # kind, and a reference of each form to each kind of signal in that
    # signal file; another, holds.json, joins two terms under a hold, so
    # the judged run meets a term changing while a hold runs; links.json
    # joins link terms with signal terms, so a change of the link meets an
    # age bound running out.
    # ConditionFileTest loads each fixture, so a seed the loader refuses
    # by mistake fails there.
    condition_files = sorted(glob.glob(os.path.join(ROOT, "tests/fixtures/conditions/*.json")))
    extras = read("tests/fixtures/signals/extras.json")
    for path in condition_files:
        name = os.path.basename(path)
        data = read(os.path.relpath(path, ROOT))
        write(out, "fuzz_condition_file", name, data)
        write(out, "fuzz_condition_file", "extras_" + name, data + SEP + extras)

    # The OBDb import: no year extending obd2 (0x00), and alone in 2019
    # (0x80 | (2019 - 1990))
    for path in obdb_sets:
        data = read(os.path.relpath(path, ROOT))
        name = os.path.basename(path)
        write(out, "fuzz_obdb_import", "default_" + name, b"\x00" + data)
        write(out, "fuzz_obdb_import", "alone_2019_" + name, bytes([0x80 | 29]) + data)
    # Two signals of one 31-byte name whose two-byte character sits across
    # the place a repeat's number is cut in: the cut must leave it whole
    name = "n" * 28 + "\u00e9x"
    repeated = {"commands": [
        {"hdr": "7E1", "rax": "7E9", "cmd": {"22": did}, "freq": 1,
         "signals": [{"id": key, "name": name,
                      "fmt": {"len": 8, "max": 255, "unit": "volts"}}]}
        for did, key in (("0101", "first"), ("0102", "second"))]}
    write(out, "fuzz_obdb_import", "repeated_name_across_a_character",
          b"\x00" + json.dumps(repeated, ensure_ascii=False).encode("utf-8"))

    # The set validator: a head (count, layout, reserved, channels, NULLs),
    # then six bytes for each def. Two defs with ids of their own make a
    # set it takes; with the second's repeat bit, one name twice
    layout = int(re.search(rb"#define ECUIO_SIGNAL_SET_LAYOUT (\d+)u",
                           read("core/include/ecuio/signals.h")).group(1))
    two = bytes([2, layout, 0, 0, 0, 1, 0, 8, 0, 0, 0, 2, 0, 8, 0, 0, 0])
    write(out, "fuzz_validate_set", "two_defs", two)
    write(out, "fuzz_validate_set", "two_defs_one_name",
          two[:15] + b"\x80" + two[16:])
    # Four bytes for each channel after the defs: one channel (0x7E8 and
    # 0x6E8), the same with its flags set, then one more than a set holds
    most = int(re.search(rb"#define ECUIO_MAX_CHANNELS (\d+)",
                         read("core/include/ecuio/signals.h")).group(1))
    head = bytes([0, layout, 0])
    write(out, "fuzz_validate_set", "one_channel",
          head + bytes([1, 0, 0xE8, 0x07, 0x00, 0x06]))
    write(out, "fuzz_validate_set", "one_channel_flags",
          head + bytes([1, 0, 0xE8, 0x07, 0x04, 0x06]))
    write(out, "fuzz_validate_set", "a_channel_too_many",
          head + bytes([most + 1, 0]) +
          b"".join(bytes([0x10 + i, 0x07, 0x00, 0x06])
                   for i in range(most + 1)))

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

    # Making and changing a packed plugin: a switches byte, then a signal
    # file to wrap, 0x1E and a signal file to put into the result; with
    # 0x40 the first piece is a packed plugin. Each file alone under two
    # switches, each ordered pair (the put's variant "other", then
    # "base", which replaces the wrap's), and the example plugin with
    # each file put as "other" and under the file's own name
    wrap_files = [(os.path.basename(os.path.dirname(p)) + "_" + os.path.basename(p),
                   read(os.path.relpath(p, ROOT))) for p in signal_files]
    wrap_files += [("grammar_format2.json", json.dumps(grammar).encode()),
                   ("chain_format2.json", json.dumps(chain).encode())]
    for name, data in wrap_files:
        write(out, "fuzz_plugin_wrap", "wrap_" + name, b"\x00" + data)
        write(out, "fuzz_plugin_wrap", "wrap_options_" + name, b"\x0f" + data)
        write(out, "fuzz_plugin_wrap", "packed_put_" + name, b"\x40" + text + SEP + data)
        write(out, "fuzz_plugin_wrap", "packed_slug_" + name, b"\x50" + text + SEP + data)
        for other, second in wrap_files:
            write(out, "fuzz_plugin_wrap", "put_" + name + "_" + other,
                  b"\x00" + data + SEP + second)
            write(out, "fuzz_plugin_wrap", "replace_" + name + "_" + other,
                  b"\x20" + data + SEP + second)

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
    # The harness's first float on 0x123: 1.0, +inf, -inf, a NaN, 2.0. The
    # three between are no samples, at the open or read again
    folder_seeds["float-not-finite"] = b"F" + SEP + SEP + SEP + header + (
        b"100000,100000,0x123,0x0,3F 80 00 00\n"
        b"200000,200000,0x123,0x0,7F 80 00 00\n"
        b"300000,300000,0x123,0x0,FF 80 00 00\n"
        b"400000,400000,0x123,0x0,7F C0 00 00\n"
        b"500000,500000,0x123,0x0,40 00 00 00\n")
    # Both floats: the first 1.0, 2.0, 3.0, the second never finite, so it
    # has no signal and a second read again passes its values by
    folder_seeds["float-never-finite"] = b"F" + SEP + SEP + SEP + header + (
        b"100000,100000,0x123,0x0,3F 80 00 00 7F C0 00 00\n"
        b"300000,300000,0x123,0x0,40 00 00 00 7F 80 00 00\n"
        b"500000,500000,0x123,0x0,40 40 00 00 FF C0 00 00\n")
    # signals.csv alone, a number signal whose one value is a NaN: its
    # range reads 0 at the open and NaN in its statistics
    folder_seeds["values-all-nan"] = (
        b"F" + SEP + b"host_us,signal,value,unit\n0,a,nan,\n")
    # A session's conditions ('3', then a fifth part, conditions.json):
    # the header's conditions line with the copy's hash, a held line,
    # changes, an unknown flag word, an eighth field and a key the reader
    # skips (ADR-0063 section 8)
    conditions = (b'{"havne_conditions":1,"conditions":[{"key":"high",'
                  b'"name":"High revs","mark":true,"when":{"signal":'
                  b'["obd2.rpm"],"unit":"RPM","above":2000}}]}')
    events = (b"profile  obd2\nframe flags  complete\n"
              b"signal  RPM,number,RPM,0,1\n"
              b"conditions  conditions.json," +
              hashlib.sha256(conditions).hexdigest().encode("ascii") +
              b"\n\n"
              b"12:00:01.000  +1.000  cond  high,on,held,3000,0,1000000,1000000\n"
              b"12:00:02.000  +2.000  cond  high,off,,1000,0,2000000,2000000\n"
              b"12:00:03.000  +3.000  cond  high,on,fired stale,3200,0,3000000,"
              b"3000000,later\n"
              b"12:00:03.000  +3.000  cond  low,on,fired blink,nan,1,3000000,"
              b"2999000\n"
              b"12:00:04.000  +4.000  cond  High,on,,1,0,1,1\n"
              b"12:00:05.000  +5.000  end  done: 3 frames, 3 values, "
              b"0 records dropped, 0 notes dropped\n")
    folder_seeds["conditions"] = b"3" + SEP.join((
        events,
        b"host_us,signal,value,unit\n1000000,RPM,3000,RPM\n"
        b"2000000,RPM,1000,RPM\n3000000,RPM,3200,RPM\n",
        b"host_us,label\n3000000,High revs\n",
        header + b"1000000,1000000,0x7E8,0x100,41 0C 2E E0\n"
        b"2000000,2000000,0x7E8,0x100,41 0C 0F A0\n"
        b"3000000,3000000,0x7E8,0x100,41 0C 32 00\n",
        conditions))
    # Its edges: a key past a set's 64, a cond line past the longest row
    # (32768 bytes) and a torn last one (a copy past the loader's 256 KiB
    # can't fit the harness's input, so RecordingTest has that one)
    keys = b"".join(b"12:00:01.000  +1.000  cond  k%d,on,,1,0,1,1\n" % k
                    for k in range(65))
    small = b"{}"
    folder_seeds["conditions-edges"] = b"3" + SEP.join((
        b"conditions  conditions.json," +
        hashlib.sha256(small).hexdigest().encode("ascii") + b"\n\n" + keys +
        b"12:00:02.000  +2.000  cond  k0,off,,1,0,2,2," + b"x" * 32768 +
        b"\n12:00:03.000  +3.000  cond  k1,off,,1,0,3,3",
        b"host_us,signal,value,unit\n", b"", b"", small))
    for name, seed in folder_seeds.items():
        write(out, "fuzz_recording", name, seed)
        write(out, "fuzz_export", name, seed)
    # The stretches' rules ('S'): the pull fixture's throttle and engine
    # speed (no speed signal, so engine speed measures; switch 16, the
    # recording has an engine speed), and the cruise fixture's speed and
    # throttle, under the default rules but the rise, 10 on the seed's
    # scale of rpm / 60
    pulls = signal_rows("tests/fixtures/recording/pull-early/signals.csv")
    write(out, "fuzz_recording", "stretches-pull",
          stretch_seed(16, pulls.get("Throttle", []), [],
                       [(t, v / 60) for t, v in pulls.get("RPM", [])]))
    cruise = signal_rows("tests/fixtures/recording/cruise/signals.csv")
    write(out, "fuzz_recording", "stretches-cruise",
          stretch_seed(3, cruise.get("Throttle", []),
                       cruise.get("Vehicle Speed", []), []))
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

    # The CSV import: the first line a column map's JSON (the logger's own,
    # else its guessed one), the rest the file
    for log_path in sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "import",
                                                  "csv", "*", "log.csv"))):
        folder = os.path.dirname(log_path)
        cmap_path = os.path.join(folder, "map.json")
        if not os.path.exists(cmap_path):
            cmap_path = os.path.join(folder, "guessed-map.json")
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
        # The start of a longer file, cut at a line feed
        write(out, "fuzz_csv_import", os.path.basename(folder) + "-guess-cut",
              b"guess cut\n" + log[:len(log) * 2 // 3])

    # The computed machine: its operations by kOperations' index
    add, sub, mul, div, mn, mx, sqrt, rnd = 0, 1, 2, 3, 4, 5, 7, 8
    gt, eq, all_, any_, if_ = 13, 15, 17, 18, 20
    inf, nzero, tiny = float("inf"), -0.0, 5e-324
    seeds = {
        "one-over-x": (True, [("k", 0), ("i", 0), div], [1.0], [inf, 0, 0, 0]),
        "boost": (True, [("i", 0), ("i", 1), sub], [], [28.5, 25.0, 0, 0]),
        "ratio": (True, [("i", 0), ("i", 1), add, ("i", 1), div], [], [tiny, 2.5, 0, 0]),
        "air-per-rev": (False, [("i", 0), ("k", 0), gt, ("i", 1), ("k", 1), mul,
                                ("i", 0), div, ("k", 0), if_],
                        [0.0, 60.0], [0.0, 12.3, 0, 0]),
        "gear": (False, [("i", 0), ("k", 0), eq, ("k", 0), ("i", 1), ("i", 2), div, if_],
                 [0.0], [0.0, 20.0, None, 0]),
        "all-any": (False, [("i", 0), ("k", 0), gt, ("i", 1), ("k", 0), gt, all_,
                            ("i", 2), ("k", 0), gt, any_, ("k", 1), ("k", 0), if_],
                    [0.0, 1.0], [1.0, None, -1.0, 0]),
        "clamp": (False, [("i", 0), ("k", 0), mx, ("k", 1), mn], [0.0, 100.0],
                  [104.7, 0, 0, 0]),
        "edges": (False, [("i", 0), sqrt, ("i", 1), rnd, mn, ("i", 2), mx],
                  [], [nzero, -0.5, inf, 0]),
        "deepest": (True, [("i", k % 4) for k in range(17)] + [add, sub, mul, div] * 4,
                    [], [1.0, 2.0, 3.0, 4.0]),
    }
    for name, (format1, steps, constants, inputs) in seeds.items():
        write(out, "fuzz_computed", name, computed(format1, steps, constants, inputs))
    # The program as the struct's own bytes (an odd first byte): boost
    program = struct.pack("<4H4I4B36s16d4I", 1, 2, 0, 0, 0, 0, 0, 0, 2, 3, 0, 0,
                          bytes([0x00, 0x01, 0x21]), *([0.0] * 16), 0, 0, 0, 0)
    write(out, "fuzz_computed", "struct-boost",
          b"\x01" + program + b"\x00" + b"".join(number_bytes(v) for v in (28.5, 25.0, 0, 0)))
    return 0


# fuzz_computed's edges, in its kEdges' order (the last, NaN, left out)
EDGES = [float("inf"), float("-inf"), 0.0, -0.0, 5e-324, -5e-324, 1.7976931348623157e308,
         -1.7976931348623157e308, 1e-300, 1e300, 0.5, -0.5, 2.5]


def number_bytes(v):
    """A value in fuzz_computed's form: an edge's byte, a small number's
    (a whole number of halves from -32 to 31.5), or 0x80 and 8 bytes"""
    for i, e in enumerate(EDGES):
        if struct.pack("<d", e) == struct.pack("<d", v):
            return bytes([0xC0 | i])
    x = v * 4
    if x == int(x) and int(x) % 2 == 0 and -128 <= int(x) < 128:
        return bytes([(int(x) & 0xFF) >> 1])
    return b"\x80" + struct.pack("<d", v)


def computed(format1, steps, constants, inputs):
    """An input to fuzz_computed's built form: each step ("i", k) pushes
    input k, ("k", c) constant c, and a number is an operation's index in
    kOperations; an input of None has no value"""
    pushed = [s[1] for s in steps if isinstance(s, tuple) and s[0] == "i"]
    data = bytes([0x02 if format1 else 0x00, max(pushed + [0]), len(steps) - 1,
                  len(constants)])
    for s in steps:
        if isinstance(s, tuple):
            data += bytes([(s[1] << 2) | (0 if s[0] == "i" else 1)])
        else:
            data += bytes([(s << 2) | 2])
    for c in constants:
        data += number_bytes(c)
    data += bytes([sum(1 << i for i, v in enumerate(inputs) if v is None)])
    for v in inputs:
        data += number_bytes(0.0 if v is None else v)
    return data


if __name__ == "__main__":
    sys.exit(main(sys.argv))
