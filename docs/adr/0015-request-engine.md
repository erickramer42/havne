# ADR-0015: Request engine — correlated, response-paced requests derived from signal definitions

- **Status:** Accepted. The values left open under "Open until the
  first vehicle session" are chosen in the appendix of 2026-10-23,
  from the measurements in the appendix of 2026-10-22. §1 to §4 are
  built; the probe's details as built are in the second appendix of
  2026-10-23.
- **Date:** 2026-10-21
- **Supersedes:** [ADR-0013](0013-protocol-profiles.md)
  §1's placement of request correlation in the profile, and §5's
  callback table as the plugin seam. Amends §3's probe semantics.
  Leaves §2 (static selection by URI), §4 (declared channels) and §6
  (ship order) as they are.

**Scope.** This record covers sending requests and classifying their
replies. Three neighboring decisions are made elsewhere:

- **Decoding on the worker thread**, with per-sample timestamps,
  decode-time filtering and computed signals, belongs to a separate
  record. That record waits for the measurements listed under "Open
  until the first vehicle session".
- **Text values from Service 09** (the VIN and calibration IDs) need
  a string path and a privacy rule; they get their own record. This
  one only carries the request.
- **The frame log's storage** is an implementation change inside the
  bridge, not a decision record.

## Context

The J2534 worker requests on a fixed clock. In scheduler mode it
writes one Mode-01 request per 100 ms cycle, then drains `ReadMsgs`
with a short first-read timeout; `?poll=` writes one request, reads
once, then sleeps. Three things follow:

- **Refresh is set by the clock, not the ECU.** Each of a set's N PIDs
  refreshes every N × 100 ms however fast the ECU answers, so growing
  the set toward the full SAE J1979 Service 01 table makes every
  signal slower.
- **Replies often land a cycle late.** ISO 15765-4 lets an ECU take up
  to 50 ms (P2) to answer, longer than the drain's first read waits.
  A late reply is read during the next request's drain. It still
  decodes, because decoding keys on the reply's own bytes, but nothing
  knows which request it answered.
- **Nothing correlates a reply with its request.** That is harmless
  for a fixed Mode-01 rotation, and it blocks everything after it.
  The PID-support probe has to know which bitmap request a reply
  answers. Manufacturer reads (UDS ReadDataByIdentifier, service 0x22)
  have to tell a positive reply from a negative one, and a "response
  pending" from either.

ADR-0013 put correlation in the profile (§1) and planned a native
callback table, `next_request` / `accept_response`, as the seam for
v0.5.0 manufacturer plugins (§5). The shipped profile slices went the
other way, and it worked: the `obd2` rotation is derived from data,
the 2-byte mux key `0x41 <PID>` on each def, and no profile code runs
on the worker thread. Nothing has needed the callback table yet, and
everything planned next (more Service 01 PIDs, Service 09 vehicle
information, Service 0x22 reads) is request-and-reply with fixed
shapes that data can describe.

## Decision

### 1. One request engine serves every request

The J2534 worker gains one request/response engine. It serves the set
rotation, `?poll=`, the probe, and later the vehicle-information read.
Passive mode is unchanged.

- **One outstanding request per channel.** A declared channel (an
  rx/tx pair, ADR-0013 §4) has at most one request in flight, as ISO
  15765 physical addressing expects. Channels progress independently,
  so a PCM and a TCM are served in parallel on the one J2534 channel.
- **Correlation.** A frame answers a channel's outstanding request
  when it arrives on that channel's rx ID and starts with either:
  - the positive response: the request's service ID + 0x40, followed
    by the request's key bytes (the PID or the DID); or
  - the negative response: `7F`, the request's service ID, and a
    response code.

  Frames that match a channel but no outstanding request are
  dispatched exactly as today; they don't complete a request.
- **Pacing.** A channel's next request goes out as soon as its
  outstanding one completes, or when its wait expires. The wait is the
  response timeout (P2, 50 ms) plus a margin for the adapter. A
  "response pending" negative reply (code 0x78) extends the wait to
  P2* (5 s). This replaces the fixed 100 ms cycle and the drain budget
  for both request modes.
- **Reply classes.** Every request ends as exactly one of: positive,
  negative (with its code), or timed out. "Pending" is a state on the
  way, not an outcome. Positive replies are dispatched and decoded as
  today. The class feeds the probe (§4) and link health:
  - a positive or negative reply, or a pending one, is the ECU
    answering, which sets `ECUIO_OK`;
  - consecutive timeouts count toward the quiet debounce, which keeps
    its role;
  - write failures, hard read failures and the backoff behave as they
    do now ([j2534.md](../j2534.md#session-lifecycle)).
- **`?poll=`** becomes a one-request rotation on the engine instead of
  its own worker branch.
- **One-shot requests.** The probe and the vehicle-information read
  send requests that aren't part of the rotation: each goes out once,
  through the same correlation and pacing, and its outcome goes back
  to whoever asked. The rotation resumes after them.

### 2. Requests are derived from the definitions, not from profile code

A def's mux key, extended to at most 3 bytes, describes the reply that
carries the def's value. The request that asks for that reply is
derived from it, so the def alone says what to request.

- **Enrolment rule.** A def whose mux key sits at payload offset 0 and
  starts with the positive-response ID of service 0x01 or 0x22 (0x41 or
  0x62) enrols one rotation request: the service ID − 0x40, followed by
  the rest of the key. The request goes to the channel whose rx ID is
  the def's `message_id`. Requests are deduplicated per channel, so
  several defs on one reply (an oxygen sensor's voltage and trim) cost
  one request. Service 09 never enrols; its values are read once, with
  one-shot requests (§1).

  | Reply carrying the value | Mux key | Request sent |
  |---|---|---|
  | Service 01, PID 0x0C | `41 0C` | `01 0C` |
  | Service 0x22, DID 0x1E1C | `62 1E 1C` | `22 1E 1C` |

- **One set can mix services.** A manufacturer set is the standard
  Service 01 defs plus the maker's DIDs, so choosing it doesn't lose
  RPM or coolant. A set-wide "request service" field would force one
  service per profile.
- **Decoding checks the whole key**, so a value only decodes from a
  positive reply with the right service byte. A key at offset 1 (the
  DID alone) wouldn't check that byte.
- **No struct change.** `mux_key` is already 32 bits. The decoder, the
  frame-match helper and the validator accept a key length of 3; keys
  of length 1 and 2 behave as before. `ecuio_signal_set_t` doesn't grow,
  so no set version has to be read, which core doesn't do today.
- **ADR-0013 §5's callback table is not built.** A manufacturer plugin
  is data: a signal set and its channel list, validated at
  registration. No plugin code runs on the worker thread, so a plugin
  can't deadlock it or break the teardown order, and §3 can be
  enforced on everything a plugin asks for. If a protocol ever needs
  logic that data can't express, a new record revisits this.

> **Amended 2026-10-22 ([ADR-0016](0016-signal-definition-layout.md)):**
> the 3-byte mux keys shipped with the new signal-definition layout,
> so the "No struct change" bullet no longer holds. The decoder, the
> frame gate and the validator take key lengths 1 to 3, and the set's
> first member is a `layout_version` that core checks before anything
> else. That also settles two remarks below: the Consequences line
> calling the layout unchanged, and the version check that the
> set-wide request service alternative says core has never made.
> Until the engine lands, the scheduler and request routing take only
> a 2-byte `41 <PID>` key at offset 0 (`ecuio_mode01_pid`), so a
> `62 <DID>` def decodes a reply but sends no request.

### 3. Read-only by construction

The engine sends services 0x01 (current data), 0x09 (vehicle
information) and 0x22 (ReadDataByIdentifier), and nothing else.

- No set can ask for anything else. The enrolment rule (§2) produces
  only 0x01 and 0x22 requests, and one-shot requests come from core
  itself (the probe and the vehicle-information read).
- The engine refuses to send any other service even if one reaches
  it, so the rule holds without trusting every caller, including a
  future enrolment path.
- Session control (0x10), security access (0x27), writes and routine
  control stay out of scope. They are v0.4 work, need their own
  record, and fall under DISCLAIMER.md's regulatory posture.

### 4. The probe runs on the engine, and only explicit answers prune

The post-connect probe (ADR-0013 §3) uses the engine before the
rotation starts.

- **Service 01:** the supported-PID bitmap chain, PID 0x00 → 0x20 →
  0x40 …, per channel.
- **Service 0x22:** one request per enrolled DID.
- **A request leaves the rotation only on an explicit "not
  supported"** from an ECU that answered something in the same pass:

  | Answer | Effect |
  |---|---|
  | Bitmap bit clear | That PID leaves the rotation |
  | Negative 0x31 (request out of range) | That DID leaves the rotation |
  | Negative 0x11 (service not supported) | Every request of that service to that ECU leaves |
  | Negative 0x33 or 0x7F (security, or wrong session) | Leaves, with a log line: this project doesn't do security access or session changes |
  | Negative 0x22 (conditions not correct) | Stays: the condition is transient |
  | Timeout | Stays: silence isn't an answer |

- **Re-probe on recovery.** When link health returns to `ECUIO_OK` from
  quiet or requests-failing, the probe runs again. A connect made with
  the ignition off answers nothing, and without a re-probe the session
  would never learn what the ECU supports.
- **Bounded.** The probe has a total time budget. Requests it didn't
  reach stay in the rotation, and probe failure degrades to the static
  set, as ADR-0013 §3 requires.
- **`?probe=0`** skips the probe. Like every query key, it gets
  rejection tests for its malformed forms.

## Open until the first vehicle session

Each value below is settled by a measurement; the measurements and the
chosen values go in the dated appendix.

| Question | What settles it |
|---|---|
| The wait's margin over P2, and whether P2 and P2* match the ISO 15765-4 text | Request-to-reply times from the session log's `OP_WRITE frame` / `OP_READ frame` stamps, per PID; the standard's text |
| Whether the ECUs send "response pending" at all | The session log |
| A minimum gap between requests on a channel | Needs the engine on a vehicle: the second vehicle session |
| The refresh policy once the set grows: rate classes (fast signals every cycle, slow ones rarely) or a curated `obd2` beside an `obd2-full` | Measured reply latency × the supported-PID count from the bitmaps |
| Whether to send several PIDs per Service 01 request | The standard's text and ECU behavior on a vehicle. Not part of this record; it needs a splitter for the combined reply, and would build on the engine |
| Whether decoding moves to the worker thread | Frames per second and GUI-thread cost measured with the engine running. It is the path to per-sample timestamps either way (built: [ADR-0024](0024-decode-on-the-worker.md)) |

The performance target the STATUS backlog asks for, sustained frames
per second and per-PID refresh at N PIDs, is recorded in the same
appendix, before and after the engine.

## Alternatives considered

- **Keep the fixed slot and shorten it.** A shorter cycle wastes time
  on a fast ECU, still misses a slow reply, and still can't tell
  which request a reply answers or handle "pending".
- **Native profile callbacks as the plugin seam (ADR-0013 §5).**
  Deferred, not rejected outright. Native code on the worker thread
  can deadlock it or break the teardown contract, and §3 couldn't be
  enforced on it. Everything planned through v0.5.0 is fixed-shape
  request-and-reply that data describes.
- **A set-wide request service** (a new `ecuio_signal_set_t` field). One
  service per profile; it grows a struct that's passed by pointer,
  which needs a version check core has never made; and it doesn't
  check the reply's service byte.
- **Prune on timeout.** At ignition off, or while an ECU wakes, it
  empties the rotation for the rest of the session.
- **Several PIDs per request as the foundation.** ECU support varies
  and the combined reply needs splitting. It can be added on top of
  the engine once measured.

## Consequences

- Refresh is limited by the ECU's response time instead of a fixed
  slot, and replies stop landing a cycle late.
- The rotation, `?poll=`, the probe, Service 09 and Service 0x22 share
  one mechanism, so `uds-generic` and later manufacturer sets are data
  work, not new worker code.
- Read-only becomes a property core enforces rather than a convention.
- The mux key's allowed length grows from 2 to 3. The struct layout is
  unchanged, and existing sets behave as before.
- The fake vendor DLL grows: configurable reply latency, pending and
  negative replies, Service 0x22, and replies addressed to the
  request's tx ID + 8 rather than the last registered filter, which is
  wrong once a set declares two channels. Its canned replies stay raw
  bytes, and tests compute their expected values independently from
  public tables: a fake that shares a def's assumption can't catch a
  wrong PID.
- Tests of the request loop stay time-driven: they assert bounded
  ranges, and exact sequences come from a fake with fixed latency.
- `uds-generic` needs a per-session signal set built at create from
  the DIDs its URI lists, because the registry holds static sets. Its
  URI grammar is decided in its own slice, with values shown raw (the
  reply's data bytes as an unsigned big-endian number) until a formula
  is known; the payload log keeps the bytes.
- Ship order stays as ADR-0013 §6 has it: `obd2` with the probe, then
  `uds-generic` to research DIDs on a vehicle, then curated sets.
  Curated sets are scoped to a platform (for example `<make>-<platform>`),
  because DIDs and formulas vary by platform and model year.

## Appendix — first vehicle session measurements (2026-10-22)

From the first read-only vehicle session
([session sheet](../bench-sessions/2026-10-22-vehicle.md)): one car,
its engine ECU on `0x7E8`, through the OBDX Pro VX and the x86
surrogate, engine off and idling. The open values are still to be
chosen; these are the measurements they wait on.

- **Reply time.** From each request's `TX_INDICATION` to its reply,
  in the vendor's clock: median about 4 ms, 95th percentile 9–10 ms in
  every connect, longest 83 ms, over 4,809 replies; far inside P2
  (50 ms). The indication marks the vendor's write call returning,
  not the frame leaving: 12–22 % of replies are stamped before their
  own request's indication, by up to 43 ms. So the indication can't
  serve as the send time, and a wait timed from the write's return is
  conservative.
- **Response pending:** none. The only negative reply was `7F 01 12`
  for an unsupported PID (`0x5C`), on every request for it.
- **Supported PIDs:** 44 in `0x01`–`0x60`, and the `0x60` bit is set,
  so the bitmap chain continues past `0x60`.
- **Late replies under the fixed slot:** 9 of 3,657 scheduler replies
  arrived after the next request was written.
- **Refresh at ten PIDs under the fixed slot:** about 1.1 s per signal
  (requests every 100–123 ms on that laptop, 109–112 ms on average);
  `?poll=` ran at about 229 ms per request.

## Appendix — the values, and the engine as built (2026-10-23)

§1 to §3 are built: `core/src/request_engine.h` holds the engine, and
the J2534 worker drives it in one loop for the rotation, `?poll=` and
passive receive. The engine does no I/O and reads no clock; the
worker passes the time in, writes the requests it hands out and gives
it the frames it reads. So its rules are unit-tested with the time
passed in (`RequestEngineTest`), and another transport could drive
it. §4, the probe, isn't built yet. The values left open are these:

| Value | Chosen | From |
|---|---|---|
| Wait | 100 ms | P2 (50 ms) plus 50 ms for the adapter. On the first vehicle the 95th percentile was 9–10 ms and the longest reply 83 ms (appendix of 2026-10-22), both timed from the write call's return, as the wait is |
| Pending wait | 5.05 s | P2* (5 s), the same margin. The first vehicle sent no "response pending" |
| Floor | 20 ms per channel | No vehicle has run the engine. At most 50 requests a second per channel, five times the fixed slot's rate the car handled; set again from the second vehicle session |
| Read slice | 10 ms | The VX holds a read for its whole timeout unless the batch fills, and the surrogate asks for 16 messages; a read held for the wait would see a 4 ms reply only when it ended |
| Requests per set | 128 | `ECUIO_PROFILE_MAX_REQUESTS`. More than the 44 PIDs the first vehicle supports in `0x01`–`0x60`, with room for DIDs |

P2 and P2* are ISO 14229-2's defaults for the server's response time
and the time after a pending reply, with the client adding its own
latency on top (ΔP2): checked against a public summary of those timing
parameters ([UDS time parameters](https://uds.readthedocs.io/en/latest/pages/knowledge_base/time_parameters.html)),
not the ISO 15765-4 text.

**What the engine does to the cadence.** A request goes out when the
last one on its channel completes or its wait runs out, and never
sooner than the floor after the last. A channel whose ECU answers
within the floor gets a request every 20 ms, and a set of N PIDs on
it refreshes each about every N × 20 ms, against N × 100 ms with the fixed slot
(about 1.1 s per signal for `obd2`'s ten on the first vehicle).
Against the fake, which answers at once and returns empty reads at
once, it ran at 32 requests a second: the worker's own sleeps, in
Windows' 15.6 ms steps, set the pace there. The vehicle figure is for
the second session.

**Link health with the engine.** §1 keeps the quiet debounce, counted
in the requests' own unit: ten requests in a row whose wait ran out.
In passive mode the unit stays the read, ten that brought nothing from
an ECU we talk to. Two rules changed with it, both decided once
replies correlated with requests:

- A frame for no channel of ours is no longer quiet at once. It never
  answers a request, so it can't move the state with requests; in
  passive mode it counts as one quiet read. A stray among the replies
  could otherwise flip the state frame by frame.
- A clean read while the adapter is hard-failed and the write latch is
  set steps down to `ECUIO_ERR_REQUEST_FAILED`, with no new episode:
  the adapter reads again, so what fails is the requests. Before, the
  state stayed "adapter fault" until a write went through.

**Still open:** the floor's value; the refresh policy once a set grows
(rate classes, or a curated set beside a full one); several PIDs per
Service 01 request; decoding on the worker thread.

## Appendix — the probe as built (2026-10-23)

§4 is built in `core/src/request_engine.h` and driven by the J2534
worker. These are the choices it makes where §4 left room:

- **The walk.** Each channel with Service 01 requests asks `01 00`
  before its rotation starts, then each next bitmap while the last bit
  of the one before is set, up to `01 E0`, one request at a time
  through the channel's one-shot slot. A bitmap reply is `41 <PID>` and
  four bytes, and PID base + k is bit `0x80 >> ((k - 1) % 8)` of byte
  `(k - 1) / 8`: the first byte's top bit is PID base + 1. Checked
  against Wikipedia's "OBD-II PIDs" (its worked example, `BE 1F A8 13`,
  is a unit test) and the python-OBD library, which reads a bitmap only
  when the one before marks it and takes the PIDs past an unmarked one
  as unsupported. The engine follows that convention: a clear last bit
  ends the chain and marks every later PID unsupported. The J1979 text
  itself wasn't consulted.
- **What drops.** §4's table, with one code added: negative 0x12
  (sub-function not supported) drops its request, as 0x31 does. It's
  what the first vehicle answered for PID 0x5C. While the probe is on,
  the table applies to every reply, not only the walk's: a DID's first
  request in the rotation is its probe, so Service 0x22 gets no pass of
  its own, and a PID the walk didn't reach still leaves when the ECU
  refuses it.
- **Late negative replies.** A negative reply carries the service but
  not the PID or DID, so one that arrives after its request timed out
  would read as the answer to the request out now. A channel is out of
  step from a timeout until its next positive reply, which does carry
  the key, and a negative reply drops nothing while it is. An ECU
  answers in order, so after a positive reply the late ones are past.
- **Resuming.** The walk resumes whenever the link returns to
  `ECUIO_OK` from any other state, not only from quiet and
  requests-failing. It starts again at the first bitmap that wasn't
  answered; a channel whose chain was answered to its end isn't walked
  again, since its answers can't change, and a dropped request stays
  dropped.
- **The budget.** 1 s per run (`ECUIO_ENGINE_PROBE_BUDGET_US`), room
  for all eight bitmaps at a full 100 ms wait each. No bitmap request
  goes out after it; one already out is waited for like any other. On
  the first vehicle the walk asks at least four bitmaps, about 100 ms at
  the floor.
- **`?probe=0`** turns off the walk and the drops, so the rotation is
  the set as written: for research, where every refusal should keep
  showing. `?poll=` implies it, since it sends its one request as asked.
- **What's reported.** `ecuio_session_get_channel_support` (`core.h`)
  gives each channel's answers: the bitmaps and which ranges came back,
  whether the chain was answered to its end, and the dropped requests
  with the reason and the code. The bridge logs a line per update, such
  as "ECU 0x7E8: supports 44 PIDs up to 0x60, no answer past that; 10
  requests, 1 dropped: 01 5C (not in its supported PIDs)", which also
  says which module answered on which channel.
- **The first vehicle.** With its bitmaps (appendix of 2026-10-22;
  `RequestEngineTest.TheFirstVehiclesBitmapsDropOnly0x5C`), the walk
  drops 0x5C and nothing else. The second vehicle session checks it on
  the car.

## Appendix — the Service 09 record (2026-10-23)

The text values this record's scope left to a record of their own have
it: [ADR-0021](0021-vehicle-identification.md). Service 09's items and
the identification DIDs go out as the one-shot requests §1 describes,
through the slot the probe and the scan use, taking turns with the
rotation, and their text is formed in core.

## Appendix — a resumed walk and a taken slot (2026-10-24)

A walk that resumes on recovery can meet the one-shot slot taken by an
identification item or a scanned DID, either waiting for its reply or
due again after a failed write. As first built, starting the walk wrote
its bitmap request into the slot. The item's reply then matched nothing
and the item was left unanswered, after that recovery's rescan had
already run. Or, with the item unsent, the bitmap's reply was stored as
the item's text. The walk now keeps its next bitmap apart from the slot,
and the slot's holder goes first: it's sent, or waited for, and settles,
then the walk takes the slot. The walk's 1 s budget still runs from the
recovery, so an item held in "response pending" for longer ends that
walk unjudged, and the next recovery resumes it.
`RequestEngineTest.RecoveryWalkWaitsForTheItemOut` and
`.RecoveryWalkWaitsForAnUnsentItem` pin both.

## Appendix — nothing left to ask (2026-10-24)

A session with requests can run out of things to ask while it runs: a
`uds-generic` scan that found nothing and polls nothing, once
identification has settled, or an `obd2` session whose probe dropped
every PID. Counted in the requests' unit, such a session could never go
quiet, since no request times out, and its worker read with a 10 ms
timeout forever. With nothing due on any channel the worker now reads as
passive mode does: the 100 ms timeout, and empty reads counting toward
quiet. The cost is that such a session turns quiet about a second after
its last reply, though the ECU answered everything, because a link that
asks nothing can't tell an ECU that went away from one with nothing to
say. The alternatives were stopping the 100 Hz reads but keeping the
state, which can't see an ECU go away, and a heartbeat request the
engine would send with no def asking for it, which §1 doesn't allow.
`J2534HealthTest.NothingLeftToAskGoesQuiet` pins it on both backends.

## Appendix — the engine on a vehicle (2026-10-25)

The second vehicle session ran the engine on the first session's car
([session sheet](../bench-sessions/2026-10-25-vehicle.md)), and its numbers are the first measurements the open values
had:

- **Pace.** Service 01 ran at 26–28 requests a second on one channel,
  so the ten `obd2` signals refreshed about every 0.4 s (the fixed
  100 ms slot gave 1.1 s). Replies came 19 ms after their request
  (median; 90th percentile 30 ms, longest 116 ms), measured by the
  session log's stamps. With the ECU awake, one reply in 7,196
  arrived after the next request went out.
- **The floor stays 20 ms.** Requests follow the replies, so the
  floor binds only when a reply comes back inside it, and about half
  did; the ECU answered every one at that pace. Nothing on this car
  argues for a wider floor.
- **No "response pending".** No `7F xx 78` in about 12,000 replies, so
  the pending wait is still unexercised on hardware.
- **The probe** walked six bitmaps, `41 00` to `41 A0` (the first
  three byte for byte as the first session read them), found 53
  supported data PIDs and dropped only `01 5C`. A connect made with the
  ignition off dropped nothing, and the walk ran once the ECU woke
  (the recovery's `ecuio_engine_probe`).
- **Service 0x22** replies took about 80 ms on this ECU (median), four
  times Service 01's, so a DID-heavy rotation runs at about 13
  requests a second; the wait (100 ms) covers it with room.
