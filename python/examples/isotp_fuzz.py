# File/layer: python/examples/isotp_fuzz.py | Python 3.11+, Linux, an example of the havne package
# Testing: HARNESS, PythonTest.test_isotp_fuzz (the sim in-process) and, in a
#          sanitizer tree, ExampleTest.isotp_fuzz_cli (an ASan havne-cli sim)
#
# Malformed ISO-TP (ISO 15765-2) exchanges against the simulated ECU on a
# Linux virtual CAN bus, from a seeded generator, so a case that fails
# replays by its seed and index. core's fuzz_isotp already feeds core's
# reassembly bytes in memory; what a bus adds is time and state: the
# receiver's N_Cr wait, the sender's N_Bs wait for a Flow Control, the
# tester's Flow Control while the sim sends a segmented reply, a new
# request in the middle of a message, CAN FD lengths where the bus takes
# them. After each case a probe asks for the engine speed (01 0C on
# 0x7E0) and the sim must answer it, and must never report its bus
# failed. Standard library only for the bus (socket.AF_CAN, CAN_RAW).
#
# vcan only: an interface whose netlink kind isn't vcan is refused, with
# no override, by the bus itself, so fuzz() called from a script of its
# own refuses too. Never point a fuzzer at a car.
#
#   python isotp_fuzz.py --interface vcan0 [--cases 200] [--seed 1]
#   python isotp_fuzz.py --interface vcan0 --seed 1 --case 37   # one case again
#   python isotp_fuzz.py --interface vcan0 --ecu-running        # an ECU already up
#   python isotp_fuzz.py --interface vcan0 --ecu-cli <havne-cli>
#       (havne-cli sim mock://fuzz?values=canned --on vcan0 as the ECU, which
#       must then end cleanly: exit 0 and its end line not failed, so a
#       sanitizer's report in a sanitizer build fails the run)
#
# Exit 0 when every case passed, 1 when one failed, 2 when it can't run.

import argparse
import json
import os
import random
import selectors
import signal
import socket
import struct
import subprocess
import sys
import time

TESTER, FUNCTIONAL, ECU = 0x7E0, 0x7DF, 0x7E8
CLASSIC = struct.Struct("=IB3x8s")    # struct can_frame
FD = struct.Struct("=IBB2x64s")       # struct canfd_frame
CAN_RAW_FD_FRAMES = 5
FD_LENGTHS = (12, 16, 20, 24, 32, 48, 64)
PROBE_WAIT_S = 1.5   # past the sim's N_Cr and N_Bs (1 s each), so a held message ends first
PROBE_TRIES = 2
DEAD_STOP = 3        # probes unanswered in a row: the ECU is gone, and each more case waits out its probe
SIM_START_S = 30     # havne-cli's sim line, and its end after SIGINT


def refuse_unless_vcan(ifname):
    """None when ifname is a vcan interface, else why not"""
    try:
        out = subprocess.run(["ip", "-details", "-json", "link", "show", "dev", ifname],
                             capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"can't ask the kernel what {ifname} is ({e})"
    if out.returncode != 0:
        return f"no interface {ifname}"
    try:
        kind = json.loads(out.stdout)[0].get("linkinfo", {}).get("info_kind")
    except (ValueError, IndexError, AttributeError):
        kind = None
    if kind != "vcan":
        return f"{ifname} is {kind or 'not a virtual CAN bus'}: the fuzzer runs on vcan only"
    return None


class Bus:
    """A CAN_RAW socket on the interface, hearing the ECU's replies only;
    ValueError for an interface that isn't vcan"""

    def __init__(self, ifname):
        why = refuse_unless_vcan(ifname)
        if why:
            raise ValueError(why)
        self.s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        with open(f"/sys/class/net/{ifname}/mtu") as f:
            self.fd = int(f.read()) >= 72
        if self.fd:
            self.s.setsockopt(socket.SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 1)
        self.s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER,
                          struct.pack("=II", ECU, 0x7FF))
        self.s.bind((ifname,))

    def send(self, can_id, data):
        if len(data) > 8:
            self.s.send(FD.pack(can_id, len(data), 0, bytes(data).ljust(64, b"\0")))
        else:
            self.s.send(CLASSIC.pack(can_id, len(data), bytes(data).ljust(8, b"\0")))

    def recv(self, timeout):
        """One frame's data from the ECU, or None after timeout seconds"""
        self.s.settimeout(max(timeout, 0.001))
        try:
            raw = self.s.recv(FD.size)
        except socket.timeout:
            return None
        if len(raw) == FD.size:
            _, n, _, data = FD.unpack(raw)
        else:
            _, n, data = CLASSIC.unpack(raw)
        return data[:n]

    def drain(self):
        while self.recv(0.001) is not None:
            pass

    def close(self):
        self.s.close()


def pad(data, n=8, byte=0xAA):
    return bytes(data) + bytes([byte]) * (n - len(data))


# --- the cases: each a list of steps ---------------------------------------
# ("send", id, bytes)   a frame
# ("sleep", seconds)
# ("ff", seconds)       wait for the ECU's First Frame (a segmented reply)

def sf_bad_length(r, bus):
    n = r.choice([0, 8, 9, 15])   # 0 and anything past 7 are no Single Frame
    return [("send", TESTER, pad([n] + [r.randrange(256) for _ in range(7)]))]


def sf_truncated(r, bus):
    n = r.randrange(2, 8)
    dlc = r.randrange(1, n + 1)   # the frame shorter than its length says
    return [("send", TESTER, bytes([n, 0x01, 0x0C] + [0xAA] * 5)[:dlc])]


def ff_too_short(r, bus):
    n = r.randrange(0, 8)         # a First Frame for what one frame holds
    steps = [("send", TESTER, bytes([0x10, n, 0x22, 0xF1, 0x90, 0, 0, 0]))]
    if r.random() < 0.5:
        steps.append(("send", TESTER, pad([0x21] + [0xAA] * 7)))
    return steps


def ff_abandoned(r, bus):
    n = r.choice([8, 20, 4095])
    first = [0x10 | (n >> 8), n & 0xFF, 0x22] + [r.randrange(256) for _ in range(5)]
    steps = [("send", TESTER, bytes(first))]
    for sn in range(1, r.randrange(1, 4)):
        steps.append(("send", TESTER, pad([0x20 | sn] + [r.randrange(256) for _ in range(7)])))
    return steps   # never finished: the sim's N_Cr ends it


def ff_escape(r, bus):
    n = r.choice([0, 1, 4096, 0xFFFFFFFF])   # the 32-bit length form
    return [("send", TESTER, bytes([0x10, 0x00]) + n.to_bytes(4, "big") + bytes([0x22, 0xF1]))]


def cf_without_ff(r, bus):
    return [("send", TESTER, pad([0x20 | r.randrange(16)] + [r.randrange(256) for _ in range(7)]))
            for _ in range(r.randrange(1, 4))]


def cf_out_of_order(r, bus):
    # A 10-byte multi-DID read: First Frame then one Consecutive Frame
    msg = [0x22, 0xF1, 0x90, 0xF1, 0x91, 0xF1, 0x92, 0xF1, 0x93, 0xF1]
    steps = [("send", TESTER, bytes([0x10, len(msg)] + msg[:6]))]
    sn = r.choice([0, 2, 3, 15])   # never the 1 it must be
    steps.append(("send", TESTER, pad([0x20 | sn] + msg[6:])))
    if r.random() < 0.5:
        steps.append(("send", TESTER, pad([0x21] + msg[6:])))   # the right one, late
    return steps


def bad_pci(r, bus):
    return [("send", TESTER, pad([r.randrange(0x40, 0x100)] + [r.randrange(256) for _ in range(7)]))]


def empty_frame(r, bus):
    return [("send", r.choice([TESTER, FUNCTIONAL]), b"")]


def fc_abuse(r, bus):
    # The VIN (09 02) comes back segmented: the tester's Flow Control then
    # says anything: wait, overflow, a reserved status, odd block sizes and
    # STmin, twice, or never
    steps = [("send", TESTER, pad([0x02, 0x09, 0x02])), ("ff", 1.0)]
    what = r.choice(["status", "wait", "twice", "none", "new-request", "functional"])
    if what == "status":
        status = r.choice([0, 2, 3, 7, 15])
        bs = r.choice([0, 1, 255])
        st = r.choice([0, 0x7F, 0x80, 0xF1, 0xF9, 0xFA, 0xFF])
        steps.append(("send", TESTER, pad([0x30 | status, bs, st])))
    elif what == "wait":
        for _ in range(r.randrange(1, 4)):
            steps += [("send", TESTER, pad([0x31, 0, 0])), ("sleep", 0.3)]
    elif what == "twice":
        steps += [("send", TESTER, pad([0x30, 0, 0])), ("send", TESTER, pad([0x30, 0, 0]))]
    elif what == "new-request":
        steps.append(("send", TESTER, pad([0x02, 0x01, 0x0D])))
    elif what == "functional":
        steps.append(("send", FUNCTIONAL, pad([0x02, 0x01, 0x00])))
    return steps   # "none": no Flow Control, the sim's N_Bs ends it


def ff_functional(r, bus):
    # Functional addressing takes Single Frames only
    return [("send", FUNCTIONAL, bytes([0x10, 0x0A, 0x22, 0xF1, 0x90, 0xF1, 0x91, 0xF1]))]


def fd_lengths(r, bus):
    if not bus.fd:
        return sf_bad_length(r, bus)
    n = r.choice(FD_LENGTHS)
    claim = r.choice([0, 1, 7, n - 2, n - 1, n, 0xFF])
    body = [0x00, claim & 0xFF, 0x01, 0x0C] + [0xAA] * (n - 4)   # the escape form
    return [("send", TESTER, bytes(body[:n]))]


FAMILIES = [sf_bad_length, sf_truncated, ff_too_short, ff_abandoned, ff_escape,
            cf_without_ff, cf_out_of_order, bad_pci, empty_frame, fc_abuse,
            ff_functional, fd_lengths]


def case(seed, index, bus):
    r = random.Random(seed * 1_000_003 + index)
    family = FAMILIES[index % len(FAMILIES)]
    return family.__name__, family(r, bus)


def run_steps(bus, steps):
    for step in steps:
        if step[0] == "send":
            bus.send(step[1], step[2])
        elif step[0] == "sleep":
            time.sleep(step[1])
        elif step[0] == "ff":
            until = time.monotonic() + step[1]
            while time.monotonic() < until:
                data = bus.recv(until - time.monotonic())
                if data and data[0] >> 4 == 1:
                    break


def probe(bus):
    """Whether the ECU answers 01 0C, once a held message has ended"""
    for _ in range(PROBE_TRIES):
        bus.drain()
        bus.send(TESTER, pad([0x02, 0x01, 0x0C]))
        until = time.monotonic() + PROBE_WAIT_S
        while time.monotonic() < until:
            data = bus.recv(until - time.monotonic())
            if data and len(data) >= 3 and data[0] >> 4 == 0 and data[1:3] == b"\x41\x0c":
                return True
    return False


def shown(steps):
    return " ".join(f"{s[1]:X}#{s[2].hex().upper()}" if s[0] == "send" else
                    (f"wait({s[1]:g}s)" if s[0] == "sleep" else "<ECU's FF>") for s in steps)


def fuzz(ifname, cases=200, seed=1, only=None, sim=None, out=sys.stdout):
    """The failed cases' indexes"""
    failed = _fuzz(ifname, cases, seed, only, sim, out)
    if sim is not None:
        st = sim.stats
        print(f"isotp_fuzz: the sim took {st.requests} requests, sent {st.replies} replies "
              f"({plural(st.refused, 'refusal')}), {st.unflowed} segmented replies unflowed", file=out)
    return failed


def _fuzz(ifname, cases, seed, only, sim, out):
    """fuzz's cases, with their probes"""
    bus = Bus(ifname)
    failed = []
    try:
        if not probe(bus):
            print(f"isotp_fuzz: no ECU answers 01 0C on {ifname}", file=out)
            return [-1]
        indexes = [only] if only is not None else range(cases)
        ran, dead = 0, 0
        for k in indexes:
            name, steps = case(seed, k, bus)
            run_steps(bus, steps)
            ran += 1
            alive = probe(bus)
            yielded = sim is not None and sim.stats.flags & sim_flag("YIELDED")
            broke = sim is not None and sim.stats.flags & sim_flag("FAILED")
            dead = 0 if alive else dead + 1
            if not alive or broke or yielded:
                failed.append(k)
                why = ("another ECU answers on the bus, so the sim went silent" if yielded else
                       "the sim's bus failed" if broke else "no answer to 01 0C after it")
                print(f"FAIL  case {k} (seed {seed}, {name}): {why}: {shown(steps)}",
                      file=out, flush=True)
            if yielded:
                print("isotp_fuzz: the fuzzer needs the bus to itself: the run stops "
                      f"after case {k}", file=out, flush=True)
                break
            if dead >= DEAD_STOP:
                print(f"isotp_fuzz: the ECU stopped answering ({DEAD_STOP} probes in a row): "
                      f"the run stops after case {k}", file=out, flush=True)
                break
        print(f"isotp_fuzz: {plural(ran, 'case')}, {len(failed)} failed "
              f"(seed {seed}, {'CAN FD' if bus.fd else 'classic CAN'})", file=out, flush=True)
    finally:
        bus.close()
    return failed


def plural(n, word):
    return f"{n} {word}{'' if n == 1 else 's'}"


def sim_flag(name):
    """ECUIO_SIM_<name>, from the havne package (imported only with the
    sim in this process: --ecu-cli and --ecu-running need no package)"""
    import havne
    return getattr(havne.c, f"ECUIO_SIM_{name}")


def with_cli_sim(cli, ifname, run):
    """run() against havne-cli's sim on ifname; 1 also when the sim
    doesn't end cleanly"""
    p = subprocess.Popen([cli, "sim", "mock://fuzz?values=canned", "--on", ifname],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    early, rc = b"", None
    try:
        started, early = sim_line(p)
        if started is True:
            rc = run()
        else:
            print(f"isotp_fuzz: havne-cli's sim didn't start: {started}", file=sys.stderr)
    finally:
        p.send_signal(signal.SIGINT)
        try:
            rest, err = p.communicate(timeout=SIM_START_S)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            rest, err = None, None
    if rc is None:
        return 2
    if rest is None:
        print(f"FAIL  havne-cli's sim didn't end within {SIM_START_S} s of SIGINT")
        return 1
    rest = (early + rest).decode("utf-8", "replace").strip()
    err = err.decode("utf-8", "replace")
    end = json.loads(rest.splitlines()[-1]) if rest else {}
    print(f"isotp_fuzz: havne-cli's sim took {end.get('requests')} requests, "
          f"sent {end.get('replies')} replies, exit {p.returncode}")
    if end.get("yielded"):
        print(f"FAIL  another ECU answers on {ifname}, so havne-cli's sim went silent: "
              f"the fuzzer needs the bus to itself")
        return 1
    if p.returncode != 0 or end.get("type") != "end" or end.get("failed"):
        print(f"FAIL  havne-cli's sim didn't end cleanly (exit {p.returncode}): {end}")
        print(err)
        return 1
    return rc


def sim_line(p):
    """(True once havne-cli's sim line arrives within SIM_START_S, else
    why not; the bytes read past it). Raw reads, so nothing waits in a
    buffer the select can't see."""
    until = time.monotonic() + SIM_START_S
    got = b""
    with selectors.DefaultSelector() as sel:
        sel.register(p.stdout, selectors.EVENT_READ)
        while (left := until - time.monotonic()) > 0:
            if not sel.select(left):
                break
            chunk = os.read(p.stdout.fileno(), 4096)
            if not chunk:
                return "it ended before its sim line", got
            got += chunk
            while b"\n" in got:
                line, got = got.split(b"\n", 1)
                kind = json.loads(line).get("type")
                if kind == "sim":
                    return True, got
                if kind in ("error", "end"):
                    return line.decode("utf-8", "replace"), got
    return f"no sim line within {SIM_START_S} s", got


def main(argv=None):
    p = argparse.ArgumentParser(description="ISO-TP fuzzing of the simulated ECU, vcan only")
    p.add_argument("--interface", default="vcan0")
    p.add_argument("--cases", type=int, default=200)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--case", type=int, help="run this one case of the seed")
    ecu = p.add_mutually_exclusive_group()
    ecu.add_argument("--ecu-running", action="store_true",
                     help="an ECU already answers on the bus: start no sim")
    ecu.add_argument("--ecu-cli", metavar="HAVNE_CLI",
                     help="run havne-cli's sim as the ECU, and fail unless it ends cleanly")
    args = p.parse_args(argv)
    if not hasattr(socket, "AF_CAN") or not sys.platform.startswith("linux"):
        print("isotp_fuzz: Linux's CAN sockets only", file=sys.stderr)
        return 2
    why = refuse_unless_vcan(args.interface)
    if why:
        print(f"isotp_fuzz: {why}", file=sys.stderr)
        return 2
    if args.ecu_running:
        return 1 if fuzz(args.interface, args.cases, args.seed, args.case) else 0
    if args.ecu_cli:
        return with_cli_sim(args.ecu_cli, args.interface,
                            lambda: 1 if fuzz(args.interface, args.cases, args.seed, args.case) else 0)
    import havne
    with havne.Sim("mock://fuzz?values=canned", interface=args.interface) as sim:
        return 1 if fuzz(args.interface, args.cases, args.seed, args.case, sim) else 0


if __name__ == "__main__":
    sys.exit(main())
