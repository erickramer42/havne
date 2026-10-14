# QtTune — Project Status

**Date:** October 14, 2026
**Version:** v0.3.0-dev (ABI-breaking frame redesign + lifecycle split); v0.2.0 still tagged. Phases 1–3 HARNESS-complete incl. URI query params + device picker — live-vehicle + x86 surrogate host pending.
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
|  * Signals: decode + mock  |   signal_sets.cpp, SimState emission
|    set + emission (done)   |
|  * Frame model (qtab)      |
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
    (Win32 only; ISO 15765, j2534:// URI) PASSTHRU_MSG layout shared
                                          with the fake DLL via
                                          j2534_ptypes.h
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
- `valuesUpdated` — one emission per non-empty batch, the Canvas's sole
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

## J2534 Transport (landed v0.3.0 Phase 3, HARNESS tier, 2026-10-13)

Transport stack for real Pass-Thru devices (Windows). **Verified against
a fake vendor DLL end-to-end; discovery additionally verified against
real installed drivers — no vendor DLL has been loaded and no live
vehicle has been connected.** Landmarks, in commit order:

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
  {vendorName, dllPath} — display values only, rule 2) and `currentUri`
  (cleared on failed connect — no stale-binding promise, 2026-10-05
  lesson applied at birth). SettingsPage: discovery list with refresh,
  manual URI field with presets (mock, bare j2534://), connect/disconnect
  with error surfacing, empty-state hint (non-Windows degrades cleanly).
  Discovery itself gained a DUAL-ROOT sweep: the SAE-spec
  `Pass-Thru Support.04.04` plus the no-hyphen `PassThruSupport.04.04`
  variant real installers write (observed: OBDX Pro, in WOW6432Node
  only), 2 roots × 2 WOW64 views = 4 sweeps, dedupe absorbs overlap.
  Spec spelling is swept first so conforming vendors keep discovery
  order precedence. First real-contact finding: the OBDX Pro VX subkey
  declares ISO15765=1 — the exact protocol this transport connects
  with — so it is a genuine connect candidate pending the architecture
  check (DLL lives under Program Files (x86); x86-only DLLs cannot load
  into the x64 app — the surrogate-host wall).
- **Shared internal contract:** `core/include/qttune/j2534_ptypes.h`
  defines `PASSTHRU_MSG` consumed by BOTH the loader and the fake DLL —
  layout drift between them is structurally impossible. ⚠️ BENCH-tier caveat: `PASSTHRU_MSG` layout is from training knowledge; diff against the vendor's 
  `j2534.h` before first live use. Shared header consumed by both loader and
  fake — layout drift impossible between them.
  Nothing in the HARNESS tier can catch that divergence — the fake
  accepts whatever the header says.
- **Deterministic fake:** `InjectFrame` enqueues into a mutex-guarded
  16-slot FIFO (test thread races the polling worker safely);
  `ReadMsgs` dequeues one message per call; `GetCurrentHandle` observes
  open/close state; `GetLastConnectParams` observes connect parameters.
  Stream tests assert EXACT counts and payload fidelity — a fabricated
  frame or a dropped injection fails the suite.
- Teardown ordering (see Data Path contract): join worker → disconnect
  channel → `PassThruClose` → `FreeLibrary`.
- **F1 Frame ABI redesign (2026-10-14):** `arbitration_id` first-class field (0
  for J2534 until filter API lands), `flags` bitfield (EXTENDED, FD, BRS, RTR, ERROR, TX_ECHO), session-relative µs timestamps, 64-byte data buffer. Old `is_extended` boolean and implicit padding eliminated. Size-neutral (still 88 bytes), layout hardened with compile-time assertions in `test_c_abi.c`.
- **F2 Message-ID decode gate (2026-10-14):** signals declare `message_id`;
  bridge filters on `frame.arbitration_id == def.message_id` before extraction. Prevents corruption from multi-ID bus traffic (previously every signal decoded against every frame). Mock traffic exercises the gate continuously (diagnostic 29-bit frames rejected). J2534 path inert (zero arb ID) — documented, not silent.

**Phase 3 remains OPEN. Outstanding:** x86-surrogate host for 32-bit-only
vendor DLLs (J2534-1 mandates only that they be in-proc; many ship x86
only and can't load into the x64 app) and the BENCH-tier live-DLL
verification round (incl. PASSTHRU_MSG header diff and the first
real-vendor load/connect attempt).

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
liveness test — cumulative-vs-in-flight conflation is invisible until
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
| 70 automated tests pass (18 core + 15 signals + 16 bridge + 15 J2534 + 6 new F1/F2 lifecycle + ABI tests) | GTest suites; 70/70 locally + 3-platform CI matrix green |
| Mock transport delivers physically plausible, signal-set-decodable frames | `MockFramesDecodeAgainstMockSignalSet`: real worker frames decoded under every mock definition, physical ranges asserted |
| J2534 registry discovery finds real installed vendors (OBDX Pro VT/VX on dev machine), dual-root spec + no-hyphen spelling, dedupes cross-sweep | `test_j2534_discovery` suite + manual smoke: picker populates from live registry |
| J2534 loader: LoadLibraryEx → 7-export resolution → PassThruOpen, clean unwind on missing DLL/exports | `test_j2534_load` suite (incl. proven-unmapped assertion after failed load) |
| J2534 end-to-end: `j2534://<path.dll>` session → connect → worker poll → InjectFrame → callback, exact counts and payload fidelity | `test_j2534_stream` suite (7 tests; FIFO order asserted) |
| `?baud=`/`?flags=` query params reach `PassThruConnect`; defaults preserve ISO 15765 @ 500k / flags=0; malformed queries rejected at parse before any DLL load | `J2534QueryParamTest` fixture: default-preservation, explicit params (decimal + hex flags, last-wins duplicates), malformed matrix incl. real-fake-path rejects |
| Bridge discovery marshaling: every discovered entry carries string vendorName/dllPath | `DiscoveredDevicesHaveWellFormedEntries` (shape contract; empty list legitimate on non-Windows) |
| currentUri tracks connect state, cleared on failed connect | `CurrentUriTracksConnectState` |
| SettingsPage picker: device list + refresh, manual URI connect/disconnect, error surfacing, empty-state hint | Manual smoke (Windows, live registry) |
| Vendor API call ordering at teardown (PassThruClose before FreeLibrary; worker joined before any of it) | `FakeFixture.OpenCloseCycleHitsVendorApi` state transitions 1→0→1 |
| Encoder/decoder layout contract | Round-trip integration test catches byte-order/scale inversions invisible to structural validation |
| Bridge marshal: worker → GUI thread | `FramesMarshalFromWorkerToGuiThread` |
| Coalesced signal updates at 10 Hz regardless of frame rate | `SignalBatchesAreCoalesced` (bounded ranges, not exact counts); manual: header `batch:` climbs ~10/s while `frames:` climbs ~20/s |
| History ring evicts oldest at capacity, order preserved through wrap | `HistoryRingEvictsOldestAtCapacity` (deterministic injected timestamps; 130 pushes into 120 slots, exact retention assert) |
| valuesUpdated fires once per non-empty batch; unsubscribed signals excluded; empty/unknown-ID batches silent | `ChartDataFiltersUnsubscribedAndEmptyBatchesDoNotEmit` |
| Strip chart populated end-to-end through real coalescer stream | `StripChartDataPopulatedByLiveStream` (bounded sample counts, monotonic timestamps) |
| Streaming continues past 10k cumulative frames (no freeze) | `StreamingContinuesPastTenThousandFrames` via `forceFrameCountForTesting` seam |
| LiveView page: subscribed values + scrolling strip chart traces, unsubscribe/resubscribe without history loss, resize repaint | Manual smoke: 20+ s run through 120-sample ring wrap, rev-blip spikes render as shape, no clipping/blanking |
| Sort toggle, clear, mid-stream disconnect, lifecycle idempotency | Existing 6 bridge behaviors + tests |
| Full app builds Windows (MSVC 2022) + macOS (AppleClang, Qt 6.12.0) | CI jobs |
| Responsive layout (sidebar ↔ TabBar at 720px) incl. Live entry | Local run |
| Version banner from generated version.h | Local run; single source of truth in root CMake `project()` |
| C ABI headers compile as strict C (no C++-isms) | `test_c_abi.c` — builds on all platforms, links core symbols |
| Close/destroy lifecycle split prevents use-after-free on double-close | `CloseIsIdempotentAndDestroyIsSafe`, `DestroyAutoClosesActiveSession` |
| Frame flag translation verified end-to-end (fake `RxStatus` bit → `qttune_frame_t.flags`) | `DirectLoadUriDeliversInjectedFramesExactly` extended with flag assertions |

**Not verified (explicit gaps):** any real vendor DLL LOAD/CONNECT
(drivers are discovered and listed; none has been loaded or connected
through the app), any live vehicle, 32-bit-only vendor DLL loading,
and the PASSTHRU_MSG field order against a vendor header (see J2534
section caveat).

## In Progress

v0.3.0 phases:

- [x] **Phase 1 — Signals layer:** POD ABI, scale/offset decode, LSB-first
      bit extraction, sign extension, validator
- [x] **Phase 1b — Mock signal emission:** tick-driven simulation;
      round-trip integration test on real worker-thread frames
- [x] **Phase 2 — Coalesced LiveView + strip chart:** CLOSED 2026-10-10.
- [x] **Phase 3 — J2534 (HARNESS tier):** vtable refactor, registry
      discovery, loader, session-integrated transport, URI routing,
      deterministic fake + stream tests. CLOSED 2026-10-13 at the
      fake-DLL tier.
- [x] **Phase 3 remainder (URI params):** `?baud=`/`?flags=` strict
      query parsing landed 2026-10-14 (Commit D).
- [x] **Phase 3 remainder (device picker):** SettingsPage picker +
      bridge discovery surface + dual-root registry sweep landed
      2026-10-14 (Commit E), picker verified against real installed
      drivers (OBDX Pro VT/VX).
- [x] **Phase 3 remainder:** frame ABI redesign (F1), message-ID decode gate (F2),
      lifecycle split, C ABI verification.
- [ ] **Phase 3 remainder:** x86 surrogate host for 32-bit-only vendor DLLs; 
      BENCH-tier live-DLL verification (incl. `PASSTHRU_MSG` header diff, dumpbin architecture check on the OBDX DLL, first real vendor load/connect).
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
   not per-frame — batch-stamped by design (staging merge destroys
   per-frame timestamps anyway); resolved when worker-side decode lands
   and per-sample timestamps flow through the `updateValues` seam.
11. **J2534 read path collapses all nonzero vendor statuses to
   "no frame."** Real devices signal ERR_TIMEOUT vs. hard errors
   distinctly; v0.3.0's uniform treatment masks device faults as
   silence. Splitting them is BENCH-tier work (needs a device that
   produces real error codes).
12. **J2534 timestamps pass through untranslated** (vendor-defined
   units → `timestamp_us` unexamined). Normalization deferred until a
   real device reveals what vendors actually emit.
13. **Picker treats one registry entry as one device.** Multiple
   physical dongles behind a single vendor DLL (selectable via
   `PassThruOpen`'s pName argument, currently passed as nullptr =
   default device) are indistinguishable. Documented granularity limit
   of the Commit E picker.
14. **Discovery tests are shape-only / potentially vacuous** on a
   machine with no discoverable vendors — they were green for weeks
   while the dev machine's real drivers (OBDX, no-hyphen root) were
   invisible to the code. Candidate fix: seeded-registry fixture with
   admin-elevation guard, or documented count-positive assertions on
   the known dev machine.
15. J2534 message-ID gate is inert (reports zero arbitration_id). The F2 gate 
    filters on message_id but J2534-1 has no per-message ID field in PASSTHRU_MSG; ISO 15765 addressing rides in the payload. Filter layer (message-pattern registration via PassThruStartMsgFilter) is the next milestone to activate the gate for real traffic. Documented, not silent.
16. ABI version string still says 0.2.0 — the struct redesign warrants a version
    bump to 0.3.0-dev in the same commit as the F-series. The string should not advertise a layout it no longer has.

## Roadmap

| Phase | Goals | Status |
|-------|-------|--------|
| v0.1.0 | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0 | Callback API, mock transport, CI + tests | ✅ Released (tagged) |
| v0.3.0 | Signals layer, coalesced LiveView, J2534 transport, live vehicle read/logging | 🚧 Phases 1–3 HARNESS-complete incl. URI params + device picker + F1/F2 ABI redesign (frame flags/message-ID gate/lifecycle); remainder (surrogate host, live verify) + vehicle work pending |
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
  etc
run full testing: 
  ctest --test-dir build -C Release --output-on-failure
./build/app/Release/qttune.exe (for full app end to end and manual testing)

For the record and for anyone hitting the same walls: the macOS job
initially failed because Apple removed the AGL framework from the Xcode 26
SDK while Qt ≤6.8 still referenced it transitively. Upgrading to Qt 6.12.0
resolved it cleanly. Windows then hit an aqtinstall bug constructing
doubled `qt6_xxxx/qt6_xxxx/` paths for the Qt 6.11+ repository layout;
worked around by invoking aqt from master directly. Both fixes are
temporary scaffolding to be reverted when upstreams ship tagged releases.
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
exactly during RPM rev blips (blip spikes temporarily pushed the buggy
max above elapsed-time). **47 green GTests were structurally blind to
it** — the bug lived in QML `onPaint` arithmetic, outside every C++
test's reach — the manual smoke tier caught it, which is why that tier
exists. A consumer of a verified contract can still mis-read the
contract. (2) Declared-but-never-emitted signals are latent debt:
`valuesUpdated` sat unemitted since its declaration until the chart
became its first consumer and exposed the gap. (3) Pull-API beats
data-roles for Canvas consumers: `chartData()` returns everything a
repaint needs in one call, avoiding role-number gymnastics from JS
paint code. (4) Timestamp injection seams make ring tests exact
arithmetic instead of timing tolerance — same philosophy as assertion
lesson 2026-10-05 (3), applied to test fixtures.

**2026-10-13 — J2534 round (Phase 3, HARNESS tier):** seven lessons,
several of them paid for in build-break debugging. (1) **An
anonymous-namespace definition does not complete a global forward
declaration** — putting `struct qttune_j2534_handle {}` inside
`namespace {}` creates a SECOND, internal-linkage type; the header's
opaque typedef keeps targeting the never-completed global tag, and
every member access dies with C2027. Everything the ABI references by
name lives at file scope, ordered before first use. (2) **Spec-shaped
signatures are load-bearing even against your own fake:** the fake's
`PassThruOpen` initially returned 1 as a "handle" where the real
J2534-1 contract returns STATUS_NOERROR with the device ID in the
out-parameter. The spec-shaped loader caught it as a clean
`QT_ERR_LOAD_FAILED` — status code 7 — instead of a stack mismatch;
had the fake's shape leaked into the loader's typedefs, the bug would
have surfaced only against real hardware. (3) **Calling-convention
mismatches are silent on x64** — `__stdcall` folds into the default
convention there, so an int-returning hook called through a
void(int*)-typed pointer doesn't crash, it just quietly writes nothing
(left the test's sentinel at −1). Function-pointer typedefs must match
the artifact's actual declaration, verified against source, not memory
of what "should" be there. (4) **CMake literal `$(Configuration)` vs
genex `$<CONFIG>`:** the literal survives into the .vcxproj where
MSBuild expands it, AND CMake's multi-config behavior auto-appends a
per-config subdir to plain RUNTIME_OUTPUT_DIRECTORY strings — net
result `build/lib/Release/Release/`. Generator expressions do neither
trick; use them for any per-config output path. (5) **Identical COMDAT
folding is a thing you'll meet in dumpbin:** two exports sharing an
RVA is MSVC merging byte-identical function bodies, not corruption.
(6) **Baked absolute paths via `$<TARGET_FILE:...>` beat any
cwd/search-order scheme for test DLLs** — the test never depends on
where ctest launches it from. Side effect: the IDE's IntelliSense
doesn't see the compile definition (cosmetic squiggle, documented in
.vscode). (7) **Exact-count assertions against a deterministic fake are
the harness version of range assertions against physics**: `EXPECT_EQ(count, 3)`
means a fabricated fourth frame fails the suite, the same way an
absurd-RPM decode does — a fake that emits anything you didn't inject
is broken, and the test should say so.

**2026-10-14 — J2534 query-param round (Commit D):** three lessons, two
paid for in red tests. (1) **Write failure-path assertions against the
actual failure path, not the intended one:** the malformed-URI matrix
drafted expecting `QT_ERR_NO_TRANSPORT` for a nonexistent direct path —
but with the query correctly split, that path legitimately fails later
at `LoadLibraryEx` with `QT_ERR_LOAD_FAILED`. The failing test wasn't
catching a product bug; it was encoding a drafting assumption as a
contract (2026-10-05 (3) resurfacing). Same fix as before: derive each
expectation from which layer should reject the input, and assert
against a REAL resolvable path when proving parse-rejection (a
wrongly-accepted query would load the fake successfully — that's the
discriminating case). (2) **Paired compiler diagnostics fingerprint a
lost middle argument:** a hand-applied edit dropped `&num` from the
`read_msgs` call, producing code-167 (type mismatch: `uint32_t` into
`unsigned long*`) and code-165 (too few arguments) on the SAME token —
the argument-shift signature, diagnosable before reading the line. It
also validates the session ritual again: diagnose from pasted current
file contents, never from the delivered version (the on-disk file had
drifted by exactly one argument). Had it compiled by luck of types, the
bug was worse than a crash — `num` staying 1 with ReadMsgs writing
nothing meant every poll would fabricate a zeroed frame. (3) **Existing
green tests can be blind to a regression a new feature introduces in
shared plumbing:** the three stream tests would pass unchanged with ANY
hardcoded connect parameters — the observable-behavior pin
(`DefaultConnectUsesIso15765At500k` via the fake's `GetLastConnectParams`
hook) exists because the feature changed what flows into
`PassThruConnect`, and only an observation of that specific call site
could notice. Same genus as 2026-10-13 (7): test the contract you
changed, at the point it changed.

**2026-10-14 — device picker + real-vendor discovery round (Commit E):**
four lessons, one of them a benchmark for what the manual tier is FOR.
(1) **The registry spec describes an ideal world; installers live in
the real one.** First contact with a real driver (OBDX Pro VX/VT) showed
a no-hyphen root spelling `PassThruSupport.04.04` living ONLY in
WOW6432Node — invisible to a spec-spelled, single-root query. Found via
`reg query` archaeology prompted by the picker's empty state, NOT by any
test: see (2). Fix: dual-root sweep (spec spelling first for
precedence) × both WOW64 views, dedupe absorbing overlap. (2) **Green
tests can be vacuously green against machine-dependent data:** the
discovery suite had been passing for weeks while the dev machine's real
drivers were invisible to the code — every vendor-loop assertion
iterated an empty array. Shape contracts are fine for marshaling;
against live external state, either seed the state (fixture registry
key, admin-guarded) or admit the test verifies the code path, not the
data. (3) **A picker's first real-world duty is diagnosing its own
environment:** the empty-state hint turned "it doesn't work" into a
bounded investigation in three commands — UI states are diagnostic
tools, not just decoration. (4) **Unverified war stories rot:** a
half-remembered "tests failed because they found drivers" from early
Phase 3 could not be reconciled with the code (which never saw those
drivers). Either keep CI logs as the record or let the memory go —
folklore debugging wastes a round of suspicion on the wrong suspect.

**2026-10-14 — F-series (Frame ABI + Lifecycle + Gate):** ABI-breaking frame redesign (arbitration ID first-class, flags word, session-relative timestamps, 64-byte payload), close/destroy split eliminating use-after-free on double-close, message-ID decode gate activating signal filtering, C ABI headers verified as strict C (`test_c_abi.c`). Test count rises 64→70. Lessons: (1) ABI breaks require version-string sync — the string advertising a struct layout it no longer has is worse than silence. (2) Poll-until-condition waits in test suites eliminate wall-clock dependence on CI runner speed. (3) Shared constants beat comments (mock arb ID, RxStatus bit); the compiler enforces what we previously trusted.

---

*(This document describes development state only. Safety, regulatory,
and warranty caveats are in [DISCLAIMER.md](DISCLAIMER.md).)*
