# ADR-0019: Sessions say what they run on, an info struct and the activity log's "Session:" line

- **Status:** Accepted.
- **Date:** 2026-10-22. Implemented with this record.
- **Amends:** [ADR-0011](0011-vendor-call-relay-wire-forms.md), whose
  ioctl relay took an output capacity in-proc only (its appendix of
  2026-10-22).
- **Related:** [ADR-0018](0018-connect-failure-reporting.md), which
  says why a connect failed; this record says what a successful one
  connected to.

## Context

A question about a session starts with what it ran on. After a
connect, the activity log showed the URI and nothing else. It didn't
show whether the DLL loaded in-proc or in the surrogate, at what baud,
with which channels, or under which profile when the URI names none.
The surrogate's session log showed the backend; the in-proc backend
has no session log.

The adapter's own readings weren't fetched at all. `READ_VBATT` was
relayed on both backends ([ADR-0011](0011-vendor-call-relay-wire-forms.md))
but never requested, and `PassThruReadVersion` wasn't wrapped. The
bench run sheet asked the person at the bench to note the adapter's
firmware version "if anything shows" it, and recorded VBATT as
"surface absent".

The ioctl relay also cut a short output buffer differently per
backend. In-proc, `READ_VBATT` copied nothing into a buffer under 4
bytes, and `GET_CONFIG` copied whole pairs only. The surrogate client
took no capacity at all, so the loader staged its replies in a 64-byte
buffer and copied the first bytes that fit.

## Decision

### 1. One POD struct, filled at connect

`ecuio_session_get_info` copies an `ecuio_session_info_t` (`core.h`)
out of the session. It is 432 bytes with no padding, pinned by static
asserts and by `test_c_abi.c`. `flags` says which of the adapter's
optional readings came back, and 16 reserved words take later fields.

| Field | Holds |
|---|---|
| `backend` | `ECUIO_BACKEND_MOCK`, `_J2534_INPROC` or `_J2534_SURROGATE`: the loader's routing ([ADR-0009](0009-pe-architecture-routing.md)) |
| `dll_arch` | The DLL's PE machine, from the sniff at open (`ECUIO_DLL_ARCH_*`, the values discovery reports). A test that forces the surrogate reports the x64 fake as x64 |
| `protocol_id`, `baud`, `connect_flags` | What `PassThruConnect` was called with: ISO 15765, the URI's baud or the 500 kbit/s default, and `?flags=` plus `CAN_29BIT_ID` for 29-bit channels |
| `channel_count`, `rx_ids`, `tx_ids` | The channel table attach derived from the profile, `?filter=` and `?txid=` |
| `vbatt_mv` | `READ_VBATT`, in millivolts, read once after the connect (`ECUIO_SESSION_INFO_VBATT`) |
| `firmware_version`, `dll_version`, `api_version` | `PassThruReadVersion`, read once after the connect (`ECUIO_SESSION_INFO_VERSIONS`) |
| `profile` | The profile's name: `?profile=`, or the scheme's default |

The transport fills the struct in attach, and core adds the profile
before create returns. Nothing writes it after that, so it needs no
lock, any thread may read it, and it stays readable after close until
destroy. A mock session reports its backend and profile and nothing
else.

### 2. Neither reading can fail a connect

`READ_VBATT` and `PassThruReadVersion` are informational. A refusal,
or a DLL without `PassThruReadVersion`, leaves the reading's flag
clear and `ecuio_last_error_detail` empty, and the connect succeeds.
The export is optional on both backends, as `PassThruGetLastError` is.
Both calls take the device ID, not the channel's.

The battery is read once, at connect. Tracking it would need the
worker to issue ioctls between requests, which belongs with the
request engine ([ADR-0015](0015-request-engine.md)).

### 3. One opcode through the surrogate

`OP_READ_VERSION` (12) relays `PassThruReadVersion`. The reply's
`text` grows from one 80-byte field to three
(`ECUIO_SUR_TEXT_FIELDS`); `OP_GET_LAST_ERROR` uses the first.
`READ_VBATT` already traveled as `OP_IOCTL`. Both backends cut each
version string at 79 characters and put it on one line, control
characters becoming spaces, so they report the same words.

### 4. The ioctl relay takes a capacity on both backends

`ecuio_j2534_sur_ioctl` takes `output_cap`, as the loader's helper
does, and the loader passes the caller's buffer straight through
instead of staging it. On both backends the output gets the first
`output_cap` bytes of the result, and the reported length is the
whole result's, so a caller spots a cut by comparing.

### 5. The activity log says it

After "Connected:", the bridge logs one "Session:" line built from the
struct (`QtTuneBridge::sessionInfoText`). Like every activity line, it
also lands in a recording's `events.log`. For the fake vendor DLL:

    Session: x64 DLL, in-proc; ISO 15765 at 500000 baud; profile obd2, channel 0x7E8/0x7E0 (rx/tx); battery 13.8 V; firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04

A reading the adapter refused shows as "no battery reading" or "no
version reading".

## Alternatives considered

- **A getter per field.** Each field added later would be a new
  symbol. A struct with reserved room grows without one, and the log
  line is built from a single copy.
- **A formatted string from core.** Simplest for the log, but the
  recorder and any later UI want values, such as the battery as a
  number. The wording belongs to the app.
- **Require `PassThruReadVersion`.** v04.04 defines it next to the
  nine calls the loader requires. Requiring it too would refuse DLLs
  that load today, for the sake of information.
- **Read the versions at discovery,** so the interface list could show
  them. Discovery would have to open every adapter. The VX and the VT
  refuse to open when their adapter isn't attached, and only one
  process can hold an adapter at a time.

## Consequences

- The surrogate's wire changed: a new opcode, and a reply `text` of
  240 bytes, which makes the response 1608 bytes. Both build trees
  rebuild, and the fingerprint refuses a stale pairing.
- `ecuio_j2534_sur_ioctl` gains a parameter. The loader is its only
  product caller.
- A connect costs two more vendor calls, and through the surrogate two
  more pipe transactions. Through the surrogate, a vendor that hangs in
  either one costs up to two 3 s transaction timeouts, and a wedged one
  then fails the start, because the child is marked unresponsive.
  In-proc, a hang in either blocks the connect, as a hang in
  `PassThruConnect` would.
- In-proc, a short ioctl buffer now gets the first bytes of the
  result, where it used to get none (`READ_VBATT`) or whole pairs only
  (`GET_CONFIG`).
- Neither reading has been seen on a real adapter. The next BENCH
  session records the VX's answers
  ([BENCH prep](../bench-sessions/bench-prep.md) #10).
- The fake vendor DLL exports `PassThruReadVersion`, through
  `fake_j2534.def` alone, so the legacy fixture lacks it. It refuses
  either reading on request (`ECUIO_FAKE_VBATT_STATUS`,
  `ECUIO_FAKE_VERSION_STATUS`).
