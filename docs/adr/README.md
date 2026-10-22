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
| [0004](0004-signal-decoding-model.md) | Signal decoding: POD definitions, linear scaling, Intel byte order, message-ID gate | Accepted |
| [0005](0005-coalesced-signal-updates.md) | Coalesce signal updates to 10 Hz; pull API for the strip chart | Accepted |
| [0006](0006-frame-abi-v0.3.md) | Frame ABI v0.3 and the session close/destroy split | Accepted |
| [0007](0007-uri-selected-transports.md) | Transports selected by URI; failures surface at create | Accepted |
| [0008](0008-x86-surrogate-host.md) | Host 32-bit J2534 DLLs in an x86 surrogate process | Accepted; data plane and threading superseded by 0014 |
| [0009](0009-pe-architecture-routing.md) | Route in-proc vs surrogate at the loader by PE machine type | Accepted |
| [0010](0010-j2534-v0404-shared-types.md) | SAE J2534-1 v04.04 is the contract; one shared types header | Accepted |
| [0011](0011-vendor-call-relay-wire-forms.md) | Vendor-call relay: flat wire forms, identical ioctl whitelist at both ends | Accepted |
| [0012](0012-j2534-transport-adoption.md) | J2534 transport adoption: caller-chosen IDs, transport-driven request loop | Accepted |
| [0013](0013-protocol-profiles.md) | Protocol profiles: URI selection, profile-owned probe and request loop, declared CAN channels | Accepted |
| [0014](0014-single-threaded-surrogate.md) | Single-threaded surrogate child; reads travel over the pipe | Accepted |
| [0015](0015-request-engine.md) | Request engine: correlated, response-paced requests derived from signal definitions; read-only by construction | Proposed |

## Adding a record

Copy the shape of an existing record and use the next number. The
sections are **Context** (the forces at play), **Decision** (what was
chosen, stated so it can be checked against the code),
**Alternatives considered** (only where real ones existed) and
**Consequences** (good and bad). Add the record to the table above.
