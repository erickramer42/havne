# ADR-0026: ELM327 transport — `elm327://` drives an ELM327-compatible adapter over a serial port, with the request engine

- **Status:** Proposed.
- **Date:** 2026-10-27.
- **Builds on:** [ADR-0007](0007-uri-selected-transports.md) (a
  transport per URI scheme, failures at create);
  [ADR-0013](0013-protocol-profiles.md) (the profile decides the
  requests and the decode); [ADR-0015](0015-request-engine.md) (what
  to ask and when, read-only by construction);
  [ADR-0018](0018-connect-failure-reporting.md) (a refused connect
  says why); [ADR-0019](0019-session-info.md) (the session says what
  it runs on).

## Context

QtTune reaches a vehicle only through J2534, and J2534 means Windows
and a vendor DLL. Off Windows the transports are `mock://` and
`replay://`, so the macOS, Linux and mobile builds can't read a car
(STATUS.md, Known Issue #2). The most common OBD adapters aren't
J2534 at all: they speak the ELM327's text command set over a serial
link, USB or Bluetooth. That includes the OBDLink family, whose STN
chips answer ELM327 commands. An OBDLink MX+, MX or LX pairs over
classic Bluetooth (the Serial Port Profile), and once it is paired
Windows, macOS and Linux each show it as a serial port: `COMn`,
`/dev/cu.*` and `/dev/rfcomm*`. A USB adapter is a serial port with no
pairing.

The core already holds everything an OBD session needs above the
wire: the profiles and their decode (ADR-0013), the request engine
that derives, paces and correlates requests and drops what the ECU
refuses (ADR-0015, `core/src/request_engine.h`, which does no I/O and
reads no clock), the supported-PID probe, the DID scan (ADR-0020) and
identification (ADR-0021). The J2534 worker is one driver of that
engine. What an ELM327 adds is a different wire: commands and replies
as ASCII lines, one command at a time, ended by a `>` prompt.

The ELM327 datasheet (Elm Electronics, ELM327DSH, describing ELM327
v1.4b) fixes most of the wire:
- Requests are hex text, and the adapter adds the ISO 15765 framing
  (CAN Auto Formatting, `AT CAF1`, the default). It sends the flow
  control after a First Frame by itself (`AT CFC1`, the default).
- With headers on (`AT H1`), each received CAN frame is printed as one
  line: the CAN ID, then the data bytes **including the PCI byte**
  ("If the headers are enabled, you will see it in the responses").
  Replies from two ECUs can interleave line by line (its `09 04`
  example from `7E8` and `7E9`), so a reader reassembles per CAN ID.
- From v1.3 on, a single hex digit after a request says how many
  responses to wait for. The adapter then returns as soon as it has
  them, rather than waiting out the `AT ST` timer (about 200 ms by
  default, with adaptive timing on) after the last one.
- The failure words are fixed: `NO DATA` (the timer ran out with no
  reply), `CAN ERROR` (the CAN system couldn't initialize, send or
  receive, "often … simply from not being connected to a CAN
  system"), `BUFFER FULL` (its 256-byte transmit buffer overflowed and
  data was lost), `STOPPED` (an operation interrupted by a received
  character), `?` (a command it didn't understand), plus `<DATA ERROR`,
  `<RX ERROR` and `ERRxx`.
- `AT MA` monitors the whole bus, silently (no acknowledgements), until
  any character arrives, and that character is discarded.
- `AT I` prints the version string, and `AT RV` reads the supply
  voltage at the connector.

## Decision

### 1. A transport in core that owns its serial port

`elm327://` is a fourth scheme, in core
(`core/src/elm327_transport.cpp`). Core opens the port itself: `CreateFileW` on `\\.\COMn` on
Windows, `open` and `termios` elsewhere, both behind one small
byte-stream interface (`elm327_port.h`: open, write, read with a
timeout, close). That keeps core Qt-free, and it covers USB and
classic Bluetooth on all three desktop systems. The OS turns a paired
SPP device into a port, so core never touches a Bluetooth stack.

Bluetooth LE adapters (the OBDLink CX, most cheap BLE dongles) and the
mobile apps have no serial port: their bytes come through the
platform's BLE or Bluetooth API, which in this project means the app
layer (QtBluetooth). This record does not build that. It keeps the
byte-stream interface as the seam: a later record gives the app a
C ABI to feed a stream into an `elm327` session (bytes in, bytes out),
and the protocol layer below doesn't change.

### 2. The URI

```
elm327://<port>[?profile=<name>][&did=…][&baud=<n>][&poll=…][&probe=0|1][&identify=0|1][&filter=<rx>[:<tx>]]
```

- **`<port>`** is the OS's name for the port: `COM5` (Windows),
  `/dev/cu.<name>` (macOS), `/dev/rfcomm0` or `/dev/ttyUSB0`
  (Linux). On Windows a bare `COMn` is opened as `\\.\COMn`, so ports
  past `COM9` work.
- **`profile`** and **`did`** are core's, as for every scheme. The
  default profile is `obd2`, as for `j2534://`.
- **`baud`**: the serial rate for a wired adapter, default 38400. The
  ELM327 starts at 9600 or 38400, chosen by a pin at power-up, and
  other chips choose their own. A Bluetooth SPP port ignores it.
- **`poll`**, **`probe`**, **`identify`** and **`filter`** mean what
  they mean for `j2534://`, so a run sheet's query carries over. The
  channels come from `?filter=` or the profile's declarations
  (`resolve_channels`, as for J2534, so `raw` hears the engine ECU).

Unknown keys and malformed values fail at create, before the port is
opened (`ecuio_elm327_query_keys`, rejection tests for each malformed
form). A port that can't be opened is `ECUIO_ERR_LOAD_FAILED` with the
OS's reason ("COM5: the port is in use by another program", "…: no
such port"). An adapter that doesn't answer the reset as an ELM327
does is `ECUIO_ERR_VENDOR_REFUSED`, with what it said. Neither detail
holds a path with an account name, since port names never do.

### 3. A pure protocol layer, then the worker

The protocol has its own header, `core/src/elm327_protocol.h`. Like
`request_engine.h` it does no I/O and reads no clock, so every rule is
a unit test over canned bytes.
- **Line assembly:** bytes in, whole lines out. CR ends a line, LF is
  ignored, and `>` ends a reply whether or not it follows a CR. Bytes
  arrive in any chunking: a line, or the prompt, split across reads,
  or several replies in one.
- **Line classification:** a CAN frame (`7E8064100BE3FA813` with spaces
  off: three hex digits of ID for 11-bit, eight for 29-bit, then the
  bytes), a failure word (section 6), an echo, `OK`, or something else,
  which is ignored and counted.
- **ISO-TP reassembly per CAN ID:** a Single Frame's length comes from
  its PCI byte; a First Frame's 12-bit length starts a message;
  Consecutive Frames must follow in sequence (`21` … `2F`, `20`, …).
  A gap, a duplicate or a length mismatch drops that ID's message and
  counts it. A complete message becomes one `ecuio_frame_t`, the same
  shape the J2534 transport delivers: `arbitration_id` from the line,
  data from the service byte on, `TRUNCATED` past
  `ECUIO_FRAME_DATA_MAX`.

**Setup at attach**, each command answered `OK` (or its value) before
the next:

| Command | Why |
|---|---|
| `AT Z` | a known state; its answer is the version string (`ELM327 v…`) |
| `AT E0`, `AT L0`, `AT S0` | no echo, no linefeeds, no spaces: fewer bytes per reply |
| `AT H1` | the CAN ID and the PCI byte on every line, so core reassembles and correlates |
| `AT SP 6` (11-bit) or `AT SP 7` (29-bit) | ISO 15765-4 CAN at 500 kbit/s, the protocol the channels need, never an automatic search |
| `AT SH <tx>` | the first channel's request ID |
| `AT CF`/`AT CM`, or `AT CRA <rx>` for one channel | receive only the channels' reply IDs |
| `AT AT1` | adaptive timing (the default, set explicitly) |
| `AT I`, `AT RV`, then `STI` | the session info (section 7) |

Any of these failing ends the connect with `VENDOR_REFUSED`, naming
the command and what came back.

**The worker** drives the request engine as the J2534 worker does,
with one difference: **one command is outstanding for the whole
adapter**, not one per channel, because the ELM327 takes the next
command only after its prompt. Each pass:
1. ends the waits that ran out;
2. asks the channels in turn for a request that is due;
3. sends `AT SH <tx>` if the channel's request ID isn't the one set;
4. sends the request as hex, plus the response-count digit `1` when
   one single-frame reply is certain (a Service 01 request to a
   physical ID; never for 09, 22, or a functional ID);
5. reads to the prompt, dispatches each reassembled frame through
   `ecuio_session_dispatch_callbacks`, and hands each matched frame to
   `ecuio_engine_on_frame`.

`NO DATA` completes the read and leaves the engine's wait to expire
the request, so a timeout is judged in one place. The engine's
constants (the 100 ms wait, the 20 ms floor) stand. The adapter's own
timer, about 200 ms and adaptive, bounds a read from above, and with
the count digit an answered request returns as soon as its reply is
in. The worker allocates nothing per frame, as `CoreTest.WorkersAllocateNothingPerFrame`
requires of every worker.

### 4. Passive listening is a later step

A session with no requests (`raw` with no `?poll=`, or a profile with
nothing to ask) would listen with `AT MA`. The ELM327's 256-byte
buffer and a serial link cannot carry a busy 500 kbit/s bus, and
`BUFFER FULL` means data was lost. This record defines the shape: the
monitor runs until a character stops it; `BUFFER FULL` is counted,
reported as frames lost, and doesn't change the link state; frames
are delivered with `?filter=` applied through `AT CF`/`AT CM`. It is
built after the request path has run on a car. Until then such a
session is refused at create, with a detail that says so.

### 5. Read-only, as every session is

Requests come only from the engine, and the engine refuses any
service but 0x01, 0x09 and 0x22 (ADR-0015 §3). The worker sends no
other OBD bytes. The AT commands it sends configure the adapter and
never reach the bus as diagnostic requests. No URI key sends raw text
to the adapter.

### 6. Link health

The J2534 transport's `link_health` rules (`update_health`,
`request_timed_out`, `begin_episode`, the backoff in
`j2534_backoff.h`) move to a header both workers include. The ELM327
transport adds its own mapping:

| What happens | State |
|---|---|
| A matched reply | `ECUIO_OK` |
| `NO DATA` while something is due; the quiet run as for J2534 | `ECUIO_ERR_VEHICLE_TIMEOUT` (quiet), no episode |
| `CAN ERROR`, `BUS ERROR`, `BUS BUSY`, `ERR94`: no ACK, or no CAN bus | `ECUIO_ERR_REQUEST_FAILED`, an episode, backoff |
| The port fails (read or write error, the Bluetooth link dropped), or no prompt within 5 s | `ECUIO_ERR_TRANSPORT_HARD_FAILURE`, an episode, backoff |
| `LV RESET`, or the version string mid-session (the adapter reset) | re-run the setup; hard failure until it succeeds |

A hard failure reopens the port on the backoff's schedule and re-runs
the setup, so a Bluetooth link that drops and returns recovers
without a reconnect, as a J2534 adapter's does. The bridge, the pages
and the activity log already show these states (`bridge.md`), so the
UI needs no new code.

### 7. Session info and discovery

- **Session info:** `backend` is `ECUIO_BACKEND_ELM327` (5, appended).
  The `firmware` version string is the `AT I` answer, plus `STI`'s
  answer when the adapter has one (an STN chip, which an OBDLink is).
  `vbatt_mv` comes from `AT RV`. `protocol` and `baud` are the CAN
  protocol and rate. The bridge's `sessionInfoText` gets an ELM327
  form, for example "Session: ELM327 on COM5 (`<AT I answer>`,
  `<STI answer>`); ISO 15765 at 500000 baud; profile obd2, channel
  7E8/7E0; battery 12.6 V". The bench fills in what an OBDLink
  actually answers.
- **Discovery:** Settings lists the machine's serial ports beside the
  J2534 interfaces (`ecuio_serial_ports`, a new call: the
  `SERIALCOMM` registry key on Windows, `/dev/cu.*` on macOS,
  `/dev/ttyUSB*`, `/dev/ttyACM*` and `/dev/rfcomm*` on Linux). Nothing
  is probed. Opening a port that isn't an adapter could disturb
  whatever is on it, so the user picks the port, as with a J2534
  interface.

### 8. Verification

- **UNIT** (`test_elm327`, Qt-free, in the Ubuntu CI job): the
  protocol layer over canned transcripts, chunked every way. That
  covers single and multi-frame replies, two ECUs interleaved, every
  failure word, a reply split at the prompt, a sequence gap, and
  `7F 22 31` and `7F 22 78` reaching the engine.
- **HARNESS:** a fake ELM327 behind the byte-stream interface
  (`tests/fake_elm327.h`), answering from the same canned replies the
  fake J2534 DLL uses (`g_obd2_canned`, its DIDs and identification
  items), with knobs for `NO DATA`, `CAN ERROR`, a dropped link and a
  slow reply. The session tests run on every platform through a
  test-only seam that hands attach the fake instead of a port. That
  covers the probe, the rotation, the scan, identification, link
  health and recovery, the pending path, and nothing allocated per
  frame.
- **BENCH**, with the OBDLink on the dev machine and no car (BENCH
  prep items added to `docs/bench-sessions/bench-prep.md`):
  - the setup's answers;
  - `AT RV`;
  - what `CAN ERROR` looks like on an adapter with no bus;
  - a Bluetooth drop and its recovery;
  - the time from a request to its prompt with nothing to answer.
- **Vehicle:** a read-only session on the same car as the J2534
  sessions, under `obd2`. Its values are compared against the VX
  sessions' recordings and against the cluster, and its request rate
  is measured.

### 9. Order of work

1. Move link health and its backoff into a shared header (no behavior
   change; the J2534 health tests pin it).
2. The protocol layer and its unit tests.
3. The transport: the port on Windows and POSIX, the worker, session
   info, the URI and its rejection tests, the fake and the HARNESS
   tests.
4. The app: serial ports in Settings' device list, an `elm327://`
   preset, the ELM327 form of the session line.
5. BENCH, then the vehicle session.
6. Later, each with its own record or appendix: passive listening
   (`AT MA`, or the STN's `STMA`), and the app-fed stream for BLE and
   the mobile apps.

## Open questions for the bench

- **Response pending:** the datasheet doesn't describe what the
  adapter does after `7F xx 78`. If it returns to the prompt after the
  pending line (with the count digit, it counts as a response), the
  final reply arrives while the adapter isn't listening and is lost.
  The engine then waits out its pending wait (5.05 s) and asks again.
  The probable mitigation is never to send the count digit with 0x22
  and 0x09, and to raise `AT ST` for them. Neither has been seen
  happening yet: the CX-5 never sent a `78` to the J2534 sessions.
- **STN commands:** that the OBDLink answers `AT I` as an ELM327
  version and `STI` with its own, and what the STN family adds that is
  worth using (its monitor, its faster timing). This is read from the
  STN reference manual and checked on the adapter before any of it is
  used.
- **Rate:** how many requests a second the MX+ sustains over
  Bluetooth against the car, compared with the VX's 26 a second on the
  same car (docs/bench-sessions/2026-10-25-vehicle.md).

## Alternatives considered

- **The app layer owns every port (QtSerialPort, QtBluetooth) and
  feeds core bytes.** This was the plan the backlog named first. It
  would put the protocol in core and the I/O in the app, but the
  session would then depend on the app to run at all, so the Qt-free
  tests would need a fake there too and the `bench_bridge`-style
  harnesses would need Qt. Classic Bluetooth and USB need no Qt to
  reach, so core owns them, and the app feeds only what core can't
  open (BLE, mobile). The seam is the same either way.
- **Treat the ELM327 as a J2534 device through a shim DLL.** Third
  parties have built such shims. But the surrogate's contract is the
  J2534 API, and an ELM327 is a text protocol with one command
  outstanding. A shim hides that, and the request engine then paces
  for a device it isn't driving. It would also stay Windows-only, which
  is the problem this record exists to solve.
- **Headers off, letting the ELM327 reassemble (`AT H0`, CAF on).** It
  prints a multi-frame reply as `014` / `0: …` / `1: …`, which is
  easier to read and fewer bytes. But it hides which ECU answered.
  The engine correlates replies by channel, and `uds-generic`'s
  per-ECU requests depend on that. The datasheet's own example shows
  two ECUs' lines mixed until the headers are turned on.
- **Probe the serial ports for an ELM327 at discovery.** That is
  convenient, but `AT Z` sent to an unknown device on a port is a
  write to something that may not want it. The user picks the port.

## Consequences

- QtTune can read a car on macOS and Linux, and on Windows without a
  J2534 adapter. The adapters it opens are the common, cheap ones.
- One request outstanding at a time caps the rate below the J2534
  path. Each request's time includes the serial or Bluetooth round
  trip, which the bench will measure. The response-count digit
  recovers most of the adapter's waiting for Service 01.
- A second worker drives the request engine, which keeps the engine
  free of transport assumptions; any assumption the J2534 worker made
  for it shows up in the second worker's tests.
- `link_health` moves out of the J2534 transport, a refactor with no
  behavior change that the existing health tests guard.
- Clone adapters claim versions they don't implement (a common
  complaint about cheap "v1.5" and "v2.1" clones). The setup checks
  every answer and refuses an adapter that gets one wrong, rather than
  running half-configured. Clones beyond that aren't supported, and
  the OBDLink is the adapter the tiers verify.
- BLE and the mobile apps still can't reach a car until the app-fed
  stream is built. This record leaves that seam in place but doesn't
  build it.
