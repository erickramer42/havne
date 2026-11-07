# File/layer: tests/example_isotp_fuzz.py | Python 3.11+, Linux, ctest's ExampleTest.isotp_fuzz_cli
# Testing: HARNESS, ExampleTest.isotp_fuzz_cli
#
# python/examples/isotp_fuzz.py against havne-cli's sim on vcan0
# (ECUIO_VCAN_IF names another): every case must leave the sim answering,
# and the sim must end cleanly, exit 0 and its end line not failed. Outside
# pytest and the havne package, so the sanitizer job (core-sanitizers,
# which has no pytest) runs it against its ASan havne-cli, where a
# sanitizer's report fails the sim's exit. Exit 77 (ctest's skip) without
# vcan0, unless ECUIO_VCAN_REQUIRED=1, which fails instead, as the vcan
# suites do.
#
#   python example_isotp_fuzz.py <havne-cli>

import os
import signal
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ = os.path.join(ROOT, "python", "examples", "isotp_fuzz.py")
IFACE = os.environ.get("ECUIO_VCAN_IF", "vcan0")
WAIT_S = 240   # under ctest's TIMEOUT, so a hang shows its stack; a whole run takes about 10 s


def vcan_why():
    if not sys.platform.startswith("linux"):
        return "SocketCAN needs Linux"
    try:
        with open(f"/sys/class/net/{IFACE}/operstate", encoding="ascii") as f:
            state = f.read().strip()
    except OSError:
        return f"no {IFACE}"
    return None if state in ("up", "unknown") else f"{IFACE} is {state}"


def main(argv):
    if len(argv) != 2:
        print("usage: example_isotp_fuzz.py <havne-cli>", file=sys.stderr)
        return 2
    why = vcan_why()
    if why:
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print(f"FAIL: {why}, and ECUIO_VCAN_REQUIRED=1")
            return 1
        print(f"skipped: {why}")
        return 77
    p = subprocess.Popen([sys.executable, "-B", "-X", "faulthandler", FUZZ,
                          "--interface", IFACE, "--ecu-cli", argv[1]])
    try:
        return p.wait(timeout=WAIT_S)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGABRT)   # faulthandler prints where it waits
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        print(f"FAIL: the fuzzer ran past {WAIT_S} s")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
