# File/layer: python/havne/_asks.py | Python 3.11+, the havne package
# Testing: PythonTest.test_import, PythonTest.test_export,
#          PythonTest.test_health, PythonTest.test_recording_aids
#
# The stop core asks about during a long call (ecuio_cancel_t, and the
# reader's open progress, ecuio_open_progress_t): the caller's cancel, an
# exception it raises, or a Ctrl+C on the main thread, each raised again
# once core has removed what it had begun. One run() holds the SIGINT
# handler over every core call its work makes, so a call that opens a
# reader first and then writes stops the same way in either.

import signal
import threading

from . import _ecuio as e


class _Asks:
    """The cancel core asks during a long call: the caller's cancel, an
    exception it raises, or a Ctrl+C on the main thread, each raised again
    once core has removed what it had begun"""

    def __init__(self, cancel):
        if cancel is not None and not callable(cancel):
            raise TypeError("cancel is a callable taking no arguments, or None")
        self._cancel = cancel
        self.failed = None
        self.interrupted = False
        self.callback = e.ecuio_cancel_t(self._ask)
        # The reader's open asks the same question, as its progress
        self.progress = e.ecuio_open_progress_t(self._progress)

    def _ask(self, _user):
        # An exception escaping a ctypes callback would leave its return
        # value unset: it stops the call and is raised after it
        try:
            if self.interrupted or self.failed is not None:
                return 1
            return 1 if self._cancel is not None and self._cancel() else 0
        except BaseException as exc:  # noqa: BLE001 (raised again by run)
            self.failed = exc
            return 1

    def _progress(self, _read, _total, user):
        return self._ask(user)

    def _on_sigint(self, _signum, _frame):
        self.interrupted = True

    def run(self, work):
        """work(callback)'s status, or what stopped it raised. work may
        make several core calls, passing callback to each (and progress to
        a reader's open); the handler stays swapped across all of them"""
        # Python's own handler would raise KeyboardInterrupt on the
        # callback's entry, outside any try: a flag stands in for it
        swapped = (threading.current_thread() is threading.main_thread()
                   and signal.getsignal(signal.SIGINT) is signal.default_int_handler)
        if swapped:
            signal.signal(signal.SIGINT, self._on_sigint)
        try:
            status = work(self.callback)
        finally:
            if swapped:
                signal.signal(signal.SIGINT, signal.default_int_handler)
        if self.failed is not None:
            raise self.failed
        if self.interrupted:
            raise KeyboardInterrupt
        return status
