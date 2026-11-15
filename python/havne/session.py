# File/layer: python/havne/session.py | Python 3.11+, the havne package
# Testing: PythonTest.test_session, test_replay, test_recording,
#          test_signal_file, test_conditions, test_vcan, test_j2534
#
# A session (core.h) and what a script reads from it: frames, values and
# condition events through pull queues (queue.h, ADR-0053 section 4),
# never a callback, so
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
from .conditions import _WHYS, ConditionEvent, ConditionState
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

RecorderState = namedtuple("RecorderState", "state frame_rows value_rows records_dropped "
                                            "notes_dropped reason conditions_lost")
RecorderState.__doc__ = ("A recorder's state: \"active\", \"detached\" (its session destroyed; "
                         "notes go on) or \"failed\"; the rows written to frames.csv and "
                         "signals.csv; records the full ring dropped and notes the full queue "
                         "dropped; the OS's reason for a failed write, else None; and the "
                         "condition events the ring and its carry had no room for, which a "
                         "drop line in events.log names too")

_RECORDER_STATES = {e.ECUIO_RECORDING_ACTIVE: "active",
                    e.ECUIO_RECORDING_DETACHED: "detached",
                    e.ECUIO_RECORDING_FAILED: "failed"}

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

# A queue kind's entry, its read and what a dropped entry is called
_KINDS = {
    e.ECUIO_QUEUE_FRAMES: (e.ecuio_frame_t, "ecuio_queue_read_frames", "frames"),
    e.ECUIO_QUEUE_VALUES: (e.ecuio_queue_value_t, "ecuio_queue_read_values", "values"),
    e.ECUIO_QUEUE_CONDITIONS: (e.ecuio_condition_event_t, "ecuio_queue_read_conditions",
                               "condition events"),
}


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
    """A pull queue of a session's frames, values or condition events
    (ConditionEvent, each with its condition's key). Iterating reads in
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
        entry, read, self._what = _KINDS[kind]
        self._buf = (entry * batch)()
        self._batch = batch
        self._read = getattr(lib, read)

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
            log.warning("havne: %d %s dropped so far: the script reads slower than "
                        "they arrive (a recording keeps every frame)", dropped.value,
                        self._what)
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
        if self.kind == e.ECUIO_QUEUE_CONDITIONS:
            keys = self._session._condition_keys.get(c.generation, ())
            return ConditionEvent(
                c.timestamp_us, keys[c.condition] if c.condition < len(keys) else "",
                "on" if c.state == e.ECUIO_CONDITION_ON else "off",
                bool(c.flags & e.ECUIO_CONDITION_FIRED),
                bool(c.flags & e.ECUIO_CONDITION_STALE), c.value, c.term, c.generation)
        name, unit = self._session._names.get(c.signal_id, ("", ""))
        has_text = c.kind in TEXT_KINDS or (c.kind == e.ECUIO_VALUE_ENUM and c.text_len > 0)
        return Value(c.timestamp_us, c.arbitration_id, c.signal_id, name, unit, c.kind,
                     c.value, text(c.text) if has_text else None)


class Recorder:
    """A recording of a session (ADR-0049), opened before or after its
    start (ADR-0056 section 3) and counting from then; it outlives the
    session, so notes and marks go on until close(); state() says what it
    wrote and lost"""

    def __init__(self, session, root, label=None):
        self._closed_state = None
        # Every use of the handle and close() hold it, so no call runs on a freed one;
        # reentrant: the session's finalizer may close it on a thread that holds it
        self._lock = threading.RLock()
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
        with self._lock:
            lib.ecuio_recorder_get_folder(self._r, None, 0, ctypes.byref(n))
            buf = ctypes.create_string_buffer(n.value + 1)
            check(lib.ecuio_recorder_get_folder(self._r, buf, n.value + 1, ctypes.byref(n)),
                  "ecuio_recorder_get_folder")
        return text(buf.raw)

    def note(self, line):
        with self._lock:
            check(lib.ecuio_recorder_note(self._r, utf8(line)), "ecuio_recorder_note")

    def mark(self, label):
        with self._lock:
            check(lib.ecuio_recorder_mark(self._r, utf8(label)), "ecuio_recorder_mark")

    @staticmethod
    def _read_state(r):
        s = e.ecuio_recorder_state_t()
        check(lib.ecuio_recorder_get_state(r, ctypes.byref(s)), "ecuio_recorder_get_state")
        return RecorderState(_RECORDER_STATES.get(s.state, "unknown"), s.frame_rows,
                             s.value_rows, s.records_dropped, s.notes_dropped,
                             text(s.reason) if s.state == e.ECUIO_RECORDING_FAILED else None,
                             s.conditions_lost)

    def state(self):
        """Its RecorderState now; after close(), the one the close read
        first. The rows are what the writer had written then (the close
        writes the rest); the drops and conditions_lost are counted as
        records go in, so they are final once the session was destroyed
        before the close, as Session.close() does"""
        with self._lock:
            if self._r is None:
                return self._closed_state
            return self._read_state(self._r)

    def close(self, why=None):
        """Closes the recording (its files end whole, with the summary)
        and returns the state read just before; None when it was closed
        already. A write that failed raises Error after the close; so does
        a state that couldn't be read, the recording closed all the same."""
        with self._lock:
            if self._r is None:
                return None
            r, self._r = self._r, None
            try:
                self._closed_state = self._read_state(r)
            finally:
                check(lib.ecuio_recorder_close(r, utf8(why) if why else None),
                      "ecuio_recorder_close")
            return self._closed_state


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
        # Each attach's keys by its generation, so an event names its own:
        # the newest KEPT_GENERATIONS attaches', under the lock that
        # orders the attaches
        self._condition_keys = {}
        self._conditions_generation = 0
        self._conditions_lock = threading.Lock()
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

    #: How many attaches' keys a session keeps for the events still unread
    KEPT_GENERATIONS = 64

    def conditions(self, capacity=1024, batch=Stream.BATCH):
        """A stream of the events of the conditions attached
        (set_conditions), each a ConditionEvent; before start()"""
        s = Stream(self, e.ECUIO_QUEUE_CONDITIONS, capacity, batch)
        self._streams.append(s)
        self._queues.append(s._queue)
        return s

    def set_conditions(self, conditions):
        """Attaches a ConditionSet (ADR-0063), judged from the next frame
        against the session's signals; None takes the conditions away.
        Before or after start(). Every state starts off and stale. key:
        references bind through the session's own signal file or plugin
        variant. While a recording is open it is core's IN_USE: a
        recording holds one set for its whole length (ADR-0063 section
        12's step 7 decides a change of set within one). An event read
        more than KEPT_GENERATIONS attaches after its own has "" for a
        key."""
        handle = None
        keys = ()
        if conditions is not None:
            if not conditions._finalizer.alive:
                raise ValueError("the condition set is closed")
            handle = conditions._handle
            keys = tuple(c.key for c in conditions.conditions)
        session, file = self._handle(), self._decoding_file()
        with self._conditions_lock:
            # Core counts each attach that succeeds. The keys go in
            # before the call: a reader on another thread may meet the
            # attach's first event before the call returns
            generation = self._conditions_generation + 1
            self._condition_keys[generation] = keys
            rc = None
            try:
                rc = lib.ecuio_session_set_conditions(session, handle, file)
            finally:
                # Core's answer alone says whether the set is attached,
                # whatever interrupts this thread before check() raises
                if rc == e.ECUIO_OK:
                    self._conditions_generation = generation
                    self._condition_keys.pop(generation - self.KEPT_GENERATIONS, None)
                else:
                    del self._condition_keys[generation]
            check(rc, "ecuio_session_set_conditions")

    def condition_states(self):
        """A ConditionState for each condition attached, in the set's
        order; [] with none"""
        count = ctypes.c_uint32()
        states, room = None, 0
        while True:
            check(lib.ecuio_session_get_condition_states(self._handle(), states, room,
                                                         ctypes.byref(count)),
                  "ecuio_session_get_condition_states")
            if count.value <= room:
                break
            # More than there was room for: an attach on another thread
            # may have grown the set since the last look
            room = count.value
            states = (e.ecuio_condition_state_t * room)()
        out = []
        for i in range(count.value):
            s = states[i]
            keys = self._condition_keys.get(s.generation, ())
            unavailable = bool(s.flags & e.ECUIO_CONDITION_UNAVAILABLE)
            out.append(ConditionState(
                keys[i] if i < len(keys) else "",
                "on" if s.state == e.ECUIO_CONDITION_ON else "off",
                bool(s.flags & e.ECUIO_CONDITION_STALE), not unavailable,
                _WHYS.get(s.why, "unknown") if unavailable else None,
                s.since_us, s.fired, s.value, s.generation))
        return out

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

    def set_focus(self, signals, min_sweep_ms=0, default_pick=False):
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
        INVALID_ARGUMENT. default_pick: the focus is a front end's default
        pick, not a log's (ECUIO_FOCUS_DEFAULT_PICK), so a recording's
        header says "pick" in place of "focus"; with an empty list it is
        core's INVALID_ARGUMENT (an empty pick sets no focus)."""
        if (isinstance(min_sweep_ms, bool) or not isinstance(min_sweep_ms, int)
                or not 0 <= min_sweep_ms <= 0xFFFFFFFF):
            raise ValueError(f"min_sweep_ms is a whole number, not {min_sweep_ms!r}")
        ids = [self._signal_id(s, "the session's set") for s in signals]
        picks = (ctypes.c_uint16 * len(ids))(*ids) if ids else None
        flags = e.ECUIO_FOCUS_DEFAULT_PICK if default_pick else 0
        check(lib.ecuio_session_set_focus_with(self._handle(), picks, len(ids),
                                               min_sweep_ms, flags),
              "ecuio_session_set_focus_with")

    def ranked_signals(self, added=()):
        """The session's default pick as the app and havne-cli's --pick
        ranked make it (cli/default_pick.h): the set's ranked signals
        (plot_rank nonzero; a text or hex signal's rank isn't counted) and
        the ids or names added, in the set's order, each once. Empty when
        the set ranks none (an empty pick sets no focus, and the whole set
        is asked) and on a session that asks nothing (the mock, a replay, a
        listening session), where neither front end sets a pick. A name
        the set lacks is a KeyError, as in set_focus. Pass a list that
        isn't empty to set_focus(..., default_pick=True)."""
        extra = {self._signal_id(s, "the session's set") for s in added}
        if not self.info.flags & e.ECUIO_SESSION_INFO_REQUESTS:
            return []
        sset = lib.ecuio_session_signal_set(self._handle())
        if not sset:
            return []
        defs = [sset.contents.defs[i] for i in range(sset.contents.count)]

        def ranked(d):
            return d.plot_rank != 0 and d.kind not in TEXT_KINDS
        if not any(ranked(d) for d in defs):
            return []
        return [d.id for d in defs if ranked(d) or d.id in extra]

    def _listed(self, getter, name):
        """The ids a count-and-buffer getter lists (capacity 0 asks the
        count; a list that grew between the two calls is read again)"""
        handle = self._handle()
        count = ctypes.c_uint32(0)
        check(getter(handle, None, 0, ctypes.byref(count)), name)
        while True:
            room = count.value
            ids = (ctypes.c_uint16 * max(room, 1))()
            check(getter(handle, ids, room, ctypes.byref(count)), name)
            if count.value <= room:
                return list(ids[:count.value])

    def unsupported_signals(self):
        """The ids, in the set's order, of the Service 01 signals their ECU
        says it doesn't support (ecuio_session_get_unsupported_signals):
        the PID's bitmap leaves it out, and a computed signal with such an
        input is listed with it. Empty until a bitmap has been answered
        and on a profile with no Service 01 signals. mock:// walks its demo
        car's bitmaps at create, so under profile=obd2 it lists the set's
        signals the car doesn't answer."""
        return self._listed(lib.ecuio_session_get_unsupported_signals,
                            "ecuio_session_get_unsupported_signals")

    def absent_signals(self):
        """The ids, in the set's order, of the signals the session's frames
        name but don't carry (ecuio_session_get_absent_signals): a field
        past the end of every reply for ECUIO_ABSENT_AFTER_FRAMES frames,
        as an ECU that answers PID 55 with bank 1's byte alone; a signal
        that arrives later leaves the list. Judged from the values decoded
        for a values stream, so with none open (values() before start())
        it is empty."""
        return self._listed(lib.ecuio_session_get_absent_signals,
                            "ecuio_session_get_absent_signals")

    def unasked_signals(self):
        """The ids, in the set's order, of the signals the focus leaves
        unasked (ecuio_session_get_unasked_signals): those whose request
        it parks, and a computed one with such an input. Empty with no
        focus and on a session that asks nothing (mock://, replay://, a
        listener); the worker judges each focus as it takes it, so a list
        read at once after set_focus may still be the last one's."""
        return self._listed(lib.ecuio_session_get_unasked_signals,
                            "ecuio_session_get_unasked_signals")

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
        """The channel's VIN, "" until core has it in (ECUIO_IDENTITY_VIN:
        09 02 answered with one, or F190 did once 09 02 settled), which
        may be long before the rest of the identification is complete"""
        ident = self.identity(channel)
        if not ident.flags & e.ECUIO_IDENTITY_VIN:
            return ""
        buf = ctypes.create_string_buffer(e.ECUIO_VIN_TEXT_LEN)
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
