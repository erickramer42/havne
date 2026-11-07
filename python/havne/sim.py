# File/layer: python/havne/sim.py | Python 3.11+, the havne package
# Testing: PythonTest.test_sim, test_vcan
#
# The simulated ECU (ADR-0046, sim.h): unbound, asked one request at a
# time with no bus on any platform (ecuio_sim_ask, ADR-0055), or on a
# Linux virtual CAN bus answering a session as the engine ECU.

import ctypes
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, utf8

Reply = namedtuple("Reply", "id data delay_us flags")
Reply.__doc__ = ("One ECU's reply: the CAN ID it's sent on, the whole message from its "
                 "service byte, when it starts after the request, ECUIO_SIM_REPLY_*")


class Sim:
    """The simulated ECU on uri (mock://..., replay://...). With
    interface, on that Linux virtual CAN bus (Sim("mock://sim",
    interface="vcan0")); Sim.unbound(uri) takes no bus. A context
    manager."""

    def __init__(self, uri, interface=None, *, _unbound=False):
        self._sim = ctypes.POINTER(e.ecuio_sim_t)()
        if _unbound:
            check(lib.ecuio_sim_create_unbound(utf8(uri), 0, ctypes.byref(self._sim)),
                  "ecuio_sim_create_unbound")
        else:
            if interface is None:
                raise ValueError("a sim on a bus needs its interface; Sim.unbound takes none")
            check(lib.ecuio_sim_create(utf8(uri), utf8(interface), ctypes.byref(self._sim)),
                  "ecuio_sim_create")

    @classmethod
    def unbound(cls, uri="mock://sim"):
        return cls(uri, _unbound=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def ask(self, request, to=0x7E0, at_us=None):
        """Every ECU's reply to one whole request on CAN ID to, at at_us
        microseconds since create (None: the sim's own clock)"""
        data = bytes(request)
        # Always a buffer, so core judges the length itself (an empty
        # request is its INVALID_ARGUMENT, not a NULL)
        req = (ctypes.c_uint8 * max(1, len(data))).from_buffer_copy(data.ljust(1, b"\0"))
        out = (e.ecuio_sim_reply_t * e.ECUIO_SIM_MAX_REPLIES)()
        count = ctypes.c_uint32()
        check(lib.ecuio_sim_ask(self._sim, to, req, len(data),
                                e.ECUIO_SIM_NOW if at_us is None else at_us,
                                out, e.ECUIO_SIM_MAX_REPLIES, ctypes.byref(count)),
              "ecuio_sim_ask")
        return [Reply(r.reply_id, ctypes.string_at(r.data, r.length), r.delay_us, r.flags)
                for r in out[:count.value]]

    @property
    def stats(self):
        out = e.ecuio_sim_stats_t()
        check(lib.ecuio_sim_get_stats(self._sim, ctypes.byref(out)), "ecuio_sim_get_stats")
        return out

    def close(self):
        if self._sim is not None:
            lib.ecuio_sim_destroy(self._sim)
            self._sim = None
