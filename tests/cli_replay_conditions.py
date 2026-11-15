# File/layer: tests/cli_replay_conditions.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliReplayTest.ACopyOfConditionsItCantTakeIsSaid):
#          havne-cli stream of a recording whose conditions.json can't be
#          taken (ADR-0063 section 8)
#
#   python tests/cli_replay_conditions.py <havne-cli> <fixtures/recording/conditions>
#
# The fixture copied to a scratch folder four ways: conditions.json
# changed after the recording (its hash no longer the header's), removed,
# replaced by one the loader refuses with the header's hash made its own,
# and signals.csv removed (the reader can't open the recording to check
# the copy). Each replay must say why on standard error, judge no
# conditions (no conditions or condition line, no conditions_dropped on
# its end line) and play every frame to its end, exit 0. The fixture as it
# is takes its copy, the control.

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


def run(cli, folder):
    uri = "replay://" + folder.replace("\\", "/") + "?profile=obd2&speed=max"
    r = subprocess.run([cli, "stream", uri], capture_output=True, timeout=30)
    lines = [json.loads(l) for l in r.stdout.decode().splitlines() if l.strip()]
    return r, lines


def rehash(folder):
    """events.log's conditions line naming conditions.json as it now is"""
    with open(os.path.join(folder, "conditions.json"), "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    log = os.path.join(folder, "events.log")
    with open(log, encoding="utf-8", newline="") as f:
        text = f.read()
    out = []
    for line in text.split("\n"):
        if line.startswith("conditions  conditions.json,"):
            line = "conditions  conditions.json," + sha
        out.append(line)
    with open(log, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(out))


def changed(folder):
    with open(os.path.join(folder, "conditions.json"), "ab") as f:
        f.write(b" ")


def missing(folder):
    os.remove(os.path.join(folder, "conditions.json"))


def refused(folder):
    with open(os.path.join(folder, "conditions.json"), "wb") as f:
        f.write(b'{"havne_conditions":1,"conditions":[{"key":"x"}]}\n')
    rehash(folder)


def unopened(folder):
    os.remove(os.path.join(folder, "signals.csv"))


CASES = [
    ("changed", changed, "doesn't match its hash"),
    ("missing", missing, "is missing"),
    ("refused", refused, "doesn't load: line 1, column "),
    ("unopened", unopened, "can't be checked: the recording doesn't open"),
]


def main():
    cli, fixture = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    ok = True
    r, lines = run(cli, fixture)
    taken = [l for l in lines if l.get("type") == "conditions"]
    if r.returncode != 0 or len(taken) != 1 or \
            taken[0].get("source") != "recording":
        print(f"FAIL  the fixture's own copy: exit {r.returncode}, {taken}")
        ok = False
    for name, spoil, words in CASES:
        scratch = tempfile.mkdtemp(prefix="havne-cli-conditions-")
        folder = os.path.join(scratch, "copy")
        try:
            shutil.copytree(fixture, folder)
            spoil(folder)
            r, lines = run(cli, folder)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        err = r.stderr.decode(errors="replace")
        want = "havne-cli: the recording's conditions.json " + words
        kinds = [l.get("type") for l in lines]
        end = lines[-1] if lines else {}
        if r.returncode != 0:
            print(f"FAIL  {name}: exit {r.returncode}: {err}")
            ok = False
        if want not in err or "; judging no conditions" not in err:
            print(f"FAIL  {name}: stderr {err!r}, want {want!r}")
            ok = False
        if "conditions" in kinds or "condition" in kinds:
            print(f"FAIL  {name}: a set was judged")
            ok = False
        if end.get("reason") != "replay-end" or end.get("frames") != 217 or \
                "conditions_dropped" in end:
            print(f"FAIL  {name}: end {end}")
            ok = False
        if ok:
            print(f"ok    {name}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
