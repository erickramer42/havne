# ADR-0020: `uds-generic` — DIDs named in the URI, a set built per session, and a scan

- **Status:** Accepted.
- **Date:** 2026-10-23. Implemented with this record.
- **Builds on:** [ADR-0013](0013-protocol-profiles.md) §6 (the ship
  order names `uds-generic` as the research profile),
  [ADR-0015](0015-request-engine.md) §2 (a def keyed `62 <DID>` enrols
  `22 <DID>`) and its Consequences (a per-session set built at create;
  values shown raw until a formula is known), and
  [ADR-0016](0016-signal-definition-layout.md)'s text kinds.

## Context

The curated sets this project wants (`mazda-kf` and the like) need
research on a car: which Service 0x22 DIDs an ECU answers, how long
each reply is, and which bytes move with what. The `obd2` profile can't
ask for a DID, and a registered profile is static data: the registry
keeps a pointer to a set that must outlive its sessions, so there is no
way to type a DID into a URI and have a session read it.

The engine already does the hard part. A def whose mux key is
`62 <DID>` at offset 0 enrols the request, correlates the reply and,
with the probe on, leaves the rotation when the ECU refuses it. A `HEX`
def shows a reply of any length as hex pairs, and a `TEXT` def as ASCII.
What's missing is a profile whose set is built from the URI.

A research session also wants to sweep a block of DIDs (Mazda's
enhanced data sits in `0x1Exx`; identification in `0xF1xx`) and see
which answer. A block of 256 is twice the request table, and most of it
is refused; it must not sit in the rotation.

## Decision

### 1. The URI names the DIDs

```
?profile=uds-generic&did=<entry>[,<entry>...]
entry := [<ecu>:]<DID>[-<DID>][:<type>]
```

- A `DID` is 1 to 4 hex digits with no prefix. An entry's type is `hex`
  (the default: the reply's data bytes as pairs), `text` (ASCII), or
  `u8`, `u16`, `u24`, `u32`, `i8`, `i16`, `i24`, `i32`: a big-endian
  number of that width, scale 1, offset 0, no unit, no decimals. Every
  type name holds a letter past `f`, so a two-token entry is told apart
  by its last token: a type, a DID (then the first token is an ECU), or
  a misspelt type.
- `<ecu>` is the ECU's response ID in hex. The request goes 8 below it
  (`7E9` asks on `7E1`; an 11-bit ID is 9 to 7FF, so the request ID is
  never 0), or, for a 29-bit `18DA<a><b>`, on the swapped pair. Any
  other ID is refused, since the transport can't address it. Without a
  prefix the ECU is `7E8`, asked on `7E0`.
- Every `did=` key counts: the key is a list, so it is the one key
  create doesn't take last-wins. `did=` belongs to this profile alone:
  with another profile it is `ECUIO_ERR_BAD_URI`, as is
  `?profile=uds-generic` with no `did=`. Both say so before anything
  attaches, like a bad `?profile=`.
- Limits, each refused at create with the detail set:
  `ECUIO_PROFILE_MAX_REQUESTS` (128) DIDs to poll,
  `ECUIO_ENGINE_SCAN_MAX` (1024) to scan, `ECUIO_MAX_CHANNELS` (4) ECUs.
  A DID listed twice is kept once.

### 2. A set of the session's own

The registry binds `uds-generic` to an empty, valid set, so a lookup
tells it from `raw`. `ecuio_session_create` then builds the session's
own set from the query (`core/src/uds_generic.h`) and stores it in the
session, which owns it until destroy; `active_signal_set` points at it,
so the transport, the bridge and the recorder read it as any other
set. One def per DID, keyed `62 <DID>` at offset 0 with the value from
byte 3, named `DID 1E1C` (or `0x7E9 DID 1E1C` off the default ECU),
ids from `ECUIO_UDS_GENERIC_SIG_ID_BASE`; one channel per ECU, in order
of first appearance. The polled defs come first, then the scanned ones,
and the session records how many are polled, so the J2534 transport
derives the rotation from that head alone.

### 3. A range is scanned once, beside the rotation

A single DID is polled: its def enrols `22 <DID>` in the rotation. A
range `A-B` is scanned (`request_engine.h`): each DID is asked once,
through the channel's one-shot slot after the probe's walk, taking
turns with the rotation so the listed DIDs keep refreshing while the
range runs. A positive reply puts the DID in the rotation while the
table has room; a negative one counts it refused; one whose wait runs
out is unanswered, and is asked again when the link returns to ok, as
the walk resumes. The scan runs whether or not the probe is on.

A negative reply carries the service but no DID, so after a timeout a
late one can at worst count the DID out now as refused; the reply that
was really its own still decodes through its def, and a DID the scan
misjudged can be listed alone next time.

`ecuio_channel_support_t` gains the counts (in its reserved room: total,
done, found, refused, polled) and `ECUIO_SUPPORT_SCAN_COMPLETE`. The
bridge's ECU line adds "scan: 9 DIDs, 1 found, 8 refused", with
"(N polled)" when the table filled and "N unanswered" while some are.

### 4. The mock answers

Under `uds-generic`, `mock://` answers every DID of the session's set
each tick, from the ECU the def names: the tick count in a number's
width, four bytes of it for `hex`, and a line naming the DID and the
tick for `text`. So the app shows the profile with no hardware, and the
QML tests show a text row on every platform.

## Alternatives considered

- **`did=` implies the profile.** Rejected: ADR-0013 §2 wants the URI to
  say what a session is, and `?profile=obd2&did=` would need a rule of
  its own. The Settings page will build the URI, so the typing cost
  lands there.
- **Spell out `rx/tx` per ECU.** Rejected for now: every ECU seen so far
  answers 8 above its request ID, or on the 29-bit swapped pair. An ECU
  outside both conventions can come back as an extension of the entry.
- **Scan first, rotation after** (the walk's shape). Rejected: a
  256-DID scan at the 100 ms wait is 26 s of silence on the listed
  DIDs, which the researcher is watching.
- **Hide the rows of DIDs the scan hasn't answered.** Not done: the set
  is fixed at create and the model shows it. A row without a value
  reads "—", and the ECU line gives the counts. Revisit with the
  decode-on-worker record if 1024 rows prove too many.
- **A flag on the def to mark a scanned DID.** Rejected: an ABI change
  for an internal need. The session knows the split, and the transport
  reads it from the session.

## Consequences

- A manufacturer set can be researched on a car with a URI: poll the
  DIDs a forum names, scan a block, watch which rows move. The recorder
  keeps the bytes (`frames.csv`) and the values (`signals.csv`).
- `ecuio_channel_support_t`'s reserved room shrinks by 12 bytes; its
  size and the pinned offsets hold.
- A set may now live in a session rather than the registry, so
  `ecuio_session_signal_set` is the only way to a session's set; code
  that reaches for the registry by name would miss these.
- The scan's defs are rows in LiveView from the start, so a range of
  256 shows 256 rows, most reading "—" until the scan ends.
- The late-negative caveat above is the price of a key-less negative
  reply; the probe already pays it.
- The fuzz job gains `fuzz_uds_query`, since `?did=` is typed text.
