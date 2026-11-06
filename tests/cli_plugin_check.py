# File/layer: tests/cli_plugin_check.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliPluginRunTest.EveryPluginsFixturesPass, Linux):
#          havne-cli plugins check --on <vcan> on every plugin in the
#          repository (ADR-0052 section 8)
#
#   python3 tests/cli_plugin_check.py <havne-cli> <folder>...
#
# Each folder named is a plugin when it holds a manifest.json, else the
# plugins in its subfolders (plugins/). Each is checked with
# its value fixtures run through the simulated ECU on the interface
# (vcan0 by default, ECUIO_VCAN_IF names another): exit 0, no fixture
# file refused, none failed or unrun, and at least one fixture, since a
# plugin in the repository must carry them. Without the interface the
# test skips (77), unless ECUIO_VCAN_REQUIRED=1, as test_vcan's suites do.

import json
import os
import subprocess
import sys

SKIP = 77


def plugins_in(folder):
    if os.path.isfile(os.path.join(folder, "manifest.json")):
        return [folder]
    return sorted(os.path.join(folder, d) for d in os.listdir(folder)
                  if os.path.isfile(os.path.join(folder, d, "manifest.json")))


def main():
    cli, folders = sys.argv[1], sys.argv[2:]
    vcan = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    if not os.path.exists("/sys/class/net/" + vcan):
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print("no %s, and ECUIO_VCAN_REQUIRED=1" % vcan)
            return 1
        print("skipped: no %s (the linux skill's vcan brings it up)" % vcan)
        return SKIP
    plugins = [p for f in folders for p in plugins_in(f)]
    if not plugins:
        print("no plugin in", " ".join(folders))
        return 1
    failed = False
    for name in plugins:
        run = subprocess.run([cli, "plugins", "check", name, "--on", vcan],
                             capture_output=True, text=True, timeout=240)
        lines = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
        checked = [l for l in lines if l.get("type") == "checked"]
        bad = [l for l in lines if l.get("type") == "fixture" and l.get("result") != "passed"]
        why = []
        if run.returncode != 0:
            why.append("exit %d" % run.returncode)
        if len(checked) != 1:
            why.append("%d checked lines" % len(checked))
        else:
            c = checked[0]
            if c["fixtures"] == 0:
                why.append("no fixtures")
            if c["passed"] != c["fixtures"] or c["files_refused"] != 0:
                why.append("%d of %d passed, %d files refused"
                           % (c["passed"], c["fixtures"], c["files_refused"]))
        if why:
            failed = True
            print("%s: %s" % (name, ", ".join(why)))
            for l in bad:
                print("  ", json.dumps(l))
            for l in lines:
                if l.get("type") in ("error", "fixture_refused"):
                    print("  ", json.dumps(l))
            if run.stderr:
                print(run.stderr.rstrip())
        else:
            print("%s: %d fixtures passed" % (name, checked[0]["passed"]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
