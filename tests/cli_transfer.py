# File/layer: tests/cli_transfer.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliTransferRunTest.*): havne-cli import and
#          export (ADR-0054) from outside the process
#
# The modes are in three scripts: this one (the export side),
# cli_transfer_import.py and cli_transfer_interrupt.py, which share
# cli_transfer_kit.py.
#
#   python tests/cli_transfer.py roundtrip <havne-cli>
#   python tests/cli_transfer.py refusals <havne-cli>
#   python tests/cli_transfer.py tshark <havne-cli>
#   python3 tests/cli_transfer.py candump <havne-cli>          (Linux)
#   python tests/cli_transfer.py csv <havne-cli> <recording>
#   python tests/cli_transfer.py folder <havne-cli> <findings> <values>
#
# roundtrip: a candump log imported, its frames read through obd2 by view
# --frames, exported to pcapng and read back with the standard library's
# struct: one interface of LINKTYPE_CAN_SOCKETCAN in microseconds, each
# frame as it arrived at the log's time. refusals: a log of two buses with
# none named, and an export whose check finds a DID echoing the VIN, each
# exit 3 with core's words and nothing written. tshark: the export read by
# Wireshark's own tshark, its CAN frames and its ISO-TP reassembled; exit
# 77 (a skip) without tshark. candump: the simulated car (canned values)
# on vcan0 answering a direct obd2 session while candump -L logs the bus;
# the log imported and read through obd2 by view --frames holds each
# signal's values as the session read them; exit 77 without vcan0 or can-
# utils unless ECUIO_VCAN_REQUIRED=1. csv: a recording core wrote (the
# replay fixture under obd2) exported as CSV and read back with the
# standard library's csv module after its comment line: each row its
# signals.csv row, host_us as seconds and the key named by events.log's
# header, the key last; the same bytes twice; a range its rows alone.
# folder: copies of the findings fixture (its frames; raw, then scrubbed,
# which leaves its torn ISO-TP message out), a recording of values alone
# and a mock's recording made with a label; each copy's info, view or
# replay the source's, the mock's label in neither the copy's URI nor its
# name.

import csv
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from cli_transfer_kit import LOG, one, partial_left, run, same

UTC = 1700000000250000


def pcapng(path):
    """The section's comment, the interfaces' (linktype, tsresol) and each
    packet's (time, bytes, comment)"""
    with open(path, "rb") as f:
        b = f.read()
    at = 0
    comment = None
    interfaces = []
    packets = []
    while at < len(b):
        kind, length = struct.unpack_from("<II", b, at)
        if length < 12 or length % 4 or struct.unpack_from(
                "<I", b, at + length - 4)[0] != length:
            raise SystemExit("FAIL  a block's length is wrong")

        def options(o, end):
            out = {}
            while o + 4 <= end:
                code, n = struct.unpack_from("<HH", b, o)
                if code == 0:
                    break
                out[code] = b[o + 4:o + 4 + n]
                o += 4 + ((n + 3) & ~3)
            return out

        end = at + length - 4
        if kind == 0x0A0D0D0A:
            if struct.unpack_from("<IHH", b, at + 8) != (0x1A2B3C4D, 1, 0):
                raise SystemExit("FAIL  the section header is wrong")
            comment = options(at + 24, end).get(1, b"").decode()
        elif kind == 1:
            link = struct.unpack_from("<H", b, at + 8)[0]
            interfaces.append((link, options(at + 16, end).get(9)))
        elif kind == 6:
            _, hi, lo, cap, orig = struct.unpack_from("<IIIII", b, at + 8)
            data = b[at + 28:at + 28 + cap]
            opts = options(at + 28 + ((cap + 3) & ~3), end)
            packets.append(((hi << 32) | lo, data,
                            opts.get(1, b"").decode() or None))
        at += length
    return comment, interfaces, packets


def roundtrip(cli):
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "bus.log")
        with open(log, "w", newline="\n") as f:
            f.write(LOG)
        rec = os.path.join(d, "rec")
        imported = one(run(cli, ["import", "candump", log, rec]), "imported")
        if (imported["frames"], imported["interfaces"], imported["utc_start_us"],
                imported["duration_us"]) != (7, 1, UTC, 150000):
            raise SystemExit(f"FAIL  imported {imported}")
        view = run(cli, ["view", rec, "--frames", "--profile", "obd2",
                         "--signal", "RPM"])
        rpm = [l for l in view if l["type"] == "signal" and l["name"] == "RPM"]
        if len(rpm) != 1 or rpm[0]["samples"] != 2 or rpm[0]["lowest"] != 1000 \
                or rpm[0]["highest"] != 1726:
            raise SystemExit(f"FAIL  view --frames read {rpm}")
        out = os.path.join(d, "bus.pcapng")
        exported = one(run(cli, ["export", rec, out, "--format", "pcapng"]),
                       "exported")
        if (exported["frames"], exported["made_up"], exported["scrubbed"]) != \
                (7, 0, True):
            raise SystemExit(f"FAIL  exported {exported}")
        comment, interfaces, packets = pcapng(out)
        if interfaces != [(227, b"\x06")] or len(packets) != 7:
            raise SystemExit(f"FAIL  {interfaces}, {len(packets)} packets")
        times = [UTC, UTC + 4000, UTC + 100000, UTC + 104000, UTC + 104500,
                 UTC + 105000, UTC + 150000]
        for (t, data, c), want in zip(packets, times):
            if t != want or c is not None:
                raise SystemExit(f"FAIL  a packet at {t}, not {want} ({c})")
        can_id, n, fd = struct.unpack_from(">IBB", packets[1][1], 0)
        if (can_id, n, fd, packets[1][1][8:8 + n]) != \
                (0x7E8, 5, 0, bytes.fromhex("04410C1AF8")):
            raise SystemExit(f"FAIL  the reply reads {packets[1][1].hex()}")
        can_id, n, fd = struct.unpack_from(">IBB", packets[6][1], 0)
        if (can_id, n, fd, len(packets[6][1])) != (0x456, 10, 0x07, 72):
            raise SystemExit(f"FAIL  the FD frame reads {packets[6][1].hex()}")
        if "a scrubbed export" not in comment:
            raise SystemExit(f"FAIL  the section says {comment!r}")
        again = os.path.join(d, "again.pcapng")
        run(cli, ["export", rec, again, "--format", "pcapng"])
        with open(out, "rb") as a, open(again, "rb") as b:
            if a.read() != b.read():
                raise SystemExit("FAIL  two exports differ")
        part = os.path.join(d, "part.pcapng")
        cut = one(run(cli, ["export", rec, part, "--format", "pcapng",
                            "--from", "0.1", "--to", "0.105"]), "exported")
        if cut["frames"] != 4 or len(pcapng(part)[2]) != 4:
            raise SystemExit(f"FAIL  the range wrote {cut}")
    print("ok    imported, read through obd2, exported and read back")
    return 0


def refusals(cli):
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "two.log")
        with open(log, "w", newline="\n") as f:
            f.write("(1.000000) can0 100#01\n(1.100000) can1 200#02\n")
        rec = os.path.join(d, "rec")
        error = one(run(cli, ["import", "candump", log, rec], 3), "error")
        if error["detail"] != ("the log has several interfaces: name one of "
                               "can0 and can1") or os.path.exists(rec):
            raise SystemExit(f"FAIL  import said {error}")
        one(run(cli, ["import", "candump", log, rec, "--interface", "can1"]),
            "imported")

        vin = "ZZZFAKE11M0000001".encode().hex().upper()
        spaced = " ".join(vin[i:i + 2] for i in range(0, len(vin), 2))
        echo = os.path.join(d, "echo")
        os.makedirs(echo)
        with open(os.path.join(echo, "frames.csv"), "w", newline="\n") as f:
            f.write("host_us,timestamp_us,id,flags,data\n"
                    f"1000,1000,0x7E8,0x100,49 02 01 {spaced}\n"
                    f"2000,2000,0x7E8,0x100,62 D8 01 {spaced}\n")
        out = os.path.join(d, "echo.pcapng")
        lines = run(cli, ["export", echo, out, "--format", "pcapng"], 3)
        if one(lines, "export_failed")["class"] != "identification":
            raise SystemExit(f"FAIL  export said {lines}")
        if "ZZZFAKE" in json.dumps(lines) or os.path.exists(out) or \
                partial_left(out):
            raise SystemExit("FAIL  the failed export left the VIN or a file")
        raw = one(run(cli, ["export", echo, out, "--format", "pcapng", "--raw"]),
                  "exported")
        if raw["scrubbed"] or raw["frames"] != 8:
            raise SystemExit(f"FAIL  the raw export said {raw}")
    print("ok    a log of two buses and a check that finds the VIN refused")
    return 0


def tshark(cli):
    exe = shutil.which("tshark")
    if exe is None:
        print("skip  no tshark")
        return 77
    with tempfile.TemporaryDirectory() as d:
        rec = os.path.join(d, "rec")
        os.makedirs(rec)
        message = "62 D8 01 " + " ".join("%02X" % i for i in range(17))
        with open(os.path.join(rec, "frames.csv"), "w", newline="\n") as f:
            f.write("host_us,timestamp_us,id,flags,data\n"
                    "1000,1000,0x7E8,0x0,04 41 0C 1A F8\n"
                    f"2000,2000,0x7E8,0x100,{message}\n")
        out = os.path.join(d, "out.pcapng")
        run(cli, ["export", rec, out, "--format", "pcapng"])
        done = subprocess.run(
            [exe, "-r", out, "-T", "fields", "-e", "can.id", "-e", "can.len",
             "-e", "frame.comment"], capture_output=True, text=True)
        rows = [r.split("\t") for r in done.stdout.splitlines()]
        ids = [int(r[0], 0) for r in rows]
        if done.returncode != 0 or ids != [0x7E8, 0x7E8, 0x7E0, 0x7E8, 0x7E8]:
            raise SystemExit(f"FAIL  tshark read {rows} {done.stderr}")
        if any("made up by Havne" not in r[2] for r in rows[1:]):
            raise SystemExit(f"FAIL  tshark's comments {rows}")
        # ISO 15765-4's IDs bound to the ISO-TP dissector: the Single
        # Frame's payload, and the made-up frames reassembled to the
        # message as stored
        iso = subprocess.run(
            [exe, "-r", out, "-o", "iso15765.can.ids:0x7e0-0x7ef", "-T",
             "fields", "-E", "separator=/t", "-e", "iso15765.message_type",
             "-e", "iso15765.reassembled.length", "-e",
             "iso15765.fragment.count", "-e", "data.data"],
            capture_output=True, text=True)
        rows = [r.split("\t") for r in iso.stdout.splitlines()]
        want = bytes.fromhex(message.replace(" ", "")).hex()
        if iso.returncode != 0 or len(rows) != 5 or \
                [r[0] for r in rows] != ["0x00", "0x01", "0x03", "0x02", "0x02"] \
                or rows[0][3] != "410c1af8" or rows[4][1:] != ["20", "3", want]:
            raise SystemExit(f"FAIL  tshark's ISO-TP read {rows} {iso.stderr}")
    print("ok    tshark reads the export, its ISO-TP reassembled")
    return 0


def candump(cli):
    vcan = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    dump = shutil.which("candump")
    if not os.path.exists("/sys/class/net/" + vcan) or dump is None:
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print(f"no {vcan} or no candump, and ECUIO_VCAN_REQUIRED=1")
            return 1
        print(f"skip  no {vcan} or no candump (can-utils)")
        return 77
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "bus.log")
        ecu = subprocess.Popen([cli, "sim", "mock://transfer?values=canned",
                                "--on", vcan, "--seconds", "30"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with open(log, "w") as f:
            listener = subprocess.Popen([dump, "-L", vcan], stdout=f)
        try:
            import time
            time.sleep(0.5)
            direct = run(cli, ["stream", "socketcan://%s?profile=obd2"
                               "&identify=0&diagnose=0" % vcan, "--values",
                               "--seconds", "4"])
            time.sleep(0.2)
        finally:
            listener.terminate()
            listener.wait(timeout=10)
            ecu.terminate()
            ecu.wait(timeout=10)
        names = {s["id"]: s["name"]
                 for s in one(direct, "signals")["signals"]}
        seen = {}
        for line in direct:
            if line["type"] == "values":
                for v in line["values"]:
                    seen.setdefault(names[v["id"]], []).append(v["value"])
        rec = os.path.join(d, "rec")
        imported = one(run(cli, ["import", "candump", log, rec]), "imported")
        view = run(cli, ["view", rec, "--frames", "--profile", "obd2"] +
                   sum((["--signal", n] for n in sorted(seen)), []))
        got = {l["name"]: l for l in view
               if l["type"] == "signal" and l["samples"] > 0}
        if len(seen) < 5 or set(got) != set(seen):
            raise SystemExit(f"FAIL  the direct session read {sorted(seen)}, "
                             f"the import {sorted(got)}")
        for name, values in seen.items():
            low, high = got[name]["lowest"], got[name]["highest"]
            if abs(low - min(values)) > 1e-9 * max(1, abs(low)) or \
                    abs(high - max(values)) > 1e-9 * max(1, abs(high)):
                raise SystemExit(f"FAIL  {name}: the direct session read "
                                 f"{min(values)} to {max(values)}, the "
                                 f"import {low} to {high}")
    print(f"ok    candump -L on {vcan}: {imported['frames']} frames imported, "
          f"{len(seen)} signals equal to the direct session's")
    return 0


def csv_rows(path):
    """The comment line, then the rows under the header"""
    with open(path, encoding="utf-8", newline="") as f:
        comment = f.readline()
        rows = list(csv.reader(f))
    if rows[0] != ["time_s", "signal", "unit", "value", "key"]:
        raise SystemExit(f"FAIL  the header reads {rows[0]}")
    return comment, rows[1:]


def csv_export(cli, recording):
    with open(os.path.join(recording, "signals.csv"), encoding="utf-8-sig",
              newline="") as f:
        recorded = list(csv.reader(f))[1:]
    # signals.csv keeps each value under its key, which events.log's header
    # names; the export writes the name and the key
    with open(os.path.join(recording, "events.log"), encoding="utf-8") as f:
        name_of = {cells[0]: cells[1] for cells in csv.reader(
            l[len("signal  "):] for l in f.read().splitlines()
            if l.startswith("signal  "))}
    want = [[f"{int(t) // 1000000}.{int(t) % 1000000:06d}", name_of[key],
             unit, value, key]
            for t, key, value, unit in recorded]
    # --estimate's bytes are the wide file's, at a sweep and on a grid
    with tempfile.TemporaryDirectory() as d:
        sized = one(run(cli, ["export", recording, "--format", "csv",
                              "--estimate"]), "estimate")
        for size in sized["sizes"]:
            out = os.path.join(d, f"wide-{size['row_ms']}.csv")
            rows = "sweep" if size["row_ms"] == 0 else str(size["row_ms"])
            wide = one(run(cli, ["export", recording, out, "--format", "csv",
                                 "--rows", rows]), "exported")
            if (os.path.getsize(out), wide["rows"]) != (size["bytes"], size["rows"]):
                raise SystemExit(f"FAIL  estimate {size} of {wide}")
        typo = one(run(cli, ["export", recording, "--format", "csv", "--estimate",
                             "--rows", "100", "--signal", "No such signal"]),
                   "estimate")
        if (typo["missing"], [s["row_ms"] for s in typo["sizes"]]) != (1, [100]):
            raise SystemExit(f"FAIL  estimate of a typo {typo}")
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "values.csv")
        exported = one(run(cli, ["export", recording, out, "--format", "csv",
                                 "--layout", "long"]), "exported")
        if (exported["layout"], exported["row_ms"]) != ("long", 0):
            raise SystemExit(f"FAIL  exported {exported}")
        names = {r[4] for r in want}  # the signals, one per key
        if (exported["values"], exported["signals"], exported["frames"],
                exported["scrubbed"]) != (len(want), len(names), 0, True):
            raise SystemExit(f"FAIL  exported {exported}")
        comment, rows = csv_rows(out)
        if not comment.startswith("# Havne ") or                 f"{len(want)} values of {len(names)} signals" not in comment:
            raise SystemExit(f"FAIL  the comment reads {comment!r}")
        for got, row in zip(rows, want):
            if got != row:
                raise SystemExit(f"FAIL  a row reads {got}, not {row}")
        if len(rows) != len(want):
            raise SystemExit(f"FAIL  {len(rows)} rows, not {len(want)}")
        again = os.path.join(d, "again.csv")
        run(cli, ["export", recording, again, "--format", "csv", "--layout",
                  "long"])
        with open(out, "rb") as a, open(again, "rb") as b:
            if a.read() != b.read():
                raise SystemExit("FAIL  two exports differ")
        part = os.path.join(d, "part.csv")
        cut = one(run(cli, ["export", recording, part, "--format", "csv",
                            "--layout", "long", "--from", "10", "--to", "20"]),
                  "exported")
        inside = [r for r in want if 10.0 <= float(r[0]) <= 20.0]
        if cut["values"] != len(inside) or csv_rows(part)[1] != inside:
            raise SystemExit(f"FAIL  the range wrote {cut}")
        # The wide layout, the default (ADR-0056 section 5): Time (ms)
        # then a column a number signal, each cell a number or empty,
        # the times whole ms ascending; a grid thins the sweeps; named
        # columns come in their order
        wide = os.path.join(d, "wide.csv")
        shaped = one(run(cli, ["export", recording, wide, "--format", "csv"]),
                     "exported")
        head, rows = wide_rows(wide)
        if head[0] != "Time (ms)" or len(head) != shaped["signals"] + 1:
            raise SystemExit(f"FAIL  the wide header reads {head}")
        if (shaped["layout"], shaped["row_ms"], shaped["rows"]) != \
                ("wide", 0, len(rows)):
            raise SystemExit(f"FAIL  wide exported {shaped}, {len(rows)} rows")
        cells = sum(1 for r in rows for c in r[1:] if c != "")
        if shaped["values"] != cells or not rows:
            raise SystemExit(f"FAIL  {cells} cells against {shaped}")
        grid = os.path.join(d, "grid.csv")
        gridded = one(run(cli, ["export", recording, grid, "--format", "csv",
                                "--rows", "1000"]), "exported")
        ghead, grows = wide_rows(grid)
        if gridded["row_ms"] != 1000 or len(grows) > len(rows) or \
                any(int(r[0]) % 1000 for r in grows):
            raise SystemExit(f"FAIL  the grid wrote {gridded}")
        picked = os.path.join(d, "picked.csv")
        named = one(run(cli, ["export", recording, picked, "--format", "csv",
                              "--signal", head[2].split(" (")[0],
                              "--signal", head[1].split(" (")[0]]), "exported")
        phead, _ = wide_rows(picked)
        if phead != [head[0], head[2], head[1]] or named["signals"] != 2 or \
                named["missing"] != 0:
            raise SystemExit(f"FAIL  the named columns read {phead}")
        # A named column the recording lacks: an empty column, a stderr
        # line naming it, the exported line counting it, exit 0
        typo = os.path.join(d, "typo.csv")
        done = subprocess.run([cli, "export", recording, typo, "--format",
                               "csv", "--signal", head[1].split(" (")[0],
                               "--signal", "Nothing here"],
                              capture_output=True, stdin=subprocess.DEVNULL)
        err = done.stderr.decode("utf-8", "replace")
        if done.returncode != 0 or "'Nothing here'" not in err or \
                "empty" not in err:
            raise SystemExit(f"FAIL  the typo's run: exit {done.returncode}, "
                             f"stderr {err!r}")
        typo_line = one([json.loads(l) for l in
                         done.stdout.decode("utf-8").splitlines()], "exported")
        thead, trows = wide_rows(typo)
        if thead != [head[0], head[1], "Nothing here"] or \
                typo_line["missing"] != 1 or typo_line["signals"] != 2 or \
                any(r[2] != "" for r in trows):
            raise SystemExit(f"FAIL  the typo's column: {thead}, {typo_line}")
    print(f"ok    {len(want)} values of {len(names)} signals exported as CSV "
          f"long and read back; wide {len(rows)} rows, grid {len(grows)}")
    return 0


def wide_rows(path):
    """The wide CSV: its header cells and its rows, each cell a number or
    empty and the times whole ms ascending"""
    with open(path, encoding="utf-8", newline="") as f:
        raw = f.read()
    if "\r\n" not in raw or "\n" in raw.replace("\r\n", ""):
        raise SystemExit("FAIL  the wide CSV isn't CRLF throughout")
    rows = list(csv.reader(raw.splitlines()))
    head, body = rows[0], rows[1:]
    last = -1
    for r in body:
        if len(r) != len(head) or not r[0].isdigit() or int(r[0]) < last:
            raise SystemExit(f"FAIL  a wide row reads {r}")
        last = int(r[0])
        for c in r[1:]:
            if c != "":
                float(c)
    return head, body


def replayed(cli, folder):
    """The frame lines and the end line of the folder's replay at max"""
    uri = f"replay://{os.path.abspath(folder).replace(os.sep, '/')}?speed=max"
    lines = run(cli, ["stream", uri])
    end = one(lines, "end")
    return [l for l in lines if l["type"] == "frame"], end["frames"]


def folder_copy(cli, source, out, want_files, raw=False):
    exported = one(run(cli, ["export", source, out, "--format", "folder"] +
                       (["--raw"] if raw else [])), "exported")
    if (exported["format"], exported["scrubbed"], exported["files"],
            exported["files_left_out"]) != ("folder", not raw, want_files, 0):
        raise SystemExit(f"FAIL  exported {exported}")
    if partial_left(out):
        raise SystemExit("FAIL  the partial folder is left")
    return exported


def folder_export(cli, findings, values):
    with tempfile.TemporaryDirectory() as d:
        # The findings fixture: a raw copy's info and replay are the
        # source's; the scrubbed copy's too, less the torn ISO-TP message
        # on the reply ID, the one frame it leaves out as unattributable
        source = run(cli, ["info", findings])
        raw = os.path.join(d, "20261103-100000-raw")
        folder_copy(cli, findings, raw, 4, raw=True)
        same("the raw copy's info", source, run(cli, ["info", raw]))
        frames, n = replayed(cli, raw)
        same("the raw copy's replay", replayed(cli, findings), (frames, n))
        copy = os.path.join(d, "20261103-100000")
        scrubbed = folder_copy(cli, findings, copy, 4)
        if (scrubbed["frames"], scrubbed["left_out"]["unattributable"]) != (
                n - 1, 1):
            raise SystemExit(f"FAIL  the scrubbed copy exported {scrubbed}")
        want = [l for l in source if l.get("kind") != "isotp"]
        summary = one(want, "summary")
        summary["frames"] -= 1
        summary["findings"]["isotp"] = 0
        same("the scrubbed copy's info", want, run(cli, ["info", copy]))
        # Values alone: view reads the copy as the source
        vcopy = os.path.join(d, "20261102-221027")
        folder_copy(cli, values, vcopy, 2)
        same("view", run(cli, ["view", values, "--columns", "20"]),
             run(cli, ["view", vcopy, "--columns", "20"]))
        # A mock's recording, its label in the URI and the folder's name:
        # neither in the copy, which replays and reads as the source
        root = os.path.join(d, "recordings")
        run(cli, ["stream", "mock://Daily driver", "--count", "40",
                  "--record", root, "--label", "Daily driver"])
        [name] = os.listdir(root)
        mock = os.path.join(root, name)
        mcopy = os.path.join(d, name.split(" ")[0])
        folder_copy(cli, mock, mcopy, 3)
        with open(os.path.join(mcopy, "events.log"), encoding="utf-8") as f:
            events = f.read()
        if "\nuri  mock://<label>\n" not in events or "Daily" in events:
            raise SystemExit(f"FAIL  the copy's events.log reads {events}")
        mframes, mn = replayed(cli, mcopy)
        same("the mock's replay", replayed(cli, mock), (mframes, mn))
        same("the mock's info", run(cli, ["info", mock]),
             run(cli, ["info", mcopy]))
    print(f"ok    copies of three recordings ({n}, {mn} frames and values "
          "alone) read and replay as their sources")
    return 0




if __name__ == "__main__":
    modes = {"roundtrip": roundtrip, "refusals": refusals, "tshark": tshark,
             "candump": candump}
    if len(sys.argv) == 4 and sys.argv[1] == "csv":
        sys.exit(csv_export(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 5 and sys.argv[1] == "folder":
        sys.exit(folder_export(sys.argv[2], sys.argv[3], sys.argv[4]))
    if len(sys.argv) != 3 or sys.argv[1] not in modes:
        print(__doc__ or "usage: cli_transfer.py roundtrip|refusals|tshark|"
              "candump <havne-cli>, csv <havne-cli> <recording>, "
              "folder <havne-cli> <findings> <values>")
        sys.exit(2)
    sys.exit(modes[sys.argv[1]](sys.argv[2]))
