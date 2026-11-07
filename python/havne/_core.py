# File/layer: python/havne/_core.py | Python 3.11+, the havne package
# Testing: every PythonTest
#
# The loaded library and the one way the package turns a status into an
# exception: right after the call, on the calling thread, so
# ecuio_last_error_detail (thread-local, like dlerror) is the call's own.

from ._lib import lib
from . import _ecuio
from .errors import Error


def check(status, call):
    """status if it's ECUIO_OK; else an Error carrying core's words and detail"""
    if status == _ecuio.ECUIO_OK:
        return status
    detail = lib.ecuio_last_error_detail()
    words = lib.ecuio_status_string(status)
    raise Error(status, words.decode("utf-8", "replace") if words else str(status),
                detail.decode("utf-8", "replace") if detail else "", call)


def text(raw):
    """A C string as str: UTF-8, a bad byte as U+FFFD; None as ""."""
    if raw is None:
        return ""
    if isinstance(raw, (bytes, bytearray)):
        return bytes(raw).split(b"\0", 1)[0].decode("utf-8", "replace")
    return str(raw)


def utf8(value):
    return value.encode("utf-8") if isinstance(value, str) else value


def with_profile(uri, profile):
    """uri with ?profile= naming profile; a different one already there is
    refused"""
    query = uri.partition("?")[2]
    for pair in query.split("&") if query else []:
        key, _, value = pair.partition("=")
        if key == "profile":
            if value != profile:
                raise ValueError(f"the URI names profile {value!r}, not {profile!r}")
            return uri
    return f"{uri}{'&' if query else '?'}profile={profile}"
