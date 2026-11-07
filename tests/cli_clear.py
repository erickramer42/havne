# File/layer: tests/cli_clear.py | test-only, Python 3.8+
# Testing: HARNESS: CliClearRunTest (ctest)
#
#   python tests/cli_clear.py <case> <havne-cli> <fake_j2534.dll>   (Windows)
#   python3 tests/cli_clear.py TheSimulatedEcuIsCleared <havne-cli>  (Linux)
#
# havne-cli clear through core's gate (ADR-0038, its appendix of
# 2026-11-06) against the fake DLL's ECU, its knobs in the environment
# the surrogate inherits: the report comes first (every diagnostics line
# before the clear line), then the clear's outcome, then, after a 44, the
# report read again with its codes gone; the exit is 0 only when the ECU
# cleared. The fake's engine turns at 2000 rpm unless ECUIO_FAKE_ENGINE_OFF;
# ECUIO_FAKE_CLEAR_NRC refuses 04; ECUIO_FAKE_SPEED_KMH moves the vehicle.
# On Linux the simulated ECU (havne-cli sim on vcan0, its key on at 0 rpm)
# is cleared; skips (77) without the interface unless ECUIO_VCAN_REQUIRED=1.

import json
import os
import subprocess
import sys
import time

SKIP = 77


def clear(cli, uri, flags=(), env=None):
    run = subprocess.run([cli, "clear", uri, "--yes"] + list(flags),
                         capture_output=True, text=True, timeout=80, env=env)
    lines = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
    return run, lines


def expect_run(run, lines, outcome, exit_code, rpm, kmh, reply=None):
    why = []
    if run.returncode != exit_code:
        why.append("exit %d, not %d: %s" % (run.returncode, exit_code, run.stderr.strip()))
    types = [l["type"] for l in lines]
    if "clear" not in types:
        why.append("no clear line: %r" % types)
    else:
        at = types.index("clear")
        first = [t for t in types[:at] if t == "diagnostics"]
        if len(first) != 1:
            why.append("the report isn't written before the clear: %r" % types)
        c = lines[at]
        want = {"type": "clear", "rx": "0x7E8", "outcome": outcome, "rpm": rpm, "kmh": kmh}
        if reply is not None:
            want["reply"] = reply
        if c != want:
            why.append("clear line %r, not %r" % (c, want))
        after = [l for l in lines[at + 1:] if l["type"] == "diagnostics"]
        if outcome == "cleared":
            if len(after) != 1 or after[0]["stored"]["codes"] != []:
                why.append("no cleared report read after: %r" % after)
            if lines[types.index("diagnostics")]["stored"]["codes"] == []:
                why.append("the report before held no codes")
        elif after:
            why.append("a report read after %s: %r" % (outcome, after))
    end = [l for l in lines if l["type"] == "end"]
    want_end = {"type": "end", "reason": "cleared" if outcome == "cleared" else "not-cleared",
                "cleared": 1 if outcome == "cleared" else 0,
                "not_cleared": 0 if outcome == "cleared" else 1}
    if end != [want_end]:
        why.append("end %r, not %r" % (end, want_end))
    for w in why:
        print(w)
    return 1 if why else 0


def fake_case(case, cli, dll):
    uri = "j2534://%s?identify=0" % dll
    knobs = {k: v for k, v in os.environ.items() if not k.startswith("ECUIO_FAKE_")}
    if case == "EngineRunningIsNotCleared":
        run, lines = clear(cli, uri, env=knobs)
        return expect_run(run, lines, "engine-running", 1, 2000, None)
    if case == "EngineOffIsCleared":
        run, lines = clear(cli, uri, env=dict(knobs, ECUIO_FAKE_ENGINE_OFF="1"))
        return expect_run(run, lines, "cleared", 0, 0, None)
    if case == "ARefusalIsNamed":
        run, lines = clear(cli, uri, env=dict(knobs, ECUIO_FAKE_ENGINE_OFF="1",
                                             ECUIO_FAKE_CLEAR_NRC="0x22"))
        return expect_run(run, lines, "refused", 1, 0, None, reply="7F 04 22")
    if case == "MovingIsNotCleared":
        run, lines = clear(cli, uri, ["--engine-running"],
                           env=dict(knobs, ECUIO_FAKE_SPEED_KMH="30"))
        return expect_run(run, lines, "moving", 1, 2000, 30)
    if case == "TheWordOnMovingClears":
        run, lines = clear(cli, uri, ["--engine-running", "--moving"],
                           env=dict(knobs, ECUIO_FAKE_SPEED_KMH="30"))
        return expect_run(run, lines, "cleared", 0, 2000, 30)
    raise SystemExit("no case %s" % case)


def vcan_up(name):
    try:
        with open("/sys/class/net/%s/operstate" % name) as f:
            return f.read().strip() in ("up", "unknown")
    except OSError:
        return False


def sim_case(cli):
    ifname = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    if not vcan_up(ifname):
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print("%s isn't up" % ifname)
            return 1
        print("%s isn't up: skipped" % ifname)
        return SKIP
    sim = subprocess.Popen([cli, "sim", "mock://sim", "--on", ifname, "--seconds", "60"],
                           stdout=subprocess.PIPE, text=True)
    try:
        for line in sim.stdout:
            if json.loads(line)["type"] in ("sim", "error"):
                break
        time.sleep(0.2)
        run, lines = clear(cli, "socketcan://%s?profile=obd2&identify=0" % ifname)
        return expect_run(run, lines, "cleared", 0, 0, None)
    finally:
        sim.terminate()
        sim.wait(timeout=10)


def main():
    case, cli = sys.argv[1], sys.argv[2]
    if case == "TheSimulatedEcuIsCleared":
        rc = sim_case(cli)
    else:
        rc = fake_case(case, cli, sys.argv[3])
    print("ok" if rc == 0 else ("skipped" if rc == SKIP else "failed"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
