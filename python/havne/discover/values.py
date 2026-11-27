import math

from . import catalog as cat
from . import fit
from .context import choose_example
from .series import Reference, at, onto
from .shapes import column, roughness, span


def _corr(x, y):
    r = fit.linear(x, y)
    return 0.0 if r is None else math.copysign(math.sqrt(r[2]), r[0])


def _ranks(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    out = [0.0] * len(v)
    for k, i in enumerate(order):
        out[i] = float(k)
    return out


def _example_fields(ctx, did, shape, signal):
    e = choose_example(signal, shape, column(did, shape))
    return (e["scale"], e["offset"]) if e else (None, None)


def knock(ctx):
    """Fields zero most of the time that step negative together: the
    retard, its total the deepest of the others"""
    signal = ctx.by_key.get("knock_retard")
    if signal is None or "knock_retard" not in ctx.wanted:
        return
    for did in ctx.drive.dids:
        named = [f for f in ctx.found if f.key == "knock_retard" and f.did == did.did and
                 f.rx == did.rx]
        group = []
        for s in ctx.free_words(did, bits=(8,)):
            if not s.signed:
                continue
            col = column(did, s)
            zeros = sum(1 for v in col if v == 0) / len(col)
            neg = [v for v in col if v != 0]
            if zeros >= 0.5 and neg and sum(1 for v in neg if v < 0) >= 0.95 * len(neg):
                group.append((s, col))
        if len(group) + len(named) < 3:
            continue
        _name_knock(ctx, did, signal, named, group)


def _name_knock(ctx, did, signal, named, group):
    total = named[0] if named else None
    if total is None:
        for s, col in group:
            others = [c for t, c in group if t != s]
            hits = sum(1 for k in range(len(col)) if col[k] == min(o[k] for o in others))
            if hits >= 0.9 * len(col):
                e = cat.examples(signal, s.bits, True)[0]
                total = ctx.claim(did, s, "knock_retard", "likely", "siblings",
                                  "the deepest of its siblings at each read, zero but under load",
                                  hits / len(col), e["scale"], e["offset"])
                break
    if total is None:
        return
    n = 0
    for s, col in group:
        if (did.rx, did.did, s.byte) in ctx.used:
            continue
        n += 1
        f = ctx.claim(did, s, "knock_retard", "likely", "siblings",
                      f"a sibling of the total at byte {total.shape.byte}: zero but in its "
                      f"events, the total's encoding", 0.9, total.scale, total.offset)
        ctx.found[-1] = f._replace(name=f"{signal['name']} cylinder {n}")


def _pairs(ctx):
    out = []
    for s in ctx.by_key.values():
        if "pair_of" in s and s["pair_of"] in ctx.by_key:
            out.append((s["pair_of"], s["key"]))
    return out


def pairs(ctx):
    """A commanded and an actual field: one named, the other the field
    beside it that tracks it in the same encoding; or two unnamed fields
    of one DID, the smoother following the other"""
    for cmd, act in _pairs(ctx):
        if cmd not in ctx.wanted and act not in ctx.wanted:
            continue
        a, c = ctx.named(act), ctx.named(cmd)
        if (a is None) != (c is None):
            _pair_with(ctx, a or c, cmd if a else act)
    for cmd, act in _pairs(ctx):
        if ctx.open(cmd) and ctx.open(act):
            _pair_unnamed(ctx, cmd, act)


def _pair_with(ctx, known, other):
    did = ctx.did_of(known)
    mine = [known.scale * r + known.offset for r in column(did, known.shape)]
    best = None
    for s in ctx.free_words(did, bits=(known.shape.bits,)):
        if s.signed != known.shape.signed or (s.bits > 8 and s.order != known.shape.order):
            continue
        vals = [known.scale * r + known.offset for r in column(did, s)]
        r = fit.linear(mine, vals)
        if r and r[2] >= 0.8 and 0.7 <= r[0] <= 1.4 and (best is None or r[2] > best[0]):
            best = (r[2], s)
    if best:
        ctx.claim(did, best[1], other, "likely", "pair",
                  f"tracks {known.name} at byte {known.shape.byte} in its encoding, "
                  f"R2 {best[0]:.3f}", best[0], known.scale, known.offset)


def _pair_unnamed(ctx, cmd, act):
    signal = ctx.by_key[act]
    for did in ctx.drive.dids:
        cands = [(s, column(did, s)) for s in ctx.free_words(did, bits=(16,))
                 if s.signed and len(set(column(did, s))) > 5 and roughness(column(did, s)) < 0.1]
        for i, (s1, c1) in enumerate(cands):
            for s2, c2 in cands[i + 1:]:
                if set(span(s1)) & set(span(s2)) or s1.order != s2.order:
                    continue
                r = fit.linear(c1, c2)
                if not r or r[2] < 0.9 or not 0.8 <= r[0] <= 1.25:
                    continue
                e = cat.examples(signal, 16, True)
                if not e or not _parks(ctx, did, c1, c2, signal, ctx.by_key[cmd]):
                    continue
                lead, follow = (s1, s2) if roughness(c1) >= roughness(c2) else (s2, s1)
                words_ = (f"two fields of one encoding that track each other (R2 {r[2]:.3f}); "
                          f"the smoother follows: no reference names which pair")
                ctx.claim(did, lead, cmd, "likely", "pair", words_, r[2], e[0]["scale"],
                          e[0]["offset"])
                ctx.claim(did, follow, act, "likely", "pair", words_, r[2], e[0]["scale"],
                          e[0]["offset"])
                return


def _parks(ctx, did, c1, c2, *signals):
    """A pair whose notes say it parks at 0 at idle must; any other pair
    must not"""
    parks = all(_idle_zero(ctx, did, c) for c in (c1, c2))
    return parks == any("parks at 0 at idle" in s.get("notes", "") for s in signals)


def _idle_zero(ctx, did, col):
    rpm = ctx.drive.reference("obd2.rpm")
    if rpm is None:
        return False
    idle = [c for c, r in zip(col, onto(rpm, did.times)) if 500 < r < 1000]
    return len(idle) >= 5 and sum(1 for c in idle if c == 0) >= 0.9 * len(idle)


def temperature(ctx):
    """A smooth field that starts at ambient and warms: oil and fluid lag
    coolant, intake air doesn't"""
    amb, cool = ctx.drive.reference("obd2.ambient"), ctx.drive.reference("obd2.coolant")
    if amb is None or cool is None:
        return
    ambient = amb.values[0]
    open_ = [s for s in ctx.by_key.values() if "temperature" in s["recognizers"] and
             ctx.open(s["key"])]
    for did in ctx.drive.dids:
        for s in ctx.free_words(did, bits=(8, 16)):
            col = column(did, s)
            if len(set(col)) < 5 or roughness(col) > 0.05:
                continue
            got = _temperature_of(ctx, did, s, col, open_, ambient, cool)
            if got is not None:
                signal, e, lags = got
                ctx.claim(did, s, signal["key"], "likely", "temperature",
                          f"ambient at the cold start ({ambient:g} deg C), warms smoothly, "
                          f"{'below coolant while it warms' if lags else 'with the air'}",
                          0.8, e["scale"], e["offset"])
                open_ = [x for x in open_ if x["key"] != signal["key"]]


def _temperature_of(ctx, did, s, col, open_, ambient, cool):
    coolant = onto(cool, did.times)
    for signal in open_:
        for e in cat.examples(signal, s.bits, s.signed):
            v = [r * e["scale"] + e["offset"] for r in col]
            if abs(sum(v[:3]) / 3 - ambient) > 3 or max(v) > max(coolant) + 15:
                continue
            if min(v) < ambient - 5 or v[-1] < v[0] + 2:
                continue
            below = sum(1 for a, c in zip(v, coolant) if a < c - 2) / len(v)
            lags = below >= 0.5
            if lags == ("lags coolant" in signal.get("notes", "")):
                return signal, e, lags
    return None


def gears(ctx):
    """Small integers that hold while rpm over road speed holds: the gear;
    of two such, the one that changes first is the commanded"""
    rpm, speed = ctx.drive.reference("obd2.rpm"), ctx.drive.reference("obd2.speed")
    if rpm is None or speed is None or not (ctx.open("gear") or ctx.open("gear_commanded")):
        return
    for did in ctx.drive.dids:
        r = onto(rpm, did.times)
        v = onto(speed, did.times)
        found = [s for s in ctx.free_words(did, bits=(8,)) if not s.signed and
                 _gear_like(column(did, s), r, v)]
        if not found:
            continue
        if len(found) >= 2:
            a, b = found[:2]
            lead, follow = (a, b) if _leads(column(did, a), column(did, b)) else (b, a)
            ctx.claim(did, follow, "gear", "likely", "ratio_clusters",
                      "holds while rpm over road speed holds; follows the field at byte "
                      f"{lead.byte}", 0.8)
            ctx.claim(did, lead, "gear_commanded", "likely", "ratio_clusters",
                      f"a gear that changes before the one at byte {follow.byte}", 0.8)
        else:
            ctx.claim(did, found[0], "gear", "likely", "ratio_clusters",
                      "holds while rpm over road speed holds", 0.8)
        return


def _gear_like(col, r, v):
    if len(set(col)) < 3 or max(col) > 10:
        return False
    groups = {}
    for g, a, b in zip(col, r, v):
        if g > 0 and b >= 10 and a > 800:
            groups.setdefault(g, []).append(a / b)
    means = []
    for g in sorted(groups):
        x = sorted(groups[g])
        if len(x) < 3:
            continue
        cut = len(x) // 10
        x = x[cut:len(x) - cut] or x
        m = sum(x) / len(x)
        sd = math.sqrt(sum((y - m) ** 2 for y in x) / len(x))
        if sd / m > 0.15:
            return False
        means.append(m)
    return len(means) >= 2 and all(a > b for a, b in zip(means, means[1:]))


def _leads(a, b):
    ahead = sum(1 for k in range(len(a) - 1) if a[k] != b[k] and a[k] == b[k + 1])
    behind = sum(1 for k in range(len(a) - 1) if a[k] != b[k] and b[k] == a[k + 1])
    return ahead >= behind


def slip(ctx):
    """Engine speed less the turbine's: a fit to that difference"""
    turbine = ctx.named("turbine_speed")
    rpm = ctx.drive.reference("obd2.rpm")
    if turbine is None or rpm is None or not ctx.open("converter_slip"):
        return
    did = ctx.did_of(turbine)
    t, tv = ctx.series(turbine)
    diff = Reference("RPM minus turbine speed", "derived", None, 0, "RPM", t,
                     [at(rpm.times, rpm.values, x) - y for x, y in zip(t, tv)])
    best = None
    for s in ctx.free_words(did, bits=(16,)):
        f = fit.against(did.times, column(did, s), diff)
        if f and f.r2 >= 0.9 and (best is None or f.r2 > best[0].r2):
            best = (f, s)
    if best:
        f, s = best
        scale = fit.snap(f.a) or f.a
        status = "solved" if f.r2 >= 0.99 and fit.snap(f.a) else "likely"
        ctx.claim(did, s, "converter_slip", status, "derived",
                  f"fit to engine speed less the turbine speed at byte {turbine.shape.byte}, "
                  f"R2 {f.r2:.4f}", f.r2, scale, 0.0)


def engine_off_zero(ctx):
    """Zero with the engine stopped, never with it running: line pressure
    beside a gear, oil pressure with rpm, else the alternator's current"""
    rpm = ctx.drive.reference("obd2.rpm")
    keys = [k for k in ("line_pressure", "oil_pressure", "alternator_current") if ctx.open(k)]
    if rpm is None or not keys:
        return
    for did in ctx.drive.dids:
        r = onto(rpm, did.times)
        for s in ctx.free_words(did, bits=(16, 8)):
            if s.signed:
                continue
            col = column(did, s)
            stopped = [c for c, x in zip(col, r) if x < 50]
            going = [c for c, x in zip(col, r) if x > 600]
            if len(stopped) < 5 or len(going) < 20 or len(set(col)) < 5:
                continue
            if sum(1 for c in stopped if c == 0) < 0.9 * len(stopped):
                continue
            if sum(1 for c in going if c > 0) < 0.95 * len(going):
                continue
            key = _engine_off_key(ctx, did, col, r, keys)
            if key is None:
                continue
            scale, offset = _example_fields(ctx, did, s, ctx.by_key[key])
            if scale is None:
                continue
            ctx.claim(did, s, key, "likely", "engine_off_zero",
                      "zero with the engine stopped, never with it running", 0.7, scale, offset)
            keys.remove(key)
            if not keys:
                return


def _engine_off_key(ctx, did, col, r, keys):
    gear = [f for f in ctx.found if f.key in ("gear", "gear_commanded") and f.did == did.did]
    if gear and "line_pressure" in keys:
        return "line_pressure"
    if "oil_pressure" in keys and _corr(col, r) > 0.8:
        return "oil_pressure"
    return "alternator_current" if "alternator_current" in keys else None


def adaptation(ctx):
    """Slow and bounded, stepping down at knock events"""
    if not ctx.open("knock_adaptation"):
        return
    knock = [f for f in ctx.found if f.key == "knock_retard"]
    if not knock:
        return
    kt, kv = ctx.series(knock[0])
    events = [t for t, v in zip(kt, kv) if v < 0]
    signal = ctx.by_key["knock_adaptation"]
    for did in ctx.drive.dids:
        for s in ctx.free_words(did, bits=(8, 16)):
            col = column(did, s)
            steps = [(did.times[k + 1], col[k + 1] - col[k]) for k in range(len(col) - 1)
                     if col[k + 1] != col[k]]
            downs = [t for t, d in steps if d < 0]
            if not downs or len(steps) > 0.25 * len(col) or len(set(col)) < 3:
                continue
            near = sum(1 for t in downs if any(abs(t - e) < 3.0 for e in events))
            if near < 0.6 * len(downs):
                continue
            scale, offset = _example_fields(ctx, did, s, signal)
            if scale is None:
                continue
            ctx.claim(did, s, "knock_adaptation", "likely", "slow_adaptation",
                      f"moves at {len(steps)} of {len(col)} reads, down at knock events", 0.7,
                      scale, offset)
            return


def maf_voltage(ctx):
    """Rises with air flow but not in proportion: a sensor's voltage"""
    maf = ctx.drive.reference("obd2.maf")
    if maf is None or not ctx.open("maf_voltage"):
        return
    signal = ctx.by_key["maf_voltage"]
    for did in ctx.drive.dids:
        for s in ctx.free_words(did, bits=(16,)):
            xs, ys = fit.steady_pairs(did.times, column(did, s), maf)
            if len(xs) < 30 or len(set(xs)) < 10:
                continue
            rank = _corr(_ranks(xs), _ranks(ys))
            r = fit.linear(xs, ys)
            if rank < 0.97 or r is None or r[2] > 0.995:
                continue
            scale, offset = _example_fields(ctx, did, s, signal)
            if scale is None:
                continue
            ctx.claim(did, s, "maf_voltage", "likely", "derived",
                      f"rises with air flow (rank r {rank:.3f}), not in proportion "
                      f"(R2 {r[2]:.3f})", rank, scale, offset)
            return


def injected(ctx):
    """Fuel for each stroke: air flow over rpm over lambda, fitted"""
    if not ctx.open("injected_quantity"):
        return
    maf, rpm = ctx.drive.reference("obd2.maf"), ctx.drive.reference("obd2.rpm")
    lam = ctx.drive.reference("obd2.pid_24")
    if maf is None or rpm is None or lam is None:
        return
    signal = ctx.by_key["injected_quantity"]
    t = sorted(set(maf.times))
    vals = []
    for x in t:
        r, lv = at(rpm.times, rpm.values, x), at(lam.times, lam.values, x)
        vals.append(at(maf.times, maf.values, x) / r / lv * 1000 if r > 100 and lv > 0.3 else 0.0)
    per_stroke = Reference("air flow over rpm over lambda", "derived", None, 0, "", t, vals)
    for did in ctx.drive.dids:
        for s in ctx.free_words(did, bits=(16,)):
            f = fit.against(did.times, column(did, s), per_stroke)
            if not f or f.r2 < 0.9:
                continue
            scale, offset = _example_fields(ctx, did, s, signal)
            if scale is None:
                continue
            ctx.claim(did, s, "injected_quantity", "likely", "derived",
                      f"follows air flow over rpm over lambda, R2 {f.r2:.3f}; the scale is "
                      f"the catalog's example", f.r2, scale, offset)
            return
