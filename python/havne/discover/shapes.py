from collections import Counter, namedtuple

Shape = namedtuple("Shape", "byte bit bits order signed")

WIDTHS = (1, 2, 3, 4)


def raw(payload, s):
    if s.bits == 1:
        return (payload[s.byte] >> s.bit) & 1
    n = s.bits // 8
    return int.from_bytes(payload[s.byte:s.byte + n], "big" if s.order == "be" else "little",
                          signed=s.signed)


def size(s):
    return max(1, s.bits // 8)


def span(s):
    return range(s.byte, s.byte + size(s))


def column(did, s):
    return [raw(p, s) for p in did.payloads]


def words(free, orders=("be", "le")):
    """Every whole-byte field over the free bytes: 1 to 4 bytes, each order
    for two or more, unsigned and signed"""
    out = []
    for b in sorted(free):
        for n in WIDTHS:
            if any(b + k not in free for k in range(n)):
                break
            for order in (("be",) if n == 1 else orders):
                for signed in (False, True):
                    out.append(Shape(b, 0, n * 8, order, signed))
    return out


def bits(free):
    return [Shape(b, k, 1, "le", False) for b in sorted(free) for k in range(8)]


def roughness(values):
    """The median step over the range: small for a smooth field, about a
    third for noise or a wrapping low byte"""
    lo, hi = min(values), max(values)
    if hi == lo:
        return 0.0
    steps = sorted(abs(b - a) for a, b in zip(values, values[1:]))
    return steps[len(steps) // 2] / (hi - lo)


def _counter(col):
    steps = Counter((b - a) & 0xFF for a, b in zip(col, col[1:]))
    step, n = steps.most_common(1)[0]
    return step != 0 and n >= 0.9 * (len(col) - 1)


def _checksum(did, b):
    others = [k for k in range(len(did.payloads[0])) if k != b]
    rules = (lambda p: sum(p[k] for k in range(b)) & 0xFF,
             lambda p: _xor(p[k] for k in range(b)),
             lambda p: sum(p[k] for k in others) & 0xFF,
             lambda p: _xor(p[k] for k in others))
    for rule in rules:
        hits = sum(1 for p in did.payloads if rule(p) == p[b])
        if hits >= 0.95 * len(did.payloads):
            return True
    return False


def _xor(values):
    out = 0
    for v in values:
        out ^= v
    return out


def byte_classes(did):
    """Each byte's class where it is plainly one: "static", "counter" or
    "checksum"; the rest are live"""
    out = {}
    length = len(did.payloads[0])
    for b in range(length):
        col = [p[b] for p in did.payloads]
        if len(set(col)) == 1:
            out[b] = "static"
    for b in range(length):
        col = [p[b] for p in did.payloads]
        if b in out:
            continue
        if b > 0 and len(set(col)) > 8 and _checksum(did, b):
            out[b] = "checksum"
        elif _counter(col):
            out[b] = "counter"
    return out


def moving_bits(did, b):
    """The bits of byte b that change at least once"""
    col = [p[b] for p in did.payloads]
    return [k for k in range(8) if len({(v >> k) & 1 for v in col}) > 1]
