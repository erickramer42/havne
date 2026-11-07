# File/layer: python/examples/read_pids.py | Python 3.11+, the havne package
# Testing: HARNESS, PythonTest.test_read_pids (mock://, the ELM327
#          emulator), PythonTest.test_vcan (SocketCAN), WheelTest.Installed
#
# Opens a session, reads its decoded PIDs and prints them: the engine
# speed, the vehicle speed and the coolant temperature, each as it
# changes, until all three have come or the time runs out. The same
# script on every bus (ADR-0053's Done-when):
#
#   python read_pids.py "socketcan://vcan0?profile=obd2"   (Linux)
#   python read_pids.py "elm327://127.0.0.1:35000"          (an adapter's address)
#   python read_pids.py mock://demo                         (no car at all)
#
# Exits 0 once it has printed all three, 1 when the time ran out first.

import argparse
import sys
import time

import havne

WANTED = ("RPM", "Vehicle Speed", "Coolant Temp")


def read_pids(uri, seconds=20.0, out=sys.stdout):
    """Prints the wanted PIDs from uri as they change; the ones seen"""
    last = {}
    with havne.Session(uri) as session:
        values = session.values()
        session.start()
        until = time.monotonic() + seconds
        while len(last) < len(WANTED) and time.monotonic() < until and not values.ended:
            for v in values.read(0.2):
                if v.name in WANTED and last.get(v.name) != v.value:
                    last[v.name] = v.value
                    print(f"{v.name}: {v.value:g} {v.unit}", file=out, flush=True)
    return last


def main(argv=None):
    parser = argparse.ArgumentParser(description="Print a session's decoded PIDs")
    parser.add_argument("uri", help="the session's URI, e.g. socketcan://vcan0?profile=obd2")
    parser.add_argument("--seconds", type=float, default=20.0,
                        help="how long to wait for all three (default 20)")
    args = parser.parse_args(argv)
    seen = read_pids(args.uri, args.seconds)
    missing = [name for name in WANTED if name not in seen]
    if missing:
        print(f"read_pids: no {', '.join(missing)} within {args.seconds:g} s", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
