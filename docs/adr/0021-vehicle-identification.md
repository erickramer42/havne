# ADR-0021: Vehicle identification, the VIN and the ECU's identifiers, asked once at connect

- **Status:** Accepted.
- **Date:** 2026-10-23. Implemented with this record.
- **Builds on:** [ADR-0015](0015-request-engine.md) §1 (one-shot
  requests through the engine's pacing) and §3 (Service 09 is among the
  read-only services the engine sends), whose scope left the text
  values of Service 09 to a record of their own; [ADR-0020](0020-uds-generic.md)
  §3 (the one-shot slot taking turns with the rotation, and the report
  that carries what it learned); [ADR-0017](0017-session-recording.md)
  (the activity log's lines land in a recording's `events.log`).

## Context

A session knows which adapter it runs on ([ADR-0019](0019-session-info.md))
and what each ECU supports ([ADR-0015](0015-request-engine.md) §4), but
not which vehicle it is talking to. A session sheet has to carry the
VIN and the calibration IDs by hand, a recording can't be told from
another car's, and the planned per-vehicle profiles need a key.

The standard services carry what's needed. SAE J1979 Service 09 gives
the VIN (item 02), the calibration IDs (04), their verification numbers
(06) and the ECU's name (0A); ISO 14229-1's identification DIDs
(`F187`–`F195`) give the part, software and hardware numbers, the
supplier, the serial and the VIN again (`F190`). All are read-only, and
all answer once: nothing in them changes during a session, so they
don't belong in the rotation. Their replies are text or short binary,
some longer than one CAN frame; the adapter reassembles ISO-TP, so a
frame carries the whole reply ([ADR-0006](0006-frame-abi-v0.3.md),
appendix).

The engine already has the slot for this: the one-shot that walks the
probe's bitmaps and scans `uds-generic`'s ranges, taking turns with the
rotation. What's missing is the list, where the answers go, and how they
reach the log.

## Decision

### 1. One fixed list, asked once per channel

Every channel that requests (a set's rotation or a scan) also asks its
ECU, once each and in this order: `09 02`, `09 04`, `09 06`, `09 0A`,
then `22 F187`, `F188`, `F189`, `F18A`, `F18C`, `F190`, `F191`, `F194`,
`F195` (`ECUIO_IDENT_REQUESTS` in `core/src/request_engine.h`, each
with its display name). A passive session sends nothing, so it asks
nothing; `?poll=` asks its one PID alone; `?identify=0` turns it off
for a session, as `?probe=0` does the probe.

The items go through the channel's one-shot slot after the walk, taking
turns with the rotation like the scan and ahead of it: one rotation
request, one item, and so on, so live values start at once and the
identity arrives within a second or two. A positive reply's data, past
the service's header (`49 <item> <count>` or `62 <DID>`), becomes the
item's text; a negative reply records its code and the item is not
asked again; a timeout leaves it unanswered, and every unanswered item
is asked again when the link returns to ok, as the walk resumes and the
scan rescans (`ecuio_engine_rescan`). Nothing learned enters the
rotation.

### 2. The text is made in core

The report shows each answer as text, formed in core
(`ecuio_identity_format`): ASCII when every byte is printable or
padding (NUL, 0xFF), the printable runs joined by ", " (an ECU with two
calibration IDs reads "A, B") and trailing blanks dropped; otherwise hex
pairs ("DE AD BE EF" for a CVN). It is cut to 63 characters, at a whole
pair. The UI receives final display values and parses no frames
([ADR-0001](0001-qt-free-core-c-abi.md)); the wording of the line is
the bridge's, the bytes' meaning is core's.

### 3. A report per channel, through the C ABI

`ecuio_channel_identity_t` (`core.h`, 1184 bytes, pinned): `flags`
(`ECUIO_IDENTITY_COMPLETE` once every item has an outcome), `rx_id`,
`generation` (bumped as each item settles), `items`, and up to
`ECUIO_IDENTITY_MAX_ITEMS` (16) `ecuio_identity_item_t` (72 bytes: the
request, its outcome, the negative code, the text). The worker publishes
it beside the support report, under the same mutex;
`ecuio_session_get_channel_identity` copies it, with the support
report's rules (a channel index below the session's channel count, any
thread, the last update after close). `ecuio_identity_request_name`
gives an item's display name ("VIN", "CALID", …) so the list and its
words live in one place.

### 4. One line in the activity log, once complete

The bridge polls the report on its coalesce tick and logs one line per
ECU when the report reads complete, and again when a recovery's re-asks
settle: each answered item by name and text, then the refused requests
with their codes, then the unanswered count. A silent ECU reads "13
unanswered". Through the fake:

    ECU 0x7E8 identity: VIN FAKEJ2534VIN00001; CALID FAKECAL-01; CVN DE AD BE EF; ECU name FAKE-ECM; part number FAKE-PART-0001; software number FAKE-SW-0001; software version FAKE 1.0; supplier FAKE SUPPLIER; ECU serial FAKE-SERIAL-000001; VIN (F190) FAKEJ2534VIN00001; hardware number FAKE-HW-01; 2 refused: 22 F1 94 (7F 22 31), 22 F1 95 (7F 22 31)

The recorder routes every activity line into `events.log`, so a
recording names its vehicle without a change to the recorder. The
recording's folder keeps its date name: it opens at connect, before the
VIN arrives, and a rename belongs with the preferences work.

### 5. What the fake says

The fake vendor DLL answers the whole list with values marked FAKE (a
17-character VIN, a NUL-padded CALID and ECU name, a binary CVN, text
DIDs), and refuses `F194` and `F195` with `7F 22 31`, so the HARNESS
tier sees every outcome but a timeout. The mock answers nothing: it has
no channels and no engine, and `mock://` sessions report no identity.

## Alternatives considered

- **Text defs keyed `49 02` in the profile sets, shown as rows.** The
  decode path exists ([ADR-0016](0016-signal-definition-layout.md)),
  but the rows would sit in every set's LiveView list, refresh never,
  and still need a one-shot request the defs can't enrol. The
  identity is a fact about the session, like the adapter, not a signal.
- **Identification as a block before the rotation.** Simpler ordering,
  but thirteen asks at the wait's worst case hold live data back for
  over a second on a car that answers slowly, and the walk's budget
  would need restarting. Taking turns costs nothing visible.
- **Raw bytes in the report, text in the bridge.** Keeps core smaller,
  but puts a decode rule (printable, padding, hex) in the UI, which the
  architecture forbids.
- **Only Service 09.** The OBD items are the portable ones, but the
  `F18x` DIDs are what a research session on a manufacturer ECU wants,
  and each refusal costs one ask.
- **Rename the recording's folder by VIN.** The folder is open and
  written to before the VIN arrives; renaming open files is fragile on
  Windows, and the name is a preference (per-vehicle profiles will want
  a label too).

## Consequences

- A connect sends thirteen more requests per channel, interleaved with
  the rotation: about a second against a quick ECU, up to 13 × 120 ms
  on one that answers nothing. `?identify=0` removes them; tests that
  pin write order use it, as they use `?probe=0`.
- The VIN and the ECU's serial land in the activity log and in every
  recording's `events.log`. The bench-session scrub rule already covers
  Service 09 replies in logs; the identity line is one more place to
  check before a sheet or a screenshot is committed.
- The engine's channel grows by the item array (about 1.2 KB a channel);
  the engine lives in the heap-allocated transport, and briefly on the
  stack in the registry's fit check and the unit tests.
- A negative `7F 22 11` to an identification DID drops every Service
  0x22 request of the channel's rotation while the probe is on, as the
  scan's would: the ECU said the service is unsupported.
- The late-negative caveat of [ADR-0020](0020-uds-generic.md) §3 holds
  here too: an item is settled by either reply whatever the channel's
  step, so a negative reply arriving after its request timed out can
  count the item out as refused.
- Per-vehicle profiles ([STATUS.md](../../STATUS.md), backlog) now have
  their key.

## Appendix: one ECU's answers (2026-10-25)

On the second vehicle session ([session sheet](../bench-sessions/2026-10-25-vehicle.md)), the engine ECU of the first
session's car, in its default session, with the ignition on:

- **Answered:** all four Service 09 items (VIN, calibration ID, CVN,
  ECU name) and two DIDs, F188 (the manufacturer's ECU software
  number) and F190 (the VIN).
- **Refused** with `7F 22 31`: F187, F189, F18A, F18C, F191, F194 and
  F195.
- **Unanswered:** none while the ECU was awake. With the ignition off
  all 13 went unanswered, and the line said so; after the ECU woke,
  the re-ask settled them and a second line followed.
- **Lengths:** the replies longer than one CAN frame (`49 02` 20
  bytes, `49 04` 19, `49 0A` 23, `62 F1 88` and `62 F1 90` 27) arrived
  whole. This ECU has one calibration ID.

The items are named here, never their values.
