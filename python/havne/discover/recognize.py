import math

from . import values
from .context import Context
from .found import Found
from .series import onto
from .shapes import Shape, column, moving_bits, roughness

NOISE_ROUGH = 0.12
PROMPT_ON = 0.6
PROMPT_OFF = 0.25


def run(drive, found, by_key, signals, classes, used):
    """The recognizers in order, each naming what the fits left: the
    prompts' bits first, the bits that follow named fields last"""
    ctx = Context(drive, list(found), by_key, signals, classes, used)
    start = len(ctx.found)
    prompts(ctx)
    values.knock(ctx)
    values.pairs(ctx)
    values.temperature(ctx)
    values.gears(ctx)
    values.slip(ctx)
    values.engine_off_zero(ctx)
    values.adaptation(ctx)
    values.maf_voltage(ctx)
    values.injected(ctx)
    lock(ctx)
    brake(ctx)
    clutch(ctx)
    return ctx.found[start:]


def windows(markers):
    """Each prompt's (from, to) times: "prompt <key>" opens one, "prompt
    end" or the next prompt closes it"""
    out = {}
    open_ = None
    for t, label in sorted(markers):
        words = label.split()
        if len(words) != 2 or words[0] != "prompt":
            continue
        if open_ is not None:
            out.setdefault(open_[1], []).append((open_[0], t))
            open_ = None
        if words[1] != "end":
            open_ = (t, words[1])
    return out


def _on_in(did, shape, spans):
    reads = [(p[shape.byte] >> shape.bit) & 1 for t, p in zip(did.times, did.payloads)
             if any(a <= t <= b for a, b in spans)]
    return (sum(reads) / len(reads), len(reads)) if reads else (None, 0)


def prompts(ctx):
    """A bit on in each window of one prompt and off in the others'"""
    spans = windows(ctx.drive.markers)
    for key, mine in spans.items():
        if not ctx.open(key):
            continue
        others = [w for k, ws in spans.items() if k != key for w in ws]
        best = None
        for did in ctx.drive.dids:
            for s in ctx.free_bits(did):
                if ctx.bit_claimed(did, s):
                    continue
                on, n = _on_in(did, s, mine)
                off, _ = _on_in(did, s, others)
                if on is None or on < PROMPT_ON or (off is not None and off > PROMPT_OFF):
                    continue
                score = on - (off or 0.0)
                if best is None or score > best[0]:
                    best = (score, did, s, on, n, off)
        if best:
            score, did, s, on, n, off = best
            ctx.claim(did, s, key, "solved", "prompt",
                      f"on in {on:.0%} of {n} reads while prompted, "
                      f"{(off or 0):.0%} in the other prompts", score)


def _phi(bits, want):
    n11 = sum(1 for a, b in zip(bits, want) if a and b)
    n10 = sum(1 for a, b in zip(bits, want) if a and not b)
    n01 = sum(1 for a, b in zip(bits, want) if not a and b)
    n00 = len(bits) - n11 - n10 - n01
    d = math.sqrt((n11 + n10) * (n01 + n00) * (n11 + n01) * (n10 + n00))
    return 0.0 if d == 0 else (n11 * n00 - n10 * n01) / d


def _best_bit(ctx, key, want_at, floor, words):
    if not ctx.open(key):
        return
    best = None
    for did in ctx.drive.dids:
        want = want_at(did)
        if want is None:
            continue
        for s in ctx.free_bits(did):
            if ctx.bit_claimed(did, s):
                continue
            phi = _phi(column(did, s), want)
            if phi >= floor and (best is None or phi > best[0]):
                best = (phi, did, s)
    if best:
        phi, did, s = best
        ctx.claim(did, s, key, "likely", "derived", f"{words} (phi {phi:.2f})", phi)


def _step_on(times, values_, t):
    k = max(0, min(len(times) - 1, sum(1 for x in times if x <= t) - 1))
    return values_[k]


def lock(ctx):
    slip = ctx.named("converter_slip")
    if slip is None:
        return
    t, v = ctx.series(slip)
    _best_bit(ctx, "converter_lock",
              lambda did: [abs(_step_on(t, v, x)) < 50 for x in did.times] if did.did == slip.did
              else None, 0.6, "on while the slip is near zero")


def brake(ctx):
    speed = ctx.drive.reference("obd2.speed")
    if speed is None:
        return

    def want(did):
        a, b = onto(speed, [t - 0.6 for t in did.times]), onto(speed, [t + 0.6 for t in did.times])
        return [(y - x) / 1.2 < -2.0 for x, y in zip(a, b)]

    _best_bit(ctx, "brake_switch", want, 0.4, "on while the road speed falls faster than a coast")


def clutch(ctx):
    gear, cmd = ctx.named("gear"), ctx.named("gear_commanded")
    rpm = ctx.drive.reference("obd2.rpm")
    if gear is None or rpm is None:
        return
    gt, gv = ctx.series(gear)
    ct, cv = ctx.series(cmd) if cmd else (gt, gv)

    def want(did):
        r = onto(rpm, did.times)
        return [x > 300 and (_step_on(gt, gv, t) == 0 or _step_on(gt, gv, t) != _step_on(ct, cv, t))
                for t, x in zip(did.times, r)]

    _best_bit(ctx, "clutch_switch", want, 0.3, "on in neutral and while the gears change")


def _segment(did, free):
    """The free live bytes cut into fields: the widest word that reads
    smoother than its first byte, else single bytes"""
    out = []
    free = sorted(free)
    while free:
        start = free[0]
        best = None
        for n in (4, 3, 2):
            if all(start + k in free for k in range(n)):
                for order in ("be", "le"):
                    for signed in (False, True):
                        s = Shape(start, 0, n * 8, order, signed)
                        r = roughness(column(did, s))
                        if best is None or r < best[0]:
                            best = (r, s)
        single = Shape(start, 0, 8, "be", False)
        r1 = roughness(column(did, single))
        chosen = best[1] if best is not None and best[0] < min(r1, NOISE_ROUGH) else single
        out.append(chosen)
        n = max(1, chosen.bits // 8)
        free = [x for x in free if x >= start + n]
    return out


def leftovers(drive, classes, used):
    """Every byte not named: static and decoy bytes by class, live fields
    unnamed, noise a decoy, bits that move unnamed"""
    out = []
    for did in drive.dids:
        cls = classes[(did.rx, did.did)]
        free = [b for b in range(len(did.payloads[0])) if (did.rx, did.did, b) not in used]
        live = []
        for b in free:
            kind = cls.get(b)
            if kind is None:
                live.append(b)
                continue
            words = f"static, 0x{did.payloads[0][b]:02X}" if kind == "static" else kind
            out.append(Found(did.rx, did.did, Shape(b, 0, 8, "be", False), None, kind, "", 1.0,
                             0.0, "static" if kind == "static" else "decoy", "class", words, 1.0))
        bitwise = [b for b in live if _bitwise(did, b, used)]
        for b in bitwise:
            for k in moving_bits(did, b):
                if (did.rx, did.did, b, k) not in used:
                    out.append(Found(did.rx, did.did, Shape(b, k, 1, "le", False), None, "bit",
                                     "", 1.0, 0.0, "unnamed", "class", "a bit that moves", 0.0))
        for s in _segment(did, [b for b in live if b not in bitwise]):
            r = roughness(column(did, s))
            noise = r >= NOISE_ROUGH
            out.append(Found(did.rx, did.did, s, None, "noise" if noise else "live", "", 1.0, 0.0,
                             "decoy" if noise else "unnamed", "class",
                             f"noise (roughness {r:.2f})" if noise else f"live, roughness {r:.3f}",
                             0.0))
        used.update((did.rx, did.did, b) for b in free)
    return out


def _bitwise(did, b, used):
    col = {p[b] for p in did.payloads}
    return len(col) <= 2 or any((did.rx, did.did, b, k) in used for k in range(8))
