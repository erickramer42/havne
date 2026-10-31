# ADR-0037: Reading trouble codes, readiness and the freeze frame, asked at connect

- **Status:** Accepted. Built on 2026-10-30: core, the J2534, ELM327
  and mock transports, the app's page and log line, and `havne-cli`'s
  line; not yet run on a car (BENCH prep #28).
- **Date:** 2026-10-30.
- **Builds on:** [ADR-0015](0015-request-engine.md) §1 (one-shot
  requests through the engine's pacing) and §3 (the services the engine
  may send, which this record amends); [ADR-0021](0021-vehicle-identification.md)
  (a fixed list asked once per channel, the text made in core, a report
  per channel through the C ABI, one line in the activity log), whose
  shape this record repeats; [ADR-0016](0016-signal-definition-layout.md)
  (the `obd2` set's defs, which decode the freeze frame).
- **Amends:** [ADR-0015](0015-request-engine.md) §3, with a dated note:
  the allowed services grow by four reads.

## Context

Havne reads a car's live values and its identity, but not what a scan
tool is first plugged in for: whether the check-engine light (the MIL)
is on, which trouble codes are stored, and whether the emissions
monitors have run. SAE J1979 carries all of it in read-only services:

| Request | Reply | What it holds |
|---|---|---|
| `01 01` | `41 01 A B C D` | A7 the MIL, A6–A0 the stored-code count; B3 the engine type (compression or spark); B, C, D which monitors exist and which are incomplete |
| `03` | `43 n DTC…` | Stored (confirmed) codes, `n` of them, two bytes each |
| `07` | `47 n DTC…` | Pending codes: seen on this or the last drive cycle, not yet confirmed |
| `0A` | `4A n DTC…` | Permanent codes: only the ECU's own monitor clears them (cars from 2010) |
| `02 PID 00` | `42 PID 00 data` | The freeze frame: Service 01's PIDs as they were when a code was stored; PID 02 names that code |

On ISO 15765-4 the count byte follows the service byte, and three codes
are already eight bytes: a multi-frame reply, which the adapter
reassembles ([ADR-0006](0006-frame-abi-v0.3.md), appendix). Service 04
clears all of it, and is the first request that changes the car; it is
not part of this record.

The engine already has what identification uses: the one-shot slot,
taking turns with the rotation, and a per-channel report beside the
support report. What's missing is the list, the reply shapes with no
key (`43` echoes nothing of its request), where the answers go, and how
they're shown.

## Decision

### 1. Four more read services; 04 stays refused

`ecuio_engine_service_allowed` takes 0x02, 0x03, 0x07 and 0x0A beside
0x01, 0x09 and 0x22. All are reads. 0x04 (clear), and every service
that changes a session, unlocks or writes, stays refused, so the rule
of [ADR-0015](0015-request-engine.md) §3 holds for whatever enrolls a
request: the engine sends no service that changes the car.

### 2. One fixed list, asked once per channel, after identification

Every channel that requests also asks, once each and in this order:
`01 01` (readiness), `03`, `07`, `0A`, then the freeze frame: `02 00
00` (the PIDs it holds), `02 02 00` (the code that stored it), and `02
<pid> 00` for each PID that is both in that bitmap and in the `obd2`
set. The items go through the one-shot slot after identification's,
taking turns with the rotation like it, so live values keep their pace.

- `03`, `07` and `0A` are one-byte requests: a positive reply is
  matched by its service byte alone (`43`, `47`, `4A`); `42` by its PID
  and frame number. Negative and pending replies as for every request.
- A negative reply records its code (a car before 2010 answers `7F 0A
  11`); a timeout leaves the item unanswered, asked again when the
  link returns to ok, as identification's are.
- `ecuio_session_read_diagnostics(session)` asks the whole list again
  on every channel ("Read again"); while a read is under way it does
  nothing.
- `?diagnose=0` turns it off for a session, as `?identify=0` does
  identification; `?poll=` and a passive session ask nothing.

### 3. The report is raw codes; the words are core's

`ecuio_channel_diagnostics_t` (`core.h`, fixed size, pinned by static
asserts and `test_c_abi.c`): `flags` (`COMPLETE`, `MIL_ON`,
`COMPRESSION`, `FREEZE_FRAME`), `rx_id`, `generation` (bumped as each
item settles), three code lists (stored, pending, permanent: each its
outcome, negative code, the count the ECU reported, the count held, and
up to `ECUIO_DIAG_MAX_DTCS` (32) codes as their two raw bytes), up to
`ECUIO_DIAG_MAX_MONITORS` (12) monitors (an ID, available, complete),
the freeze frame's code, and up to `ECUIO_DIAG_MAX_FREEZE` (16)
freeze-frame values (a def ID and its value). Reserved words for growth.
`ecuio_session_get_channel_diagnostics` copies it under the identity
report's rules.

- `ecuio_dtc_format(code, out)` names a code: A7–A6 the letter (P, C,
  B, U), then four hex digits ("P0301"). `ecuio_monitor_name(id,
  compression)` names a monitor; C and D's bits mean different monitors
  on spark and compression engines, hence the flag. The UI receives
  these and parses no bytes ([ADR-0001](0001-qt-free-core-c-abi.md)).
- A count past the cap shows as `reported > held`. 32 is far past what
  an ECU stores in practice; the count byte allows 255.
- A description beside a code ("cylinder 1 misfire") is not here. The
  standard texts are SAE J2012's; where they come from is its own
  decision, and it would arrive as a separate call, changing neither
  the report nor `ecuio_dtc_format`.

### 4. The freeze frame decodes through the `obd2` set

A freeze-frame reply carries a Service 01 PID's bytes after one more
byte (the frame number). Core rewrites `42 PID 00 data` as `41 PID
data` and decodes it with the `obd2` set's def for that PID, so each
formula exists once and the values arrive in the set's units, through
the same display conversion as Live ([ADR-0022](0022-display-units.md)).
A PID the set doesn't define isn't asked.

### 5. One line in the activity log, once complete

The bridge polls the report on its coalesce tick and logs one line per
ECU when it reads complete, and again after "Read again". Incomplete
monitors are named by their short names (`tests/fixtures/obd2/monitors.csv`),
as in "readiness 6 of 8 complete (EVAP, O2 heater incomplete)". Through
the fake:

    ECU 0x7E8 diagnostics: MIL on; stored P0301 P0420 U0100; pending P0171 C0035 B1001; permanent P0420; readiness 8 of 8 complete; freeze frame for P0301

The recorder routes it into `events.log`, so a recording keeps the
car's codes as they were when it was made.

### 6. The fakes, and the simulated car

The fake ECU (`tests/fake_ecu.h`), which the fake J2534 DLL and the
fake ELM327 both answer from, holds three stored codes (eight bytes: a
First Frame), three pending ones of three letters, one permanent, the
MIL on and a freeze frame at 3000 rpm, unlike its live values, so a
test can't pass by reading the wrong service.

`mock://` has no engine and no channels, so the report call would
refuse it (`INVALID_ARGUMENT`). It gets one channel, `0x7E8`, whose
diagnostics report is the fake ECU's, published complete at start,
whatever the profile; its support and identity reports stay empty, as
they are today. The freeze frame's values come from the same lists. So
the app, the README's screenshots and `havne-cli`'s goldens show
diagnostics on every platform with no hardware. What else reads the
mock's channel count (the bridge's per-channel polls, `havne-cli`) is
checked against an empty report.

> **Amended 2026-10-30:** the mock's report is published at attach, so
> it is complete when `ecuio_session_create` returns, before the
> session starts; a Read again is taken on the worker's next tick and
> read at once. `?diagnose=0` gives the mock no report. Its answers
> are `core/src/mock_ecu.h`, which mirrors `tests/fake_ecu.h` byte for
> byte.

### 7. ELM327's response count

An ELM327 request may carry a digit, which saves the adapter's `AT ST`
wait ([ADR-0026](0026-elm327-transport.md)). For OBD requests it counts
frames (lines), not messages: the datasheet (ELM327DSH v1.4b) gives
the VIN as `09 02 5` for its five lines, and contrasts it with `AT MP`,
whose digit alone counts whole messages. A digit too small ends the
read mid-message, so it's safe only where the reply's frame count is
known before asking.

`elm327_response_count` already gives a digit only to a two-byte
Service 01 request sent to one ECU whose PID is in the one-frame set
(the support bitmaps, and the session set's PIDs whose defs end within
a Single Frame), so the new items need no change: `03`, `07`, `0A` and
every `02` go out with none (a code list's length, and so its frame
count, is what's being asked), and `01 01` gets a `1` only under a set
that defines PID 01. A digit that does cut a reply is already caught:
the transport takes that PID out of the one-frame set and logs it.
Each undigited item costs one `AT ST` wait (100 ms at the setup's `AT
ST 19`), about two seconds for the whole list at its longest, spread
across the rotation's turns. The fake ELM327 counts lines, as the
datasheet does.

## Alternatives considered

- **Codes as text defs in the sets, shown as Live rows.** The decode
  path exists, but the rows would refresh never and the list's length
  varies; the codes are a fact about the car, like its identity, not a
  signal.
- **Ask only on request.** Saves a handful of requests a connect, but a
  MIL and its codes are what a user plugs in to see, and they'd be
  missing from the recording's `events.log` unless asked.
- **Text in the report instead of raw codes.** Five characters a code
  is cheap, but raw bytes keep the report exact (and a later
  description call keyed by the code), while `ecuio_dtc_format` keeps
  the rule in core.
- **Decode the freeze frame with its own table.** Duplicates every
  Service 01 formula and lets the two drift; `Obd2TableTest` checks one
  set against the J1979 table, not two.
- **Reading and clearing in one record.** Clearing changes the car, the
  README's "read-only" banner and DISCLAIMER.md's promise, and needs a
  gate (codes saved first, the engine off, a confirmation); reading
  needs none of it and ships on its own.

## Consequences

- A connect sends four to about twenty more requests per channel,
  interleaved with the rotation: under a second against a quick ECU.
  `?diagnose=0` removes them; tests that pin write order add it beside
  `?identify=0` and `?probe=0`.
- DISCLAIMER.md's list of services grows by the four reads, and still
  says Havne has no capability to clear codes.
- The engine's channel grows by the list and the report (about 0.5 KB a
  channel).
- A `7F 03 11` (Service 03 unsupported) settles that item only: no
  rotation request uses these services, so nothing is dropped.
- Clearing (Service 04) gets its own record, which amends §1, and its
  own gate.
