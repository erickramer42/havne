# ADR-0060: Reads in the extended session, one built-in step

- **Status:** Accepted; built in v0.7.1. Run against the fakes and the
  simulated ECU only, not on a car.
- **Date:** 2026-11-09.
- **Builds on:** [ADR-0015](0015-request-engine.md) (the request
  engine and its service allow-list, section 3);
  [ADR-0020](0020-uds-generic.md) (uds-generic, its DID scan);
  [ADR-0046](0046-simulated-ecu.md) (the simulated ECU).
- **Amends:** ADR-0015 section 3's "session control (0x10) ... stays out
  of scope".
- **Read against:** [ADR-0052](0052-plugin-framework.md) section 6 (the
  conversation tier, which this doesn't build) and
  [DISCLAIMER.md](../../DISCLAIMER.md).

## Context

Many modules answer some DIDs only in the extended diagnostic session
(ISO 14229-1 DiagnosticSessionControl 0x10, sub-function 03): in the
default session such a DID reads as `7F 22 31` (out of range) or
`7F 22 7F` (not in the active session), so uds-generic's scan counts it
refused and a sweep of a module's range misses it. The test car's
module sweeps found refusals that a scan in the extended session may
turn into readings, and the private research workspace asked for one
read-only scan in that session to tell the two apart.

ADR-0052 places session control in its conversation tier (tier 2): code
in a sandbox that drives sessions, security access and gateway routing,
built with v0.9.0's discovery capture. A scan in the extended session
needs none of that: one request per module before the reads, one to
keep it, one to leave it. Waiting for tier 2 means waiting for a WASM
runtime to read DIDs that one fixed step reads.

## Decision

### 1. The URI asks for it, and only uds-generic takes it

- `?session=03` enters the extended session; `?session=01`, or no key,
  reads in the default session as before: no session request is sent
  at all. Any other value is BAD_URI ("?session= needs 01 (the default
  session) or 03 (the extended one), not '02'").
- Only `profile=uds-generic` takes `?session=03` (BAD_URI otherwise:
  "?session=03 belongs to profile=uds-generic, the profile that reads
  DIDs"). obd2's services are the default session's.
- `?keepalive_ms=` (500 to 4500, default 2000) sets how long a channel
  goes without a request before tester present; it needs
  `?session=03`.
- Every requesting transport takes both keys: `j2534://`, `elm327://`,
  `socketcan://`. `mock://` and `replay://` ask nothing and refuse them
  as unknown keys. A scrubbed URI keeps both (they are numbers).

### 2. The step, in the request engine

`ecuio_engine_session(e, 0x03, keepalive_us)` turns it on; each channel
(each module the `did=` keys name) then runs:

- **Entry first.** The channel's one-shot sends `10 03` before anything
  else, the walk, identification, diagnostics, the scan and the
  rotation all waiting; `50 03` enters it. A `7F 10 78` extends the wait
  to P2* (5 s) as for any request.
- **A refusal reads on in default.** `7F 10 <nrc>` (0x12 for a module
  with no such session, 0x22 for conditions not correct, 0x11, or any
  code) leaves the channel in the default session with the code kept,
  and the session goes on: the connect never fails for it, and the
  other modules' sessions are their own. A refusal stands for the
  session. An unanswered `10 03` reads on in default too, and is asked
  again when the link recovers (`ecuio_engine_rescan`).
- **Tester present in quiet gaps.** ISO 14229-2's S3 server time is
  5 s: an ECU leaves the session after that long with no request. Any
  request resets it, so a busy rotation needs nothing more; a channel
  that has sent nothing for `keepalive_ms` sends `3E 00`. The
  suppress-reply bit (`3E 80`) isn't used: the reply `7E 00` keeps
  link health's judgment on every request and keeps the engine's one
  outstanding request per channel whole. Its cost is one reply per gap.
  The engine's next event includes the keepalive, so a worker never
  sleeps past it (a focus's minimum time between sweeps included).
- **A lost session is entered again.** In the session, a `7F <sid> 7E`
  or `7F <sid> 7F` to any other request means the ECU left it (a reset,
  its S3 run out): the channel sends `10 03` again and the request is
  asked again, not dropped (a one-shot goes back unasked; a rotation
  request stays in turn). The same code straight after an entry, before
  any other positive reply, is the request's own answer and drops it as
  today (`ECUIO_DROP_ACCESS`), so a DID that needs more than the
  session can never loop. Tester present unanswered, a channel quiet
  for S3 (a stalled worker), and a link recovery each enter it again
  too.
- **A bound on re-entry.** A module that takes `10 03` and then refuses
  a read every few requests would otherwise be asked to enter again
  every few requests. Its `ECUIO_SESSION_LOSSES_MAX` (3) losses to 7E
  or 7F within `ECUIO_SESSION_LOSS_WINDOW_MS` (60 s) abandon the
  session: the module reads in the default session from then on, its
  state `ABANDONED` with the last code, and the reply that hit the bound
  takes the usual rules. Losses to silence, S3 or a link recovery don't
  count toward it: they are the link's, not the module's, and each is
  bounded by the link's own backoff. A module that entered the session
  still gets `10 01` at the close.
- **A close leaves it.** Each channel that entered the session sends
  `10 01` after its worker's loop ends: J2534 and SocketCAN write it and
  leave the reply to the disconnect; ELM327 waits for its prompt, 500 ms
  at most, since the adapter takes no command before the last one's.

### 3. The allow-list (amends ADR-0015 section 3)

`ecuio_engine_session_request_allowed` names the only session requests
the engine sends: `10 03`, `10 01` and `3E 00`. The session's one-shot
is built only through it, in the engine, whatever reached the engine's
state: a channel told to enter session 02 by any path sends nothing and
reads in default (`SessionControlTest.TheEngineNeverSendsASessionItsListRefuses`).
`ecuio_engine_service_allowed`, the table's list, is unchanged: 0x10
and 0x3E never enter a rotation, a def or a one-shot list. A
`static_assert` pins both: never 10 02 (programming), 10 04 or any
other sub-function, 3E 80, 0x27, 0x2E, 0x2F, 0x31, 0x14 or 0x11. Writes
and security access stay out of scope.

### 4. Relation to ADR-0052's tier 2

This is a built-in step, not the conversation tier: it is fixed in
core, takes no plugin code, and sends three requests the tier's ceiling
(ADR-0052 section 6) already allows. When tier 2 is built, a
conversation plugin that opens a session does so through the tier's
`begin`; the built-in step stays for `?session=03` with no plugin, and
a session that loads a conversation plugin for a module doesn't run the
built-in step on that module. Nothing here widens what tier 2 may send.

### 5. DISCLAIMER.md

The extended session is a diagnostic session for reads and adjustments;
on some modules it relaxes limits a write would need. Havne sends no
write, routine or security request in it, and returns each module to
the default session at the close. DISCLAIMER.md's "Current state" names
the session requests (`10 03`, `3E 00`, `10 01`) beside the reads, says
they are sent only on `?session=03`, and says a module may refuse the
session or, rarely, behave differently in it (some quiet a function
while a tester holds the session), so the first runs on a car are
parked.

### 6. The report: which session each module was read in

A diff of a scan against the default baseline needs each module's
session and refusal, so it can't be inferred from the counts.
Append-only, in `ecuio_channel_support_t`'s reserved room (152 bytes
kept; offsets static-asserted and in `test_c_abi.c`):

```c
/* reserved[0] at 136 becomes */
uint8_t session_asked;   /* 0 none (the default), 3 (?session=03) */
uint8_t session;         /* the session in force: 1 default, 3 extended */
uint8_t session_state;   /* ECUIO_SESSION_STATE_*: 0 NOT_ASKED, 1 ENTERING,
                            2 ENTERED, 3 REFUSED, 4 UNANSWERED,
                            5 ABANDONED */
uint8_t session_nrc;     /* REFUSED or ABANDONED: the code */
/* reserved[1] at 140 becomes */
uint16_t session_entries; /* 50 03s, the first and each re-entry */
uint16_t session_lost;    /* re-entries: a 7E or 7F, a timeout, S3, a
                             link recovery */
uint32_t reserved[2];     /* at 144 */
```

Both counts saturate at 65535 and never wrap. The size stays 152, and
`test_c_abi.c` pins each new field's offset in strict C.

with `ECUIO_SUPPORT_EXTENDED` (0x20) set while the channel reads in the
extended session. The generation moves with each change, so the
publish that support already does carries it.

- **havne-cli (schema 1, grows):** a `session` line per module when its
  state, code or counts change, `{"type":"session","ecu":"0x7E8","asked":"03",
  "session":"03","state":"entered","nrc":null,"entries":1,"lost":0}`
  (`state` in words: `entering`, `entered`, `refused`, `unanswered`,
  `abandoned`; `nrc` a hex byte or null), and the `scan` line gains
  `"session":"03"` or `"01"` as its last key, the session in force when
  it was written. A session with no `?session=` writes neither, so every
  existing line and golden stays byte for byte: a new line type and a
  new key, no key changed.
- **Python:** the regenerated `_ecuio.py` carries the fields;
  `Session.channel_support()` returns them as it returns the struct
  today, and `havne` gains the `ECUIO_SESSION_STATE_*` names.
- **The app:** the URI passes through Settings' field as any key does.
  The activity log gives each module's session a line of its own when
  its state, code or counts change (the CLI's `session` rule): "ECU
  0x7E8: reading in the extended session (50 03)", "ECU 0x7E9: refused
  the extended session (subFunctionNotSupported, 7F 10 12): reading in
  the default session", and one each for a re-entry, no answer and an
  abandoned session. Each support line ends with the session in force
  ("; extended session" or "; default session"), and Disconnect names
  each module that entered as "returned to the default session (10 01)".
  A recording keeps every one of them as a note. Without `?session=`
  none of them is written.

### 7. The simulated ECU and the fakes

The simulated ECU's engine node has an extended session and answers
DIDs D200 to D203 only in it (`7F 22 31` in default), leaves it after
S3 with no request, answers `3E 00`, and refuses any other
sub-function `7F 10 12`; its second module (`modules=2`) and a
profile's nodes have none (`7F 10 12`). The fakes' ECU
(`tests/fake_ecu.h`, which the fake J2534 DLL, the fake ELM327 and the
vcan ECU answer through) does the same, its modules refusing with a
knob's code (`ECUIO_FAKE_SESSION_NRC`, default 0x12).

## Alternatives considered

- **Fail the connect when a module refuses `10 03`:** loses a
  multi-module scan to one module that has no extended session, the
  usual case for a body module. The refusal is named instead.
- **Tester present with the suppress bit:** halves the gap's traffic
  but leaves a request with no reply, which the engine's one-outstanding
  rule and link health would have to special-case; a gap's single reply
  costs nothing worth that.
- **Wait for the conversation tier:** a WASM runtime and a plugin to
  send three fixed requests; the reads are wanted now.

## Consequences

- uds-generic reads DIDs the default session hides, read-only, on every
  requesting transport, and the CLI and Python see which session each
  module was read in.
- The engine sends a service outside its table for the first time
  besides 0x04: a second list, with its own static_assert, and a test
  that drives the engine past its setter.
- A car run sends `10 03` parked only, until a parked run shows no
  module refusing or misbehaving.
- A module that answers `7F 22 7F` to a DID it holds back for more than
  the session costs one re-entry before the DID drops.

## Appendix: a DID refused alone in the session (2026-11-10)

This amends section 2's "A lost session is entered again" and "A bound
on re-entry". A module may keep a DID to another session (programming,
or a manufacturer's own) and answer it `7F 22 7F` while its other DIDs
read in the extended session. Section 2 took that refusal, once other
reads had answered, as a lost session each time it came round, so one
such DID abandoned its whole module's session within three sweeps.

- **The refused request goes first once back in.** After a re-entry,
  the request the 7E or 7F answered is asked again before the channel's
  next one: a one-shot as before, and now a rotation request too (its
  class's cursor goes back to it; a focus's sweep counts it once).
- **Refused again before it has answered, it is its own answer.** Each
  channel keeps the request whose refusal made the last re-entry. The
  same code from that request again, before it has answered positively,
  is the request's answer whatever else answered between: with the
  probe on it drops (`ECUIO_DROP_ACCESS`), and the loss its first refusal
  counted is given back once, so `session_lost` and the window's count
  each fall by one. Its first refusal still costs one `10 03`, as the
  Consequences say.
- **A loss stays a loss.** A 7E or 7F from any other request, or from
  that request after it has answered, is a lost session as section 2
  says. A module whose reads all refuse once it leaves the session, and
  answer again once it is entered, still meets the bound and is
  abandoned.
- **7E** is judged the same way, but drops nothing (ADR-0015 section 4
  drops 0x7F, not 0x7E): a request refused 7E alone stays in turn, its
  own answer each time, with no loss counted.
- **With the probe off** nothing drops, and the remembered request is
  one per channel: one DID kept to another session is its own answer in
  every sweep after the first, but two or more take turns as the one
  remembered, and each such DID then costs one `10 03` per sweep. None
  of them counts toward the bound.

`SessionControlTest.ADidRefusedInTheSessionIsDroppedAlone`,
`.SeveralHeldDidsEachDropAlone`,
`.ADidRefusedInTheSessionWithTheProbeOffCostsOneEntry`,
`.AModuleWhoseReadsAllRefuseStillFallsBack` and
`.AReadAskedAgainAfterAnEntryCountsOnceTowardTheSweep` pin it, at the
unit tier only: no car has answered `7F 22 7F` in the session yet.

## Appendix: the close's line (2026-11-10)

Section 6's Disconnect line for a module that entered reads "returning to the
default session (10 01)", written as the close starts: the `10 01` goes out
on core's teardown thread, which the bridge can't see, so the line says what
the close is about to do and not that it did.
