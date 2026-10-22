# ADR-0015: Request engine — correlated, response-paced requests derived from signal definitions

- **Status:** Proposed. The structure below is decided. The values
  listed under "Open until the first vehicle session" are set from
  that session's measurements and recorded in a dated appendix; the
  appendix moves this record to Accepted. On acceptance, ADR-0013's
  status line points here for the parts this record supersedes.
- **Date:** 2026-10-21
- **Supersedes, on acceptance:** [ADR-0013](0013-protocol-profiles.md)
  §1's placement of request correlation in the profile, and §5's
  callback table as the plugin seam. Amends §3's probe semantics.
  Leaves §2 (static selection by URI), §4 (declared channels) and §6
  (ship order) as they are.

**Scope.** This record covers sending requests and classifying their
replies. Three neighbouring decisions are made elsewhere:

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
| Whether to send several PIDs per Service 01 request | The standard's text and ECU behaviour on a vehicle. Not part of this record; it needs a splitter for the combined reply, and would build on the engine |
| Whether decoding moves to the worker thread | Frames per second and GUI-thread cost measured with the engine running. It is the path to per-sample timestamps (Known Issue #10) either way |

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
  Curated sets are scoped to a platform (for example `mazda-kf`),
  because DIDs and formulas vary by platform and model year.
