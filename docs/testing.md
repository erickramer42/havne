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
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on pull requests, and on request: a manual run (Actions → CI → Run workflow; `smoke_only` skips the other platform jobs), or `[ios-smoke]` in the commit message (`ios-simulator-smoke`, Intel macOS runner) |
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
| `test_c_abi` (ctest name `c_abi`) | Every public header compiles as ISO C11 with extensions off (`-pedantic` on GCC and Clang) and links unmangled; layout asserts | A C compiler |
| `test_bridge` | `BridgeTest` | Qt Core. Compiles `app/qttunebridge.cpp` directly, so the bridge must stay QtCore-only. |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_surrogate` | `J2534SurrogateTest` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_routing` | `J2534RoutingTest` | Windows. The cross-arch case skips unless `ECUIO_FAKE_DLL` is set. |
| `test_obd2` | `Obd2SetTest`, `Obd2DecodeTest`, `Obd2MuxTest`, `PidSchedulerTest` | Nothing (Qt-free) |

`ctest -N` lists every case. The current total is in
[STATUS.md](../STATUS.md).

## What is verified

| Capability | Verified by |
|---|---|
| Core builds with zero Qt dependencies | CI `core-and-tests` job (Ubuntu, no Qt installed), plus the configure-time link check in the root `CMakeLists.txt`. That check was verified 2026-10-20 to fire on an injected `Qt6::Core` link, in core-only and in full configures. |
| Core and the Qt-free tests run clean under ASan, UBSan and LeakSanitizer | CI `core-sanitizers` (Linux, blocking). Its first run, 2026-10-20 (`8dcba3a`), found no memory errors or UB, but 5 tests closed their session without destroying it. With those fixed, run #85 (`6991bdd`) passed 74/74. |
| Every public C ABI header (`core.h`, `signals.h`, `signal_sets.h`, `profile.h`, `j2534.h`, `j2534_surrogate.h`) is valid strict C, each included on its own first | `c_abi` |
| Callback registration: dedup, idempotent unregister, reentrant register/unregister during dispatch | `CoreTest` |
| Callbacks run on the worker thread | `CoreTest`, through `ecuio_session_start` |
| Session lifecycle: idempotent close, close/destroy split | `CoreTest` |
| Signal decode: bit extraction, sign extension, error-code order (the kind and byte order before the bit parameters), the validator; results in `double`, exact for every 32-bit raw value; 16-bit payload lengths and start bytes past byte 255; binary32 floats and signed integers in both byte orders | `SignalDecodeTest` (including `.Uint32DecodesExactly`, `.PayloadLengthPastByte255IsNotNarrowed`, `.StartBytePastByte255Reaches`, `.FloatDecodesInBothByteOrders`, `.BigEndianIntSignExtends`), `SignalsTest` |
| The validator accepts exactly what the decoder decodes: `TEXT`, unknown kinds and unknown byte orders are refused; a `FLOAT` must be 32 bits at bit 0, in either byte order; big-endian fields must be whole bytes; every reserved byte and `flags` must be zero; a set's layout version is checked before anything else, and its reserved fields must be zero; a set can't repeat a def ID; an empty set (no defs, maybe channels) is valid | `SignalsTest.ValidatorAcceptsExactlyWhatTheDecoderDecodes`, `.ValidatorRejectsKindsWithNoDecoder`, `.ValidatorRejectsUnknownByteOrder`, `.ValidatorRequiresFloatAs32BitsAtBitZero`, `.ValidatorRequiresByteAlignedBigEndian`, `.ValidatorRefusesEveryNonzeroReservedByte`, `.SetValidationChecksLayoutVersionFirst`, `.SetValidationRefusesNonzeroReservedFields`, `.SetValidationRejectsDuplicateIds`, `.SetValidationAcceptsAnEmptySet` |
| The def's layout: 120 bytes with no padding, every field's offset, and `ECUIO_SIGNAL_SET_LAYOUT`; the size and key offsets again in C, with `ecuio_validation_t`'s | `SignalsTest.SignalDefLayoutIsPinned`, `c_abi`; both also built and run in the Win32 tree by hand, 2026-10-22, since no suite runs them on x86 |
| Validation names the failing element: the def or channel at fault, the second of a duplicate pair, and `UINT32_MAX` for a pass or a code about the whole set. Registration hands the result back, with code 0 when it refused on the name or the set is NULL | `SignalsTest.SetValidationNamesTheFailingElement`, `CoreTest.ProfileRegisterReportsTheValidatorResult` |
| The frame gate `ecuio_def_applies_to_frame` is a true predicate: 1 on a match, 0 for another ID, a missing or different mux key, or NULL | `SignalsTest.ApplyGateIsTruePredicate`, `Obd2MuxTest` |
| Mux keys are 1 to 3 bytes, read first byte most significant in either byte order, and every key byte must match. A 4-byte key is refused by the validator (code 9) and the decoder (code 3) alike, and the gate never applies it; a key past the 64-byte payload or wider than its length is refused (10, 11) | `SignalDecodeTest.MuxKeyReadsFirstByteMostSignificantInEitherOrder`, `.ThreeByteMuxKeyGatesAndDecodes`, `SignalsTest.ValidatorBoundsMuxKeysToThreeBytes` |
| Display strings: every quantity the mock and `obd2` sets both carry has one name, unit and decimal places in both (the names the Dashboard's `Tap`s look up); temperatures are a UTF-8 degree sign, never `degC` | `SignalsTest.MockAndObd2ShareDisplayNamesAndUnits` |
| Mock frames decode to physically plausible values | `CoreTest.MockFramesDecodeAgainstMockSignalSet` |
| Mock drive cycle, stepped deterministically: every phase in order; 2nd- and 3rd-gear pulls hold the limiter exactly 5 s; the 3rd-gear pull is longer, faster and hotter; the drag run crosses 402 m in 11–15 s at 150–200 km/h through 4th; every tick of two loops round-trips through the mock set without clamping | `CoreTest.MockDriveCycle*` |
| Frames marshal from the worker to the GUI thread | `BridgeTest.FramesMarshalFromWorkerToGuiThread` |
| Signal updates are coalesced to 10 Hz whatever the frame rate | `BridgeTest.SignalBatchesAreCoalesced` |
| History ring eviction, `valuesUpdated` semantics; `chartData()` reports each series' model row, which keys its trace colour | `BridgeTest` |
| A subscription only chooses what the chart plots: an unsubscribed row keeps taking values and history, so the Dashboard stays live | `BridgeTest.UnsubscribedSignalKeepsUpdating` |
| Display precision: `DecimalsRole` and `chartData()`'s `decimals` carry each def's `decimal_places` | `BridgeTest.DecimalsComeFromTheDefAndReachTheChart` |
| The signal role names LiveView and `SignalHub` take as required properties (`name`, `unit`, `value`, `subscribed`, `hasValue`, `ageMs`, `decimals`) | `BridgeTest.SignalRoleNamesMatchQmlBindings` |
| An async connect starts its session only on the GUI thread; a bridge destroyed before the completion runs destroys the unstarted session | `BridgeTest.DestroyedMidConnectLeavesNoRunningSession` (a probe callback sees no frames before the completion) |
| The vendor calls run off the GUI thread: an async connect's open, and a disconnect's teardown. Disconnect drops the UI state at once, refuses a connect until the teardown ends, and logs `Disconnecting:` then `Disconnected:`; a bridge destroyed mid-teardown waits for it | `BridgeTest.AsyncConnectOpensOffGuiThread`, `.DisconnectTearsDownOffGuiThread`, `.DestroyedMidDisconnectWaitsForTeardown` (Windows only: the fake's open and disconnect delays stall the call, and `GetLastCallThreadIds` names the thread that made it) |
| Streaming continues past 10k cumulative frames; drops are counted and reported | `BridgeTest.StreamingContinuesPastTenThousandFrames`, `.BackpressureDropsAreCountedAndReported` |
| Log sort toggle (newest first / oldest first). Rows sort by arrival, not by frame timestamp, and a connect starts an empty table while a disconnect keeps the rows | `BridgeTest.SortProxy*`, `.FrameLogOrdersByArrivalNotTimestamp`, `.ConnectStartsAnEmptyFrameLog` |
| The frame role names the Log page binds (`timestampUs`, `dlc`, `extended`, `payloadHex`, `arbId`), read through the sort proxy | `BridgeTest.FrameRoleNamesMatchLogPageBindings` |
| Log payloads are the frame's own length as spaced uppercase hex (`34 12 AA 55`) | `BridgeTest.DispatchedShortFrameHonorsDataLength` |
| Log Pause: nothing is appended while paused, while frames still count and decode; disconnect clears it and a new session starts unpaused | `BridgeTest.LogPauseStopsFrameLogNotDecode` |
| Discovery: dual-root, dual-WOW64 sweep, dedup, PE architecture annotation | `J2534DiscoveryTest`, plus manual smoke against real installed drivers (OBDX Pro VT and VX) |
| Bridge discovery marshaling, including the architecture field | `BridgeTest.DiscoveredDevicesHaveWellFormedEntries` |
| Loader: `LoadLibraryEx`, nine-export resolution, `PassThruOpen`, clean unwind on a missing DLL | `FakeFixture` |
| Teardown call order against the vendor API | `FakeFixture.OpenCloseCycleHitsVendorApi`, plus surrogate teardown |
| `j2534://` session end to end with exact counts, payload fidelity and FIFO order | `J2534StreamTest` |
| `?baud=` and `?flags=` reach `PassThruConnect`; defaults kept; malformed URIs rejected before any DLL load | `J2534QueryParamTest` |
| 29-bit channels connect with `CAN_29BIT_ID` (from `?filter=`/`?txid=` or a profile), on top of `?flags=`; a profile mixing 11- and 29-bit channels is rejected at create | `J2534QueryParamTest.Wide29BitChannelSetsConnectFlag`, `.ProfileChannelWidthDecidesConnectFlag` |
| Surrogate: spawn under the job object, pipe handshake, batched `OP_READ` delivery, dead DLL fails fast, clean teardown | `J2534SurrogateTest` |
| Write, ioctl and filter relay on both backends, including whitelist refusal and malformed filter shapes | `J2534SurrogateTest`, `J2534RoutingTest.InprocWriteIoctlFilterRoundTrips` |
| PE routing: x64 goes in-proc; a missing DLL gives an honest load failure with no spawn | `J2534RoutingTest` |
| Paths are UTF-8 to UTF-16: a DLL in a directory named in Greek and CJK (outside every ANSI code page) loads in-proc and in the surrogate child, same-arch and cross-arch. With `LoadLibraryExA` the in-proc case fails (checked by hand, 2026-10-21, on code page 1252) | `FakeFixture.NonAsciiDllPathLoadsInProc`, `J2534SurrogateTest.NonAsciiDllPathLoadsInTheChild` |
| `ECUIO_SURROGATE_EXE` only relocates the surrogate: a relative path, another exe's name or a missing file fails the open; the spawn line names the exe used | `J2534SurrogateTest.SurrogateExeOverrideIsRestricted`, `.ClientAndChildLinesShareTheSessionLog` |
| Cross-arch: x64 test → x86 surrogate → x86 fake, loader round trip plus full session lifecycle | `J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` (dev machine) |
| Settings picker: device list, refresh, manual URI connect and disconnect, error surfacing | Manual smoke (Windows, live registry) |
| Full app builds, and ctest passes, on Windows (MSVC 2022), macOS (AppleClang) and Linux (GCC), Qt 6.12.0 | CI `win-build`, `macos-build`, `linux-build` |
| Android arm64-v8a APK builds and packages (NDK r27, JDK 21) | CI `android-build` (build only; first green 2026-10-20) |
| iOS arm64 device build compiles and links, unsigned | CI `ios-build` (build only; first green 2026-10-20) |
| iOS app launches on the Simulator, stays up, and draws: Qt Quick on Metal ("Apple iOS simulator GPU"), on a 375×667 pt iPhone SE-class device | CI `ios-simulator-smoke`, 2026-10-20 (`4be8ba5` and `a0f5c0f`: both shots drawn; the first run's single shot was blank) |
| Transport adoption: `?filter=` registers a FLOW_CONTROL filter; `?txid=` selects the transmit ID; `?poll=` drives a Mode-01 loop; header strip + arbitration_id stamp; filter stopped on detach; gating discards non-matching frames; `?poll=` with no channel (`raw`, no `?filter=`) rejected at attach, before any DLL load | `J2534AdoptionTest` |
| `?poll=` outranks the profile's rotation: under `obd2`, five consecutive writes all request the URI's PID, to the derived 7E0 channel, and the reply decodes with the 7E8 identity | `J2534AdoptionTest.PollOverridesSchedulerRotation` |
| `?poll=0x00` requests the supported-PIDs bitmap and its reply arrives with the 7E8 identity | `J2534AdoptionTest.PollZeroRequestsSupportedPidBitmap`; in the app through the fake, 2026-10-21 |
| A blocked vendor `PassThruOpen` fails at the (shrunken) handshake timeout with the child alive, not wedged | `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout` |
| A child whose build fingerprint differs from the client's refuses `OP_OPEN` before calling the vendor, and the open fails fast. Editing a shared header and rebuilding one tree changes only that tree's fingerprint (checked by hand, 2026-10-22) | `J2534SurrogateTest.MismatchedFingerprintRefusesOpenBeforeVendor`, also cross-arch (2026-10-22, 24/24) |
| Session log: the client's lines share the child's log file, stamped and written as whole lines. A second concurrent session gets its own per-spawn log instead of none. Each spawn keeps the log it replaces under a dated name, the newest 50 (the cap checked by hand, 2026-10-22: 50 kept after four surrogate-suite runs) | `J2534SurrogateTest.ClientAndChildLinesShareTheSessionLog`, `.SecondConcurrentSessionGetsItsOwnLog`; both also cross-arch (2026-10-20, 19/19). `.EarlierSessionLogIsKept`, also cross-arch (2026-10-22, 25/25) |
| Payload logging: with `ECUIO_SUR_LOG_FRAMES=1` at spawn, the child logs every written and read message verbatim (status, TxFlags or RxStatus, vendor timestamp, bytes); without it, no frame lines | `J2534SurrogateTest.FrameLoggingRecordsPayloadsVerbatimWhenEnabled`, also cross-arch (2026-10-21, 20/20); end to end in the app through the x86 surrogate, 2026-10-21 |
| A child that misses two deadlines in a row is marked unresponsive: later transactions fail at once and close kills it without waiting | `J2534SurrogateTest.UnresponsiveChildFailsFastAfterTwoMissedDeadlines`, also cross-arch (2026-10-21, 21/21) |
| A child that exits during `OP_OPEN` (failed DLL load) is reported by its exit code, not as a blocked vendor. A missing surrogate exe leaves its `CreateProcess failed` line in the session log. | Session logs read by hand, 2026-10-20 (`DeadDllFailsFastAtOpen`, `BlockedVendorOpenFailsAtHandshakeTimeout`, and a run with `ECUIO_SURROGATE_EXE` pointing nowhere) |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), both backends | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path) plus the whole existing suite against the legacy-spelling fake (fallback path) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally, since the child didn't yet log filter ops. The 4-byte shape that replaced it was accepted on 2026-10-20 (row below). Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Single-threaded surrogate child ([ADR-0014](adr/0014-single-threaded-surrogate.md)): the whole surrogate and routing suite on the redesigned child, same-arch and cross-arch (x64 test → x86 surrogate → x86 fake) | `J2534SurrogateTest`, `J2534RoutingTest` (2026-10-19, both trees rebuilt: full suite 136/136, cross-arch subset 15/15) |
| A transaction that times out doesn't put later replies out of step | `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest` |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins; registration refuses an invalid set or a malformed name, and reports the validator's result; the built-ins can't be replaced or unregistered; unregister is idempotent and leaves existing sessions on their set | `CoreTest.Profile*` |
| `mock://` shares the query grammar: only `profile`, and every malformed form (a bare `?`, an empty segment, a missing `=`, an uppercase key) fails at create | `CoreTest.MockUriQueryIsStrict` |
| Profile selection: `mock://`→mock set, `j2534://`→obd2, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |
| `obd2` signal set: mux grammar, decoder round-trips against canned wire bytes, validator including channel-decl rejection (duplicates, zero/illegal IDs, over-capacity) | `Obd2SetTest`, `Obd2DecodeTest` |
| Mode-01 scheduler: rotation order in set order, wrap; drain delivers a backlog beyond one cycle's budget | `J2534AdoptionTest` |
| One Mode-01 key test for enrolment and request routing: only a 2-byte `41 <PID>` key at offset 0 enrols a PID or routes its request. A `62 <DID>` key ending in the same byte, another service's 2-byte echo, a key at offset 1 or a 1-byte key does neither; a `62 <DID>` def on a second ECU, listed first, doesn't draw the PID's requests to that ECU | `PidSchedulerTest.Mode01KeyIsTwoBytes41AtOffsetZero`, `.EnrollsOnlyMode01Keys`, `J2534AdoptionTest.Mode01RequestRoutesPastOtherServiceKeys`. With routing that matches any key's last byte, the routing case fails, sending the requests to 0x7E1 (checked by hand, 2026-10-22) |
| Channel derivation: a bare `j2534://` URI under `obd2` registers the 7E8 filter with exact mask/pattern shape, sends to 7E0, and starts; a lone `?txid=` keeps the derived 7E8 filter but rewrites the request target | `J2534AdoptionTest.BareJ2534UriDerivesObd2ChannelAndStarts`, `.TxidOverrideRewritesDerivedTransmitTarget` |
| Link health (in-proc): a hard read failure records `ECUIO_ERR_TRANSPORT_HARD_FAILURE` with one episode per outage and recovers through the probe write; a quiet bus debounces to `ECUIO_ERR_VEHICLE_TIMEOUT` with no episode | `J2534AdoptionTest.HardFailureSetsCanonicalErrorAndRecovers`, `.QuietBusDebouncesToVehicleTimeoutWithoutEpisode` |
| Write failures (in-proc): a failed write latches `ECUIO_ERR_REQUEST_FAILED` with one episode; empty reads past the quiet debounce don't move it; a successful write alone clears it, with reads held quiet | `J2534AdoptionTest.WriteFailureLatchesRequestFailedAndClears` (`ECUIO_FAKE_WRITE_STATUS`) |
| A hard read failure outranks a failing write: exactly two episodes (write, then read), and continued failing writes don't downgrade the state | `J2534AdoptionTest.HardReadFailureOutranksRequestFailed` |
| A delivered frame that matches no channel ends a hard failure but reads as `ECUIO_ERR_VEHICLE_TIMEOUT`, not `ECUIO_OK`, with no episode, inside the quiet debounce window | `J2534AdoptionTest.UnmatchedDeliveredFrameGoesQuietNotHealthy` |
| The backoff grows under a sustained hard failure: at most 8 probe writes in 1.5 s, against ~15 with the old reset-to-base | `J2534AdoptionTest.BackoffGrowsUnderSustainedHardFailure` |
| The backoff step (`j2534_backoff_next`, shared by all three worker modes) doubles from the base, never exceeds the 5 s cap, including the step that lands on it, holds at the cap, and doesn't wrap | `J2534StreamTest.BackoffStepDoublesToCapWithoutOvershoot` (session-free) |
| `ECUIO_ERR_REQUEST_FAILED` has its own status string | `J2534StreamTest.RequestFailedStatusHasDistinctString` |
| A batch the vendor returns with `ERR_TIMEOUT` is delivered through the surrogate; an empty one maps to a vehicle timeout | `J2534SurrogateTest.SpecTimeoutReadDeliversPartialBatch` (spec-mode fake) |
| A vendor that returns a hard error with the message count untouched delivers no frames: in-proc, the session reports a hard failure, dispatches nothing and recovers once reads are honest; through the surrogate, the read maps to a hard failure and the next request still correlates | `J2534AdoptionTest.UntouchedCountUnderHardErrorReportsUnhealthy`, `J2534SurrogateTest.HardErrorUntouchedCountDeliversNoFrames` (`ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`) |
| Signal freshness: `ageMs` is -1 before the first sample and a sane clock delta after; `updateValues` notifies `AgeMsRole`; `refreshAges()` notifies `AgeMsRole` alone, over every row, so a stalled row re-evaluates | `BridgeTest.AgeMsRoleEmittedAndRefreshesWhileStalled` |
| Link-state mapping: `ECUIO_OK` → `ok`, vehicle timeout → `quiet`, `ECUIO_ERR_REQUEST_FAILED` → `requests`, hard failure → `fault`, any other code → `degraded` | `BridgeTest.LinkStateMapsEveryPublishedCode` (session-free, through `QtTuneBridge::linkStateForError`) |
| Dashboard: every gauge the mock set feeds sweeps under `mock://demo`; a gauge whose signal the profile lacks reads "not in profile" (the mock set feeds every gauge, so `?profile=raw` is what shows it); the grid collapses from 4 to 2 columns across 760 px and tightens its gaps at 600 px; fault and quiet states tint correctly; the temperature rings start at 0 °C, so a cold engine reads on the scale; unticking a row on Live leaves its gauge live | Manual smoke, 2026-10-20 (`40e51ce`; quiet and fault through the knob recipe below). The header, which takes its dot and label from `Theme`, was re-run 2026-10-21 (`55373f4` plus the reordered series palette): ok ("Live · mock://demo", green), quiet, requests, fault and disconnected, plus "not in profile" on every gauge under `?profile=raw`. Re-run later on 2026-10-21 for the rings and the untick (working tree on `d1c20c0`): coolant 53 °C and oil 24 °C on 0–130 and 0–160 scales under mock; with RPM unticked the tach kept moving and recorded a 7,000 peak; every gauge found its signal under `obd2` through the x64 fake. No automated coverage: `SignalHub`'s name lookup and the gauges are QML only |
| Live: the list sizes to its rows up to a cap, then scrolls with the bar pinned on; each row's swatch matches its trace; rows past `Theme.seriesColors` reuse its colours with a dashed trace (under `obd2`, the last two rows); values at each def's decimal places; the chart prints each trace's value at its right-hand end, nudged apart, with a sample of its stroke, has a seconds axis, and gives each flat trace its own lane in list order; disconnected, a dim message and a Connect button | Manual smoke, 2026-10-21 (`55373f4` plus the reordered series palette): all eight mock rows fit and each swatch matches its trace; under `obd2` through the x64 fake, ten rows with the scroll bar pinned on, and MAF drawn dashed in row 1's colour. Re-run later on 2026-10-21 (working tree on `d1c20c0`): `745 RPM`, `49 °C`, `0.0 %`, `7.0 g/s` under mock, with the three flat mock traces in separate lanes; under the fake, all ten flat traces in their own lanes, top down in list order, Battery Voltage at `13.87 V`, and the two dashed rows' labels showing the dash. No automated coverage: the swatches, palette, dashes and chart labels are QML only; `BridgeTest` pins the `row` and `decimals` they key on |
| Settings activity-log lines carry local `HH:MM:SS.mmm` stamps; after Disconnect and a new connect, the Log page holds only the new session's frames | Manual run, 2026-10-22 (working tree on `c029caa`, `mock://demo`, the run-qttune driver) |
| App chrome: the header holds the name, a link pill (state and URI, eliding first) and one button (Connect… opens Settings; Disconnect); the sidebar is one full-height panel; below 720 px the tab bar underlines the selected tab; Settings sizes the device list to its rows and gives the activity log the rest; Log shows `s.mmm` times, spaced uppercase payloads and a full 29-bit ID, and Pause holds the table while the count keeps rising | Manual smoke, 2026-10-21 (working tree on `d1c20c0`, `mock://demo` and the x64 fake, the run-qttune driver), at 1280 and 400 px. Not checked: keyboard focus rings (the driver sends no keys) and an OS light colour scheme |
| QML reaches the bridge as the `Bridge` singleton (`app/bridgesingleton.h`), not a context property: every page binds through it, and qmllint resolves its properties | Manual smoke, 2026-10-21: `mock://demo` streaming on Home, Dashboard, Live and Log, and the x86 fake through the app's own deployed surrogate (2000 RPM decoded, the spawn line naming the exe). `all_qmllint` went from 103 `[unqualified]` warnings to 7, all on LogPage's delegate roles |
| Link-health UI: quiet/fault/degraded banner, activity-log transitions, "—" cells, stale dimming, the arbId column | Manual smoke, 2026-10-20 (fake quiet/fault via `ECUIO_FAKE_READ_STATUS`; mock; recipe below). The first run recorded dimming and the arbId column as working when the code couldn't produce either (dimming keyed on value changes; the ID column bound a misspelled role). Both rows were re-run and passed after the fixes, the arbId row through the x86 surrogate. |
| Fault state on real hardware, clean teardown; 4-byte filter shape accepted with logged vendor status 0 | Informal bench observation 2026-10-20 ([sheet](bench-sessions/2026-10-20.md)), under the rules before `946bdf5`. The same no-ACK setup now latches `ECUIO_ERR_REQUEST_FAILED` instead, which hasn't been seen on hardware. |

## Manual smoke — link-health UI

The QML half of #11 has no automated coverage (nothing drives the
bridge from QML). Recipe — run the app from a shell so the session
inherits the knobs at spawn (works for both in-proc and surrogate
routing; the Win32 child snapshots the environment, so toggling
mid-session only works in-proc):

| Goal | Environment | URI | Expected |
|---|---|---|---|
| Quiet banner | `ECUIO_FAKE_READ_STATUS=9` | `j2534://<fake.dll>?profile=raw&filter=0x7E8` | Amber "Vehicle not responding" after ~1.2 s; no frames, empty signal list (raw profile) |
| Fault banner | `ECUIO_FAKE_READ_STATUS=7` | same | Red "Adapter fault" banner with episode count; clearing the knob mid-session recovers only in-proc |
| Requests banner | `ECUIO_FAKE_WRITE_STATUS=7` | `j2534://<fake.dll>` (bare: the `obd2` scheduler writes every cycle) | At the first failed write: amber "Requests failing — check the vehicle connection and ignition" on LiveView, "Requests failing" with an amber dot on Home and the Dashboard header, an amber Settings label, and "Link: requests failing (1 episode(s))" in the activity log. **Verified 2026-10-21** (`55373f4` plus the reordered series palette, x64 fake in-proc): every surface as listed; values stay "—" |
| Frames + arbId column | — | `j2534://<fake.dll>?profile=raw&filter=0x7E8&poll=0x0C` | Frames on LogPage with `0x7E8` in the ID column, DLC 4, payload `41 0C 1F 40` (2000 RPM canned); link ok, no banner. **Verified 2026-10-20** after the role-name fix (`1674f5c`), using the Win32-tree fake (`build-win32/tests/Release/fake_j2534.dll`). The session log confirmed it ran through the x86 surrogate, so this row also covers surrogate routing, the strip/stamp path and poll mode in the app. It doesn't exercise `?poll=` outranking a profile's rotation: `raw` has no scheduler. |
| Dimming baseline | — | `mock://demo` | Values stream at full opacity, steady ones (IAT) included, because every mock signal arrives every 50 ms. Only a signal that stops arriving dims, 3 s after its last sample. **Verified 2026-10-20** after dimming moved to the model's `ageMs` (`7e9bd5e`): nothing dims on mock. The first run had expected IAT to dim, which was the bug itself. |
| Disconnect reset | after any of the above | Disconnect | Home dot and Settings label show plain disconnected, not the last session's fault/quiet |
| Slow teardown | `ECUIO_FAKE_DISCONNECT_DELAY_MS=6000` | `j2534://<fake.dll>`, then Disconnect | For the 6 s: "Disconnecting…" on the header pill, the Settings label and button, LiveView's status line and the header button; Connect and the presets disabled; `Disconnecting:` in the activity log. Then "No session", Connect enabled, `Disconnected:` in the log, and a reconnect works. **Verified 2026-10-21** with the x64 fake in-proc, and through the x86 surrogate (`-x86` fake, a 20000 ms stall): the session log marked the child unresponsive, `Disconnected:` followed about 6 s after `Disconnecting:`, and the window answered UI Automation throughout |

All four states (ok/quiet/fault/disconnected) were verified 2026-10-20,
plus the fault state on the OBDX Pro VX quiet bus (BENCH,
[bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)). The
first run's dimming and arbId results couldn't have been real. Both
rows were re-run after the fixes the same day and passed. The
requests-banner row, which covers the `requests` state, passed on
2026-10-21, along with a re-run of the quiet, fault and disconnect
rows against the current Home, Settings and Dashboard header.

## Gaps (not verified)

- **The surrogate pipe's protections have no direct test.** Its
  current-user DACL, `FILE_FLAG_FIRST_PIPE_INSTANCE`,
  `PIPE_REJECT_REMOTE_CLIENTS` and the one-handle inheritance list are
  exercised by every surrogate test (they'd fail to connect if the
  child were shut out) and by the app's x86 path in a manual smoke
  (2026-10-21), but nothing tries to connect as another user, squat
  the name, or look for a leaked handle in the child.
- **Discovery's UTF-16 registry read is untested.** The non-ASCII
  tests start from a path; reading one from `FUNCTIONLIBRARY` would
  need an HKLM entry, which needs elevation.
- **GCC and Clang warnings are seen only in CI logs.** Every target
  builds with warnings on, and the local MSVC count is 0, but no GCC
  or Clang build runs on the dev machine and warnings don't fail CI.

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
  `ECUIO_FAKE_READ_STATUS`, `ECUIO_FAKE_UNTOUCHED_COUNT_STATUS` and
  `ECUIO_FAKE_WRITE_STATUS` through the worker. Not covered:
  - the surrogate path's health mapping through the worker, end to
    end (the surrogate test stops at `read_frame`'s status). Accepted
    until the first vehicle session, which drives it live through the
    surrogate, with the session log as the record;
  - how the UI renders `ECUIO_ERR_REQUEST_FAILED`: the bridge's mapping
    to `requests` is unit-tested, but the QML banner, Home card,
    Dashboard header, Settings label and log line are covered only by
    the smoke recipe's requests-banner row (passed 2026-10-21);
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
  to that. `ECUIO_FAKE_SPEC_READ_SEMANTICS=1` switches to J2534-1
  behavior: a short batch comes back with `ERR_TIMEOUT`, and an empty
  timeout-0 read with `ERR_BUFFER_EMPTY`. Spec mode still returns at
  once instead of waiting out the timeout, and it skips filter gating.
  The worker never reads with timeout 0, so the `ERR_BUFFER_EMPTY`
  mapping is reachable only through `ECUIO_FAKE_READ_STATUS=0x10`,
  and no test sets it: the mapping is untested on both backends.
  A vendor that leaves its outputs untouched on error is modelled
  (`ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`).
- **ID 0 handling is untested.** The child tracks the open device and
  channel with flags instead of treating ID 0 as "none", but the fake
  always returns device 1 and channel 0x55.
- **The fake emits no ISO 15765 indications.** TX-done and
  first-frame indications never appear, so indication handling (#28)
  is untested.
- **Child survival after a refused op isn't asserted.** The surrogate
  child must stay alive after a vendor refusal (ADR-0014). Tests check
  the refusal's status, but `sur_close` returns `ECUIO_OK` whether or not
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
  hooks, `ecuio_j2534_inject_test_frame`) are never called by app
  code.
- **When you change shared plumbing, pin the contract you changed, at
  the point you changed it.** For example,
  `DefaultConnectUsesIso15765At500k` was added alongside the query
  parameters.
