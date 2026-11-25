# File/layer: python/havne/_conversation.py | Python 3.11+, the havne package
# Testing: PythonTest.test_consent
#
# What a Session does for a plugin whose code talks to a module (ADR-0066):
# a consent applied between create and start, the security store its
# unlock attempts are counted in, the conversation's report, and the
# caller's word for one unlock attempt. Core sends the seed and the key;
# the word is given once and core never asks it again.

import ctypes
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8
from .errors import TooSoon

Conversation = namedtuple("Conversation", "generation rx_id state unlock level nrc failed "
                          "delay_left_ms requests fault kept")
Conversation.__doc__ = ("A channel's conversation as it stands: the report's generation (the "
                        "word names it), the module's reply ID, state and unlock in words "
                        "(\"talking\", \"unlocked\"), the last word's level, a refused seed's "
                        "or key's code (0 for none), the failed attempts at that level, the "
                        "delay left in ms, the requests the door took, the fault's name "
                        "(\"\" for none) and whether the attempts are kept in a store")


def _words(prefix):
    return {getattr(e, n): n[len(prefix):].lower() for n in dir(e)
            if n.startswith(prefix) and isinstance(getattr(e, n), int)}


_FAULTS = _words("ECUIO_CONVERSATION_FAULT_")
_STATES = {k: v for k, v in _words("ECUIO_CONVERSATION_").items()
           if k < 16 and not v.startswith("fault_") and v not in ("not_kept", "poll_us")}
_UNLOCKS = _words("ECUIO_UNLOCK_")


class _Talk:
    """Session's part for a plugin that talks to a module"""

    def security_store(self, path, label=None):
        """Names the file the unlock attempts are counted in, before start (so a
        delay and a count outlive the session). label is the car's name where
        the module answers no serial and the car no VIN; core trims and folds
        it. Without a store the counts live in the session."""
        check(lib.ecuio_session_security_store(self._handle(), utf8(path), utf8(label)),
              "ecuio_session_security_store")

    def apply_consent(self, store):
        """Applies the person's consent, a ConsentStore's record of this
        session's plugin, between create and start. An Error with core's
        words (CONSENT_REQUIRED) when the store holds none or the plugin
        names no publisher; with no consent the session reads signals only."""
        if self._plugin is None:
            raise ValueError("apply_consent needs a session made with plugin=")
        fingerprint = (ctypes.c_uint8 * 32).from_buffer_copy(
            bytes.fromhex(self._plugin.fingerprint))
        check(lib.ecuio_session_apply_consent(self._handle(), store._handle(), fingerprint),
              "ecuio_session_apply_consent")

    def _channel_of(self, ecu):
        """A channel by number, or by the plugin's id for the ECU"""
        if isinstance(ecu, int):
            return ecu
        if self._plugin is None:
            raise ValueError("an ECU is named by the plugin's id: make the session with "
                             "plugin=, or give the channel's number")
        ecus = self._plugin.conversation_ecus()
        if ecu not in ecus:
            raise ValueError(f"the plugin's code talks to no ECU {ecu!r}: " + ", ".join(ecus))
        for channel, (rx, _) in enumerate(self.modules):
            if rx == ecus[ecu]:
                return channel
        raise ValueError(f"ECU {ecu!r} is not one of the session's modules")

    def _report(self, channel):
        out = e.ecuio_channel_conversation_t()
        check(lib.ecuio_session_get_channel_conversation(self._handle(), channel,
                                                         ctypes.byref(out)),
              "ecuio_session_get_channel_conversation")
        return out

    def conversation(self, ecu=0):
        """The conversation of a channel (its number, or the plugin's ECU id) as it
        stands: state "none" where no plugin's code talks"""
        r = self._report(self._channel_of(ecu))
        return Conversation(r.generation, r.rx_id, _STATES.get(r.state, str(r.state)),
                            _UNLOCKS.get(r.unlock, str(r.unlock)), r.level, r.nrc, r.failed,
                            r.delay_left_ms, r.requests,
                            "" if r.fault == 0 else _FAULTS.get(r.fault, str(r.fault)),
                            not r.flags & e.ECUIO_CONVERSATION_NOT_KEPT)

    def unlock(self, ecu, level, generation=None):
        """The person's word for one attempt at security access `level` (odd, the
        seed request's sub-function) of a module: a channel's number or the
        plugin's ECU id. Core asks the seed and the plugin's code computes the
        key; the word is not asked again and the attempt counts if the key was
        sent. generation is the report the person was shown (conversation()'s);
        None takes the one as it stands. Returns the generation the word named
        once it was taken; how the attempt ended is conversation()'s unlock. An
        Error says why nothing was sent; TooSoon carries wait_ms where the
        level's delay runs (0: the module's identity is still being read)."""
        channel = self._channel_of(ecu)
        if generation is None:
            generation = self._report(channel).generation
        wait = ctypes.c_uint32()
        rc = lib.ecuio_session_security_unlock(self._handle(), channel, level, generation,
                                               ctypes.byref(wait))
        if rc == e.ECUIO_ERR_TOO_SOON:
            words, detail = lib.ecuio_status_string(rc), lib.ecuio_last_error_detail()
            raise TooSoon(rc, text(words), text(detail), "ecuio_session_security_unlock",
                          wait.value)
        check(rc, "ecuio_session_security_unlock")
        return generation
