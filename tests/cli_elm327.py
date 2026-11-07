# File/layer: tests/cli_elm327.py | test-only, Python 3.8+
# Testing: HARNESS: CliElm327RunTest (ctest)
#
#   python tests/cli_elm327.py adapter <havne-cli>
#   python tests/cli_elm327.py session <havne-cli>
#   python tests/cli_elm327.py modules <havne-cli>
#   python tests/cli_elm327.py plugin <havne-cli> <plugins/example-sim>
#   python tests/cli_elm327.py python-obd <havne-cli>
#
# havne-cli sim's ELM327 emulator (ADR-0055) on a port the system picks
# on 127.0.0.1, every desktop platform.
# adapter: a host speaks ELM327 to it over a socket: the reset banner,
# echo, the automatic search, headers on and off, the count digit, the
# adapter's wait; a second host is refused while one is connected, and
# the next connection starts from the power-on settings.
# session: core's own elm327:// session against it reads the simulated
# car: the VIN, the mock's three stored codes, the engine speed's values.
# modules: modules=2's second module answers a uds-generic DID through it
# (D101, the gear, 03).
# plugin: the example plugin's DIDs (sim --plugin), on both its ECUs, read
# by a session through the same plugin: each answers raw 0, its offset.
# python-obd: python-OBD reads it (skip 77 without the package).

import json
import socket
import subprocess
import sys
import time

SKIP = 77


class Emulator:
    """havne-cli sim <uri> --elm327 tcp:0 as a child, its port read from
    its elm327 line"""

    def __init__(self, cli, uri, seconds=60, extra=()):
        self.p = subprocess.Popen(
            [cli, "sim", uri, "--elm327", "tcp:0", "--seconds", str(seconds)] +
            list(extra), stdout=subprocess.PIPE, text=True)
        self.lines = []
        for line in self.p.stdout:
            j = json.loads(line)
            self.lines.append(j)
            if j["type"] == "elm327":
                host, port = j["listen"].rsplit(":", 1)
                assert host == "127.0.0.1", j
                self.port = int(port)
                return
            if j["type"] == "error":
                raise AssertionError("the emulator didn't start: %r" % j)
        raise AssertionError("no elm327 line: %r" % self.lines)

    def end(self, wait=False):
        """Its lines after the elm327 one: at its seconds' end (its end
        line), or cut short now"""
        if not wait:
            self.p.terminate()
        out, _ = self.p.communicate(timeout=60)
        self.lines += [json.loads(l) for l in out.splitlines() if l.strip()]
        return self.lines


class Host:
    """A host on the emulator's port, a command at a time"""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)

    def send(self, cmd):
        """The answer up to the prompt, and how long it took (s)"""
        t = time.monotonic()
        self.s.sendall(cmd.encode() + b"\r")
        buf = b""
        while not buf.endswith(b">"):
            got = self.s.recv(4096)
            if not got:
                raise AssertionError("closed after %r: %r" % (cmd, buf))
            buf += got
        return buf.decode(), time.monotonic() - t

    def close(self):
        self.s.close()


def expect(cond, what):
    if not cond:
        raise AssertionError(what)


def adapter(cli):
    emu = Emulator(cli, "mock://emu?values=canned", seconds=6)
    h = Host(emu.port)
    out, _ = h.send("ATZ")
    expect(out == "ATZ\r\r\rELM327 v1.4b\r\r>", "the reset's banner: %r" % out)
    out, _ = h.send("AT E0")
    expect(out == "AT E0\rOK\r\r>", "echo until it's off: %r" % out)
    for cmd in ("ATL0", "ATSP0"):
        expect(h.send(cmd)[0] == "OK\r\r>", cmd)
    out, took = h.send("0100")
    expect(out.startswith("SEARCHING...\r41 00 "), "the search, once: %r" % out)
    expect(took >= 0.18, "the adapter waits its timeout (0x32) for more: %.3f s"
           % took)
    expect(h.send("ATDPN")[0] == "A6\r\r>", "the search found protocol 6")
    out, _ = h.send("010C")
    expect(out == "41 0C 1F 40\r\r>", "the canned 2000 rpm: %r" % out)
    h.send("ATH1")
    out, took = h.send("09021")
    expect(out == "7E8 10 14 49 02 01 5A 5A 5A\r\r>",
           "the count digit ends at its first line: %r" % out)
    expect(took < 0.15, "and doesn't wait: %.3f s" % took)
    out, _ = h.send("0902")
    expect(out.count("\r7E8 2") == 2, "the VIN's Consecutive Frames: %r" % out)
    out, _ = h.send("ATST19")
    out, took = h.send("2201")
    expect(out == "NO DATA\r\r>", "a functional refusal is never sent: %r" % out)
    expect(0.09 <= took < 0.5, "NO DATA after AT ST's 100 ms: %.3f s" % took)
    expect(h.send("STI")[0] == "?\r\r>", "a plain ELM327")

    # a second host waits for no one: it's closed at once
    second = socket.create_connection(("127.0.0.1", emu.port), timeout=5)
    expect(second.recv(16) == b"", "a second host is closed")
    second.close()
    h.close()
    time.sleep(0.3)
    h = Host(emu.port)
    out, _ = h.send("ATI")
    expect(out == "ATI\rELM327 v1.4b\r\r>", "a new link is a power on: %r" % out)
    h.close()
    lines = emu.end(wait=True)
    events = [l["event"] for l in lines if l["type"] == "client"]
    expect(events[:4] == ["connected", "refused", "closed", "connected"],
           "client lines: %r" % events)
    end = [l for l in lines if l["type"] == "end"]
    expect(len(end) == 1 and end[0]["reason"] == "seconds" and
           end[0]["requests"] == 5 and end[0]["replies"] == 4,
           "the sim's counts: five requests, the functional 22 01 refused "
           "unsent: %r" % end)


def stream(cli, uri, extra):
    run = subprocess.run([cli, "stream", uri] + extra, capture_output=True,
                         text=True, timeout=120)
    lines = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
    expect(run.returncode == 0, "stream exit %d: %s %r" %
           (run.returncode, run.stderr, lines[-3:]))
    return lines


def session(cli):
    emu = Emulator(cli, "mock://emu")
    uri = "elm327://127.0.0.1:%d" % emu.port
    lines = stream(cli, uri, ["--values", "--count", "60"])
    s = [l for l in lines if l["type"] == "session"]
    expect(s and s[0]["backend"] == "elm327" and s[0]["firmware"] == "ELM327 v1.4b",
           "session: %r" % s)
    ident = [l for l in lines if l["type"] == "identity"]
    expect(ident and ident[0]["vin"] is not None and
           ident[0]["vin"]["vin"] == "ZZZFAKE11M0000001", "identity: %r" % ident)
    diag = [l for l in lines if l["type"] == "diagnostics"]
    expect(diag and diag[0]["stored"]["codes"] == ["P0301", "P0420", "U0100"],
           "diagnostics: %r" % diag)
    names = {}
    for l in lines:
        if l["type"] == "signals":
            names = {d["id"]: d["name"] for d in l["signals"]}
    rpm = [v["value"] for l in lines if l["type"] == "values"
           for v in l["values"] if names.get(v["id"]) == "RPM"]
    expect(rpm, "engine speed values: %r" % lines[-5:])
    end = [l for l in lines if l["type"] == "end"]
    expect(end and end[0]["isotp"]["dropped"] == 0 and end[0]["isotp"]["malformed"] == 0,
           "end: %r" % end)
    emu.end()


def modules(cli):
    emu = Emulator(cli, "mock://emu?modules=2")
    uri = "elm327://127.0.0.1:%d?profile=uds-generic&did=7E9:D101" % emu.port
    lines = stream(cli, uri, ["--values", "--seconds", "3"])
    texts = [v.get("text") for l in lines if l["type"] == "values"
             for v in l["values"] if l["id"] == "0x7E9"]
    expect("03" in texts, "the module's D101, 03: %r" % lines[-5:])
    emu.end()


def plugin(cli, package):
    profile = "plugin.example-sim.base"
    emu = Emulator(cli, "mock://emu?profile=" + profile,
                   extra=["--plugin", package])
    uri = "elm327://127.0.0.1:%d?profile=%s&identify=0&diagnose=0" % (
        emu.port, profile)
    lines = stream(cli, uri, ["--values", "--seconds", "4", "--plugin", package])
    names = {}
    for l in lines:
        if l["type"] == "signals":
            names = {d["id"]: d["name"] for d in l["signals"]}
    got = {}
    for l in lines:
        if l["type"] == "values":
            for v in l["values"]:
                got[(l["id"], names.get(v["id"]))] = v.get("value")
    expect(got.get(("0x7E8", "Oil Temp")) == -40, "pcm's F40D: %r" % got)
    expect(got.get(("0x7E9", "Trans Fluid Temp")) == -40, "tcm's F410: %r" % got)
    emu.end()


def python_obd(cli):
    try:
        import obd  # noqa: F401  (python-OBD, a test-only package)
    except ImportError:
        print("python-OBD isn't installed: skipped")
        return SKIP
    emu = Emulator(cli, "mock://emu?values=canned")
    c = obd.OBD("socket://127.0.0.1:%d" % emu.port, fast=False, timeout=5)
    try:
        expect(c.status() == obd.OBDStatus.CAR_CONNECTED, "status %r" % c.status())
        expect(c.protocol_id() == "6", "protocol %r" % c.protocol_id())
        rpm = c.query(obd.commands.RPM)
        expect(not rpm.is_null() and abs(rpm.value.magnitude - 2000) < 1,
               "rpm %r" % rpm.value)
        codes = c.query(obd.commands.GET_DTC)
        expect([code for code, _ in codes.value] == ["P0301", "P0420", "U0100"],
               "codes %r" % codes.value)
    finally:
        c.close()
        emu.end()
    return 0


def main():
    mode, cli = sys.argv[1], sys.argv[2]
    if mode == "plugin":
        rc = plugin(cli, sys.argv[3])
    else:
        rc = {"adapter": adapter, "session": session, "modules": modules,
              "python-obd": python_obd}[mode](cli)
    print("ok" if not rc else "skipped")
    return rc or 0


if __name__ == "__main__":
    sys.exit(main())
