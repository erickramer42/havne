# ADR-0046: The simulated ECU as a product: `havne-cli sim` answers as the mock's engine ECU or a recording's on a virtual CAN bus, through core's ISO-TP, with periodic faults

- **Status:** Accepted. Built: the mock's car (sections 1 to 6), a
  recording as the ECU (section 7) and the desk bus (the appendix of
  2026-11-03).
- **Date:** 2026-11-01.
- **Builds on:** [ADR-0032](0032-socketcan-listening.md) and
  [ADR-0040](0040-socketcan-requests-isotp.md) (SocketCAN, requests
  through core's ISO-TP); [ADR-0037](0037-reading-trouble-codes.md)
  (the mock's diagnostics); [ADR-0041](0041-replay-fault-injection.md)
  section 4 (the recording as the ECU, its faults);
  [ADR-0031](0031-command-line-client.md) (havne-cli).

## Context

A requesting `socketcan://` session (ADR-0040) is tested against ECUs
that live in test code: `vcan_bus::Ecu` (the kernel's ISO-TP answering
from `tests/fake_ecu.h`) and `vcan_bus::RecordingEcu` (a recording,
framed by hand so it can break frames). Outside the test binaries
nothing answers on a virtual bus, so until SocketCAN's hardware is on
hand nobody can run Havne's SocketCAN requests end to end, and a user
can't try the obd2 profile, the diagnostics or the inspection line on
Linux with no car.

## Decision

### 1. A simulated ECU in core, behind `ecuio/sim.h`

```c
ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out);   /* answering from return */
ecuio_status_t ecuio_sim_get_stats(const ecuio_sim_t* sim,
                                   ecuio_sim_stats_t* out);
void           ecuio_sim_destroy(ecuio_sim_t* sim);
```

Core holds it because it owns ISO-TP (`core/src/isotp.h`, the sender
and the receiver) and the mock's car, and because havne-cli reaches
core only through public headers. The answers are a pure function
(`core/src/sim_ecu.h`), tested on every platform; the bus side
(`core/src/sim.cpp`) is Linux only, and elsewhere `ecuio_sim_create`
is `ECUIO_ERR_NO_TRANSPORT`, as a `socketcan://` session is.

### 2. Only on a virtual bus

The interface must be a `vcan` link and up (`LOAD_FAILED` otherwise,
with the reason). An ECU that answers 0x7DF and 0x7E0 on a car's bus
would talk over the car's own engine ECU, so a real controller is
refused, whatever its state, unless the caller says the bus is a
desk's with no car on it (the appendix "the desk bus").

### 3. The URI

```
mock://<label>[?values=drive|canned][&vin=<17 characters>][&faults=<fault>:<n>[,...]]
```

- **`values`:** `drive` (the default) answers Service 01 from the mock's
  drive cycle (`core/src/mock_drive_cycle.h`), stepped every 50 ms on the
  sim's own clock: load, coolant, engine speed, vehicle speed, intake
  air, MAF, throttle, run time since the engine started, wideband
  lambda, module voltage, ambient air and oil temperature, the values
  moving as the mock's do, and the bitmaps list exactly those. `canned`
  answers every linear J1979 PID with the fakes' one reading each
  (`core/src/sim_canned.h`, which mirrors `tests/fake_ecu.h` byte for
  byte, pinned by a test).
- **`vin`:** what Service 09 item 02 answers (17 characters of the VIN
  alphabet, upper case); a made-up VIN that decodes when none is given,
  so the identity line, its model year and the inspection line all show.
- **`faults`:** `silent`, `late`, `bad_sequence` and `stalled`, each
  every Nth reply (1 to 10000), as ADR-0041 section 4's ECU breaks
  them: single-frame and segmented replies counted apart, the bitmaps
  never broken, `bad_sequence` and `stalled` on segmented replies only.
  Periods, not chances, for the reason that section gives.

Any other key is `BAD_URI`, naming the keys taken. `replay://` takes
its own keys (section 7).

### 4. What it answers

As the engine ECU, 0x7E8, to 0x7DF and 0x7E0, 11-bit:

| Request | Answer |
|---|---|
| 01, one PID | its value, a bitmap, or PID 01's and 41's readiness; `7F 01 12` for one it lacks |
| 01, two to six PIDs | J1979's packed answer: each PID it has, in the order asked; none answers nothing |
| 02, 03, 07, 0A | the mock's diagnostics (`ECUIO_MOCK_ANSWERS`, the fakes' codes, monitors and freeze frame), and the PIDs that count since a clear (21, 30, 31, 4D, 4E) |
| 04 | clears: 44, then the cleared answers, until the sim ends |
| 09 | 00 (items 02 and 0A), 02 the VIN, 0A the ECU name; `7F 09 12` otherwise |
| 22 | `7F 22 31` |
| anything else | `7F <service> 11` |

Requests take core's ISO-TP receiver, so a request longer than a
Single Frame is taken too; replies go out through core's sender,
waiting on the tester's Flow Control.

### 5. `havne-cli sim`

`havne-cli sim <uri> --on <interface> [--seconds N]` writes `start`, a
`sim` line (source, values, VIN, interface, faults), a `sim_stats`
line every 10 s while its counts change, and `end` with the reason and
the counts: requests, replies, refusals, each fault, Flow Control that
never came and clears. It runs until Ctrl+C or `--seconds`. A failed
create is an `error` line and exit 3, as a failed connect is.

### 6. Each front end

`havne-cli` runs the sim. The app doesn't: on Linux it connects to the
sim like any other SocketCAN bus (`socketcan://vcan0?profile=obd2`),
which is what the sim is for; running one is a developer's or a
tester's step, done from a terminal next to the app.

### 7. A recording as the ECU

```
replay://<absolute path>[?loop=0|1][&vin=<17 characters>][&faults=<fault>:<n>[,...]]
```

The path is a recording's folder or its `frames.csv` (ADR-0017), read
as `replay://` reads it (`core/src/frames_csv.h`, shared) and absolute
for the same reason (`BAD_URI` otherwise, on every platform). Create
reads the whole file before it judges the interface, so a bad file is
reported first (`LOAD_FAILED`, the line named, never the path). It
keeps the engine ECU's (0x7E8) whole positive replies, each under the
request it answers (`core/src/sim_recording.h`, pure):

| Reply | Answers |
|---|---|
| `41 <PID>` (a packed one split first, `ecuio_obd2_split`) | `01 <PID>`, and its record in any pack asked |
| `42 <PID> <frame>` | `02 <PID> <frame>` |
| `43`, `47`, `4A` | `03`, `07`, `0A` |
| `49 <item>` | `09 <item>` |
| `62 <DID>` | `22 <DID>` |

with its time since the recording started (replay's session time).
A request gets the reply held for it at the simulated ECU's time since
create, counted in its own 50 ms ticks: the last at or before that
time, the first one before it, the last one after the recording ends
(held), or with `loop=1` the time taken round the recording's span
(`BAD_URI` for a recording whose replies share one time). A pack
(two to six PIDs) is put together from each PID's reply in the order
asked, so packing works against any recording, packed or not.

- **Bitmaps:** the car's own when the recording holds any (one it
  doesn't hold is refused: that car's chain is what it is), else
  worked out from the PIDs it holds, as the mock's are.
- **Refusals:** a request it holds no reply for is refused (`7F 01 12`,
  `7F 22 31`, `7F <service> 12` for 02, 03, 07, 09 and 0A), and the
  functional ones kept to itself as in section 4. A clear (`04`) is
  always refused, `7F 04 22`: a recording can't say what the car
  answers after one.
- **`vin`:** what `09 02` answers instead of the recording's.
- **Faults:** as section 3's, the same counters.

A recording made with the probe holds the car's bitmaps and its
Service 01 replies; identification and diagnostics answer only when
the session that made it read them. `tests/vcan_recording_ecu.h`
stays as test code: its log of every reply sent and its fate is the
oracle `SocketCanFaultTest` compares a session against, which the
product has no need to keep.

## Alternatives considered

- **The ECU in havne-cli:** the CLI includes only public headers, so it
  would need its own ISO-TP and its own copy of the mock's car.
- **The kernel's ISO-TP (`can-isotp`):** frames correctly, but can't
  break a frame on purpose, and needs a module some kernels lack (WSL's
  needs loading).
- **Any SocketCAN link:** a sim on a car's bus answers for the car's
  ECU; refusing a real controller costs nothing a tester needs.
- **The fakes' canned values only:** every linear PID, but none moves;
  the drive cycle is the default so a session against the sim looks
  like a car being driven.
- **A recording replayed frame for frame, as `play` puts it on a
  bus:** a session's requests would get the replies of another
  session's rotation, out of step with what it asks; keyed by request,
  each request gets an answer to itself.
- **A recording's clear obeyed, its later codes answered empty:** it
  would invent the car's state after a clear, which the recording
  never saw.

## Consequences

- **Good:**
  - Anyone on Linux runs Havne's SocketCAN requests end to end, the
    obd2 profile, diagnostics, identification and the inspection line,
    with no hardware; faults show what a bad bus does.
  - A recording from any transport (J2534, ELM327, SocketCAN) answers
    a live session as the car did, its own bitmaps and values, so a
    car's session runs again at the desk.
  - Core's ISO-TP sender, so far only unit-tested, carries real replies.
- **Bad:**
  - A new public header and its ABI for a testing tool.
  - Linux only: Windows and macOS have no virtual CAN bus a session can
    open.

## Appendix: 2026-11-03, the desk bus

A tester reading the simulated ECU over real wire, the VX through
J2534 on the desk bus of
[2026-11-02-socketcan-desk](../bench-sessions/2026-11-02-socketcan-desk.md),
needs the sim on a real controller, which section 2 refuses. The one
on hand, the Innomaker USB2CAN, offers no one-shot mode.

- **An option, said by the caller.**
  `ecuio_sim_create_with(uri, ifname, options, out)` takes
  `ECUIO_SIM_ALLOW_DESK_BUS`, and `havne-cli sim --desk-bus` passes it;
  `ecuio_sim_create` is the same call with no options, so section 2
  holds unless a caller says the bus is a desk's with no car on it. An
  option bit not defined is `INVALID_ARGUMENT`. The `sim` line carries
  `desk_bus`.
- **What a desk bus's controller must be:** up, able to transmit (not
  listen-only, and reporting its mode) and error-active, each refusal
  `LOAD_FAILED` with the `ip link` commands that change it
  (`socketcan_judge_sim`, `socketcan_sim_refusal` in
  `core/src/socketcan_wire.h`, pure). The sim answers at the rate the
  controller is up at, since it only ever replies, and with or without
  one-shot mode: without it, a reply nobody acknowledges is sent again
  until the interface goes down (ADR-0040's appendix of 2026-11-02),
  which on a desk bus disturbs nothing but the desk, and in the run
  this is for, the VX acknowledges every reply. `restart-ms` isn't
  checked, for the same reason.
- **Silent once another node answers.** On any bus the sim also hears
  0x7E8, its reply ID. A frame there from another node means an ECU
  answers already, so the sim sets `ECUIO_SIM_YIELDED` and from then on
  sends nothing, no reply and no Flow Control, until it is destroyed;
  it still counts the requests. A CAN_RAW socket isn't sent its own
  frames, so its replies never count. `havne-cli sim` ends with
  `yielded`, exit 1, and the stats and end lines carry `yielded`. This
  limits a mistake, not prevents one: on a car's bus, the sim answers
  until the car's ECU first does. The option is the guard.
- **Testing:** the verdicts and every refusal's words on every platform
  (`SimTest.ARealControllerAnswersOnlyOnADeskBusReadyToSend`,
  `.ADeskBusRefusalSaysWhatToChange`,
  `.AnOptionThatIsntOneIsRefusedBeforeTheUri`), the yield on `vcan0`
  (`SimBusTest.AnotherNodeOnTheReplyIdSilencesItForGood`). A real
  controller runs only at the desk, by its owner: the desk run sheet's
  Phase 3, BENCH prep #33.

Alternatives considered:

- **A URI key (`?desk=1`):** the URI says what answers (the mock's car
  or a recording), not where; a key would let a saved URI carry the
  permission to a car's bus.
- **One-shot mode required, as a requesting session has it:** the
  controller on hand lacks it, so the desk run couldn't happen, and on
  a desk bus a resent reply harms nothing.
- **Silent for a while, then answering again:** an ECU on the bus
  doesn't leave; a sim that resumes would talk over it again.
