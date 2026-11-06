# ADR-0046: The simulated ECU as a product: `havne-cli sim` answers as the mock's engine ECU or a recording's on a virtual CAN bus, through core's ISO-TP, with periodic faults

- **Status:** Accepted. Built: the mock's car (sections 1 to 6), a
  recording as the ECU (section 7), the desk bus (the appendix of
  2026-11-03), a second module and "response pending" (the appendix of
  2026-11-04), and DIDs from a signal set (the appendix of
  2026-11-05).
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
  controller runs only at the desk: the desk run sheet's Phase 3,
  BENCH prep #33.

Alternatives considered:

- **A URI key (`?desk=1`):** the URI says what answers (the mock's car
  or a recording), not where; a key would let a saved URI carry the
  permission to a car's bus.
- **One-shot mode required, as a requesting session has it:** the
  controller on hand lacks it, so the desk run couldn't happen, and on
  a desk bus a resent reply harms nothing.
- **Silent for a while, then answering again:** an ECU on the bus
  doesn't leave; a sim that resumes would talk over it again.

*Amended 2026-11-03 (the desk run):* on the desk bus
([2026-11-03-socketcan-desk-sim](../bench-sessions/2026-11-03-socketcan-desk-sim.md)),
the Innomaker USB2CAN, without one-shot mode, was refused without the
option and while listen-only, then answered the VX's `obd2` session
over J2534 in normal mode: identification (the VIN a segmented reply),
the diagnostics, the inspection line and the drive cycle's values,
packed, every reply acknowledged and none unflowed, `can0` error-active
with no error counted. One frame on 0x7E8 from another socket
ended it `yielded`. BENCH prep #33 is confirmed for that controller.

## Appendix: 2026-11-04, a second module and "response pending"

Two things a session against the sim couldn't meet: a car with more
than one ECU answering, which the test car is (an engine ECU on
0x7E0/0x7E8 and a second on 0x7E1/0x7E9) and which v0.5.0's own
signals on a second module need; and an ECU that answers
`7F <service> 78` before its reply, which BENCH prep #17 waits for.

- **`modules=2`, on `mock://`:** a second module, on 0x7E1, answering as
  0x7E9, besides the engine ECU. Section 3's URI says what answers, the
  simulated car, and a second module is part of that car, so it is a
  key there; the desk bus's appendix kept its option out of the URI
  because that option says *where* the sim may answer, which a saved
  URI mustn't carry. `modules=1` (the default) is the engine ECU alone,
  so every session, test and run sheet that names `mock://sim` meets
  the same car as before. `replay://` doesn't take the key: a recording
  keeps the engine ECU's replies (section 7), and a recording's other
  modules are a later step.
- **What the module answers:** Service 22 from three DIDs of its own,
  made up for the simulated car and no maker's (D100 a fluid
  temperature, A - 40, reading 90 deg C; D101 the gear, 3; D102 the
  module's name, `TCM-HavneSim`, a segmented reply); `7F 22 31` for a
  DID it lacks; `7F <service> 11` for every other service, Service 01
  included. To a functional request (0x7DF) it answers its DIDs and
  keeps its refusals to itself, as section 4's ECU does.
- **Each module its own ISO-TP:** each has its own receiver, sender and
  reply under way, so a functional request answered by both, or a
  segmented reply from each, never share a message's state; the
  tester's Flow Control for the module's reply comes on 0x7E1. A frame
  from another node on either reply ID silences both (the desk bus's
  appendix).
- **`pending:<n>`, a fault on both sources:** every Nth reply goes out
  as `7F <service> 78` at once and the reply itself 300 ms later, whole.
  300 ms is past the request engine's 100 ms wait and an ELM327's
  `AT ST` timeout (`ATST19`, 100 ms), well inside P2* (5 s), so only a
  tester that keeps waiting after the 78 gets the answer. Each module
  counts its replies apart, so a period is that module's own.
  `ecuio_sim_stats_t` gains `pending` in its reserved room (offset 80;
  the size stays 96), and `havne-cli sim`'s `sim_stats` and `end` lines
  a `pending` field.
- **Testing:** the module's answers and refusals, the key, and pending's
  period per module on every platform (`SimEcuTest.TheSecondModule*`,
  `.PendingIsEveryNthReplyAndEachModuleCountsItsOwn`); on `vcan0`, one
  `uds-generic` session asking both ECUs, the second module opt-in, a
  pending reply waited for (no request timing out, the slowest reply
  past 250 ms) and the module's pending replies arriving
  (`SimBusTest.OneSessionAsksBothModules`, `.TheSecondModuleIsOptIn`,
  `.AResponsePendingIsWaitedFor`, `.TheSecondModulesPendingRepliesArrive`).
  BENCH prep #17 itself, an ELM327 adapter keeping on listening after a
  78, is a desk run: the sim on the desk bus with `faults=pending:<n>`
  and the adapter's session against it.

Alternatives considered:

- **A command-line option (`--modules 2`):** the car's shape would live
  outside the URI that `sim`'s line reports, unlike its values and
  faults.
- **The second module always there:** every existing session would meet
  an ECU on 0x7E9 it never asked for, and the default car would no
  longer be the one the desk run confirmed.
- **Pending as a value of `late`:** a late reply and a pending one test
  different things (a wait that runs out, a wait that is extended), and
  the stats count them apart.

## Appendix: 2026-11-05, DIDs from a signal set

v0.5.0's own signals and plugins (ADR-0051, and the plugin framework
after it) describe a maker's DIDs as a signal set; checking one with no
car needs an ECU that answers those DIDs as the set reads them.

- **`profile=<name>`, on `mock://`:** a registered profile
  (`ecuio_profile_register`) whose set's Service 22 DIDs the simulated
  car answers. Like `modules=2`, it says what the car is, so it is a key
  of section 3's URI. It is looked up at create, after the URI and
  before the platform and the interface, so its refusals are the same
  on every platform: a name not registered is
  `ECUIO_ERR_UNKNOWN_PROFILE`. The set must stay valid while the sim
  runs, as for a session (profile.h's lifetime). Not with `modules=2`:
  the profile's channels are its modules.
- **The modules are the set's channels:** a channel on 0x7E0 to 0x7E8
  is the engine ECU, which answers the set's DIDs as well as everything
  section 4 lists; a set with no channels puts its DIDs there too, where
  a session's default channel asks. Each other channel is a module of
  its own, with its own ISO-TP as the second module's, answering the
  set's DIDs alone (`7F 22 31` for another, `7F <service> 11` for
  another service, a functional request's refusals unsaid). 11-bit IDs
  only, as the sim's socket hears; a 29-bit channel, one on 0x7DF, or
  one sharing an ID with the engine ECU's pair but not the pair itself
  is `ECUIO_ERR_INVALID_ARGUMENT`.
- **Each field inverted from its def:** every def keyed `62 <DID>` at
  offset 0 on a module's reply ID is a field of that DID's reply, and
  every field of one DID shares the reply. The raw value is (value -
  offset) / scale rounded to the nearest, written in the def's width,
  signedness, byte order, start byte and bit offset; a FLOAT is its
  binary32. A text or hex def's field holds its name. An enumeration's
  def (ADR-0051) is answered by its raw number, as an integer field.
- **The values:** raw 0 until given one, then the value
  `ecuio_sim_set_value(sim, signal_id, value)` set, from any thread and
  while the sim answers; a value the field can't hold, a text field, or
  a signal that isn't one of the set's DIDs is refused
  (`ECUIO_ERR_INVALID_ARGUMENT`) and changes nothing.
- **Testing:** every kind and byte order through the decoder, the
  field's range, the refusals, the modules from the channels and the
  key, on every platform (`SimDidsTest`,
  `SimTest.TheProfileIsLookedUpBeforeTheInterface`); on `vcan0`, a
  session on the same profile reading the given values from the engine
  ECU and from the set's own module, a segmented text reply among them,
  and a value given mid-session
  (`SimBusTest.ASessionReadsTheValuesTheProfilesDidsWereGiven`,
  `.ASimWithNoProfileTakesNoValue`).

Alternatives considered:

- **A set passed to a new create function:** no registry lookup, but a
  sim's car would then hold something its URI doesn't name, and
  `havne-cli sim` would need a second way to say it.
- **Values from a range (the field's midpoint):** no call to make, but a
  test could check no value it chose, and a plugin's value fixtures
  name theirs.
