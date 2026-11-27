import bisect
import math
from collections import namedtuple

from .series import at

Fit = namedtuple("Fit", "a b r2 lag n")

LAGS = [round(-1.5 + 0.1 * k, 1) for k in range(31)]
STEADY = 0.05
TRIM = 0.9


def linear(x, y):
    n = len(x)
    if n < 3:
        return None
    mx = sum(x) / n
    my = sum(y) / n
    sxx = sum((v - mx) ** 2 for v in x)
    syy = sum((v - my) ** 2 for v in y)
    if sxx == 0 or syy == 0:
        return None
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    a = sxy / sxx
    return a, my - a * mx, sxy * sxy / (sxx * syy)


def steady_pairs(times, raws, ref, lag=0.0, frac=STEADY):
    """(raw, reference) where neither moves much around the read: two
    polls a fraction of a second apart only agree on a steady value"""
    span = (max(ref.values) - min(ref.values)) or 1.0
    rspan = (max(raws) - min(raws)) or 1.0
    xs, ys = [], []
    for k in range(1, len(raws) - 1):
        t = times[k] - lag
        i = bisect.bisect_left(ref.times, t)
        if i <= 0 or i >= len(ref.times):
            continue
        if abs(ref.values[i] - ref.values[i - 1]) > frac * span:
            continue
        if abs(raws[k + 1] - raws[k - 1]) > 2 * frac * rspan:
            continue
        xs.append(raws[k])
        ys.append(at(ref.times, ref.values, t))
    return xs, ys


def trimmed(xs, ys, keep=TRIM):
    first = linear(xs, ys)
    if first is None:
        return None
    a, b, _ = first
    order = sorted(range(len(xs)), key=lambda i: abs(ys[i] - a * xs[i] - b))
    kept = order[:max(3, int(len(xs) * keep))]
    again = linear([xs[i] for i in kept], [ys[i] for i in kept])
    return again if again is not None else first


def unpinned(times, raws, ref):
    """The reads off a value the field rests on at either end of its range
    while the reference goes on moving (a clamp, or zero with the engine
    stopped): a fit runs where the field moves"""
    lo, hi = min(raws), max(raws)
    n = len(raws)
    spread = (max(ref.values) - min(ref.values)) or 1.0
    drop = set()
    for v in (lo, hi):
        at_v = [at(ref.times, ref.values, t) for t, r in zip(times, raws) if r == v]
        if len(at_v) >= 0.05 * n and max(at_v) - min(at_v) > 0.1 * spread:
            drop.add(v)
    if not drop or len(drop) == len(set(raws)):
        return times, raws
    kept = [(t, r) for t, r in zip(times, raws) if r not in drop]
    if len(kept) < 0.3 * n:
        return times, raws
    return [t for t, _ in kept], [r for _, r in kept]


def against(times, raws, ref, lags=(0.0,), min_pairs=20):
    """The best Fit of the reference on the raws over the lags: a and b
    from the trimmed fit, r2 from every steady pair (the ranking score)"""
    best = None
    times, raws = unpinned(times, raws, ref)
    for lag in lags:
        xs, ys = steady_pairs(times, raws, ref, lag)
        if len(xs) < min_pairs or len(set(xs)) < 5:
            continue
        whole = linear(xs, ys)
        if whole is None:
            continue
        if best is None or whole[2] > best.r2:
            a, b, _ = trimmed(xs, ys)
            best = Fit(a, b, whole[2], lag, len(xs))
    return best


def offset_for(times, raws, ref, a, lag):
    times, raws = unpinned(times, raws, ref)
    xs, ys = steady_pairs(times, raws, ref, lag)
    if not xs:
        return 0.0
    res = sorted(y - a * x for x, y in zip(xs, ys))
    return res[len(res) // 2]


def _scales():
    """(scale, plainness): 1/2^n and decimals first, then percent of a
    full count, then their small multiples"""
    out = {}

    def put(v, rank):
        out[v] = min(rank, out.get(v, rank))

    for n in range(0, 17):
        put(1 / 2 ** n, 0)
        put(100 / 2 ** n, 1)
        if n >= 4:
            put(100 / (2 ** n - 1), 1)
        for m in (3, 5):
            put(m / 2 ** n, 2)
    for k in range(0, 5):
        put(1 / 10 ** k, 0)
        for m in (2, 5):
            put(m / 10 ** k, 1)
    return sorted(out.items())


SCALES = _scales()


def snap(a, tolerance=0.02):
    """The plainest rational form within tolerance of a (within 1.5 % a
    plainer form wins over a closer one), signed as a, or None"""
    if a == 0 or not math.isfinite(a):
        return None
    m = abs(a)
    near = [(rank, abs(math.log(m / c)), c) for c, rank in SCALES if abs(m / c - 1) <= tolerance]
    if not near:
        return None
    close = [x for x in near if x[1] <= 0.015] or near
    return math.copysign(min(close)[2], a)


def snap_offset(b, scale, spread):
    """b at a whole unit, else a half, when that close (a percent of the
    value's spread or a step of the scale); else on the scale's grain"""
    near = max(abs(scale), spread * 0.01)
    for step in (10.0, 1.0, 0.5):
        whole = round(b / step) * step
        if abs(whole - b) <= near:
            return whole + 0.0
    grain = abs(scale)
    return round(b / grain) * grain + 0.0
