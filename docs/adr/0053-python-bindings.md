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

- **Amended 2026-11-07, the examples as built:** they live in
  `python/examples/` beside `read_pids.py`. The DID scan runs core's own
  scan (ADR-0020) through a session, so the same script scans a car
  through any adapter; `ecuio_sim_ask` is its test's oracle, not its
  engine. The notebook reads a committed synthetic recording, and CI
  carries no scientific stack, so it runs locally. See the appendix
  "the examples as built".

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

## Appendix: the shared core as built (2026-11-06)

Section 2 and 2a, built (step 1 of the bindings; the pull queue,
section 4, is the next).

- **The target:** `ecuio` (`core/CMakeLists.txt`), built where
  `HAVNE_SHARED_CORE` is on: by default every desktop platform, not
  Android or iOS (their apps link core into themselves) and not a
  fuzzing tree, which refuses it at configure, since its instrumented
  core links only into a harness. It is `ecuio-core`'s archive linked
  whole (`/WHOLEARCHIVE:` on MSVC, `--whole-archive` on GNU ld,
  `-force_load` on Apple's; CMake's floor, 3.21, predates
  `$<LINK_LIBRARY:WHOLE_ARCHIVE>`), so it holds every object the
  archive does, with an empty source of its own
  (`core/src/shared_core.cpp`). The archive is position independent
  off Windows. The app, `havne-cli` and the tests still link the
  archive.
- **The list:** `scripts/ecuio_exports.py` reads `ECUIO_PUBLIC_HEADERS`,
  `core/CMakeLists.txt`'s one list of public headers (what's installed,
  what `scripts/api_check.py` compares with a release, what's exported),
  with `api_check.py`'s parser, and writes `generated/ecuio/` at build
  time: `ecuio.exports` (a name a line), `ecuio.def`, `ecuio.map` (GNU
  ld's version script) and `ecuio.exp` (Apple's exported symbols list).
  It refuses a declared name without the `ecuio_` prefix. The headers
  declare functions only, so no data symbol is exported.
  `scripts/abi-check.sh` makes both trees' lists with it, so the `abi`
  job's shared object exports what the library does, by one rule for
  the release and for HEAD.
- **The checks** (`scripts/exports_check.py`, ctest, label `abi`,
  wherever the option is on):
  - `ExportsTest.TheLibraryExportsTheDeclarations`: the library's
    export table equals the list, read with `dumpbin /exports` (found
    beside `link.exe`), `nm -D --defined-only` or `nm -gU` (Mach-O's
    image header the one linker-made name let through).
  - `ExportsTest.EachAllocationHasItsRelease`: section 2's rule, with
    the release's name widened from `_free` to a word of free, close,
    destroy or unregister, since a handle's release is its close
    (`ecuio_recording_close`, `ecuio_sim_destroy`,
    `ecuio_stream_unregister`). A non-const `T**` parameter or a
    non-const `T*` return needs a function in the same header taking
    `T*` first; a `const` one is borrowed. Thirteen hand-outs on this
    date, each with its release.
  - `ExportsTest.SelfTest`, the rules' own cases.
  - `c_abi_shared`: `tests/test_c_abi.c` linked against the library,
    the calls it doesn't export (`internal.h`'s test hooks, the
    surrogate client's log path) left out.
  - `scripts/verify.sh`'s cross-arch step runs `c_abi_shared` and the
    exports check in `build-win32` too, on the x86 DLL, which CI never
    builds.
- **The soname:** `VERSION` is the project's; `SOVERSION` is
  major.minor while 0.x (`libecuio.so.0.6`), since a minor release may
  break the C ABI before 1.0 (ADR-0048 section 2), and the major from
  1.0 (`libecuio.so.1`). On macOS `compatibility_version` follows
  `SOVERSION` and `current_version` `VERSION`. Windows names no version:
  the DLL is loaded by path.
- **Built, not shipped:** nothing installs the library yet, so the core
  deb and tarball, the SBOM and the release's files stay as they were
  until the wheel (section 6) decides what ships with it.
- **`ecuio_version`** (section 2a, `core.h`): `version.h`'s three
  numbers as the library was built; `ECUIO_ERR_NULL_ARGUMENT`, writing
  nothing, when any of the pointers is NULL.

## Appendix: the pull queue as built (2026-11-06)

Section 4, built (step 2 of the bindings), in `ecuio/queue.h` (a public
header of its own) and `core/src/queue.cpp`.

- **One kind per queue, typed reads:** section 4's single
  `ecuio_queue_read` over frames, value batches or both became
  `ecuio_queue_open(session, kind, capacity, &out)` with one kind,
  `ECUIO_QUEUE_FRAMES` or `ECUIO_QUEUE_VALUES`, read by
  `ecuio_queue_read_frames` or `ecuio_queue_read_values`. A read of both
  would take a `void*` or a union of a 4120-byte frame and a value: both
  are awkward in ctypes, and the bindings' check of declarations against
  the headers (section 7) can check neither. A program wanting both
  opens two queues.
- **The value entry:** `ecuio_queue_value_t`, 160 bytes with no pointer
  (one layout on 32- and 64-bit builds): a value of `ecuio_value_t`'s
  with its frame's `timestamp_us` and `arbitration_id`, and its text (a
  TEXT or HEX value's, or an ENUM's label) copied into 128 bytes;
  `ECUIO_QUEUE_VALUE_TEXT_CUT` marks one cut at 127. One entry per value,
  so a full queue can drop part of one frame's values; `dropped` counts
  entries.
- **The bound:** slots made at open, 1 to `ECUIO_QUEUE_MAX_FRAMES` (4096
  frames, about 17 MB) or `ECUIO_QUEUE_MAX_VALUES` (65536); a full queue
  drops its oldest entry, and `dropped` is the running total since open.
  At most `ECUIO_QUEUE_MAX_PER_SESSION` (4) queues a session, opened
  before `ecuio_session_start` (`ECUIO_ERR_INVALID_ARGUMENT` after it, as
  `ecuio_session_record`).
- **The worker's side:** each queue shares a link with its session, as
  the recorder does (`core/src/pull_queue.h`): dispatch pins it, copies
  under the queue's mutex after the frame callbacks (frames) or the
  values callbacks (values), and wakes a reader only when one waits; a
  values queue makes dispatch decode, as a values callback or a
  recording does. Nothing allocates on the worker.
- **A read** copies up to its capacity, oldest first, letting go of the
  queue's mutex between chunks (32 frames, 256 values), and waits up to
  its timeout for the first entry. Once the session has closed and the
  queue is empty it returns `ECUIO_ERR_SESSION_CLOSED`. Two threads may
  read one queue; each entry goes to one of them.
- **Close (ADR-0002's ordering):** `ecuio_session_close` detaches every
  queue after the transport's detach has joined the worker, and wakes
  their readers; it frees none. `ecuio_queue_close` takes the queue off
  its link (waiting for the worker's pin), ends the reads under way and
  waits for them to return, then frees it, before or after its session
  is closed or destroyed. Reading or closing a queue after
  `ecuio_queue_close` is undefined, as for every handle.
- **Tests:** `QueueTest` (test_core, `tests/test_queue.cpp`; the
  internal `ecuio_queue_test_waiting` tells a case that a read on another
  thread waits), `CoreTest.WorkersAllocateNothingPerFrameIntoQueues`,
  `c_abi` and `c_abi_shared` (the layout and the NULL refusals).
- **Amended 2026-11-06 (step 3), the stream's end:** a replay that
  doesn't loop ends its session's queues when its last pass is over or
  it stops early (its file changed): what's waiting is still read, then
  a read returns `ECUIO_ERR_STREAM_ENDED` (21, "the stream has ended").
  Without it a script iterating a replay's values would wait forever,
  since only a frame count tells a replay's end and a values queue has
  none. Only a replay ends a stream; a looping replay and every live
  transport end only at the session's close. The end stays the answer
  after the close. `QueueTest.AReplaysLastFrameEndsItsQueues`,
  `.AReplayThatDropsFramesStillEnds`, `.ALoopingReplayEndsOnlyAtTheClose`.

## Appendix: the havne package as built (2026-11-06)

Sections 1, 3, 5 and 7, built (step 3 of the bindings; the wheel,
section 6, and the examples, section 8, are next), in `python/havne/`
with its tests in `python/tests/`.

- **The declarations are generated:** `scripts/ctypes_gen.py` reads the
  public headers (`ECUIO_PUBLIC_HEADERS`) with `api_check.py`'s parser,
  as the export list is made, and writes `python/havne/_ecuio.py`:
  every macro and enumerator as a constant, each enum as `c_int`, each
  struct and union with its fields, each opaque handle as an empty
  `Structure` (so a handle is a typed pointer, never a bare `c_void_p`),
  each callback typedef as a `CFUNCTYPE`, and every function's argument
  and result types, set on the loaded library by `bind(lib)`. The file
  is committed, so the package needs no headers at run time; section 7's
  check of declarations against headers became the generator run again
  and compared, which also catches a struct or constant out of date.
  `havne.c` is the module, for a call the classes don't wrap.
- **Finding the library:** `HAVNE_LIBRARY` when set and not empty (a
  path to the shared core, which must be a file), else the package's own
  folder (`ecuio.dll`, `libecuio.so.<n>`, `libecuio.dylib`), else
  `havne.LibraryNotFound`, an `ImportError` naming both places. Never
  the system's search path: a library found there is some other
  build's. On Windows the package sets section 3's surrogate variables
  from its own folder before the load, unless the environment sets them.
- **The version rule:** section 6's "another major version" is the
  rule from 1.0. While 0.x a minor release may break the C ABI (ADR-0048
  section 2, the soname's major.minor), so the package refuses a core
  whose major or minor differs from its own (`_version.py`, the
  project's version) and takes any patch. The refusal is
  `havne.LibraryVersionError`, an `ImportError` naming both versions and
  the library's path.
- **Errors:** a call that fails raises `havne.Error`, carrying the
  status number, its name (`ECUIO_ERR_BAD_URI`), core's words for it,
  `ecuio_last_error_detail` and the call's name.
- **Streams:** `Session.frames()` and `.values()` open a pull queue
  each (before `start()`, as core requires) and read it in batches of
  256, waiting at most 100 ms a read, so a script's Ctrl+C lands between
  reads. Iteration ends when the queue says the stream ended (a replay's
  last frame) or the session closed; the stream's `ended` says which.
  Each time the dropped count grows it's logged once on the `havne`
  logger, with the total. A `Stream` is read from one thread at a time
  (a lock around each read); closing the session from another thread
  ends the iteration. `Session.record()` records through core's
  recorder, with `note` and `mark`.
- **Signals through signal files only** (section 5): `havne.SignalFile`
  takes a file's text or path, loads it and registers its profile.
  Every file and plugin loaded stays loaded for the process's life,
  since a registered profile's set is borrowed by the sessions that use
  it; registering a name again replaces it for the sessions opened
  after, as in core.
- **The plugin, minimal:** `havne.Plugin(path)` loads a plugin's folder
  (its files read through core's `read` callback) or a packed one,
  lists `id`, `name`, `version` and `variants`, and `register(variant)`
  returns the profile name; `Session(uri, plugin=..., variant=...)`
  registers and opens through it. Declared but not wrapped, through
  `havne.c`: a VIN's proposed variants (`ecuio_plugin_match`), a
  variant's merged set and signal file (`ecuio_plugin_variant_set`,
  `_variant_file`, `_variant_find`), its simulated values
  (`ecuio_plugin_values`) and its fixtures (`ecuio_plugin_fixture`).
- **The rest of section 5:** `Recording` (the reader: signals, markers,
  `query`, `text_at`, `find_threshold`, `find_marker`; a profile or a
  signal file to read `frames.csv` through), `Sim` (on a Linux virtual
  bus, or `Sim.unbound(uri).ask(request)` on every platform),
  `Session.info`, `.support`, `.identity`, `.vin` and `.diagnostics`.
  `Session.clear(confirm=True)` passes the engine's and the vehicle's
  state to core's gate (ADR-0038), which judges it as for every front
  end; the tests clear only the simulated car.
- **Tests:** each `python/tests/test_*.py` is a ctest of its own,
  `PythonTest.<file>` (label `python`), under pytest through
  `python/tests/run.py`: exit 77, ctest's skip, without pytest or when
  every case in the file skipped, or a failure instead under
  `ECUIO_PYTEST_REQUIRED=1`. They load the tree's own shared core
  through `HAVNE_LIBRARY`. Every session they open is `mock://`,
  `replay://`, the simulated ECU, `vcan0` or the fake J2534 DLL.
  `PythonTest.test_declarations` runs the generator again and compares,
  compares the bound functions with the export list, and evaluates each
  header's `static_assert`s of sizes and offsets against the ctypes
  types. `PythonTest.test_vcan` is the Done-when's first clause: the
  simulated ECU on `vcan0`, a script opening `socketcan://vcan0` under
  `obd2` and printing the decoded PIDs.
  - **Where they run:** a tree registers them only when the tests'
    Python has the tree's pointer size, so `build-win32`'s x86 core runs
    them under a 32-bit Python (`scripts/verify.sh`'s cross-arch step,
    the J2534 case through the x86 fake and surrogate), which CI never
    builds. In CI, `core-and-tests`, `win-build`, `macos-build` and
    `linux-build` install pytest from `.github/pytest-requirements.txt`
    (hash-pinned) and run them with `ECUIO_PYTEST_REQUIRED=1`;
    `linux-build`'s `vcan0` runs `test_vcan`. `core-sanitizers` has no
    pytest, so they skip there.
- **Left for step 4:** the Done-when's other clauses (the same script
  against the ELM327 emulator on Windows; the wheel and its
  attestation). And the Windows wheel's C runtime: `ecuio.dll` imports
  `MSVCP140.dll` and `VCRUNTIME140.dll` (and `VCRUNTIME140_1.dll` on
  x64; the archive is built `/MD` for the app); python.org's Python
  carries the `vcruntime140` DLLs but not `msvcp140.dll`, so on a machine without
  the Visual C++ redistributable the load would fail. The wheel either
  carries the runtime's DLLs beside `ecuio.dll`, as the app's zip does,
  or the shared core links the runtime statically, as the surrogate
  does.
  Amended 2026-11-07: the shared core links it statically (the
  appendix "the wheel as built").

## Appendix: the wheel as built (2026-11-07)

Section 6, the rest of section 7 and the Done-when's other clauses,
built (step 4 of the bindings, the last; the examples of section 8
follow in `python/examples/`, where `read_pids.py` already is, not
`examples/python/`).

- **Writing it:** `packaging/python/make_wheel.py`, standard library
  only (a wheel is a zip with `METADATA` 2.1, `WHEEL` and `RECORD`), so
  there is no build backend to pin. It packs `python/havne/`'s `.py`
  files, the library beside them, on Windows both surrogates, and
  `LICENSE` and `core/THIRD_PARTY_NOTICES.txt` under the dist-info's
  `licenses/`. Two builds of a commit are the same bytes: entries
  sorted, every time `SOURCE_DATE_EPOCH`'s, fixed modes. A `-rcN` tag
  gives PEP 440's `0.6.0rc1` in the name and `METADATA`; the package's
  `__version__` stays `0.6.0`. It refuses a `_version.py` or a library
  (`ecuio_version`, read by loading it) of another version, a tag the
  library can't keep, a missing or misnamed surrogate. Beside each
  wheel, `<wheel>.components.json` for the SBOM: a job artifact,
  never a release file.
- **The wheels:** `win_amd64`, `manylinux_2_39_x86_64`,
  `manylinux_2_39_aarch64` and `macosx_13_0_arm64`, each tag given by
  `release.yml` and checked against the library itself (PE machine;
  ELF machine and newest `GLIBC_` version; Mach-O CPU and `minos`).
- **No x86 Windows wheel:** a 32-bit Python is rare, and the x86 core is
  still tested, by `scripts/verify.sh`'s cross-arch step under a 32-bit
  Python.
- **The Windows C runtime, linked in:** `ecuio.dll` links the runtime
  statically (`/MT`), as the surrogates do, and imports only
  `KERNEL32`, `ADVAPI32`, `bcrypt` and `WS2_32`. The other way, the
  runtime's DLLs beside it, fails inside a Python process that already
  loaded an older `msvcp140.dll` through another extension: Windows
  reuses a loaded DLL of that name, and an STL from VS 2022 17.10 on
  (whose `std::mutex` constructor is `constexpr`) crashes in
  `std::mutex` against an `msvcp140` older than 14.40. The runtime's
  separate heap is safe, since every allocation core hands out has its
  release in core (`ExportsTest.EachAllocationHasItsRelease`). On MSVC,
  core's sources compile a second time for it (`ecuio-core-mt`, an
  object library with `MSVC_RUNTIME_LIBRARY MultiThreaded`), only in a
  tree that builds the shared core; `ecuio-core` and `havne.exe` keep
  `/MD`. `make_wheel.py` refuses a library or surrogate that imports a
  runtime DLL (`vcruntime`, `msvcp`, `ucrtbase`, `concrt`, `vccorlib`,
  `api-ms-win-crt`), the twin of `stage.ps1`'s surrogate check.
- **Linux:** the library comes from `make-core-packages.sh`'s tree (the
  packages' reproducible build), its debug info stripped and its build
  ID kept. A wheel holds no symlinks, so it carries one file under its
  SONAME (`libecuio.so.0.6`), which the package's search finds by that
  name. Refused: a `NEEDED` outside manylinux's set (it needs `libc`,
  `libm`, `libstdc++`, `libgcc_s` and the loader), no SONAME, or a
  `GLIBC_` version above the tag's 2.39 (Ubuntu 24.04's, the runner's).
- **macOS 13.0:** Qt 6.12 needs 14.4 (`QT_SUPPORTED_MIN_MACOS_VERSION`
  in the kit's `Qt6ConfigExtras.cmake`), and core nothing past 13.0, so
  `release.yml`'s `macos` job builds a core-only tree, arm64, with
  `CMAKE_OSX_DEPLOYMENT_TARGET=13.0` for the whole tree, no per-target
  flag. CI's `macos-build` builds the app at Qt's minimum, so the 13.0
  build first runs on a release candidate. From macOS 11 a wheel tag
  names the major alone (pip offers a machine only `<major>_0`), so a
  library built for 14.4, as a CI tree with no deployment target may
  be, is tagged `macosx_14_0` (rounding up would refuse it on 14.x
  itself) and would install on 14.0 to 14.3, where its load fails; a
  release builds for an exact major, so its wheel claims no more than
  it keeps. A tag naming a minor from 11 on is refused, and so is one
  whose major is below the library's. The library may load only
  `/usr/lib` and `/System/Library` dylibs.
- **The release:** `release.yml`'s `windows` job builds `ecuio` beside
  the app and packs the wheel with both surrogates; each `linux` leg
  packs its arch's from the packages' tree; the new `macos` job builds
  and packs its own. Each job then runs `packaging/python/check_wheel.py`
  on its wheel under `ECUIO_PYTEST_REQUIRED=1`: installed into a fresh
  venv with `pip --no-index`, used from outside the tree with
  `HAVNE_LIBRARY`, `PYTHONPATH` and the surrogates' variables unset,
  the library found in `site-packages`, and `read_pids.py mock://`
  through it. `publish` adds the four wheels to the SBOM (purpose
  `LIBRARY`; each depends on Python 3.11 or later, and on Linux on
  glibc, libstdc++ and libgcc_s, on macOS on macOS 13.0; the Windows
  wheel lists the C runtime linked in), `SHA256SUMS.txt`, both
  attestations' subjects and the release's files.
- **Tests:** `WheelTest.SelfTest` (made-up PE, ELF and Mach-O files and
  a made-up package: the tags, every refusal, `RECORD`, two builds byte
  for byte) and `WheelTest.Installed` (`tests/wheel_installed.py`, in
  every 64-bit tree with the shared core: the tree's own library built
  twice to the same bytes, then `check_wheel.py`; on Windows the fake
  DLL through the wheel's x64 surrogate, and the Win32 tree's fake
  through its x86 one where that tree exists, which CI's has not).
  Mutation-checked 2026-11-07: each of make_wheel's refusals, the
  sorting, the epoch's time, the modes and `RECORD`'s hashes (18); the
  package's SONAME search and its surrogate setup (2).
- **The Done-when's script:** `python/examples/read_pids.py <uri>`
  prints each of the engine speed, vehicle speed and coolant
  temperature as `name: value unit` and exits 0 once all three came.
  `PythonTest.test_vcan` runs it on `vcan0` against the simulated ECU,
  `PythonTest.test_read_pids` against `mock://` and against the ELM327
  emulator on loopback (`havne-cli sim --elm327 tcp:0`, ADR-0055) on
  every platform, Windows included, and `check_wheel.py` through each
  installed wheel. The last clause, a wheel installed from a release's
  downloads passing `gh attestation verify`, is the release skill's
  check on the first release that carries the wheels.

## Appendix: the examples as built (2026-11-07)

Section 8, built in `python/examples/`. Each runs against `mock://`,
the simulated ECU or a committed synthetic recording, never a car.

- **`did_scan.py <uri> [--range F180-F1FF] [--ecu 7E8 ...]`:** core's
  own DID scan (ADR-0020) through a session: the URI under
  `uds-generic` with one `did=` range per ECU (`<rx>:` for any but
  `7E8`), so it asks Service 0x22 only. Each answer prints once, as
  text when every byte is printable, as hex otherwise; then each ECU's
  scanned, found and refused counts from its channel's support. A URI
  that already names a profile is refused. Done when every ECU's scan
  completes (`ECUIO_SUPPORT_SCAN_COMPLETE`), or, for a transport that
  answers DIDs itself with no scan to count (the mock), when every DID
  of the range answered; exit 0 then, 1 when `--seconds` run out first,
  2 for a bad argument.
- **`isotp_fuzz.py --interface vcan0 [--cases 200] [--seed 1]`:**
  malformed ISO-TP exchanges against the simulated ECU, from a seeded
  generator, so a failure replays alone (`--case K`). Core's
  `fuzz_isotp` already feeds the reassembly bytes in memory; on a bus
  the cases add time and state: the sim's N_Cr wait for a Consecutive
  Frame, its N_Bs wait for a Flow Control, the tester's Flow Control
  abused while the sim sends a segmented reply (an overflow, waits, two
  in a row, none, a new request instead, a functional one), a new
  request in the middle of a message, CAN FD lengths where the bus
  takes them. After each case a probe (`01 0C` on 0x7E0) must be
  answered, and the sim must not report its bus failed. It stops at
  once, with words, when its sim yields to another ECU on the bus or
  three probes in a row go unanswered: a shared bus fails in seconds,
  where every case waiting out its probe would take minutes. Only an
  interface whose netlink kind is `vcan` is accepted, with no
  override. The standard library's CAN sockets carry the frames; the
  package only starts the sim, and `--ecu-running` or `--ecu-cli
  <havne-cli>` (`havne-cli sim`, which must then end with exit 0 and
  its end line not failed) need no package at all.
- **`ecu_states.ipynb`:** whether a recording's own signals say what
  the engine does half a second later, for one state, fuel cut (lambda
  over 1.3 with the engine turning). The reader's columns at 10 Hz give
  the features (engine speed, speed and throttle, how much each moved
  over 0.5 s, how long the throttle has been closed); a logistic
  regression and a depth-3 decision tree are judged on five blocked
  folds against a baseline that says the state stays as it is, where
  it changes, and the tree's rules are printed beside the drive
  cycle's own. Its last cell checks the tree is right at more than half
  the changes, where the baseline is right at none. It needs numpy and
  scikit-learn, the examples' requirement only.
- **The notebook's data:** `python/examples/data/drive-loop/`, one
  loop of the mock's drive cycle from key-on to the engine's stop:
  `events.log` and `signals.csv` with the four signals the notebook
  reads, under 500 KB, `mock://demo` as its uri, its times fixed at
  2026-01-01 00:00 UTC and no label. `data/make_drive_loop.py` makes it
  again through core's recorder in about three minutes; the mock's
  timing makes a new run differ in its rows' times only.
- **Tests:** `PythonTest.test_did_scan` (the mock on every platform; on
  `vcan0` the sim's second module, `modules=2`, against the unbound
  sim's own answers to each DID of the range, the engine ECU refusing
  them all) and `PythonTest.test_isotp_fuzz` (Linux, `vcan0`: 200
  cases, one replayed alone, no ECU, the ECU closed mid-run, a second
  ECU already on the bus, `lo` refused) under pytest, both serial as
  `vcan0` is shared; `ExampleTest.isotp_fuzz_cli`
  (`tests/example_isotp_fuzz.py`, outside pytest, so `core-sanitizers`
  runs it on its ASan `havne-cli`, where a sanitizer's report fails the
  sim's exit) and `ExampleTest.notebook` (`tests/example_notebook.py`:
  the notebook run top to bottom through `nbconvert`, in a kernelspec
  of the test's own naming the test's Python, so a user's `python3`
  kernel can't stand in; exit 77 without scikit-learn, nbconvert or
  ipykernel, as in every CI job). Mutation-checked 2026-11-07: core's
  N_Bs timeout and the sim's reset on a timed-out reply (2); the
  scan's completion, its ECUs' prefixes and reply IDs, the probe, the
  vcan refusal and both stops (8).
