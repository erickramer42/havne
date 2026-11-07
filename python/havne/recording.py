# File/layer: python/havne/recording.py | Python 3.11+, the havne package
# Testing: PythonTest.test_recording
#
# A recording read offline (ADR-0049 section 5, recording.h): its
# signals with their ranges, its markers, range queries, the value in
# force at a time, and finding a threshold's crossing or a marker. Times
# are the recording's host_us, microseconds from its start.

import ctypes
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8

Signal = namedtuple("Signal", "index name unit kind samples lowest highest first_us last_us "
                              "decimal_places group")
Signal.__doc__ = ("A recording's signal: kind 'number', 'text' or 'enum'; a number's range; "
                  "first_us and last_us None without samples")
Marker = namedtuple("Marker", "host_us label")
Column = namedtuple("Column", "lowest highest samples")

NONE = (1 << 64) - 1   # ECUIO_RECORDING_NONE


class Recording:
    """The recording in folder, opened once. With profile (a registered
    profile's name) or signals (a SignalFile), its values are decoded from
    its frames.csv through that set instead of read from signals.csv
    (ecuio_recording_open_frames). A context manager."""

    def __init__(self, folder, profile=None, signals=None):
        self._r = ctypes.POINTER(e.ecuio_recording_t)()
        if signals is not None:
            profile = signals.profile
        if profile is None:
            rc = lib.ecuio_recording_open(utf8(str(folder)), e.ecuio_open_progress_t(0), None,
                                          ctypes.byref(self._r))
            check(rc, "ecuio_recording_open")
        else:
            sset = ctypes.POINTER(e.ecuio_signal_set_t)()
            check(lib.ecuio_profile_lookup(utf8(profile), ctypes.byref(sset)),
                  "ecuio_profile_lookup")
            rc = lib.ecuio_recording_open_frames(utf8(str(folder)), sset,
                                                 e.ecuio_open_progress_t(0), None,
                                                 ctypes.byref(self._r))
            check(rc, "ecuio_recording_open_frames")
        self.info = e.ecuio_recording_info_t()
        check(lib.ecuio_recording_get_info(self._r, ctypes.byref(self.info)),
              "ecuio_recording_get_info")
        self.signals = [self._signal(i) for i in range(self.info.signal_count)]
        self.markers = [self._marker(i) for i in range(self.info.marker_count)]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self._r is not None:
            lib.ecuio_recording_close(self._r)
            self._r = None

    def _signal(self, i):
        s = e.ecuio_recording_signal_t()
        check(lib.ecuio_recording_get_signal(self._r, i, ctypes.byref(s)),
              "ecuio_recording_get_signal")
        kind = ("text" if s.flags & e.ECUIO_RECORDING_SIGNAL_TEXT else
                "enum" if s.flags & e.ECUIO_RECORDING_SIGNAL_ENUM else "number")
        none = s.samples == 0
        return Signal(i, text(s.name), text(s.unit), kind, s.samples, s.lowest, s.highest,
                      None if none else s.first_us, None if none else s.last_us,
                      s.decimal_places, s.group)

    def _marker(self, i):
        m = e.ecuio_recording_marker_t()
        check(lib.ecuio_recording_get_marker(self._r, i, ctypes.byref(m)),
              "ecuio_recording_get_marker")
        return Marker(m.host_us, ctypes.string_at(m.label_utf8, m.label_length).decode(
            "utf-8", "replace"))

    def signal(self, name_or_index):
        """A signal by its name or index"""
        if isinstance(name_or_index, int):
            return self.signals[name_or_index]
        for s in self.signals:
            if s.name == name_or_index:
                return s
        raise KeyError(f"the recording has no signal {name_or_index!r}")

    def query(self, signals, t0_us, t1_us, columns):
        """columns columns over [t0_us, t1_us) for each signal (names or
        indexes): a list per signal of Column(lowest, highest, samples)"""
        ids = [self.signal(s).index for s in signals]
        idx = (ctypes.c_uint32 * len(ids))(*ids)
        out = (e.ecuio_recording_column_t * (len(ids) * columns))()
        check(lib.ecuio_recording_query(self._r, idx, len(ids), t0_us, t1_us, out, columns),
              "ecuio_recording_query")
        return [[Column(c.lowest, c.highest, c.samples)
                 for c in out[k * columns:(k + 1) * columns]] for k in range(len(ids))]

    def text_at(self, signal, t_us):
        """(the value in force at t_us as signals.csv holds it, its time);
        ("", None) before the signal's first sample"""
        buf = ctypes.create_string_buffer(e.ECUIO_DECODE_TEXT_MAX + 1)
        at = ctypes.c_uint64()
        check(lib.ecuio_recording_text_at(self._r, self.signal(signal).index, t_us, buf,
                                          len(buf), ctypes.byref(at)),
              "ecuio_recording_text_at")
        return text(buf.raw), None if at.value == NONE else at.value

    def find_threshold(self, signal, level, rising=True, hysteresis=0.0, from_us=0):
        """The first time at or after from_us the signal reaches level,
        rising or falling past its hysteresis; None if it never does"""
        what = e.ecuio_recording_find_t()
        what.kind = e.ECUIO_RECORDING_FIND_THRESHOLD
        what.signal = self.signal(signal).index
        what.direction = e.ECUIO_RECORDING_FIND_RISING if rising else e.ECUIO_RECORDING_FIND_FALLING
        what.level = level
        what.hysteresis = hysteresis
        return self._find(what, from_us)

    def find_marker(self, label, from_us=0):
        """The first marker labeled label at or after from_us; None if none"""
        what = e.ecuio_recording_find_t()
        what.kind = e.ECUIO_RECORDING_FIND_MARKER
        what.label_utf8 = utf8(label)
        return self._find(what, from_us)

    def _find(self, what, from_us):
        at = ctypes.c_uint64()
        check(lib.ecuio_recording_find(self._r, ctypes.byref(what), from_us, ctypes.byref(at)),
              "ecuio_recording_find")
        return None if at.value == NONE else at.value
