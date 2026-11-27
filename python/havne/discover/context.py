from . import catalog as cat
from .found import Found, decoded
from .series import onto
from .shapes import Shape, column, span, words


class Context:
    """What the recognizers share: the drive, what is named so far, the
    catalog and the bytes still free"""

    def __init__(self, drive, found, by_key, signals, classes, used):
        self.drive = drive
        self.found = found
        self.by_key = by_key
        self.wanted = {s["key"] for s in signals}
        self.classes = classes
        self.used = used

    def free(self, did):
        cls = self.classes[(did.rx, did.did)]
        return {b for b in range(len(did.payloads[0]))
                if (did.rx, did.did, b) not in self.used and b not in cls}

    def free_bits(self, did):
        out = []
        for b in sorted(self.free(did)):
            col = [p[b] for p in did.payloads]
            for k in range(8):
                if len({(v >> k) & 1 for v in col}) > 1:
                    out.append(Shape(b, k, 1, "le", False))
        return out

    def free_words(self, did, bits=None):
        return [s for s in words(self.free(did)) if bits is None or s.bits in bits]

    def taken(self, key):
        return any(f.key == key for f in self.found)

    def open(self, key):
        return key in self.wanted and key in self.by_key and not self.taken(key)

    def named(self, key):
        return next((f for f in self.found if f.key == key and f.status in ("solved", "likely")),
                    None)

    def did_of(self, f):
        return next(d for d in self.drive.dids if d.rx == f.rx and d.did == f.did)

    def series(self, f):
        """A named field's (times, values)"""
        did = self.did_of(f)
        return did.times, decoded(f, did)

    def ref_on(self, name, times, lag=0.0):
        r = self.drive.reference(name)
        return None if r is None else onto(r, times, lag)

    def bit_claimed(self, did, shape):
        return (did.rx, did.did, shape.byte, shape.bit) in self.used

    def claim(self, did, shape, key, status, method, words_, score, scale=1.0, offset=0.0):
        signal = self.by_key.get(key)
        f = Found(did.rx, did.did, shape, key, signal["name"] if signal else key,
                  signal["unit"] if signal else "", scale, offset, status, method, words_, score)
        self.found.append(f)
        if shape.bits == 1:
            self.used.add((did.rx, did.did, shape.byte, shape.bit))
            bits_left = [k for k in range(8) if (did.rx, did.did, shape.byte, k) not in self.used
                         and len({(p[shape.byte] >> k) & 1 for p in did.payloads}) > 1]
            if not bits_left:
                self.used.add((did.rx, did.did, shape.byte))
        else:
            for b in span(shape):
                self.used.add((did.rx, did.did, b))
        return f


def choose_example(signal, shape, col):
    """The catalog example of the field's width and sign whose values fill
    most of the signal's plausible range without leaving it"""
    lo, hi = signal["range"]
    best = None
    for e in cat.examples(signal, shape.bits, shape.signed) or cat.examples(signal, shape.bits):
        vals = [r * e["scale"] + e["offset"] for r in col]
        a, b = min(vals), max(vals)
        if a < lo - 0.05 * (hi - lo) or b > hi + 0.05 * (hi - lo):
            continue
        cover = (b - a) / (hi - lo)
        if best is None or cover > best[0]:
            best = (cover, e)
    return None if best is None else best[1]


def raw_column(did, shape):
    return column(did, shape)
