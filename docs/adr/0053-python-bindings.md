# ADR-0053: Python bindings: ctypes over the C ABI, a wheel per platform

- **Status:** Accepted.
- **Date:** 2026-11-06.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (the Qt-free
  core, the C ABI as the boundary, the product's name only where a user
  sees it); [ADR-0002](0002-callback-pair-identity-snapshot-dispatch.md)
  (callbacks on the worker); [ADR-0025](0025-one-j2534-backend.md)
  (every J2534 DLL in a surrogate); [ADR-0030](0030-signed-android-releases.md)
  and [ADR-0035](0035-linux-downloads-sbom-reproducible.md) (what a release ships and how
  it is attested); [ADR-0031](0031-command-line-client.md) (`havne-cli`: what an
  outside program can do); [ADR-0038](0038-clearing-trouble-codes.md)
  (the clear's gates); [ADR-0046](0046-simulated-ecu.md) (the simulated
  ECU); [ADR-0048](0048-semantic-versioning.md) (what a version may
  break); [ADR-0049](0049-recordings-in-core.md) (recordings and their
  reader); [ADR-0052](0052-plugin-framework.md) (plugins).
- **Leads to:** v0.6.0's bindings, their wheel on each release, and the
  examples against the simulated ECU.

## Context

v0.6.0 is for people who script, pen testers among them: everything a
user can do in the app or `havne-cli` should be reachable from Python
too. The C ABI was built for this. It has opaque handles, flat
`extern "C"` functions and POD structs, every name prefixed `ecuio_`,
public headers that compile as strict C, and enum values that only
grow. `havne-cli` already proves that the public headers are enough to
connect, stream, record, read recordings, load plugins and run the
simulated ECU.

Three things are missing:

- **A shared library.** Core builds as a static archive (`ecuio-core`),
  linked into `havne.exe`, `havne-cli` and the tests. A Python process
  can load only a shared library. The `abi` CI job already builds a
  shared object that exports only the public `ecuio_` names (a version
  script of exact names) to compare releases, so the export list
  exists; nothing ships one.
- **A home for the J2534 surrogates.** Core finds
  `ecuio-j2534-surrogate.exe` and its x64 twin next to the running
  executable, or at `ECUIO_SURROGATE_EXE` / `ECUIO_SURROGATE_X64_EXE`.
  Under Python the running executable is `python.exe`.
- **A way to receive frames without callbacks into an interpreter.**
  Frames and values arrive on core's worker thread. A Python callback
  runs there only while holding the interpreter's lock, so a slow
  script, or one busy on another thread, would stall the worker, the
  request engine's timing and every other callback.

## Decision

### 1. ctypes, over the C ABI only

The bindings are pure Python on the standard library's `ctypes`. Each
public function is declared once, with its argument and result types,
in a module generated from the public headers and checked against them
by a test (section 7). Nothing is compiled at install time, and nothing
binds core's C++.

- **Why not cffi:** its API mode compiles a module per Python version
  and platform, and its ABI mode is ctypes with another dependency.
- **Why not pybind11 or nanobind:** they bind C++, which would make
  core's internals a second public surface and break ADR-0001's
  boundary. The bindings see exactly what `havne-cli` sees.

A thin Pythonic layer sits on the declarations: `Session` and
`Recording` as context managers that close their handles, iterators
for frames and values, exceptions that carry core's status name and
detail text, and `bytes` for payloads.

### 2. A shared core, exporting the ABI and nothing else

Core gains a shared build of the same sources, `ecuio` (`ecuio.dll`,
`libecuio.so`, `libecuio.dylib`). It exports the public names only:
the `abi` job's version script on Linux, an exports list on macOS and a
`.def` on Windows, all generated from one list. `internal.h`'s seams
are never exported. The static archive stays as it is for the app,
`havne-cli` and the tests.

The list is generated from the installed public headers' declarations,
never kept by hand, and CI checks the built library against it:

- **The exports equal the declarations:** every public function is
  exported, and nothing else is (`dumpbin /exports` on Windows, `nm -D`
  on Linux and macOS, compared with the headers' list).
- **Every allocation has its free:** a public function that hands the
  caller memory core allocated names its own `_free` in the same
  header (`ecuio_obdb_free` and `ecuio_signal_file_free` today), and
  the check fails on one that doesn't. A program that frees core's
  memory with its own C runtime is the classic crash across a Windows
  DLL boundary, since the two runtimes keep separate heaps.

### 2a. A numeric version

Core gains `ecuio_version(uint32_t* major, uint32_t* minor, uint32_t*
patch)`, an append-only addition beside `ecuio_version_string`, so a
program that loads the library compares numbers instead of parsing
text. The package's load-time check (section 6) uses it.

### 3. The surrogates travel with the library

On Windows the wheel carries both surrogate executables beside
`ecuio.dll`. Before the library loads, the package sets
`ECUIO_SURROGATE_EXE` and `ECUIO_SURROGATE_X64_EXE` to their absolute
paths, unless the caller's environment already sets them. Core's lookup
doesn't change: those variables already exist, are checked for a file
of the right name, and are named in the spawn line.

### 4. No Python on the worker thread

A ctypes callback is Python: it takes the interpreter's lock on the
worker, so even a handler that only appends to a list waits whenever
the script's own thread holds the lock. The bindings therefore register
no callbacks. Core gains a **pull queue** instead, an append-only ABI
addition that any foreign-function caller can use (Rust, Go and C#
have the same problem):

- `ecuio_queue_open(session, kinds, capacity, &out)` attaches a bounded
  queue of frames, value batches or both, before `ecuio_session_start`.
  Core's own handler copies into it on the worker and returns; a full
  queue drops the oldest entry and counts the drop, as `havne-cli`'s
  ring does.
- `ecuio_queue_read(queue, out, capacity, &count, &dropped, timeout_ms)`
  copies what's waiting into the caller's array (the serial ports'
  contract for sizes) and blocks up to the timeout when nothing is.
  ctypes releases the interpreter's lock for the call, so a script
  waiting on the bus blocks no other thread.
- `ecuio_queue_close(queue)` detaches it; `ecuio_session_close` closes
  any still open, after the worker has stopped, so no copy reaches a
  freed queue (ADR-0002's ordering).

The iterator over a session's frames or values reads in batches and
reports the dropped count once each time it grows. A script that needs
every frame records the session and reads the recording.

### 5. What's bound, and the gates

- Sessions: every transport URI the platform supports, profiles, signal
  files and plugins, recording on or off, trouble codes and readiness,
  identification.
- Recordings: ADR-0049's reader (open, signals, query, `text_at`,
  `find`, markers) and the frames path that decodes `frames.csv`
  through a profile.
- The simulated ECU (ADR-0046), and its direct `ecuio_sim_ask`
  (ADR-0055): a script asks it a request and reads each node's reply
  with no bus, on every platform.
- **Signal sets through signal files:** a script defines its own
  signals as a signal file (ADR-0051's JSON, given as text or a path)
  and loads it through `ecuio_signal_file_*`, never by filling
  `ecuio_signal_def_t` structs in Python. `ecuio_validate_signal_set`
  refuses any set whose `layout_version` isn't core's, and the layout's
  next version (a wider bit length for a DBC signal's 64 bits, say)
  would refuse every script that built one by hand; a signal file
  reads the same across layouts.
- The clear goes through core's own gate (ADR-0038). The binding takes
  `confirm=True` as `havne-cli clear --yes` does, and writes nothing
  core wouldn't. The bindings add no write of their own: a write that
  core refuses stays refused from Python.

### 6. The wheel

- **One wheel per platform:** `havne-<version>-py3-none-<platform>.whl`
  for `win_amd64`, `manylinux_2_39_x86_64`, `manylinux_2_39_aarch64`
  (Ubuntu 24.04's glibc, the AppImage's floor) and
  `macosx_<min>_arm64`. Each holds the package and its library, and on
  Windows the surrogates. `py3-none` because ctypes ties the wheel to
  no Python version or interpreter ABI. The minimum Python is the
  oldest one still supported upstream when the wheel is built
  (3.11 today), tested in CI.
- **Versioned with Havne:** the wheel's version is the release's. The
  package reads core's version through `ecuio_version` (section 2a) at
  load and refuses a library of another major version, with words
  naming both.
- **Attached to each release** beside the other downloads, in
  `SHA256SUMS.txt`, the provenance bundle and the SBOM (ADR-0035's
  pattern), built by `release.yml` from the tagged commit.
- **Not published to PyPI.** A release asset is enough for v0.6.0's
  users, and a PyPI name is a public, long-lived choice made apart from
  this one. A repository of its own stays later (the potential
  roadmap's "Repo splits").

### 7. Tests

- **Declarations against headers:** a test parses the installed public
  headers' function declarations and compares each with its ctypes
  declaration (names, argument and result types), so a new or changed
  function can't go unbound or bound wrong.
- **pytest on every desktop platform in CI:** sessions against
  `mock://` and the replay fixture, the reader against a recording made
  in the test, the simulated ECU through `ecuio_sim_ask`; on Linux,
  SocketCAN against `vcan0` with the simulated ECU answering.
- **The wheel itself:** built, installed into a fresh virtual
  environment and imported in CI, then a `mock://` session streamed
  through it, so the package works as shipped, not only from the tree.
- **The queue:** core's own tests (Qt-free, C ABI) for its order, its
  bound and drop count, its timeout and its close against a running
  worker under the sanitizers; from Python, a script that never reads
  loses frames by count while the worker's timing stays within the
  bench's bounds.

### 8. Examples

`examples/python/`, each runnable against the simulated ECU with no
car: a DID scan through `ecuio_sim_ask`, ISO-TP fuzzing of the
simulated ECU on `vcan0`, and a notebook predicting ECU state changes
from a recording (scikit-learn, the examples' own requirement, never
the package's). The examples' outputs are synthetic: they run against
the simulated car, so no VIN or identifier of a real one appears.

## Done when

A Python script opens SocketCAN, decodes PIDs and prints the values;
the same script runs on Windows against the ELM327 emulator (ADR-0055)
and on every platform against `mock://`; the wheel installs from a
release's downloads and passes `gh attestation verify`.

## Alternatives considered

- **A C extension module:** fastest per call, but compiled per Python
  version, so one wheel per platform becomes one per platform and
  interpreter, for calls whose cost is the bus, not the binding.
- **Python callbacks on the worker:** the simplest binding, and the one
  that lets a `print` in a script, or the interpreter's lock held by
  another thread, delay the request engine. Refused.
- **A queue in a small compiled helper of the binding's own:** keeps
  core's ABI as it is, but brings back a compiled module per platform
  and leaves every other language's binding the same problem.
- **Shelling out to `havne-cli`'s JSON lines:** works today with no
  binding, and stays a fine choice for simple scripts; it can't share a
  session's handle, read a recording's query, or ask the simulated ECU
  directly.
- **PyPI from the start:** easier `pip install havne`, but a public
  package name and an upload credential are decisions of their own.

## Consequences

- Core ships a shared library for the first time, so its export list
  becomes a promise: a public function added is exported, and the
  `abi` job's comparison covers what the wheel loads.
- Every public function needs a ctypes declaration in the same change,
  which the header test enforces, and an entry in the generated export
  list, which the exports check enforces.
- A public struct that grows only through its reserved room, and an
  array element (`ecuio_enum_entry_t`, `ecuio_channel_decl_t`,
  `ecuio_obdb_note_t`, whose size is the array's stride) or a struct
  with none (`ecuio_dtc_list_t`), grows as a new struct beside it.
- Core's ABI grows by the pull queue, which the app and `havne-cli`
  don't need and which every other language's binding can use.
- The release grows by four wheels, and its checks (checksums,
  provenance, the SBOM) grow with them.
- Scripts get frames at the rate they read them, with drops counted;
  a script that needs every frame records the session and reads the
  recording.
