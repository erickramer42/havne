# File/layer: tests/cli_health.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliHealthRunTest.*): havne-cli health (ADR-0054's
#          appendix "the health report as built") from outside the process
#
#   python tests/cli_health.py session <havne-cli>
#   python tests/cli_health.py recording <havne-cli> <values-only recording>
#
# session: the mock's report as text (its label left out, the file written
# once, a second run refused with core's words); a uds-generic mock with
# diagnostics off, whose reports are never asked, ending well before its
# --seconds; and the simulated car as an ELM327 adapter on loopback (sim
# --elm327 tcp:0), read through obd2 to JSON: its VIN decoded and in
# neither the file nor the lines, its ranked values and since-cleared
# PIDs given; and the same car answering late (faults=late:1) through
# uds-generic, whose identification outlasts the values' quiet: the
# report waits for it, no item left unasked. recording: a recording of
# values alone to JSON (no ECU and why, its key values), and a range
# that starts after it ends refused.

import json
import os
import subprocess
import sys
import tempfile
import time

SIM_VIN = "ZZZFAKE11M0000001"


def run(cli, args, want=0):
    started = time.monotonic()
    done = subprocess.run([cli] + args, capture_output=True)
    took = time.monotonic() - started
    out = done.stdout.decode("utf-8")
    lines = [json.loads(l) for l in out.splitlines()]
    if done.returncode != want:
        raise SystemExit(f"FAIL  {' '.join(args)} exited {done.returncode}, "
                         f"not {want}: {lines} {done.stderr.decode()}")
    return lines, out, took


def one(lines, kind):
    found = [l for l in lines if l["type"] == kind]
    if len(found) != 1:
        raise SystemExit(f"FAIL  {len(found)} {kind} lines in {lines}")
    return found[0]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def on_emulator(cli, sim_uri, uri, out):
    """havne-cli health over sim <sim_uri> --elm327 tcp:0, PORT in uri the
    emulator's port: health's lines, its stdout and how long it took"""
    sim = subprocess.Popen([cli, "sim", sim_uri, "--elm327", "tcp:0",
                            "--seconds", "100"], stdout=subprocess.PIPE,
                           text=True)
    try:
        port = None
        for line in sim.stdout:
            j = json.loads(line)
            if j["type"] == "elm327":
                port = j["listen"].rsplit(":", 1)[1]
                break
            if j["type"] == "error":
                raise SystemExit(f"FAIL  the emulator didn't start: {j}")
        if port is None:
            raise SystemExit("FAIL  the emulator wrote no elm327 line")
        return run(cli, ["health", uri.replace("PORT", port), out,
                         "--format", "json"])
    finally:
        sim.terminate()
        sim.communicate(timeout=60)


def session(cli):
    with tempfile.TemporaryDirectory() as d:
        txt = os.path.join(d, "mock.txt")
        lines, _, _ = run(cli, ["health", "mock://Daily driver", txt,
                                "--format", "text"])
        h = one(lines, "health")
        if h["source"] != "session" or not h["settled"] or h["ecus"] != 1 \
                or h["codes"] == 0 or h["vin_decoded"]:
            raise SystemExit(f"FAIL  the mock's health line {h}")
        report = read(txt)
        if "mock://<label>" not in report or "Daily" in report:
            raise SystemExit(f"FAIL  the mock's report reads {report}")
        lines, _, _ = run(cli, ["health", "mock://Daily driver", txt,
                                "--format", "text"], want=3)
        if one(lines, "error")["detail"] != "the file exists already":
            raise SystemExit(f"FAIL  a second report wrote {lines}")

        # Diagnostics never asked: the wait doesn't sit out --seconds
        uds = os.path.join(d, "uds.json")
        lines, _, took = run(cli, [
            "health", "mock://x?profile=uds-generic&did=7E8:F190&diagnose=0",
            uds, "--format", "json", "--seconds", "60"])
        if not one(lines, "health")["settled"] or took > 20:
            raise SystemExit(f"FAIL  the unasked reports took {took:.1f} s: "
                             f"{lines}")
        ecu = json.loads(read(uds))["ecus"][0]
        if ecu["state"] != "not asked":
            raise SystemExit(f"FAIL  the unasked ECU reads {ecu}")

        # The simulated car as an ELM327 adapter on loopback
        car = os.path.join(d, "car.json")
        lines, out, took = on_emulator(
            cli, "mock://sim?values=canned",
            "elm327://127.0.0.1:PORT?profile=obd2", car)
        h = one(lines, "health")
        report = read(car)
        if SIM_VIN in report or SIM_VIN in out:
            raise SystemExit("FAIL  the VIN is in the report or its lines")
        v = json.loads(report)
        if not h["vin_decoded"] or not h["settled"] or h["values"] == 0 \
                or h["since_cleared"] != 5 or took > 30:
            raise SystemExit(f"FAIL  the car's health line {h} in {took:.1f} s")
        if not v["vehicle"]["vin_read"] or not v["key_values"] or \
                v["source"]["uri"] != "elm327://<address>?profile=obd2":
            raise SystemExit(f"FAIL  the car's report {v}")

        # Late replies through uds-generic: nothing ranked, so the values
        # are quiet at once, and the identification takes longer
        late = os.path.join(d, "late.json")
        lines, _, late_took = on_emulator(
            cli, "mock://sim?faults=late:1",
            "elm327://127.0.0.1:PORT?profile=uds-generic&did=7E8:F190", late)
        items = [i for e in json.loads(read(late))["ecus"]
                 for i in e["identification"]]
        if not one(lines, "health")["settled"] or not items or \
                any(i["outcome"] == "unasked" for i in items):
            raise SystemExit(f"FAIL  a late car's report in {late_took:.1f} "
                             f"s: {items}")
    print(f"ok    the mock's, an unasked session's and the simulated car's "
          f"reports ({h['values']} values, {took:.1f} s)")
    return 0


def recording(cli, values_only):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "rec.json")
        lines, _, _ = run(cli, ["health", values_only, out, "--format", "json"])
        h = one(lines, "health")
        v = json.loads(read(out))
        if h["source"] != "recording" or h["ecus"] != 0 or h["values"] == 0 \
                or v["ecus_note"] != "the recording has no frames" or \
                v["source"]["kind"] != "recording":
            raise SystemExit(f"FAIL  the recording's report {h} {v}")
        past = os.path.join(d, "past.json")
        lines, _, _ = run(cli, ["health", values_only, past, "--format",
                                "json", "--from", "100000"], want=3)
        detail = one(lines, "error")["detail"]
        if not detail.startswith("the range starts at 100000 s, after the "
                                 "recording ends at ") or os.path.exists(past):
            raise SystemExit(f"FAIL  a range past the end: {lines}")
    print(f"ok    a recording of values alone ({h['values']} values) and a "
          "range past its end")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "session":
        sys.exit(session(sys.argv[2]))
    if len(sys.argv) == 4 and sys.argv[1] == "recording":
        sys.exit(recording(sys.argv[2], sys.argv[3]))
    print(__doc__ or "usage: cli_health.py session|recording <havne-cli> ...")
    sys.exit(2)
