from collections import namedtuple

Found = namedtuple("Found", "rx did shape key name unit scale offset status method evidence score")
Found.__doc__ = ("A field discovery named or classed: shape a shapes.Shape; key the catalog's "
                 "or None; status 'solved', 'likely', 'related', 'unnamed', 'decoy' or "
                 "'static'; method what found it; evidence its words; score its R squared "
                 "or confidence")

RANK = {"solved": 0, "likely": 1, "related": 2, "unnamed": 3, "decoy": 4, "static": 5}


def value(found, raw):
    return raw * found.scale + found.offset


def decoded(found, did):
    from .shapes import column
    return [value(found, r) for r in column(did, found.shape)]
