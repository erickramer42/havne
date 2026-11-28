import collections
import ctypes

from . import _ecuio as e
from ._core import check, lib, text, utf8


Part = collections.namedtuple(
    "Part", "profile name format plugin version variant signals modules source")
Part.__doc__ = """One file of a merged set, in merge order: the profile it registers
under alone, its own name and format, its plugin's id, version and
variant (None for a signal file), how many of its signals and modules
are in the set, and its Source"""

Module = collections.namedtuple("Module", "rx tx id name part session")
_SESSIONS = {1: "default", 3: "extended"}
Module.__doc__ = """One module of a file's set: its reply and request IDs, its entry's
id and name (None when not given), and in a merged set the profile of
the part whose id stands for it (None for none), and the session it
declares ("default", "extended", or None)"""

Note = collections.namedtuple("Note", "kind text key base_def")
Note.__doc__ = """One thing a load or a merge did that its files didn't say outright:
kind as a word ("replaced_by_address", "beside_base", ...), core's
sentence, the key of the signal it is about (None for one not in the
set), and for beside_base the key of obd2's signal it is read beside"""

_KINDS = {
    e.ECUIO_NOTE_REPLACED_BY_ADDRESS: "replaced_by_address",
    e.ECUIO_NOTE_REPLACED_BY_KEY: "replaced_by_key",
    e.ECUIO_NOTE_KEY_TAKEN: "key_taken",
    e.ECUIO_NOTE_BESIDE_BASE: "beside_base",
    e.ECUIO_NOTE_MODULE_RENAMED: "module_renamed",
    e.ECUIO_NOTE_MODULE_LEFT_OUT: "module_left_out",
    e.ECUIO_NOTE_PART_LEFT_OUT: "part_left_out",
    e.ECUIO_NOTE_SIGNAL_LEFT_OUT: "signal_left_out",
}

_BASES = {None: 0, "obd2": e.ECUIO_MERGE_BASE_OBD2, "modules": e.ECUIO_MERGE_BASE_MODULES}


def _part(file, index, source_of):
    p = e.ecuio_signal_part_t()
    p.struct_size = ctypes.sizeof(p)
    check(lib.ecuio_signal_file_part(file, index, ctypes.byref(p)), "ecuio_signal_file_part")
    s = e.ecuio_signal_source_t()
    s.struct_size = ctypes.sizeof(s)
    check(lib.ecuio_signal_file_part_source(file, index, ctypes.byref(s)),
          "ecuio_signal_file_part_source")
    return Part(text(p.profile), text(p.name), p.format, text(p.plugin_id) or None,
                text(p.plugin_version) or None, text(p.plugin_variant) or None,
                p.signal_count, p.module_count, source_of(s))


def parts(file, source_of):
    return [_part(file, i, source_of) for i in range(lib.ecuio_signal_file_part_count(file))]


def _def(file, key):
    found = lib.ecuio_signal_file_find(file, utf8(key))
    if found:
        return found
    sset = lib.ecuio_signal_file_set(file)
    count = ctypes.c_uint32()
    check(lib.ecuio_signal_set_keys(sset, file, None, 0, ctypes.byref(count)),
          "ecuio_signal_set_keys")
    keys = (e.ecuio_signal_key_t * count.value)()
    check(lib.ecuio_signal_set_keys(sset, file, keys, count.value, ctypes.byref(count)),
          "ecuio_signal_set_keys")
    for i, k in enumerate(keys[:count.value]):
        if text(k.text) == key:
            return ctypes.pointer(sset.contents.defs[i])
    raise LookupError(f"no signal with the key {key!r}")


def part_of(file, key, source_of):
    at = lib.ecuio_signal_file_def_part(file, _def(file, key))
    return None if at == e.ECUIO_SIGNAL_PART_NONE else _part(file, at, source_of)


def modules(file):
    sset = lib.ecuio_signal_file_set(file)
    count = sset.contents.channel_count if sset else 0
    out = []
    for i in range(count):
        m = e.ecuio_signal_module_t()
        m.struct_size = ctypes.sizeof(m)
        check(lib.ecuio_signal_file_module(file, i, ctypes.byref(m)), "ecuio_signal_file_module")
        part = None
        if m.part != e.ECUIO_SIGNAL_PART_NONE:
            part = _part(file, m.part, lambda s: None).profile
        out.append(Module(m.rx_id, m.tx_id, text(m.id) or None, text(m.name) or None, part,
                          _SESSIONS.get(m.session)))
    return out


def notes(file, keys):
    out = []
    for i in range(lib.ecuio_signal_file_note_count(file)):
        n = e.ecuio_signal_note_t()
        n.struct_size = ctypes.sizeof(n)
        check(lib.ecuio_signal_file_note(file, i, ctypes.byref(n)), "ecuio_signal_file_note")
        at = getattr(n, "def")
        key = text(keys[at].text) if at < len(keys) else None
        base = text(keys[n.base_def].text) if n.base_def < len(keys) else None
        out.append(Note(_KINDS.get(n.kind, "other"), text(n.text), key, base))
    return out


def _key_list(call, fill):
    count = ctypes.c_uint32()
    check(fill(None, 0, ctypes.byref(count)), call)
    keys = (e.ecuio_signal_key_t * count.value)()
    check(fill(keys, count.value, ctypes.byref(count)), call)
    return [text(k.text) for k in keys[:count.value]]


def needs(file):
    return _key_list("ecuio_signal_file_needs",
                     lambda k, n, c: lib.ecuio_signal_file_needs(file, k, n, c))


def replaced_keys(file, key):
    found = _def(file, key)
    return _key_list("ecuio_signal_file_replaced_keys",
                     lambda k, n, c: lib.ecuio_signal_file_replaced_keys(file, found, k, n, c))


def def_module(file, key):
    at = lib.ecuio_signal_set_def_module(lib.ecuio_signal_file_set(file), _def(file, key))
    return None if at == e.ECUIO_SIGNAL_MODULE_NONE else at


def merge(handles, base):
    if base not in _BASES:
        raise ValueError(f"base is 'obd2', 'modules' or None, not {base!r}")
    options = e.ecuio_merge_options_t()
    options.struct_size = ctypes.sizeof(options)
    options.flags = _BASES[base]
    array = (ctypes.POINTER(e.ecuio_signal_file_t) * len(handles))(*handles)
    out = ctypes.POINTER(e.ecuio_signal_file_t)()
    check(lib.ecuio_signal_file_merge(array, len(handles), ctypes.byref(options),
                                      ctypes.byref(out)),
          "ecuio_signal_file_merge")
    return out


def expr_names(expr):
    data = utf8(expr)
    count = ctypes.c_uint32()
    check(lib.ecuio_expr_names(data, len(data), None, 0, ctypes.byref(count)),
          "ecuio_expr_names")
    names = (e.ecuio_expr_name_t * count.value)()
    check(lib.ecuio_expr_names(data, len(data), names, count.value, ctypes.byref(count)),
          "ecuio_expr_names")
    return [text(n.text) for n in names[:count.value]]
