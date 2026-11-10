# File/layer: python/havne/errors.py | Python 3.11+, the havne package
# Testing: PythonTest.test_session, test_version, test_export
#
# What the package raises: core's refusals as Error, carrying the status's
# number, its name (ECUIO_ERR_BAD_URI is "BAD_URI") and core's detail text,
# a failed scrub check as CheckFailed (an Error naming the class), and the
# two ways the library itself can fail to load.

from . import _ecuio

_NAMES = {getattr(_ecuio, n): n[len("ECUIO_ERR_"):]
          for n in dir(_ecuio) if n.startswith("ECUIO_ERR_")}
_NAMES[_ecuio.ECUIO_OK] = "OK"


def status_name(status):
    """A status's name, "BAD_URI" for ECUIO_ERR_BAD_URI; its number when
    the package doesn't know it (a newer core's)"""
    return _NAMES.get(status, str(status))


class Error(Exception):
    """A call core refused. status is core's ecuio_status_t, name its
    name, words core's text for it (ecuio_status_string) and detail what
    ecuio_last_error_detail said, "" when it said nothing."""

    def __init__(self, status, words, detail="", call=""):
        self.status = status
        self.name = status_name(status)
        self.words = words
        self.detail = detail
        self.call = call
        text = f"{call}: {words} ({self.name})" if call else f"{words} ({self.name})"
        super().__init__(f"{text}: {detail}" if detail else text)


class CheckFailed(Error):
    """An export or report whose scrub check found a class of what it
    leaves out: nothing was written. failed_class is the class's word
    ("identification", "unattributable", "text_values", "activity" or
    "vin_shaped"; "unknown" for a class a newer core adds), never the
    value found."""

    def __init__(self, status, words, detail="", call="", failed_class="unknown"):
        super().__init__(status, words, detail, call)
        self.failed_class = failed_class


class LibraryNotFound(ImportError):
    """No core library where the package looks for one"""


class LibraryVersionError(ImportError):
    """A core library of another version than the package's"""
