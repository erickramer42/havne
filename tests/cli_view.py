# File/layer: tests/cli_view.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliViewRunTest.*): havne-cli view (ADR-0049
#          section 5) from outside the process, on the two pulls in
#          tests/fixtures/recording/
#
#   python tests/cli_view.py once <havne-cli> <pull-early> <pull-late>
#   python tests/cli_view.py agree <havne-cli> <pull-early> <pull-late>
#
# once: over many zooms and alignments, a signal's columns count each of
# its samples once when the axis covers the recording (the default axis
# does): none lost where the axis starts before a recording, none counted
# twice where its first column is split. agree: aligned on the pull's
# rise, the two recordings' columns are equal wherever both have samples,
# in raw columns (under a second) and in whole-second ones (the
# fixtures' pulls are whole seconds apart: wider columns place whole
# seconds).

import json
import subprocess
import sys

SIGNALS = ["--signal", "RPM", "--signal", "Throttle", "--signal", "Coolant",
           "--signal", "VIN"]


def view(cli, folders, args):
    done = subprocess.run([cli, "view"] + folders + args, capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"FAIL  view {' '.join(args)} exited "
                         f"{done.returncode}: {done.stderr.decode()}")
    return [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()]


def once(cli, folders):
    alignments = [[], ["--align-on", "rise:RPM:4000"],
                  ["--align-on", "fall:RPM:3000", "--hysteresis", "100"],
                  ["--align-on", "marker:pull"], ["--align-on", "utc"]]
    zooms = ["1", "3", "7", "13", "40", "77", "400", "1000", "9999"]
    bad = 0
    for align in alignments:
        for n in zooms:
            args = align + ["--columns", n] + SIGNALS
            rows = view(cli, folders, args)
            samples = {(r["recording"], r["name"]): r["samples"]
                       for r in rows if r["type"] == "signal"}
            for r in rows:
                if r["type"] != "columns":
                    continue
                key = (r["recording"], r["signal"])
                if sum(r["samples"]) != samples[key]:
                    print(f"FAIL  {' '.join(args)}: recording {key[0]}'s "
                          f"{key[1]} counts {sum(r['samples'])} of "
                          f"{samples[key]} samples")
                    bad += 1
    if bad == 0:
        print(f"ok    {len(alignments) * len(zooms)} views count every "
              f"sample once")
    return 1 if bad else 0


def agree(cli, folders):
    bad = 0
    for span in [["--from", "-1", "--to", "1", "--columns", "20"],
                 ["--from", "-0.35", "--to", "0.45", "--columns", "16"],
                 ["--from", "-5", "--to", "10", "--columns", "15"],
                 ["--from", "-10", "--to", "15", "--columns", "5"]]:
        args = ["--align-on", "rise:RPM:4000", "--hysteresis", "500"] + span
        rows = view(cli, folders, args + SIGNALS)
        columns = [r for r in rows if r["type"] == "columns"]
        for name in ("RPM", "Throttle"):
            a, b = [r for r in columns if r["signal"] == name]
            pairs = [(x, y) for k in ("lowest", "highest")
                     for x, y in zip(a[k], b[k]) if None not in (x, y)]
            if not pairs or any(x != y for x, y in pairs):
                print(f"FAIL  {' '.join(span)}: {name} differs aligned: "
                      f"{a} against {b}")
                bad += 1
    if bad == 0:
        print("ok    the aligned pulls agree at every zoom")
    return 1 if bad else 0


def main(argv):
    if len(argv) != 5 or argv[1] not in ("once", "agree"):
        print(__doc__ or "see the header", file=sys.stderr)
        return 2
    cli, folders = argv[2], argv[3:5]
    return once(cli, folders) if argv[1] == "once" else agree(cli, folders)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
