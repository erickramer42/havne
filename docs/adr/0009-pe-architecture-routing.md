# ADR-0009: Route in-proc vs surrogate at the loader by PE machine type

- **Status:** Accepted
- **Date:** 2026-10-14 (Phase 3 Part 3, `d98792f`)

## Context

A caller should not have to know whether a vendor DLL is 32- or
64-bit, and the transport code should not fork on it either. Without
routing, every future capability would be implemented twice.

## Decision

- `qttune_j2534_open` reads the DLL's PE machine field once:

  | Machine type | Backend |
  |---|---|
  | x86 (I386) | Surrogate host ([ADR-0008](0008-x86-surrogate-host.md)) |
  | x64 | In-proc `LoadLibraryEx` |
  | Unknown (missing, corrupt or unreadable file) | In-proc |

  Unknown stays in-proc so that such a path still fails honestly with
  `QT_ERR_LOAD_FAILED`, instead of spawning a surrogate process for
  garbage.
- The loader handle records which backend it uses. Every vendor-call
  helper forwards on `is_surrogate` in a single line.
- Internal introspection and test hooks:
  - `qttune_j2534_backend_name()`
  - `qttune_j2534_inject_test_frame()`, a test hook routed like
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
