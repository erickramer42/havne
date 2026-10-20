# Testing

What the test suites cover, how much confidence each tier gives, and
where the gaps are. For build and run commands, see
[development.md](development.md#running-tests).

## Verification tiers

| Tier | Meaning | Where it runs |
|---|---|---|
| **Unit** | Core, signal and ABI logic with no I/O | CI on all three desktop platforms |
| **Bridge** | Qt bridge and models, headless (`Qt6::Core` only) against the real 50 ms mock worker | CI on Windows, macOS and Linux, and locally |
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL | CI on Windows (same-arch, all x64) |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on pull requests, and on request: `[ios-smoke]` in the commit message, or manual dispatch (`ios-simulator-smoke`, Intel macOS runner) |
| **Manual smoke** | Running the app: layout, LiveView, strip chart, Settings picker against the live registry | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); talking-bus behaviors pending |
| **Vehicle** | A live, read-only vehicle session | **Not yet reached** |

The manual smoke tier earns its place. The 2026-10-10 strip-chart
indexing bug lived in QML paint code, and every C++ test was blind to
it. The simulator screenshots caught two more layout bugs on their
first good run (2026-10-20). The Home status card was off-centre. And
the compact layout's tab bar, which only narrow windows show, said
"Setup" for the page the sidebar calls "Settings".

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
| `test_obd2` | `Obd2SetTest`, `Obd2DecodeTest`, `Obd2MuxTest`, `PidSchedulerTest` | Nothing (Qt-free) |

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
| Surrogate: spawn under the job object, pipe handshake, batched `OP_READ` delivery, dead DLL fails fast, clean teardown | `J2534SurrogateTest` |
| Write, ioctl and filter relay on both backends, including whitelist refusal and malformed filter shapes | `J2534SurrogateTest`, `J2534RoutingTest.InprocWriteIoctlFilterRoundTrips` |
| PE routing: x64 goes in-proc; a missing DLL gives an honest load failure with no spawn | `J2534RoutingTest` |
| Cross-arch: x64 test → x86 surrogate → x86 fake, loader round trip plus full session lifecycle | `J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` (dev machine) |
| Settings picker: device list, refresh, manual URI connect and disconnect, error surfacing | Manual smoke (Windows, live registry) |
| Full app builds, and ctest passes, on Windows (MSVC 2022), macOS (AppleClang) and Linux (GCC), Qt 6.12.0 | CI `win-build`, `macos-build`, `linux-build` |
| Android arm64-v8a APK builds and packages (NDK r27, JDK 21) | CI `android-build` (build only; first green 2026-10-20) |
| iOS arm64 device build compiles and links, unsigned | CI `ios-build` (build only; first green 2026-10-20) |
| iOS app launches on the Simulator, stays up, and draws: Qt Quick on Metal ("Apple iOS simulator GPU"), on a 375×667 pt iPhone SE-class device | CI `ios-simulator-smoke`, 2026-10-20 (`4be8ba5`: both shots drawn; the first run's single shot was blank) |
| Transport adoption: `?filter=` registers a FLOW_CONTROL filter; `?txid=` selects the transmit ID; `?poll=` drives a Mode-01 loop; header strip + arbitration_id stamp; filter stopped on detach; gating discards non-matching frames; poll-without-filter rejected at parse | `J2534AdoptionTest` |
| A blocked vendor `PassThruOpen` fails at the (shrunken) handshake timeout with the child alive, not wedged | `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout` |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), both backends | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path) plus the whole existing suite against the legacy-spelling fake (fallback path) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally, since the child didn't yet log filter ops (Known Issue #23). The 4-byte shape has since replaced it and hasn't met hardware. Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Single-threaded surrogate child ([ADR-0014](adr/0014-single-threaded-surrogate.md)): the whole surrogate and routing suite on the redesigned child, same-arch and cross-arch (x64 test → x86 surrogate → x86 fake) | `J2534SurrogateTest`, `J2534RoutingTest` (2026-10-19, both trees rebuilt: full suite 136/136, cross-arch subset 15/15) |
| A transaction that times out doesn't put later replies out of step | `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest` |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins | `CoreTest.Profile*` |
| Profile selection: `mock://`→mock set, `j2534://`→obd2, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |
| `obd2` signal set: mux grammar, decoder round-trips against canned wire bytes, validator including channel-decl rejection (duplicates, zero/illegal IDs, over-capacity) | `Obd2SetTest`, `Obd2DecodeTest` |
| Mode-01 scheduler: rotation order in set order, wrap; drain delivers a backlog beyond one cycle's budget | `J2534AdoptionTest` |
| Channel derivation: a bare `j2534://` URI under `obd2` registers the 7E8 filter with exact mask/pattern shape, sends to 7E0, and starts; a lone `?txid=` keeps the derived 7E8 filter but rewrites the request target | `J2534AdoptionTest.BareJ2534UriDerivesObd2ChannelAndStarts`, `.TxidOverrideRewritesDerivedTransmitTarget` |
| Link health (in-proc): a hard read failure records `QT_ERR_TRANSPORT_HARD_FAILURE` with one episode per outage and recovers through the probe write; a quiet bus debounces to `QT_ERR_VEHICLE_TIMEOUT` with no episode | `J2534AdoptionTest.HardFailureSetsCanonicalErrorAndRecovers`, `.QuietBusDebouncesToVehicleTimeoutWithoutEpisode` |
| A batch the vendor returns with `ERR_TIMEOUT` is delivered through the surrogate; an empty one maps to a vehicle timeout | `J2534SurrogateTest.SpecTimeoutReadDeliversPartialBatch` (spec-mode fake) |
| Link-health UI: quiet/fault/degraded banner, activity-log transitions, "—" cells, stale dimming, arbId column | Manual smoke, 2026-10-20 (fake quiet/fault via `QTTUNE_FAKE_READ_STATUS`; mock; recipe above) |
| Fault state on real hardware, clean teardown; 4-byte filter shape accepted with logged vendor status 0 | Informal bench observation 2026-10-20 ([sheet](bench-sessions/2026-10-20.md)) |

## Manual smoke — link-health UI

The QML half of #11 has no automated coverage (nothing drives the
bridge from QML). Recipe — run the app from a shell so the session
inherits the knobs at spawn (works for both in-proc and surrogate
routing; the Win32 child snapshots the environment, so toggling
mid-session only works in-proc):

| Goal | Environment | URI | Expected |
|---|---|---|---|
| Baseline, dimming | — | `mock://demo` | Values stream; slow signals (IAT) dim past 3 s |
| Quiet banner | `QTTUNE_FAKE_READ_STATUS=9` | `j2534://<fake.dll>?profile=raw&filter=0x7E8` | Amber "Vehicle not responding" after ~1.2 s; no frames, empty signal list (raw profile) |
| Fault banner | `QTTUNE_FAKE_READ_STATUS=7` | same | Red "Adapter fault" banner with episode count; clearing the knob mid-session recovers only in-proc |
| Frames + arbId column | — | `j2534://<fake.dll>?profile=raw&filter=0x7E8&poll=0x0C` | Frames on LogPage with `0x7E8` in the ID column, payload `410c1f40` (2000 RPM canned); link ok |
| Disconnect reset | after any of the above | Disconnect | Home dot and Settings label show plain disconnected, not the last session's fault/quiet |

All four states (ok/quiet/fault/disconnected) were verified 2026-10-20,
plus the fault state on the OBDX Pro VX quiet bus (BENCH,
[bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)).

## Gaps (not verified)

- **BENCH coverage stops at the quiet bus.** The formal session
  (2026-10-16) verified load, open, and acceptance of the since-replaced
  8-byte filter shape; an informal 2026-10-20 observation additionally
  confirmed the current 4-byte shape (logged vendor status 0) and the
  fault-banner path on the VX. Still unverified against real hardware:
  talking-bus behavior (request framing, checksum expectations,
  timestamp units), VBATT, ReadMsgs timeout-vs-error discrimination
  (#11), and any vendor status on filter registration itself. That
  session inferred acceptance; the child now logs the vendor's filter
  status, so the next session can record it (#23).
- **Link-health coverage is partial.** The two in-proc tests above
  drive `QTTUNE_FAKE_READ_STATUS` through the worker. Not covered:
  - the surrogate path's health mapping, end to end;
  - backoff growth: the hard-failure test also passes with the old
    reset-to-base backoff;
  - a vendor that leaves the message count untouched on error, because
    the knob always zeroes it (code review 2026-10-20, finding 1);
  - the real-vendor flavors on a talking bus: the VX quiet bus was
    observed once (2026-10-20: empty reads return `NOERROR` with zero
    messages; writes fail `ERR_FAILED` after ~8 cycles on a no-ACK
    bus — [bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)),
    but `ERR_TIMEOUT`-with-data and ECU-present write behavior are
    still unknown.
- **The fake's default read model is simpler than J2534's.** By
  default its `ReadMsgs` returns one message per call whatever
  `NumMsgs` and the timeout ask for, and `STATUS_NOERROR` with zero
  messages on an empty queue. The exact-count stream tests are pinned
  to that. `QTTUNE_FAKE_SPEC_READ_SEMANTICS=1` switches to J2534-1
  behavior: a short batch comes back with `ERR_TIMEOUT`, and an empty
  timeout-0 read with `ERR_BUFFER_EMPTY`. Spec mode still returns at
  once instead of waiting out the timeout, and it skips filter gating.
  The worker never reads with timeout 0, so the `ERR_BUFFER_EMPTY`
  mapping is reachable only through `QTTUNE_FAKE_READ_STATUS=0x10`.
- **ID 0 handling is untested.** The child now tracks open device and
  channel with flags instead of treating ID 0 as "none", but the fake
  always returns device 1 and channel 0x55.
- **The fake emits no ISO 15765 indications.** TX-done and
  first-frame indications never appear, so indication handling (#28)
  is untested.
- **Child survival after a refused op isn't asserted.** The surrogate
  child must stay alive after a vendor refusal (ADR-0014). Tests check
  the refusal's status, but `sur_close` returns `QT_OK` whether or not
  the child is still alive, so an early exit would go unnoticed.
- **Profile-derived channels against a real vendor.** The
  derived-filter shape and per-PID request routing are
  fake-verified; whether the VX accepts them is the natural first
  item for the next bench sheet.
- **No live vehicle has been connected.**
- **Vendor conformance to J2534-1 v04.04 is unknown.** The shared
  types header is verified against public references, but only a real
  device can catch a vendor that departs from the spec
  ([ADR-0010](adr/0010-j2534-v0404-shared-types.md)).
- **Discovery tests check shape only.** They pass vacuously on a
  machine with no vendors installed (Known Issue #14).
- **The mobile apps have never run on a device or emulator** (Known
  Issue #2). CI compiles both kits. The simulator smoke shows the iOS
  app starts and draws, but nothing taps through its UI, and it skips
  ordinary pushes. No tests run on either mobile
  platform, because cross builds set `BUILD_TESTING=OFF`. The Android
  app has never been launched anywhere.
- **CI never launches the Linux or macOS desktop app.** Those jobs
  build the app and run ctest, but nothing opens a window. The manual
  smoke tier covers that, on the dev machine.

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
