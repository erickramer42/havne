# File/layer: python/havne/sim.py | Python 3.11+, the havne package
# Testing: PythonTest.test_sim, test_vcan
#
# The simulated ECU (ADR-0046, sim.h): unbound, asked one request at a
# time with no bus on any platform (ecuio_sim_ask, ADR-0055), or on a
# Linux virtual CAN bus answering a session as the engine ECU; the
# simulated gateway's routes, lock and counts (gateway= in the URI).

import ctypes
import weakref
from collections import namedtuple

from . import _ecuio as e
from ._core import check, lib, utf8

Reply = namedtuple("Reply", "id data delay_us flags fc_us cf_us")
Reply.__doc__ = ("One ECU's reply: the CAN ID it's sent on, the whole message from its "
                 "service byte, when it starts after the request, ECUIO_SIM_REPLY_*, "
                 "the tester's Flow Control to its first Consecutive Frame (fc=) and "
                 "its least gap between them (cf=), in microseconds")

CLASSES = ("reads", "session", "write", "prog", "other")
ROUTES = {e.ECUIO_SIM_ROUTE_FORWARD: "forward", e.ECUIO_SIM_ROUTE_REFUSE: "refuse",
          e.ECUIO_SIM_ROUTE_SILENT: "silent", e.ECUIO_SIM_ROUTE_GATED: "gated",
          e.ECUIO_SIM_ROUTE_GATED_SILENT: "gated-silent"}
_COUNTS = ("forwarded", "refused", "dropped", "seeds", "keys", "wrong_keys", "lockouts",
           "unlocks", "relocks", "too_early")

Gateway = namedtuple("Gateway", ("request_id reply_id extended unlocked delayed never "
                                 "unlocked_level wrong delayed_until_us routes route_nrc now "
                                 "overrides ").split() + list(_COUNTS))
Gateway.__doc__ = ("The simulated gateway as of the last request it routed or answered: "
                   "its IDs, its session and lock, each class's route as configured "
                   "(routes, route_nrc: 0 for the default code) and as it stands by the "
                   "lock (now), its entries for a service or a module (overrides), and "
                   "its counts since create")
Override = namedtuple("Override", "reply_id service cls route nrc now")
Override.__doc__ = ("A route= entry: the module's reply ID (0 for the whole gateway), "
                    "the service byte or the class's name (the other None), its route "
                    "and code as configured, and its route now")


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
        # Left unclosed, destroyed when collected or at the interpreter's
        # exit, so its worker leaves the bus
        self._finalizer = weakref.finalize(self, _destroy, self._sim)

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
        return [Reply(r.reply_id, ctypes.string_at(r.data, r.length), r.delay_us, r.flags,
                      r.fc_us, r.cf_us)
                for r in out[:count.value]]

    def set_value(self, signal_id, value):
        check(lib.ecuio_sim_set_value(self._sim, signal_id, float(value)), "ecuio_sim_set_value")

    @property
    def stats(self):
        out = e.ecuio_sim_stats_t()
        check(lib.ecuio_sim_get_stats(self._sim, ctypes.byref(out)), "ecuio_sim_get_stats")
        return out

    @property
    def gateway(self):
        """The gateway's view (ecuio_sim_get_gateway and its overrides);
        havne.Error INVALID_ARGUMENT with no gateway= in the URI"""
        g = e.ecuio_sim_gateway_t()
        check(lib.ecuio_sim_get_gateway(self._sim, ctypes.byref(g)), "ecuio_sim_get_gateway")
        overrides = []
        for i in range(g.overrides):
            o = e.ecuio_sim_gateway_override_t()
            check(lib.ecuio_sim_get_gateway_override(self._sim, i, ctypes.byref(o)),
                  "ecuio_sim_get_gateway_override")
            overrides.append(Override(o.reply_id, o.key if o.service else None,
                                      None if o.service else CLASSES[o.key],
                                      ROUTES[o.route], o.nrc, ROUTES[o.now]))
        flag = lambda f: (g.flags & f) != 0
        return Gateway(g.request_id, g.reply_id, flag(e.ECUIO_SIM_GATEWAY_EXTENDED),
                       flag(e.ECUIO_SIM_GATEWAY_UNLOCKED), flag(e.ECUIO_SIM_GATEWAY_DELAYED),
                       flag(e.ECUIO_SIM_GATEWAY_NEVER), g.unlocked_level, g.wrong,
                       g.delayed_until_us,
                       {c: ROUTES[g.route[i]] for i, c in enumerate(CLASSES)},
                       {c: g.route_nrc[i] for i, c in enumerate(CLASSES)},
                       {c: ROUTES[g.now[i]] for i, c in enumerate(CLASSES)},
                       overrides, *(getattr(g, c) for c in _COUNTS))

    def close(self):
        if self._sim is not None:
            self._sim = None
            self._finalizer()


def _destroy(sim):
    lib.ecuio_sim_destroy(sim)
