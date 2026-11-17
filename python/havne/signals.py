# File/layer: python/havne/signals.py | Python 3.11+, the havne package
# Testing: PythonTest.test_signal_file, test_plugin, test_expr
#
# A script's own signals, as data (ADR-0053 section 5): a signal file
# (ADR-0051) or a plugin's variant (ADR-0052), loaded by core from bytes
# and registered as a profile a session's URI names. A signal file
# wrapped as a packed plugin, and a variant put into or dropped from one
# (ADR-0064), are core's calls on text: nothing is loaded or kept. The package never
# builds an ecuio_signal_def_t: a signal file reads the same across the
# set's layouts, a hand-built set doesn't.
#
# An expression against a loaded file (ADR-0062) is core's too: whether
# it compiles there as a computed signal's, the name an expression reads
# each signal by, and its value worked out from values given, with no
# session. The package compiles nothing and runs nothing itself.
#
# A loaded file stays registered, and in memory, until the process ends,
# as havne-cli's --signals does: a session may decode with its set at any
# time. Core copies what it loads (the file or plugin owns its set), and
# the package never frees a handle, so nothing on the Python side needs
# keeping alive. Loading a file of the same name again registers the new
# one in its place (the registry's last wins); a session already open
# keeps the set it opened with.

import collections
import ctypes
import itertools
import os
import threading

from . import _ecuio as e
from ._core import check, lib, text, utf8
from .errors import ExprRefused


Gauge = collections.namedtuple("Gauge", "min max warn_low warn_high")
Gauge.__doc__ = """A signal's gauge, in the signal's own unit: the range a
dial shows and the levels past which it warns, None for one not given"""

Source = collections.namedtuple("Source", "name url commit license modified share_alike")
Source.__doc__ = """Where a signal file's signals come from, its "source": name
always, each other text "" when not given; share_alike when the license
is Creative Commons' share-alike (CC-BY-SA, CC-BY-NC-SA), whose terms go
with the signals to whoever they're passed on to"""

PutVariant = collections.namedtuple("PutVariant", "text notes")
PutVariant.__doc__ = """Plugin.put_variant's answer: the packed plugin's text, and
which of the file's ECUs took another id, in core's words ("" when none
did)"""


ExprCheck = collections.namedtuple("ExprCheck", "format depth inputs leaves")
ExprCheck.__doc__ = """What check_expr found of an expression that compiles:
format the oldest "havne_signals" whose loader takes it, depth its chain's
levels (1 reading no computed signal), inputs the names it reads, in its
order, and leaves the names of the numbers under it through any computed
signal: what evaluate takes a value for"""

Evaluation = collections.namedtuple("Evaluation", "value why leaf bound_us")
Evaluation.__doc__ = """evaluate's answer: value, or None with why "missing",
"stale" or "no_number" (the steps gave none: a division by zero); leaf the
name of the first signal read without a value (None for no_number) and
bound_us the age a stale one is past (None otherwise)"""

_WHY = {e.ECUIO_EVAL_WHY_MISSING: "missing", e.ECUIO_EVAL_WHY_STALE: "stale",
        e.ECUIO_EVAL_WHY_NO_NUMBER: "no_number"}


def _defs(file):
    """The defs of file's set, each the set's own (expr_ref finds a def
    by its address)"""
    sset = lib.ecuio_signal_file_set(file).contents
    return [sset.defs[i] for i in range(sset.count)]


def _ref(file, d):
    """The name file's expressions read the def d by"""
    buf = ctypes.create_string_buffer(e.ECUIO_EXPR_REF_LEN)
    check(lib.ecuio_signal_file_expr_ref(file, ctypes.byref(d), buf, len(buf)),
          "ecuio_signal_file_expr_ref")
    return text(buf.raw)


def _expr_ref(file, signal):
    """expr_ref's answer for a signal given by its id, its name, or
    anything with an id (a session's Value, a recording's Signal)"""
    wanted = getattr(signal, "id", signal)
    for d in _defs(file):
        if (text(d.name) == wanted if isinstance(wanted, str) else d.id == wanted):
            return _ref(file, d)
    raise LookupError(f"the file has no signal {signal!r}")


def _checked(file, expr, key):
    """expr compiled as file's signal key would be: core's filled check,
    or ExprRefused"""
    data = utf8(expr)
    out = e.ecuio_expr_check_t()
    out.struct_size = ctypes.sizeof(out)
    call = "ecuio_signal_file_check_expr"
    status = lib.ecuio_signal_file_check_expr(file, utf8(key), data, len(data),
                                              ctypes.byref(out))
    if status != e.ECUIO_OK and out.at:
        raise ExprRefused(status, text(lib.ecuio_status_string(status)),
                          text(lib.ecuio_last_error_detail()), call, out.at)
    check(status, call)
    return out


def _leaf_ids(file, program):
    """The IDs of the number defs under program, in core's order"""
    count = ctypes.c_uint32()
    call = "ecuio_signal_file_expr_leaves"
    check(lib.ecuio_signal_file_expr_leaves(file, ctypes.byref(program), None, 0,
                                            ctypes.byref(count)), call)
    ids = (ctypes.c_uint16 * max(count.value, 1))()
    check(lib.ecuio_signal_file_expr_leaves(file, ctypes.byref(program), ids, count.value,
                                            ctypes.byref(count)), call)
    return list(ids[:count.value])


def _check_expr(file, expr, key):
    out = _checked(file, expr, key)
    by_id = {d.id: d for d in _defs(file)}
    program = out.program
    return ExprCheck(out.format, out.depth,
                     [_ref(file, by_id[program.input_ids[i]])
                      for i in range(program.input_count)],
                     [_ref(file, by_id[i]) for i in _leaf_ids(file, program)])


def _value_id(file, name):
    """The ID of the number a value's name reads: the name compiled
    alone, as an expression would read it, so every spelling core takes
    names its def"""
    try:
        program = _checked(file, name, None).program
    except ExprRefused as refused:
        raise ValueError(f"a value for {name!r}: {refused.detail}") from None
    if (program.input_count != 1 or program.step_count != 1
            or program.steps[0] != e.ECUIO_COMPUTED_PUSH_INPUT):
        raise ValueError(f"a value for {name!r}: a value goes to a signal by its name")
    found = program.input_ids[0]
    for d in _defs(file):
        if d.id == found and d.flags & e.ECUIO_DEF_FLAG_COMPUTED:
            raise ValueError(f"a value for {name!r}: the signal is computed: give the "
                             "signals it reads their values")
    return found


def _evaluate(file, expr, values, key):
    program = _checked(file, expr, key).program
    inputs = (e.ecuio_eval_input_t * max(len(values), 1))()
    named = {}
    for i, (name, given) in enumerate(values.items()):
        value, age_us = given if isinstance(given, tuple) else (given, 0)
        if isinstance(age_us, bool) or not isinstance(age_us, int) or not 0 <= age_us < 2**64:
            raise ValueError(f"a value for {name!r}: its age is whole microseconds, 0 to "
                             f"2**64 - 1")
        inputs[i].def_id = _value_id(file, name)
        if inputs[i].def_id in named:
            raise ValueError(f"one signal is given two values, as {named[inputs[i].def_id]!r} "
                             f"and {name!r}")
        named[inputs[i].def_id] = name
        inputs[i].value = float(value)
        inputs[i].age_us = age_us
    out = e.ecuio_eval_result_t()
    out.struct_size = ctypes.sizeof(out)
    check(lib.ecuio_signal_file_evaluate(file, ctypes.byref(program), inputs, len(values),
                                         ctypes.byref(out)), "ecuio_signal_file_evaluate")
    if out.flags & e.ECUIO_EVAL_VALUE:
        return Evaluation(out.value, None, None, None)
    why = _WHY.get(out.why, "unknown")
    leaf = None
    if out.why in (e.ECUIO_EVAL_WHY_MISSING, e.ECUIO_EVAL_WHY_STALE):
        leaf = next(_ref(file, d) for d in _defs(file) if d.id == out.leaf)
    return Evaluation(None, why, leaf,
                      out.bound_us if out.why == e.ECUIO_EVAL_WHY_STALE else None)


def _source(file):
    """The Source of a loaded signal file, or a plugin variant's file"""
    s = e.ecuio_signal_source_t()
    s.struct_size = ctypes.sizeof(s)
    check(lib.ecuio_signal_file_source(file, ctypes.byref(s)), "ecuio_signal_file_source")
    return Source(text(s.name), text(s.url), text(s.commit), text(s.license),
                  text(s.modified), bool(s.flags & e.ECUIO_SOURCE_SHARE_ALIKE))


def _made(call, status_of):
    """The text one of plugin.h's make calls gives, freed in core; an
    Error with core's words for a refusal. status_of(out, length) makes
    the call. Returns the text and the detail an OK leaves (its notes)."""
    out = ctypes.POINTER(ctypes.c_char)()
    length = ctypes.c_size_t()
    status = status_of(ctypes.byref(out), ctypes.byref(length))
    try:
        check(status, call)
        notes = text(lib.ecuio_last_error_detail())
        return ctypes.string_at(out, length.value).decode("utf-8"), notes
    finally:
        lib.ecuio_plugin_text_free(out)


def _gauge(file, key):
    """The Gauge of file's signal named key; None for no such signal, or
    one that gives no gauge"""
    found = lib.ecuio_signal_file_gauge(file, lib.ecuio_signal_file_find(file, utf8(key)))
    if not found:
        return None
    g = found.contents
    return Gauge(g.min, g.max,
                 g.warn_low if g.flags & e.ECUIO_GAUGE_WARN_LOW else None,
                 g.warn_high if g.flags & e.ECUIO_GAUGE_WARN_HIGH else None)


def _expr(file, key):
    """The expression of file's computed signal named key, as written;
    None for no such signal, or one that isn't computed"""
    found = text(lib.ecuio_signal_file_expr(file, lib.ecuio_signal_file_find(file, utf8(key))))
    return found or None


def _keys(file):
    """The key each def of file's set is kept under, as core's array"""
    sset = lib.ecuio_signal_file_set(file)
    count = ctypes.c_uint32()
    check(lib.ecuio_signal_set_keys(sset, file, None, 0, ctypes.byref(count)),
          "ecuio_signal_set_keys")
    keys = (e.ecuio_signal_key_t * count.value)()
    check(lib.ecuio_signal_set_keys(sset, file, keys, count.value, ctypes.byref(count)),
          "ecuio_signal_set_keys")
    return keys


def _key_of(file, name_or_key):
    """The key of the signal of file's set that name_or_key names"""
    keys = _keys(file)
    index = ctypes.c_uint32()
    check(lib.ecuio_signal_set_find(lib.ecuio_signal_file_set(file), keys, len(keys),
                                    utf8(name_or_key), ctypes.byref(index)),
          "ecuio_signal_set_find")
    return text(keys[index.value].text)


class SignalFile:
    """A signal file (ADR-0051), from its text or a path; registered as
    its profile ("user.<slug>") on load. recorded=True reads a recording's
    signals.json: a session's whole set, which may hold more signals and
    bytes than a file a user writes (ADR-0065 section 3)."""

    def __init__(self, text_or_bytes=None, path=None, recorded=False):
        if (text_or_bytes is None) == (path is None):
            raise ValueError("a signal file's text or its path, one of them")
        if path is not None:
            with open(path, "rb") as f:
                data = f.read()
        else:
            data = utf8(text_or_bytes)
        self._handle = ctypes.POINTER(e.ecuio_signal_file_t)()
        check(lib.ecuio_signal_file_load_with(
                  data, len(data),
                  e.ECUIO_SIGNAL_FILE_LOAD_SET if recorded else 0,
                  ctypes.byref(self._handle)),
              "ecuio_signal_file_load_with")
        self.name = text(lib.ecuio_signal_file_name(self._handle))
        self.profile = text(lib.ecuio_signal_file_profile(self._handle))
        self.extends = text(lib.ecuio_signal_file_extends(self._handle))
        self.format = lib.ecuio_signal_file_format(self._handle)
        check(lib.ecuio_profile_register(utf8(self.profile),
                                         lib.ecuio_signal_file_set(self._handle), None),
              "ecuio_profile_register")

    def gauge(self, key):
        """The Gauge of the signal named key; None for no such signal, or
        one that gives no gauge"""
        return _gauge(self._handle, key)

    def expr(self, key):
        """The expression of the computed signal named key, as the file
        writes it; None for no such signal, or one that isn't computed"""
        return _expr(self._handle, key)

    def check_expr(self, expr, key=None):
        """expr checked as a computed signal's expression in this file
        (ecuio_signal_file_check_expr): an ExprCheck when it compiles,
        ExprRefused with the character and core's words when it doesn't.
        key names the signal it is for: one of the file's keys for an
        edit, so a circle through it and what reads it today are judged;
        None for a new signal. format can be newer than the file's own:
        the file then needs that "havne_signals" to hold the expression."""
        return _check_expr(self._handle, expr, key)

    def expr_ref(self, signal):
        """The name the file's expressions read a signal by
        (ecuio_signal_file_expr_ref): its key for one of the file's own,
        "obd2.<key>" for one of obd2's in a file that extends it. signal
        is its id, its name, or anything with an id (a session's Value).
        LookupError for a signal the file's set lacks, Error for one no
        expression can name (obd2's computed Boost, a signal whose key
        starts with a digit)."""
        return _expr_ref(self._handle, signal)

    def evaluate(self, expr, values, key=None):
        """expr worked out over this file's set from values, as a
        session's decoder would at a frame (ecuio_signal_file_evaluate),
        with no session: an Evaluation. values maps a name an expression
        reads a number by to its value, in the signal's own unit, or to
        (value, age_us) for one read that long ago; a name the expression
        doesn't read is passed by. A computed signal's key is an
        expression too: evaluate("boost", values) works the file's own
        signal out, one level deeper than the signal itself, so a signal
        at the chain's bound is given by its own text with key=. key as
        check_expr takes it. ExprRefused for an
        expression that doesn't compile; ValueError for a value whose
        name reads no number of the file, or two values for one signal."""
        return _evaluate(self._handle, expr, values, key)

    @property
    def source(self):
        """Where the file's signals come from: its "source" as a Source"""
        return _source(self._handle)

    def keys(self):
        """The key each signal of the file's set is kept under, in the
        set's order (ADR-0065 section 9): its address key
        ("7E8:410C:2.0:16"), or the author's key for a computed signal
        and for the later of two at one address. The base's signals come
        first in a file that extends one."""
        return [text(k.text) for k in _keys(self._handle)]

    def key_of(self, name_or_key):
        """The key of the signal name_or_key names: the signal with that
        key, else the one signal with that name. An Error with core's
        words for text no signal has and for a name several have, which
        lists their keys."""
        return _key_of(self._handle, name_or_key)

    def __repr__(self):
        return f"SignalFile({self.name!r}, profile {self.profile!r})"


@ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
                  ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_size_t))
def _read_file(user, rel, out_bytes, out_length):
    """ecuio_plugin_read_t: a folder package's file, on the loading
    thread. user is the loading Plugin's id in _loading, so loads on two
    threads each read their own folder. Any exception is a file that
    can't be read: one escaping a ctypes callback would leave its return
    value unset."""
    try:
        with _loading_lock:
            plugin = _loading[user]
        # One byte past core's bound, so an oversized file is still
        # refused by core, in its words, without reading all of it
        with open(os.path.join(plugin._folder, rel.decode("utf-8")), "rb") as f:
            plugin._last = ctypes.create_string_buffer(
                f.read(e.ECUIO_SIGNAL_FILE_MAX_BYTES + 1))
        out_bytes[0] = ctypes.cast(plugin._last, ctypes.c_char_p)
        out_length[0] = len(plugin._last) - 1
    except BaseException:
        return 1
    return 0


_loading = {}  # id -> the Plugin loading under it
_loading_lock = threading.Lock()
_loading_ids = itertools.count(1)  # never 0: ctypes passes it as None


class Plugin:
    """A plugin (ADR-0052): a folder holding manifest.json, or a packed
    package's file. variants lists its variants' ids; register(variant)
    makes one a profile, the name it returns, which Session(plugin=,
    variant=) does for you."""

    def __init__(self, path):
        self._folder = None
        self._last = None
        self._handle = ctypes.POINTER(e.ecuio_plugin_t)()
        if os.path.isdir(path):
            self._folder = path
            with open(os.path.join(path, "manifest.json"), "rb") as f:
                manifest = f.read()
            with _loading_lock:
                load_id = next(_loading_ids)
                _loading[load_id] = self
            try:
                rc = lib.ecuio_plugin_load(manifest, len(manifest), _read_file,
                                           ctypes.c_void_p(load_id),
                                           ctypes.byref(self._handle))
            finally:
                with _loading_lock:
                    del _loading[load_id]
                self._last = None
        else:
            with open(path, "rb") as f:
                packed = f.read()
            rc = lib.ecuio_plugin_load(packed, len(packed), e.ecuio_plugin_read_t(), None,
                                       ctypes.byref(self._handle))
        check(rc, "ecuio_plugin_load")
        info = e.ecuio_plugin_info_t()
        info.struct_size = ctypes.sizeof(info)
        check(lib.ecuio_plugin_info(self._handle, ctypes.byref(info)), "ecuio_plugin_info")
        self.id, self.name, self.version = text(info.id), text(info.name), text(info.version)
        self.variants = []
        for i in range(info.variant_count):
            buf = ctypes.create_string_buffer(e.ECUIO_PLUGIN_VARIANT_ID_MAX + 1)
            check(lib.ecuio_plugin_variant(self._handle, i, buf, len(buf)),
                  "ecuio_plugin_variant")
            self.variants.append(text(buf.raw))

    def variant_index(self, variant=None):
        """A variant's index by its id; the first's when None"""
        return 0 if variant is None else self.variants.index(variant)

    def register(self, variant=None):
        """Registers a variant (its id; the first when None) and returns
        its profile's name, "plugin.<id>.<variant>" """
        index = self.variant_index(variant)
        buf = ctypes.create_string_buffer(128)
        check(lib.ecuio_plugin_register(self._handle, index, buf, len(buf)),
              "ecuio_plugin_register")
        return text(buf.raw)

    def gauge(self, key, variant=None):
        """The Gauge of a variant's signal named key (the first variant
        when None); None for no such signal, or one that gives no gauge"""
        return _gauge(lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)),
                      key)

    def expr(self, key, variant=None):
        """The expression of a variant's computed signal named key (the
        first variant when None), as written; None for no such signal, or
        one that isn't computed"""
        return _expr(lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)),
                     key)

    def check_expr(self, expr, key=None, variant=None):
        """expr checked as a computed signal's expression in a variant's
        file (the first variant when None), as SignalFile.check_expr"""
        return _check_expr(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)), expr, key)

    def expr_ref(self, signal, variant=None):
        """The name a variant's expressions read a signal by (the first
        variant when None), as SignalFile.expr_ref"""
        return _expr_ref(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)), signal)

    def evaluate(self, expr, values, key=None, variant=None):
        """expr worked out over a variant's set from values (the first
        variant when None), as SignalFile.evaluate"""
        return _evaluate(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)),
            expr, values, key)

    def keys(self, variant=None):
        """The key each signal of a variant's set is kept under (the first
        variant when None), as SignalFile.keys"""
        return [text(k.text) for k in _keys(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)))]

    def key_of(self, name_or_key, variant=None):
        """The key of the signal name_or_key names in a variant's set (the
        first variant when None), as SignalFile.key_of"""
        return _key_of(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)),
            name_or_key)

    def format(self, variant=None):
        """A variant's signal-file format (the first variant when None):
        the newest "havne_signals" among its files"""
        return lib.ecuio_signal_file_format(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)))

    def source(self, variant=None):
        """Where a variant's signals come from (the first variant when
        None): its first data file's own "source", else the manifest's,
        as a Source"""
        return _source(lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)))

    def variant_text(self, variant=None):
        """A variant (the first when None) as one signal file's text: its
        files merged, which a recording copies and SignalFile reads back
        to the variant's set, or put_variant takes into another plugin"""
        length = ctypes.c_size_t()
        raw = lib.ecuio_signal_file_text(
            lib.ecuio_plugin_variant_file(self._handle, self.variant_index(variant)),
            ctypes.byref(length))
        return (raw or b"")[:length.value].decode("utf-8")

    @property
    def lineage(self):
        """The plugin's lineage, 32 hex digits naming the signal file it
        was first made from (wrap writes it, put_variant and drop_variant
        keep it), or None when the manifest has none"""
        buf = ctypes.create_string_buffer(e.ECUIO_PLUGIN_LINEAGE_LEN + 1)
        check(lib.ecuio_plugin_lineage(self._handle, buf, len(buf)), "ecuio_plugin_lineage")
        return text(buf.raw) or None

    @staticmethod
    def wrap(signal_file, id=None, variant=None, wmi=None, years=None):
        """A signal file's text (str or bytes) as a packed plugin's text of
        one variant (ecuio_plugin_wrap): id from the file's name and the
        variant "base" unless given; a match only for wmi (manufacturer
        codes in any case, "AAA,BBB" or a list) with years (first, last)
        or none. Raises Error with core's words for what core refuses."""
        data = utf8(signal_file)
        options = e.ecuio_plugin_wrap_options_t()
        options.struct_size = ctypes.sizeof(options)
        options.id = utf8(id)
        options.variant = utf8(variant)
        if wmi is not None and not isinstance(wmi, (str, bytes)):
            wmi = ",".join(wmi)
        # bytes.upper() leaves all but ASCII alone, as havne-cli's --wmi does
        options.wmi = None if wmi is None else utf8(wmi).upper()
        if years is not None:
            options.years[0], options.years[1] = years
        made, _ = _made("ecuio_plugin_wrap", lambda out, length: lib.ecuio_plugin_wrap(
            data, len(data), ctypes.byref(options), out, length))
        return made

    @staticmethod
    def put_variant(packed, signal_file, variant=None):
        """A packed plugin's text (str or bytes) with a signal file put in
        as a variant (ecuio_plugin_put_variant): added, or in the place of
        the variant of that id; with no variant named, the file's name as
        its id. The version's minor is raised. Returns a PutVariant: the
        text, and which of the file's ECUs took another id ("" when none
        did). Raises Error with core's words for what core refuses."""
        pk = utf8(packed)
        data = utf8(signal_file)
        made, notes = _made("ecuio_plugin_put_variant",
                            lambda out, length: lib.ecuio_plugin_put_variant(
                                pk, len(pk), utf8(variant), data, len(data), out, length))
        return PutVariant(made, notes)

    @staticmethod
    def drop_variant(packed, variant):
        """A packed plugin's text (str or bytes) without the variant of
        that id, never its last (ecuio_plugin_drop_variant), the version's
        minor raised. Raises Error with core's words for what core
        refuses."""
        pk = utf8(packed)
        made, _ = _made("ecuio_plugin_drop_variant",
                        lambda out, length: lib.ecuio_plugin_drop_variant(
                            pk, len(pk), utf8(variant), out, length))
        return made

    @property
    def requires_core(self):
        """The manifest's requires.core, the oldest core that reads the
        plugin, as (major, minor, patch); a manifest always names one"""
        parts = [ctypes.c_uint16() for _ in range(3)]
        check(lib.ecuio_plugin_requires_core(self._handle, *(ctypes.byref(p) for p in parts)),
              "ecuio_plugin_requires_core")
        return tuple(p.value for p in parts)

    def __repr__(self):
        return f"Plugin({self.id!r} {self.version}, variants {self.variants})"
