# File/layer: scripts/bench_ab.py | Python 3.8+, no packages
# Testing: none (a measurement harness; its parsers checked against
#          bench_bridge's and bench_timing's output, 2026-11-01)
#
# Two builds' benchmarks, compared run by run. Back-to-back runs of one
# build vary by tens of percent on a desktop (40 % in the bisect behind
# ADR-0042's decode index), so one run of each proves nothing: this runs
# A, B, A, B ... and reports each figure's median, lowest and highest per
# build, and B's median over A's. A change that moves a figure by less
# than the spread of either build hasn't moved it.
#
#   python3 scripts/bench_ab.py [-n N] <tree A> <tree B> load
#   python3 scripts/bench_ab.py [-n N] <tree A> <tree B> timing [seconds]
#
# load: bench_bridge --load (the replay fixture through the bridge):
#   the worker's frames a second looping, and frame to GUI thread p99 at
#   100x. timing: bench_timing (each transport against the fakes, the
#   fake's 4 ms reply): each transport's lowest P50 latency and its
#   requests a second. N is 5 by default. The trees are build trees with
#   the bench targets built (Release); each run's raw output and a CSV
#   of every figure go to Testing/bench-ab/<stamp>/.

import os
import re
import statistics
import subprocess
import sys
import time


def exe(tree, name):
    for sub in ("tests/Release", "tests"):
        for suffix in (".exe", ""):
            path = os.path.join(tree, sub, name + suffix)
            if os.path.isfile(path):
                return path
    sys.exit(f"bench_ab: no {name} in {tree}/tests (build its target first)")


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def parse_load(text):
    """bench_bridge --load's table: the looping row's worker frames/s and
    the 100x row's frame-to-GUI p99"""
    out = {}
    for line in text.splitlines():
        if not line.startswith("| "):
            continue
        c = cells(line)
        if c[0].startswith("full speed, looping") and len(c) > 6:
            out["worker frames/s"] = float(c[2])
        elif c[0].startswith("100x") and len(c) > 6:
            out["100x frame to GUI p99, ms"] = float(c[6].split("/")[1])
    return out


def parse_timing(text, seconds):
    """bench_timing's table: per transport, the lowest P50 latency and the
    requests a second"""
    out = {}
    for line in text.splitlines():
        if not line.startswith("| ") or line.startswith("| Transport"):
            continue
        c = cells(line)
        if len(c) < 7 or not c[3].isdigit():
            continue
        name = c[0].split(" (")[0]
        m = re.match(r"([0-9.]+)", c[6])
        if m:
            out[f"{name} P50 latency, ms (lowest)"] = float(m.group(1))
        out[f"{name} requests/s"] = int(c[3]) / seconds
    return out


def main():
    args = sys.argv[1:]
    n = 5
    if len(args) >= 2 and args[0] == "-n":
        n, args = int(args[1]), args[2:]
    if len(args) < 3 or args[2] not in ("load", "timing"):
        for line in open(__file__, encoding="utf-8"):
            if not line.startswith("#"):
                break
            print(line[2:].rstrip())
        sys.exit(2)
    trees, kind = args[:2], args[2]
    seconds = int(args[3]) if kind == "timing" and len(args) > 3 else 10
    if kind == "load":
        cmds = [[exe(t, "bench_bridge"), "--load"] for t in trees]
    else:
        cmds = [[exe(t, "bench_timing"), str(seconds)] for t in trees]
    out_dir = os.path.join("Testing", "bench-ab", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)
    figures = [{}, {}]
    for i in range(n):
        for side in (0, 1):
            run = subprocess.run(cmds[side], capture_output=True, text=True, errors="replace")
            text = run.stdout + run.stderr
            label = "AB"[side]
            with open(os.path.join(out_dir, f"{label}{i + 1}.txt"), "w", encoding="utf-8") as fh:
                fh.write(text)
            got = parse_load(text) if kind == "load" else parse_timing(text, seconds)
            if run.returncode != 0 or not got:
                sys.exit(f"bench_ab: run {label}{i + 1} failed (exit {run.returncode}); see {out_dir}")
            for k, v in got.items():
                figures[side].setdefault(k, []).append(v)
            print(f"run {i + 1}/{n} {label}: " + ", ".join(f"{k} {v:g}" for k, v in got.items()), flush=True)
    with open(os.path.join(out_dir, "figures.csv"), "w", encoding="utf-8") as fh:
        fh.write("build,figure,run,value\n")
        for side in (0, 1):
            for k, vs in figures[side].items():
                for j, v in enumerate(vs):
                    fh.write(f"{'AB'[side]},\"{k}\",{j + 1},{v}\n")
    print(f"\nA = {trees[0]}\nB = {trees[1]}\n{n} runs each, alternated; raw output in {out_dir}\n")
    print(f"| Figure | A median (lowest to highest) | B median (lowest to highest) | B / A |")
    print("|---|---|---|---|")
    for k in figures[0]:
        a, b = figures[0][k], figures[1].get(k, [])
        if not b:
            continue
        ma, mb = statistics.median(a), statistics.median(b)
        ratio = f"{mb / ma:.2f}" if ma else "-"
        print(f"| {k} | {ma:g} ({min(a):g} to {max(a):g}) | {mb:g} ({min(b):g} to {max(b):g}) | {ratio} |")


if __name__ == "__main__":
    main()
