# File/layer: python/havne/session.py | Python 3.11+, the havne package
# Testing: PythonTest.test_session, test_replay, test_recording,
#          test_signal_file, test_vcan, test_j2534
#
# A session (core.h) and what a script reads from it: frames and values
# through pull queues (queue.h, ADR-0053 section 4), never a callback, so
# no Python runs on core's worker; its report, identity and support; a
# recording of it (recording.h); its health report as it stands
# (health.py), from the newest value of each signal a values stream has
# read. Session is a context manager: leaving it closes the session, then
# its streams and the recorders it opened.
#
# A session left unclosed is closed in that order when it's collected, or
# at the interpreter's exit (weakref.finalize), so a notebook cell run
# again doesn't leave the last one's worker holding the adapter. The
# finalizer holds the handles, never an object that holds the session: a
# stream's queue lives in a _Queue of its own, apart from its Stream, and
# a Recorder holds no session.

import ctypes
import logging
import threading
import time
import weakref
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8, with_profile
from .export import _whole, checked
from .health import _report, options_for

log = logging.getLogger("havne")

Frame = namedtuple("Frame", "timestamp_us id channel flags data")
Frame.__doc__ = ("A frame as core delivers it: session time in us, its CAN ID, "
                 "its channel, ECUIO_FRAME_FLAG_* and its payload as bytes")
Value = namedtuple("Value", "timestamp_us id signal_id name unit kind value text")
Value.__doc__ = ("A decoded value: its frame's time and ID, its def's id, name and "
                 "unit, ECUIO_VALUE_* kind, the number (an enum's raw one) and the "
                 "text (a TEXT or HEX value's, an enum's label), else None")

ModuleSession = namedtuple("ModuleSession", "ecu asked session state nrc entries lost")
ModuleSession.__doc__ = ("One module's extended session (ADR-0060), in havne-cli's words: its "
                         "reply ID as \"0x7E8\", the session asked and the one in force "
                         "(\"03\" or \"01\"), the state (not-asked, entering, entered, refused, "
                         "unanswered, abandoned), the refusal's NRC as two hex digits (a refused "
                         "or abandoned session's, else None) and how many times the session was "
                         "entered and lost")

TEXT_KINDS = (e.ECUIO_VALUE_TEXT, e.ECUIO_VALUE_HEX)

_SESSION_STATES = {
    e.ECUIO_SESSION_STATE_NOT_ASKED: "not-asked",
    e.ECUIO_SESSION_STATE_ENTERING: "entering",
    e.ECUIO_SESSION_STATE_ENTERED: "entered",
    e.ECUIO_SESSION_STATE_REFUSED: "refused",
    e.ECUIO_SESSION_STATE_UNANSWERED: "unanswered",
    e.ECUIO_SESSION_STATE_ABANDONED: "abandoned",
}
_EXTENDED = 0x03


class _Queue:
    """A pull queue's handle and the lock its reads hold"""

    def __init__(self):
        self.q = ctypes.POINTER(e.ecuio_queue_t)()
        self.lock = threading.Lock()

    def close(self):
        with self.lock:
            if self.q is not None:
                lib.ecuio_queue_close(self.q)
                self.q = None


class Stream:
    """A pull queue of a session's frames or values. Iterating reads in
    batches, 100 ms at a time, so Ctrl+C lands within that, and ends when
    the stream does: a replay's last frame (ECUIO_ERR_STREAM_ENDED) or the
    session's close. `dropped` is the running total of entries a full queue
    dropped; each time it grows the "havne" logger says so once. One
    thread reads a stream at a time; close() from another thread waits for
    the read under way, at most its timeout. A values stream keeps the
    newest number or enumeration value of each signal its read() returned,
    and when (time.monotonic_ns), for Session.health_report; text and hex
    values are never kept."""

    BATCH = 256

    def __init__(self, session, kind, capacity, batch=BATCH):
        self.kind = kind
        self.dropped = 0
        self.ended = None   # "stream ended" or "session closed" once it has
        self._session = session
        self._queue = _Queue()
        # Apart from the queue's lock, which a read holds up to its
        # timeout: a report never waits out a read
        self._held = {}
        self._held_lock = threading.Lock()
        check(lib.ecuio_queue_open(session._handle(), kind, capacity,
                                   ctypes.byref(self._queue.q)),
              "ecuio_queue_open")
        entry = e.ecuio_frame_t if kind == e.ECUIO_QUEUE_FRAMES else e.ecuio_queue_value_t
        self._buf = (entry * batch)()
        self._batch = batch
        self._read = (lib.ecuio_queue_read_frames if kind == e.ECUIO_QUEUE_FRAMES
                      else lib.ecuio_queue_read_values)

    def read(self, timeout=0.1):
        """What's waiting, as a list, waiting up to timeout seconds for the
        first entry; [] once the stream has ended"""
        count, dropped = ctypes.c_uint32(), ctypes.c_uint64()
        with self._queue.lock:
            if self._queue.q is None or self.ended:
                self.ended = self.ended or "session closed"
                return []
            rc = self._read(self._queue.q, self._buf, self._batch, ctypes.byref(count),
                            ctypes.byref(dropped), max(0, int(timeout * 1000)))
            if rc == e.ECUIO_ERR_STREAM_ENDED:
                self.ended = "stream ended"
            elif rc == e.ECUIO_ERR_SESSION_CLOSED:
                self.ended = "session closed"
            else:
                check(rc, self._read.__name__)
            now = time.monotonic_ns()
            entries = [self._entry(self._buf[i]) for i in range(count.value)]
        if self.kind == e.ECUIO_QUEUE_VALUES and entries:
            with self._held_lock:
                for v in entries:
                    if v.kind not in TEXT_KINDS:
                        self._held[v.signal_id] = (v.value, now)
        if dropped.value > self.dropped:
            what = "frames" if self.kind == e.ECUIO_QUEUE_FRAMES else "values"
            log.warning("havne: %d %s dropped so far: the script reads slower than "
                        "they arrive (a recording keeps every frame)", dropped.value, what)
            self.dropped = dropped.value
        return entries

    def __iter__(self):
        while not self.ended:
            yield from self.read(0.1)

    def _newest(self):
        """{signal id: (value, monotonic_ns of the read that returned it)}"""
        with self._held_lock:
            return dict(self._held)

    def close(self):
        self._queue.close()
        # Ended, so an iteration stops whether it's reading or in its
        # loop body: read() returns nothing once the queue is gone
        self.ended = self.ended or "session closed"

    def _entry(self, c):
        if self.kind == e.ECUIO_QUEUE_FRAMES:
            return Frame(c.timestamp_us, c.arbitration_id, c.channel_id, c.flags,
                         ctypes.string_at(c.data, c.data_length))
        name, unit = self._session._names.get(c.signal_id, ("", ""))
        has_text = c.kind in TEXT_KINDS or (c.kind == e.ECUIO_VALUE_ENUM and c.text_len > 0)
        return Value(c.timestamp_us, c.arbitration_id, c.signal_id, name, unit, c.kind,
                     c.value, text(c.text) if has_text else None)


class Recorder:
    """A recording of a session (ADR-0049), opened before or after its
    start (ADR-0056 section 3) and counting from then; it outlives the
    session, so notes and marks go on until close()"""

    def __init__(self, session, root, label=None):
        opts = e.ecuio_record_options_t()
        opts.root_utf8 = utf8(str(root))
        opts.label_utf8 = utf8(label) if label else None
        self._r = ctypes.POINTER(e.ecuio_recorder_t)()
        check(lib.ecuio_recorder_open(ctypes.byref(opts), ctypes.byref(self._r)),
              "ecuio_recorder_open")
        try:
            # The set it decodes with goes in as signals.json, a signal
            # file's or a plugin variant's, as havne-cli's --record does
            file = session._decoding_file()
            if file:
                check(lib.ecuio_recorder_signal_file(self._r, file),
                      "ecuio_recorder_signal_file")
            check(lib.ecuio_session_record(session._handle(), self._r), "ecuio_session_record")
        except Exception:
            lib.ecuio_recorder_close(self._r, None)
            self._r = None
            raise

    @property
    def folder(self):
        n = ctypes.c_uint32()
        lib.ecuio_recorder_get_folder(self._r, None, 0, ctypes.byref(n))
        buf = ctypes.create_string_buffer(n.value + 1)
        check(lib.ecuio_recorder_get_folder(self._r, buf, n.value + 1, ctypes.byref(n)),
              "ecuio_recorder_get_folder")
        return text(buf.raw)

    def note(self, line):
        check(lib.ecuio_recorder_note(self._r, utf8(line)), "ecuio_recorder_note")

    def mark(self, label):
        check(lib.ecuio_recorder_mark(self._r, utf8(label)), "ecuio_recorder_mark")

    def close(self, why=None):
        if self._r is not None:
            r, self._r = self._r, None
            check(lib.ecuio_recorder_close(r, utf8(why) if why else None),
                  "ecuio_recorder_close")


class Session:
    """A session on uri (core.h): mock://, replay://, elm327://,
    socketcan:// (Linux), j2534:// (Windows). signals: a SignalFile whose
    profile decodes it; plugin and variant: a Plugin's variant to decode
    with. Open streams before start(); a recorder at any time, one at a
    time; set_focus() at any time."""

    def __init__(self, uri, signals=None, plugin=None, variant=None):
        if signals is not None and plugin is not None:
            raise ValueError("a session decodes through one set: signals or plugin")
        self.signal_file = signals
        self._plugin, self._variant = plugin, None
        if signals is not None:
            uri = with_profile(uri, signals.profile)
        elif plugin is not None:
            uri = with_profile(uri, plugin.register(variant))
            self._variant = plugin.variant_index(variant)
        self.uri = uri
        self._streams, self._queues, self._recorders = [], [], []
        self._s = ctypes.POINTER(e.ecuio_session_t)()
        check(lib.ecuio_session_create(utf8(uri), ctypes.byref(self._s)), "ecuio_session_create")
        self._finalizer = weakref.finalize(self, _close_session, self._s, self._queues,
                                           self._recorders)
        self._names = self._read_names()

    def _decoding_file(self):
        """The signal file whose set the session decodes with: its own
        SignalFile's, or its plugin variant's; None for neither"""
        if self.signal_file is not None:
            return self.signal_file._handle
        if self._plugin is not None:
            return lib.ecuio_plugin_variant_file(self._plugin._handle, self._variant)
        return None

    def _handle(self):
        if self._s is None:
            raise ValueError("the session is closed")
        return self._s

    def _read_names(self):
        names = {}
        sset = lib.ecuio_session_signal_set(self._s)
        if sset:
            for i in range(sset.contents.count):
                d = sset.contents.defs[i]
                names[d.id] = (text(d.name), text(d.unit))
        return names

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def frames(self, capacity=1024, batch=Stream.BATCH):
        """A stream of the session's frames, before start()"""
        s = Stream(self, e.ECUIO_QUEUE_FRAMES, capacity, batch)
        self._streams.append(s)
        self._queues.append(s._queue)
        return s

    def values(self, capacity=4096, batch=Stream.BATCH):
        """A stream of the session's decoded values, before start()"""
        s = Stream(self, e.ECUIO_QUEUE_VALUES, capacity, batch)
        self._streams.append(s)
        self._queues.append(s._queue)
        return s

    def record(self, root, label=None):
        """A recording of the session into a new folder under root, from
        now on: before start() it holds the whole session, after it the
        analysis log of a run under way (ADR-0056 section 3), its times
        counted from this call. One recorder at a time: a second while
        one is open is core's INVALID_ARGUMENT; after close() another can
        open."""
        r = Recorder(self, root, label)
        self._recorders.append(r)
        return r

    def _signal_id(self, s, where):
        """A signal's id from its name or id: a name the set lacks is a
        KeyError, an id that isn't an int (a bool isn't) or is outside
        0-65535 a ValueError"""
        if isinstance(s, str):
            found = [sid for sid, (name, _) in self._names.items() if name == s]
            if not found:
                raise KeyError(f"no signal named {s!r} in {where}")
            return found[-1]
        if isinstance(s, bool) or not isinstance(s, int):
            raise ValueError(f"a signal id is an int, not {s!r}")
        if not 0 <= s <= 0xFFFF:
            raise ValueError(f"a signal id is 0-65535, not {s}")
        return s

    def set_focus(self, signals, min_sweep_ms=0):
        """Asks only the requests that carry these signals, until called
        again (ADR-0056 section 2): ids or names of the session's set, in
        any order; an empty list asks the whole set again. The picks run
        in the fast class, the rest wait, parked. Before or after
        start(); on mock:// and replay:// it changes nothing. A name the
        set lacks is a KeyError; an id that isn't an int (a bool isn't) or
        is outside 0-65535 a ValueError; an id the set lacks, or a focus
        that leaves a requesting session nothing to ask, core's
        INVALID_ARGUMENT. min_sweep_ms: at least this long from one
        sweep's start to the next's (ADR-0056 section 4), for a slow
        adapter or a busy ECU; 0, the default, is none. One that isn't
        an int (a bool isn't) or is below 0 is a ValueError; one past
        ECUIO_FOCUS_MIN_SWEEP_MAX_MS, or one with an empty list, core's
        INVALID_ARGUMENT."""
        if (isinstance(min_sweep_ms, bool) or not isinstance(min_sweep_ms, int)
                or not 0 <= min_sweep_ms <= 0xFFFFFFFF):
            raise ValueError(f"min_sweep_ms is a whole number, not {min_sweep_ms!r}")
        ids = [self._signal_id(s, "the session's set") for s in signals]
        picks = (ctypes.c_uint16 * len(ids))(*ids) if ids else None
        check(lib.ecuio_session_set_focus_with(self._handle(), picks, len(ids),
                                               min_sweep_ms, 0),
              "ecuio_session_set_focus_with")

    def start(self):
        check(lib.ecuio_session_start(self._handle()), "ecuio_session_start")

    @property
    def signals(self):
        """{signal id: (name, unit)} of the session's set"""
        return dict(self._names)

    @property
    def info(self):
        out = e.ecuio_session_info_t()
        check(lib.ecuio_session_get_info(self._handle(), ctypes.byref(out)),
              "ecuio_session_get_info")
        return out

    def support(self, channel=0):
        out = e.ecuio_channel_support_t()
        check(lib.ecuio_session_get_channel_support(self._handle(), channel, ctypes.byref(out)),
              "ecuio_session_get_channel_support")
        return out

    def session_state(self, channel=0):
        """The channel's extended session as words (ADR-0060), the fields of
        havne-cli's session line; None while no session was asked
        (no ?session=03) or the module's report has not come in"""
        s = self.support(channel)
        if s.session_asked == 0 or s.generation == 0:
            return None
        state = _SESSION_STATES.get(s.session_state, "unknown")
        refused = s.session_state in (e.ECUIO_SESSION_STATE_REFUSED,
                                      e.ECUIO_SESSION_STATE_ABANDONED)
        return ModuleSession(f"0x{s.rx_id:X}", "03",
                             "03" if s.session == _EXTENDED else "01", state,
                             f"{s.session_nrc:02X}" if refused else None,
                             s.session_entries, s.session_lost)

    def identity(self, channel=0):
        out = e.ecuio_channel_identity_t()
        check(lib.ecuio_session_get_channel_identity(self._handle(), channel, ctypes.byref(out)),
              "ecuio_session_get_channel_identity")
        return out

    def vin(self, channel=0):
        """The channel's VIN, "" until identification reads one"""
        buf = ctypes.create_string_buffer(e.ECUIO_VIN_TEXT_LEN)
        ident = self.identity(channel)
        check(lib.ecuio_identity_vin(ctypes.byref(ident), buf, len(buf)), "ecuio_identity_vin")
        return text(buf.raw)

    def diagnostics(self, channel=0):
        """The channel's trouble-code report (ADR-0037): core's
        ecuio_channel_diagnostics_t"""
        out = e.ecuio_channel_diagnostics_t()
        check(lib.ecuio_session_get_channel_diagnostics(self._handle(), channel,
                                                        ctypes.byref(out)),
              "ecuio_session_get_channel_diagnostics")
        return out

    def clear(self, channel=0, confirm=False, engine_running=False, moving=False):
        """Asks the channel's ECU to clear its trouble codes (ADR-0038):
        Service 04, which also erases the freeze frame and sets the
        readiness monitors back to incomplete. confirm=True is the
        caller's word, as havne-cli clear --yes. Core's gate applies
        whatever is asked: the report read first must be complete and
        still the channel's, and 04 goes out only with the engine stopped
        (0 rpm) and the car standing still, unless engine_running or
        moving gives the caller's word for that too. Returns at once; the
        report's `clear` field says what became of it. The package's
        tests clear only the simulated car."""
        if not confirm:
            raise ValueError("clear() erases the car's trouble codes: pass confirm=True")
        flags = ((e.ECUIO_CLEAR_ALLOW_RUNNING if engine_running else 0) |
                 (e.ECUIO_CLEAR_ALLOW_MOVING if moving else 0))
        report = self.diagnostics(channel)
        check(lib.ecuio_session_clear_diagnostics_with(self._handle(), channel,
                                                       report.generation, flags),
              "ecuio_session_clear_diagnostics_with")
        return report.generation

    def reports_settled(self):
        """Whether every channel's diagnostics are complete or were never
        asked, and its identification is complete or has no items: the
        first half of what havne-cli health waits for, enough for a
        script's own loop. True for a session with no channels."""
        info = self.info
        for c in range(min(info.channel_count, e.ECUIO_SESSION_INFO_MAX_CHANNELS)):
            d, ident = self.diagnostics(c), self.identity(c)
            diag = bool(d.flags & e.ECUIO_DIAG_COMPLETE) or not d.flags & e.ECUIO_DIAG_ASKED
            if not diag or not (ident.flags & e.ECUIO_IDENTITY_COMPLETE or ident.items == 0):
                return False
        return True

    def _health_values(self, values, now_ns):
        """The key values for a report: the caller's (signal, value, age_us)
        triples, else the newest of each signal the values streams have
        read, each aged from its read. Returns a ctypes array and its count"""
        if values is None:
            newest = {}
            for st in self._streams:
                if st.kind == e.ECUIO_QUEUE_VALUES:
                    for sid, held in st._newest().items():
                        if sid not in newest or held[1] > newest[sid][1]:
                            newest[sid] = held
            given = [(sid, v, max(0, (now_ns - at) // 1000)) for sid, (v, at) in newest.items()]
        else:
            given = []
            for item in values:
                if not isinstance(item, (tuple, list)) or len(item) != 3:
                    raise TypeError("a value is (signal, value, age_us)")
                signal, value, age = item
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    raise TypeError(f"a value is a number, not {value!r}")
                given.append((self._signal_id(signal, "the session's set"), float(value),
                              _whole("age_us", age)))
            if len({g[0] for g in given}) != len(given):
                raise ValueError("a signal is given twice in values")
        array = (e.ecuio_health_value_t * len(given))()
        for slot, (sid, value, age) in zip(array, given):
            slot.def_id, slot.value, slot.age_us = sid, value, age
        return array, len(given)

    def health_report(self, out, format, *, units="metric", baseline=None, values=None,
                      utc_us=None, latest_model_year=None):
        """The health report of the connected session as it stands, written
        to the file out as format, "json" or "text" (ecuio_health_export):
        whatever its diagnostics and identification hold now, with no wait
        (reports_settled() says when they are in; the report's `settled` is
        its answer at the write). The key values are the newest number or
        enumeration value of each signal a values stream of the session
        has returned from read() (or been iterated), each aged from that
        read; a session with no values stream, or one never read, has none
        and the report says so. values replaces them: an iterable of
        (signal, value, age_us), the signal an id or a name of the
        session's set. baseline, utc_us, latest_model_year and units are
        health_report's. A session has no range and nothing to cancel:
        core never asks during its report. Returns a HealthReport; a
        failed scrub check raises CheckFailed, writing nothing."""
        o, baseline, _, _ = options_for(out, format, units, baseline, utc_us,
                                        latest_model_year)
        held, count = self._health_values(values, time.monotonic_ns())
        o.session = self._handle()
        if count:
            o.values = held
            o.value_count = count
        settled = self.reports_settled()
        report = e.ecuio_health_report_t()
        rc = lib.ecuio_health_export(ctypes.byref(o), ctypes.byref(report))
        checked(rc, "ecuio_health_export", report.failed_class)
        return _report(str(out), format, "session", report, baseline, settled)

    def close(self):
        """Closes the session (its worker stops), then its streams and the
        recorders it opened; destroys it. A second call does nothing."""
        if self._s is None:
            return
        self._s = None
        self._finalizer()
        for st in self._streams:
            st.close()   # each says it ended, before any read


def _close_session(s, queues, recorders):
    """Session.close()'s order, from close() or the finalizer: the
    session closed (its worker joined), its queues, the session
    destroyed, then its recorders"""
    lib.ecuio_session_close(s)
    for q in queues:
        q.close()
    lib.ecuio_session_destroy(s)
    for r in recorders:
        r.close()
