# ADR-0040: SocketCAN requests: ISO-TP in core, and a controller that sends only after its first request proves the rate

- **Status:** Accepted. Built on 2026-10-31 and verified on `vcan0`;
  section 2's one-shot rule is provisional until the desk run (BENCH
  prep #32).
- **Date:** 2026-10-31.
- **Builds on:** [ADR-0032](0032-socketcan-listening.md) (the
  transport, the netlink checks, the simulated car on `vcan0`);
  [ADR-0015](0015-request-engine.md) (the request engine and its
  pacing); [ADR-0026](0026-elm327-transport.md) (core already
  reassembles ISO-TP from an ELM327's lines);
  [ADR-0018](0018-connect-failure-reporting.md) (a refused connect says
  why).
- **Amends:** [ADR-0032](0032-socketcan-listening.md) sections 2 and 3,
  with a dated note: a profile that requests runs on SocketCAN, on a
  controller in normal mode.

## Context

Every requesting transport so far hands the bus to an adapter that
does ISO 15765-2 itself: a J2534 driver's ISO15765 channel or an
ELM327 segments a request, sends the Flow Control frame a long reply
needs, and hands over whole messages. A SocketCAN interface is a bare
controller: core sees each CAN frame, a Protocol Control Information
(PCI) byte first, and nothing answers a First Frame unless core does.
So `socketcan://` refuses every profile that requests
(`NOT_IMPLEMENTED`, ADR-0032 section 3), and with it obd2,
uds-generic, identification, the trouble codes and clearing.

An Innomaker USB2CAN (a `gs_usb` adapter), an OBD-to-DB9 cable and an
OBD Y-splitter are now on hand. SocketCAN is Linux's, so the adapter
reaches core through WSL: `usbipd-win` attaches it, and WSL's kernel
(6.18) carries `gs_usb`, `can-isotp`, `vcan` and the USB/IP client.
The bench machine has both; the Windows laptop that goes to the car
gets the same once WSL is installed there. Android is out: its kernels
ship without CAN (ADR-0032 section 1).

ISO 15765-4:2005 (legislated OBD on CAN) sets what the external test
equipment must do, and these are the facts the decision rests on:

- **Timing (its Table 6):** N_As and N_Ar 25 ms, N_Bs 75 ms, N_Cr
  150 ms; the tester's Flow Control frame must follow a reply's First
  Frame within 25 ms (N_Br + N_Ar).
- **Flow control (Table 7):** the tester answers a First Frame with
  one Flow Control frame, ClearToSend, block size 0 and STmin 0, and
  never a wait frame.
- **Frames:** every diagnostic frame has a DLC of 8, its unused bytes
  undefined; up to eight ECUs may send segmented replies at once.
- **A wrong bit rate (section 4.2.1):** the tester sends one request
  and checks for any CAN error. On an error it must stop disturbing the
  bus at once (the standard asks for the interface to disconnect within
  100 microseconds), before the ECUs' error counters reach critical
  values.
- **Rates:** 500 and 250 kbit/s where the legislation names none.

A controller in normal mode acknowledges every frame it receives, and
at a wrong rate it reads errors and sends error frames from the moment
it comes up, before any program opens it. A transmit at a wrong rate
is worse: a CAN controller retransmits a failed frame by itself, in
hardware, until it goes error-passive or bus-off, far faster than any
user-space program can react. Core can't take the interface down or
flush its queue without `CAP_NET_ADMIN`, which ADR-0032 decided core
never uses.

## Decision

### 1. ISO-TP is core's, one module for every transport

`core/src/isotp.h`: pure, header-only, no I/O and no clock (the time
is passed in, as the request engine's is), so every rule is a unit
test on every platform and a fuzz target.

- **The receiver** keeps one slot per reply ID (eight, the standard's
  eight ECUs), reassembles Single, First and Consecutive Frames, and
  when the transport turns flow control on, answers each First Frame
  with the Flow Control frame Table 7 names (ClearToSend, BS 0, STmin
  0), returned for the transport to write. A Consecutive Frame later
  than N_Cr (150 ms) ends its message unfinished. A First Frame's
  32-bit escape length is refused: a message past 4095 bytes doesn't
  fit a frame (`ECUIO_FRAME_DATA_MAX`).
- **The ELM327's reassembly moves into it:** `elm327_isotp` becomes
  the receiver with flow control off (the adapter sends its own,
  `ATCFC1`) and no timer (the prompt ends a reply), so both transports
  share one reassembly, and `fuzz_elm327_reply` keeps covering it.
- **The sender** segments a message into a Single Frame, or a First
  Frame and Consecutive Frames paced by the receiver's Flow Control:
  its block size, its STmin (0x00 to 0x7F ms, 0xF1 to 0xF9 as 100 to
  900 microseconds, a reserved value taken as 127 ms, as ISO 15765-2
  says), a wait frame restarting N_Bs (75 ms), an overflow ending the
  send. No request the engine makes today is longer than a Single
  Frame (seven bytes: a packed `01` request is the longest), so the
  transport sends only Single Frames, and a `static_assert` ties the
  engine's longest request to that. The sender is ready for a longer
  request, and the conformance tests hold it to the kernel's receiver
  (section 6).
- **Every frame core sends has a DLC of 8,** padded with 0x00 (the
  standard leaves the bytes undefined). Frames received with a shorter
  DLC are taken when they hold what their PCI says: uds-generic reaches
  ECUs outside legislated OBD, and ISO 15765-2 lets those leave out the
  padding.
- **Counted, not logged per frame:** messages finished, messages
  dropped (a gap in the sequence, N_Cr, a new First Frame on the same
  ID, a Single Frame cutting one short) and frames no message could use
  (a bad PCI, a Consecutive Frame with nothing begun). The session
  reports them as it reports unsplit replies (section 5).

### 2. Which controller may send

| The interface | A profile that only listens (`raw`, `mock`) | A profile that requests (`obd2`, `uds-generic`) |
|---|---|---|
| `vcan` | accepted | accepted: nothing reaches a car |
| a `can` controller in listen-only mode | accepted (ADR-0032) | `LOAD_FAILED`: "can0 is listen-only, and this profile sends requests", with the commands that bring it up in normal mode |
| a `can` controller in normal mode | `LOAD_FAILED`, as ADR-0032 has it: listening never needs a controller that can transmit | accepted, if the checks below pass |

A requesting session on a `can` controller also needs, at attach:

- **`?baud=` in the URI,** equal to the interface's rate (`BAD_URI`
  when it isn't given, or differs). The user states the rate the car
  runs at; an interface someone else brought up at a guessed rate isn't
  enough.
- **One-shot mode** (`CAN_CTRLMODE_ONE_SHOT`), so that a frame that
  fails goes out once and the controller never repeats it. It is the
  only way a user-space program meets section 4.2.1's "stop sending at
  once": without it the controller's own retransmissions continue
  until it goes error-passive or bus-off, whatever core does. A
  controller without it (`LOAD_FAILED`, with the commands) can't run a
  requesting session. A frame that loses arbitration also goes out
  only once in this mode; section 3 sends it again.
- **The controller error-active** (`IFLA_CAN_STATE`, read without
  privileges): error-warning, error-passive or bus-off at attach means
  something on the bus already disagrees with this controller, and
  `LOAD_FAILED` says so.

Core still never sets a rate or a mode. The refusals name the
commands:

```
ip link set can0 down
ip link set can0 up type can bitrate 500000 one-shot on
```

The docs give the safe order on a car: first bring the interface up
listen-only and run a listening session while the car's bus has
traffic (the engine running, or another scan tool polling through the
Y-splitter); frames that arrive with no error frames prove the rate
without transmitting anything. Then bring it up in normal mode.

### 3. The first request proves the rate

The worker sends its first request as section 4.2.1 has the tester
send one, and it watches for every sign of failure before it sends a
second:

- **The echo:** the socket receives its own frames
  (`CAN_RAW_RECV_OWN_MSGS`), marked `MSG_CONFIRM`. A driver that echoes
  on completion (`IFF_ECHO`, as `gs_usb` does) echoes a frame only once
  the controller has sent it and a node acknowledged it. The echo is
  the transmit confirmation; its absence after N_As (25 ms) is a frame
  that didn't go out.
- **Error frames and state:** any error frame (a bus error, the
  controller's warning, passive or bus-off report, a missing ACK)
  between the request and its reply.

Until one request has been echoed and answered at this rate, the
session is unproven, and:

| What the worker sees | What follows |
|---|---|
| the echo, then a reply | proven: the rotation starts |
| the echo, no reply within the engine's wait | proven (a node acknowledged it at this rate); the engine's timeout rules apply |
| no echo and no error within N_As | the frame lost arbitration or the bus was busy: sent again on the next pass, at most three times, then as the next row |
| any error frame or state change, or no echo three times running | **nothing more is sent this session**: `TRANSPORT_HARD_FAILURE`, one episode, the detail "can0 reported a CAN error on the first request at 500000 bit/s; nothing more was sent. Check the rate, the cable and the ignition, take the interface down, and connect again", and the session goes on listening |

Once proven, the link-health rules are J2534's: a request with no echo
is a failed write (`ecuio_engine_sent(false)`, `REQUEST_FAILED` while
reads stay healthy, the ignition turned off), and bus-off is a hard
failure. A session never sends again after a bus-off; the user
reconnects.

`vcan` is proven from the start: it has no rate.

### 4. The URI

```
socketcan://<interface>?profile=obd2&baud=500000[&filter=<rx id>][&txid=...][&poll=...][&probe=0|1][&identify=0|1][&diagnose=0|1][&pack=0|1]
```

With a profile that requests, `socketcan://` takes the request keys of
`j2534://` and `elm327://` (`ecuio_request_param`,
`core/src/request_setup.h`), with the same meaning, so a run sheet's
query means the same on any scheme. `filter` is then the request key's:
one receive ID that replaces the profile's channels; a mask or a list
is refused (`BAD_URI`). With a profile that only listens, `filter`
keeps ADR-0032's list of IDs and masks.

### 5. The worker

- One `CAN_RAW` socket, as ADR-0032's, with own frames received for
  the echo. The kernel's filter passes the channels' reply IDs and
  their request IDs (the echo), and error frames; the whole bus beside
  requests is v0.4.0's discovery.
- Each pass: take every frame waiting; each reply ID's frames go
  through the receiver, and a whole message reaches the callbacks and
  the engine as a J2534 or ELM327 message does (the reply ID, the
  service byte first, no PCI), so the defs, the recordings and replay
  see the same shapes on every transport. Flow Control frames go out
  in the same pass as the First Frame that asked for them, well inside
  25 ms; the echo of a request is matched to it and never dispatched.
  Then the receiver's timers, then the engine's due requests, each
  channel's as a Single Frame to its request ID.
- The engine's waits stay ADR-0015's (P2CAN's 50 ms plus 50 ms). The
  50 ms margin was set for an adapter; the benchmark (STATUS, v0.3.5)
  measures what SocketCAN needs.
- Nothing is allocated per frame (`CoreTest.WorkersAllocateNothingPerFrame`'s
  rule): the receiver's slots live in the transport.
- **Counts:** the receiver counts the messages it finishes and drops
  and the frames no message could use (section 1); they reach the user
  with the benchmark's report per session (STATUS, v0.3.5), not as a
  getter of their own. While section 2 requires one-shot mode, a
  session that sends always has it, so the session info has no flag
  for it.

### 6. The simulated ECU answers through ISO-TP

The fakes' ECU (`tests/fake_ecu.h`) already answers on `vcan0`
through the kernel's `can-isotp` (`tests/vcan_bus.h`'s `Ecu`, the
v0.3.4 tooling), so the probe, the rotation, packing, identification,
the trouble codes and a clear all run on CI's `linux-build` against it
with no car. The kernel's ISO-TP is also the independent peer core's
needs: a test of core's receiver against core's sender would miss a
mistake both halves share. Core's sender meets the kernel's receiver in
the conformance tests (its block size and STmin, set through
`CAN_ISOTP_RECV_FC`), and core's receiver the kernel's sender (each
length to 4095 bytes).

### 7. Tests

- **Unit** (`IsoTpTest`, Qt-free, every platform): each frame type's
  encoding; reassembly in order, interleaved across eight IDs, and with
  padding; the Flow Control frame (bytes, DLC 8, sent once per First
  Frame); a sequence gap, N_Cr, a new First Frame on a busy ID and a
  Single Frame cutting one short, each dropped and counted; the escape
  length refused; the sender's block size, STmin (the microsecond
  values, a reserved value), wait frames, overflow and N_Bs. The
  rate-proof table (section 3) as a pure function of what the worker
  saw.
- **Fuzz** (`fuzz_isotp`): arbitrary frames and times to the receiver;
  a whole message never exceeds what its First Frame said, every slot
  stays within its bounds, and two runs agree.
- **HARNESS on `vcan0`** (`SocketCanRequestTest`, in `test_vcan`): the
  requesting sessions of `Elm327SessionTest` and `J2534AdoptionTest`
  (support, identity, diagnostics, a clear behind its gate, packing),
  against the simulated ECU; conformance against `can-isotp` (a reply
  of each length to 4095 bytes; the peer's block size and STmin
  honored by the sender; a late Consecutive Frame); the refusals of
  section 2 over canned netlink messages, since `vcan` reports no
  controller.
- **BENCH**, a new item (#32), on the bench machine first: the USB2CAN
  through `usbipd-win` into WSL takes one-shot mode and reports it; with
  no other node on its bus, a frame yields no echo and an error frame,
  and nothing more goes out. Then a bus of two on the desk: the DB9
  cable into the Y-splitter, the VX in its other socket on USB power,
  one terminating resistor: the USB2CAN's frames echo once the VX
  acknowledges them, the VX's monitor (`AT MA`) hears them, and a
  listen-only USB2CAN hears the VX's requests. A wrong rate is tried
  there, where no car's bus can be harmed. Then the test car through
  the laptop's WSL, at the car's rate: listen-only first while the MX+
  or the VX polls through the Y-splitter (BENCH prep #19 and #26), then
  requests, the other adapter's reading of the same values beside them
  (BENCH prep #30).

## Alternatives considered

- **The kernel's `can-isotp` sockets in the transport.** The kernel
  does the timing (Flow Control in softirq, STmin to the microsecond)
  and the code is the kernel's, well used. But it's one socket per
  request and reply pair, opened only where the module loads; it hides
  the frames, so the echo and the error frames that prove the rate come
  from another socket; and the reassembly couldn't be shared with
  ELM327, tested on Windows or fuzzed in-tree. Core's needs are small
  (Single Frames out, segmented replies in, one Flow Control frame per
  reply), and Table 6's 25 ms is far longer than a worker that wakes on
  the socket takes. The kernel's stays as the tests' independent peer.
- **Normal mode without one-shot.** Every frame that loses arbitration
  would go out again by itself, so busy buses lose no requests. But a
  first request at a wrong rate would then be repeated by the
  controller until it went passive or bus-off, and core can't stop it.
  A request lost to arbitration costs one pass; a car's bus disturbed
  costs more.
- **The USB2CAN on Windows or Android directly.** Its USB protocol
  (`gs_usb`'s) could be spoken by core itself, over WinUSB on Windows
  or Android's USB host API through an app-fed stream (ADR-0028), with
  no Linux at all. That is a transport of its own, with this ISO-TP
  above it; SocketCAN comes first because the kernel's driver is
  already there, and the ISO-TP here is what such a transport would
  reuse.
- **A warning instead of refusing.** As ADR-0032 found for listening:
  the harm is to the car, and a warning in the activity log is read
  after it.
- **Core configures the interface when it has the privilege.** It
  could take the interface down on the first error, as section 4.2.1
  asks. But ADR-0032's rule stands: a reader that reconfigures a bus
  is the wrong default, and one that does it only sometimes, more so.

## Consequences

- **Good:**
  - Every profile runs on a native CAN interface, so a $30 adapter
    reads a car, with the kernel's time on every frame and no adapter
    between core and the bus.
  - The whole request path (the probe, diagnostics, clearing) runs on
    CI over real sockets, against an ECU that answers through ISO-TP.
  - One ISO-TP reassembly for ELM327 and SocketCAN, unit-tested and
    fuzzed.
  - The first request follows the standard's own procedure for a wrong
    rate, as far as a program without privileges can.
- **Bad:**
  - Core owns ISO-TP's timing, which adapters do for the other
    transports; a stalled worker (a loaded machine) can miss N_Br.
  - The user brings the interface up in normal mode with one-shot mode
    and the right rate, a step the other transports don't have; an
    adapter without one-shot mode can't request.
  - Even so, a normal-mode controller at a wrong rate disturbs the bus
    from the moment it comes up; the docs' listen-only check first is
    the only guard against that, and it's the user's step.
  - Requests lost to arbitration in one-shot mode on a busy bus cost
    refresh rate; the benchmark will say how much.

## Appendix: 2026-11-01, Flow Control after a functional request

A functional request (`?txid=0x7DF`, or `0x18DB33F1`) is answered by
each ECU on its physical response ID, and ISO 15765-4:2005 section
6.3.2.1 has the tester send a segmented reply's Flow Control on the
physical request ID paired with it ("shall only be used for physically
addressed FlowControl frames"): 0x7E0 for 0x7E8. The worker sent it on
the request's ID, 0x7DF, where the simulated ECU, like a real one,
never looks, so the reply never went on.
`isotp_flow_control_id(rx, tx)` (`core/src/isotp.h`) gives the ID, and
the J2534 transport's `FLOW_CONTROL` filter uses the same rule (the
test car through the VX:
[2026-11-01-vehicle](../bench-sessions/2026-11-01-vehicle.md)).
`IsoTpTest.AFunctionalRequestsFlowControlGoesToThePhysicalId`,
`SimBusTest.AFunctionalRequestsSegmentedReplyCompletes`.
