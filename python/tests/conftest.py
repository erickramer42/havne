# File/layer: python/tests/conftest.py | pytest, the havne package's tests
# Testing: shared by every PythonTest
#
# The tree's own fixtures, by path from here: the replay fixture (a
# scrubbed recording of the test car's engine start), the signal files
# and the example plugin; run_example runs a python/examples/ script as a
# user would, in a child Python. The package comes from python/ (ctest's
# PYTHONPATH) and its library from HAVNE_LIBRARY. Every connect in these
# tests is mock://, replay://, the simulated ECU, vcan0 or the fake J2534
# DLL: never a real bus.

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


@pytest.fixture
def root():
    return ROOT
