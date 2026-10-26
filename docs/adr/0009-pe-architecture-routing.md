# ADR-0009: Route in-proc vs surrogate at the loader by PE machine type

- **Status:** Accepted. The routing table is superseded by
  [ADR-0025](0025-one-j2534-backend.md) (appendix below); the sniff
  and the rule that a file that can't be a DLL spawns nothing carry
  over.
- **Date:** 2026-10-14 (Phase 3 Part 3, `d98792f`)

## Context

A caller should not have to know whether a vendor DLL is 32- or
64-bit, and the transport code should not fork on it either. Without
routing, every future capability would be implemented twice.

## Decision

- `ecuio_j2534_open` reads the DLL's PE machine field once:

  | Machine type | Backend |
  |---|---|
  | x86 (I386) | Surrogate host ([ADR-0008](0008-x86-surrogate-host.md)) |
  | x64 | In-proc `LoadLibraryEx` |
  | Unknown (missing, corrupt or unreadable file) | In-proc |

  Unknown stays in-proc so that such a path still fails honestly with
  `ECUIO_ERR_LOAD_FAILED`, instead of spawning a surrogate process for
  garbage.
- The loader handle records which backend it uses. Every vendor-call
  helper forwards on `is_surrogate` in a single line.
- Internal introspection and test hooks:
  - `ecuio_j2534_backend_name()`
  - `ecuio_j2534_inject_test_frame()`, a test hook routed like
    everything else. It exists because an x64 test process cannot
    load an x86 fake to reach its queue.

## Consequences

- `j2534_transport.cpp` doesn't know which backend it is using. Part 3
  left it byte-identical, and a session lifecycle runs over the
  surrogate with no changes to the transport.
- Every capability added at the loader inherits routing for free. The
  write/ioctl/filter relay was the first to do so.
- Each loader helper needs a matching surrogate op, and parity has to
  be tested on both backends.
- Writing this decision exposed an existing bug: the sniffer returned
  1 on unreadable files, contradicting its own header. It now returns 0
  (unknown). The bridge's architecture label still shows unknown as
  "x86" (Known Issue #18).

## Appendix — every DLL in a surrogate (2026-10-26)

[ADR-0025](0025-one-j2534-backend.md) replaces the table above: an x86
DLL loads in `ecuio-j2534-surrogate.exe` as before, and an x64 DLL, or
a PE for any other machine, in `ecuio-j2534-surrogate-x64.exe`, built
in the app's own tree and shipped beside it. Nothing loads in the
app's process. The sniff now tells apart what this record's "unknown"
lumped together: a file it can't open and a file that isn't a PE are
refused at the sniff with no spawn (the outcome in-proc
`LoadLibraryEx` gave them), and a PE for another machine goes to the
x64 surrogate, whose load answers as the app's process would. Each
surrogate has its own relocation variable, `ECUIO_SURROGATE_EXE` and
`ECUIO_SURROGATE_X64_EXE`.
