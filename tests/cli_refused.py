# File/layer: tests/cli_refused.py | test-only, Python 3.8+
# Testing: HARNESS: CliRefusedRunTest (ctest)
#
#   python tests/cli_refused.py fake <havne-cli> <fake_j2534.dll>   (Windows)
#   python3 tests/cli_refused.py sim <havne-cli>                    (Linux)
#
# A DID its ECU refuses is named in a `refused` line, with the code by
# name (ADR-0051 section 3), and one it answers takes values and isn't.
# fake: the fake DLL's ECU answers 1E1C and refuses F1FE (7F 22 31).
# sim: the simulated ECU (havne-cli sim, no profile) on vcan0 refuses
# every DID (7F 22 31); skips (77) without the interface unless
# ECUIO_VCAN_REQUIRED=1, as test_vcan does.

import json
import os
import subprocess
import sys
import time

SKIP = 77
REFUSED = {"request": "22 F1 FE", "reply": "7F 22 31", "nrc": "requestOutOfRange"}


def stream(cli, uri):
    run = subprocess.run([cli, "stream", uri, "--values", "--seconds", "3"],
                         capture_output=True, text=True, timeout=60)
    lines = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
    return run, lines


def ids_by_name(lines):
    signals = [l for l in lines if l.get("type") == "signals"]
    if len(signals) != 1:
        return {}
    return {s["name"]: s["id"] for s in signals[0]["signals"]}


def check(run, lines, refused_name, answered_name=None):
    why = []
    if run.returncode != 0:
        why.append("exit %d: %s" % (run.returncode, run.stderr.strip()))
    ids = ids_by_name(lines)
    refused = [l for l in lines if l.get("type") == "refused"]
    if refused_name not in ids:
        why.append("no %s in the signals line" % refused_name)
    elif not refused:
        why.append("no refused line")
    else:
        last = {s["id"]: s for s in refused[-1]["signals"]}
        got = last.get(ids[refused_name])
        want = dict(REFUSED, id=ids[refused_name])
        if got != want:
            why.append("%s refused as %r, not %r" % (refused_name, got, want))
        if answered_name is not None and ids.get(answered_name) in last:
            why.append("%s refused too" % answered_name)
    if answered_name is not None:
        values = [v for l in lines if l.get("type") == "values" for v in l.get("values", [])]
        if not any(v.get("id") == ids.get(answered_name) for v in values):
            why.append("%s never took a value" % answered_name)
    for w in why:
        print(w)
    if why:
        print("\n".join(json.dumps(l) for l in lines[:12]))
    return 1 if why else 0


def fake(cli, dll):
    run, lines = stream(cli, "j2534://%s?profile=uds-generic&did=1E1C:u16,F1FE:u16"
                             "&identify=0&diagnose=0" % dll)
    return check(run, lines, "DID F1FE", "DID 1E1C")


def sim(cli):
    vcan = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    if not os.path.exists("/sys/class/net/" + vcan):
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print("no %s, and ECUIO_VCAN_REQUIRED=1" % vcan)
            return 1
        print("skipped: no %s (the linux skill's vcan brings it up)" % vcan)
        return SKIP
    ecu = subprocess.Popen([cli, "sim", "mock://refuses", "--on", vcan, "--seconds", "20"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        run, lines = stream(cli, "socketcan://%s?profile=uds-generic&did=F1FE:u16"
                                 "&identify=0&diagnose=0" % vcan)
    finally:
        ecu.terminate()
        ecu.wait(timeout=10)
    return check(run, lines, "DID F1FE")


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "fake":
        return fake(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 3 and sys.argv[1] == "sim":
        return sim(sys.argv[2])
    print(__doc__ or "usage: cli_refused.py fake <cli> <dll> | sim <cli>")
    return 2


if __name__ == "__main__":
    sys.exit(main())
