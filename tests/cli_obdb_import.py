# File/layer: tests/cli_obdb_import.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliObdbImportTest.*): havne-cli signals import
#          obdb (ADR-0051 section 7) from outside the process
#
#   python tests/cli_obdb_import.py convert <havne-cli> <OBDb set> <golden.jsonl> <golden.json> [--update]
#   python tests/cli_obdb_import.py unwritable <havne-cli> <OBDb set>
#
# convert: the synthetic set (tests/fixtures/obdb/) imported for model year
# 2019 with a name, URL and commit; the command's lines against
# golden.jsonl, the file it wrote against golden.json byte for byte (its
# CRLF taken as LF, a Windows checkout's), and
# nothing else left in the folder (no .partial). unwritable: an --out in a
# folder that isn't there exits 1 with why, never the path, and writes
# nothing. --update rewrites both goldens from the run.

import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside the tests
import golden  # noqa: E402

IGNORED = {"core"}
OPTIONS = ["--year", "2019", "--name", "Example car",
           "--url", "https://example.org/obdb/Example-Car", "--commit", "0000000"]


def run(cli, args):
    done = subprocess.run([cli] + args, capture_output=True)
    rows = golden.parse(done.stdout.decode("utf-8"), "stdout")
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


def convert(cli, fixture, golden_lines, golden_file, rewrite):
    with tempfile.TemporaryDirectory(prefix="havne-cli-obdb-") as root:
        out = Path(root) / "converted.json"
        code, rows, err = run(cli, ["signals", "import", "obdb", fixture,
                                    "--out", str(out)] + OPTIONS)
        if code != 0:
            print(f"FAIL  exit {code}\n{err}")
            return 1
        left = sorted(p.name for p in Path(root).iterdir())
        if left != ["converted.json"]:
            print(f"FAIL  the folder holds {left}, not the file alone")
            return 1
        written = out.read_bytes()
        if rewrite:
            Path(golden_lines).write_text(golden.golden_text(rows, IGNORED),
                                          encoding="utf-8", newline="\n")
            Path(golden_file).write_bytes(written)
            print(f"ok    rewrote {golden_lines} and {golden_file}")
            return 0
        expected = golden.parse(Path(golden_lines).read_text(encoding="utf-8"),
                                golden_lines)
        if golden.report(golden.compare(rows, expected, IGNORED), golden_lines):
            return 1
        # The file is LF on every platform; a Windows checkout may give the
        # golden CRLF
        if b"\r" in written:
            print("FAIL  the file written holds a CR")
            return 1
        if written != Path(golden_file).read_bytes().replace(b"\r\n", b"\n"):
            print(f"FAIL  the file written differs from {golden_file}"
                  " (rewrite it from this run with --update)")
            return 1
        print(f"ok    the lines and the file match {golden_file}")
        return 0


def unwritable(cli, fixture):
    with tempfile.TemporaryDirectory(prefix="havne-cli-obdb-") as root:
        out = Path(root) / "missing" / "converted.json"
        code, rows, err = run(cli, ["signals", "import", "obdb", fixture,
                                    "--out", str(out)] + OPTIONS)
        if code != 1:
            print(f"FAIL  exit {code}, not 1\n{err}")
            return 1
        last = rows[-1] if rows else {}
        if last.get("type") != "error":
            print(f"FAIL  the last line is {last}, not an error")
            return 1
        detail = last.get("detail", "")
        if detail != "the converted file can't be written: there's no such folder":
            print(f"FAIL  the error says {detail!r}")
            return 1
        if root in detail or root.replace("\\", "/") in detail or root in err:
            print("FAIL  the error names the path")
            return 1
        if any(Path(root).rglob("*")):
            print("FAIL  something was written")
            return 1
        print("ok    an --out that can't be written says why and writes nothing")
        return 0


def main(argv):
    if len(argv) >= 6 and argv[1] == "convert":
        return convert(argv[2], argv[3], argv[4], argv[5], "--update" in argv[6:])
    if len(argv) == 4 and argv[1] == "unwritable":
        return unwritable(argv[2], argv[3])
    print(__doc__ or "usage: see the file's header")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
