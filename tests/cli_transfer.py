# File/layer: tests/cli_transfer.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliTransferRunTest.*): havne-cli import and
#          export (ADR-0054) from outside the process
#
#   python tests/cli_transfer.py roundtrip <havne-cli>
#   python tests/cli_transfer.py refusals <havne-cli>
#   python tests/cli_transfer.py tshark <havne-cli>
#   python3 tests/cli_transfer.py candump <havne-cli>          (Linux)
#
# roundtrip: a candump log imported, its frames read through obd2 by view
# --frames, exported to pcapng and read back with the standard library's
# struct: one interface of LINKTYPE_CAN_SOCKETCAN in microseconds, each
# frame as it arrived at the log's time. refusals: a log of two buses
# with none named, and an export whose check finds a DID echoing the
# VIN, each exit 3 with core's words and nothing written. tshark: the
# export read by Wireshark's own tshark, its CAN frames and its ISO-TP
# reassembled; exit 77 (a skip) without tshark. candump: the simulated
# car (canned values) on vcan0 answering a direct obd2 session while
# candump -L logs the bus; the log imported and read through obd2 by
# view --frames holds each signal's values as the session read them;
# exit 77 without vcan0 or can-utils unless ECUIO_VCAN_REQUIRED=1.

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

LOG = (
    "(1700000000.250000) can0 7DF#0201000000000000\n"
    "(1700000000.254000) can0 7E8#04410C1AF8\n"
    "(1700000000.350000) can0 7E0#05010C0D0504\n"
    "(1700000000.354000) can0 7E8#100A410C0FA00D32\n"
    "(1700000000.354500) can0 7E0#3000000000000000\n"
    "(1700000000.355000) can0 7E8#21055A0480\n"
    "(1700000000.400000) can0 456##3AABBCCDDEEFF00112233\n"
)
UTC = 1700000000250000


def run(cli, args, want=0):
    done = subprocess.run([cli] + args, capture_output=True)
    lines = [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()]
    if done.returncode != want:
        raise SystemExit(f"FAIL  {' '.join(args)} exited {done.returncode}, "
                         f"not {want}: {lines} {done.stderr.decode()}")
    return lines


def one(lines, kind):
    found = [l for l in lines if l["type"] == kind]
    if len(found) != 1:
        raise SystemExit(f"FAIL  {len(found)} {kind} lines in {lines}")
    return found[0]


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
                os.path.exists(out + ".partial"):
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


if __name__ == "__main__":
    modes = {"roundtrip": roundtrip, "refusals": refusals, "tshark": tshark,
             "candump": candump}
    if len(sys.argv) != 3 or sys.argv[1] not in modes:
        print(__doc__ or "usage: cli_transfer.py roundtrip|refusals|tshark "
              "<havne-cli>")
        sys.exit(2)
    sys.exit(modes[sys.argv[1]](sys.argv[2]))
