# File/layer: tests/golden.py | test-only, Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest GoldenTest.SelfTest); then the
#          command-line client's golden runs
#
# Golden-output tests for JSON-lines output: runs a command (or reads a
# file), parses every line as a JSON object, drops the keys whose value
# depends on the run (timing), and compares the rest with a committed
# golden file line by line. A difference names the line and the key,
# and --update rewrites the golden file from the run.
#
#   python3 tests/golden.py run --golden <file> [--ignore key ...] [--last N] [--exit N] [--slow]
#                               [--update] -- <command...>
#   python3 tests/golden.py compare <actual> <golden> [--ignore key ...]
#   python3 tests/golden.py selftest
#
# --ignore drops a key wherever it appears, at any depth. --last N
# compares (and --update writes) only the run's last N lines, for a run
# too long to keep whole. --exit N is the command's expected exit code
# (default 0). --slow reads the output 4 KB at a time with a pause
# between, a slow terminal, so a command that writes faster meets a full
# pipe and its own backpressure. Exit 0 on a match, 1 on a difference, 2 on an error (a
# line that isn't JSON, a command exiting with another code).

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def parse(text, source):
    """Each non-empty line as a JSON object; raises ValueError naming the line."""
    rows = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as e:
            raise ValueError(f"{source}:{number}: not JSON ({e.msg})") from None
        if not isinstance(value, dict):
            raise ValueError(f"{source}:{number}: not a JSON object")
        rows.append(value)
    return rows


def drop(value, ignore):
    """value without the ignored keys, at any depth"""
    if isinstance(value, dict):
        return {k: drop(v, ignore) for k, v in value.items() if k not in ignore}
    if isinstance(value, list):
        return [drop(v, ignore) for v in value]
    return value


def first_difference(actual, golden, path=""):
    """The first key path where two values differ, or None"""
    if isinstance(actual, dict) and isinstance(golden, dict):
        for key in sorted(set(actual) | set(golden)):
            here = f"{path}.{key}" if path else key
            if key not in actual:
                return f"{here}: missing (golden has {json.dumps(golden[key])})"
            if key not in golden:
                return f"{here}: unexpected ({json.dumps(actual[key])})"
            found = first_difference(actual[key], golden[key], here)
            if found:
                return found
        return None
    if isinstance(actual, list) and isinstance(golden, list):
        if len(actual) != len(golden):
            return f"{path}: {len(actual)} items, golden has {len(golden)}"
        for i, (a, g) in enumerate(zip(actual, golden)):
            found = first_difference(a, g, f"{path}[{i}]")
            if found:
                return found
        return None
    if actual != golden or type(actual) is not type(golden):
        return f"{path}: {json.dumps(actual)}, golden has {json.dumps(golden)}"
    return None


def compare(actual_rows, golden_rows, ignore):
    """A list of the differences, empty on a match"""
    actual = [drop(r, ignore) for r in actual_rows]
    golden = [drop(r, ignore) for r in golden_rows]
    problems = []
    for number, (a, g) in enumerate(zip(actual, golden), start=1):
        found = first_difference(a, g)
        if found:
            problems.append(f"line {number}: {found}")
    if len(actual) != len(golden):
        problems.append(f"{len(actual)} lines, golden has {len(golden)}")
    return problems


def golden_text(rows, ignore):
    return "".join(json.dumps(drop(r, ignore), sort_keys=True) + "\n" for r in rows)


def report(problems, golden):
    if not problems:
        print(f"ok    matches {golden}")
        return 0
    for p in problems[:20]:
        print(f"FAIL  {p}")
    if len(problems) > 20:
        print(f"FAIL  ... {len(problems) - 20} more")
    print(f"      (rewrite it from this run with --update: {golden})")
    return 1


def run_slowly(command):
    """(exit code, stdout, stderr), its output read a little at a time
    with a pause between, as a slow terminal would: a writer that
    outpaces it meets a full pipe"""
    with tempfile.TemporaryFile() as err:
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err)
        chunks = []
        while True:
            chunk = p.stdout.read1(SLOW_READ_BYTES)
            if not chunk:
                break
            chunks.append(chunk)
            time.sleep(SLOW_READ_PAUSE_S)
        code = p.wait()
        err.seek(0)
        return code, b"".join(chunks).decode("utf-8"), err.read().decode("utf-8", "replace")


SLOW_READ_BYTES = 4096
SLOW_READ_PAUSE_S = 0.001


def cmd_run(args):
    if not args.command:
        print("golden: run needs a command after --", file=sys.stderr)
        return 2
    if args.slow:
        code, stdout, stderr = run_slowly(args.command)
    else:
        done = subprocess.run(args.command, capture_output=True, text=True, encoding="utf-8")
        code, stdout, stderr = done.returncode, done.stdout, done.stderr
    if code != args.exit:
        print(f"golden: {args.command[0]} exited {code}, not {args.exit}\n{stderr}",
              file=sys.stderr)
        return 2
    ignore = set(args.ignore)
    try:
        actual = parse(stdout, args.command[0])
    except ValueError as e:
        print(f"golden: {e}", file=sys.stderr)
        return 2
    if args.last:
        actual = actual[-args.last:]
    golden = Path(args.golden)
    if args.update:
        golden.write_text(golden_text(actual, ignore), encoding="utf-8", newline="\n")
        print(f"ok    wrote {golden}: {len(actual)} lines")
        return 0
    try:
        expected = parse(golden.read_text(encoding="utf-8"), str(golden))
    except (OSError, ValueError) as e:
        print(f"golden: {e}", file=sys.stderr)
        return 2
    return report(compare(actual, expected, ignore), golden)


def cmd_compare(args):
    try:
        actual = parse(Path(args.actual).read_text(encoding="utf-8"), args.actual)
        expected = parse(Path(args.golden).read_text(encoding="utf-8"), args.golden)
    except (OSError, ValueError) as e:
        print(f"golden: {e}", file=sys.stderr)
        return 2
    return report(compare(actual, expected, set(args.ignore)), args.golden)


def cmd_selftest(_args):
    """The comparator's rules, each against a case that must fail"""
    rows = parse('{"t_us": 5, "id": "0x7E8", "v": [1, 2]}\n\n{"t_us": 9, "id": "0x7E8"}\n', "a")
    checks = [
        ("blank lines skipped", len(rows) == 2),
        ("a match", compare(rows, rows, set()) == []),
        ("an ignored key differs", compare(
            rows, parse('{"t_us": 6, "id": "0x7E8", "v": [1, 2]}\n{"t_us": 1, "id": "0x7E8"}', "g"),
            {"t_us"}) == []),
        ("the same key, not ignored", compare(
            rows, parse('{"t_us": 6, "id": "0x7E8", "v": [1, 2]}\n{"t_us": 9, "id": "0x7E8"}', "g"),
            set()) == ["line 1: t_us: 5, golden has 6"]),
        ("ignored at depth", drop({"a": {"t_us": 1, "b": [{"t_us": 2, "c": 3}]}}, {"t_us"})
            == {"a": {"b": [{"c": 3}]}}),
        ("a list item differs", compare(
            rows[:1], parse('{"t_us": 5, "id": "0x7E8", "v": [1, 3]}', "g"), set())
            == ["line 1: v[1]: 2, golden has 3"]),
        ("1 and 1.0 differ", first_difference({"v": 1}, {"v": 1.0}) is not None),
        ("true and 1 differ", first_difference({"v": True}, {"v": 1}) is not None),
        ("a missing key", first_difference({}, {"k": 1}) == 'k: missing (golden has 1)'),
        ("an extra key", first_difference({"k": 1}, {}) == 'k: unexpected (1)'),
        ("a line short", compare(rows[:1], rows, set()) == ["1 lines, golden has 2"]),
    ]
    for text, bad in (("not json\n", "not JSON"), ("[1, 2]\n", "not a JSON object")):
        try:
            parse(text, "x")
            checks.append((f"refuses {bad}", False))
        except ValueError as e:
            checks.append((f"refuses {bad}", bad in str(e) and "x:1" in str(e)))
    # run --update, then run again against what it wrote
    with tempfile.TemporaryDirectory() as tmp:
        golden = Path(tmp) / "out.jsonl"
        emit = [sys.executable, "-c", 'print(\'{"t_us": 7, "id": "0x7E8"}\')']
        ns = argparse.Namespace(golden=str(golden), ignore=["t_us"], update=True, command=emit,
                                last=0, exit=0, slow=False)
        wrote = cmd_run(ns) == 0 and golden.read_text(encoding="utf-8") == '{"id": "0x7E8"}\n'
        checks.append(("--update writes the run without ignored keys", wrote))
        ns.update = False
        checks.append(("a run matches what --update wrote", cmd_run(ns) == 0))
        # --last: only the tail is written and compared
        ns.command = [sys.executable, "-c", 'for n in (1, 2, 3): print(\'{"n": %d}\' % n)']
        ns.update, ns.last = True, 2
        cmd_run(ns)
        checks.append(("--last 2 writes the last two lines",
                       golden.read_text(encoding="utf-8") == '{"n": 2}\n{"n": 3}\n'))
        ns.update = False
        checks.append(("--last 2 matches them", cmd_run(ns) == 0))
        ns.last = 3
        checks.append(("--last 3 sees the line more", cmd_run(ns) == 1))
        # --exit: the code the command must exit with; any other is an error
        ns.command = [sys.executable, "-c", 'import sys; print(\'{"n": 3}\'); sys.exit(3)']
        ns.last, ns.update = 0, True
        checks.append(("a run exiting 3 is an error by default", cmd_run(ns) == 2))
        ns.exit = 3
        checks.append(("--exit 3 takes it", cmd_run(ns) == 0))
        # --slow reads the same output and exit code, a chunk at a time
        ns.update, ns.slow = False, True
        checks.append(("--slow reads the same run", cmd_run(ns) == 0))
        ns.command = [sys.executable, "-c", 'for n in range(3000): print(\'{"n": %d}\' % n)']
        ns.update, ns.slow, ns.exit = True, False, 0
        cmd_run(ns)
        ns.update, ns.slow = False, True
        checks.append(("--slow reads a run longer than one chunk whole", cmd_run(ns) == 0))
    bad = [name for name, passed in checks if not passed]
    for name in bad:
        print(f"FAIL  {name}")
    print(f"{len(checks) - len(bad)}/{len(checks)} checks passed")
    return 1 if bad else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1] if __doc__ else None)
    sub = parser.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    run.add_argument("--golden", required=True)
    run.add_argument("--ignore", action="append", default=[])
    run.add_argument("--update", action="store_true")
    run.add_argument("--last", type=int, default=0)
    run.add_argument("--exit", type=int, default=0)
    run.add_argument("--slow", action="store_true")
    run.add_argument("command", nargs=argparse.REMAINDER)
    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("actual")
    cmp_.add_argument("golden")
    cmp_.add_argument("--ignore", action="append", default=[])
    sub.add_parser("selftest")
    args = parser.parse_args()
    if args.cmd == "run" and args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return {"run": cmd_run, "compare": cmd_compare, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
