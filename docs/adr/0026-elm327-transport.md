# ADR-0026: ELM327 transport — `elm327://` drives an ELM327-compatible adapter over a serial port, with the request engine

- **Status:** Accepted.
- **Date:** 2026-10-27. Built in the steps of section 9.
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

The OBDLink Family Reference and Programming Manual (FRPM, revision E,
2025-01-10) covers the adapter on hand:
- **The chip:** the MX+ is an STN2255 or STN2256, "OBD to Bluetooth w/
  support for MS CAN and SW CAN". It answers the ELM327 command set
  and starts with `ELM327 v1.4b`. A parallel `ST` command set adds
  more.
- **Version:** `STI` prints the firmware as `STN<device_id> vX.Y.Z`.
- **Voltage:** `STVR` reads it as `12.34`, or dashes above 65.5 V.
  `ATRV`, `ATMA` and `ATST` are "supported for backwards compatibility
  only", in favor of `STVR`, `STMA` and `STPTO`.
- **Response pending:** programmable parameter 2A, default `04`, has
  bit 2 set: "Processing 7F xx 78's: Enabled (CAN & KWP)", for every
  service and every CAN protocol.
- **Programmable parameters** live in the adapter's non-volatile
  memory, so another app could have changed them.
- **Timing:** the first frame of a reply is always waited for up to
  the full `ATST`/`STPTO` timeout, 102 ms by default on an OBDLink.
- **Its own framing:** `STPX` sends a request with its header, its
  timeout and its expected response count in one command, up to 4 KB
  on the MX+. `STCSEGR 1` would have the adapter reassemble
  multi-frame replies itself (off by default).
- **The serial rate:** on a Bluetooth OBDLink it "varies by device
  and cannot be changed".
- **The buffer:** much larger than the ELM327's 256 bytes, so
  `BUFFER FULL` is rare.
- **Three CAN transceivers, one active at a time:** high-speed CAN on
  pins 6 and 14 (the ISO 15765 presets: 11- or 29-bit, 500 or 250
  kbit/s), medium-speed CAN on pins 3 and 11 (125 kbit/s), and
  single-wire CAN on pin 1.
- **More error words:** `FC RX TIMEOUT`, `OUT OF MEMORY`,
  `UART RX OVERFLOW` and `UNABLE TO CONNECT`.

The OBDX Pro's own documents ("OBDX Pro Command Set Reference Guide",
version 3.00, 2025; "How to Connect to Wired/Wireless", version 1.00,
2022) show that the J2534 adapter on hand speaks the same wire:
- **ELM by default:** the OBDX starts up with the ELM command set, plus
  its own `DX` commands. Its J2534 DLL uses a binary protocol (DVI)
  instead.
- **Protocols:** the VX supports `AT SP 6` to `9`, the same four
  ISO 15765-4 protocols.
- **Connections:** the VT, VX, FT and GT reach a host over USB (a
  serial port Windows creates once the OBDX USB driver is installed),
  classic Bluetooth, BLE (the Nordic UART service UUIDs) and WiFi (a
  TCP socket to 192.168.4.1, port 23).
- **Different defaults:** headers are off, linefeeds on and echo on.
- **`ATI`:** its answer doesn't match the STN's. The manual gives two
  forms itself, `ELM327 V1.4` in an example and `ELM327 v2.1` in the
  command's description.
- **`AT RV`:** answers in the form `14.5v`, read from pin 16.
- **`ATPPS`:** a compatibility stub that answers `OK`.
- **The CAN header:** described as "the first 11bit (2bytes)" for
  11-bit CAN. The manual has no example line that shows whether such
  an ID prints as three hex digits or four.
- **`DX US`:** returns a unique serial number.

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
elm327://<port>[?profile=<name>][&did=…][&baud=500000|250000][&serial=<n>][&filter=…][&txid=…][&poll=…][&probe=0|1][&identify=0|1]
```

- **`<port>`** is the OS's name for the port: `COM5` (Windows),
  `/dev/cu.<name>` (macOS), `/dev/rfcomm0` or `/dev/ttyUSB0`
  (Linux). On Windows a bare `COMn` is opened as `\\.\COMn`, so ports
  past `COM9` work.
- **`profile`** and **`did`** are core's, as for every scheme. The
  default profile is `obd2`, as for `j2534://`.
- **`baud`** is the CAN bus rate, as it is for `j2534://`: 500000 (the
  default) or 250000, the two rates ISO 15765-4 uses and the ELM327
  has protocols for. Any other value is refused at create.
- **`serial`** is the serial port's rate for a wired adapter, default
  38400. The ELM327 starts at 9600 or 38400, chosen by a pin at
  power-up, and other chips choose their own. A Bluetooth SPP port
  ignores it. It has its own key because `baud` already means the CAN
  rate in a J2534 run sheet.
- **`filter`**, **`txid`**, **`poll`**, **`probe`** and **`identify`**
  mean what they mean for `j2534://`, so a run sheet's query carries
  over. `flags` is J2534's connect flags and isn't taken. The channels
  come from `?filter=` or the profile's declarations
  (`resolve_channels`, shared with J2534, so `raw` hears the engine
  ECU).
- **The addressing mode** follows from the channels by the J2534 rule:
  a channel ID above `0x7FF` is 29-bit, and a mix of 11- and 29-bit
  channels is refused at create. With `baud` it picks the protocol:
  `AT SP 6` (11-bit, 500 kbit/s), `7` (29-bit, 500), `8` (11-bit, 250)
  or `9` (29-bit, 250).

Unknown keys and malformed values fail at create, before the port is
opened (`ecuio_elm327_query_keys`, rejection tests for each malformed
form). A port that can't be opened is `ECUIO_ERR_LOAD_FAILED` with the
OS's reason ("COM5: the port is in use by another program", "…: no
such port"). That is the code's reuse the replay transport made
first: its text reads "what the URI names couldn't be …", which fits a
port as it fits a recording. An adapter that doesn't answer the reset
as an ELM327 does is `ECUIO_ERR_VENDOR_REFUSED`, with what it said.
Neither detail holds a path with an account name, since port names
never do.

### 3. A pure protocol layer, then the worker

The protocol has its own header, `core/src/elm327_protocol.h`. Like
`request_engine.h` it does no I/O and reads no clock, so every rule is
a unit test over canned bytes. It is set up with the session's
addressing mode (11- or 29-bit, section 2), the one input its parsing
depends on.
- **Line assembly:** bytes in, whole lines out. CR ends a line, LF is
  ignored, and `>` ends a reply whether or not it follows a CR. Bytes
  arrive in any chunking: a line, or the prompt, split across reads,
  or several replies in one.
- **Line classification:** a CAN frame (`7E8064100BE3FA813`), a
  failure word (section 6), `OK`, a value (the voltage, a version
  string), or something else, which is ignored and counted. With
  spaces off nothing marks where the ID ends, so the mode decides it:
  eight hex digits of ID for 29-bit. For 11-bit, three digits, or four
  with a leading `0` (an adapter that prints the ID as two bytes, as
  the OBDX's manual may mean). Data bytes are whole pairs, so the
  line's length tells the two apart: odd for a three-digit ID, even for
  four. A frame line whose length fits neither is counted as
  malformed, not guessed at.
- **Values:** the voltage (`12.6V` from an ELM327, `14.5v` from an
  OBDX, `12.34` from `STVR`) becomes millivolts here, the unit letter
  in either case. It is parsed digit by digit and never through
  `strtod`, which follows the locale Qt sets (the replay transport
  parses its numbers the same way, for the same reason).
- **The version string** is recognized by `ELM327` at its start,
  ignoring case, and kept whole for the session info. Its version
  number is never compared: the adapters on hand report `v1.4b`,
  `V1.4` and `v2.1` for the same command set.
- **ISO-TP reassembly per CAN ID:** a Single Frame's length comes from
  its PCI byte; a First Frame's 12-bit length starts a message;
  Consecutive Frames must follow in sequence (`21` … `2F`, `20`, …).
  A gap, a duplicate or a length mismatch drops that ID's message and
  counts it. A complete message becomes one `ecuio_frame_t`, the same
  shape the J2534 transport delivers: `arbitration_id` from the line,
  data from the service byte on, `TRUNCATED` past
  `ECUIO_FRAME_DATA_MAX`.

**Setup at attach**, one command at a time, each answered before the
next. The commands fall in two kinds. A **contract** command sets
something the framing depends on; a wrong answer to it refuses the
connect (`VENDOR_REFUSED`, naming the command and what came back), so
an adapter never runs half-configured. A **reading** fills the session
info; ADR-0019 already rules that the session info's readings never
fail a connect, so a `?` or anything unexpected there means "not
present", and the field stays empty.

| Command | Kind | Why |
|---|---|---|
| `AT Z` | contract | a known state; its answer must be an ELM327 version string (`ELM327` and a version, any case), or `OK` alone, as an OBDX's is: then `AT I`, sent next, must answer one |
| `AT E0`, `AT L0`, `AT S0` | contract | no echo, no linefeeds, no spaces: fewer bytes per reply |
| `AT H1` | contract | the CAN ID and the PCI byte on every line, so core reassembles and correlates |
| `AT D0` | contract | no data length digit between the ID and the data. An STN prints one when its parameter 29 is on, which another app could have set; the OBDX takes `AT D0` as a compatibility command and answers OK |
| `AT CAF1`, `AT CFC1` | contract | the adapter adds the PCI bytes when sending and sends the flow control after a First Frame. Both are defaults, set anyway: the framing rests on them, and a clone's defaults aren't trusted |
| `AT SP 6`, `7`, `8` or `9` | contract | ISO 15765-4 at the mode and rate of section 2, never an automatic search |
| `AT SH <tx>` | contract | the first channel's request ID |
| `AT CF`/`AT CM`, or `AT CRA <rx>` for one channel | contract | receive only the channels' reply IDs |
| `AT AT1` | contract | adaptive timing (the default, set explicitly) |
| `AT ST 19` | contract | the first reply's timeout, matched to the engine's 100 ms wait so the adapter and the engine give up together: 0x19 × 4 ms = 100 ms on an ELM327 (default about 200 ms). On an OBDLink 0x19 is already the default; the FRPM gives its step as 4.096 ms there, 102 ms |
| `AT I` | reading | the version string for the session info |
| `STI` | reading | an STN chip's firmware (`STN<id> vX.Y.Z`). A plain ELM327 answers `?`, which means it isn't an STN, not that it failed |
| `STVR` on an STN, else `AT RV` | reading | the adapter's input voltage, for `vbatt_mv`. Dashes (over 65.5 V) or anything unparsable mean no reading |
| `AT PPS` on an STN | reading | the programmable parameters. If parameter 2A is on with bit 2 clear (no `7F xx 78` processing), the session info flags it (`ECUIO_SESSION_INFO_PENDING_OFF`) and the session line says so, since a pending reply would then be lost (open questions) |

**The worker** drives the request engine as the J2534 worker does,
with one difference: **one command is outstanding for the whole
adapter**, not one per channel, because the ELM327 takes the next
command only after its prompt. Each pass:
1. ends the waits that ran out;
2. asks the channels in turn for a request that is due;
3. sends `AT SH <tx>` if the channel's request ID isn't the one set;
4. sends the request as hex, plus the response-count digit `1` when
   one single-frame reply is expected (a Service 01 request to a
   physical ID whose reply fits one frame, by the appendix of
   2026-10-29; never for 09, 22, or a functional ID);
5. reads to the prompt, dispatches each reassembled frame through
   `ecuio_session_dispatch_callbacks`, and hands each matched frame to
   `ecuio_engine_on_frame`.

`NO DATA` completes the read and leaves the engine's wait to expire
the request, so a timeout is judged in one place. The engine's
constants (the 100 ms wait, the 20 ms floor) stand, and `AT ST 19`
makes the adapter's first-reply timeout the same 100 ms. With the
count digit, an answered request returns as soon as its reply is in.
Without it, adaptive timing ends the wait for further replies soon
after the last one. A `7F xx 78` reaches the engine as on J2534 and
extends its wait. On an STN the adapter keeps listening for the final
reply (parameter 2A), so that reply arrives in the same read. The
worker allocates nothing per frame, as `CoreTest.WorkersAllocateNothingPerFrame`
requires of every worker.

**One path for every adapter.** The worker uses only the ELM327
command set: `AT SH` and the count digit, with core reassembling the
replies. It doesn't use the STN's `STPX` (a header, timeout and count
per request, saving the `AT SH` round trip on a channel switch) or
`STCSEGR` (the adapter reassembling the replies). That way the fake
covers one path, a plain ELM327 and an OBDLink behave alike, and core
keeps the per-ID reassembly the interleaving case needs. `STPX` is
the first candidate for an STN fast path, once the bench measures how
much a channel switch costs. With one channel (the `obd2` profile on
one ECU) there is no switch to save.

### 4. Passive listening is a later step

A session with no requests (`raw` with no `?poll=`, or a profile with
nothing to ask) would listen with `AT MA`, or `STMA` on an STN. The
ELM327's 256-byte buffer and a serial link cannot carry a busy
500 kbit/s bus, and `BUFFER FULL` means data was lost. An STN's buffer
is much larger, but the Bluetooth link's rate still caps it. This record defines the shape: the
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

The adapter is left as it was found. The transport never writes its
non-volatile memory: no `AT PP … SV`/`ON`, `STWBR`, `STRSTNVM`,
`STSATI`, `STSDI`, `STS@1` or `STBTDN`. Every setting it makes
(`AT` and `ST` commands without a write) lasts until the adapter's
next reset. It never asks for a serial number, the STN's `STSN` or
the OBDX's `DX US`, since the bench-session rules keep serial numbers
out of every record. It never switches an OBDX to its binary protocol
(`DX DP1`). The transport speaks only the ELM command set.

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
| `FC RX TIMEOUT`, `OUT OF MEMORY`, `UART RX OVERFLOW`, `DATA ERROR`, `<DATA ERROR`, `<RX ERROR` | that request's read ends without a reply, counted and logged; the state is unchanged, and the engine's wait judges the request |
| `UNABLE TO CONNECT` | the same as `CAN ERROR`. It belongs to an automatic search, which the setup never starts, so it means the bus isn't there |

A hard failure reopens the port on the backoff's schedule and re-runs
the setup, so a Bluetooth link that drops and returns recovers
without a reconnect, as a J2534 adapter's does. The bridge, the pages
and the activity log already show these states (`bridge.md`), so the
UI needs no new code.

### 7. Session info and discovery

- **Session info:** `backend` is `ECUIO_BACKEND_ELM327` (5, appended).
  The `firmware` version string is the `AT I` answer, plus `STI`'s
  answer when the adapter has one (an STN chip, which an OBDLink is).
  `vbatt_mv` comes from `STVR` or `AT RV`. `protocol` and `baud` are
  the CAN protocol and rate. The bridge's `sessionInfoText` gets an
  ELM327 form, for example "Session: ELM327 on COM5 (ELM327 v1.4b,
  STN225x vX.Y.Z); ISO 15765 at 500000 baud; profile obd2, channel
  7E8/7E0; battery 12.6 V". The bench records what the MX201 actually
  answers.
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
  fake J2534 DLL uses (one ECU for both fakes, `tests/fake_ecu.h`: its
  PIDs, DIDs and identification items), with knobs for `NO DATA`, `CAN ERROR`, `?` to `STI` (a plain
  ELM327), a dropped link and a slow reply. That covers the probe, the
  rotation, the scan, identification, link health and recovery, the
  pending path, and nothing allocated per frame.
- **The seam:** the session tests reach the fake through a port
  factory declared in `internal.h`, the header that isn't part of the
  ABI and that `test_core` already includes. Nothing is added to a
  public header, and no `_for_testing` call joins the C ABI. The
  factory replaces only the port, the I/O at the bottom: the protocol
  layer, the worker and link health run exactly as they do on an
  adapter. That is what separates it from the test-only J2534 backend
  switch ADR-0025 deleted, which chose a code path the app never ran.
- **The port code itself:** the fake bypasses it, so it is tested
  separately. On Linux and macOS a pseudo-terminal pair (`openpty`)
  puts the fake ELM327 on the far side of a real terminal device, so
  the POSIX port, its `termios` settings and its read timeouts run in
  CI. Windows has no built-in virtual COM pair, so the Win32 port code
  is verified at BENCH and on the vehicle only. This is a known gap.
- **BENCH** (BENCH prep items added to
  `docs/bench-sessions/bench-prep.md`). The adapter on hand is an
  OBDLink MX+, model MX201. It has no USB port and takes its power only
  from the OBD connector, so a bench session without a car needs a
  12 V supply on the connector's power pin (16) and grounds (4, 5).
  Its one push button starts pairing mode. It has four indicators,
  Power, OBD, Host and Link, and the run sheet notes what they show
  at each step, so a failure can be placed on the Bluetooth side or
  the bus side. With that supply:
  - the setup's answers;
  - `AT RV` against the supply's own reading;
  - what `CAN ERROR` looks like with no bus;
  - a Bluetooth drop and its recovery;
  - the time from a request to its prompt with nothing to answer.

  Without the supply these move to the vehicle session, ahead of its
  first request.

  **The VX is a second ELM327 adapter.** It runs on USB power, so a
  bench session needs no supply. It speaks the ELM command set by
  default, and it isn't an STN, so it exercises the readings' "not
  present" path (`STI` refused) on real hardware. That depends on the
  OBDX USB driver giving the VX a serial port. The dev machine lists
  only `COM1` with no adapter attached, so the first bench step is to
  plug the VX in and see what Windows creates. The J2534 DLL and an
  `elm327://` session can't hold that port at once.
- **Vehicle:** a read-only session on the same car as the J2534
  sessions, under `obd2`. Its values are compared against the VX
  sessions' recordings and against the cluster, and its request rate
  is measured.

### 9. Order of work

1. Move link health and its backoff into a shared header (no behavior
   change; the J2534 health tests pin it).
2. The protocol layer and its unit tests.
3. The transport: the port on Windows and POSIX, the worker, session
   info, the URI and its rejection tests, the fake, the HARNESS tests
   and the pseudo-terminal test. The docs that name the schemes move
   in the same change:
   - CONTRIBUTING.md (its input-validation and URI sections list
     `mock://`, `replay://` and `j2534://`);
   - README.md's architecture diagram;
   - docs/architecture.md;
   - STATUS.md and CHANGELOG.md's "Unreleased" section.
4. The app: serial ports in Settings' device list, an `elm327://`
   preset, the ELM327 form of the session line.
5. BENCH, then the vehicle session.
6. Later, each with its own record or appendix: passive listening
   (`AT MA`, or the STN's `STMA`); the app-fed stream for BLE and the
   mobile apps; and a TCP socket as a second kind of port, for WiFi
   adapters (the OBDX at 192.168.4.1, port 23). Core can open a TCP
   socket without Qt, and WiFi is a route to a car from iOS without
   BLE.

## Open questions for the bench

- **Response pending:** the FRPM settles it for an STN: by default
  (parameter 2A, bit 2) the adapter processes a `7F xx 78` and goes on
  listening for the final reply. Two things are left. One is whether
  the pending line is printed as well, which the engine handles
  either way. The other is the plain ELM327, whose datasheet says
  nothing. If a plain chip returns to the prompt after the pending
  line, the final reply arrives while it isn't listening and is lost,
  and the engine waits out its pending wait (5.05 s) and asks again.
  The count digit is never sent with 0x22 or 0x09, where a `78` is
  plausible. The test car never sent a `78` to the J2534 sessions.
- **STN answers:** what the MX201 answers to `AT I`, `STI` and
  `AT PPS` (whether any parameter has been changed by another app),
  and which STN chip it is (STN2255 or STN2256). The FRPM gives the
  forms; the bench records the values.
- **The voltage:** the MX201 runs only from the OBD connector (it has
  no USB port), so a bench session needs a 12 V supply. The question
  is what `STVR` and `AT RV` read there. The datasheet's chip measures
  its own pin 2, divided down from the adapter's input in the
  reference circuit, and the FRPM's `STVR` reads the calibrated
  `ANALOG_IN` pin, so both should read the supply. The VX, by contrast, reads 0.0 V on USB
  power alone (BENCH prep #10). The run sheet records what it read and
  against what.
- **The OBDX in ELM mode:**
  - whether the VX gets a serial port over USB;
  - what it answers to `AT Z`, `AT I`, `STI` and `AT RV` (answered:
    the appendix of 2026-10-27, "the VX's answers");
  - how it prints an 11-bit ID with `AT H1` (three digits or four);
  - whether it keeps listening after a `7F xx 78`, which its manual
    doesn't say.
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
  every contract command's answer and refuses an adapter that gets one
  wrong, rather than running half-configured. The readings are
  tolerated, so a genuine ELM327 without STN's commands connects.
  Clones beyond that aren't supported. The tiers verify two
  implementations of the command set: the OBDLink MX+ (an STN) and the
  OBDX Pro VX in its ELM mode.
- Only ISO 15765-4 CAN at 500 and 250 kbit/s. The one car verified
  runs 11-bit at 500; the other three protocols are the same code with
  a different `AT SP`, tested against the fake only until a car on
  them is read.
- BLE and the mobile apps still can't reach a car until the app-fed
  stream is built. This record leaves that seam in place but doesn't
  build it.

## Appendix: 2026-10-27, no bench supply for the MX+

There is no 12 V supply on hand, and the MX201 takes its power only
from the OBD connector, so the MX+ first powers up on the car. As §8
provides, its bench items move to the vehicle session, ahead of its
first request:

- the setup's answers, and what it says to `AT I`, `STI` and `AT PPS`
  (the open questions' "STN answers");
- `STVR` against the battery voltage the VX's `READ_VBATT` reports on
  the same car in the same visit, in place of a supply's own reading;
- the time from a request to its prompt with nothing to answer, with
  the ignition off: this car's bus still acknowledges then, and no ECU
  answers (docs/bench-sessions/2026-10-22-vehicle.md);
- a Bluetooth drop and its recovery, by turning the laptop's Bluetooth
  off and on mid-session.

`CAN ERROR` with no bus at all is left to the VX's bench session, on
USB power with no car; that session covers the VX alone (its serial
port, its ELM answers, its voltage reading).

## Appendix: 2026-10-27, a wire log

Several of §8's bench items are what the adapter prints: each setup
command's answer, how an 11-bit ID is printed, a `7F xx 78` line, and
how long a request waits for its prompt with nothing to answer. The
J2534 path has the surrogate's session log for that; `elm327://` had
nothing below the activity log, which shows only states. So each
connect writes a wire log of its own (`core/src/elm327_log.cpp`):
`ecuio-elm327.<YYYYMMDD-HHMMSS-mmm>.log` in the temporary directory,
the newest 50 kept. The setup, the readings, lines the adapter sends
unasked, and the port's failures and reopens go in always. Every
request and reply goes in with `ECUIO_SUR_LOG_FRAMES=1`, the switch a
recording already sets for the surrogate's frame lines, so one switch
serves both transports; each prompt carries its time after the
request. The worker writes it with no allocation, which
`Elm327SessionTest.TheWorkerAllocatesNothingPerFrame` checks with the
requests' lines on.

## Appendix: 2026-10-27, the VX's answers

The VX's bench session (docs/bench-sessions/2026-10-27-elm327-bench.md)
opened its USB serial port (COM4, Windows' own `usbser.sys`) and was
refused at the first command: the VX answers `AT Z` with `OK` alone,
where an ELM327 prints its version. The OBDX Command Set Reference
says as much: `AT Z` "does a full reset", and `AT I` reports
`ELM327 v2.1` "for legacy support". A probe of the same adapter, ELM
commands only, gave the rest:

- `AT I`: `ELM327 V2.1`; `AT @1`: `OBDX Pro VX`; `STI`: `?`;
  `AT RV`: `0.00V` on USB power.
- Every other contract command answers `OK`, `AT SP 8` included.
- With no bus at all, a request answers `NO DATA`, not `CAN ERROR`.

So the reset's rule widens: `AT Z` answered by a version, or by `OK`
alone, in which case `AT I` is asked next and must answer a version.
A device that isn't an ELM327 is still refused, now at `AT I`.

`NO DATA` with no bus means the VX can't tell a missing bus from an
ECU that doesn't answer, so on the bench it reads as quiet
("Vehicle not responding"), not "Requests failing". §6's mapping is
unchanged: on a car the bus is there.

An adapter that resets on its own is found by its version line
arriving unasked (§6). The VX's reset prints none, so a reset
without a power cut would go unseen; on USB it can't lose power
without the port closing, which reopens the port and runs the setup
again. What it prints on power-up is for its next session's wire log.

**Note, 2026-10-27, later:** the second session
(docs/bench-sessions/2026-10-27-elm327-bench-2.md) connected through
the widened rule, went quiet with no bus, and recovered from a cable
pull with no reconnect. After the reopen nothing came unasked: the VX
prints nothing on power-up that reaches the port. With no node to
acknowledge them, its first three requests after a setup reach their
prompt in 100 ms and every later one in 300 ms, the three frames its
CAN controller holds; on a car the bus acknowledges.

## Appendix: 2026-10-27, the first vehicle session

The MX+ (Bluetooth, an STN2255 v5.12.2) and the VX in ELM mode both read
the car (docs/bench-sessions/2026-10-27-elm327-vehicle.md), the first
`elm327://` sessions on a vehicle. The `obd2` values decoded through a
cold start and idle to the values the VX produced through J2534 on
2026-10-25, and identification returned the same items. This settles the
open questions:

- **STN answers:** the MX201 is an STN2255 (firmware v5.12.2). `AT I`,
  `STI` and `AT PPS` answered in the datasheet's and the FRPM's forms,
  parameter 2A at its default (no app had changed it).
- **The voltage:** `STVR` read the battery at the connector, 11.6-12.1 V,
  within 0.2 V of the VX's reading on the same car. The MX+ first powered
  up here, as the appendix "no bench supply for the MX+" provided; no
  12 V bench supply was needed.
- **Rate:** over Bluetooth the MX+ sustained ~32 completed requests a
  second, on both a single-PID poll and the full rotation, above the
  VX's 26 a second through J2534 on the same car. The STN's turnaround,
  not the PID count, sets it.
- **The OBDX in ELM mode:** the VX took a serial port over USB, answered
  `AT Z` with `OK` and the version to `AT I` (the appendix "the VX's
  answers"), printed 11-bit IDs as three hex digits, and decoded to the
  same values.

A Bluetooth drop (the laptop's Bluetooth off, then on) failed the port,
went to "Adapter fault", and recovered in about 28 seconds: the port
reopened on the backoff and the setup ran again, with no reconnect, as a
J2534 adapter's hard failure does. The car sent no `7F xx 78`, so
response pending (an open question) is still unseen.

This completes ADR-0026 step 5 (section 9). The wire logs hold the
identification replies in hex and are committed with those redacted
(bench-sessions.md).

**Note, 2026-10-27, later:** three statements above go further than the
committed evidence. The MX+'s `STVR` read 12.1 V with the ignition off,
the VX's reading; the 11.6 V came with the ignition on and isn't
comparable. The VX's ELM-mode connect in the session kept no wire log;
a later one that day (the sheet's `vx_elm/` logs) ran through an engine
start at about 30 completed requests a second, its replies printed as
`7E8` like the MX+'s. And the drop failed the port at once, its fault within 0.1 s;
of the 28 seconds to recovery most was Bluetooth off, and the first
value came within about 6.5 seconds of Bluetooth coming back (the
reopen's 5-second backoff and the setup).

## Appendix: 2026-10-27, passive listening

The request path has run on a car (the appendix "the first vehicle
session"), so §4's later step is built. This appendix settles its
details. A session with nothing to ask (`raw` without `?poll=`, or a
profile with no requests) listens with `AT MA` instead of being refused
at create.

**`AT MA` on every adapter, never `STMA`.** On both chips `AT MA` keeps
the CAN receive filter the setup sets (`AT CRA`, or `AT CF` and
`AT CM`). The datasheet says the monitor "will be affected by" CF, CM
and CRA. The FRPM says `ATMA` opens the flow control, pass and block
filters but not the CAN hardware filter, which `STMA` opens as well. So
`AT MA` hears exactly the channels' reply IDs on both chips, as a J2534
passive session does, and §3's one path for every adapter holds. The
FRPM lists `ATMA` among the commands kept for backwards compatibility;
the STN still answers it.

**What it hears:**
- **The channels, from §2:** `?filter=`, the profile's, or for `raw`
  the default profile's engine ECU.
- **A session with no channel is still refused at create:** a profile
  that declares none, without `?filter=`. An unfiltered bus is more than
  a 256-byte buffer and a serial link carry, and its broadcast frames
  aren't ISO-TP, so the reassembly below can't take them. Listening to
  the whole bus (raw frames, `AT CAF0`, for a car whose OBD port carries
  more than diagnostics) would be a step of its own.
- **The monitor is silent.** Both manuals say it sends no ACK and no
  flow control, so a passive session never reaches the bus, which goes
  beyond §5.
- **A multi-frame message completes only when the tester it answers
  sends the flow control.**

**Lines become frames as a reply's do.** Each frame line goes through
the same ISO-TP reassembly per CAN ID and is dispatched with its arrival
time. Some lines the monitor marks: a flow control prefixed `FC:`, a
line ending `<DATA ERROR` or `<RX ERROR`. These aren't frames; each is
ignored and counted. The engine gets nothing, since it has no requests.

**Starting and stopping.** The worker sends `AT MA` once the setup has
run. It then reads lines, with no prompt expected, in the request
path's 20 ms slices. To stop the monitor it sends one space and reads to
the prompt, with `STOPPED` before it.
- **Why a space:** both chips ignore spaces at the prompt, so a stop
  that crosses a monitor already ended can't become a command. A lone
  CR would repeat the last command, `AT MA` itself.
- **Detach:** the worker waits at most 1 s for the prompt, then closes
  the port either way. An adapter left monitoring is already handled at
  the next connect: the setup sends `AT Z` twice at most for exactly
  that case.

**Link health**, §6's table for a session that only listens:

| What happens | State |
|---|---|
| A whole message from a channel | `ECUIO_OK` |
| No message: each 100 ms read counts toward the quiet run, as J2534's passive reads do | `ECUIO_ERR_VEHICLE_TIMEOUT` (quiet), no episode |
| 5 s with no line at all | the worker stops the monitor. `STOPPED` and the prompt within 5 s show the adapter is there, and `AT MA` starts again with the state unchanged. No prompt is a hard failure. A quiet bus and a hung adapter look the same until the adapter is asked |
| The port fails | `ECUIO_ERR_TRANSPORT_HARD_FAILURE`; the port is reopened and the setup runs on the backoff's schedule, then `AT MA` (§6) |
| `BUFFER FULL` | frames lost (below), the state unchanged. If a prompt follows (the monitor ended), `AT MA` again at once |
| The monitor ends unasked any other way (`CAN ERROR`, `STOPPED` with no stop sent, a prompt) | logged, and `AT MA` again on the backoff's schedule; counts as quiet. The VX answers `NO DATA` with no bus at all (the appendix "the VX's answers"), so a missing bus isn't something the monitor can report reliably |
| `LV RESET`, or the version string | the setup again, as §6 |

**Frames lost.** `BUFFER FULL` means the adapter dropped lines it
couldn't send in time, and how many is unknown.
- **The next frame dispatched carries a new flag,**
  `ECUIO_FRAME_FLAG_OVERFLOW` (`0x80`, appended): frames were lost
  before this one. The flag is J2534's own rule. J2534-1 (v05.00,
  §7.2.2.1 and its error handling) sets the `BUFFER_OVERFLOW` bit of
  `RxStatus` "for the first successfully received message after the
  overflow condition has cleared". A read that returns such a message
  answers `ERR_BUFFER_OVERFLOW`, with the messages it read.
- **The loss travels with the data.** `frames.csv` records each frame's
  flags, so a recording shows where the gap is and its replay plays it
  back.
- **The bridge counts flagged frames.** The Log page's frame view shows
  the count beside the frames the bridge itself dropped, and the
  activity log says so the first time in a session ("The adapter's
  buffer overflowed: frames were lost").

**The wire log** always takes the monitor's start, each stop and why,
each `BUFFER FULL` and each liveness check. Frame lines go in only with
`ECUIO_SUR_LOG_FRAMES=1`, as a request's do.

**Verification:**
- **UNIT:** the `FC:` line and a frame line ending `<DATA ERROR` are
  classified as not frames.
- **HARNESS:** the fake ELM327 learns `AT MA`. It streams its ECU's
  frames through the receive filter it was given (single and
  multi-frame, plus one ID the filter must stop) until a character
  arrives. Its knobs cover `BUFFER FULL` with and without a prompt
  after it, no prompt to a stop (a hung adapter), and a dropped link.
  The tests:
  - a `raw` session's messages are dispatched, the channel's only;
  - the quiet run reads as quiet;
  - the liveness check stops and restarts the monitor;
  - a hung adapter is a hard failure and recovers;
  - `BUFFER FULL` flags the next frame and leaves the state alone;
  - detach sends a space and waits for the prompt;
  - nothing is allocated per frame;
  - a session with no channel is refused at create. The test that
    expected a passive session to be refused now expects it to run.
- **Bridge:** a flagged frame is counted, and the activity log line
  appears once.
- **BENCH:** the VX on USB power with no bus. What it prints to
  `AT MA`, the quiet state, the liveness cycle every 5 s and the stop at
  detach, all from the wire log.
- **Vehicle:** a passive session needs another tester on the bus. That
  means an OBD Y-splitter cable, with the VX polling through J2534 and
  the MX+ listening through `elm327://…?profile=raw`. The listener's
  messages are compared with the poller's recording of the same
  replies. Without a splitter this item waits.

**Note, 2026-10-27, later:** the VX's bench item ran
(docs/bench-sessions/2026-10-27-elm327-bench-3.md). With no bus, `AT MA`
printed nothing; each 5 s check's space got `STOPPED` and the prompt
within 0.3 ms, and the monitor started again with the link quiet and no
failure. The stop at close went the same way.

## Appendix: 2026-10-27, WiFi over TCP

§9's item 6 names a TCP socket as a second kind of port. A WiFi
adapter runs its own access point and takes a TCP connection on a fixed
address:
- **The OBDX:** 192.168.4.1, port 23. Its "How to Connect to
  Wired/Wireless" guide gives the address, the same on Windows, iOS and
  Android, and an access point with no password. The VX's product page
  lists USB, Bluetooth and WiFi, so the adapter on hand has it.
- **OBDLink's WiFi models:** 192.168.0.74, port 23.
- **Clones:** most at 192.168.0.10, port 35000; PLX's KiWi on port
  30000; some on 192.168.1.10.

The ELM command set is the
same over the socket as over a serial port, so only the port changes.
The setup, the worker, the monitor and link health run unchanged, behind
§1's byte-stream interface (`ecuio_elm327_port_ops_t`). A socket in
core also serves step 6's mobile apps: WiFi reaches a car from iOS
without BLE.

**The URI:** `elm327://<IPv4 address>:<port>[?…]`, for example
`elm327://192.168.0.10:35000`.
- **Which kind of port.** The text before the query is a TCP address
  when it's a dotted IPv4 address, a colon and a port from 1 to 65535.
  Anything else is a serial port, as in §2. The two can't be confused:
  a `COMn` name has no dot or colon, and a POSIX device starts with `/`.
- **The port is required.** The adapters differ (23, 30000 or 35000), and a
  default would turn a wrong guess into "connection refused", which
  hides the cause.
- **No host names.** Every adapter has a fixed address on its own
  network. A name lookup can block for seconds with no timeout core
  controls, and it sends the name to whatever DNS server the machine
  uses. IPv6 isn't taken either: the adapters' access points are IPv4.
- **`serial=` with an address is refused** (`BAD_URI`). A socket has no
  serial rate, and a run sheet that carries one over from a USB session
  should be told so. A Bluetooth port takes the key and ignores it only
  because core can't tell it from a USB port; a socket it can tell.
- **Every other key** works as in §2.

**Connecting:**
- **A 2 s limit.** The opener connects without blocking and waits at
  most 2 s. On the adapter's own network an answer, or a refusal, takes
  milliseconds. The limit also bounds how long a close waits on a
  worker that is reconnecting.
- **Nagle off (`TCP_NODELAY`).** Each command is a few bytes followed by
  a wait for its prompt, so nothing gains from coalescing writes, and a
  held write would delay every request.
- **No keepalive.** The worker already notices a dead link: a request
  with no prompt in 5 s is silent, and a monitor with no line for 5 s is
  stopped and asked (the appendix "passive listening"). Both are a hard
  failure, as a Bluetooth port's silence is.
- **The far side closing** (a read of 0 bytes) or resetting is a failed
  read, as a pulled cable is. It is a hard failure, and the socket is
  reconnected and the setup runs on the backoff (§6). A laptop that
  leaves the adapter's network and rejoins it recovers without a
  reconnect.
- **No `SIGPIPE`.** On POSIX a write to a closed socket must not raise
  `SIGPIPE`, which would end the app: `MSG_NOSIGNAL` on Linux,
  `SO_NOSIGPIPE` on macOS.
- **Winsock.** On Windows the opener calls `WSAStartup` and the close
  calls `WSACleanup`. Both count, so `ecuio_init` stays a no-op, as
  core.h promises.
- **Failures** read as a serial port's do: `LOAD_FAILED` with the
  address and the reason. Examples: "192.168.0.10:35000: no answer
  within 2 s (is this computer on the adapter's WiFi?)", "…: the
  connection was refused", "…: the network is unreachable".

**No discovery.** Settings lists no WiFi adapters. Finding one would
mean connecting to addresses on the network: the write §7 won't make to
an unknown serial port. The user joins the adapter's network and types
its address. The URI field's hint and the `elm327://` preset name both
forms. The address is the session's port name: it appears in the
session line, the "Connected:" line and the wire log. An adapter's
address on its own access point is the maker's default, not personal
data, so a record keeps it.

**Verification:**
- **UNIT:** the name rules.
  - Accepted: addresses with ports at both bounds.
  - Refused: no port, a port of 0 or 65536, a host name, an IPv6
    address, five octets, an octet over 255, a stray dot.
  - Unchanged: `COMn` names and `/dev/` paths.
- **HARNESS, on every platform:** a loopback test. The test listens on
  127.0.0.1, on a port the OS picks, and puts the fake ELM327 behind
  the socket it accepts. So the real TCP opener runs in CI on Linux,
  macOS and Windows: its reads, its timeouts, and its end when the far
  side closes. It is the first Win32 port code CI runs (the serial
  port's stays a BENCH item, §8). Loopback only, so no firewall prompt.
  The tests:
  - a whole session through the socket;
  - the far side closing, then listening again: a hard failure, then
    recovery on the backoff;
  - nothing listening: `LOAD_FAILED`, "the connection was refused";
  - `serial=` with an address: `BAD_URI`.
- **Not tested: the 2 s limit.** Reaching it needs an address that
  drops packets, and a test can't count on one.
- **BENCH:** the VX over WiFi, on USB power with no bus, from the
  laptop. The VX's network ("OBDX Pro VX" and its MAC address, open,
  2.4 GHz) is up on USB power alone and on the OBD port's alone, with
  no USB cable; the laptop sees
  it and the dev machine doesn't (2026-10-27), a matter of that
  machine's radio: with its adapter set to prefer 2.4 GHz, it joined. A record writes the network's name without the MAC, which
  identifies the adapter as a serial number does.

  A probe from the dev machine (ELM commands only, outside QtTune)
  settled three things. The socket connected in 25 ms, well inside the
  2 s limit. Nothing came before the first command: port 23 carries no
  telnet negotiation. And `AT Z`, `AT I`, `AT @1` and `AT RV` answered
  as on COM4 (the appendix "the VX's answers"), each prompt about 5 ms
  after its command, where USB's take 0.1 to 0.3 ms. The items left:
  - `elm327://192.168.4.1:23` connects and the session goes quiet;
  - whether WiFi answers while a program holds the USB port too;
  - leaving the VX's network and rejoining it: a hard failure, then
    recovery on the backoff;
  - on the car, the request rate against the VX's serial and J2534
    sessions'.

**The access point has no password.** Anyone in range can join it and
reach the adapter while it sits on the car. That is the adapter's
design; QtTune can't change it, and it is the reason to unplug a WiFi
adapter rather than leave it on the car.

**Note, 2026-10-27, later:** built (`core/src/elm327_tcp.cpp`), with
the HARNESS tests above (`Elm327TcpTest`). One statement above holds
only off Windows: a refusal doesn't take milliseconds there. Windows
answers a refused connect by sending the SYN again, every 500 ms, and
reports the refusal only after 2 s, which the 2 s limit would read as
no answer. The socket sets `SIO_TCP_INITIAL_RTO` to two SYN retries, so
a refusal arrives in about 1 s and a SYN that WiFi drops is still sent
again. The VX items are BENCH prep #20. The first, a session over the
VX's WiFi, ran on the bench the same evening
(docs/bench-sessions/2026-10-27-elm327-bench-4.md): the setup and
`NO DATA` as on its USB port, a few ms slower per command. Leaving the
network mid-session (docs/bench-sessions/2026-10-27-elm327-bench-5.md)
failed the read at once and recovered on the backoff with no reconnect,
but only after the VX was power-cycled: once a client left without
closing, its WiFi answered no ARP, no TCP and no DHCP, whether QtTune
went on reopening or was disconnected. The adapter can't be reached to
be fixed from here,
so a WiFi adapter that stays in "Adapter fault" after its network is
back wants a power cycle. So does WiFi after USB: with a session on the
VX's USB port, J2534 or `elm327://`, a WiFi join was refused, and after
the J2534 session closed the access point was gone
(docs/bench-sessions/2026-10-27-elm327-bench-6.md). The VX gives one
link at a time.

## Appendix: 2026-10-29, the count digit and long Service 01 replies

§9's step 4 sent the digit with every Service 01 request to a physical
ID, on the reasoning that one ECU answers in one Single Frame. The
datasheet counts the digit in lines ("Setting the number of
responses": `09 02 5` for the VIN's five lines, and "if you should
mistakenly send `09 02 1`, you might cause problems"), and J1979 has
Service 01 PIDs whose replies don't fit a Single Frame's seven bytes:
`0x7F`, engine run time, returns 13 data bytes (Wikipedia's "OBD-II
PIDs" table, among others). With `1` the adapter returns after
the First Frame's line, the message never finishes, and the request
times out every time. The shipped `obd2` set asks none of them, so a
registered set naming one was the only way to meet it.

The digit now goes only where the reply is expected to fit:
- **The bitmaps** (`0x00`, `0x20`, … `0xE0`): four data bytes, always.
- **A PID whose every def** keyed `41 <PID>` ends within the Single
  Frame (`elm327_one_frame_pids_for` in `core/src/elm327_protocol.h`).
  A PID no def describes (`?poll=` under `raw`) gets none.
- **Learned:** a def reading only the first bytes of a longer reply
  (`0x7F`'s total run time, bytes 3 to 6) misleads that rule. A reply
  to a request with the digit that ends with a message under way takes
  the PID off the digit for the rest of the session, with a wire-log
  line ("01 7F's reply spans frames: asked with no response count from
  now on"). That one reply is lost to the engine's wait; the next goes
  without the digit and arrives whole.

A request without the digit waits out the adapter's `AT ST` timer after
its reply, 100 ms, so only the long PIDs pay for it. The fake ELM327
honors the digit as the datasheet does, printing at most that many
frame lines, so the session tests can fail on what it does
(`Elm327SessionTest.ALongServiceOnePidGoesWithoutTheCountDigit`,
`.AReplyThatSpansFramesTakesThePidOffTheCountDigit`). Whether an
adapter counts a `7F xx 78` line toward the digit is still unknown:
neither the datasheet nor the FRPM says, and the fake doesn't count it.
