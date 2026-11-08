# File/layer: python/havne/session.py | Python 3.11+, the havne package
# Testing: PythonTest.test_session, test_replay, test_recording,
#          test_signal_file, test_vcan, test_j2534
#
# A session (core.h) and what a script reads from it: frames and values
# through pull queues (queue.h, ADR-0053 section 4), never a callback, so
# no Python runs on core's worker; its report, identity and support; a
# recording of it (recording.h). Session is a context manager: leaving it
# closes the session, then its streams and the recorders it opened.
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
import weakref
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8, with_profile

log = logging.getLogger("havne")

Frame = namedtuple("Frame", "timestamp_us id channel flags data")
Frame.__doc__ = ("A frame as core delivers it: session time in us, its CAN ID, "
                 "its channel, ECUIO_FRAME_FLAG_* and its payload as bytes")
Value = namedtuple("Value", "timestamp_us id signal_id name unit kind value text")
Value.__doc__ = ("A decoded value: its frame's time and ID, its def's id, name and "
                 "unit, ECUIO_VALUE_* kind, the number (an enum's raw one) and the "
                 "text (a TEXT or HEX value's, an enum's label), else None")

TEXT_KINDS = (e.ECUIO_VALUE_TEXT, e.ECUIO_VALUE_HEX)


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
    the read under way, at most its timeout."""

    BATCH = 256

    def __init__(self, session, kind, capacity, batch=BATCH):
        self.kind = kind
        self.dropped = 0
        self.ended = None   # "stream ended" or "session closed" once it has
        self._session = session
        self._queue = _Queue()
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
            entries = [self._entry(self._buf[i]) for i in range(count.value)]
        if dropped.value > self.dropped:
            what = "frames" if self.kind == e.ECUIO_QUEUE_FRAMES else "values"
            log.warning("havne: %d %s dropped so far: the script reads slower than "
                        "they arrive (a recording keeps every frame)", dropped.value, what)
            self.dropped = dropped.value
        return entries

    def __iter__(self):
        while not self.ended:
            yield from self.read(0.1)

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

    def set_focus(self, signals):
        """Asks only the requests that carry these signals, until called
        again (ADR-0056 section 2): ids or names of the session's set, in
        any order; an empty list asks the whole set again. The picks run
        in the fast class, the rest wait, parked. Before or after
        start(); on mock:// and replay:// it changes nothing. A name the
        set lacks is a KeyError; an id that isn't an int (a bool isn't) or
        is outside 0-65535 a ValueError; an id the set lacks, or a focus
        that leaves a requesting session nothing to ask, core's
        INVALID_ARGUMENT."""
        by_name = {name: sid for sid, (name, unit) in self._names.items()}
        ids = []
        for s in signals:
            if isinstance(s, str):
                if s not in by_name:
                    raise KeyError(f"no signal named {s!r} in the session's set")
                ids.append(by_name[s])
            elif isinstance(s, bool) or not isinstance(s, int):
                raise ValueError(f"a signal id is an int, not {s!r}")
            elif not 0 <= s <= 0xFFFF:
                raise ValueError(f"a signal id is 0-65535, not {s}")
            else:
                ids.append(s)
        picks = (ctypes.c_uint16 * len(ids))(*ids) if ids else None
        check(lib.ecuio_session_set_focus(self._handle(), picks, len(ids)),
              "ecuio_session_set_focus")

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
