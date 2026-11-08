# File/layer: python/havne/__init__.py | Python 3.11+, the havne package
# Testing: python/tests (PythonTest.*)
#
# Havne's core from Python (ADR-0053): ctypes over the C ABI, the shared
# core's exports and nothing else. Importing it finds and loads the core
# library (HAVNE_LIBRARY, else the package's own folder) and refuses one
# of another version.
#
#   import havne
#   with havne.Session("mock://demo?profile=obd2") as s:
#       values = s.values()
#       s.start()
#       for v in values:
#           print(v.name, v.value, v.unit)

"""Havne's core from Python: sessions, recordings and the simulated ECU"""

from ._version import __version__
from ._lib import path as library_path, version as core_version
from .errors import Error, LibraryNotFound, LibraryVersionError, status_name
from .session import Frame, Recorder, Session, Stream, Value
from .recording import (Column, GuessedMap, Imported, Marker, RecordedFrame, Recording,
                        Signal, guess_csv_map, import_candump, import_csv, read_frames)
from .sim import Reply, Sim
from .signals import Plugin, SignalFile
from . import _ecuio as c

__all__ = [
    "__version__", "library_path", "core_version", "c",
    "Error", "LibraryNotFound", "LibraryVersionError", "status_name",
    "Session", "Stream", "Frame", "Value", "Recorder",
    "Recording", "Signal", "Marker", "Column",
    "import_csv", "import_candump", "Imported", "read_frames", "RecordedFrame",
    "guess_csv_map", "GuessedMap",
    "Sim", "Reply", "SignalFile", "Plugin",
]
