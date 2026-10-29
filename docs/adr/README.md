# Architecture Decision Records

Each record captures one significant design decision: the problem it
answered, what was chosen, and what that choice costs. Records are
not rewritten when the design moves on. A new record supersedes the
old one, and the old record's status is updated to point at it.

| # | Decision | Status |
|---|---|---|
| [0001](0001-qt-free-core-c-abi.md) | Qt-free core behind a stable C ABI | Accepted |
| [0002](0002-callback-pair-identity-snapshot-dispatch.md) | Callback identity by `(callback, user_data)` pair, snapshot dispatch | Accepted |
| [0003](0003-backpressure-bounds-in-flight-frames.md) | Backpressure bounds in-flight frames, not cumulative count | Accepted |
| [0004](0004-signal-decoding-model.md) | Signal decoding: POD definitions, linear scaling, Intel byte order, message-ID gate | Accepted; field model superseded by 0016 |
| [0005](0005-coalesced-signal-updates.md) | Coalesce signal updates to 10 Hz; pull API for the strip chart | Accepted |
| [0006](0006-frame-abi-v0.3.md) | Frame ABI v0.3 and the session close/destroy split | Accepted |
| [0007](0007-uri-selected-transports.md) | Transports selected by URI; failures surface at create | Accepted |
| [0008](0008-x86-surrogate-host.md) | Host 32-bit J2534 DLLs in an x86 surrogate process | Accepted; data plane and threading superseded by 0014 |
| [0009](0009-pe-architecture-routing.md) | Route in-proc vs surrogate at the loader by PE machine type | Accepted; routing superseded by 0025 |
| [0010](0010-j2534-v0404-shared-types.md) | SAE J2534-1 v04.04 is the contract; one shared types header | Accepted |
| [0011](0011-vendor-call-relay-wire-forms.md) | Vendor-call relay: flat wire forms, identical ioctl whitelist at both ends | Accepted |
| [0012](0012-j2534-transport-adoption.md) | J2534 transport adoption: caller-chosen IDs, transport-driven request loop | Accepted |
| [0013](0013-protocol-profiles.md) | Protocol profiles: URI selection, profile-owned probe and request loop, declared CAN channels | Accepted; request correlation and the callback table superseded by 0015 |
| [0014](0014-single-threaded-surrogate.md) | Single-threaded surrogate child; reads travel over the pipe | Accepted |
| [0015](0015-request-engine.md) | Request engine: correlated, response-paced requests derived from signal definitions; read-only by construction | Accepted |
| [0016](0016-signal-definition-layout.md) | Signal definition layout: value kind, explicit byte order, layout version, zero-checked reserved room | Accepted |
| [0017](0017-session-recording.md) | Session recording: one folder per connect, `frames.csv` as the replay format | Accepted |
| [0018](0018-connect-failure-reporting.md) | Connect failures say why: four status codes and a thread-local detail with the vendor's text | Accepted |
| [0019](0019-session-info.md) | Sessions say what they run on: an info struct (backend, channel, profile, battery, versions) and the activity log's "Session:" line | Accepted |
| [0020](0020-uds-generic.md) | `uds-generic`: DIDs named in the URI (`?did=`), a set built per session, ranges scanned once beside the rotation | Accepted |
| [0021](0021-vehicle-identification.md) | Vehicle identification: the VIN and the ECU's identifiers (Service 09, DIDs F187–F195) asked once per channel at connect, a report per channel and one activity-log line | Accepted |
| [0022](0022-display-units.md) | Display units: metric or imperial, converted in core by unit string at the UI's edge; recordings keep decoded values | Accepted |
| [0023](0023-replay-transport.md) | Replay transport: `replay://` plays a recording's `frames.csv` back through the frame API, paced by its session time | Accepted |
| [0024](0024-decode-on-the-worker.md) | Decoding on the worker: a values callback delivers each frame's decoded signals with the frame's time | Accepted |
| [0025](0025-one-j2534-backend.md) | One J2534 backend: every vendor DLL runs in a surrogate of its own architecture; the in-proc vendor calls are deleted | Accepted |
| [0026](0026-elm327-transport.md) | ELM327 transport: `elm327://` drives an ELM327-compatible adapter over a serial port, with the request engine | Accepted |
| [0027](0027-vehicle-profiles.md) | Vehicle profiles: a named vehicle remembers its connection, its plotted signals and its recordings' name; a salted hash of the VIN recognizes it | Accepted |
| [0028](0028-app-fed-streams.md) | App-fed streams: the app opens the Bluetooth links core can't (Android SPP, BLE) and feeds an `elm327` session through `ecuio/stream.h` | Accepted |
| [0029](0029-android-background-sessions.md) | Android background sessions: the GUI thread keeps running, a foreground service keeps the process, one Bluetooth connect at a time | Proposed |

## Adding a record

Copy the shape of an existing record and use the next number. The
sections are **Context** (the forces at play), **Decision** (what was
chosen, stated so it can be checked against the code),
**Alternatives considered** (only where real ones existed) and
**Consequences** (good and bad). Add the record to the table above.
