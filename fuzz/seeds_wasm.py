# File/layer: fuzz/seeds_wasm.py | Python 3, no packages; fuzz/seeds.py imports it
# Testing: FUZZ: CI's fuzz job, fuzz_wasm_module, fuzz_guest and
#          fuzz_plugin_manifest started from what it writes
#
# The WebAssembly modules among the seeds, written byte by byte
# (wasm_module()): fuzz_wasm_module's, with the exports the harness calls,
# fuzz_guest's guest modules, and the conversation plugins
# fuzz_plugin_manifest starts from, each holding a guest module.

import base64
import hashlib
import json

SEP = b"\x1e"  # the cut between a seed's pieces, as in seeds.py


def leb(v):
    """An unsigned LEB128 number"""
    out = b""
    while True:
        b = v & 0x7F
        v >>= 7
        if v == 0:
            return out + bytes([b])
        out += bytes([b | 0x80])


def sleb(v):
    """A signed LEB128 number"""
    out = b""
    while True:
        b = v & 0x7F
        v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
            return out + bytes([b])
        out += bytes([b | 0x80])


def wasm_section(kind, content):
    return bytes([kind]) + leb(len(content)) + content


def wasm_name(text):
    return leb(len(text)) + text.encode()


def wasm_module(funcs, memory=b"\x01\x01\x01", table=b"", global_=b"", element=b"",
                data=b"", imports=b"", start=None, tail=b""):
    """A module's bytes. funcs: (export name or None, parameters, results,
    body), the types as text of 'i' (i32); each function type is written
    once. memory, table, global_, element, data and imports are a
    section's entries after its count of one (memory's: its limits)"""
    types = []
    for _, params, results, _ in funcs:
        if (params, results) not in types:
            types.append((params, results))
    out = b"\x00asm\x01\x00\x00\x00"
    out += wasm_section(1, leb(len(types)) + b"".join(
        b"\x60" + leb(len(p)) + b"\x7f" * len(p) + leb(len(r)) + b"\x7f" * len(r)
        for p, r in types))
    if imports:
        out += wasm_section(2, b"\x01" + imports)
    out += wasm_section(3, leb(len(funcs)) + b"".join(
        leb(types.index((p, r))) for _, p, r, _ in funcs))
    if table:
        out += wasm_section(4, b"\x01" + table)
    if memory:
        out += wasm_section(5, b"\x01" + memory)
    if global_:
        out += wasm_section(6, b"\x01" + global_)
    exports = [(name, i) for i, (name, _, _, _) in enumerate(funcs) if name]
    out += wasm_section(7, leb(len(exports)) + b"".join(
        wasm_name(name) + b"\x00" + leb(i) for name, i in exports))
    if start is not None:
        out += wasm_section(8, leb(start))
    if element:
        out += wasm_section(9, b"\x01" + element)
    bodies = [b"\x00" + body + b"\x0b" for _, _, _, body in funcs]
    out += wasm_section(10, leb(len(funcs)) + b"".join(leb(len(b)) + b for b in bodies))
    if data:
        out += wasm_section(11, b"\x01" + data)
    return out + tail


def guest_module(buffer_at=1024, imports=b"", begin=b"\x41\x00",
                 next_request=b"\x41\x00", accept=b"\x41\x00"):
    """The least module the guest ABI takes (core/src/guest_abi.h): its six
    exports, each returning a constant unless a body is given, and one
    page of memory"""
    const = lambda v: b"\x41" + sleb(v)   # i32.const
    return wasm_module([("ecuio_abi", "", "i", const(1)),
                        ("ecuio_buffer", "", "i", const(buffer_at)),
                        ("begin", "ii", "i", begin),
                        ("next_request", "", "i", next_request),
                        ("accept", "ii", "i", accept),
                        ("end", "", "", b"")], imports=imports)


def guest_seeds():
    """fuzz_guest's seeds by name: a guest that asks nothing, one in the
    extended session asking a DID, one answering a seed with a key of
    zeroes, and one of each fault"""
    const = lambda v: b"\x41" + sleb(v)   # i32.const
    byte_at = lambda at, v: const(at) + const(v) + b"\x3a\x00\x00"  # i32.store8
    did = byte_at(1024, 0x22) + byte_at(1025, 0xF1) + byte_at(1026, 0x90) + const(3)
    return {
        "quiet": guest_module(),
        "extended": guest_module(begin=const(3)),
        "asks-a-did": guest_module(begin=const(3), next_request=did),
        "zero-key": guest_module(begin=const(3), accept=const(4)),
        "declines": guest_module(begin=const(-1)),
        "traps": guest_module(begin=b"\x00"),                      # unreachable
        "spins": guest_module(begin=const(3),
                              next_request=b"\x03\x40\x0c\x00\x0b" + const(0)),
        "long-request": guest_module(begin=const(3), next_request=const(5000)),
        "buffer-outside": guest_module(buffer_at=65000),
    }


def conversation_seeds():
    """fuzz_plugin_manifest's seeds for a format 2 plugin with the
    conversation tier, and a format 3 one with its publisher, by name"""
    data = {"havne_signals": 1, "signals": [
        {"key": "oil_temp", "name": "Oil Temp", "ecu": "pcm",
         "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}}]}

    publisher = {"name": "Made-up Tools", "contact": "https://example.com/help",
                 "license": "MIT", "terms": "Use at your own risk.\nNo warranty."}

    def plugin(module, packed, sha=None, level="01", format=2, publisher=None):
        doc = {"havne_plugin": 2, "id": "talker", "name": "Talker", "version": "1.0.0",
               "requires": {"core": "0.1.0"}, "source": {"name": "Havne's fuzz seeds"},
               "tiers": ["data", "conversation"],
               "ecus": [{"id": "pcm", "rx": "7E8"}, {"id": "tcm", "rx": "7E9"}],
               "variants": [{"id": "base", "files": ["a.json"]}]}
        if packed:
            doc["files"] = {"a.json": data, "guest.wasm": {
                "kind": "wasm", "base64": base64.b64encode(module).decode()}}
        doc["conversation"] = {
            "code": {"file": "guest.wasm",
                     "sha256": sha or hashlib.sha256(module).hexdigest()},
            "budget": 100000, "ecus": ["pcm", "tcm"],
            "services": [{"sid": "10", "sub": ["03"]}, {"sid": "22"}, {"sid": "19"},
                         {"sid": "27", "sub": ["01", "02", "03", "04"]},
                         {"sid": "2C", "sub": ["01", "03"]}, {"sid": "3E", "sub": ["00"]}],
            "security": [{"ecu": "pcm", "level": level, "seed_bytes": 4, "key_bytes": 4},
                         {"ecu": "tcm", "level": "03", "seed_bytes": 16, "key_bytes": 16,
                          "delay_ms": 30000, "key_window_ms": 250}]}
        doc["havne_plugin"] = format
        if publisher is not None:
            doc["publisher"] = publisher
        return json.dumps(doc, indent=1).encode()

    module = guest_module()
    plain = guest_module(buffer_at=0)   # no 0x1E byte: a folder's piece
    assert SEP not in plain
    importing = guest_module(imports=wasm_name("env") + wasm_name("now") + b"\x00\x00")
    return {
        "packed_conversation": b"\x00" + plugin(module, True),
        "folder_conversation": b"\x01" + plugin(plain, False) + SEP +
                               json.dumps(data).encode() + SEP + plain,
        "packed_conversation_other_hash": b"\x00" + plugin(module, True, sha="0" * 64),
        "packed_conversation_imports": b"\x00" + plugin(importing, True),
        "packed_conversation_undeclared_level": b"\x00" + plugin(module, True, level="05"),
        "packed_conversation3": b"\x00" + plugin(module, True, format=3, publisher=publisher),
        "folder_conversation3": b"\x01" + plugin(plain, False, format=3, publisher=publisher) +
                                SEP + json.dumps(data).encode() + SEP + plain,
        "packed_conversation3_no_publisher": b"\x00" + plugin(module, True, format=3),
    }


def wasm_seeds():
    """fuzz_wasm_module's seeds by name: its five exports doing each thing
    a module can do to the sandbox, and a module the check refuses of
    each kind"""
    const = lambda v: b"\x41" + sleb(v)   # i32.const
    get = lambda i: bytes([0x20, i])      # local.get
    load = b"\x28\x02\x00"                # i32.load, aligned, no offset
    store = b"\x36\x02\x00"
    add, drop, unreachable = b"\x6a", b"\x1a", b"\x00"
    spin = b"\x03\x40\x0c\x00\x0b"       # loop, br 0, end
    five = [("a", "", "", b""),
            ("b", "", "i", const(42)),
            ("c", "i", "i", get(0)),
            ("d", "ii", "i", get(0) + get(1) + add),
            ("e", "iiii", "i", get(0) + get(1) + add + get(2) + add + get(3) + add)]
    nest = 64
    seeds = {
        "exports": wasm_module(five),
        # One export that doesn't return in each: the first ends the instance
        "spin": wasm_module(five[1:2] + [("c", "i", "i", spin + const(0))]),
        "trap-unreachable": wasm_module(five[1:3] + [("a", "", "", unreachable)]),
        "trap-outside": wasm_module([("b", "", "i", const(-1) + load)]),  # past the memory
        "trap-divide": wasm_module([("c", "i", "i", get(0) + const(0) + b"\x6e")]),  # i32.div_u by 0
        "trap-overflow": wasm_module([    # an f64 past an i32: f64.const, i32.trunc_f64_s
            ("b", "", "i", b"\x44" + bytes(7) + b"\x7f" + b"\xaa")]),
        "recurse": wasm_module([("a", "", "", b"\x10\x00")]),       # calls itself
        "recurse-argument": wasm_module([("c", "i", "i", get(0) + b"\x10\x00")]),
        "memory": wasm_module([
            ("b", "", "i", const(65532) + load),                   # its last word
            ("c", "i", "i", get(0) + get(0) + store + get(0) + load),
            ("d", "ii", "i", get(0) + get(1) + b"\x3a\x00\x00" + b"\x3f\x00")],  # store8, size
            memory=b"\x01\x01\x01"),
        "memory-largest": wasm_module(five[:3], memory=b"\x01\x10\x10"),
        "grow": wasm_module([
            ("b", "", "i", const(1) + b"\x40\x00"),               # memory.grow
            ("c", "i", "i", get(0) + b"\x40\x00" + b"\x3f\x00" + add)]),
        "blocks": wasm_module([("a", "", "", b"\x02\x40" * nest + b"\x0b" * nest),
                               ("b", "", "i", b"\x02\x7f" + const(1) + const(1) +
                                b"\x0d\x00" + drop + const(2) + b"\x0b")]),  # br_if with a value
        "table": wasm_module([
            ("b", "", "i", const(7)),
            (None, "", "i", const(9)),
            ("c", "i", "i", get(0) + b"\x11\x00\x00")],           # call_indirect, type 0
            table=b"\x70\x00\x02",                               # two function references
            element=b"\x00" + const(0) + b"\x0b" + b"\x02\x00\x01"),
        "globals": wasm_module([
            ("b", "", "i", b"\x23\x00"),                          # global.get
            ("c", "i", "i", get(0) + b"\x24\x00" + b"\x23\x00")],
            global_=b"\x7f\x01" + const(5) + b"\x0b"),            # a mutable i32 of 5
        "data": wasm_module([("b", "", "i", const(0) + load)],
                            data=b"\x00" + const(0) + b"\x0b" + b"\x04\xde\xad\xbe\xef"),
        "data-outside": wasm_module(five[:1],
                                    data=b"\x00" + const(65534) + b"\x0b" + b"\x04abcd"),
        "numbers": wasm_module([
            ("c", "i", "i", get(0) + b"\xac" + b"\x42\x03" + b"\x7f" + b"\xa7"),  # i64: extend, div_s, wrap
            ("d", "ii", "i", get(0) + b"\xc0" + get(1) + b"\x74")]),   # i32.extend8_s, shl
        # Work the budget counts beyond instructions: blocks scanned for
        # their ends on every turn of a loop, a block left by a branch,
        # br_tables the runtime keeps in its list (a depth past 255)
        "scan-if": wasm_module([("a", "", "", b"\x03\x40" + (
            b"\x41\x00\x04\x40" + b"\x01" * 2043 + b"\x0b") * 3 + b"\x0c\x00\x0b")]),
        "scan-block": wasm_module([("a", "", "", b"\x02\x40\x0c\x00" + b"\x01" * 2000 + b"\x0b")]),
        "table-list": wasm_module([("a", "", "", b"\x02\x40" * 256 + (
            b"\x02\x40\x41\x01\x0e\x01\x80\x02\x00\x0b") * 8 + b"\x0b" * 256)]),
        # What the check refuses, each of its kinds
        "refused-start": wasm_module(five, start=0),
        "refused-import": wasm_module(five[1:2], imports=wasm_name("env") + wasm_name("f") +
                                      b"\x00\x00"),
        "refused-no-maximum": wasm_module(five, memory=b"\x00\x01"),
        "refused-grows": wasm_module(five, memory=b"\x01\x01\x02"),
        "refused-large": wasm_module(five, memory=b"\x01\x11\x11"),
        "refused-no-memory": wasm_module(five, memory=b""),
        "refused-initialize": wasm_module(five + [("_initialize", "", "", spin)]),
        "refused-kind": wasm_module(five, tail=wasm_section(12, b"\x00")),
        "refused-two-exports": wasm_module(five, tail=wasm_section(7, b"\x00")),
        "refused-nul-name": wasm_module(five + [("b\x00c", "", "", b"")]),
        "refused-twice": wasm_module(five + [("b", "", "i", const(7))]),
        "refused-types": wasm_module(five[:1], tail=wasm_section(
            1, leb(1025) + b"\x60\x00\x00" * 1025)),
        "refused-body": wasm_module(five[:1] + [("b", "", "i", b"\x01" * 65536 + const(1))]),
        # What only the runtime refuses: an add with nothing to add
        "invalid-code": wasm_module([("b", "", "i", add)]),
    }
    return seeds
