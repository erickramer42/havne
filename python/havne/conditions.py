# File/layer: python/havne/conditions.py | Python 3.11+, the havne package
# Testing: PythonTest.test_conditions
#
# A condition file (ADR-0063), loaded by core from bytes: what each
# condition holds, and how its references meet a set of signals. The
# package never reads the JSON itself: what core's loader refuses is
# refused here in its words, and what it reads is what a session judges
# (Session.set_conditions, whose events and states are named here).

import collections
import ctypes
import weakref

from . import _ecuio as e
from ._core import check, lib, text, utf8


Checks = collections.namedtuple("Checks", "gap_us rate_drop settle_us")
Checks.__doc__ = """A condition file's "checks": the frames check's
thresholds for its check terms, None for one the file leaves to the
check's default"""

Alert = collections.namedtuple("Alert", "level sound banner")
Alert.__doc__ = """A condition's alert: its level ("warning" or "danger")
and whether it sounds and shows a banner"""

Record = collections.namedtuple("Record", "before_us after_us")
Record.__doc__ = """A condition's recording: the time kept from before a
firing and written after the condition turns OFF"""

Term = collections.namedtuple("Term", "kind signal unit test level clear max_age_us")
Term.__doc__ = """One term. kind is "signal", "link" or "check". A signal
term has its references (signal, as the file wrote them), unit (None when
the file wrote none), test ("above", "below", "equals" or "differs"),
level, clear (None with none) and max_age_us (None for the default
bound). A link or a check term has its state or kind in test and nothing
else."""

Condition = collections.namedtuple(
    "Condition", "key name enabled join terms for_us clear_for_us alert record mark")
Condition.__doc__ = """A condition as its file wrote it: join is "all" or
"any" over terms; alert and record are None when it has none"""

Resolved = collections.namedtuple("Resolved", "reference id")
Resolved.__doc__ = """The reference a signal term took in a set of
signals, and the id of the signal it names there"""

Binding = collections.namedtuple("Binding", "available why why_term terms")
Binding.__doc__ = """How a condition meets a set of signals: whether it can
be judged there, else why not ("disabled", "no_signal", "unit",
"not_a_number" or "enum_order") and the first term that doesn't bind
(None when no one term is the reason); terms holds a Resolved for each
signal term that named a signal, None for any other"""

ConditionEvent = collections.namedtuple(
    "ConditionEvent", "timestamp_us key state fired stale value term generation")
ConditionEvent.__doc__ = """One change of a condition in a session: its
session time (a hold's end or an age bound is stamped with its own time,
earlier than the frame that settled it), the condition's key, state ("on"
or "off"), whether this change fired it (OFF to ON) and whether an input is
too old to judge (the state is then the one kept), the sample of the term
that settled it, that term's index, and the attach the event belongs to
(Session.set_conditions counts them from 1)"""

ConditionState = collections.namedtuple(
    "ConditionState", "key state stale available why since_us fired value generation")
ConditionState.__doc__ = """A condition's state in a session: "on" or
"off", stale, whether the session can judge it and else why not (a
Binding's words, and "no_link" and "no_check" for a term the session
doesn't judge), the time of its last event, its firings since the attach
and the sample that settled its last event"""

_TESTS = {e.ECUIO_TERM_ABOVE: "above", e.ECUIO_TERM_BELOW: "below",
          e.ECUIO_TERM_EQUALS: "equals", e.ECUIO_TERM_DIFFERS: "differs"}
_LINKS = {e.ECUIO_TERM_LINK_NOT_OK: "not_ok", e.ECUIO_TERM_LINK_TIMEOUT: "timeout",
          e.ECUIO_TERM_LINK_REQUEST_FAILED: "request_failed",
          e.ECUIO_TERM_LINK_HARD_FAILURE: "hard_failure"}
_CHECKS = {e.ECUIO_CHECK_GAP: "gap", e.ECUIO_CHECK_RATE_DROP: "rate_drop",
           e.ECUIO_CHECK_LATE_ID: "late_id", e.ECUIO_CHECK_OVERFLOW: "overflow",
           e.ECUIO_CHECK_ISOTP: "isotp"}
_WHYS = {e.ECUIO_CONDITION_WHY_DISABLED: "disabled",
         e.ECUIO_CONDITION_WHY_NO_SIGNAL: "no_signal",
         e.ECUIO_CONDITION_WHY_UNIT: "unit",
         e.ECUIO_CONDITION_WHY_NOT_A_NUMBER: "not_a_number",
         e.ECUIO_CONDITION_WHY_ENUM_ORDER: "enum_order",
         e.ECUIO_CONDITION_WHY_NO_LINK: "no_link",
         e.ECUIO_CONDITION_WHY_NO_CHECK: "no_check"}
_NONE = 0xFFFFFFFF


def _free(handle):
    lib.ecuio_condition_set_free(handle)


def _term(t):
    if t.kind == e.ECUIO_TERM_LINK:
        return Term("link", (), None, _LINKS.get(t.test, "unknown"), None, None, None)
    if t.kind == e.ECUIO_TERM_CHECK:
        return Term("check", (), None, _CHECKS.get(t.test, "unknown"), None, None, None)
    return Term("signal",
                tuple(text(t.references[k].text) for k in range(t.reference_count)),
                text(t.unit) if t.flags & e.ECUIO_TERM_HAS_UNIT else None,
                _TESTS.get(t.test, "unknown"), t.level,
                t.clear if t.flags & e.ECUIO_TERM_HAS_CLEAR else None,
                t.max_age_us or None)


class ConditionSet:
    """A condition file (ADR-0063), from its text or a path. name is the
    file's ("" with none), checks its Checks and conditions its
    Conditions in the file's order."""

    def __init__(self, text_or_bytes=None, path=None):
        if (text_or_bytes is None) == (path is None):
            raise ValueError("a condition file's text or its path, one of them")
        if path is not None:
            with open(path, "rb") as f:
                data = f.read()
        else:
            data = utf8(text_or_bytes)
        self._handle = ctypes.POINTER(e.ecuio_condition_set_t)()
        check(lib.ecuio_condition_set_load(data, len(data), ctypes.byref(self._handle)),
              "ecuio_condition_set_load")
        # Left unclosed, freed when collected or at the interpreter's exit
        self._finalizer = weakref.finalize(self, _free, self._handle)
        self.name = text(lib.ecuio_condition_set_name(self._handle))
        checks = e.ecuio_check_options_t()
        check(lib.ecuio_condition_set_checks(self._handle, ctypes.byref(checks)),
              "ecuio_condition_set_checks")
        self.checks = Checks(checks.gap_us or None,
                             checks.rate_drop_x100 / 100.0 if checks.rate_drop_x100 else None,
                             checks.settle_us or None)
        self.conditions = [self._condition(i)
                           for i in range(lib.ecuio_condition_set_count(self._handle))]

    @classmethod
    def load(cls, text_or_bytes):
        """The set a condition file's text holds"""
        return cls(text_or_bytes)

    def _condition(self, index):
        info = e.ecuio_condition_info_t()
        check(lib.ecuio_condition_set_get(self._handle, index, ctypes.byref(info)),
              "ecuio_condition_set_get")
        terms = []
        for k in range(info.term_count):
            t = e.ecuio_condition_term_t()
            check(lib.ecuio_condition_set_get_term(self._handle, index, k, ctypes.byref(t)),
                  "ecuio_condition_set_get_term")
            terms.append(_term(t))
        alert = None
        if info.uses & e.ECUIO_CONDITION_USE_ALERT:
            alert = Alert("danger" if info.alert_level == e.ECUIO_ALERT_DANGER else "warning",
                          bool(info.uses & e.ECUIO_CONDITION_ALERT_SOUND),
                          bool(info.uses & e.ECUIO_CONDITION_ALERT_BANNER))
        record = None
        if info.uses & e.ECUIO_CONDITION_USE_RECORD:
            record = Record(info.before_us, info.after_us)
        return Condition(text(info.key), text(info.name),
                         not info.flags & e.ECUIO_CONDITION_INFO_DISABLED,
                         "any" if info.flags & e.ECUIO_CONDITION_INFO_ANY else "all",
                         tuple(terms), info.for_us, info.clear_for_us, alert, record,
                         bool(info.uses & e.ECUIO_CONDITION_USE_MARK))

    def resolve(self, signals=None, variant=None):
        """A Binding for each condition, in the file's order, against a
        set of signals: a SignalFile (its set, and its keys for key:
        references), a Plugin (its variant's set and keys: the one named,
        the first when None), a registered profile's name ("obd2",
        "mock"), or None for no signals at all"""
        if not self._finalizer.alive:
            raise ValueError("the condition set is closed")
        file = None
        chosen = None
        if isinstance(signals, str):
            chosen = ctypes.POINTER(e.ecuio_signal_set_t)()
            check(lib.ecuio_profile_lookup(utf8(signals), ctypes.byref(chosen)),
                  "ecuio_profile_lookup")
        elif hasattr(signals, "variant_index"):
            index = signals.variant_index(variant)
            file = lib.ecuio_plugin_variant_file(signals._handle, index)
            chosen = lib.ecuio_plugin_variant_set(signals._handle, index)
        elif signals is not None:
            file = signals._handle
            chosen = lib.ecuio_signal_file_set(file)
        found = []
        for index, condition in enumerate(self.conditions):
            b = e.ecuio_condition_binding_t()
            check(lib.ecuio_condition_set_resolve(self._handle, index, chosen, file,
                                                  ctypes.byref(b)),
                  "ecuio_condition_set_resolve")
            terms = tuple(
                Resolved(condition.terms[k].signal[b.reference[k]], b.signal_id[k])
                if b.signal_id[k] != _NONE else None
                for k in range(b.term_count))
            found.append(Binding(b.why == e.ECUIO_CONDITION_WHY_NONE,
                                 _WHYS.get(b.why, "unknown") if b.why else None,
                                 b.why_term if b.why_term != _NONE else None, terms))
        return found

    def close(self):
        """Frees the set; its conditions stay readable. Safe to call twice."""
        self._finalizer()

    def __repr__(self):
        return f"ConditionSet({self.name!r}, {len(self.conditions)} conditions)"
