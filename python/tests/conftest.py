# File/layer: python/tests/conftest.py | pytest, the havne package's tests
# Testing: shared by every PythonTest
#
# The tree's own fixtures, by path from here: the replay fixture (a
# scrubbed recording of the test car's engine start), the signal files
# and the example plugin. The package comes from python/ (ctest's
# PYTHONPATH) and its library from HAVNE_LIBRARY. Every connect in these
# tests is mock://, replay://, the simulated ECU, vcan0 or the fake J2534
# DLL: never a real bus.

import os
import sys

import pytest

sys.dont_write_bytecode = True
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "python"))

REPLAY = os.path.join(ROOT, "tests", "fixtures", "replay", "2026-10-25-engine-start")
SIGNALS = os.path.join(ROOT, "tests", "fixtures", "signals")
PLUGINS = os.path.join(ROOT, "plugins")


def replay_uri(query=""):
    return "replay://" + REPLAY.replace("\\", "/") + query


@pytest.fixture
def root():
    return ROOT
