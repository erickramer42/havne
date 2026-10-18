# Testing

What the test suites cover, how much confidence each tier gives, and
where the gaps are. For build and run commands, see
[development.md](development.md#running-tests).

## Verification tiers

| Tier | Meaning | Where it runs |
|---|---|---|
| **Unit** | Core, signal and ABI logic with no I/O | CI on all three platforms |
| **Bridge** | Qt bridge and models, headless (`Qt6::Core` only) against the real 50 ms mock worker | CI on Windows and macOS, and locally |
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL | CI on Windows (same-arch, all x64) |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Manual smoke** | Running the app: layout, LiveView, strip chart, Settings picker against the live registry | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); talking-bus behaviors pending |
| **Vehicle** | A live, read-only vehicle session | **Not yet reached** |

The manual smoke tier earns its place. The 2026-10-10 strip-chart
indexing bug lived in QML paint code, and every C++ test was blind to
it.

## Suites

| Binary | Suites | Needs |
|---|---|---|
| `test_core` | `CoreTest` | Nothing (Qt-free) |
| `test_signals` | `SignalDecodeTest`, `SignalsTest` | Nothing (Qt-free) |
| `test_c_abi` (ctest name `c_abi`) | Public headers compile as strict C; layout asserts | A C compiler |
| `test_bridge` | `BridgeTest` | Qt Core. Compiles `app/qttunebridge.cpp` directly, so the bridge must stay QtCore-only. |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_surrogate` | `J2534SurrogateTest` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_routing` | `J2534RoutingTest` | Windows. The cross-arch case skips unless `QTTUNE_FAKE_DLL` is set. |

`ctest -N` lists every case. The current total is in
[STATUS.md](../STATUS.md).

## What is verified

| Capability | Verified by |
|---|---|
| Core builds with zero Qt dependencies | CI `core-and-tests` job (Ubuntu, no Qt installed) |
| Public C ABI headers are valid strict C | `c_abi` |
| Callback registration: dedup, idempotent unregister, reentrant register/unregister during dispatch | `CoreTest` |
| Callbacks run on the worker thread | `CoreTest`, through `qttune_session_start` |
| Session lifecycle: idempotent close, close/destroy split | `CoreTest` |
| Signal decode: bit extraction, sign extension, error-code order, validator | `SignalDecodeTest`, `SignalsTest` |
| Mock frames decode to physically plausible values | `CoreTest.MockFramesDecodeAgainstMockSignalSet` |
| Frames marshal from the worker to the GUI thread | `BridgeTest.FramesMarshalFromWorkerToGuiThread` |
| Signal updates are coalesced to 10 Hz whatever the frame rate | `BridgeTest.SignalBatchesAreCoalesced` |
| History ring eviction, `valuesUpdated` semantics | `BridgeTest` |
| Streaming continues past 10k cumulative frames; drops are counted and reported | `BridgeTest.StreamingContinuesPastTenThousandFrames`, `.BackpressureDropsAreCountedAndReported` |
| Log sort toggle (newest first / oldest first) | `BridgeTest.SortProxy*` |
| Discovery: dual-root, dual-WOW64 sweep, dedup, PE architecture annotation | `J2534DiscoveryTest`, plus manual smoke against real installed drivers (OBDX Pro VT and VX) |
| Bridge discovery marshaling, including the architecture field | `BridgeTest.DiscoveredDevicesHaveWellFormedEntries` |
| Loader: `LoadLibraryEx`, nine-export resolution, `PassThruOpen`, clean unwind on a missing DLL | `FakeFixture` |
| Teardown call order against the vendor API | `FakeFixture.OpenCloseCycleHitsVendorApi`, plus surrogate teardown |
| `j2534://` session end to end with exact counts, payload fidelity and FIFO order | `J2534StreamTest` |
| `?baud=` and `?flags=` reach `PassThruConnect`; defaults kept; malformed URIs rejected before any DLL load | `J2534QueryParamTest` |
| Surrogate: spawn under the job object, pipe handshake, shared ring, dead DLL fails fast, clean teardown | `J2534SurrogateTest` |
| Write, ioctl and filter relay on both backends, including whitelist refusal and malformed filter shapes | `J2534SurrogateTest`, `J2534RoutingTest.InprocWriteIoctlFilterRoundTrips` |
| PE routing: x64 goes in-proc; a missing DLL gives an honest load failure with no spawn | `J2534RoutingTest` |
| Cross-arch: x64 test → x86 surrogate → x86 fake, loader round trip plus full session lifecycle | `J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` (dev machine) |
| Settings picker: device list, refresh, manual URI connect and disconnect, error surfacing | Manual smoke (Windows, live registry) |
| Full app builds on Windows (MSVC 2022) and macOS (AppleClang, Qt 6.12.0) | CI |
| Transport adoption: `?filter=` registers a FLOW_CONTROL filter; `?txid=` selects the transmit ID; `?poll=` drives a Mode-01 loop; header strip + arbitration_id stamp; filter stopped on detach; gating discards non-matching frames; poll-without-filter rejected at parse | `J2534AdoptionTest` |
| A blocked vendor `PassThruOpen` fails at the (shrunken) handshake timeout with the child alive, not wedged | `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout` |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), both backends | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path) plus the whole existing suite against the legacy-spelling fake (fallback path) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally; the child traces no filter ops (Known Issue #23). Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Surrogate vendor-call serialization: one lock around every vendor entry (read pump + control pump under real concurrency), single vendor disconnect after a clean client close, crash-on-stuck-vendor replaced by job-reaped linger | The full J2534 suites on both backends (2026-10-17, both trees green) — single-disconnect confirmed by the child trace log: `OP_DISCONNECT: vendor status 0` present, `teardown: disconnect` absent after clean closes |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins | `CoreTest.Profile*` |
| Profile selection: `mock://`→mock set, `j2534://`→raw, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.DefaultProfileIsRaw`, `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |

## Gaps (not verified)

- **BENCH coverage stops at the quiet bus.** The formal session
  (2026-10-16) verified load, open, filter-shape acceptance and
  quiet-bus read behavior. Still unverified against real hardware:
  talking-bus behavior (request framing, checksum expectations,
  timestamp units), VBATT, ReadMsgs timeout-vs-error discrimination
  (#11), and any vendor status on filter registration itself (#23 —
  acceptance was inferred, not logged).
- **No live vehicle has been connected.**
- **Vendor conformance to J2534-1 v04.04 is unknown.** The shared
  types header is verified against public references, but only a real
  device can catch a vendor that departs from the spec
  ([ADR-0010](adr/0010-j2534-v0404-shared-types.md)).
- **Discovery tests check shape only.** They pass vacuously on a
  machine with no vendors installed (Known Issue #14).
- **The Linux full app isn't built in CI** (Known Issue #3).
- **Mobile kits have never been built** (Known Issue #2).

## Conventions

- **Exact counts against the deterministic fake.** It emits only what
  a test injected or what it echoes back, so a fabricated or dropped
  frame fails the test.
- **Bounded ranges against the timed mock worker.** Its 50 ms tick
  makes exact counts a race, not a contract.
- **Wait by polling for the condition, not with fixed sleeps.** If
  the condition is produced by polling, drain inside the wait loop, or
  the predicate can never become true.
- **Test-only seams** (`dispatchFrameForTesting`,
  `forceFrameCountForTesting`, `maxInFlightForTesting`, the fake's
  hooks, `qttune_j2534_inject_test_frame`) are never called by app
  code.
- **When you change shared plumbing, pin the contract you changed, at
  the point you changed it.** For example,
  `DefaultConnectUsesIso15765At500k` was added alongside the query
  parameters.
