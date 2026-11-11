# File/layer: python/tests/conftest.py | pytest, the havne package's tests
# Testing: shared by every PythonTest
#
# The tree's own fixtures, by path from here: the replay fixture (a
# scrubbed recording of the test car's engine start), the signal files
# and the example plugin; run_example runs a python/examples/ script as a
# user would, in a child Python. Asks is a cancel that counts its asks and
# partials finds what core leaves beside a file it was making; the made-up
# recordings (a replay of the fixture under obd2, a values-only import, a
# frames-only one that echoes its VIN) and havne-cli as a child serve the
# export and health tests. The package comes from python/ (ctest's
# PYTHONPATH) and its library from HAVNE_LIBRARY. Every connect in these
# tests is mock://, replay://, the simulated ECU, vcan0 or the fake J2534
# DLL: never a real bus.

import json
import os
import subprocess
import sys

import pytest

sys.dont_write_bytecode = True
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "python"))

REPLAY = os.path.join(ROOT, "tests", "fixtures", "replay", "2026-10-25-engine-start")
SIGNALS = os.path.join(ROOT, "tests", "fixtures", "signals")
PLUGINS = os.path.join(ROOT, "plugins")
FINDINGS = os.path.join(ROOT, "tests", "fixtures", "recording", "findings")
EXAMPLES = os.path.join(ROOT, "python", "examples")
VCAN = os.environ.get("ECUIO_VCAN_IF", "vcan0")


def vcan_why():
    """Why vcan0 (ECUIO_VCAN_IF) can't be used here, or None"""
    if not sys.platform.startswith("linux"):
        return "SocketCAN needs Linux"
    try:
        with open(f"/sys/class/net/{VCAN}/operstate", encoding="ascii") as f:
            state = f.read().strip()
    except OSError:
        return f"no {VCAN} (the linux skill's vcan, or ip link add {VCAN} type vcan)"
    return None if state in ("up", "unknown") else f"{VCAN} is {state}"


def need_vcan():
    """vcan0's name; skips the case without it, or fails it under
    ECUIO_VCAN_REQUIRED=1, as the C++ vcan suites do (CI's linux-build)"""
    why = vcan_why()
    if why:
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            pytest.fail(f"{why}, and ECUIO_VCAN_REQUIRED=1")
        pytest.skip(why)
    return VCAN


def replay_uri(query=""):
    return "replay://" + REPLAY.replace("\\", "/") + query


def run_example(name, *args, timeout=120):
    """python/examples/<name> in a child Python, the tree's package and
    this tree's library (HAVNE_LIBRARY passes through): its exit status,
    output and errors"""
    env = {**os.environ, "PYTHONPATH": os.path.join(ROOT, "python"),
           "PYTHONIOENCODING": "utf-8", "PYTHONDONTWRITEBYTECODE": "1"}
    run = subprocess.run([sys.executable, "-B", os.path.join(EXAMPLES, name), *args],
                         capture_output=True, text=True, encoding="utf-8", env=env,
                         timeout=timeout)
    return run.returncode, run.stdout, run.stderr


def partials(folder, name):
    """Entries of folder named <name>.partial-...: core's own file or folder left behind"""
    return [e for e in os.listdir(folder) if e.startswith(name + ".partial-")]


class Asks:
    """A cancel counting its asks, stopping (True) or raising at the nth"""

    def __init__(self, at=None, then=True):
        self.asked = 0
        self.at = at
        self.then = then

    def __call__(self):
        self.asked += 1
        if self.asked != self.at:
            return False
        if isinstance(self.then, BaseException):
            raise self.then
        if callable(self.then):
            return self.then()
        return self.then


def nothing_made(folder, name):
    return not os.path.exists(os.path.join(folder, name)) and not partials(folder, name)


def havne_cli():
    """The havne-cli the tree built (HAVNE_CLI); skips the case without it"""
    exe = os.environ.get("HAVNE_CLI")
    if not exe or not os.path.exists(exe):
        pytest.skip("no havne-cli (HAVNE_CLI)")
    return exe


def cli(*args, status=0):
    """havne-cli's JSON lines for args: a list of dicts, its exit status
    checked"""
    done = subprocess.run([havne_cli(), *map(str, args)], capture_output=True, text=True,
                          encoding="utf-8", timeout=120)
    assert done.returncode == status, (args, done.stdout, done.stderr)
    return [json.loads(line) for line in done.stdout.splitlines() if line.strip()]


def line_of(lines, kind):
    """The one line of type kind"""
    got = [x for x in lines if x.get("type") == kind]
    assert len(got) == 1, (kind, lines)
    return got[0]


@pytest.fixture(scope="session")
def car(tmp_path_factory):
    """A recording of the replay fixture under obd2 played at max: frames,
    ISO-TP replies and ranked values (what a car's report is made of)"""
    import havne
    root = tmp_path_factory.mktemp("car")
    with havne.Session(replay_uri("?profile=obd2&speed=max")) as s:
        values = s.values(capacity=65536)
        rec = s.record(root)
        s.start()
        for _ in values:
            pass
        folder = rec.folder
    return folder


@pytest.fixture(scope="session")
def values_only(tmp_path_factory):
    """logger-b's CSV imported: values alone, with a text signal"""
    import havne
    csv = os.path.join(ROOT, "tests", "fixtures", "import", "csv", "logger-b")
    folder = str(tmp_path_factory.mktemp("values") / "logger-b")
    with open(os.path.join(csv, "map.json"), encoding="utf-8") as f:
        havne.import_csv(os.path.join(csv, "log.csv"), folder, json.load(f))
    return folder


@pytest.fixture(scope="session")
def echoes_vin(tmp_path_factory):
    """A frames-only recording whose DID reply echoes the identification
    reply's VIN: a scrubbed export of it fails the check"""
    folder = tmp_path_factory.mktemp("echo")
    vin = "ZZZFAKE11M0000001".encode().hex().upper()
    spaced = " ".join(vin[i:i + 2] for i in range(0, len(vin), 2))
    with open(folder / "frames.csv", "w", newline="\n") as f:
        f.write("host_us,timestamp_us,id,flags,data\n"
                f"1000,1000,0x7E8,0x100,49 02 01 {spaced}\n"
                f"2000,2000,0x7E8,0x100,62 D8 01 {spaced}\n")
    return str(folder)


@pytest.fixture(scope="session")
def health_car(tmp_path_factory):
    """A frames-only recording of an engine ECU by hand: its VIN (made
    up), readiness, one stored code and an engine speed, as reassembled
    messages under an obd2 profile: what a car's health report is made of"""
    folder = tmp_path_factory.mktemp("health-car")
    vin = "ZZZFAKE11M0000001".encode().hex().upper()
    spaced = " ".join(vin[i:i + 2] for i in range(0, len(vin), 2))
    with open(folder / "events.log", "w", newline="\n") as f:
        f.write("ecuio session recording, format 2 (ADR-0049)\nstarted  2026-11-03 10:00:00.000 (local time)\n"
                "utc  2026-11-03T16:00:00.000Z\ncore  0.7.1\nuri  mock://Daily driver?profile=obd2\n"
                "profile  obd2\nframe flags  complete\ncaller  havne-cli 0.7.1\ncolumns  local time, seconds"
                " since the recording started, kind, text\n\n"
                "10:00:01.000  +1.000  end  havne-cli: seconds: 4 frames, 0 values, 0 records dropped,"
                " 0 notes dropped\n")
    with open(folder / "frames.csv", "w", newline="\n") as f:
        f.write("host_us,timestamp_us,id,flags,data" + "\n"
                f"100000,101000,0x7E8,0x100,49 02 01 {spaced}\n"
                f"200000,201000,0x7E8,0x100,41 01 83 07 E5 00\n"
                f"300000,301000,0x7E8,0x100,43 01 01 33\n"
                f"400000,401000,0x7E8,0x100,41 0C 1A F8\n"
                f"900000,901000,0x7E8,0x100,41 0C 1B 58\n")
    return str(folder)


@pytest.fixture
def root():
    return ROOT
