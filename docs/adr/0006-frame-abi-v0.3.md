# ADR-0006: Frame ABI v0.3 and the session close/destroy split

- **Status:** Accepted
- **Date:** 2026-10-14 (`d0fb07e`, `feat!`)

## Context

The v0.2.0 `ecuio_frame_t` had an `is_extended` bool and assumed an
8-byte payload. Seven of its 88 bytes were accidental tail padding.
J2534, CAN-FD and the message-ID gate
([ADR-0004](0004-signal-decoding-model.md)) all need more than that
struct could carry.

Separately, `ecuio_session_close` freed the session. That made the
closed-flag check on a second `close` a use-after-free.

## Decision

**Frame layout (breaking change).** The struct stays at 88 bytes, now
with no hidden padding. `static_assert`s pin its size and offsets.

| Field | Type | Meaning |
|---|---|---|
| `timestamp_us` | `uint64_t` | Microseconds since session start |
| `arbitration_id` | `uint32_t` | CAN ID |
| `channel_id` | `uint16_t` | Bus index only; it never multiplexes an ID |
| `data_length` | `uint16_t` | Bytes used in `data`, 0–64 |
| `data[64]` | `uint8_t` | Inline payload |
| `flags` | `uint32_t` | `ECUIO_FRAME_FLAG_*`: EXTENDED, REMOTE, FD, BITRATE_SWITCH, ERROR, TX_ECHO |
| `reserved` | `uint32_t` | Producers leave it zero |

**Session lifecycle.** The session follows `FILE`/`free` discipline:

- `create` allocates and attaches the transport.
- `close` stops the session and detaches. It is safe to call twice:
  the second call returns `ECUIO_ERR_SESSION_CLOSED`.
- `destroy` frees the session, closing it first if needed.

## Consequences

- Integrations built against v0.2.0 must migrate:
  - read `flags & ECUIO_FRAME_FLAG_EXTENDED` instead of `is_extended`;
  - honor `data_length` instead of assuming 8 bytes;
  - treat timestamps as session-relative.
- Because the payload is inline, copying the struct is a deep copy.
  The bridge relies on this when it moves frames across threads.
- The version string still reads 0.2.0 until the v0.3.0 tag (Known
  Issue #16). The ABI break is documented, but the version number
  doesn't show it yet.

## Appendix — the version (2026-10-22)

The version string became 0.3.0 with the v0.3.0 tag, so the number
now shows this record's ABI break.
