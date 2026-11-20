# File/layer: tests/cli_transfer_kit.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (imported by the cli_transfer*.py scripts; ctest never runs it
#          by itself)
#
# The helpers cli_transfer.py, cli_transfer_import.py and
# cli_transfer_interrupt.py share: the candump log, a run of havne-cli
# that reads its JSON lines, one line of a kind, a comparison that exits
# with words, and the check for core's partial file.

import json
import os
import subprocess

LOG = (
    "(1700000000.250000) can0 7DF#0201000000000000\n"
    "(1700000000.254000) can0 7E8#04410C1AF8\n"
    "(1700000000.350000) can0 7E0#05010C0D0504\n"
    "(1700000000.354000) can0 7E8#100A410C0FA00D32\n"
    "(1700000000.354500) can0 7E0#3000000000000000\n"
    "(1700000000.355000) can0 7E8#21055A0480\n"
    "(1700000000.400000) can0 456##3AABBCCDDEEFF00112233\n"
)
def partial_left(path):
    """Whether core's own file or folder, <path>.partial-..., is left beside path"""
    folder, name = os.path.split(path)
    return any(e.startswith(name + ".partial-") for e in os.listdir(folder))


def run(cli, args, want=0):
    # No standard input: stream --record reads it as marks, and a test
    # run's own input (a script fed to a shell) would become markers
    done = subprocess.run([cli] + args, capture_output=True,
                          stdin=subprocess.DEVNULL)
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


def same(what, a, b):
    if a != b:
        raise SystemExit(f"FAIL  {what} differs: {a} then {b}")
