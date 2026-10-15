# QtTune — Project Status

**Date:** October 14, 2026
**Version:** v0.3.0-dev (ABI-breaking frame redesign + lifecycle split); v0.2.0 still tagged. Phases 1–3 HARNESS-complete incl. URI query params, device picker, x86 surrogate host, and PE-architecture routing — remaining: WRITE/IOCTL relay, BENCH-tier live-DLL verification, live-vehicle read.
**Repository:** https://github.com/erickramer42/qttune
**CI:** [![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

QtTune is a cross-platform ECU reader/logger, evolving into a full tuning
application. The guiding architecture principle: **a Qt-free core library
behind a stable C ABI, with UI layers as replaceable skins around it.**

## Architecture

+----------------------------+
|   Qt Quick UI (desktop)    |   Responsive QML shell — sidebar above
|   Main / Dashboard / Live /|   720 px width, bottom TabBar below
|   Log / Settings pages     |   LogPage: live frame table, sort toggle
|   LiveViewPage: signal     |   SettingsPage: J2534 device picker +
|   table + strip chart      |   manual URI connect (done)
+---------------+------------+
                | QtTuneBridge (thread-marshaling seam)
                |   worker→GUI via QMetaObject::invokeMethod (QueuedConnection)
                |   + 100ms coalesce timer draining decode staging buffer
+---------------v------------+
|         qttune-core        |   C++, C++17, C ABI exports
|  * Session lifecycle       |   ZERO Qt dependencies (CI-enforced)
|  * Version/status reporting|
|  * Frame callbacks (done)  |
|  * Transports:             |   pluggable vtable ops — mock (all
|    mock (done), j2534      |   platforms) + j2534 (Win32) attach via
|    (Win32, HARNESS) (done) |   URI routing in qttune_session_create
|  * J2534 loader routing:   |   PE sniff at open() — x86 DLLs delegate
|    in-proc | surrogate     |   to the surrogate child, x64/unknown
|                            |   load in-process. Transport layer is
|                            |   single-path, backend-agnostic.
+-------------+--------------+
              | named pipe (control) + shared-memory SPSC ring (data)
              | + kill-on-close job object
+-------------v--------------+
|  qttune-j2534-surrogate    |   Win32 x86-capable host process; loads
|  (surrogate host exe)      |   32-bit vendor DLLs an x64 app cannot
+----------------------------+

Rules the project is built around:

1. **Core never links Qt.** Enforced in CI on a runner with no Qt installed.
2. **UI layers receive final display values, never raw frames.** All
   parsing, decoding, and unit conversion lives in core. (Applies to the
   future mobile client too: the C ABI supports a native or Flutter
   front end without changes.)
3. **The C ABI is the FFI boundary** — stable handles, flat functions,
   POD structs. Enum values evolve append-only; anything above the ABI
   is disposable.
  
* Frame struct redesigned in v0.3.0: `arbitration_id`, `flags` bitfield (extended/FD/etc.), session-relative timestamps, 64-byte payload capacity. Previous struct (`is_extended` flag, 8-byte payload) retired. See commit message `feat!: frame ABI ...` for migration details.

## Data Path

    [MockTransport worker thread]         std::thread, 50ms tick
    — or —
    [J2534 transport worker thread]       std::thread, 20ms poll,
                                          ReadMsgs → qttune_frame_t
    (Win32 only; ISO 15765, j2534:// URI) (in-proc directly, or drained
                                          from the surrogate's shared
                                          ring — identical callback path)
              │  qttune_frame_t (stack-owned)
              ▼
    core: snapshot-dispatch callbacks    (register/unregister safe mid-dispatch)
              │  QtTuneBridge::onFrame   — copies POD, checks in-flight gate,
              │                           posts QueuedConnection lambda
              ▼
    GUI thread: handleFrameInternal      — hex display values to FrameListModel
              │                           + qttune_decode_signal per frame
              │                             into staging buffer
              ▼
    100ms coalesce timer ──drain──▶ SignalListModel.updateValues()
              │                        one dataChanged per window
              │                        (contiguous rowrange, ValueRole)
              │                        + valuesUpdated + history ring push
              ▼
    QML LiveViewPage: value table + strip chart Canvas (redraws on
    valuesUpdated, pulls data via chartData())

Frame-log path (unchanged from v0.2.0):

    handleFrameInternal → FrameListModel (arrival order, 10k ring,
    back-eviction) → FrameSortProxy (newest/oldest toggle) → LogPage

Contract highlights:

- UI receives final display values, never raw frames (rule 2).
- Backpressure bounds IN-FLIGHT queue depth (posted-but-undrained
  lambdas, cap 1000), NOT cumulative frame count. Model ring eviction
  handles memory. Stream never freezes.
- GUI emissions for the LiveView path: one batch per 100ms window
  (10 Hz), decoupled from frame rate (mock 20 fps, future J2534 500 Hz).
- Strip-chart history timestamps are BATCH-stamped (one timestamp per
  coalesce window, from a model-side QElapsedTimer restarted per
  connect). Honest resolution given the staging buffer merges latest-per-ID across the window; per-sample timestamps arrive when the F2 message-ID gate activates (decode worker-side under J2534).
- J2534 teardown ordering is a hard contract: worker joined BEFORE
  `PassThruDisconnect`/`FreeLibrary` — the worker calls ReadMsgs through
  function pointers that dangle the instant the module unmaps.

## Signals Layer (landed v0.3.0 Phases 1 / 1b)

POD signal metadata + linear decode: `display = raw * scale + offset`.

- `QttuneSignalDef` — POD struct: byte/bit position, type enum, scale/offset, 
  unit string, `message_id` (CAN arbitration ID for filtering), display hints, reserved fields for ABI-stable growth. Size pinned at 88 bytes (ABI check in `test_signals.cpp`).
- `qttune_decode_signal(def, payload, len, out)` — Intel (LSB-first) bit
  extraction, sign extension for INT types, IEEE-754 passthrough for
  FLOAT32. Standardized error codes (NULL=1, bounds=2, bit params=3,
  unsupported type=4).
- `qttune_validate_signal(def)` — structural validation; fixed-width strings
  must terminate within buffer.
- `QttuneSignalSet` — swappable definition collections; set origin is
  opaque to the decoder. Mock ships a built-in set
  (`qttune_mock_signal_set`); future providers (DBC loader, manufacturer
  plugins) construct sets through the same struct.
- **Motorola (MSB-first) bit numbering is a deliberate non-goal** until a
  flags bit selects it — first customer of a reserved flags bit is byte
  order (DBC mixes Motorola and Intel freely).
- **Message-ID gating (v0.3.0 Phase 2b):** signals declare the CAN arbitration 
  ID they ride on; the bridge decodes only frames matching that ID. Mock set
  declares `MOCK_TELEMETRY_ARB_ID = 0x0C0`. J2534 transport reports zero `arbitration_id` until filter-layer support lands (documented inertness —
  frames flow, signals stay empty, nothing corrupts silently).
- **Mock emission (Phase 1b):** tick-driven simulation (idle RPM wobble +
  rev blips, coolant warm-up toward thermostat, trailing IAT, coupled
  throttle/load), encoded as the inverse of the signal definitions.
  Transport never decodes (rule 2); round-trip enforced by integration
  test because byte-order/scale inversions pass structural checks but
  produce absurd physics — only range assertions catch that class.

## Coalesced Signal Path + Strip Chart (landed v0.3.0 Phase 2, closed 2026-10-10)

- `handleFrameInternal` decodes the mock signal set per frame into
  `m_stagingBuffer` (GUI thread; decode ABI is pure computation).
- 100ms `QTimer` (`m_coalesceTimer`) drains staging into
  `SignalListModel::updateValues()` — one contiguous-rowrange
  `dataChanged` for the whole batch, started on connect, stopped on
  disconnect.
- `SignalListModel` — QAbstractListModel over the signal set: name, unit,
  value, subscribed roles; `toggleSubscribed(row)` Q_INVOKABLE drives the
  LiveView checkboxes. Value updates filter on `subscribed`.
- **History ring (per signal):** 120-sample fixed-capacity evict-oldest
  ring inside each model `Entry` — `Sample { timestampMs, value }`,
  batch-stamped at drain time (see Data Path contract note). No ABI
  surface touched; ring lives Qt-side so the existing `test_bridge`
  target covers it headlessly-adjacent (Qt Core only). Unsubscribe
  freezes history; resubscribe restores the accumulated trace.
- **`chartData()` pull API:** one Q_INVOKABLE returning a flattened
  `[ts, val, ...]` sample list per subscribed signal, oldest-first —
  chosen over a HistoryRole/data-role approach because a Canvas paints
  outside delegate context; one call per repaint instead of per-row role
  lookups.
- **`valuesUpdated`** — one emission per non-empty batch, the Canvas's sole
  repaint trigger. (Was declared since v0.2 era and never emitted —
  a signal promise QML never received until this became its first
  consumer.)
- Bridge metrics: `guiNotifyCount` (per-frame instrumentation, baseline
  1:1 with frames) and `signalBatchCount` (~10/s at any frame rate).
  Baseline: 200 frame notifications / 10 s at mock rate → LiveView path now ~10
  batch emissions / 10 s (100ms windows). **~20× reduction, decoupled from frame rate.**
- LiveViewPage QML: subscription checkboxes, tabular-digit value labels,
  and strip-chart Canvas — shared time axis across series, per-series
  value scaling (RPM vs coolant on one shared y-axis would flatten the
  small signals into noise), legend with live values. Wired into
  sidebar/TabBar/StackLayout at index 2 (Home, Dash, **Live**, Log, Setup).
- Known inefficiency (deliberate, mock-rate harmless): decode loop runs
  all signals per frame regardless of subscription; filter decode on
  `isSubscribed(id)` when signal counts grow.

## J2534 Transport (landed v0.3.0 Phase 3, HARNESS tier)

Transport stack for real Pass-Thru devices (Windows). **Verified against
a fake vendor DLL end-to-end, in BOTH same-arch and cross-arch (x64 app →
x86 surrogate → x86 fake) configurations; discovery additionally verified
against real installed drivers — no vendor DLL other than the fake has
been loaded and no live vehicle has been connected.** Landmarks, in commit order:

- **`844f44e` — transport vtable refactor:** `qttune_transport_ops_t`
  ({start, detach}); session routes through ops only. Mock migrated with
  zero test changes (behavior-preserving).
- **`1752fc9` — registry discovery:** enumerate Pass-Thru Support
  subkeys in BOTH WOW64 views (KEY_WOW64_64KEY / _32KEY flags, not
  hardcoded WOW6432Node — works from any process bitness); read
  `FUNCTIONLIBRARY` per vendor; dedupe across sweeps (dll_path +
  vendor name). C-ABI array out, NULL-safe free.
- **`9b33aeb` / `6b58c3a` / `0fa4883` — fake DLL + status code + loader:**
  `tests/fake_j2534.cpp` implements the seven mandatory PassThru exports
  with SAE J2534-1 signatures (`__stdcall`, `long` status return,
  32-bit `unsigned long` IDs) plus three test hooks. Loader
  (`qttune_j2534_open`) does `LoadLibraryExA(..., LOAD_WITH_ALTERED_SEARCH_PATH)`
  → resolve all 7 exports → `PassThruOpen`, with single-site failure
  unwinding (no leaked HMODULE on any failure path). `QT_ERR_LOAD_FAILED`
  added append-only to the status enum.
- **`SESSION-INTEGRATED TRANSPORT (Commit C):** `j2534_transport.cpp`
  plugs into the session vtable. Handle opens at ATTACH (load failures
  surface at `qttune_session_create`, where the caller can pick another
  URI); `PassThruConnect` (ISO 15765, 500k) happens at START;
  `ReadMsgs` polling on a worker thread (20ms poll interval) dispatches
  frames through the identical callback path as the mock.
- **`URI QUERY PARAMETERS (Commit D, 2026-10-14):** `?baud=<decimal>` and
  `?flags=<0xHEX|decimal>` parsed strictly in `resolve_uri` — the query is
  split off BEFORE selector resolution, which also fixed two latent bugs
  (a query glued onto a vendor name defeated discovery matching; a query
  on a path form reached `LoadLibraryEx` as part of the filename). Params
  thread transport → `qttune_j2534_connect_channel(handle, baud, flags, ...)`;
  baud=0 is the internal "unset" sentinel (explicit `baud=0` is rejected
  at parse) resolved to the ISO 15765 500k default in the loader TU —
  the one TU owning the vendor ABI constants. flags pass through
  verbatim with NO bit semantics interpreted pre-BENCH (pending the
  vendor `j2534.h` diff). Malformed queries (unknown keys, `baud=abc`,
  `baud=0`, bare `?`, trailing `&`) reject at `qttune_session_create`
  with `QT_ERR_NO_TRANSPORT` before any DLL load. Fake gained a
  `GetLastConnectParams` hook recording the last `PassThruConnect`
  invocation — the existing stream tests couldn't have caught a
  regression here (any hardcoded connect params would satisfy them);
  `DefaultConnectUsesIso15765At500k` now pins behavior preservation.
- **`URI grammar (full):** `j2534://[<selector>][?baud=<decimal>[&flags=<0xHEX|decimal>]]`.
  Selector: `` (first discovered device), `<vendor-name>` (case-insensitive
  discovered match), `<path ending .dll or containing \ or />` (direct load —
  the test/dev route to the fake). Misses return `QT_ERR_NO_TRANSPORT`,
  preserving `test_core`'s `UnknownTransportUriRejected`.
- **`DEVICE PICKER + REAL-VENDOR DISCOVERY (Commit E, 2026-10-14):**
  Bridge exposes `discoverJ2534Devices()` (QVariantList of
  {vendorName, dllPath, architecture} — display values only, rule 2) and
  `currentUri` (cleared on failed connect). Discovery gained a DUAL-ROOT
  sweep: the SAE-spec `Pass-Thru Support.04.04` plus the no-hyphen
  `PassThruSupport.04.04` variant real installers write (observed: OBDX
  Pro, in WOW6432Node only), 2 roots × 2 WOW64 views = 4 sweeps, dedupe
  absorbs overlap. Discovery annotates each device's PE architecture
  (`reserved[0]`: 1=x86, 2=x64, 0=unknown) via header sniff — feeds both
  the picker's labeling and (authoritatively, re-sniffed at open) the
  Part 3 routing decision. First real-contact finding: the OBDX Pro VX
  subkey declares ISO15765=1, a genuine connect candidate.
- **`X86 SURROGATE HOST (2026-10-14/15, Phase 3 Part 2):** a separate
  Win32 executable, `qttune-j2534-surrogate`, that loads a 32-bit vendor
  DLL an x64 process physically cannot. Architecture: the app-side client
  (`core/src/j2534_surrogate_client.cpp`, `qttune_j2534_sur_*` C ABI)
  creates all named objects (pipe/mapping/event — app-generated names,
  passed on the child's command line; the child can never know them in
  advance), spawns the surrogate suspended under a
  KILL_ON_JOB_CLOSE job object (closing the job handle kills the child
  on any teardown path, including app crash), then handshakes OP_OPEN.
  Control plane: overlapped byte-mode named pipe with bounded-timeout
  transact() and request-id desync detection. Data plane: shared-memory
  SPSC ring (4096 slots, producer owns head, consumer owns tail, power-of-two
  index masking) + auto-reset event for consumer wakeup. Surrogate-side:
  control pump (OPEN/CONNECT/DISCONNECT/SHUTDOWN/INJECT) + read pump
  (PassThruReadMsgs poll → ring push, gated on channel != 0). Diagnosability:
  distinct child exit codes 1–7 per failure stage, plus client-side
  SUR_LOG failure-path tracing. Deployment: exe resolves next to the
  running image (`QTTUNE_SURROGATE_EXE` env override for cross-arch
  dev runs); CMake copies it next to every test exe that spawns it.
- **`fake_j2534.def`:** undecorated export names via linker def file —
  mandatory because x86 MSVC decorates `__stdcall` exports
  (`_PassThruOpen@8`), breaking `GetProcAddress("PassThruOpen")` in the
  surrogate (surfaced as exit code 5 in the cross-arch round). Real
  J2534-1 vendors use the same mechanism; bare EXPORTS entries emit
  undecorated names on both x86 and x64.
- **`PE-ARCHITECTURE ROUTING (2026-10-15, Phase 3 Part 3):**
  `qttune_j2534_open` sniffs the DLL's PE machine field once and routes:
  x86 (I386) → surrogate host; x64/unknown → in-proc LoadLibraryEx.
  Unknown (sniff failure — missing/corrupt file) stays in-proc so
  unreadable paths keep today's honest `QT_ERR_LOAD_FAILED` (no
  speculative surrogate spawn for garbage). The loader handle boxes a
  backend mode; `connect_channel`/`disconnect_channel`/`read_frame`/
  `close` forward one line each on `is_surrogate` —
  **`j2534_transport.cpp` is unchanged and backend-blind** (single-path
  by construction; routing is invisible to the policy layer, verified by
  the session-level smoke over the routed surrogate). New internal
  surface: `qttune_j2534_backend_name()` ("inproc"/"surrogate", tests +
  diagnostics) and `qttune_j2534_inject_test_frame()` (routed test hook:
  lazy InjectFrame resolve in-proc, OP_INJECT pipe relay for surrogate
  — the hook that made routing tests possible in surrogate mode, since
  an x64 test process cannot LoadLibrary an x86 fake to inject). The
  sniffer also now honors its documented contract: returns 0 on
  unreadable/invalid PE (previously returned 1, contradicting the header).
- **`Shared internal contract:** `core/include/qttune/j2534_ptypes.h`
  defines `PASSTHRU_MSG` consumed by BOTH the loader, the fake DLL, and
  the surrogate — layout drift among them is structurally impossible.
  ⚠️ BENCH-tier caveat: `PASSTHRU_MSG` layout is from training knowledge; diff against the vendor's 
  `j2534.h` before first live use. Nothing in the HARNESS tier can catch
  that divergence — the fake accepts whatever the header says.
- **Deterministic fake:** `InjectFrame` enqueues into a mutex-guarded
  16-slot FIFO (test thread races the polling worker safely);
  `ReadMsgs` dequeues one message per call; `GetCurrentHandle` observes
  open/close state; `GetLastConnectParams` observes connect parameters.
  Stream tests assert EXACT counts and payload fidelity — a fabricated
  frame or a dropped injection fails the suite.
- Teardown ordering (see Data Path contract): join worker → disconnect
  channel → `PassThruClose` → `FreeLibrary`. Surrogate mode mirrors
  this: worker joined before the client's polite-SHUTDOWN/job-kill path.
- **Cross-arch verification (2026-10-15):** the full test surface —
  surrogate HARNESS suite and the routing round trips, INCLUDING a
  complete `session_create → start → close` lifecycle — has been driven
  in the true cross-arch configuration: x64 test process → x86
  surrogate exe → x86 fake DLL, frames flowing across pipe + shared
  ring. That is the exact OBDX Pro VX topology, verified with zero
  hardware. CI runs the same-arch (all-x64) form; the cross-arch
  pairing is a documented dev-machine tier via env overrides.

**Phase 3 remaining work:** WRITE/IOCTL relay through the surrogate
(request frames and filter/init control — a read-only Mode 01 session
still needs PassThruWriteMsgs at minimum), fake `PassThruWriteMsgs`
upgrade from echo-accept to recorded-response for HARNESS round-trip
coverage, then the BENCH-tier live-DLL verification round (incl.
PASSTHRU_MSG header diff, first real-vendor load/connect attempt on
the OBDX Pro VX, and the first live-vehicle session).

## Backpressure (corrected 2026-10-07)

Original v0.2.0 design gated delivery on `m_currentFrameCount >= 10k`
(cumulative lifetime count) — once 10k frames had EVER been processed,
all delivery stopped permanently and the UI froze while the counter
climbed. The model's ring eviction never engaged because nothing reached
it. Corrected: the gate now bounds **in-flight** posted-but-undrained
queued lambdas (`MaxInFlightFrames = 1000`, decremented in the lambda
before processing). At mock rate in-flight sits at 0–2; drop counter
climbs only under genuine backlog (GUI thread stalled under burst).
Regression sentinel: `StreamingContinuesPastTenThousandFrames`, using
test-only seam `forceFrameCountForTesting(int)` (Q_INVOKABLE, never
called by app code). Lesson: a drop statistic must be paired with a
liveness test — cumulative-vs-inflight conflation is invisible until
someone lets the app run 8+ minutes.

## Callback API Design (landed v0.2.0)

Session-scoped, pair-identity registration — no opaque registration
handles:

- `qttune_register_frame_callback(session, callback, user_data)`
  — duplicate pairs silently deduplicated (idempotent success).
- `qttune_unregister_frame_callback(session, callback, user_data)`
  — idempotent.
- `qttune_session_dispatch_callbacks(session, frame)` — **internal**,
  snapshot-dispatch pattern (copy list under mutex, invoke unlocked) —
  callbacks may register/unregister mid-dispatch without deadlock.
- **Callback contract:** worker thread, must not block, must not call
  `qttune_session_close()`.
- **Close discipline:** atomic closed flag flipped before teardown;
  detach joins the worker.

## What Works (verified)

| Capability | Verified by |
|---|---|
| Core builds standalone with zero Qt deps | `core-and-tests` CI job (Ubuntu, Qt absent) |
| 77 automated tests pass (18 core + 15 signals + 16 bridge + 15 J2534 + 6 F1/F2 ABI+lifecycle + 4 surrogate + 3 routing; +1 routing test env-gated-skip in CI, run on dev machine cross-arch) | GTest suites; 77/77 locally + 3-platform CI matrix green |
| Mock transport delivers physically plausible, signal-set-decodable frames | `MockFramesDecodeAgainstMockSignalSet` |
| J2534 registry discovery finds real installed vendors (OBDX Pro VT/VX), dual-root + dual-WOW64 sweep, dedupes, annotates PE architecture | `test_j2534_discovery` suite + manual smoke |
| J2534 loader: LoadLibraryEx → 7-export resolution → PassThruOpen, clean unwind on missing DLL/exports | `test_j2534_load` suite |
| J2534 end-to-end: `j2534://<path.dll>` session → connect → worker poll → InjectFrame → callback, exact counts and payload fidelity | `test_j2534_stream` suite (FIFO order asserted) |
| `?baud=`/`?flags=` reach PassThruConnect; defaults preserved; malformed rejected at parse before any DLL load | `J2534QueryParamTest` fixture |
| Bridge discovery marshaling incl. architecture field | `DiscoveredDevicesHaveWellFormedEntries` |
| SettingsPage picker: device list + refresh, manual URI connect/disconnect, error surfacing | Manual smoke (Windows, live registry) |
| Surrogate host: spawn under kill-on-close job, overlapped pipe handshake, SPSC shared ring, fail-fast dead-DLL detection (ms, not the 8s deadline), clean teardown | `J2534SurrogateTest` suite (cycle/gating/dead-DLL/nulls) |
| x86 fake DLL exports undecorated PassThru names (def file; works under both GetProcAddress conventions) | `dumpbin /exports` before/after + all surrogate+load suites |
| PE-architecture routing: x64 fake → "inproc", missing DLL → honest QT_ERR_LOAD_FAILED with no spawn | `J2534RoutingTest.X64FakeSelectsInprocBackendAndRoundTrips`, `.MissingDllFailsAtOpenThroughInproc` |
| Cross-arch routing + full session lifecycle over surrogate: x64 test → x86 surrogate → x86 fake DLL, loader-API round trip + session create/start/close | `J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` (dev machine, env-gated) |
| Vendor API call ordering at teardown (in-proc + surrogate mirror) | `FakeFixture.OpenCloseCycleHitsVendorApi` + surrogate teardown |
| Encoder/decoder layout contract | Round-trip integration test |
| Bridge marshal: worker → GUI thread | `FramesMarshalFromWorkerToGuiThread` |
| Coalesced signal updates at 10 Hz regardless of frame rate | `SignalBatchesAreCoalesced` + manual header counters |
| History ring eviction, valuesUpdated semantics, strip-chart population | Dedicated bridge tests + manual smoke |
| Streaming continues past 10k cumulative frames | `StreamingContinuesPastTenThousandFrames` |
| Responsive layout, version banner, sort toggle, lifecycle idempotency, C ABI strict-C headers, close/destroy split, frame flag translation | Pre-existing suites (summarized; see v0.2/0.3 history) |
| Full app builds Windows (MSVC 2022) + macOS (AppleClang, Qt 6.12.0) | CI jobs |

**Not verified (explicit gaps):** any REAL vendor DLL load/connect
(OBDX Pro VX and OBDLink MX+ are installed and discovered but never
loaded — WRITE/IOCTL relay outstanding before a live session is
possible), any live vehicle, and the `PASSTHRU_MSG` field order against
a vendor header (see J2534 section caveat).

## In Progress

v0.3.0 phases:

- [x] **Phase 1 — Signals layer:** POD ABI, scale/offset decode, LSB-first
      bit extraction, sign extension, validator
- [x] **Phase 1b — Mock signal emission:** tick-driven simulation;
      round-trip integration test on real worker-thread frames
- [x] **Phase 2 — Coalesced LiveView + strip chart:** CLOSED 2026-10-10.
- [x] **Phase 3 — J2534 (HARNESS tier):** vtable refactor, registry
      discovery, loader, session-integrated transport, URI routing,
      deterministic fake + stream tests. CLOSED 2026-10-13.
- [x] **Phase 3 remainder (URI params):** landed 2026-10-14 (Commit D).
- [x] **Phase 3 remainder (device picker + architecture annotation):**
      landed 2026-10-14 (Commit E).
- [x] **Phase 3 remainder:** frame ABI redesign (F1), message-ID decode
      gate (F2), lifecycle split, C ABI verification.
- [x] **Phase 3 Part 2 — x86 surrogate host:** landed 2026-10-15;
      verified same-arch (CI) AND cross-arch (dev machine) incl. dead-DLL
      fail-fast and undecorated-export fix.
- [x] **Phase 3 Part 3 — PE-architecture routing:** landed 2026-10-15;
      loader-boundary routing, backend introspection, routed inject hook;
      cross-arch session lifecycle verified.
- [ ] **Phase 3 Part 4 — WRITE/IOCTL relay:** PassThruWriteMsgs +
      PassThruIoCtl through the surrogate control pipe; fake
      WriteMsgs upgraded from echo-accept to recorded-response so the
      request/response loop is HARNESS-testable pre-vehicle.
- [ ] **Phase 3 close — BENCH-tier live verification:** first real vendor
      DLL load/connect (OBDX Pro VX via the surrogate path, OBDLink MX+
      via in-proc if its DLL is x64), PASSTHRU_MSG header diff,
      first live-vehicle read-only session.
- [ ] **Phase 4 — Record-replay transport:** fixture capture +
      deterministic playback through the same frame API

## Known Issues / Technical Debt

1. **aqtinstall pinned to git master on Windows CI.** Sporadic py7zr
   `Bad7zFile` extraction bug seen once (clean re-run passed). If it
   recurs: pin a known-good post-#1000 commit + `py7zr==1.1.0`; revert
   to PyPI release once aqtinstall 3.4+ ships the layout fix.
2. **Mobile builds unvalidated.** Responsive layout only; no kit build
   attempted yet.
3. **Linux full-app build not in CI.** Deliberate (core invariant is the
   Ubuntu job's purpose); add when Linux is a ship target.
4. **Decode ignores subscription state** (all signals decoded per
   frame, filtered at model). Harmless at mock rate; filter decode when
   signal counts grow.
5. **Ascending (oldest-first) log mode lacks auto-follow UX.** v0.3
   candidate.
6. **Session API single-threaded by contract** (bridge is sole caller).
7. **Drop-counter emit is still per-frame in handleFrameInternal**
   (TODO in code from v0.2); move into the coalesce batch for real-data
   rates.
8. **`SessionStartWithoutTransportFails` is a `SUCCEED()` stub** — weak
   test, either implement or delete.
9. **Mock simulation lacks a scripted drive cycle** (candidate: 2nd-gear
   pull to WOT with 2→3 shift). Current idle-plus-blips profile exercises
   the full pipeline but is visually quiet for demos; a scripted pull
   would exercise throttle/load/RPM coupling dramatically. Needs SimState
   physics changes + `MockFramesDecodeAgainstMockSignalSet` range updates
   — future task, not drive-by.
10. **Strip-chart x-axis resolution is one coalesce window (~100 ms),**
   not per-frame — batch-stamped by design; resolved when worker-side
   decode lands and per-sample timestamps flow through the
   `updateValues` seam.
11. **J2534 read path collapses all nonzero vendor statuses to
   "no frame."** Real devices signal ERR_TIMEOUT vs. hard errors
   distinctly. Splitting them is BENCH-tier work.
12. **J2534 timestamps pass through untranslated** (vendor-defined
   units). Normalization deferred until a real device reveals what
   vendors actually emit.
13. **Picker treats one registry entry as one device.** Multiple
   physical dongles behind a single vendor DLL (selectable via
   `PassThruOpen`'s pName argument, currently passed as nullptr =
   default device) are indistinguishable.
14. **Discovery tests are shape-only / potentially vacuous** on a
   machine with no discoverable vendors. Candidate fix: seeded-registry
   fixture with admin-elevation guard, or documented count-positive
   assertions on the known dev machine.
15. **J2534 message-ID gate is inert** (reports zero arbitration_id).
   Filter layer (PassThruStartMsgFilter message-pattern registration) is
   the next milestone to activate the gate for real traffic. Documented,
   not silent.
16. **ABI version string still says 0.2.0** — root CMake `project()`
   and the propagated version.h await the v0.3.0 tag (deliberate:
   bumping mid-cycle causes gratuitous rebuild churn).
17. **Surrogate ring drops frames silently when full** (producer-side
   backpressure counter is Part 4 work). 4096-slot ring at current rates
   makes overflow theoretical, but the drop is currently unobservable.
18. **Bridge architecture label maps unknown (0) to "x86"** — display-
   only imprecision now that the sniffer can legitimately return 0
   (unreadable PE). Routing itself correctly treats 0 as in-proc.

## Roadmap

| Phase | Goals | Status |
|-------|-------|--------|
| v0.1.0 | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0 | Callback API, mock transport, CI + tests | ✅ Released (tagged) |
| v0.3.0 | Signals layer, coalesced LiveView, J2534 transport, live vehicle read/logging | 🚧 Phases 1–3 HARNESS-complete incl. URI params, device picker, F1/F2 ABI redesign, **x86 surrogate host + PE-architecture routing (cross-arch verified)**; remaining: WRITE/IOCTL relay, BENCH live verify, live vehicle |
| v0.4.0 | Security access discovery, flash capability | Planned |
| v0.5.0+ | Multi-manufacturer plugin modules | Planned |

## Recent Engineering Notes

example of full build and testing workflow:
rm -rf build
cmake -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build build --config Release
run tests per suite:
  ./build/tests/Release/test_core.exe
  ./build/tests/Release/test_signals.exe
  ./build/tests/Release/test_bridge.exe
  or
  ctest --test-dir build -C Release -R J2534Surrogate --output-on-failure
run full testing: 
  ctest --test-dir build -C Release --output-on-failure
full app end to end and manual testing
  ./build/app/Release/qttune.exe

cross-arch build and test (x64 app with Win32 artifacts/DLLs):
cmake -B build-win32 -A Win32 -DQTTUNE_BUILD_CORE_ONLY=ON
cmake --build build-win32 --config Release --target qttune-j2534-surrogate fake_j2534
export QTTUNE_SURROGATE_EXE='E:/CodeProjects/qttune/build-win32/surrogate/Release/qttune-j2534-surrogate.exe'
export QTTUNE_FAKE_DLL='E:/CodeProjects/qttune/build-win32/tests/Release/fake_j2534.dll'
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate" --output-on-failure
(note: env vars need WINDOWS paths — MSYS does not translate environment
variable values, only command arguments; use E:/ style with forward
slashes, which Win32 APIs accept)

For the record and for anyone hitting the same walls: the macOS job
initially failed because Apple removed the AGL framework from the Xcode 26
SDK while Qt ≤6.8 still referenced it transitively. Upgrading to Qt 6.12.0
resolved it cleanly. Windows then hit an aqtinstall bug constructing
doubled `qt6_xxxx/qt6_xxxx/` paths for the Qt 6.11+ repository layout;
worked around by invoking aqt from master directly. Also: modern Qt ships
no 32-bit Windows kits, so the cross-arch tree is core-only by necessity —
which the surrogate design anticipated (the app is x64 forever; only the
surrogate + vendor DLLs are x86). Both CI fixes are temporary scaffolding
to be reverted when upstreams ship tagged releases.
(Recurring CI-noise, ignorable: `Qt6TaskTree`/`QmlAssetDownloaderPrivate`
not-found warnings and missing Vulkan headers — optional Quick plugins,
never used by this app.)

**2026-10-04 — callback API round:** dropped never-implemented registration
handle (pair-identity instead), duplicate registration idempotent-success,
tests assert actual behavior, snapshot-dispatch mandated so callback
reentry can't deadlock. MSVC caught an implicit int→enum conversion
(C2664) — three-compiler matrix earns its keep.

**2026-10-05 — bridge + log round:** (1) captureless lambdas unsafe as
pair-identity C callbacks needing self-reference — named free functions
only. (2) Thread-identity tests must exercise real execution paths
(session_start), not manual dispatch. (3) Assertions encode contracts,
not timing coincidences. Q_PROPERTY NOTIFY is a promise; skipping emit
leaves QML serving stale values. Windows DLL deployment for test targets
via `$<TARGET_RUNTIME_DLLS>` copy, not PATH injection (semicolon fragility).

**2026-10-06 — signals round (Phase 1):** (1) error-code precedence is a
contract — type validation before bit-parameter validation. (2)
zero-initialized PODs mask string-termination test bugs — corrupt the
ENTIRE buffer. Also migrated all suites to `gtest_discover_tests()`
(per-case granularity, free empty-suite detection); DISCOVERY runs the
test binary at BUILD time on Windows, so runtime DLL copying must be
ordered before discovery (we hit exit 0xc0000135 until reordered).

**2026-10-07 — emission + coalescing round (Phases 1b/2):** three lessons.
(1) Designated initializers are C++20 — MSVC accepts permissively, GCC on
Ubuntu rejects; C++17 code needs plain constructor helpers. (2)
`strncpy_s`/`_TRUNCATE` are MSVC-only Annex K — absent from glibc —
"fix a warning" edits can compile locally and fail only CI; direction of
travel matters (MSVC-green ≠ portable). Use a small bounded-copy helper.
(3) **Cumulative count is not backpressure**: gating delivery on lifetime
frame count froze the stream permanently at 10k while the model's ring
eviction sat unused. Bound in-flight queue depth, not totals; pair drop
statistics with liveness tests. Also: measuring before optimizing — the
guiNotifyCount instrumentation locked the 20× coalescing-reduction claim
with a baseline number. New-QML-file linker failures (QmlCacheGeneratedCode
unresolved externals) mean stale QML cache — clean reconfigure fixes.

**2026-10-10 — strip chart round (Phase 2 close):** (1) The QML paint
code indexed the LAST ELEMENT of a flattened `[ts, val, ...]` array as
the newest timestamp — it's the newest VALUE. Symptom was deliciously
diagnostic: chart fine for ~1.5 s, then stretched/clipped, "recovering"
exactly during RPM rev blips. **47 green GTests were structurally blind to
it** — the bug lived in QML `onPaint` arithmetic, outside every C++
test's reach — the manual smoke tier caught it, which is why that tier
exists. (2) Declared-but-never-emitted signals are latent debt. (3)
Pull-API beats data-roles for Canvas consumers. (4) Timestamp injection
seams make ring tests exact arithmetic instead of timing tolerance.

**2026-10-13 — J2534 round (Phase 3, HARNESS tier):** seven lessons,
several of them paid for in build-break debugging. (1) **An
anonymous-namespace definition does not complete a global forward
declaration** — C2027 trap; everything the ABI references by name lives
at file scope, ordered before first use. (2) **Spec-shaped signatures
are load-bearing even against your own fake.** (3) **Calling-convention
mismatches are silent on x64** — function-pointer typedefs must match the
artifact's actual declaration. (4) **CMake literal `$(Configuration)` vs
genex `$<CONFIG>`** — doubled Release/Release output dirs; use genex for
per-config paths. (5) **Identical COMDAT folding in dumpbin** is MSVC
merging byte-identical bodies, not corruption. (6) **Baked absolute
paths via `$<TARGET_FILE:...>` beat cwd/search-order schemes for test
DLLs.** (7) **Exact-count assertions against a deterministic fake are
the harness version of range assertions against physics.**

**2026-10-14 — J2534 query-param round (Commit D):** (1) **Write
failure-path assertions against the actual failure path, not the intended
one** — derive each expectation from which layer should reject the input.
(2) **Paired compiler diagnostics fingerprint a lost middle argument**
(code-167 + code-165 on the same token = argument shift). (3) **Existing
green tests can be blind to a regression a new feature introduces in
shared plumbing** — pin the contract you changed, at the point it
changed (`DefaultConnectUsesIso15765At500k`).

**2026-10-14 — device picker + real-vendor discovery round (Commit E):**
(1) **The registry spec describes an ideal world; installers live in the
real one** — dual-root sweep. (2) **Green tests can be vacuously green
against machine-dependent data.** (3) **A picker's first real-world duty
is diagnosing its own environment.** (4) **Unverified war stories rot.**

**2026-10-14 — F-series (Frame ABI + Lifecycle + Gate):** ABI-breaking frame
redesign, close/destroy split, message-ID decode gate, strict-C ABI
verification. Test count rises 64→70. Lessons: (1) ABI breaks require
version-string sync. (2) Poll-until-condition waits eliminate wall-clock
dependence. (3) Shared constants beat comments.

**2026-10-14 — surrogate host round (Phase 3 Part 2):** five lessons,
every one paid for in a hang, a ghost theory, or a red test. (1) **A
synchronous pipe handle silently neuters overlapped I/O:** `ConnectNamedPipe`
with an OVERLAPPED struct on a handle created WITHOUT
`FILE_FLAG_OVERLAPPED` *ignores the struct and blocks forever* — the
carefully-built wait/cancel/fail-fast code downstream of it was dead code,
never reached. Symptom: same-arch tests passed (their children connect
instantly) while the dead-DLL test hung forever. The fix is the flag; the
discipline is knowing that every subsequent ReadFile/WriteFile then needs
its own OVERLAPPED — pseudo-sync wrappers with timeouts came free with
the correct fix and closed the wedged-child risk too. (2) **A wait
predicate that doesn't DO the thing can never fire:** the test waited on
`count >= 3` while the drain that increments count ran only *after* the
wait — every run burned the full timeout and then failed. When the
condition is produced by polling, the predicate must drain inside the
loop. (3) **x86 `__stdcall` name decoration breaks `GetProcAddress` by
spec name:** MSVC exports `_PassThruOpen@8` on x86; x64 has one calling
convention so names go undecorated — the same-arch suite passed for
weeks while the cross-arch run failed on export resolution (child exit
code 5). Fix: a linker `.def` file with bare EXPORTS entries — the exact
mechanism real J2534-1 vendors use to publish undecorated names. The
spec mandates undecorated exports precisely because of this trap.
(4) **Instrument BEFORE theorizing:** the hang took three blind fix
attempts (path dialect, missing x86 CRT — both disproven by one
`ls /c/Windows/SysWOW64` and one bare-exe run) before distinct child
exit codes + client-side stage logging turned the next failure into a
one-command diagnosis. Cheap exit codes on every early-return in a child
process are permanent diagnosability, not scaffolding. (5) **MSYS shells
don't translate environment-variable values** — only command arguments
get path mangling; `/e/...` in an env var reaches CreateProcessA
verbatim and fails. Windows-style paths (forward slashes are fine) in
env overrides. Honorable mention: the two failed theories were each
disproved by a single command because the evidence was checkable — the
methodology is the lesson, not the guesses.

**2026-10-14 — routing round (Phase 3 Part 3):** (1) **Route at the
lowest complete boundary:** putting the in-proc/surrogate switch inside
`qttune_j2534_open` and forwarding one line per vendor-call helper left
`j2534_transport.cpp` byte-identical — the policy layer is
backend-blind, proven by a session-level lifecycle running over the
surrogate with zero transport changes. A routing decision placed higher
forks every future capability; placed at the loader it is inherited for
free by all of them. (2) **A sniffer's failure mode is a routing
decision:** returning "x86" on unreadable PE would have spawned
surrogate processes for garbage paths; returning "unknown → in-proc"
preserves the honest LoadLibraryEx failure. The docstring said one
thing, the code did another — the routing work surfaced the contract
violation that had been hiding in discovery since Part 1. (3) **Test
hooks need to exist at the boundary under test:** cross-arch routing
tests required `qttune_j2534_inject_test_frame` precisely because an
x64 test process cannot LoadLibrary an x86 fake to reach the shared
queue — the inject hook routes like everything else and closes the gap.

---

*(This document describes development state only. Safety, regulatory,
and warranty caveats are in [DISCLAIMER.md](DISCLAIMER.md).)*


TODO:
hide mock connections behind developer mode setting on settings page
basic dashboard design - RPM and speed gauges, maybe one other - eventually customizable
change LogPage to application log messages rather than mock data
add ability to save data logs to a file (after implementing data being displayed from a live vehicle on LiveView)
mobile build android/iOS
macOS and linux build of full desktop version
WRITE/IOCTL relay through surrogate (PassThruWriteMsgs, PassThruIoCtl, PassThruStartMsgFilter) + fake WriteMsgs recorded-response upgrade
add WIN32 build to CI (with testing)