# File/layer: tests/cli_plugin_make.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliPluginMakeTest.*): havne-cli plugins wrap,
#          put, drop and signals import obdb --plugin (ADR-0064 Q9) from
#          outside the process
#
#   python tests/cli_plugin_make.py golden <havne-cli> <signals dir> <OBDb set> <golden dir> [--update]
#   python tests/cli_plugin_make.py refused <havne-cli> <signals dir>
#   python tests/cli_plugin_make.py vcan <havne-cli> <signals dir>
#
# golden: a run of each command on copies of the fixtures with LF line
# ends (a lineage is the hash of the bytes read, and a Windows checkout
# gives CRLF): each command's lines against <golden dir>/cli/<name>.jsonl
# ("core" and the written line's "bytes" ignored: both follow the core's
# version; "bytes" is checked against the file written instead) and the
# packages written against <golden dir>/plugins/, their
# "requires" core taken as one value. A put of a file whose ECU the
# plugin has at other addresses names the rename; an OBDb set imported
# with no --year is the variant "base" with no match. --update rewrites
# the goldens from the run.
# refused: a file core refuses, the last variant dropped, a folder for a
# packed plugin and an --out that can't be written each end with an error
# line (exit 3, or 1 for the --out), never the path, and leave nothing; an
# --out naming the input replaces it whole.
# vcan (Linux, skip 77 without vcan0 unless ECUIO_VCAN_REQUIRED=1): a
# wrapped signal file, served by havne-cli sim --plugin on the bus and
# streamed back through its variant, decodes each of the file's DIDs the
# sim answers to the same values the file alone decodes.

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside the tests
import golden  # noqa: E402

IGNORED = {"core", "bytes"}
OBDB = ["--name", "Example car", "--url", "https://example.org/obdb/Example-Car",
        "--commit", "0000000"]


def run(cli, args):
    done = subprocess.run([cli] + [str(a) for a in args], capture_output=True)
    rows = golden.parse(done.stdout.decode("utf-8"), "stdout")
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


def lf_copies(signals, root, names):
    """Each fixture copied into root with LF line ends"""
    for name in names:
        data = (Path(signals) / name).read_bytes().replace(b"\r\n", b"\n")
        (Path(root) / name).write_bytes(data)


def same_core(text):
    """A package's JSON with its "requires" core taken as one value"""
    doc = json.loads(text)
    doc["requires"]["core"] = "<core>"
    return doc


def check_step(name, rows, written, gold, rewrite):
    """name's lines against its golden, and the package written against
    its own when it has one; False on a difference"""
    lines = Path(gold) / "cli" / (name + ".jsonl")
    package = Path(gold) / "plugins" / (name + ".json")
    if rewrite:
        lines.write_text(golden.golden_text(rows, IGNORED), encoding="utf-8",
                         newline="\n")
        if written is not None:
            package.parent.mkdir(exist_ok=True)
            package.write_bytes(written)
        return True
    expected = golden.parse(lines.read_text(encoding="utf-8"), str(lines))
    if golden.report(golden.compare(rows, expected, IGNORED), str(lines)):
        return False
    if written is not None:
        if b"\r" in written:
            print(f"FAIL  {name}: the package holds a CR")
            return False
        kept = package.read_bytes().replace(b"\r\n", b"\n")
        if same_core(written) != same_core(kept):
            print(f"FAIL  {name}: the package differs from {package} "
                  "(rewrite it from this run with --update)")
            return False
    return True


def golden_run(cli, signals, obdb, gold, rewrite):
    with tempfile.TemporaryDirectory(prefix="havne-cli-plugins-") as root:
        r = Path(root)
        lf_copies(signals, root, ["extras.json", "gauges.json", "modules.json"])
        shutil.copyfile(obdb, r / "set.json")
        # name, the command, the package it writes and whether it's kept
        steps = [
            ("plugins-wrap", ["plugins", "wrap", r / "extras.json",
                              "--out", r / "a.json"], "a.json", True),
            ("plugins-wrap-named", ["plugins", "wrap", r / "extras.json",
                                    "--id", "my-desk", "--variant", "desk",
                                    "--out", r / "n.json"], "n.json", False),
            ("plugins-put", ["plugins", "put", r / "a.json", r / "gauges.json",
                             "--out", r / "b.json"], "b.json", False),
            ("plugins-put-renamed", ["plugins", "put", r / "b.json",
                                     r / "modules.json", "--variant",
                                     "modules", "--out", r / "c.json"],
             "c.json", True),
            ("plugins-put-replaced", ["plugins", "put", r / "c.json",
                                      r / "gauges.json", "--variant", "base",
                                      "--out", r / "e.json"], "e.json", False),
            ("plugins-drop", ["plugins", "drop", r / "c.json", "--variant",
                              "gauged-extras", "--out", r / "d.json"],
             "d.json", False),
            ("obdb-import-plugin", ["signals", "import", "obdb", r / "set.json",
                                    "--plugin", "--year", "2019", "--wmi",
                                    "zzz,ZZ9abc", "--years", "2015-2020",
                                    "--out", r / "o.json"] + OBDB,
             "o.json", True),
        ]
        failed = False
        for name, args, out, kept in steps:
            code, rows, err = run(cli, args)
            if code != 0:
                print(f"FAIL  {name}: exit {code}\n{err}")
                return 1
            written = (r / out).read_bytes()
            if not check_step(name, rows, written if kept else None, gold,
                              rewrite):
                failed = True
            # "bytes" follows the core's version, so it's checked here, not
            # in the golden: the written line's size is the file's
            said = [row["bytes"] for row in rows if row.get("type") == "written"]
            if said != [len(written)]:
                print(f"FAIL  {name}: the written line says {said} bytes, "
                      f"the file holds {len(written)}")
                failed = True
        if rewrite:
            print(f"ok    rewrote the goldens in {gold}")
            return 0

        # No --year: the variant is base, and nothing makes a match
        code, rows, err = run(cli, ["signals", "import", "obdb",
                                    r / "set.json", "--plugin",
                                    "--out", r / "y.json"] + OBDB)
        written = [row for row in rows if row.get("type") == "written"]
        doc = json.loads((r / "y.json").read_text(encoding="utf-8"))
        if (code != 0 or len(written) != 1 or written[0]["variant"] != "base"
                or [v["id"] for v in doc["variants"]] != ["base"]
                or "match" in doc):
            print(f"FAIL  an import with no --year: exit {code}, {written}, "
                  f"variants {doc.get('variants')}, match {doc.get('match')}")
            failed = True
        left = sorted(p.name for p in r.iterdir())
        expected = sorted(["extras.json", "gauges.json", "modules.json",
                           "set.json", "a.json", "n.json", "b.json", "c.json",
                           "e.json", "d.json", "o.json", "y.json"])
        if left != expected:
            print(f"FAIL  the folder holds {left}: a partial left behind?")
            failed = True
        if failed:
            return 1
        print("ok    every command's lines and package match the goldens")
        return 0


def refused_step(cli, root, args, code_wanted, words, before):
    """One refused command: its exit, an error line holding words and
    never the folder, and root holding what it did before"""
    code, rows, err = run(cli, args)
    last = rows[-1] if rows else {}
    detail = last.get("detail", "")
    ok = True
    if code != code_wanted or last.get("type") != "error":
        print(f"FAIL  {args[:2]}: exit {code}, last line {last}\n{err}")
        ok = False
    elif words not in detail:
        print(f"FAIL  {args[:2]}: the error says {detail!r}, not {words!r}")
        ok = False
    elif root in detail or root.replace("\\", "/") in detail or root in err:
        print(f"FAIL  {args[:2]}: the error names the path")
        ok = False
    now = sorted(p.name for p in Path(root).rglob("*"))
    if now != before:
        print(f"FAIL  {args[:2]}: the folder holds {now}, not {before}")
        ok = False
    return ok


def refused(cli, signals):
    with tempfile.TemporaryDirectory(prefix="havne-cli-plugins-") as root:
        r = Path(root)
        lf_copies(signals, root, ["extras.json", "gauges.json", "refused.json"])
        code, _, err = run(cli, ["plugins", "wrap", r / "extras.json",
                                 "--out", r / "a.json"])
        if code != 0:
            print(f"FAIL  the wrap: exit {code}\n{err}")
            return 1
        (r / "folder").mkdir()
        before = sorted(p.name for p in r.rglob("*"))
        ok = all([
            refused_step(cli, root, ["plugins", "wrap", r / "refused.json",
                                     "--out", r / "x.json"], 3, "line ",
                         before),
            refused_step(cli, root, ["plugins", "drop", r / "a.json",
                                     "--variant", "base", "--out",
                                     r / "x.json"], 3, "base", before),
            refused_step(cli, root, ["plugins", "put", r / "folder",
                                     r / "gauges.json", "--out", r / "x.json"],
                         3, "the plugin is a folder", before),
            refused_step(cli, root, ["plugins", "put", r / "a.json",
                                     r / "missing.json", "--out",
                                     r / "x.json"], 3,
                         "the signal file can't be read", before),
            refused_step(cli, root, ["plugins", "wrap", r / "extras.json",
                                     "--out", r / "none" / "x.json"], 1,
                         "the plugin can't be written: there's no such "
                         "folder", before),
            refused_step(cli, root, ["plugins", "wrap", r / "extras.json",
                                     "--out", r / "folder"], 1,
                         "the plugin can't be written: it's a folder", before),
        ])
        # An --out naming the input replaces it whole
        code, rows, err = run(cli, ["plugins", "put", r / "a.json",
                                    r / "gauges.json", "--out", r / "a.json"])
        doc = json.loads((r / "a.json").read_text(encoding="utf-8"))
        if code != 0 or len(doc["variants"]) != 2 or doc["version"] != "1.1.0":
            print(f"FAIL  a put over its input: exit {code}, {doc['variants']}"
                  f"\n{err}")
            ok = False
        now = sorted(p.name for p in r.rglob("*"))
        if now != before:
            print(f"FAIL  a put over its input left {now}")
            ok = False
        if not ok:
            return 1
        print("ok    each refusal ends with its words and leaves nothing")
        return 0


def vcan_why():
    if not sys.platform.startswith("linux"):
        return "the bus is Linux's"
    iface = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    if not Path("/sys/class/net", iface).exists():
        return f"no {iface}"
    return None


def values_by_name(rows, names):
    """The first value of each signal named, from a stream's lines"""
    ids = {}
    for row in rows:
        if row.get("type") == "signals":
            ids = {s["id"]: s["name"] for s in row["signals"]
                   if s["name"] in names}
    got = {}
    for row in rows:
        if row.get("type") == "values":
            for v in row["values"]:
                name = ids.get(v["id"])
                if name is not None and name not in got:
                    got[name] = v.get("value")
    return got


def stream_values(cli, iface, args, names):
    code, rows, err = run(cli, ["stream", f"socketcan://{iface}?" + args[0],
                                "--values", "--seconds", "4"] + args[1:])
    if code != 0:
        print(f"FAIL  the stream: exit {code}\n{err}")
        return None
    return values_by_name(rows, names)


def vcan(cli, signals):
    why = vcan_why()
    if why is not None:
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print(f"FAIL  {why}, and ECUIO_VCAN_REQUIRED=1")
            return 1
        print(f"skipped: {why}")
        return 77
    iface = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    with tempfile.TemporaryDirectory(prefix="havne-cli-plugins-") as root:
        r = Path(root)
        lf_copies(signals, root, ["modules.json"])
        code, _, err = run(cli, ["plugins", "wrap", r / "modules.json",
                                 "--out", r / "m.json"])
        if code != 0:
            print(f"FAIL  the wrap: exit {code}\n{err}")
            return 1
        names = {"Gear Held", "Oil Pressure Desk"}
        variant = "plugin.desk-modules.base"
        results = {}
        for label, sim_args, stream_args in [
            ("the plugin", ["--plugin", r / "m.json"],
             [f"profile={variant}&identify=0&diagnose=0", "--plugin",
              r / "m.json"]),
            ("the file", ["--signals", r / "modules.json"],
             ["profile=user.desk-modules&identify=0&diagnose=0", "--signals",
              r / "modules.json"]),
        ]:
            profile = variant if label == "the plugin" else "user.desk-modules"
            sim = subprocess.Popen([cli, "sim", f"mock://sim?profile={profile}",
                                    "--on", iface, "--seconds", "8"]
                                   + [str(a) for a in sim_args],
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
            try:
                time.sleep(0.5)
                results[label] = stream_values(cli, iface, stream_args, names)
            finally:
                sim.terminate()
                sim.communicate(timeout=10)
            if results[label] is None:
                return 1
        if set(results["the plugin"]) != names:
            print(f"FAIL  the plugin's variant decoded {results['the plugin']}"
                  f", not each of {sorted(names)}")
            return 1
        if results["the plugin"] != results["the file"]:
            print(f"FAIL  the plugin decoded {results['the plugin']}, the file "
                  f"alone {results['the file']}")
            return 1
        print(f"ok    the wrapped plugin decodes as the file: "
              f"{results['the plugin']}")
        return 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "golden" and len(argv) in (5, 6):
        return golden_run(argv[1], argv[2], argv[3], argv[4],
                          argv[5:] == ["--update"])
    if len(argv) == 3 and argv[0] == "refused":
        return refused(argv[1], argv[2])
    if len(argv) == 3 and argv[0] == "vcan":
        return vcan(argv[1], argv[2])
    print(__doc__ or "see the file's opening lines")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
