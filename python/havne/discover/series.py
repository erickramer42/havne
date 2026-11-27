import bisect
import os
import re
import shutil
import tempfile
from collections import Counter, namedtuple

from ..recording import Recording, import_csv

Did = namedtuple("Did", "rx did times payloads")
Reference = namedtuple("Reference", "name source pid byte unit times values")
Reference.__doc__ = "A known quantity: source 'obd2' (pid and byte set) or 'oracle' (pid None)"

OBD2_NAMES = {"load": 0x04, "coolant": 0x05, "map": 0x0B, "rpm": 0x0C, "speed": 0x0D,
              "timing": 0x0E, "iat": 0x0F, "maf": 0x10, "throttle": 0x11, "fuel_level": 0x2F,
              "baro": 0x33, "module_voltage": 0x42, "ambient": 0x46, "oil_temp": 0x5C}
TOO_FEW = 20
TOO_FLAT = 3

_DID_KEY = re.compile(r"^([0-9A-F]{3,8}):62([0-9A-F]{4}):")
_PID_KEY = re.compile(r"^([0-9A-F]{3,8}):41([0-9A-F]{2}):(\d+)\.")


class Drive:
    """A recording read for discovery: each DID's payloads, each numeric
    PID signal, an oracle's channels on the drive's clock, and the markers;
    times in seconds"""

    def __init__(self, dids, references, markers, utc_us):
        self.dids = dids
        self.references = references
        self.markers = markers
        self.utc_us = utc_us
        self.oracle = []
        self.oracle_shift_s = None

    def reference(self, name):
        """The reference a catalog name means ("obd2.rpm", "obd2.pid_24",
        "oracle.<signal key>"), or None"""
        source, _, word = name.partition(".")
        if source == "oracle":
            return next((r for r in self.oracle if _key(r.name) == word), None)
        if source != "obd2":
            return None
        pid = OBD2_NAMES.get(word)
        if pid is None and word.startswith("pid_"):
            pid = int(word[4:], 16)
        found = [r for r in self.references if r.pid == pid]
        return min(found, key=lambda r: r.byte) if found else None


def too_slow(ref):
    return len(ref.times) < TOO_FEW or len(set(ref.values)) < TOO_FLAT


def _key(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def _samples(rec, s):
    times, texts = [], []
    t = s.first_us
    while t is not None:
        value, at_us = rec.text_at(s.index, t)
        if at_us == t:
            times.append(t / 1e6)
            texts.append(value)
        t = rec.step(s.index, t)
    return times, texts


def _did(rx, did, times, texts):
    payloads = [bytes.fromhex(x) for x in texts]
    length = Counter(len(p) for p in payloads).most_common(1)[0][0]
    kept = [(t, p) for t, p in zip(times, payloads) if len(p) == length]
    return Did(rx, did, [t for t, _ in kept], [p for _, p in kept])


def read(folder):
    """The Drive in a recording's folder"""
    with Recording(folder) as rec:
        dids, refs = [], []
        for s in rec.signals:
            if not s.samples:
                continue
            m = _DID_KEY.match(s.key)
            if m and s.kind == "text":
                dids.append(_did(m.group(1), m.group(2), *_samples(rec, s)))
                continue
            m = _PID_KEY.match(s.key)
            if m and s.kind == "number":
                times, texts = _samples(rec, s)
                refs.append(Reference(s.name, "obd2", int(m.group(2), 16), int(m.group(3)),
                                      s.unit, times, [float(x) for x in texts]))
        markers = [(m.host_us / 1e6, m.label) for m in rec.markers]
        return Drive(dids, refs, markers, rec.info.utc_start_us)


def _oracle_refs(csv_path):
    work = tempfile.mkdtemp(prefix="havne-oracle-")
    try:
        folder = os.path.join(work, "oracle")
        import_csv(csv_path, folder)
        with Recording(folder) as rec:
            out = []
            for s in rec.signals:
                if s.samples and s.kind == "number":
                    times, texts = _samples(rec, s)
                    out.append(Reference(s.name, "oracle", None, 0, s.unit, times,
                                         [float(x) for x in texts]))
            return out
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _agreement(drive_ref, oracle_ref, shift):
    from .fit import linear
    ys = drive_ref.values
    xs = [at(oracle_ref.times, oracle_ref.values, t + shift) for t in drive_ref.times]
    r = linear(xs, ys)
    return r[2] if r else 0.0


def align(drive_ref, oracle_ref, reach_s=120.0):
    """The shift that puts the oracle's clock on the drive's (oracle time =
    drive time + shift), by engine speed, coarse then fine"""
    best = max((k * 0.5 for k in range(int(-reach_s * 2), int(reach_s * 2) + 1)),
               key=lambda s: _agreement(drive_ref, oracle_ref, s))
    fine = [best + k * 0.02 for k in range(-25, 26)]
    return max(fine, key=lambda s: _agreement(drive_ref, oracle_ref, s))


def add_oracle(drive, csv_path):
    """The oracle's channels, its clock aligned to the drive's by engine speed"""
    refs = _oracle_refs(csv_path)
    rpm = drive.reference("obd2.rpm")
    theirs = next((r for r in refs if _key(r.name) == "engine_speed"), None)
    if rpm is None or theirs is None:
        raise ValueError("an oracle needs an engine speed channel, and the drive PID 0C, "
                         "to put its clock on the drive's")
    shift = align(rpm, theirs)
    drive.oracle_shift_s = shift
    drive.oracle = [r._replace(times=[t - shift for t in r.times]) for r in refs]
    return drive


def at(times, values, t):
    """values linearly interpolated at t, held past either end"""
    i = bisect.bisect_left(times, t)
    if i <= 0:
        return values[0]
    if i >= len(times):
        return values[-1]
    t0, t1 = times[i - 1], times[i]
    v0, v1 = values[i - 1], values[i]
    return v0 + (v1 - v0) * (t - t0) / (t1 - t0) if t1 > t0 else v1


def onto(ref, times, lag=0.0):
    """The reference at each of times, lag seconds earlier"""
    return [at(ref.times, ref.values, t - lag) for t in times]
