# File/layer: python/havne/health.py | Python 3.11+, the havne package
# Testing: PythonTest.test_health
#
# A car's health as one scrubbed file, JSON or text (ADR-0054's appendix
# "the health report as built", ecuio_health_export_with): from a
# recording's folder here, from a connected session through
# Session.health_report (session.py), which builds its options with
# options_for. The file is what havne-cli health writes for the same
# recording and options; the report's time and the latest model year are
# the caller's or the clock's, so the same inputs give the same bytes. The
# result never carries the VIN, a value or the baseline's folder.

import ctypes
import time
from collections import namedtuple
from datetime import datetime, timezone

from . import _ecuio as e
from ._asks import _Asks
from ._core import lib, utf8
from .export import _path, _whole, _word, checked, left_out_of, range_us

HealthReport = namedtuple("HealthReport", "path format source ecus codes values since_cleared "
                                          "left_out vin_decoded baseline_read settled")
HealthReport.__doc__ = ("What a health report wrote: its path, format ('json' or 'text') and "
                        "source ('recording' or 'session'), core's counts, left_out as a LeftOut "
                        "(the identification items whose text is left out), vin_decoded (a VIN "
                        "was read), baseline_read (None with no baseline given, else whether it "
                        "was read) and settled (a session's reports_settled() at the write; "
                        "None for a recording)")

FORMATS = {"json": e.ECUIO_HEALTH_JSON, "text": e.ECUIO_HEALTH_TEXT}
UNITS = {"metric": e.ECUIO_UNITS_METRIC, "imperial": e.ECUIO_UNITS_IMPERIAL}
_UINT32 = 0xFFFFFFFF


def _clock_us():
    return time.time_ns() // 1000


def _this_year_plus_one():
    return datetime.now(timezone.utc).year + 1


def options_for(out, format, units, baseline, utc_us, latest_model_year):
    """(core's health options with the fields a folder and a session share,
    the baseline as given or None, the time and model year used) after
    the package's checks. utc_us None is the clock's now and 0 writes no
    time; latest_model_year None is this UTC year plus one and 0 none. The
    options hold the strings core reads: the caller keeps the options
    alive across the call"""
    o = e.ecuio_health_options_t()
    o.format = FORMATS[_word("format", format, tuple(FORMATS))]
    o.units = UNITS[_word("units", units, tuple(UNITS))]
    if baseline is not None:
        baseline = _path("baseline", baseline)
        if not baseline:
            raise ValueError("baseline is a recording's folder, or None")
        o.baseline_folder_utf8 = utf8(baseline)
    if utc_us is None:
        utc_us = _clock_us()
    o.utc_us = _whole("utc_us", utc_us)
    if latest_model_year is None:
        latest_model_year = _this_year_plus_one()
    if _whole("latest_model_year", latest_model_year) > _UINT32:
        raise ValueError(f"latest_model_year is too large: {latest_model_year}")
    o.latest_model_year = latest_model_year
    o.out_path_utf8 = utf8(_path("out", out))
    return o, baseline, utc_us, latest_model_year


def _report(path, format, source, report, baseline, settled):
    return HealthReport(path, format, source, report.ecus, report.codes, report.values,
                        report.since_cleared, left_out_of(report.left_out),
                        bool(report.flags & e.ECUIO_HEALTH_VIN_DECODED),
                        None if baseline is None
                        else bool(report.flags & e.ECUIO_HEALTH_BASELINE_READ), settled)


def health_report(folder, out, format, *, units="metric", from_us=0, to_us=None, baseline=None,
                  utc_us=None, latest_model_year=None, cancel=None):
    """The health report of the recording in folder written to the file
    out as format, "json" or "text" (ecuio_health_export_with): its ECUs,
    codes, inspection readiness and key values over the range from_us to
    to_us (host_us, both included; None the end), in units "metric" or
    "imperial". baseline names the vehicle's baseline recording's folder
    (one that can't be read is said in the report, which is still written).
    utc_us is the report's time in microseconds since 1970 UTC (None the
    clock's now, 0 none) and latest_model_year bounds the VIN's decode
    (None this year plus one, 0 none): give both for the same bytes
    twice. Returns a HealthReport. A folder that is no recording, an out
    that exists, or a failed scrub check (CheckFailed) raises, leaving
    nothing written. cancel, a callable taking no arguments, is asked on
    the calling thread as core reads: true stops the call with Error
    ECUIO_ERR_CANCELED; an exception it raises, or a Ctrl+C on the main
    thread, stops it at core's next ask and is raised again; a Ctrl+C
    after the last ask raises once the report is whole and in place."""
    folder = _path("folder", folder)
    o, baseline, _, _ = options_for(out, format, units, baseline, utc_us, latest_model_year)
    o.from_us, o.to_us = range_us(from_us, to_us)
    o.folder_utf8 = utf8(folder)
    asks = _Asks(cancel)
    report = e.ecuio_health_report_t()
    rc = asks.run(lambda callback: lib.ecuio_health_export_with(
        ctypes.byref(o), callback, None, ctypes.byref(report)))
    checked(rc, "ecuio_health_export_with", report.failed_class)
    return _report(str(out), format, "recording", report, baseline, None)
