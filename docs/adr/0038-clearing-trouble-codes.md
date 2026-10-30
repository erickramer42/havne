# ADR-0038: Clearing trouble codes, each ECU by its own ID, behind a gate core checks

- **Status:** Accepted. Not yet built.
- **Date:** 2026-10-30.
- **Builds on:** [ADR-0037](0037-reading-trouble-codes.md) (the report
  a clear needs first, its generation, Read again's path through the
  worker); [ADR-0015](0015-request-engine.md) §1 (the one-shot slot)
  and §3 (the services the engine may send).
- **Amends:** [ADR-0015](0015-request-engine.md) §3 and
  [ADR-0037](0037-reading-trouble-codes.md) §1, with dated notes:
  Service 04 is sent, by one path only.

## Context

SAE J1979 Service 04 is the first request Havne would send that changes
the car. On each ECU that obeys it, it erases the stored and pending
codes, the freeze frame and the MIL, sets every readiness monitor back
to incomplete (so the car fails an emissions inspection until it has
been driven enough for them to run), and zeroes the distance and time
since the codes were cleared (PIDs 31 and 4E). Permanent codes stay:
only the ECU's own monitor clears one. An ECU may refuse it with the
engine running, `7F 04 22` (conditions not correct); J1979 lets it.

Everything Havne sends today is a read, and three places say so to a
user: the README's "Experimental, read-only" banner, DISCLAIMER.md
("It has **no** capability to clear codes"), and the Diagnostics page
("Havne reads codes and can't clear them"). Code holds the rule too:
`ecuio_engine_service_allowed` refuses 04, and a `static_assert` checks
that no one-shot list ever holds it.

What's erased can't be read back. So the clear gets a gate, which
STATUS set out before any code: the codes and the freeze frame read and
saved first, the ignition on with the engine off, and a confirmation
that says what clearing resets.

## Decision

### 1. One path sends 04; no list ever holds it

`ecuio_engine_service_allowed` stays reads only, and the `static_assert`
that keeps 04 out of every one-shot list stays. A channel's clear is a
small state of its own in the engine, which builds the one byte `04`
itself after its gate (§2) passes, through the same one-shot slot as
identification and diagnostics, so it takes its turn with the rotation.
Nothing else can enrol 04: not a def, not a URI key, not a list. A
second `static_assert` checks that the only request the clear state
builds is `04`.

### 2. The gate: the report saved, the engine off, the user's word

- **The report the caller saved.** `ecuio_session_clear_diagnostics(
  session, channel, generation)` clears one ECU, named by its channel,
  at the report generation the caller saved. Core refuses before
  anything is sent when that channel's report isn't complete, or its
  generation isn't the one given: what the caller saved is then not
  what the ECU holds. A new status, `ECUIO_ERR_REPORT_CHANGED` (17),
  says so; a channel past the count is `INVALID_ARGUMENT`; after close,
  `SESSION_CLOSED`. The worker checks the generation once more when it
  takes the request, since a re-read could settle between the call and
  its turn.
- **The engine off.** On the worker, the clear asks `01 0C` (engine
  speed) first. Only an answer of 0 rpm sends `04`. Any speed above it
  ends the clear unsent, as "engine running" with the speed it read;
  no answer, or a negative one, ends it unsent as "no engine speed".
  The ignition being on is what the ECU's answering shows.
- **The user's word** is the app's (§5): core can't see a confirmation,
  and doesn't pretend to through a flag.

### 3. Each ECU by its own ID

A clear goes to one ECU's physical request ID, the channel that read
it, never the functional `0x7DF`. What's cleared is then exactly what
the report showed and the saved file holds; a module that never
answered the read is never cleared. The app clears each ECU in its
report, one call each.

### 4. The result in the report, then a read

The diagnostics report grows into its reserved room: the clear's
outcome (`ECUIO_CLEAR_NONE`, `_CLEARED`, `_REFUSED` with its code,
`_UNANSWERED`, `_ENGINE_RUNNING` with the engine speed it read,
`_NO_ENGINE_SPEED`, `_REPORT_CHANGED`) and its code and speed, and a
flag, `ECUIO_DIAG_CLEARING`, while one runs. After a `44`, or no answer
to `04` (which may still have cleared), the channel reads its
diagnostics again at once (ADR-0037 §2's list), so the report shows
what the ECU holds after the clear, at a new generation; a refusal
changed nothing, so nothing is read. The clear's outcome stays until
the next clear, a Read again included. The ABI check's growth rule
accepts fields that take reserved room at the same size.

### 5. The app: saved to a file of its own, then confirmed

- **Saved first, always.** Before any call, the bridge writes the
  report as it stands, every ECU's, to a file of its own in the
  recordings folder, `diagnostics-<YYYYMMDD-HHMMSS>[ <vehicle>].jsonl`,
  whether or not Record sessions is on: one `diagnostics` line per ECU
  in `havne-cli`'s form ([ADR-0031](0031-command-line-client.md)'s
  appendix of 2026-10-30), after a line with the time and the vehicle's
  label. A write that fails stops the clear. The activity log names the
  file, never its folder's path. A recording, when on, keeps the lines
  in its `events.log` as well.
- **Confirmed in place.** The Diagnostics page's "Clear codes…" button,
  enabled once a read has completed while connected, opens a
  confirmation that says what clearing erases (the list in Context;
  permanent codes stay), that the engine must be off with the ignition
  on, and the saved file's name; Clear and Cancel. Cancel sends
  nothing.
- **Each ECU's outcome** is a line in the activity log: "ECU 0x7E8
  clear: cleared", "refused (7F 04 22)", "no answer", "not sent: engine
  running (812 rpm)", "not sent: no engine speed", "not sent: the
  report changed".
  The read that follows logs its own line.

### 6. The transports, and the simulated car

- **J2534 and ELM327** take a clear as they take Read again: an atomic
  request per channel with its generation, taken on the worker's turn.
  `04` is one byte (5 bytes with the ID through J2534); its `44` is
  matched by service, as `43` is. ELM327 sends it with no count digit.
- **The simulated car** (`mock://`) answers `01 0C` from its drive
  cycle's engine speed, and clears its report the way the fake ECU
  does: codes and freeze frame gone, P0420 permanent, the readiness
  monitors of byte C incomplete. A new connect starts uncleared.
- **An engine-off phase** in its drive cycle, so a clear can be shown:
  every loop starts with the key on and the engine off for 15 s (0 rpm,
  0 km/h, no airflow, the temperatures soaking), then a start, cold in
  the first loop and warm after. In the first loop that's the first 15
  s after connect; in later loops it follows the cool-down idle. The
  drive cycle's tests take the phase in: the engine-speed rules (never
  under 1000 in gear, never holding still) skip it, and the heat model
  burns nothing in it.

### 7. Out of this record

- **`havne-cli clear`.** The app's confirmation is the third step of
  the gate; a script needs its own answer to who confirms. On the
  roadmap.
- **UDS 0x14** (clear diagnostic information) and enhanced codes.
- **A car.** The fakes and the simulated car carry this branch. A car's
  clear waits for one with a code worth clearing and a drive to bring
  its monitors back; it'll be a BENCH prep item.

## Alternatives considered

- **The user's word alone for the engine.** Simpler, and J1979 lets an
  ECU refuse anyway; but not every ECU refuses with the engine running,
  and `01 0C` is one request.
- **A functional clear on `0x7DF`**, as many scan tools send. It
  reaches every module, including ones that never answered the read,
  whose codes would be erased unsaved.
- **The report saved only into a recording.** A recording may be off,
  and requiring it to clear couples two settings; a file of its own
  always exists.
- **Core takes the user's confirmation as a flag.** Anything can pass a
  flag; the confirmation belongs where the user is.
- **04 as a one-shot list item.** The list path is shared with reads,
  and the `static_assert` that keeps 04 out of it is the property worth
  keeping.

## Consequences

- Havne is no longer read-only. The README's banner, DISCLAIMER.md
  (what clearing does, its inspection consequence, the list of
  services), security-assurance's request row, CONTRIBUTING's read-only
  rule and the page's status line change with the branch.
- `core.h` grows by one function, one status (17) and the clear's
  fields in reserved room; `test_c_abi` pins them.
- The drive cycle's loop is 15 s longer: the README GIF's recipe (loop
  5's times in the `run-havne` skill) and any test that pins a phase's
  time move with it.
- A clear costs one `01 0C`, one `04` and the diagnostics list again
  per ECU.
