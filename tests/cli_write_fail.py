# File/layer: tests/cli_write_fail.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliExitTest.AFailedWriteIsExit1)
#
#   python tests/cli_write_fail.py <havne-cli> <pull-early> <findings> <replay-obd2> <replay-frames>
#
# A command whose output can't be written exits 1 with a stderr line
# (cli.md's exit codes), even when every write fails only at the flush:
# standard output a file opened for reading, so stdio takes the lines
# into its buffer and the failure comes when the buffer goes out. Each
# command first runs once into a file it can write, which must end with
# its own exit (0; 3 for a refusal's error line; 1 for a recording that
# can't be made) and output, so a refusal never stands in for the failure.

import os
import subprocess
import sys
import tempfile

LOG = "(1700000000.250000) can0 7E8#04410C1AF8\n"
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
CONDITIONS = os.path.join(FIXTURES, "conditions")
SIGNALS = os.path.join(FIXTURES, "signals")
LAYOUT = b'{"format": 2, "form": "desktop", "pages": [{"name": "E", "cells": []}]}\n'


def run(cli, args, out):
    return subprocess.run([cli] + args, stdout=out, stderr=subprocess.PIPE,
                          text=True, timeout=60)


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        return 2
    cli, pull, findings, values, replay = sys.argv[1:]
    why = []
    with tempfile.TemporaryDirectory(prefix="havne-cli-write-") as tmp:
        log = os.path.join(tmp, "bus.log")
        with open(log, "w", encoding="utf-8", newline="\n") as f:
            f.write(LOG)
        layout = os.path.join(tmp, "layout.json")
        with open(layout, "wb") as f:
            f.write(LAYOUT)
        latin1 = os.path.join(tmp, "latin1.json")
        with open(latin1, "wb") as f:
            f.write(LAYOUT.replace(b'"E"', b'"Mot\xe9ur"'))
        # A recording's folder under a file can't be made
        unmade = os.path.join(log, "under")
        bad_uri = "mock://write-fail?speed=2"
        locked = os.path.join(tmp, "locked.jsonl")
        open(locked, "wb").close()
        missing = os.path.join(tmp, "missing.json")
        commands = {
            "vin": lambda n: ["vin", "1FAFAKE10M0000001"],
            "dtc": lambda n: ["dtc", "P0301"],
            "view": lambda n: ["view", pull],
            "info": lambda n: ["info", findings],
            "export": lambda n: ["export", values, os.path.join(tmp, n + ".csv"),
                                 "--format", "csv"],
            "import": lambda n: ["import", "candump", log, os.path.join(tmp, n)],
            "frames": lambda n: ["frames", replay, "--from", "100", "--to", "101"],
            "log": lambda n: ["log", "mock://write-fail", os.path.join(tmp, n),
                              "--seconds", "1"],
            "layout": lambda n: ["layout", "check", layout],
            "layout-refused": lambda n: ["layout", "check", latin1],
            "conditions-refused": lambda n: [
                "conditions", "check", os.path.join(CONDITIONS, "refused.json")],
            "conditions-unread": lambda n: ["conditions", "check", missing],
            "conditions-no-profile": lambda n: [
                "conditions", "check", os.path.join(CONDITIONS, "example.json"),
                "--profile", "none-such"],
            "expr": lambda n: [
                "signals", "expr", "--signals", os.path.join(SIGNALS, "chain.json"),
                "margin / 2", "--value", "coolant=95", "--value", "oil=110"],
            "expr-names": lambda n: [
                "signals", "expr", "--signals", os.path.join(SIGNALS, "chain.json")],
            "expr-refused": lambda n: [
                "signals", "expr", "--signals", os.path.join(SIGNALS, "chain.json"),
                "max(coolant, oli)"],
            "expr-unread": lambda n: ["signals", "expr", "--signals", missing, "a"],
            "wrap": lambda n: ["plugins", "wrap",
                               os.path.join(SIGNALS, "extras.json"),
                               "--out", os.path.join(tmp, n + ".json")],
            "wrap-refused": lambda n: [
                "plugins", "wrap", os.path.join(SIGNALS, "refused.json"),
                "--out", os.path.join(tmp, n + ".json")],
            "wrap-unread": lambda n: ["plugins", "wrap", missing, "--out",
                                      os.path.join(tmp, n + ".json")],
            "put-unread": lambda n: [
                "plugins", "put", missing, os.path.join(SIGNALS, "extras.json"),
                "--out", os.path.join(tmp, n + ".json")],
            "drop-unread": lambda n: ["plugins", "drop", missing, "--variant",
                                      "base", "--out",
                                      os.path.join(tmp, n + ".json")],
            "match-unread": lambda n: ["plugins", "match", missing, "--vin",
                                       "1FAFAKE10M0000001"],
            "check-unread": lambda n: ["plugins", "check", missing],
            "log-unread": lambda n: ["log", "mock://write-fail",
                                     os.path.join(tmp, n), "--conditions",
                                     missing],
            "log-bad-uri": lambda n: ["log", bad_uri, os.path.join(tmp, n)],
            "log-unrecorded": lambda n: ["log", "mock://write-fail", unmade],
            "health-bad-uri": lambda n: ["health", bad_uri,
                                         os.path.join(tmp, n + ".json"),
                                         "--format", "json"],
            "stream-unread": lambda n: ["stream", "mock://write-fail",
                                        "--conditions", missing],
            "stream-bad-uri": lambda n: ["stream", bad_uri],
            "stream-unrecorded": lambda n: ["stream", "mock://write-fail",
                                            "--count", "5", "--record", unmade],
        }
        # A run into a writable file ends with this exit
        exits = {"layout-refused": 3, "conditions-refused": 3,
                 "conditions-unread": 3, "conditions-no-profile": 3,
                 "expr-refused": 3, "expr-unread": 3,
                 "wrap-refused": 3, "wrap-unread": 3, "put-unread": 3,
                 "drop-unread": 3, "match-unread": 3, "check-unread": 3,
                 "log-unread": 3, "log-bad-uri": 3, "log-unrecorded": 1,
                 "health-bad-uri": 3, "stream-unread": 3, "stream-bad-uri": 3,
                 "stream-unrecorded": 1}
        if sys.platform.startswith("linux"):
            # play is Linux's alone; elsewhere it is usage, before any line
            commands["play-no-interface"] = lambda n: [
                "play", "mock://write-fail", "--to", "havne-none"]
            exits["play-no-interface"] = 3
        for name, args in commands.items():
            good = os.path.join(tmp, name + "-good.jsonl")
            with open(good, "wb") as out:
                first = run(cli, args(name + "-good"), out)
            if (first.returncode != exits.get(name, 0)
                    or os.path.getsize(good) == 0):
                why.append("%s into a file: exit %d, %d bytes: %s"
                           % (name, first.returncode, os.path.getsize(good),
                              first.stderr.strip()))
                continue
            with open(locked, "rb") as out:
                failed = run(cli, args(name + "-locked"), out)
            if failed.returncode != 1:
                why.append("%s into an unwritable file: exit %d, not 1"
                           % (name, failed.returncode))
            elif "writing the output failed" not in failed.stderr:
                why.append("%s: no stderr line: %r" % (name, failed.stderr))
    for w in why:
        print(w)
    return 1 if why else 0


if __name__ == "__main__":
    sys.exit(main())
