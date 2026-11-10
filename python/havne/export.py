# File/layer: python/havne/export.py | Python 3.11+, the havne package
# Testing: PythonTest.test_export
#
# A recording's folder written as a pcapng, a CSV or a scrubbed copy of the
# folder, and the wide CSV sized (ADR-0054's exports, ADR-0056 sections 4
# and 5; ecuio_export_with, ecuio_export_estimate_with). Each file is what
# havne-cli export writes for the same options, byte for byte. A cancel or
# a Ctrl+C stops a call at core's next ask with nothing left behind
# (ADR-0053's appendix "exports and the health report from Python"). The
# range is host_us, both ends included; the file's name is never replaced.

import ctypes
import os
from collections import namedtuple

from . import _ecuio as e
from ._asks import _Asks
from ._core import check, lib, text, utf8
from .errors import CheckFailed, Error

LeftOut = namedtuple("LeftOut", "identification unattributable text_values activity")
LeftOut.__doc__ = ("What a scrubbed export left out, by class: the frames (pcapng) or values "
                   "(CSV) of identification replies, of messages no ECU could be told from, of "
                   "text signals, and the activity lines of a kind the export doesn't know")
Exported = namedtuple("Exported", "path format scrubbed frames made_up values signal_count rows "
                                  "files files_left_out left_out missing")
Exported.__doc__ = ("What an export wrote: its path and format ('pcapng', 'csv' or 'folder'), "
                    "whether it is scrubbed, core's counts (0 where the format has none), "
                    "left_out as a LeftOut and missing, the names in signals the recording "
                    "has no values for, as a tuple")
EstimatedSize = namedtuple("EstimatedSize", "row_ms rows bytes")
EstimatedSize.__doc__ = "The wide CSV one row interval gives: its rows and the file's exact bytes"
Estimate = namedtuple("Estimate", "columns missing sizes")
Estimate.__doc__ = ("What estimate_csv found: the columns, missing as Exported's and sizes, an "
                    "EstimatedSize for each interval asked, in order")

# The words of ECUIO_EXPORT_LEFT_OUT_*, by index (havne-cli's)
_CLASS_WORDS = ("identification", "unattributable", "text_values", "activity", "vin_shaped")
ROW_MS_MAX = 86400000
SIZES_MAX = 64
_LAYOUTS = ("wide", "long")
_UNITS = {"metric": e.ECUIO_UNITS_METRIC, "imperial": e.ECUIO_UNITS_IMPERIAL}
_UINT64 = 0xFFFFFFFFFFFFFFFF


def class_word(failed_class):
    """The word of a failed check's class (a class plus one, as core
    reports it); "unknown" for one this package doesn't know"""
    i = failed_class - 1
    return _CLASS_WORDS[i] if 0 <= i < len(_CLASS_WORDS) else "unknown"


def left_out_of(counts):
    """A LeftOut from a report's left_out[] array"""
    return LeftOut(*(counts[i] for i in range(len(LeftOut._fields))))


def checked(status, call, failed_class):
    """status if OK; else an Error, or CheckFailed when core's scrub check
    found a class (failed_class, core's: a class plus one, 0 for none)"""
    try:
        return check(status, call)
    except Error as err:
        if failed_class:
            raise CheckFailed(err.status, err.words, err.detail, call,
                              class_word(failed_class)) from None
        raise


def _whole(name, value, low=0):
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} is a whole number, not {value!r}")
    if value < low:
        raise ValueError(f"{name} is {low} or more, not {value}")
    if value > _UINT64:
        raise ValueError(f"{name} is too large: {value}")
    return value


def range_us(from_us, to_us):
    """(from_us, to_us as core takes it, 0 the end) after the package's
    checks: whole numbers, and a to_us that is None (the end) or at or
    after from_us and above 0"""
    _whole("from_us", from_us)
    if to_us is None:
        return from_us, 0
    _whole("to_us", to_us)
    if to_us == 0:
        raise ValueError("to_us is 0: that would be the whole recording; give None for the end")
    if to_us < from_us:
        raise ValueError(f"to_us ({to_us}) is before from_us ({from_us})")
    return from_us, to_us


def _word(name, value, words):
    if not isinstance(value, str):
        raise TypeError(f"{name} is a str, not {value!r}")
    if value not in words:
        raise ValueError(f"{name} is {' or '.join(repr(w) for w in words)}, not {value!r}")
    return value


def _flag(name, value):
    if not isinstance(value, bool):
        raise TypeError(f"{name} is True or False, not {value!r}")
    return value


def _path(name, value):
    if not isinstance(value, (str, os.PathLike)):
        raise TypeError(f"{name} is a str or a path, not {value!r}")
    return str(value)


def _names(signals):
    """signals as a list of str: None stays None"""
    if signals is None:
        return None
    if isinstance(signals, (str, bytes)) or not hasattr(signals, "__iter__"):
        raise TypeError("signals is a list of names, or None")
    names = list(signals)
    for n in names:
        if not isinstance(n, str):
            raise TypeError(f"a signal's name is a str, not {n!r}")
    if not names:
        raise ValueError("signals is empty: name at least one, or give None for the "
                         "recording's own pick")
    for n in names:
        if not n or "\0" in n:
            raise ValueError(f"a signal's name is not empty and has no NUL: {n!r}")
    return names


def _options(fmt, folder, out, raw, from_us, to_us):
    o = e.ecuio_export_options_t()
    o.format = fmt
    o.flags = e.ECUIO_EXPORT_RAW if _flag("raw", raw) else 0
    o.from_us, o.to_us = from_us, to_us
    o.folder_utf8 = utf8(folder)
    o.out_path_utf8 = None if out is None else utf8(out)
    return o


def _csv_shape(o, layout, row_ms, units, names, keep):
    """The CSV's fields of options o; the columns' memory is put in keep,
    which the caller holds until core's call has returned"""
    o.layout = e.ECUIO_CSV_LONG if layout == "long" else e.ECUIO_CSV_WIDE
    o.row_ms = row_ms
    o.units = _UNITS[units]
    if names:
        encoded = [utf8(n) for n in names]
        array = (ctypes.c_char_p * len(encoded))(*encoded)
        keep.extend((encoded, array))
        o.column_count = len(encoded)
        o.columns_utf8 = array


def _csv_args(layout, signals, row_ms, units):
    """The CSV keywords checked: (layout, names, row_ms, units)"""
    _word("layout", layout, _LAYOUTS)
    _word("units", units, tuple(_UNITS))
    row_ms = _whole("row_ms", row_ms)
    if row_ms > ROW_MS_MAX:
        raise ValueError(f"row_ms is 0 to {ROW_MS_MAX}, not {row_ms}")
    names = _names(signals)
    if layout == "long" and (names is not None or row_ms != 0 or units != "metric"):
        raise ValueError("signals, row_ms and units are the wide layout's")
    return layout, names, row_ms, units


def _recorded_names(folder, asks):
    """(the recording's signal names, None) from a reader's open under the
    caller's cancel; ((), status) when the open was stopped; None, None
    when the reader can't open it (the export says why)"""
    r = ctypes.POINTER(e.ecuio_recording_t)()
    rc = lib.ecuio_recording_open(utf8(folder), asks.progress, None, ctypes.byref(r))
    if rc == e.ECUIO_ERR_CANCELED:
        return (), rc
    if rc != e.ECUIO_OK or not r:
        return None, None
    try:
        info = e.ecuio_recording_info_t()
        if lib.ecuio_recording_get_info(r, ctypes.byref(info)) != e.ECUIO_OK:
            return None, None
        names = []
        for i in range(info.signal_count):
            s = e.ecuio_recording_signal_t()
            if lib.ecuio_recording_get_signal(r, i, ctypes.byref(s)) == e.ECUIO_OK:
                names.append(text(s.name))
        return names, None
    finally:
        lib.ecuio_recording_close(r)


class _Work:
    """One long call: the reader's open that finds the missing columns (only
    with names), then core's call, under one cancel and one handler swap"""

    def __init__(self, folder, names, cancel):
        self.asks = _Asks(cancel)
        self.folder = folder
        self.names = names
        self.missing = ()
        self.call = None

    def run(self, call, name):
        """call(callback)'s status, with self.call the name of the call
        that gave it and self.missing the names the recording lacks"""
        self.call = name

        def work(callback):
            if self.names:
                recorded, stopped = _recorded_names(self.folder, self.asks)
                if stopped is not None:
                    self.call = "ecuio_recording_open"
                    return stopped
                if recorded is not None:
                    self.missing = tuple(n for n in self.names if n not in recorded)
            return call(callback)

        return self.asks.run(work)


def _exported(fmt, out, raw, report, missing):
    return Exported(out, fmt, not raw, report.frames, report.made_up, report.values,
                    report.signal_count, report.rows, report.files, report.files_left_out,
                    left_out_of(report.left_out), missing)


def export_pcapng(folder, out, *, raw=False, from_us=0, to_us=None, cancel=None):
    """The recording in folder written to the file out as a pcapng for
    Wireshark (ecuio_export_with, ADR-0054): scrubbed unless raw=True,
    its frames from from_us to to_us (host_us, both included; None the end).
    Returns an Exported; a folder that is no recording, an out that
    exists or a failed scrub check raises (Error, CheckFailed), leaving
    nothing written. cancel, a callable taking no arguments, is asked on
    the calling thread as core reads: true stops the call with Error
    ECUIO_ERR_CANCELED; an exception it raises, or a Ctrl+C on the main
    thread, stops it at core's next ask and is raised again."""
    return _write(e.ECUIO_EXPORT_PCAPNG, "pcapng", folder, out, raw, from_us, to_us, cancel)


def export_folder(folder, out, *, raw=False, from_us=0, to_us=None, cancel=None):
    """The recording in folder copied, scrubbed unless raw=True, into the
    new folder out, its frames, values and activity from from_us to to_us
    (host_us, both included; None the end), as export_pcapng takes them.
    The source's name is never carried. Returns an Exported."""
    return _write(e.ECUIO_EXPORT_FOLDER, "folder", folder, out, raw, from_us, to_us, cancel)


def export_csv(folder, out, *, layout="wide", signals=None, row_ms=0, units="metric",
               raw=False, from_us=0, to_us=None, cancel=None):
    """The recording's values in folder written to the file out as a CSV
    (ADR-0056 sections 4 and 5), over the range export_pcapng takes.
    layout "wide" (the default) has a column for each signal, Time (ms)
    first: signals names the columns in order (None takes the recording's
    focus line, else every number signal with a sample in range), row_ms 0 a
    row for each sweep, else a grid that many ms apart (to 86400000), units
    "metric" or "imperial". A named signal the recording has no values for
    is an empty column, named in Exported.missing. layout "long" has a row
    for each value (time_s,signal,unit,value after a comment line: pandas
    skiprows=1) and takes none of signals, row_ms or units. Returns an
    Exported."""
    layout, names, row_ms, units = _csv_args(layout, signals, row_ms, units)
    return _write(e.ECUIO_EXPORT_CSV, "csv", folder, out, raw, from_us, to_us, cancel,
                  (layout, row_ms, units), names)


def _write(fmt, word, folder, out, raw, from_us, to_us, cancel, shape=None, names=None):
    folder, out = _path("folder", folder), _path("out", out)
    o = _options(fmt, folder, out, raw, *range_us(from_us, to_us))
    keep = []
    if shape:
        layout, row_ms, units = shape
        _csv_shape(o, layout, row_ms, units, names, keep)
    work = _Work(folder, names if _is_wide(shape) else None, cancel)
    report = e.ecuio_export_report_t()
    rc = work.run(lambda callback: lib.ecuio_export_with(ctypes.byref(o), callback, None,
                                                         ctypes.byref(report)),
                  "ecuio_export_with")
    checked(rc, work.call, report.failed_class)
    return _exported(word, out, raw, report, work.missing)


def _is_wide(shape):
    return bool(shape) and shape[0] == "wide"


def estimate_csv(folder, *, row_ms=(0, 100, 250), signals=None, units="metric", raw=False,
                 from_us=0, to_us=None, cancel=None):
    """The wide CSV export_csv writes for these options, sized for each
    interval in row_ms (one, or up to 64) in a single pass over the
    recording, writing nothing: an Estimate of the columns, the missing
    names and an EstimatedSize (row_ms, rows, bytes) for each interval, in
    the order asked. The file check doesn't run, so an export of the same
    options can still fail it."""
    folder = _path("folder", folder)
    intervals = [row_ms] if isinstance(row_ms, int) and not isinstance(row_ms, bool) \
        else (list(row_ms) if hasattr(row_ms, "__iter__") and not isinstance(row_ms, str)
              else None)
    if intervals is None:
        raise TypeError(f"row_ms is a whole number or a list of them, not {row_ms!r}")
    if not 1 <= len(intervals) <= SIZES_MAX:
        raise ValueError(f"row_ms holds 1 to {SIZES_MAX} intervals, not {len(intervals)}")
    for ms in intervals:
        if _whole("row_ms", ms) > ROW_MS_MAX:
            raise ValueError(f"row_ms is 0 to {ROW_MS_MAX}, not {ms}")
    _, names, _, units = _csv_args("wide", signals, 0, units)
    o = _options(e.ECUIO_EXPORT_CSV, folder, None, raw, *range_us(from_us, to_us))
    keep = []
    _csv_shape(o, "wide", 0, units, names, keep)
    asked = (ctypes.c_uint32 * len(intervals))(*intervals)
    sized = (e.ecuio_export_estimate_t * len(intervals))()
    work = _Work(folder, names, cancel)
    rc = work.run(lambda callback: lib.ecuio_export_estimate_with(
        ctypes.byref(o), asked, len(intervals), callback, None, sized), "ecuio_export_estimate_with")
    check(rc, work.call)
    return Estimate(sized[0].columns, work.missing,
                    tuple(EstimatedSize(ms, s.rows, s.bytes) for ms, s in zip(intervals, sized)))
