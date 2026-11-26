# File/layer: fuzz/seeds_dump.py | Python 3, no packages; fuzz/seeds.py imports it
# Testing: FUZZ: CI's fuzz job, fuzz_dump_file started from what it writes
#
# The dump files among the seeds (ecuio/dump.h's format 1), built field by
# field: a file of one range, of as many as a file holds and of a record
# at its bounds, and a file of each refusal. Every value is made up.

import hashlib
import json
import struct

DUMP_MAGIC = bytes([0x89, 0x48, 0x56, 0x44, 0x0D, 0x0A, 0x1A, 0x0A])


def dump_range(name, address, size, offset, step="read", session="extended"):
    return {"name": name, "step": step, "session": session,
            "address": "%08X" % address, "bytes": size, "offset": offset}


def dump_record(image, ranges):
    """A dump's record (ecuio/dump.h) for image, every key set"""
    return {
        "havne_dump": 1, "kind": "read", "utc": 1795312345, "core": "0.9.1",
        "image": {"bytes": len(image), "sha256": hashlib.sha256(image).hexdigest()},
        "ranges": ranges,
        "module": {"rx": "7E8", "tx": "7E0",
                   "ident": [{"did": "D220", "role": "hardware", "hex": "48572D31"},
                             {"did": "D221", "role": "base", "hex": "00FF7A"}],
                   "obd": {"vin": "ZZZFAKE11M0000001", "calids": ["FAKECAL-0001"],
                           "cvns": ["0A1B2C3D"], "name": "ECM-MadeUp"}},
        "plugin": {"id": "talker", "version": "1.0.0", "module_sha256": "a" * 64,
                   "fingerprint": "b" * 64, "operation": "copy"},
        "transport": {"scheme": "mock", "adapter": "Made-up Adapter", "firmware": "1.0"},
        "read": {"passes": 2, "seconds": 12, "volts_mv": 12600, "volts_min_mv": 12100,
                 "rpm": 0, "kmh": 0, "parked_tick": True, "unlocks": 1,
                 "writes_before": 0},
    }


def dump_file(image, record, text=None, fmt=1, image_sum=None, flags=0, reserved=bytes(32)):
    """A dump file's bytes: the 128-byte header, the image, the record"""
    if text is None:
        text = json.dumps(record).encode()
    if image_sum is None:
        image_sum = hashlib.sha256(image).digest()
    header = (DUMP_MAGIC + struct.pack("<IIQII", fmt, 128, len(image), len(text), flags) +
              image_sum + hashlib.sha256(text).digest() + reserved)
    assert len(header) == 128
    return header + image + text


def dump_seeds():
    """fuzz_dump_file's form: a byte whose low bit is memory (0) or the
    disk (1) and whose next two pick the name, then the file"""
    image = bytes((i * 131 ^ (i >> 8) ^ 0x5A) & 0xFF for i in range(768))
    two = [dump_range("calibration", 0x00010000, 512, 0),
           dump_range("boot", 0x00020000, 256, 512, "upload", "programming")]
    plain = dump_file(image, dump_record(image, two))
    one = dump_file(image[:64], dump_record(image[:64], [dump_range("all", 0x10000, 64, 0)]))
    many = [dump_range("r%d" % k, 0x10000 + 16 * k, 12, 12 * k) for k in range(64)]
    widest = dump_record(image, [dump_range("n" * 31, 0xFFFFFF00 - 256 * k, 12, 12 * k)
                                 for k in range(64)])
    widest["utc"] = 253402300799
    widest["module"] = {
        "rx": "18DAF110", "tx": "18DA10F1",
        "ident": [{"did": "%04X" % (0xF180 + k), "role": "software", "hex": "FF" * 64}
                  for k in range(8)],
        "obd": {"vin": "ZZZFAKE11M0000001", "calids": ["C" * 16] * 8,
                "cvns": ["FFFFFFFF"] * 8, "name": "N" * 20}}
    widest["plugin"] = {"id": "t" * 31, "version": "65535.65535.65",
                        "module_sha256": "f" * 64, "fingerprint": "e" * 64,
                        "operation": "o" * 31}
    widest["transport"] = {"scheme": "s" * 15, "adapter": "A" * 63, "firmware": "F" * 63}
    widest["read"].update(seconds=4294967295, unlocks=64, writes_before=4294967295)
    seeds = {
        "two_ranges": b"\x00" + plain,
        "one_range": b"\x00" + one,
        "most_ranges": b"\x00" + dump_file(image, dump_record(image, many)),
        "widest": b"\x00" + dump_file(image, widest),
        "bare": b"\x00" + dump_file(image, bare_record(image, two)),
        # On the disk: under its own name, another copy's, no copy's, a
        # partial's
        "disk_own": b"\x01" + plain,
        "disk_other": b"\x03" + plain,
        "disk_plain": b"\x05" + plain,
        "disk_partial": b"\x07" + plain,
        "disk_widest": b"\x01" + dump_file(image, widest),
    }
    # A file of each refusal
    damaged = bytearray(plain)
    damaged[128 + 100] ^= 0x01
    edited = bytearray(plain)
    edited[-20] ^= 0x01
    gap = dump_record(image, [dump_range("calibration", 0x10000, 500, 0),
                              dump_range("boot", 0x20000, 256, 512)])
    over = dump_record(image, [dump_range("calibration", 0x10000, 512, 0),
                               dump_range("boot", 0x100FF, 256, 512)])
    extra = dict(dump_record(image, two), notes="none")
    later = dict(dump_record(image, two), havne_dump=2)
    refusals = {
        "no_magic": b"PK" + plain[2:],
        "short": plain[:100],
        "later_format": dump_file(image, later, fmt=2),
        "record_format": dump_file(image, later),
        "cut": plain[:-1],
        "longer": plain + b"\n",
        "flags": dump_file(image, dump_record(image, two), flags=1),
        "reserved": dump_file(image, dump_record(image, two), reserved=b"\x01" + bytes(31)),
        "image_damaged": bytes(damaged),
        "record_edited": bytes(edited),
        "ranges_gap": dump_file(image, gap),
        "ranges_overlap": dump_file(image, over),
        "unknown_key": dump_file(image, extra),
        "not_json": dump_file(image, None, text=b"{"),
    }
    for name, data in refusals.items():
        seeds["refused_" + name] = b"\x00" + data
    seeds["disk_image_damaged"] = b"\x01" + bytes(damaged)
    seeds["disk_record_edited"] = b"\x01" + bytes(edited)
    return seeds


def bare_record(image, ranges):
    """A record with nothing a dump may leave out"""
    record = dump_record(image, ranges)
    record["module"] = {"rx": "7E8", "tx": "7E0", "ident": []}
    record["read"] = {"passes": 2, "seconds": 0, "parked_tick": False, "unlocks": 0,
                      "writes_before": 0}
    record["transport"] = {"scheme": "mock", "adapter": "", "firmware": ""}
    return record
