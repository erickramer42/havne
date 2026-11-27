import json
import os
import re

from .found import RANK

NAMED = ("solved", "likely", "related")


def _field(f):
    s = f.shape
    out = {"module": f.rx, "did": f.did, "byte": s.byte, "bits": s.bits}
    if s.bits == 1:
        out["bit"] = s.bit
    elif s.bits > 8:
        out["order"] = s.order
    out["signed"] = s.signed
    if f.status in NAMED:
        out.update({"key": f.key, "name": f.name, "unit": f.unit, "scale": f.scale,
                    "offset": f.offset})
    out.update({"status": f.status, "method": f.method, "evidence": f.evidence,
                "score": round(f.score, 4)})
    return out


def _where(f):
    s = f.shape
    if s.bits == 1:
        return f"{f.rx} {f.did} byte {s.byte} bit {s.bit}"
    n = max(1, s.bits // 8)
    where = f"byte {s.byte}" if n == 1 else f"bytes {s.byte}-{s.byte + n - 1}"
    kind = f"{'s' if s.signed else 'u'}{s.bits}" + (f" {s.order.upper()}" if n > 1 else "")
    return f"{f.rx} {f.did} {where} {kind}"


def _number(x):
    return f"{x:.10g}"


class Report:
    """What discovery found: each field by status, the static DIDs in
    one line each, the references too slow to fit, and the wanted signals
    no field showed"""

    def __init__(self, drive, found, notes, orders, signals, classes):
        self.drive = drive
        self.found = found
        self.notes = notes
        self.orders = orders
        self.signals = signals
        self.classes = classes

    def by_status(self, status):
        return [f for f in self.found if f.status == status]

    def unseen(self):
        named = {f.key for f in self.found if f.status in NAMED}
        return [s["key"] for s in self.signals if s["key"] not in named]

    def static_dids(self):
        """Each DID's static bytes in one entry: all of a mostly static DID's"""
        out = []
        for did in self.drive.dids:
            fixed = sorted(f.shape.byte for f in self.found
                           if f.rx == did.rx and f.did == did.did and f.status == "static")
            if fixed:
                out.append({"module": did.rx, "did": did.did, "length": len(did.payloads[0]),
                            "bytes": fixed,
                            "values": " ".join(f"{did.payloads[0][b]:02X}" for b in fixed)})
        return out

    def to_dict(self):
        fields = [f for f in self.found if f.status != "static"]
        fields.sort(key=lambda f: (RANK[f.status], f.rx, f.did, f.shape.byte, f.shape.bit))
        out = {"discovery_report": 1,
               "recording": {"utc_us": self.drive.utc_us, "dids": len(self.drive.dids),
                             "references": len(self.drive.references)},
               "oracle": None if self.drive.oracle_shift_s is None else
               {"channels": [r.name for r in self.drive.oracle],
                "shift_s": round(self.drive.oracle_shift_s, 3)},
               "byte_orders": self.orders,
               "references_too_slow": [{"reference": k, "words": v}
                                       for k, v in sorted(self.notes.items())]}
        for status in ("solved", "likely", "related", "unnamed", "decoy"):
            out[status] = [_field(f) for f in fields if f.status == status]
        out["static"] = self.static_dids()
        out["unseen"] = self.unseen()
        return out

    def text(self):
        lines = []
        for status in ("solved", "likely", "related"):
            for f in sorted(self.by_status(status), key=lambda f: (f.rx, f.did, f.shape.byte)):
                lines.append(f"{status:8} {_where(f)} x{_number(f.scale)} {_number(f.offset):>+}"
                             f" {f.unit}: {f.name} ({f.evidence})")
        for f in self.by_status("unnamed"):
            lines.append(f"unnamed  {_where(f)}: {f.evidence}")
        for f in self.by_status("decoy"):
            lines.append(f"decoy    {_where(f)}: {f.evidence}")
        for d in self.static_dids():
            whole = len(d["bytes"]) == d["length"]
            lines.append(f"static   {d['module']} {d['did']}: {len(d['bytes'])} of {d['length']} "
                         f"bytes{' (the whole DID)' if whole else ''}, {d['values']}")
        for k, v in sorted(self.notes.items()):
            lines.append(f"slow     {k}: {v}")
        if self.unseen():
            lines.append("unseen   " + ", ".join(self.unseen()))
        return "\n".join(lines) + "\n"

    def signal_file(self, name="Discovered"):
        """The solved fields as a signal file (havne_signals 1), each with
        how it was found in x-discovery"""
        solved = sorted(self.by_status("solved"), key=lambda f: (f.rx, f.did, f.shape.byte))
        ecus = sorted({f.rx for f in solved})
        signals, keys = [], set()
        for f in solved:
            key = re.sub(r"[^a-z0-9_]", "_", f"{f.did.lower()}_{f.key}")
            while key in keys:
                key += "_2"
            keys.add(key)
            field = {"byte": f.shape.byte}
            if f.shape.bits == 1:
                field.update({"bit": f.shape.bit, "bits": 1, "order": "le"})
            else:
                field.update({"bits": f.shape.bits, "order": f.shape.order})
            if f.shape.signed:
                field["signed"] = True
            sig = {"key": key, "name": f.name[:31], "ecu": f"m{f.rx.lower()}",
                   "read": {"service": "22", "number": f.did}, "field": field,
                   "scale": f.scale, "offset": f.offset}
            if f.unit:
                sig["unit"] = f.unit
            sig["x-discovery"] = {"method": f.method, "evidence": f.evidence,
                                  "score": round(f.score, 4), "catalog": f.key}
            signals.append(sig)
        return {"havne_signals": 1, "name": name,
                "source": {"name": "Found by havne.discover from a recording"},
                "ecus": [{"id": f"m{rx.lower()}", "rx": rx} for rx in ecus],
                "signals": signals}

    def write(self, out):
        """report.json, report.txt and signals.json into the folder out"""
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "report.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.to_dict(), f, indent=1, ensure_ascii=False)
            f.write("\n")
        with open(os.path.join(out, "report.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(self.text())
        with open(os.path.join(out, "signals.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.signal_file(), f, indent=1, ensure_ascii=False)
            f.write("\n")
