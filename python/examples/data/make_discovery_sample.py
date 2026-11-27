# One cold loop of mock://sim?enhanced=1 recorded by havne-cli over the
# ELM327 emulator, into discovery-sample/ with a logger's oracle.csv
# (about 3 minutes):
#   python make_discovery_sample.py --cli <havne-cli>
# By hand, in two terminals:
#   havne-cli sim "mock://sim?enhanced=1" --elm327 tcp:35000
#   havne-cli stream "elm327://127.0.0.1:35000?identify=0&diagnose=0"
#       --record <folder> --signals discovery-sample.json --seconds 170
# then type the prompts' lines (PROMPTS) at their times after the sim's start.
import argparse
import bisect
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

SECONDS = 170
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TRUTH = os.path.join(ROOT, "tests", "fixtures", "discovery", "sim-enhanced-truth.json")
KEPT = ("events.log", "frames.csv", "markers.csv", "signals.csv", "signals.json")
PROMPTS = ((2.0, "prompt door_open"), (4.0, "prompt end"), (6.0, "prompt headlights"),
           (8.0, "prompt end"), (10.0, "prompt ac_request"), (12.0, "prompt end"))
ORACLE_CLOCK_S = 7.25
ORACLE_EVERY_S = 0.25
ORACLE = (("Ignition timing (deg)", "7E8", "FD02", "timing"),
          ("Knock retard (deg)", "7E8", "FD02", "knock"),
          ("Intake cam (deg)", "7E8", "FD03", "cam"),
          ("Lambda", "7E8", "FD03", "lambda"),
          ("Injected quantity (mg)", "7E8", "FD07", "injected"))


def emulator(cli, seconds):
    p = subprocess.Popen([cli, "sim", "mock://sim?enhanced=1", "--elm327", "tcp:0",
                          "--seconds", str(seconds + 60)], stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        j = json.loads(line)
        if j["type"] == "error":
            raise RuntimeError(j)
        if j["type"] == "elm327":
            return p, int(j["listen"].rsplit(":", 1)[1]), time.monotonic()
    raise RuntimeError("the emulator never said where it listens")


def prompt(log, start):
    for at, line in PROMPTS:
        time.sleep(max(0.0, start + at - time.monotonic()))
        log.stdin.write(line + "\n")
        log.stdin.flush()


def record(cli, root, seconds):
    sim, port, start = emulator(cli, seconds)
    try:
        log = subprocess.Popen([cli, "stream", f"elm327://127.0.0.1:{port}?identify=0&diagnose=0",
                                "--record", root, "--signals",
                                os.path.join(HERE, "discovery-sample.json"),
                                "--seconds", str(seconds)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        typist = threading.Thread(target=prompt, args=(log, start))
        typist.start()
        folder, marks = None, 0
        for line in log.stdout:
            j = json.loads(line)
            if j["type"] == "recording" and "folder" in j:
                folder = j["folder"]
            marks += j["type"] == "mark"
        typist.join()
        log.stdin.close()
        if log.wait() != 0 or folder is None or marks != len(PROMPTS):
            raise RuntimeError(f"havne-cli stream ended {log.returncode}, "
                               f"{marks} marks of {len(PROMPTS)}")
    finally:
        sim.terminate()
        sim.communicate(timeout=60)
    return folder, port


def fixed(text, port):
    text = re.sub(r"^started  .*$", "started  2026-01-01 00:00:00.000 (local time)", text,
                  count=1, flags=re.M)
    text = re.sub(r"^utc  .*$", "utc  2026-01-01T00:00:00.000Z", text, count=1, flags=re.M)

    def clock(m):
        ms = round(float(m.group(1)) * 1000)
        return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d}.{ms % 1000:03d}  +"

    text = re.sub(r"^\d\d:\d\d:\d\d\.\d{3}  \+(?=(\d+\.\d+))", lambda m: clock(m), text, flags=re.M)
    return text.replace(f"127.0.0.1:{port}", "127.0.0.1:35000")


def decoded(data, f):
    n = max(1, f["bits"] // 8)
    raw = int.from_bytes(data[f["byte"]:f["byte"] + n], "big" if f["order"] == "be" else "little",
                         signed=f["signed"])
    return raw * f["scale"] + f["offset"]


def series(out):
    """Each oracle column's (seconds, value), read back from the sample as a
    logger that knows the decoding would have it, and the PID's rpm"""
    with open(TRUTH, encoding="utf-8") as f:
        truth = json.load(f)
    fields = {(d["module"], d["did"], x["key"]): x for d in truth["dids"] for x in d["fields"]}
    got = {name: ([], []) for name, *_ in ORACLE}
    got["Engine speed (rpm)"] = ([], [])
    with open(os.path.join(out, "signals.csv"), encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            t = int(row["host_us"]) / 1e6
            key = row["signal"]
            if key.startswith("7E8:410C:"):
                got["Engine speed (rpm)"][0].append(t)
                got["Engine speed (rpm)"][1].append(float(row["value"]))
                continue
            for name, rx, did, field in ORACLE:
                if key.startswith(f"{rx}:62{did}:"):
                    got[name][0].append(t)
                    got[name][1].append(decoded(bytes.fromhex(row["value"]),
                                                fields[(rx, did, field)]))
    return got


def at(times, values, t):
    i = bisect.bisect_left(times, t)
    if i <= 0 or i >= len(times):
        return values[0] if i <= 0 else values[-1]
    w = (t - times[i - 1]) / (times[i] - times[i - 1])
    return values[i - 1] + (values[i] - values[i - 1]) * w


def write_oracle(out, seconds):
    got = series(out)
    names = ["Engine speed (rpm)"] + [name for name, *_ in ORACLE]
    rows = ["time (s)," + ",".join(names)]
    t = 1.0
    while t < seconds - 1.0:
        cells = [f"{at(*got[n], t):.4g}" for n in names]
        rows.append(f"{t + ORACLE_CLOCK_S:.2f}," + ",".join(cells))
        t += ORACLE_EVERY_S
    with open(os.path.join(out, "oracle.csv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")


def main(argv=None):
    p = argparse.ArgumentParser(description="one cold loop of the simulated car's "
                                            "enhanced DIDs, recorded, with a logger's CSV")
    p.add_argument("--cli", required=True, help="havne-cli")
    p.add_argument("--seconds", type=int, default=SECONDS)
    p.add_argument("--out", default=os.path.join(HERE, "discovery-sample"))
    args = p.parse_args(argv)
    root = tempfile.mkdtemp(prefix="havne-discovery-")
    try:
        folder, port = record(args.cli, root, args.seconds)
        if os.path.isdir(args.out):
            shutil.rmtree(args.out)
        os.makedirs(args.out)
        for name in KEPT:
            with open(os.path.join(folder, name), encoding="utf-8", newline="") as f:
                text = f.read()
            with open(os.path.join(args.out, name), "w", encoding="utf-8", newline="") as f:
                f.write(fixed(text, port) if name == "events.log" else text)
        write_oracle(args.out, args.seconds)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(args.out, x)) for x in os.listdir(args.out))
    print(f"{args.out}: {args.seconds} s, {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
