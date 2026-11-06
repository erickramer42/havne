# File/layer: tests/cli_paths.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliPathTest.ANonAsciiPathWorks): havne-cli's
#          paths from outside the process
#
#   python tests/cli_paths.py <havne-cli> <replay fixture folder> <signal file> <obdb file> <plugin folder>
#
# Every path havne-cli takes, under a folder whose name a Windows code
# page can't hold whole (an accented letter and Greek): a replay of the
# fixture copied there, a recording made into it, view and info of that
# recording, signals check of a file there, an OBDb import written there,
# and a plugin folder there. On Windows the arguments arrive as UTF-16
# and core takes UTF-8, so each one exits 0; elsewhere they are bytes,
# and the same holds.

import os
import shutil
import subprocess
import sys
import tempfile

NAME = "café αβγ"


def run(args):
    r = subprocess.run(args, capture_output=True, timeout=60)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def main():
    if len(sys.argv) != 6:
        print("usage: cli_paths.py <havne-cli> <replay fixture> <signal file> <obdb file> <plugin folder>")
        return 2
    cli, fixture, signals, obdb, plugin = sys.argv[1:]
    failed = []
    root = tempfile.mkdtemp(prefix="havne-cli-paths-")
    try:
        here = os.path.join(root, NAME)
        os.makedirs(here)
        replay = os.path.join(here, "recording")
        shutil.copytree(fixture, replay)
        signal_file = os.path.join(here, "signals.json")
        shutil.copy(signals, signal_file)
        package = os.path.join(here, "plugin")
        shutil.copytree(plugin, package)
        recordings = os.path.join(here, "recordings")

        def check(what, args):
            code, out, err = run(args)
            if code != 0:
                failed.append("%s: exit %d: %s %s" % (what, code, out[-300:], err[-300:]))
            return out

        uri = "replay://" + replay.replace("\\", "/")
        check("replay", [cli, "stream", uri + "?speed=max&to=1", "--count", "5"])
        check("record", [cli, "stream", uri + "?speed=max&to=2", "--record", recordings])
        made = os.listdir(recordings) if os.path.isdir(recordings) else []
        if len(made) != 1:
            failed.append("record: %d folders made" % len(made))
        else:
            folder = os.path.join(recordings, made[0])
            check("view", [cli, "view", folder])
            check("info", [cli, "info", folder])
        check("signals check", [cli, "signals", "check", signal_file])
        out_file = os.path.join(here, "imported.json")
        check("signals import", [cli, "signals", "import", "obdb", obdb, "--out", out_file])
        if not os.path.isfile(out_file):
            failed.append("signals import: no file written")
        check("plugins list", [cli, "plugins", "list", package])
    finally:
        shutil.rmtree(root, ignore_errors=True)
    for f in failed:
        print("FAIL  " + f.encode("ascii", "backslashreplace").decode("ascii"))
    if failed:
        return 1
    print("ok    replay, record, view, info, signals check and import, plugins list "
          "under a folder named outside any one code page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
