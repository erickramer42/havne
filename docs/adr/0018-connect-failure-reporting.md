# ADR-0018: Connect failures say why, status codes and a thread-local detail

- **Status:** Accepted.
- **Date:** 2026-10-22. Implemented with this record.
- **Amends:** [ADR-0007](0007-uri-selected-transports.md) and
  [ADR-0013](0013-protocol-profiles.md), whose malformed-URI and
  unknown-profile failures returned `ECUIO_ERR_NO_TRANSPORT` (their
  appendices of 2026-10-22).

## Context

`ecuio_session_create` folded every failed connect into two codes. A
malformed URI, an unknown profile and an adapter name that matched
nothing all read "no transport available" (`ECUIO_ERR_NO_TRANSPORT`).
A DLL that wouldn't load, a surrogate that wouldn't start and a vendor
that refused `PassThruOpen` all read "failed to load transport library"
(`ECUIO_ERR_LOAD_FAILED`).

The bench and vehicle sessions met the third case: the OBDX Pro VT and
VX refuse to open with status 34 when their adapter isn't attached
([vehicle session](../bench-sessions/2026-10-22-vehicle.md)). The
Settings error line said "failed to load transport library", and only
the surrogate's session log held the status. The in-proc backend has no
session log at all. Nothing fetched the vendor's own description of a
failure, `PassThruGetLastError`, either.

Each of these causes asks the user for a different fix: correct the
URI, pick another profile, attach or power the adapter, repair the
install.

## Decision

### 1. Four status codes for the causes

Appended to `ecuio_status_t`, as the enum's rule requires:

| Code | Cause | Examples |
|---|---|---|
| `ECUIO_ERR_BAD_URI` (12) | The URI is malformed | A query that breaks the shared grammar, a key the transport doesn't take, a bad value, `?poll=` with no channel, channels mixing 11- and 29-bit IDs |
| `ECUIO_ERR_UNKNOWN_PROFILE` (13) | `?profile=` names no registered profile | `?profile=bogus` |
| `ECUIO_ERR_VENDOR_REFUSED` (14) | The adapter's driver refused a call, or `PassThruOpen` never returned | `PassThruOpen`, `PassThruConnect` or `PassThruStartMsgFilter` returning a vendor status |
| `ECUIO_ERR_SURROGATE_FAILED` (15) | The surrogate couldn't start or stopped answering | No surrogate exe, a failed spawn or handshake, a surrogate built from other shared headers |

`ECUIO_ERR_NO_TRANSPORT` keeps "nothing handles this URI": an unknown
scheme, or no installed adapter that matches the selector.
`ECUIO_ERR_LOAD_FAILED` keeps "the DLL won't load or lacks a mandatory
export", in-proc and in the surrogate (its exit codes 4 and 5).

### 2. A thread-local detail in words

`const char* ecuio_last_error_detail(void)` returns why the last
`ecuio_session_create` or `ecuio_session_start` on the calling thread
failed: which URI key and what it should hold, which vendor call, its
status by its J2534-1 v04.04 name (or "vendor-specific"), and the
vendor's own text from `PassThruGetLastError`. For example:

    PassThruOpen returned status 34 (vendor-specific): <vendor text>
    ?baud= needs a nonzero decimal, not '0'

Both calls clear it on entry. The layer that knows the cause sets it
once, at the failure, and the layers above pass the status up without
touching it. A success leaves "". The text names Windows errors by
number and the session log by its folder (`%TEMP%`), never a path that
carries the account name.

It is thread-local, like `dlerror()`, because create fails before a
session exists, so there is no handle to hang it on, and because the
bridge creates sessions on a connect thread while starting them on the
GUI thread. Each reads its own.

### 3. The same words from both backends

In-proc, the loader calls `PassThruGetLastError` right after a refused
call. Through the surrogate, the client asks the child with
`OP_GET_LAST_ERROR` (opcode 11), whose reply carries an 80-byte text
field, before it closes the child. `PassThruGetLastError` is optional
on both: a DLL without it loses only the vendor's text. The connect
tests compare the detail word for word on each backend.

### 4. The app shows both

The bridge reports a failed connect as "<status>: <detail>", in the
Settings error line and the activity log's "Connect failed:" line.

## Alternatives considered

- **Codes only.** The vendor's text and the offending URI key would
  reach only the surrogate's session log, and the in-proc backend has
  none.
- **Detail only, codes unchanged.** No caller breaks, but none can
  tell the causes apart without parsing text.
- **An error string on the session.** Create fails before a session
  exists.
- **A caller-supplied buffer on create.** It changes a stable
  signature, and start would need the same.

## Consequences

- A malformed URI returns `ECUIO_ERR_BAD_URI` and an unknown profile
  `ECUIO_ERR_UNKNOWN_PROFILE`, where both returned
  `ECUIO_ERR_NO_TRANSPORT`. A caller that compared against
  `ECUIO_ERR_NO_TRANSPORT` for those sees a different code. The bridge
  is the only caller, and it shows the text.
- The surrogate client's open returns `ECUIO_ERR_SURROGATE_FAILED` for
  a spawn, handshake or fingerprint failure, and
  `ECUIO_ERR_VENDOR_REFUSED` for a refusal or a `PassThruOpen` that
  never returns. It returns `ECUIO_ERR_LOAD_FAILED` only for a DLL the
  child couldn't load or resolve.
- The surrogate's wire changed (an opcode and a reply field), so both
  build trees rebuild; the fingerprint refuses a stale pairing.
- The fake vendor DLL exports `PassThruGetLastError` and refuses open,
  connect or a filter on request (`ECUIO_FAKE_OPEN_STATUS`,
  `ECUIO_FAKE_CONNECT_STATUS`, `ECUIO_FAKE_FILTER_STATUS`).

## Appendix: the helpers below the connect path (2026-10-23)

The same codes now cover the J2534 helpers the connect path doesn't
reach, on both backends (`internal.h`, and the surrogate client in
`j2534_surrogate.h`): writes, ioctls, filter stops, disconnect and
`PassThruReadVersion`. Until then a vendor's refusal of any of them
and a surrogate that stopped answering both read
`ECUIO_ERR_NO_TRANSPORT`, and a bad length or filter shape read
`ECUIO_ERR_NULL_ARGUMENT`:

| Code | Cause |
|---|---|
| `ECUIO_ERR_NULL_ARGUMENT` | A required pointer is NULL |
| `ECUIO_ERR_INVALID_ARGUMENT` | A length or shape the call can't take, refused before any vendor call |
| `ECUIO_ERR_VENDOR_REFUSED` | A status other than `STATUS_NOERROR`, including the surrogate child's own refusals, which answer as a strict vendor would |
| `ECUIO_ERR_SURROGATE_FAILED` | The child didn't answer, or answered past the wire's limits |

`ECUIO_ERR_NOT_IMPLEMENTED` keeps "an ioctl outside the whitelist, or
a DLL without an optional export". The read path keeps its own link
health mapping. A malformed `GET_CONFIG` or `SET_CONFIG` array is now
judged at the loader seam, before the backend; before, in-proc refused
it as a NULL argument and the surrogate as a refusal.

Nothing above the seam changes: the worker compares a write's status
only with `ECUIO_OK`, and the session info's readings ignore why they
failed. These calls set no `ecuio_last_error_detail`, which stays the
connect path's. The surrogate client's functions are in a public
header, so a direct caller of them sees the new codes; the loader and
the tests are the only ones.

## Appendix: the two public functions (2026-10-23)

`ecuio_profile_lookup` answers an unknown name with
`ECUIO_ERR_UNKNOWN_PROFILE`, the code `ecuio_session_create` gives
that `?profile=`, in place of `ECUIO_ERR_NO_TRANSPORT`; create passes
lookup's code through. `ecuio_session_send` answers a `data_length`
past the frame's `data` with `ECUIO_ERR_INVALID_ARGUMENT` in place of
`ECUIO_ERR_NULL_ARGUMENT`; any frame that fits still gets
`ECUIO_ERR_NOT_IMPLEMENTED`, since send isn't built. The bridge
reaches neither.

## Appendix: the status in hex (2026-10-25)

A vendor status is printed in hex as well as decimal, before its name:
`PassThruOpen returned status 34 (0x22, vendor-specific): <vendor text>`.
Vendors document their own codes in hex, and the J2534 headers define
the standard ones that way, so the decimal alone sent a reader to a
converter (the VX answers 34, 0x22, when no adapter is attached). The
example in section 2 shows the earlier form. Pinned by
`CoreTest.VendorDetailIsOneLine` and `J2534ConnectTest`.

## Appendix: `LOAD_FAILED`'s words (2026-10-26)

`ecuio_status_string(ECUIO_ERR_LOAD_FAILED)` reads "couldn't load what
the URI names". The code covers a replay's recording as well as an
adapter's DLL ([ADR-0023](0023-replay-transport.md)), and the bridge
puts the status before the detail, so a replay refused for an empty
`frames.csv` read "failed to load transport library: …". The detail
after the colon still says which: the DLL and the loader's words, or
the recording's line.
