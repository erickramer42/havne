# File/layer: tests/cli_replay_signals.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliReplayTest.AReplayReadsWithItsOwnSignalFile):
#          havne-cli stream replay:// with no --signals decodes with the
#          recording's own signals.json (ADR-0051 section 4)
#
#   python tests/cli_replay_signals.py <havne-cli> <replay fixture> <signal file>
#
# A stretch of the replay fixture is streamed with --signals and
# --record. Its recording, replayed with no --signals and no profile=,
# must select the file's profile and give the same values lines; with its
# own name (rec.<16 hex>) the same again; with profile=obd2 the copy is
# unused; and once signals.json no longer matches its hash the replay
# decodes nothing (raw) and stderr says why.

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


def run(args):
    r = subprocess.run(args, capture_output=True, timeout=60)
    lines = [json.loads(l) for l in r.stdout.decode().splitlines() if l.strip()]
    return r, lines


def of_type(lines, kind):
    return [l for l in lines if l.get("type") == kind]


def values(lines):
    return [l["values"] for l in of_type(lines, "values")]


def profile(lines):
    session = of_type(lines, "session")
    return session[0].get("profile") if session else None


def main():
    cli, fixture, signals = sys.argv[1], sys.argv[2], sys.argv[3]
    ok = True
    root = tempfile.mkdtemp(prefix="havne-cli-replay-signals-")
    try:
        r, made = run([cli, "stream", fixture + "?speed=max&from=100&to=102",
                       "--signals", signals, "--values", "--record", root])
        folders = os.listdir(root)
        if r.returncode != 0 or len(folders) != 1:
            print(f"FAIL  the recording: exit {r.returncode}, "
                  f"{len(folders)} folders, {r.stderr.decode(errors='replace')}")
            return 1
        folder = os.path.join(root, folders[0]).replace("\\", "/")
        recorded = profile(made)
        want = values(made)
        if not want:
            print("FAIL  the recording run gave no values lines")
            return 1

        # No profile=: the recorded one, the copy's values
        r, lines = run([cli, "stream", f"replay://{folder}?speed=max",
                        "--values"])
        if r.returncode != 0 or profile(lines) != recorded:
            print(f"FAIL  no profile=: exit {r.returncode}, profile "
                  f"{profile(lines)}, want {recorded}: "
                  f"{r.stderr.decode(errors='replace')}")
            ok = False
        elif values(lines) != want:
            print(f"FAIL  no profile=: {len(values(lines))} values lines "
                  f"unlike the recording run's {len(want)}")
            ok = False

        # Its own name
        with open(os.path.join(folder, "signals.json"), "rb") as f:
            own = "rec." + hashlib.sha256(f.read()).hexdigest()[:16]
        r, lines = run([cli, "stream",
                        f"replay://{folder}?speed=max&profile={own}",
                        "--values"])
        if r.returncode != 0 or profile(lines) != own or values(lines) != want:
            print(f"FAIL  profile={own}: exit {r.returncode}, profile "
                  f"{profile(lines)}, {len(values(lines))} values lines")
            ok = False

        # Another profile: the copy is unused
        r, lines = run([cli, "stream",
                        f"replay://{folder}?speed=max&profile=obd2",
                        "--values", "--count", "5"])
        if r.returncode != 0 or profile(lines) != "obd2" or r.stderr:
            print(f"FAIL  profile=obd2: exit {r.returncode}, profile "
                  f"{profile(lines)}, stderr {r.stderr!r}")
            ok = False

        # A copy changed since: raw, and stderr says why
        with open(os.path.join(folder, "signals.json"), "ab") as f:
            f.write(b" ")
        r, lines = run([cli, "stream", f"replay://{folder}?speed=max",
                        "--values"])
        err = r.stderr.decode(errors="replace")
        if (r.returncode != 0 or profile(lines) != "raw" or values(lines)
                or "doesn't match its hash" not in err):
            print(f"FAIL  a changed copy: exit {r.returncode}, profile "
                  f"{profile(lines)}, {len(values(lines))} values lines, "
                  f"stderr {err!r}")
            ok = False
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if ok:
        print(f"ok    {len(want)} values lines replayed through the copy "
              f"({recorded}), its own name, unused under obd2, refused "
              "once changed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
