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
| **Manual smoke** | Running the app: layout, Dashboard gauges, LiveView, strip chart, Settings picker against the live registry | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); informal quiet-bus follow-up 2026-10-20 ([note](bench-sessions/2026-10-20.md)); talking-bus behaviors pending |
| **Vehicle** | A live, read-only vehicle session | **Not yet reached**; run sheet ready ([vehicle-runsheet.md](bench-sessions/vehicle-runsheet.md)) |

The manual smoke tier earns its place. The 2026-10-10 strip-chart
indexing bug lived in QML paint code, and every C++ test was blind to
it. The simulator screenshots caught two more layout bugs on their
first good run (2026-10-20). The Home status card was off-centre. And
the compact layout's tab bar, which only narrow windows show, said
"Setup" for the page the sidebar calls "Settings". Both were fixed,
and the next run's screenshots confirmed the fixes the same day.

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
| Core builds with zero Qt dependencies | CI `core-and-tests` job (Ubuntu, no Qt installed), plus the configure-time link check in the root `CMakeLists.txt`. That check was verified 2026-10-20 to fire on an injected `Qt6::Core` link, in core-only and in full configures. |
| Core and the Qt-free tests run clean under ASan, UBSan and LeakSanitizer | CI `core-sanitizers` (Linux, blocking). Its first run, 2026-10-20 (`8dcba3a`), found no memory errors or UB, but 5 tests closed their session without destroying it. With those fixed, run #85 (`6991bdd`) passed 74/74. |
| Public C ABI headers are valid strict C | `c_abi` |
| Callback registration: dedup, idempotent unregister, reentrant register/unregister during dispatch | `CoreTest` |
| Callbacks run on the worker thread | `CoreTest`, through `qttune_session_start` |
| Session lifecycle: idempotent close, close/destroy split | `CoreTest` |
| Signal decode: bit extraction, sign extension, error-code order, validator | `SignalDecodeTest`, `SignalsTest` |
| Mock frames decode to physically plausible values | `CoreTest.MockFramesDecodeAgainstMockSignalSet` |
| Mock drive cycle, stepped deterministically: every phase in order; 2nd- and 3rd-gear pulls hold the limiter exactly 5 s; the 3rd-gear pull is longer, faster and hotter; the drag run crosses 402 m in 11–15 s at 150–200 km/h through 4th; every tick of two loops round-trips through the mock set without clamping | `CoreTest.MockDriveCycle*` |
| Frames marshal from the worker to the GUI thread | `BridgeTest.FramesMarshalFromWorkerToGuiThread` |
| Signal updates are coalesced to 10 Hz whatever the frame rate | `BridgeTest.SignalBatchesAreCoalesced` |
| History ring eviction, `valuesUpdated` semantics; `chartData()` reports each series' model row, which keys its trace colour | `BridgeTest` |
| Streaming continues past 10k cumulative frames; drops are counted and reported | `BridgeTest.StreamingContinuesPastTenThousandFrames`, `.BackpressureDropsAreCountedAndReported` |
| Log sort toggle (newest first / oldest first) | `BridgeTest.SortProxy*` |
| The frame role names the Log page binds (`timestampUs`, `dlc`, `extended`, `payloadHex`, `arbId`), read through the sort proxy | `BridgeTest.FrameRoleNamesMatchLogPageBindings` |
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
| iOS app launches on the Simulator, stays up, and draws: Qt Quick on Metal ("Apple iOS simulator GPU"), on a 375×667 pt iPhone SE-class device | CI `ios-simulator-smoke`, 2026-10-20 (`4be8ba5` and `a0f5c0f`: both shots drawn; the first run's single shot was blank) |
| Transport adoption: `?filter=` registers a FLOW_CONTROL filter; `?txid=` selects the transmit ID; `?poll=` drives a Mode-01 loop; header strip + arbitration_id stamp; filter stopped on detach; gating discards non-matching frames; `?poll=` with no channel (`raw`, no `?filter=`) rejected at attach, before any DLL load | `J2534AdoptionTest` |
| `?poll=` outranks the profile's rotation: under `obd2`, five consecutive writes all request the URI's PID, to the derived 7E0 channel, and the reply decodes with the 7E8 identity | `J2534AdoptionTest.PollOverridesSchedulerRotation` |
| A blocked vendor `PassThruOpen` fails at the (shrunken) handshake timeout with the child alive, not wedged | `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout` |
| Session log: the client's lines share the child's log file, stamped and written as whole lines. A second concurrent session gets its own per-spawn log instead of none. | `J2534SurrogateTest.ClientAndChildLinesShareTheSessionLog`, `.SecondConcurrentSessionGetsItsOwnLog`; both also cross-arch (2026-10-20, 19/19) |
| Payload logging: with `QTTUNE_SUR_LOG_FRAMES=1` at spawn, the child logs every written and read message verbatim (status, TxFlags or RxStatus, vendor timestamp, bytes); without it, no frame lines | `J2534SurrogateTest.FrameLoggingRecordsPayloadsVerbatimWhenEnabled`, also cross-arch (2026-10-21, 20/20); end to end in the app through the x86 surrogate, 2026-10-21 |
| A child that exits during `OP_OPEN` (failed DLL load) is reported by its exit code, not as a blocked vendor. A missing surrogate exe leaves its `CreateProcess failed` line in the session log. | Session logs read by hand, 2026-10-20 (`DeadDllFailsFastAtOpen`, `BlockedVendorOpenFailsAtHandshakeTimeout`, and a run with `QTTUNE_SURROGATE_EXE` pointing nowhere) |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), both backends | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path) plus the whole existing suite against the legacy-spelling fake (fallback path) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally, since the child didn't yet log filter ops. The 4-byte shape that replaced it was accepted on 2026-10-20 (row below). Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Single-threaded surrogate child ([ADR-0014](adr/0014-single-threaded-surrogate.md)): the whole surrogate and routing suite on the redesigned child, same-arch and cross-arch (x64 test → x86 surrogate → x86 fake) | `J2534SurrogateTest`, `J2534RoutingTest` (2026-10-19, both trees rebuilt: full suite 136/136, cross-arch subset 15/15) |
| A transaction that times out doesn't put later replies out of step | `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest` |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins | `CoreTest.Profile*` |
| Profile selection: `mock://`→mock set, `j2534://`→obd2, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |
| `obd2` signal set: mux grammar, decoder round-trips against canned wire bytes, validator including channel-decl rejection (duplicates, zero/illegal IDs, over-capacity) | `Obd2SetTest`, `Obd2DecodeTest` |
| Mode-01 scheduler: rotation order in set order, wrap; drain delivers a backlog beyond one cycle's budget | `J2534AdoptionTest` |
| Channel derivation: a bare `j2534://` URI under `obd2` registers the 7E8 filter with exact mask/pattern shape, sends to 7E0, and starts; a lone `?txid=` keeps the derived 7E8 filter but rewrites the request target | `J2534AdoptionTest.BareJ2534UriDerivesObd2ChannelAndStarts`, `.TxidOverrideRewritesDerivedTransmitTarget` |
| Link health (in-proc): a hard read failure records `QT_ERR_TRANSPORT_HARD_FAILURE` with one episode per outage and recovers through the probe write; a quiet bus debounces to `QT_ERR_VEHICLE_TIMEOUT` with no episode | `J2534AdoptionTest.HardFailureSetsCanonicalErrorAndRecovers`, `.QuietBusDebouncesToVehicleTimeoutWithoutEpisode` |
| Write failures (in-proc): a failed write latches `QT_ERR_REQUEST_FAILED` with one episode; empty reads past the quiet debounce don't move it; a successful write alone clears it, with reads held quiet | `J2534AdoptionTest.WriteFailureLatchesRequestFailedAndClears` (`QTTUNE_FAKE_WRITE_STATUS`) |
| A hard read failure outranks a failing write: exactly two episodes (write, then read), and continued failing writes don't downgrade the state | `J2534AdoptionTest.HardReadFailureOutranksRequestFailed` |
| A delivered frame that matches no channel ends a hard failure but reads as `QT_ERR_VEHICLE_TIMEOUT`, not `QT_OK`, with no episode, inside the quiet debounce window | `J2534AdoptionTest.UnmatchedDeliveredFrameGoesQuietNotHealthy` |
| The backoff grows under a sustained hard failure: at most 8 probe writes in 1.5 s, against ~15 with the old reset-to-base | `J2534AdoptionTest.BackoffGrowsUnderSustainedHardFailure` |
| The backoff step (`j2534_backoff_next`, shared by all three worker modes) doubles from the base, never exceeds the 5 s cap, including the step that lands on it, holds at the cap, and doesn't wrap | `J2534StreamTest.BackoffStepDoublesToCapWithoutOvershoot` (session-free) |
| `QT_ERR_REQUEST_FAILED` has its own status string | `J2534StreamTest.RequestFailedStatusHasDistinctString` |
| A batch the vendor returns with `ERR_TIMEOUT` is delivered through the surrogate; an empty one maps to a vehicle timeout | `J2534SurrogateTest.SpecTimeoutReadDeliversPartialBatch` (spec-mode fake) |
| A vendor that returns a hard error with the message count untouched delivers no frames: in-proc, the session reports a hard failure, dispatches nothing and recovers once reads are honest; through the surrogate, the read maps to a hard failure and the next request still correlates | `J2534AdoptionTest.UntouchedCountUnderHardErrorReportsUnhealthy`, `J2534SurrogateTest.HardErrorUntouchedCountDeliversNoFrames` (`QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS`) |
| Signal freshness: `ageMs` is -1 before the first sample and a sane clock delta after; `updateValues` notifies `AgeMsRole`; `refreshAges()` notifies `AgeMsRole` alone, over every row, so a stalled row re-evaluates | `BridgeTest.AgeMsRoleEmittedAndRefreshesWhileStalled` |
| Link-state mapping: `QT_OK` → `ok`, vehicle timeout → `quiet`, `QT_ERR_REQUEST_FAILED` → `requests`, hard failure → `fault`, any other code → `degraded` | `BridgeTest.LinkStateMapsEveryPublishedCode` (session-free, through `QtTuneBridge::linkStateForError`) |
| Dashboard: every gauge the mock set feeds sweeps under `mock://demo`; a gauge whose signal the profile lacks reads "not in profile" (the mock set feeds every gauge, so `?profile=raw` is what shows it); the grid collapses from 4 to 2 columns across 760 px and switches to phone margins at 600 px; fault and quiet states tint correctly | Manual smoke, 2026-10-20 (`40e51ce`; quiet and fault through the knob recipe below). The header, which takes its dot and label from `Theme`, was re-run 2026-10-21 (`55373f4` plus the reordered series palette): ok ("Live · mock://demo", green), quiet, requests, fault and disconnected, plus "not in profile" on every gauge under `?profile=raw`. No automated coverage: `SignalHub`'s name lookup and the gauges are QML only |
| Live: the list sizes to its rows up to a cap, then scrolls with the bar pinned on; each row's swatch matches its trace; rows past `Theme.seriesColors` reuse its colours with a dashed trace (under `obd2`, the last two rows) | Manual smoke, 2026-10-21 (`55373f4` plus the reordered series palette): all eight mock rows fit and each swatch matches its trace; under `obd2` through the x64 fake, ten rows with the scroll bar pinned on, and MAF (row 9) drawn dashed in row 1's colour. The fake's canned values are constant, so every trace is flat and they overlap; the dash was seen with MAF as the only plotted row. No automated coverage: the swatches, palette and dashes are QML only; `BridgeTest` pins the `row` they key on |
| Link-health UI: quiet/fault/degraded banner, activity-log transitions, "—" cells, stale dimming, the arbId column | Manual smoke, 2026-10-20 (fake quiet/fault via `QTTUNE_FAKE_READ_STATUS`; mock; recipe below). The first run recorded dimming and the arbId column as working when the code couldn't produce either (dimming keyed on value changes; the ID column bound a misspelled role). Both rows were re-run and passed after the fixes, the arbId row through the x86 surrogate. |
| Fault state on real hardware, clean teardown; 4-byte filter shape accepted with logged vendor status 0 | Informal bench observation 2026-10-20 ([sheet](bench-sessions/2026-10-20.md)), under the rules before `946bdf5`. The same no-ACK setup now latches `QT_ERR_REQUEST_FAILED` instead, which hasn't been seen on hardware. |

## Manual smoke — link-health UI

The QML half of #11 has no automated coverage (nothing drives the
bridge from QML). Recipe — run the app from a shell so the session
inherits the knobs at spawn (works for both in-proc and surrogate
routing; the Win32 child snapshots the environment, so toggling
mid-session only works in-proc):

| Goal | Environment | URI | Expected |
|---|---|---|---|
| Quiet banner | `QTTUNE_FAKE_READ_STATUS=9` | `j2534://<fake.dll>?profile=raw&filter=0x7E8` | Amber "Vehicle not responding" after ~1.2 s; no frames, empty signal list (raw profile) |
| Fault banner | `QTTUNE_FAKE_READ_STATUS=7` | same | Red "Adapter fault" banner with episode count; clearing the knob mid-session recovers only in-proc |
| Requests banner | `QTTUNE_FAKE_WRITE_STATUS=7` | `j2534://<fake.dll>` (bare: the `obd2` scheduler writes every cycle) | At the first failed write: amber "Requests failing — check the vehicle connection and ignition" on LiveView, "Requests failing" with an amber dot on Home and the Dashboard header, an amber Settings label, and "Link: requests failing (1 episode(s))" in the activity log. **Verified 2026-10-21** (`55373f4` plus the reordered series palette, x64 fake in-proc): every surface as listed; values stay "—" |
| Frames + arbId column | — | `j2534://<fake.dll>?profile=raw&filter=0x7E8&poll=0x0C` | Frames on LogPage with `0x7E8` in the ID column, DLC 4, payload `410c1f40` (2000 RPM canned); link ok, no banner. **Verified 2026-10-20** after the role-name fix (`1674f5c`), using the Win32-tree fake (`build-win32/tests/Release/fake_j2534.dll`). The session log confirmed it ran through the x86 surrogate, so this row also covers surrogate routing, the strip/stamp path and poll mode in the app. It doesn't exercise `?poll=` outranking a profile's rotation: `raw` has no scheduler. |
| Dimming baseline | — | `mock://demo` | Values stream at full opacity, steady ones (IAT) included, because every mock signal arrives every 50 ms. Only a signal that stops arriving dims, 3 s after its last sample. **Verified 2026-10-20** after dimming moved to the model's `ageMs` (`7e9bd5e`): nothing dims on mock. The first run had expected IAT to dim, which was the bug itself. |
| Disconnect reset | after any of the above | Disconnect | Home dot and Settings label show plain disconnected, not the last session's fault/quiet |

All four states (ok/quiet/fault/disconnected) were verified 2026-10-20,
plus the fault state on the OBDX Pro VX quiet bus (BENCH,
[bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)). The
first run's dimming and arbId results couldn't have been real. Both
rows were re-run after the fixes the same day and passed. The
requests-banner row, which covers the `requests` state, passed on
2026-10-21, along with a re-run of the quiet, fault and disconnect
rows against the current Home, Settings and Dashboard header.

## Gaps (not verified)

- **BENCH coverage stops at the quiet bus.** The formal session
  (2026-10-16) verified load, open, and acceptance of the since-replaced
  8-byte filter shape; an informal 2026-10-20 observation additionally
  confirmed the current 4-byte shape (logged vendor status 0), the
  first accepted Mode-01 request, and the fault-banner path on the VX.
  Still unverified against real hardware: talking-bus behavior
  (replies to requests, checksum expectations, timestamp units),
  VBATT, and whether a vendor ever pairs `ERR_TIMEOUT` with data
  (#11).
- **Link-health coverage is in-proc only.** The tests above drive
  `QTTUNE_FAKE_READ_STATUS`, `QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS` and
  `QTTUNE_FAKE_WRITE_STATUS` through the worker. Not covered:
  - the surrogate path's health mapping through the worker, end to
    end (the surrogate test stops at `read_frame`'s status). Accepted
    until the first vehicle session, which drives it live through the
    surrogate, with the session log as the record;
  - how the UI renders `QT_ERR_REQUEST_FAILED`: the bridge's mapping
    to `requests` is unit-tested, but the QML banner, Home card,
    Settings label and log line are covered only by the smoke recipe's
    requests-banner row, which hasn't been run yet;
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
  A vendor that leaves its outputs untouched on error is modelled
  since 2026-10-20 (`QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS`).
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
- **The client's skew and late-reply paths aren't exercised.** No test
  child sends a `num_frames` or ioctl `data_len` past the wire limit,
  and none answers an `OP_READ` late enough for `drain_pending` to
  drop frames. Reaching either needs a misbehaving child, such as a
  fault-injection knob in the surrogate.
- **Profile-derived channels against a real vendor.** The
  derived-filter shape and per-PID request routing are
  fake-verified. On 2026-10-20 a bare `j2534://` URI got the derived
  filter and its first request accepted by the VX, but no ECU
  answered, so the receive side is unverified.
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
