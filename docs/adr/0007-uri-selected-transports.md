# ADR-0007: Transports selected by URI; failures surface at create

- **Status:** Accepted
- **Date:** 2026-10-13. Transport vtable `844f44e`, session-integrated
  J2534 `1cc3334`, query parameters `728533e`.

## Context

J2534 had to sit beside the mock transport without forking the
session code. The caller (the Settings page) needs to find out early
whether a chosen device is usable, while it can still offer another
one, rather than after streaming has supposedly started.

## Decision

- The session reaches its transport only through an ops table,
  `ecuio_transport_ops_t` (`start`, `detach`). The mock moved onto
  this table with no test changes.
- `ecuio_session_create` routes on the URI prefix: `mock://` on every
  platform, `j2534://` on Windows. Anything else returns
  `ECUIO_ERR_NO_TRANSPORT`.
- **Work is split between create and start.**
  - *Create (attach)* resolves the URI and loads and opens the vendor
    DLL, so load failures surface at create.
  - *Start* connects the channel and launches the worker.
- **URI parsing is strict.** A malformed URI is rejected with
  `ECUIO_ERR_NO_TRANSPORT` at create, before any DLL is loaded. The query
  string is split off before the selector is resolved. That ordering
  also fixed two bugs: a query glued onto a vendor name broke the
  discovery match, and a query on a path form reached `LoadLibraryEx`
  as part of the filename.
- **`baud = 0` is the internal "unset" value.** The loader turns it
  into the ISO 15765 default of 500 kbit/s. An explicit `baud=0` is
  rejected. `flags` passes through verbatim, with no bit meanings
  interpreted until real hardware has been observed.

The full grammar is in [docs/j2534.md](../j2534.md#uri-grammar).

## Consequences

- The device picker can retry with another URI and nothing leaks,
  because a failed create leaves no session behind.
- Every new query parameter also needs a rejection test for its
  malformed forms.
- Adding a transport means adding a prefix in `core.cpp` and an ops
  table. The bridge and QML do not change.

## Appendix — failure codes (2026-10-22)

A malformed query returns `ECUIO_ERR_BAD_URI` instead of
`ECUIO_ERR_NO_TRANSPORT`, and `ecuio_last_error_detail` names the key
and what it should hold ([ADR-0018](0018-connect-failure-reporting.md)).
An unknown scheme, and a selector that matches no installed adapter,
still return `ECUIO_ERR_NO_TRANSPORT`.
