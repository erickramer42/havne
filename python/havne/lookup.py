# File/layer: python/havne/lookup.py | Python 3.11+, the havne package
# Testing: PythonTest.test_lookup, against havne-cli vin and dtc
#
# The two offline lookups havne-cli has: a VIN's decode (vin.h, ADR-0043)
# and a trouble code's title (core.h, ADR-0044). Typed text is taken as the
# CLI and the app take it: spaces dropped, upper case.

import datetime
import ctypes
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, text, utf8

VinInfo = namedtuple("VinInfo", "vin decoded model_year year_earlier make wmi check_digit "
                                "check_matches source")
VinInfo.__doc__ = ("A VIN's decode, the fields of havne-cli vin's line: model_year None when "
                   "position 10 is no year code, make None when the vPIC release lists none")
DtcTitle = namedtuple("DtcTitle", "code title source")
DtcTitle.__doc__ = ("A trouble code as core writes it, its title (None when the table has "
                    "none) and where the titles come from")


def _typed(value):
    return value.replace(" ", "").upper()


def decode_vin(vin, latest_model_year=None):
    """The decode of a typed VIN. latest_model_year bounds the year the cycle
    can mean (default: this year + 1, as the CLI and the app pass it). Text
    that is not a VIN (49 CFR 565: 17 capital letters and digits, never I, O
    or Q) is a ValueError"""
    typed = _typed(vin)
    if latest_model_year is None:
        latest_model_year = datetime.datetime.now(datetime.timezone.utc).year + 1
    info = e.ecuio_vin_info_t()
    if lib.ecuio_vin_decode(utf8(typed), latest_model_year, ctypes.byref(info)) != e.ECUIO_OK:
        raise ValueError("not a VIN: 49 CFR 565 takes 17 digits and capital letters, "
                         "never I, O or Q")
    wmi = text(info.wmi)
    make = text(lib.ecuio_vin_make(utf8(wmi)))
    return VinInfo(typed, True, info.model_year or None,
                   bool(info.flags & e.ECUIO_VIN_YEAR_EARLIER), make or None, wmi,
                   text(info.check_digit), bool(info.flags & e.ECUIO_VIN_CHECK_MATCHES),
                   text(lib.ecuio_vin_source()))


def dtc_title(code):
    """A typed trouble code's title: a letter P, C, B or U, then four hex
    digits, the first 0-3 (a ValueError otherwise)"""
    value = ctypes.c_uint16()
    if lib.ecuio_dtc_parse(utf8(_typed(code)), ctypes.byref(value)) != e.ECUIO_OK:
        raise ValueError("not a trouble code: a letter P, C, B or U, then four hex "
                         "digits, the first 0-3")
    buf = ctypes.create_string_buffer(8)
    check(lib.ecuio_dtc_format(value.value, buf, len(buf)), "ecuio_dtc_format")
    return DtcTitle(text(buf.raw), text(lib.ecuio_dtc_title(value.value)) or None,
                    text(lib.ecuio_dtc_title_source()))
