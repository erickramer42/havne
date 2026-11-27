import math
from collections import defaultdict, namedtuple

from . import catalog as cat
from . import fit
from .context import choose_example
from .found import Found
from .series import Reference, at, too_slow
from .shapes import column, span, words

Proposal = namedtuple("Proposal", "r2 key ref did shape fit")

SOLVED_R2 = 0.99
RELATED_R2 = 0.3
ORDER_MARGIN = 3.0
SEEN = 0.3


def _derived_boost(drive):
    m, b = drive.reference("obd2.map"), drive.reference("obd2.baro")
    if m is None or b is None:
        return None
    values = [v - at(b.times, b.values, t) for t, v in zip(m.times, m.values)]
    return Reference("MAP minus baro", "derived", None, 0, "kPa", m.times, values)


def references(drive, signal, notes):
    """The references that can name the signal by a fit, each (name, Reference);
    a reference too slow to fit is noted, not used"""
    out = []
    theirs = drive.reference(f"oracle.{signal['key']}")
    if theirs is not None:
        out.append((f"oracle.{signal['key']}", theirs))
    if signal["key"] == "boost":
        r = _derived_boost(drive)
        if r is not None:
            out.append(("derived.map_minus_baro", r))
        return out
    if "linear_fit" not in signal["recognizers"]:
        return out
    for name in signal["references"]:
        if name.startswith("oracle."):
            continue
        r = drive.reference(name)
        if r is None:
            continue
        if too_slow(r):
            notes[name] = (f"reference too slow: {len(r.times)} readings, "
                           f"{len(set(r.values))} values ({r.name})")
            continue
        out.append((name, r))
    return out


def _fit(did, shape, col, ref):
    first = fit.against(did.times, col, ref)
    if first is None or first.r2 < SEEN:
        return first
    return fit.against(did.times, col, ref, fit.LAGS)


def propose(drive, free, signals, notes, orders=("be", "le")):
    """Every (field, reference) fit worth a look, best first"""
    wanted = [(s, references(drive, s, notes)) for s in signals]
    out = []
    for did in drive.dids:
        seen = {}
        for shape in words(free[(did.rx, did.did)], orders):
            col = column(did, shape)
            if len(set(col)) < 3:
                continue
            for s, refs in wanted:
                for name, ref in refs:
                    k = (tuple(col), name)
                    if k not in seen:
                        seen[k] = _fit(did, shape, col, ref)
                    f = seen[k]
                    if f is not None and f.r2 >= RELATED_R2:
                        out.append(Proposal(f.r2, s["key"], name, did, shape, f))
    out.sort(key=_rank)
    return out


def _rank(p):
    """Best fit first; within a ten-thousandth of R2, a scale that snaps,
    then the narrower field (a neighbor's byte below a field fits as
    well; extend() takes a low byte that belongs)"""
    return (-math.floor(p.r2 * 1e4), fit.snap(p.fit.a) is None, p.shape.bits, -p.r2)


def module_orders(proposals):
    """Each module's byte order prior: the order of most of its strong
    multi-byte fits, one per catalog signal"""
    best = {}
    for p in proposals:
        if p.shape.bits >= 16 and p.r2 >= SOLVED_R2:
            best.setdefault((p.did.rx, p.key), p)
    votes = defaultdict(lambda: defaultdict(int))
    for (rx, _), p in best.items():
        votes[rx][p.shape.order] += 1
    return {rx: max(v, key=lambda o: (v[o], o == "be")) for rx, v in votes.items()}


def _beaten_by_prior(p, proposals, prior):
    """A fit in a module's other order must beat the best fit in its own
    order over the same bytes clearly: a third of its unexplained part"""
    if p.shape.bits < 16 or prior.get(p.did.rx, p.shape.order) == p.shape.order:
        return False
    rival = max((q.r2 for q in proposals
                 if q.did is p.did and q.key == p.key and q.shape.bits >= 16 and
                 q.shape.order == prior[p.did.rx] and set(span(q.shape)) & set(span(p.shape))),
                default=None)
    return rival is not None and (1 - p.r2) * ORDER_MARGIN > 1 - rival


def _sign(p, signal, scale):
    """A field whose readings never reach its top bit reads alike either
    way: take the sign of the catalog's example of its width and scale"""
    col = column(p.did, p.shape._replace(signed=False))
    if max(col) < 1 << (p.shape.bits - 1):
        for e in cat.examples(signal, p.shape.bits):
            if scale is not None and abs(e["scale"] / scale - 1) < 0.02:
                return e["signed"]
    return p.shape.signed


def found_of(p, signal, ref, status=None):
    scale = fit.snap(p.fit.a)
    shape = p.shape._replace(signed=_sign(p, signal, scale))
    col = column(p.did, shape)
    a = scale if scale is not None else p.fit.a
    spread = max(ref.values) - min(ref.values)
    offset = fit.snap_offset(fit.offset_for(p.did.times, col, ref, a, p.fit.lag), a, spread)
    words_ = f"fit to {ref.name} ({p.ref}), R2 {p.r2:.4f}"
    if ref.unit != signal["unit"] and ref.source == "obd2":
        e = choose_example(signal, shape, col)
        if e is not None:
            return Found(p.did.rx, p.did.did, shape, signal["key"], signal["name"],
                         signal["unit"], e["scale"], e["offset"], status or "likely", "fit",
                         words_ + f" in {ref.unit}: the scale is the catalog's example", p.r2)
    if status is None:
        capped = signal.get("needs_oracle") and ref.source != "oracle"
        status = "solved" if p.r2 >= SOLVED_R2 and scale is not None and not capped else "related"
    if p.fit.lag:
        words_ += f", lag {p.fit.lag:+.1f} s"
    if status == "related" and signal.get("needs_oracle") and ref.source != "oracle":
        words_ += "; the enhanced value may differ from the PID's: an oracle confirms it"
    return Found(p.did.rx, p.did.did, shape, signal["key"], signal["name"], signal["unit"], a,
                 offset, status, "fit", words_, p.r2)


def assign(drive, proposals, by_key, used):
    """Each field its best catalog signal, best fits first: a signal not
    per cylinder, bank or sensor takes one field"""
    prior = module_orders(proposals)
    taken = set()
    out = []
    for p in proposals:
        bytes_ = {(p.did.rx, p.did.did, b) for b in span(p.shape)}
        signal = by_key[p.key]
        if bytes_ & used or (p.key in taken and "per" not in signal):
            continue
        if _beaten_by_prior(p, proposals, prior):
            continue
        if "per" in signal and p.key in taken and p.r2 < SOLVED_R2:
            continue
        out.append(found_of(p, signal, _ref(drive, p.ref)))
        used |= bytes_
        taken.add(p.key)
    return out, prior


def _plausible(p, signal):
    """A loose fit names a field only at a plain scale of the catalog's
    sign that keeps its values in the signal's range"""
    scale = fit.snap(p.fit.a)
    if scale is None:
        return False
    if scale < 0 and not any(e["scale"] < 0 for e in signal["encodings"]):
        return False
    lo, hi = signal["range"]
    vals = [scale * r + p.fit.b for r in column(p.did, p.shape)]
    room = 0.1 * (hi - lo)
    return min(vals) >= lo - room and max(vals) <= hi + room


def related(drive, proposals, by_key, used):
    """Fields left over that follow a reference loosely: a filtered or
    per-cylinder copy of a PID, never solved by it"""
    out = []
    for p in proposals:
        bytes_ = {(p.did.rx, p.did.did, b) for b in span(p.shape)}
        if bytes_ & used or not _plausible(p, by_key[p.key]):
            continue
        out.append(found_of(p, by_key[p.key], _ref(drive, p.ref), "related"))
        used |= bytes_
    return out


def _ref(drive, name):
    if name == "derived.map_minus_baro":
        return _derived_boost(drive)
    return drive.reference(name)
