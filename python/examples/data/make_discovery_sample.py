# One cold loop of mock://sim?enhanced=1 recorded by havne-cli over the
# ELM327 emulator, into discovery-sample/ (about 3 minutes):
#   python make_discovery_sample.py --cli <havne-cli>
# By hand, in two terminals:
#   havne-cli sim "mock://sim?enhanced=1" --elm327 tcp:35000
#   havne-cli log "elm327://127.0.0.1:35000?identify=0&diagnose=0" <folder>
#       --signals discovery-sample.json --seconds 170

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SECONDS = 170
HERE = os.path.dirname(os.path.abspath(__file__))
KEPT = ("events.log", "frames.csv", "signals.csv", "signals.json")


def emulator(cli, seconds):
    p = subprocess.Popen([cli, "sim", "mock://sim?enhanced=1", "--elm327", "tcp:0",
                          "--seconds", str(seconds + 60)], stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        j = json.loads(line)
        if j["type"] == "error":
            raise RuntimeError(j)
        if j["type"] == "elm327":
            return p, int(j["listen"].rsplit(":", 1)[1])
    raise RuntimeError("the emulator never said where it listens")


def record(cli, root, seconds):
    sim, port = emulator(cli, seconds)
    try:
        out = subprocess.run([cli, "log", f"elm327://127.0.0.1:{port}?identify=0&diagnose=0",
                              root, "--signals", os.path.join(HERE, "discovery-sample.json"),
                              "--seconds", str(seconds)],
                             capture_output=True, text=True, check=True).stdout
    finally:
        sim.terminate()
        sim.communicate(timeout=60)
    end = json.loads(out.strip().splitlines()[-1])
    return end["folder"], port


def fixed(text, port):
    text = re.sub(r"^started  .*$", "started  2026-01-01 00:00:00.000 (local time)", text,
                  count=1, flags=re.M)
    text = re.sub(r"^utc  .*$", "utc  2026-01-01T00:00:00.000Z", text, count=1, flags=re.M)

    def clock(m):
        ms = round(float(m.group(1)) * 1000)
        return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d}.{ms % 1000:03d}  +"

    text = re.sub(r"^\d\d:\d\d:\d\d\.\d{3}  \+(?=(\d+\.\d+))", lambda m: clock(m), text, flags=re.M)
    return text.replace(f"127.0.0.1:{port}", "127.0.0.1:35000")


def main(argv=None):
    p = argparse.ArgumentParser(description="one cold loop of the simulated car's "
                                            "enhanced DIDs, recorded")
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
    finally:
        shutil.rmtree(root, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(args.out, x)) for x in os.listdir(args.out))
    print(f"{args.out}: {args.seconds} s, {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
