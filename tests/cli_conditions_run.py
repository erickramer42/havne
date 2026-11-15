# File/layer: tests/cli_conditions_run.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliConditionsRunTest.*): havne-cli stream and log
#          with --conditions (ADR-0063)
#
#   python tests/cli_conditions_run.py unread <havne-cli> <fixtures/conditions>
#   python tests/cli_conditions_run.py log <havne-cli> <fixtures/recording/conditions>
#
# unread: stream and log of mock://demo?profile=obd2, whose mock sends the
# demo car's PIDs and disclaims PID 0A, with unread.json: the conditions
# line at the attach says the PID's condition available, and a later one
# says it isn't received; the link's (the mock has no link) reads no_link
# on both. log: the conditions fixture replayed by log with its own
# conditions.json named: one conditions line and no condition line, and
# the recording holding the copy and every event the fixture's own cond
# lines hold.

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


def lines_of(r):
    return [json.loads(l) for l in r.stdout.decode().splitlines() if l.strip()]


def run(cli, args):
    r = subprocess.run([cli] + args, capture_output=True, timeout=60)
    return r, lines_of(r)


def availability(line):
    return {c["key"]: (c["available"], c["why"]) for c in line["conditions"]}


def unread(cli, fixtures):
    ok = True
    named = os.path.join(fixtures, "unread.json")
    uri = "mock://demo?profile=obd2"
    root = tempfile.mkdtemp(prefix="havne-cli-unread-")
    try:
        runs = [
            ("stream", ["stream", uri, "--conditions", named, "--seconds", "2"]),
            ("log", ["log", uri, root, "--conditions", named, "--seconds", "2"]),
        ]
        for name, args in runs:
            r, lines = run(cli, args)
            shown = [l for l in lines if l.get("type") == "conditions"]
            # The attach's line and one more as the session starts: the
            # mock's report, published at the connect, never changes
            if r.returncode != 0 or len(shown) != 2:
                print(f"FAIL  {name}: exit {r.returncode}, {len(shown)} "
                      f"conditions line(s): {r.stderr.decode(errors='replace')}")
                ok = False
                continue
            first, last = availability(shown[0]), availability(shown[-1])
            if first != {"fuel_high": (True, None),
                         "link": (False, "no_link")}:
                print(f"FAIL  {name}: the attach's line {first}")
                ok = False
            if last != {"fuel_high": (False, "not_received"),
                        "link": (False, "no_link")}:
                print(f"FAIL  {name}: the last line {last}")
                ok = False
            if any(l["source"] != "file" or l["name"] != "Unread"
                   for l in shown):
                print(f"FAIL  {name}: {shown}")
                ok = False
            if ok:
                print(f"ok    {name}: {len(shown)} conditions lines")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return ok


def cond_lines(events_log):
    with open(events_log, encoding="utf-8") as f:
        return [l.rstrip("\n").split("  cond  ", 1)[1]
                for l in f if "  cond  " in l]


def without_host(cond):
    """A cond line's fields less its host_us (the sixth)"""
    fields = cond.split(",")
    return fields[:5] + fields[6:]


def log(cli, fixture):
    ok = True
    named = os.path.join(fixture, "conditions.json")
    uri = "replay://" + fixture.replace("\\", "/") + "?profile=obd2&speed=max"
    root = tempfile.mkdtemp(prefix="havne-cli-log-conditions-")
    try:
        r, lines = run(cli, ["log", uri, root, "--conditions", named])
        kinds = [l.get("type") for l in lines]
        end = lines[-1] if lines else {}
        if r.returncode != 0 or kinds.count("conditions") != 1 or \
                "condition" in kinds or end.get("reason") != "replay-end":
            print(f"FAIL  the run: exit {r.returncode}, {kinds}, "
                  f"{r.stderr.decode(errors='replace')}")
            return False
        folders = os.listdir(root)
        folder = os.path.join(root, folders[0]) if len(folders) == 1 else ""
        with open(named, "rb") as f:
            want = f.read()
        copy = os.path.join(folder, "conditions.json")
        if not os.path.isfile(copy) or open(copy, "rb").read() != want:
            print(f"FAIL  the recording's copy in {folders}")
            return False
        sha = hashlib.sha256(want).hexdigest()
        events = os.path.join(folder, "events.log")
        with open(events, encoding="utf-8") as f:
            header = f.read()
        if f"conditions  conditions.json,{sha}\n" not in header:
            print("FAIL  the header names no copy")
            ok = False
        got = cond_lines(events)
        expected = cond_lines(os.path.join(fixture, "events.log"))
        # host_us is this run's own: at max speed a stale event's is the
        # host_us of the frame before it
        if [without_host(g) for g in got] != \
                [without_host(e) for e in expected]:
            print(f"FAIL  the events {got}, want {expected}")
            ok = False
        if ok:
            print(f"ok    log: {len(got)} events recorded")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return ok


def main():
    mode, cli, where = sys.argv[1], os.path.abspath(sys.argv[2]), \
        os.path.abspath(sys.argv[3])
    ok = unread(cli, where) if mode == "unread" else log(cli, where)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
