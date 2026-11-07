# ADR-0055: The ELM327 emulator: `havne-cli sim <uri> --elm327 tcp:<port>` serves the simulated car as an ELM327 adapter on every platform, through an unbound sim in core and the adapter's text in havne-cli

- **Status:** Accepted. Built.
- **Date:** 2026-11-06.
- **Builds on:** [ADR-0046](0046-simulated-ecu.md) (the simulated ECU,
  its sources, faults, modules and a profile's DIDs);
  [ADR-0026](0026-elm327-transport.md) and its appendix "WiFi over TCP"
  (an ELM327 adapter on a TCP socket); [ADR-0031](0031-command-line-client.md)
  (havne-cli); [ADR-0052](0052-plugin-framework.md) (a plugin's
  variants as profiles).

## Context

The simulated ECU answers only on Linux's vcan (ADR-0046): off Linux
`ecuio_sim_create` is `ECUIO_ERR_NO_TRANSPORT`. So on Windows and
macOS nothing outside the test binaries answers a request, and ELM327
tools (python-OBD, phone apps that talk to a WiFi adapter, Havne's own
`elm327://`) have no car to read without hardware. The test fake,
`tests/fake_elm327.h`, models the adapter well, but it is test code:
it answers from `tests/fake_ecu.h`, includes `internal.h` and carries
knobs for the transport's tests.

havne-cli includes only the public headers (ADR-0031): what it does,
an outside program can do. sim.h offers no way to ask the simulated
ECU anything except through a bus.

## Decision

### 1. An unbound sim in core

`ecuio_sim_create_unbound(uri, options, &out)` (`sim.h`) makes the same
simulated car with no bus, on every platform: the whole URI grammar
(`mock://` with `values`, `modules=2`, `profile=`, `vin` and `faults`;
`replay://` with `loop`, `from`, `to`, `vin` and `faults`) and the same
refusals in the same order (BAD_URI, UNKNOWN_PROFILE, INVALID_ARGUMENT,
LOAD_FAILED). It never opens a socket or reaches a bus; a caller that
puts its replies somewhere chooses where. No option is defined yet.
Stats, `ecuio_sim_set_value` and `ecuio_sim_destroy` work on it as on
a sim on a bus; its flags stay 0.

`ecuio_sim_ask(sim, request_id, request, length, time_us, replies,
capacity, &count)` takes one whole request on `request_id` as a tester
sends it. Every ECU that hears that ID (its own request ID, or 0x7DF)
answers as on a bus, faults and suppressed functional refusals
included, each reply in an `ecuio_sim_reply_t` (4136 bytes: `reply_id`,
`flags`, `delay_us`, `length`, `reserved[5]`, `data[4096]`; static
asserts and `test_c_abi.c`) in the ECUs' order, by serial-ports'
contract (`*count` every reply, the first `capacity` copied, at most
`ECUIO_SIM_MAX_REPLIES`, 10). An ID no ECU hears gets no reply and
isn't counted. A sim on a bus refuses ask (INVALID_ARGUMENT): its own
worker answers there, and two callers would race over the ECU's state.

**The caller's clock.** `time_us` is the ECU's time when the request
comes, microseconds since create on the caller's clock, or
`ECUIO_SIM_NOW` (-1) for the sim's own steady clock. Ask ticks the ECU
(the drive cycle, a recording's time) up to it, and a time before the
last ask's is taken as the last's. A test or a script then gets the
same answers at the same times on every run. A step of more than an
hour moves the ECU's time but steps the drive cycle through the last
hour of it alone, so a time from the wrong clock can't stall the call.

**A fault is a reply's time and a flag.** A sim on a bus breaks a reply
on the wire; an unbound one hands the reply over whole and says what
the bus would have carried: `late` starts 130 ms after the request
(`ECUIO_SIM_REPLY_LATE`), `pending` is a 7F <service> 78 at once
(`ECUIO_SIM_REPLY_PENDING`) and the reply 300 ms later in a second
entry, `bad_sequence` and `stalled` flag a segmented reply
(`ECUIO_SIM_REPLY_BAD_SEQUENCE`: its last Consecutive Frame's number
skips one; `ECUIO_SIM_REPLY_STALLED`: its last Consecutive Frame comes
160 ms after the one before), `silent` is no entry. The counts are the
bus's: a broken reply isn't whole, the 78 isn't the reply. One caller
at a time; stats and `set_value` from any thread meanwhile.

The answering is the bus's code: `sim.cpp`'s `answer` (the profile's
DIDs, the engine ECU, `modules=2`'s module) and `decide` (the fault and
its count) serve both, and `build_sim` makes both kinds of sim, so the
car can't differ between them.

### 2. The adapter's text in havne-cli

`cli/elm327_emulator.h` is an ELM327 v1.4b as its datasheet describes
it and `tests/fake_elm327.h` models it, in front of an unbound sim,
public headers only. Pure: bytes in at a time, bytes out each due at a
time. It keeps the AT settings (echo, linefeeds, spaces, headers, the
DLC digit, responses, the protocol, the header `AT SH`, the receive
filter `AT CRA` or `AT CF`/`AT CM`, the timeout `AT ST`), answers
`ATZ`/`ATWS` with its banner, `ATI`, `AT@1`, `ATRV` (13.8V), `ATDP`,
`ATDPN`, the settings the setup commands of core and python-OBD send
with OK, and anything else `?`; ST commands `?` (a plain ELM327). A CR
alone repeats the last command, and any byte stops `AT MA` (which hears
nothing: no other node is on the car's bus), as a real adapter does.

A request is hex, at most seven bytes, with an optional response count
digit; it goes to `ecuio_sim_ask` on `AT SH`'s 11-bit ID (0x7DF by
default) at the emulator's time, and each reply is framed as an ISO
15765 ECU sends it (a Single Frame, or a First Frame and Consecutive
Frames, padded with zeros as the vcan sim pads) and printed as the
settings say: with headers the ID, the DLC digit and eight bytes; with
none the data alone, a long message as its length and numbered lines.
The adapter waits `AT ST`'s timeout (4.096 ms a count, 0x32 by default)
for each next frame, P2* (5 s) after a 7F <service> 78, so a reply past
it is lost as an adapter loses it (a late reply under `AT ST 19`, a
stalled last frame); the count digit ends a request after that many
frame lines, at once. Nothing heard is `NO DATA` after the wait.

**The simulated car's bus** is 11-bit CAN at 500 kbaud: protocol 6, or
the automatic search (0, `SPAh`), which finds 6 and prints SEARCHING...
once (`ATDPN` A6 after it). A 29-bit protocol (7, 9) hears no ECU (NO
DATA), 250 kbaud (8) is the wrong rate (CAN ERROR), and the protocols
before CAN don't connect (UNABLE TO CONNECT).

### 3. `havne-cli sim <uri> --elm327 tcp:[<address>:]<port>`

`cli/elm327_listener.h` serves the emulator on a TCP port, as a WiFi
adapter does, with `cli/elm327_lines.h`'s address and lines.

- **127.0.0.1 unless the user names another address.** `tcp:<port>`
  listens on the loopback alone; `tcp:<a.b.c.d>:<port>` (four decimal
  octets, `0.0.0.0` for every interface) is the user's choice of a
  listener the LAN can reach, for a phone app on the same network.
  Port 0 takes one the system picks; the `elm327` line names the one
  taken.
- **One host at a time,** as an adapter takes one: another connection
  is closed at once. Each connection is a power on: the settings start
  from their defaults, so one host's `ATE0` never reaches the next.
- **Lines** (schema 1, new types): `start`, `elm327` {uri, listen},
  `client` {event connected|closed|refused, peer} as hosts come and
  go, `sim_stats` every 10 s while the counts change, `end` {reason
  interrupted|seconds, the counts}, as `sim --on` writes them. A port
  that won't listen is an `error` line (LOAD_FAILED, the address in the
  detail) and exit 3; a URI core refuses is the bus's `error` line.
- `--signals` and `--plugin` give the profile as for `--on`, so a
  plugin's DIDs answer through the emulator; `--desk-bus` is for `--on`
  alone (usage). `--on` and `--elm327` together are usage.

A pty on Linux and macOS (for tools that take only a serial port) is
left for later; it would serve the same emulator.

### 4. Tested

`SimAskTest` (test_obd2: answers against `sim_ecu.h`'s, the contracts,
the caller's clock, each fault's time and flag, modules and a profile's
DIDs, a recording at the caller's time); `CliElm327Test` (test_cli: the
text at given times: settings, the search and protocols, headers,
spaces and the DLC digit, the count digit, the header and the filter
picking the ECU, the faults as an adapter sees them, responses off, the
monitor, the address grammar and lines); `CliElm327RunTest`
(`tests/cli_elm327.py`, every desktop platform: a host's ELM327
exchange and a second host refused, core's own `elm327://` session
reading the VIN, the codes and the engine speed, a second module's DID,
the example plugin's DIDs on both its ECUs, and python-OBD when it is
installed); `CliGoldenTest.SimElm327RefusesABadUri` (the bus's golden:
the same refusal); `CliUsageTest.SimElm327*`.

## Alternatives considered

- **A cli header over sim.h alone.** Impossible: sim.h answers only on
  a bus, which Windows and macOS lack.
- **The adapter's text in core** (`ecuio_elm327_emulator_*`). More C
  ABI for one caller: the app connects to an adapter, it never serves
  one, and the Python bindings (ADR-0053) can call `ecuio_sim_ask`
  directly for a DID scan. The text stays in havne-cli, where a change
  to it is no ABI change.
- **`tests/fake_elm327.h` itself.** It answers from the tests' ECU,
  includes `internal.h` and holds the transport tests' knobs; serving
  it would put test code in the product and the tests' ECU in front of
  users instead of the simulated car.
- **The simulated ECU's bus stepped by a thread** in the unbound sim.
  The caller's clock gives the same answers on every run and needs no
  thread; `ECUIO_SIM_NOW` covers a caller with no clock of its own.

## Consequences

- python-OBD, phone apps that take a WiFi adapter's address and
  Havne's own `elm327://127.0.0.1:<port>` read a simulated car on
  Windows, macOS and Linux with no hardware: the no-car demo is the
  release's own app against `havne-cli sim mock://demo --elm327
  tcp:35000`.
- The C ABI grows by two functions, a struct and their defines,
  append-only.
- A request through the emulator takes the adapter's wait unless it
  carries a count digit, as on a real adapter: python-OBD's reads take
  about 200 ms each; core's (which sends the digit) don't.
- python-OBD 0.7.3 strips the characters `0`, `1`, `2`, `x` and `\`
  from both ends of a text item (its `decode_encoded_string`), so it
  reads the default VIN `ZZZFAKE11M0000001` as `ZZZFAKE11M`; core reads
  the whole VIN, and a `vin=` that ends otherwise reads whole in both.
- A listener on another address is reachable by anything on that
  network: the user names it, and it stays off by default.
