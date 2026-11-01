# ADR-0046: The simulated ECU as a product: `havne-cli sim` answers as the mock's engine ECU on a virtual CAN bus, through core's ISO-TP, with periodic faults

- **Status:** Accepted. The mock's car is built (sections 1 to 6); a
  recording as the ECU (section 7) is the next step.
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
refused, whatever its state.

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

Any other key is `BAD_URI`, naming the keys taken. `replay://` is
`NOT_IMPLEMENTED` until section 7.

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

### 7. Next: a recording as the ECU

`replay://<path>` as the sim's source, `tests/vcan_recording_ecu.h`'s
ECU promoted: each Service 01 request answered with the recording's
reply at its time since the start, the car's own bitmaps.

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

## Consequences

- **Good:**
  - Anyone on Linux runs Havne's SocketCAN requests end to end, the
    obd2 profile, diagnostics, identification and the inspection line,
    with no hardware; faults show what a bad bus does.
  - Core's ISO-TP sender, so far only unit-tested, carries real replies.
- **Bad:**
  - A new public header and its ABI for a testing tool.
  - Linux only: Windows and macOS have no virtual CAN bus a session can
    open.
