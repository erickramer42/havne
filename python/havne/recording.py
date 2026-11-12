# File/layer: python/havne/recording.py | Python 3.11+, the havne package
# Testing: PythonTest.test_recording, PythonTest.test_import,
#          PythonTest.test_frames_read, PythonTest.test_recording_aids
#
# A recording read offline (ADR-0049 section 5, recording.h): its
# signals with their ranges, its markers, range queries, the value in
# force at a time, and finding a threshold's crossing or a marker. Times
# are the recording's host_us, microseconds from its start. import_csv and
# import_candump make a recording's folder from a logger's file
# (ADR-0054, ecuio_import_with), stopped by a cancel or a Ctrl+C with
# nothing left made; guess_csv_map guesses a CSV's column map from its
# header (ecuio_import_csv_guess), which import_csv uses without one.
# read_frames reads its frames.csv back as recorded (ADR-0049's appendix
# "frames read back"). stats, step, role_signal and stretches are the
# chart's span statistics and cursor step and ADR-0056's navigation aids.

import ctypes
import json
import weakref
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8
from ._asks import _Asks
from .errors import Error

Signal = namedtuple("Signal", "index name unit kind samples lowest highest first_us last_us "
                              "decimal_places group")
Signal.__doc__ = ("A recording's signal: kind 'number', 'text' or 'enum'; a number's range; "
                  "first_us and last_us None without samples")
Marker = namedtuple("Marker", "host_us label")
Column = namedtuple("Column", "lowest highest samples")
Imported = namedtuple("Imported", "folder lines frames error_frames interfaces values "
                                  "columns_ignored utc_start_us duration_us rows_no_time")
Imported.__doc__ = ("What an import made: its folder and core's counts; utc_start_us None "
                    "when the file gave no UTC time; rows_no_time a CSV's rows left out "
                    "for an empty time cell")
RecordedFrame = namedtuple("RecordedFrame", "host_us timestamp_us id flags data")
RecordedFrame.__doc__ = ("A frames.csv row as recorded: its host_us (the time Recording's "
                         "queries take), its session time in us, its CAN ID, "
                         "ECUIO_FRAME_FLAG_* and its payload as bytes")
GuessedMap = namedtuple("GuessedMap", "map notes flags columns")
GuessedMap.__doc__ = ("A CSV's column map as core guessed it: map the column map (a dict "
                      "import_csv takes), notes its lines of words, flags ECUIO_CSV_GUESS_*, "
                      "columns each header column as {'column', 'used'} and, left out, 'why'")

Stats = namedtuple("Stats", "samples first last lowest highest lowest_us highest_us mean")
Stats.__doc__ = ("A signal's statistics over a span: first and last the values in force at "
                 "its ends; lowest, highest and mean leaving a NaN out, each NaN with no "
                 "sample; lowest_us and highest_us the first sample at each, or None")
Stretch = namedtuple("Stretch", "kind signal from_us to_us start end flags")
Stretch.__doc__ = ("A pull or a cruise: kind 'pull' or 'cruise', signal the measured one's "
                   "name, start and end a pull's two ends (a cruise's mean in both), flags "
                   "ECUIO_RECORDING_STRETCH_*")

NONE = e.ECUIO_RECORDING_NONE
_NO_INDEX = 0xFFFFFFFF
_ROLES = {"rpm": e.ECUIO_RECORDING_ROLE_ENGINE_SPEED,
          "speed": e.ECUIO_RECORDING_ROLE_VEHICLE_SPEED,
          "throttle": e.ECUIO_RECORDING_ROLE_THROTTLE,
          "load": e.ECUIO_RECORDING_ROLE_ENGINE_LOAD}
_ROLE_FIELDS = {"rpm": "engine_speed", "speed": "vehicle_speed", "throttle": "throttle",
                "load": "engine_load"}
_KINDS = {"pulls": e.ECUIO_STRETCH_PULLS, "cruises": e.ECUIO_STRETCH_CRUISES,
          "all": e.ECUIO_STRETCH_PULLS | e.ECUIO_STRETCH_CRUISES}
_LEVELS = ("pull_level", "pull_load_level", "pull_hold_us", "pull_gap_us", "pull_min_rise",
           "pull_min_gain", "cruise_hold_us", "cruise_speed_band", "cruise_throttle_band",
           "cruise_floor")


def _role(word):
    if word not in _ROLES:
        raise ValueError(f"a role is 'throttle', 'load', 'speed' or 'rpm', not {word!r}")
    return _ROLES[word]


class Recording:
    """The recording in folder, opened once. With profile (a registered
    profile's name) or signals (a SignalFile), its values are decoded from
    its frames.csv through that set instead of read from signals.csv
    (ecuio_recording_open_frames). folder is the folder as given, as a str
    (what the export functions take). A context manager."""

    def __init__(self, folder, profile=None, signals=None):
        self.folder = str(folder)
        self._r = ctypes.POINTER(e.ecuio_recording_t)()
        if signals is not None and profile is not None:
            raise ValueError("a recording decodes through one set: profile or signals")
        if signals is None and profile is None:
            rc = lib.ecuio_recording_open(utf8(str(folder)), e.ecuio_open_progress_t(0), None,
                                          ctypes.byref(self._r))
            check(rc, "ecuio_recording_open")
        else:
            if signals is not None:
                # The file's own set: the registry's, under its profile's
                # name, may be a later file of the same name
                sset = lib.ecuio_signal_file_set(signals._handle)
            else:
                sset = ctypes.POINTER(e.ecuio_signal_set_t)()
                check(lib.ecuio_profile_lookup(utf8(profile), ctypes.byref(sset)),
                      "ecuio_profile_lookup")
            rc = lib.ecuio_recording_open_frames(utf8(str(folder)), sset,
                                                 e.ecuio_open_progress_t(0), None,
                                                 ctypes.byref(self._r))
            check(rc, "ecuio_recording_open_frames")
        # Left unclosed, closed when collected or at the interpreter's exit
        self._finalizer = weakref.finalize(self, _close, self._r)
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
            self._r = None
            self._finalizer()

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

    def stats(self, signals, t0_us, t1_us):
        """Each signal's (names or indexes) Stats over the closed span
        [t0_us, t1_us], a sample at t1_us counted; a text signal's has its
        samples alone, the numbers NaN"""
        ids = [self.signal(s).index for s in signals]
        idx = (ctypes.c_uint32 * len(ids))(*ids)
        out = (e.ecuio_recording_stats_t * len(ids))()
        check(lib.ecuio_recording_stats(self._r, idx, len(ids), t0_us, t1_us, out),
              "ecuio_recording_stats")
        return [Stats(s.samples, s.first, s.last, s.lowest, s.highest,
                      None if s.lowest_us == NONE else s.lowest_us,
                      None if s.highest_us == NONE else s.highest_us, s.mean) for s in out]

    def step(self, signal, t_us, forward=True):
        """The signal's first sample strictly after t_us (or before it,
        forward False); None past the last or before the first"""
        at = ctypes.c_uint64()
        way = e.ECUIO_RECORDING_STEP_FORWARD if forward else e.ECUIO_RECORDING_STEP_BACKWARD
        check(lib.ecuio_recording_step(self._r, self.signal(signal).index, t_us, way,
                                       ctypes.byref(at)),
              "ecuio_recording_step")
        return None if at.value == NONE else at.value

    def role_signal(self, role):
        """The Signal core picks for a role ('throttle', 'load', 'speed' or
        'rpm') by its unit and name; None if none plays it"""
        index = ctypes.c_uint32()
        check(lib.ecuio_recording_role_signal(self._r, _role(role), ctypes.byref(index)),
              "ecuio_recording_role_signal")
        return None if index.value == _NO_INDEX else self.signals[index.value]

    def stretches(self, kinds="all", from_us=0, to_us=None, roles=None, **levels):
        """The pulls and cruises (kinds 'pulls', 'cruises' or 'all') within
        the closed range [from_us, to_us] (to_us the recording's end by
        default; an empty default range answers []), in time order, as
        Stretch. roles maps a role ('throttle',
        'load', 'speed', 'rpm') to the signal that plays it, in place of
        core's pick; levels are ecuio_stretch_rules_t's own fields in its
        units (pull_level=85, pull_gap_us=750_000, pull_min_rise in rpm,
        pull_min_gain and cruise_floor in km/h), each left out taking
        core's default"""
        rules = e.ecuio_stretch_rules_t()
        if kinds not in _KINDS:
            raise ValueError(f"kinds is 'pulls', 'cruises' or 'all', not {kinds!r}")
        rules.kinds = _KINDS[kinds]
        for field in _ROLE_FIELDS.values():
            setattr(rules, field, _NO_INDEX)
        for role, signal in (roles or {}).items():
            _role(role)
            setattr(rules, _ROLE_FIELDS[role], self.signal(signal).index)
        for name, value in levels.items():
            if name not in _LEVELS:
                raise TypeError(f"stretches() has no level {name!r}")
            setattr(rules, name, value)
        count = ctypes.c_uint32()
        if to_us is None and self.info.duration_us <= from_us:
            # Nothing past from_us (a recording whose samples sit at one
            # time): core refuses an empty range, so it judges the rules
            # over a microsecond and the answer is no stretch
            check(lib.ecuio_recording_stretches(self._r, ctypes.byref(rules), from_us,
                                                from_us + 1, None, 0, ctypes.byref(count)),
                  "ecuio_recording_stretches")
            return []
        if to_us is None:
            to_us = self.info.duration_us
        check(lib.ecuio_recording_stretches(self._r, ctypes.byref(rules), from_us, to_us,
                                            None, 0, ctypes.byref(count)),
              "ecuio_recording_stretches")
        if count.value == 0:
            return []
        capacity = count.value
        out = (e.ecuio_recording_stretch_t * capacity)()
        check(lib.ecuio_recording_stretches(self._r, ctypes.byref(rules), from_us, to_us,
                                            out, capacity, ctypes.byref(count)),
              "ecuio_recording_stretches")
        return [Stretch("pull" if s.kind == e.ECUIO_RECORDING_STRETCH_PULL else "cruise",
                        self.signals[s.signal].name, s.from_us, s.to_us, s.start, s.end,
                        s.flags)
                for s in out[:min(count.value, capacity)]]


def _import(options, folder, cancel):
    report = e.ecuio_import_report_t()
    asks = _Asks(cancel)
    check(asks.run(lambda callback: lib.ecuio_import_with(
        ctypes.byref(options), callback, None, ctypes.byref(report))), "ecuio_import_with")
    return Imported(folder, report.lines, report.frames, report.error_frames,
                    report.interfaces, report.values, report.columns_ignored,
                    report.utc_start_us or None, report.duration_us, report.rows_no_time)


def _guessed(path, time_unit):
    """core's guess: its whole text and its report"""
    if time_unit not in (None, "s", "ms", "us"):
        raise ValueError("time_unit is 's', 'ms', 'us' or None")
    unit = None if time_unit is None else utf8(time_unit)
    report = e.ecuio_csv_guess_t()
    needed = ctypes.c_size_t()
    check(lib.ecuio_import_csv_guess(utf8(str(path)), unit, None, 0, ctypes.byref(needed),
                                     ctypes.byref(report)), "ecuio_import_csv_guess")
    for _ in range(4):  # a file grown between the calls is read again
        buf = ctypes.create_string_buffer(needed.value)
        now = ctypes.c_size_t()
        check(lib.ecuio_import_csv_guess(utf8(str(path)), unit, buf, len(buf),
                                         ctypes.byref(now), ctypes.byref(report)),
              "ecuio_import_csv_guess")
        if now.value <= len(buf):
            return buf.raw[:now.value - 1], report
        needed = now
    raise Error(e.ECUIO_ERR_LOAD_FAILED, text(lib.ecuio_status_string(e.ECUIO_ERR_LOAD_FAILED)),
                "the file changed while it was read", "ecuio_import_csv_guess")


def guess_csv_map(path, time_unit=None):
    """The column map core guesses from the header of the CSV at path
    (ADR-0054's appendix "a column map guessed from the header"), as a
    GuessedMap. A time column of plain numbers takes time_unit ('s', 'ms'
    or 'us') when given, else the unit its header names, else 'ms' for a
    column named as a time with whole numbers alone, which a note says;
    otherwise (a first column that only rises, a time with fractions, ISO
    8601 with no offset, times of day) the time has none and flags lacks
    ECUIO_CSV_GUESS_TIME_UNIT_KNOWN.
    Reads the file only; a file it can't guess raises Error with core's
    words."""
    raw, report = _guessed(path, time_unit)
    whole = json.loads(raw.decode("utf-8"))
    return GuessedMap(whole["map"], whole["notes"], report.flags, whole["columns"])


def import_csv(path, folder, column_map=None, time_unit=None, cancel=None):
    """A logger's CSV at path made into a recording's folder (signals.csv
    and events.log) at folder, which must not exist yet, by column_map: a
    dict or its JSON text (ADR-0054's column map: the time column and each
    signal's column, name and unit). With no column_map, the map
    guess_csv_map(path, time_unit) gives, taken as core wrote it; one whose
    time has no unit raises Error before anything is made. Returns an
    Imported; a refused file or map raises Error with core's words, leaving
    nothing made.
    cancel, a callable taking no arguments, is asked on the calling
    thread as core reads: true stops the import with Error
    ECUIO_ERR_CANCELED. An exception it raises, or a Ctrl+C on the main
    thread (KeyboardInterrupt), stops it at core's next ask and is raised
    again. Stopped either way, nothing is left made; a Ctrl+C after the
    import's last ask raises once the recording is whole."""
    if column_map is None:
        raw, report = _guessed(path, time_unit)
        if not report.flags & e.ECUIO_CSV_GUESS_TIME_UNIT_KNOWN:
            # time_unit gives a unit only to plain numbers
            words = ("the guessed map's time has no unit: give time_unit 's', 'ms' or 'us', "
                     "or a column_map"
                     if report.flags & e.ECUIO_CSV_GUESS_TIME_NUMBERS else
                     "the guessed map's time has no unit a guess can give: its cells need a "
                     "column_map with the zone (or the date and offset)")
            raise Error(e.ECUIO_ERR_LOAD_FAILED,
                        text(lib.ecuio_status_string(e.ECUIO_ERR_LOAD_FAILED)),
                        words, "ecuio_import_csv_guess")
        column_map = raw[report.map_offset:report.map_offset + report.map_length].decode("utf-8")
    elif time_unit is not None:
        raise ValueError("time_unit is a guessed map's: a column_map names its own")
    elif isinstance(column_map, dict):
        column_map = json.dumps(column_map)
    elif not isinstance(column_map, str):
        raise TypeError("column_map is a dict or its JSON text")
    o = e.ecuio_import_options_t()
    o.format = e.ECUIO_IMPORT_CSV
    o.in_path_utf8 = utf8(str(path))
    o.out_folder_utf8 = utf8(str(folder))
    o.column_map_utf8 = utf8(column_map)
    return _import(o, str(folder), cancel)


def import_candump(path, folder, interface=None, cancel=None):
    """A candump -L log at path made into a recording's folder (frames.csv
    and events.log) at folder, which must not exist yet; interface names
    the bus when the log holds several. Returns an Imported; a refused log
    raises Error with core's words, leaving nothing made.
    cancel, a callable taking no arguments, is asked on the calling
    thread as core reads: true stops the import with Error
    ECUIO_ERR_CANCELED. An exception it raises, or a Ctrl+C on the main
    thread (KeyboardInterrupt), stops it at core's next ask and is raised
    again. Stopped either way, nothing is left made; a Ctrl+C after the
    import's last ask raises once the recording is whole."""
    o = e.ecuio_import_options_t()
    o.format = e.ECUIO_IMPORT_CANDUMP
    o.in_path_utf8 = utf8(str(path))
    o.out_folder_utf8 = utf8(str(folder))
    o.interface_utf8 = None if interface is None else utf8(interface)
    return _import(o, str(folder), cancel)


def read_frames(folder, from_us=0, to_us=0, limit=None):
    """The recording's frames.csv rows whose host_us is from_us to to_us,
    both included (to_us 0: to the end), as recorded: a list of
    RecordedFrame in the file's order, at most limit of them. Nothing is
    decoded or scrubbed; a last row cut short is left out. folder is the
    recording's folder or its frames.csv. A refused file or row raises
    Error with core's words (ecuio_recording_read_frames)."""
    if limit is not None and limit < 1:
        raise ValueError("limit is 1 or more, or None")
    rows = []
    failed = []

    def take(_user, frame, host_us):
        # An exception escaping a ctypes callback would leave its return
        # value unset: it stops the read and is raised after it
        try:
            f = frame.contents
            rows.append(RecordedFrame(host_us, f.timestamp_us, f.arbitration_id, f.flags,
                                      ctypes.string_at(f.data, f.data_length)))
            return 1 if limit is not None and len(rows) >= limit else 0
        except BaseException as exc:  # noqa: BLE001 (raised again below)
            failed.append(exc)
            return 1

    callback = e.ecuio_frame_row_callback_t(take)
    summary = e.ecuio_frames_read_t()
    rc = lib.ecuio_recording_read_frames(utf8(str(folder)), from_us, to_us, callback, None,
                                         ctypes.byref(summary))
    if failed:
        raise failed[0]
    if rc != e.ECUIO_ERR_CANCELED:
        check(rc, "ecuio_recording_read_frames")
    return rows


def _close(r):
    lib.ecuio_recording_close(r)
