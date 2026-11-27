"""Discovery: which fields of a car's own DIDs are which signals, from a
recording, the wanted list and, where one is given, an oracle's CSV"""

from . import catalog as _catalog
from . import direct, recognize, series
from .found import RANK, Found
from .report import Report
from .shapes import byte_classes, moving_bits

__all__ = ["solve", "Report", "Found"]


def _free(drive, classes):
    return {(d.rx, d.did): {b for b in range(len(d.payloads[0])) if b not in classes[(d.rx, d.did)]}
            for d in drive.dids}


def solve(folder, oracle=None, catalog=None, packs=None):
    """The Report on a recording's folder: oracle a logger's CSV whose
    channels name signals of the wanted list (with an engine speed channel
    to put its clock on the drive's), catalog a wanted list's path"""
    doc = _catalog.load(catalog)
    signals = _catalog.signals(doc, packs)
    by_key = {s["key"]: s for s in _catalog.signals(doc)}
    drive = series.read(folder)
    if oracle is not None:
        series.add_oracle(drive, oracle)
    classes = {(d.rx, d.did): byte_classes(d) for d in drive.dids}
    notes = {}
    proposals = direct.propose(drive, _free(drive, classes), signals, notes)
    used = set()
    found, prior = direct.assign(drive, proposals, by_key, used)
    found += recognize.run(drive, found, by_key, signals, classes, used)
    found += direct.related(drive, proposals, by_key, used)
    found += recognize.leftovers(drive, classes, used)
    found.sort(key=lambda f: (f.rx, f.did, f.shape.byte, f.shape.bit))
    return Report(drive, found, notes, prior, signals, classes)
