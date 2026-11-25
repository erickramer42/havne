# File/layer: python/havne/consent.py | Python 3.11+, the havne package
# Testing: PythonTest.test_consent
#
# A person's consent to a plugin (ADR-0066's appendix of 2026-11-23), as an
# act a script performs: the store core keeps in a file, the lines every
# front end shows, and give(), which takes the terms the person was shown
# and refuses any other text. The record is made by core from the plugin
# itself; a script names no field of it. A store proves nothing about who
# consented: anyone who can call give() can make a record.

import ctypes
import os
import sys
import time
import weakref
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8

ConsentRecord = namedtuple("ConsentRecord",
                           "id version fingerprint terms_sha256 publisher given_utc")
ConsentRecord.__doc__ = ("One consent: the plugin's id and version, its fingerprint and its "
                         "terms' SHA-256 as 64 lower-case hex digits each, the publisher's "
                         "name and the second it was given (UTC)")
ConsentLine = namedtuple("ConsentLine", "id key text heading action")
ConsentLine.__doc__ = ("One line a consent shows: ECUIO_CONSENT_LINE_*, its name in records, "
                       "its text and one-line heading (\"\" for none), and whether it is an "
                       "action line, shown in full")

_SETS = {"conversation": e.ECUIO_CONSENT_SET_CONVERSATION,
         "read": e.ECUIO_CONSENT_SET_READ, "write": e.ECUIO_CONSENT_SET_WRITE}


def default_path():
    """The app's own consent.json, where havne-cli's --consent-store defaults to
    too: HAVNE_SETTINGS_DIR, else the platform's data folder under Havne/Havne.
    None when no folder is known."""
    folder = os.environ.get("HAVNE_SETTINGS_DIR", "")
    if not folder:
        if sys.platform == "win32":
            base = os.environ.get("APPDATA", "")
        elif sys.platform == "darwin":
            home = os.environ.get("HOME", "")
            base = home + "/Library/Application Support" if home else ""
        else:
            base = os.environ.get("XDG_DATA_HOME", "")
            if not base.startswith("/"):
                home = os.environ.get("HOME", "")
                base = home + "/.local/share" if home else ""
        folder = os.path.join(base, "Havne", "Havne") if base else ""
    return os.path.join(folder, "consent.json") if folder else None


def _hex(raw):
    return bytes(raw).hex()


def _print_bytes(fingerprint):
    """A fingerprint (64 hex digits, either case, or its 32 bytes) as a ctypes array"""
    try:
        raw = bytes.fromhex(fingerprint) if isinstance(fingerprint, str) else bytes(fingerprint)
    except ValueError:
        raw = b""
    if len(raw) != 32:
        raise ValueError("a fingerprint is 64 hex digits (32 bytes)")
    return (ctypes.c_uint8 * 32).from_buffer_copy(raw)


def _record(raw):
    return ConsentRecord(text(raw.id), text(raw.version), _hex(raw.fingerprint),
                         _hex(raw.terms_sha256), text(raw.publisher), raw.given_utc)


def _blank():
    out = e.ecuio_consent_record_t()
    out.struct_size = ctypes.sizeof(out)
    return out


def _close_store(handle):
    lib.ecuio_consent_store_close(handle)


class ConsentStore:
    """The file of a person's consents. path None is default_path(). Core
    reads and writes the file itself: a give or a withdraw is on the disk
    before it returns, and a file that doesn't read is an Error, never an
    empty store; no file is an empty one."""

    def __init__(self, path=None):
        if path is None:
            path = default_path()
            if path is None:
                raise ValueError("no settings folder is known: name the consent store's path")
            os.makedirs(os.path.dirname(path), exist_ok=True)
        self.path = os.fspath(path)
        self._h = ctypes.POINTER(e.ecuio_consent_store_t)()
        check(lib.ecuio_consent_store_open(utf8(self.path), ctypes.byref(self._h)),
              "ecuio_consent_store_open")
        self._finalizer = weakref.finalize(self, _close_store, self._h)

    def _handle(self):
        if not self._finalizer.alive:
            raise ValueError("the consent store is closed")
        return self._h

    def close(self):
        self._finalizer()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def give(self, plugin, terms_shown):
        """Records the person's consent to plugin, now. terms_shown is the
        text the person was shown and agreed to: it must be plugin.terms
        exactly, else ValueError and nothing is recorded. A plugin that names
        no publisher is core's refusal (Error). A fingerprint given already
        keeps its first second. Returns the record."""
        terms = plugin.terms
        if terms is not None and terms_shown != terms:
            raise ValueError("terms_shown is not the plugin's terms: show the person "
                             "plugin.terms whole, then give")
        check(lib.ecuio_consent_store_give(self._handle(), plugin._handle, int(time.time())),
              "ecuio_consent_store_give")
        return self.find(plugin.fingerprint)

    def find(self, fingerprint):
        """The record of a fingerprint (64 hex digits), None for none"""
        out = _blank()
        rc = lib.ecuio_consent_store_find(self._handle(), _print_bytes(fingerprint),
                                          ctypes.byref(out))
        if rc == e.ECUIO_ERR_CONSENT_REQUIRED:
            return None
        check(rc, "ecuio_consent_store_find")
        return _record(out)

    def latest(self, plugin_id):
        """The newest record of a plugin id, whatever its version; None for none"""
        out = _blank()
        rc = lib.ecuio_consent_store_latest(self._handle(), utf8(plugin_id), ctypes.byref(out))
        if rc == e.ECUIO_ERR_CONSENT_REQUIRED:
            return None
        check(rc, "ecuio_consent_store_latest")
        return _record(out)

    def records(self):
        """Every record, newest first"""
        n = ctypes.c_uint32()
        check(lib.ecuio_consent_store_count(self._handle(), ctypes.byref(n)),
              "ecuio_consent_store_count")
        out = []
        for i in range(n.value):
            raw = _blank()
            check(lib.ecuio_consent_store_get(self._handle(), i, ctypes.byref(raw)),
                  "ecuio_consent_store_get")
            out.append(_record(raw))
        return out

    def withdraw(self, plugin_id, fingerprint=None):
        """Withdraws the consent to one fingerprint, or with None every
        consent to the plugin id; an Error when none is held"""
        check(lib.ecuio_consent_store_withdraw(
            self._handle(), utf8(plugin_id),
            None if fingerprint is None else _print_bytes(fingerprint)),
            "ecuio_consent_store_withdraw")


def consent_lines(classes="conversation"):
    """The lines a consent shows for a class of what a plugin does
    ("conversation", "read", "write"; a name or several), in order: core's
    words, so every front end shows the same. The publisher block and the
    terms come from the plugin and follow them."""
    names = (classes,) if isinstance(classes, str) else tuple(classes)
    bad = [n for n in names if n not in _SETS]
    if bad or not names:
        raise ValueError(f"classes are among {', '.join(_SETS)}: {bad or 'none given'}")
    mask = 0
    for n in names:
        mask |= _SETS[n]
    lines = []
    while True:
        raw = e.ecuio_consent_line_t()
        raw.struct_size = ctypes.sizeof(raw)
        rc = lib.ecuio_consent_line(mask, len(lines), ctypes.byref(raw))
        if rc == e.ECUIO_ERR_INVALID_ARGUMENT and lines:
            return lines
        check(rc, "ecuio_consent_line")
        lines.append(ConsentLine(raw.id, text(raw.key), text(raw.text), text(raw.heading),
                                 bool(raw.flags & e.ECUIO_CONSENT_LINE_ACTION)))


__all__ = ["ConsentStore", "ConsentRecord", "ConsentLine", "consent_lines", "default_path"]
