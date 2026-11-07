# File/layer: python/havne/signals.py | Python 3.11+, the havne package
# Testing: PythonTest.test_signal_file, test_plugin
#
# A script's own signals, as data (ADR-0053 section 5): a signal file
# (ADR-0051) or a plugin's variant (ADR-0052), loaded by core from bytes
# and registered as a profile a session's URI names. The package never
# builds an ecuio_signal_def_t: a signal file reads the same across the
# set's layouts, a hand-built set doesn't.
#
# A loaded file stays registered, and in memory, until the process ends,
# as havne-cli's --signals does: a session may decode with its set at any
# time. Core copies what it loads (the file or plugin owns its set), and
# the package never frees a handle, so nothing on the Python side needs
# keeping alive. Loading a file of the same name again registers the new
# one in its place (the registry's last wins); a session already open
# keeps the set it opened with.

import ctypes
import itertools
import os
import threading

from . import _ecuio as e
from ._core import check, lib, text, utf8


class SignalFile:
    """A signal file (ADR-0051), from its text or a path; registered as
    its profile ("user.<slug>") on load"""

    def __init__(self, text_or_bytes=None, path=None):
        if (text_or_bytes is None) == (path is None):
            raise ValueError("a signal file's text or its path, one of them")
        if path is not None:
            with open(path, "rb") as f:
                data = f.read()
        else:
            data = utf8(text_or_bytes)
        self._handle = ctypes.POINTER(e.ecuio_signal_file_t)()
        check(lib.ecuio_signal_file_load(data, len(data), ctypes.byref(self._handle)),
              "ecuio_signal_file_load")
        self.name = text(lib.ecuio_signal_file_name(self._handle))
        self.profile = text(lib.ecuio_signal_file_profile(self._handle))
        self.extends = text(lib.ecuio_signal_file_extends(self._handle))
        check(lib.ecuio_profile_register(utf8(self.profile),
                                         lib.ecuio_signal_file_set(self._handle), None),
              "ecuio_profile_register")

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
            rc = lib.ecuio_plugin_load(packed, len(packed), None, None,
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

    def __repr__(self):
        return f"Plugin({self.id!r} {self.version}, variants {self.variants})"
