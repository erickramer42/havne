# Testing

What the test suites cover, how much confidence each tier gives, and
where the gaps are. For build and run commands, see
[development.md](development.md#running-tests).

## Verification tiers

| Tier | Meaning | Where it runs |
|---|---|---|
| **Unit** | Core, signal and ABI logic with no I/O | CI on all three desktop platforms |
| **Bridge** | Qt bridge and models, headless (`Qt6::Core` only) against the real 50 ms mock worker | CI on Windows, macOS and Linux, and locally |
| **QML** | The app's QML module against a real bridge (Qt Quick Test, `test_qml`), offscreen on the software renderer. Pages are built and bound as in the app, and checked through their properties (colour, text, visibility), not their pixels | CI on Windows, macOS and Linux, and locally. The link states past ok need the fake vendor DLL, so they run on Windows only |
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL | CI on Windows (same-arch, all x64) |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Fuzzing** | Two libFuzzer harnesses, `fuzz_decode_signal` and `fuzz_validate_set`, feed arbitrary defs, payloads and sets to the decoder and validators under ASan (and UBSan with Clang) and check their contracts on every input. Not ctest cases: a time-bounded fuzzer would make a green suite stop meaning green ([fuzz/README.md](../fuzz/README.md)) | CI `fuzz` job (Linux, Clang), 60 s per harness, crashing inputs uploaded; locally with MSVC's libFuzzer in a tree of its own |
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on pull requests, and on request: a manual run (Actions → CI → Run workflow; `smoke_only` skips the other platform jobs), or `[ios-smoke]` in the commit message (`ios-simulator-smoke`, Intel macOS runner) |
| **Manual smoke** | Running the app: layout, Dashboard gauges, LiveView, strip chart, Settings picker against the live registry | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); informal quiet-bus follow-up 2026-10-20 ([note](bench-sessions/2026-10-20.md)); quiet-bus run of the vehicle session's URIs with recording, 2026-10-22 ([sheet](bench-sessions/2026-10-22.md)) |
| **Vehicle** | A live, read-only vehicle session | First session 2026-10-22: one car, its engine ECU on `0x7E8`, ignition off, engine off and idling ([session sheet](bench-sessions/2026-10-22-vehicle.md)); run sheet [vehicle-runsheet.md](bench-sessions/vehicle-runsheet.md) |

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
| `test_qml` | One ctest case per `tests/qml/tst_*.qml` file, `QmlTest.<name>`: `theme`, `liveview`, `dashboard`, `linkhealth` | Qt Quick. Links the app's QML module (`qttune_ui`). The quiet, fault and requests cases need the fake DLL (Windows) and skip elsewhere. |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_health` | `J2534HealthTest`, every case run twice: `/InProc` and `/Surrogate` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_connect` | `J2534ConnectTest` (each case `/InProc` and `/Surrogate`), `J2534SurrogateConnectTest` | Windows, the fake DLL and the surrogate exe |
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
| `ecuio_init` and `ecuio_shutdown` are optional: a session needs neither, and one open when shutdown runs keeps delivering | `CoreTest.InitShutdownIdempotent`, `.ShutdownLeavesAnOpenSessionWorking` |
| Signal decode: bit extraction, sign extension, error-code order (the kind and byte order before the bit parameters), the validator; results in `double`, exact for every 32-bit raw value; 16-bit payload lengths and start bytes past byte 255; binary32 floats and signed integers in both byte orders | `SignalDecodeTest` (including `.Uint32DecodesExactly`, `.PayloadLengthPastByte255IsNotNarrowed`, `.StartBytePastByte255Reaches`, `.FloatDecodesInBothByteOrders`, `.BigEndianIntSignExtends`), `SignalsTest` |
| The validator accepts exactly what the decoder decodes: `TEXT`, unknown kinds and unknown byte orders are refused; a `FLOAT` must be 32 bits at bit 0, in either byte order; big-endian fields must be whole bytes; every reserved byte and `flags` must be zero; a set's layout version is checked before anything else, and its reserved fields must be zero; a set can't repeat a def ID; an empty set (no defs, maybe channels) is valid | `SignalsTest.ValidatorAcceptsExactlyWhatTheDecoderDecodes`, `.ValidatorRejectsKindsWithNoDecoder`, `.ValidatorRejectsUnknownByteOrder`, `.ValidatorRequiresFloatAs32BitsAtBitZero`, `.ValidatorRequiresByteAlignedBigEndian`, `.ValidatorRefusesEveryNonzeroReservedByte`, `.SetValidationChecksLayoutVersionFirst`, `.SetValidationRefusesNonzeroReservedFields`, `.SetValidationRejectsDuplicateIds`, `.SetValidationAcceptsAnEmptySet` |
| The def's layout: 120 bytes with no padding, every field's offset, and `ECUIO_SIGNAL_SET_LAYOUT`; the size and key offsets again in C, with `ecuio_validation_t`'s | `SignalsTest.SignalDefLayoutIsPinned`, `c_abi`; both also built and run in the Win32 tree by hand, 2026-10-22, since no suite runs them on x86 |
| Validation names the failing element: the def or channel at fault, the second of a duplicate pair, and `UINT32_MAX` for a pass or a code about the whole set. Registration hands the result back, with code 0 when it refused on the name or the set is NULL | `SignalsTest.SetValidationNamesTheFailingElement`, `CoreTest.ProfileRegisterReportsTheValidatorResult` |
| On arbitrary defs and payloads, the decoder returns only its defined codes, gives the same answer twice (NaN bits included), writes nothing unless it succeeds, and reads nothing past `payload_len`; a def the validator accepts fails only on the payload's bounds or its mux key. On arbitrary sets, the set validator returns only defined codes, and its `ecuio_validation_t` matches the return and names the element its code names | `fuzz_decode_signal`, `fuzz_validate_set` (CI `fuzz` job; green on PR #17's run, `ce8284f`) |
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
| Session recording ([ADR-0017](adr/0017-session-recording.md)): off by default; with it on, one folder per connect holding every accepted frame (paused or not, host time never going backwards), each decoded value and the activity lines from `Connected:` through `Disconnected:`; the switch holds during a session; recording sets `ECUIO_SUR_LOG_FRAMES` in the OS environment the surrogate inherits and removes it at the next unrecorded connect, leaving a launch value alone | `BridgeTest.RecordingIsOffByDefault`, `.RecordingWritesFramesValuesAndEvents`, `.RecordingSwitchHoldsDuringASession`, `.RecordingTurnsOnSurrogateFrameLogging` |
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
| A child that exits during `OP_OPEN` (failed DLL load) is reported by its exit code, not as a blocked vendor | Session logs read by hand, 2026-10-20 (`DeadDllFailsFastAtOpen`, `BlockedVendorOpenFailsAtHandshakeTimeout`) |
| A missing surrogate exe is reported before any spawn, as "no surrogate (ecuio-j2534-surrogate.exe)" and why: `ECUIO_SURROGATE_EXE` naming no file, or none next to the app | `J2534SurrogateConnectTest.MissingSurrogateExeIsASurrogateFailure` (the variable). None next to the app: in the app, 2026-10-22, with the deployed copy moved aside (before the check, it read "couldn't start the surrogate: CreateProcess (Windows error 2)"). Every test binary has a surrogate beside it, so no test covers that case |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), both backends | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path) plus the whole existing suite against the legacy-spelling fake (fallback path) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally, since the child didn't yet log filter ops. The 4-byte shape that replaced it was accepted on 2026-10-20 (row below). Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Single-threaded surrogate child ([ADR-0014](adr/0014-single-threaded-surrogate.md)): the whole surrogate and routing suite on the redesigned child, same-arch and cross-arch (x64 test → x86 surrogate → x86 fake) | `J2534SurrogateTest`, `J2534RoutingTest` (2026-10-19, both trees rebuilt: full suite 136/136, cross-arch subset 15/15) |
| A transaction that times out doesn't put later replies out of step | `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest` |
| An `OP_READ` answered past its deadline: the next transaction drains the late reply and drops its frame, logged with the count ("drain_pending: dropped 1 frame(s)"), and the next read gets its own reply's frame | `J2534SurrogateTest.LateReadReplyIsDroppedAndLogged` (`ECUIO_FAKE_READ_DELAY_MS`). With the log line removed (checked by hand, 2026-10-23), it failed |
| Device and channel ID 0 are real IDs in the child: with a fake that hands out 0 for both, a frame reads once connected, and a close with no disconnect first still disconnects channel 0 and closes device 0 | `J2534SurrogateTest.DeviceAndChannelIdZeroAreRealIds` (`ECUIO_FAKE_ZERO_IDS`). With the child's flags replaced by `ID != 0` (checked by hand, 2026-10-23): in the read gate, no frame arrived; at shutdown, neither call was made |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins; registration refuses an invalid set or a malformed name, and reports the validator's result; the built-ins can't be replaced or unregistered; unregister is idempotent and leaves existing sessions on their set | `CoreTest.Profile*` |
| `mock://` shares the query grammar: only `profile`, and every malformed form (a bare `?`, an empty segment, a missing `=`, an uppercase key) fails at create | `CoreTest.MockUriQueryIsStrict` |
| Profile selection: `mock://`→mock set, `j2534://`→obd2, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |
| `obd2` signal set: mux grammar, decoder round-trips against canned wire bytes, validator including channel-decl rejection (duplicates, zero/illegal IDs, over-capacity) | `Obd2SetTest`, `Obd2DecodeTest` |
| Mode-01 scheduler: rotation order in set order, wrap; drain delivers a backlog beyond one cycle's budget | `J2534AdoptionTest` |
| One Mode-01 key test for enrolment and request routing: only a 2-byte `41 <PID>` key at offset 0 enrols a PID or routes its request. A `62 <DID>` key ending in the same byte, another service's 2-byte echo, a key at offset 1 or a 1-byte key does neither; a `62 <DID>` def on a second ECU, listed first, doesn't draw the PID's requests to that ECU | `PidSchedulerTest.Mode01KeyIsTwoBytes41AtOffsetZero`, `.EnrollsOnlyMode01Keys`, `J2534AdoptionTest.Mode01RequestRoutesPastOtherServiceKeys`. With routing that matches any key's last byte, the routing case fails, sending the requests to 0x7E1 (checked by hand, 2026-10-22) |
| Channel derivation: a bare `j2534://` URI under `obd2` registers the 7E8 filter with exact mask/pattern shape, sends to 7E0, and starts; a lone `?txid=` keeps the derived 7E8 filter but rewrites the request target | `J2534AdoptionTest.BareJ2534UriDerivesObd2ChannelAndStarts`, `.TxidOverrideRewritesDerivedTransmitTarget` |
| The link-health cases run twice, once in-proc and once through the surrogate (the x64 surrogate with the loader's routing forced; the x86 one in the cross-arch run), driven through the fake's control page; each checks which process opened the fake | `J2534HealthTest`, every case `/InProc` and `/Surrogate`; the surrogate half also cross-arch (2026-10-22) |
| Link health: a hard read failure records `ECUIO_ERR_TRANSPORT_HARD_FAILURE` with one episode per outage and recovers through the probe write; a quiet bus debounces to `ECUIO_ERR_VEHICLE_TIMEOUT` with no episode | `J2534HealthTest.HardFailureSetsCanonicalErrorAndRecovers`, `.QuietBusDebouncesToVehicleTimeoutWithoutEpisode` |
| Write failures: a failed write latches `ECUIO_ERR_REQUEST_FAILED` with one episode; empty reads past the quiet debounce don't move it; a successful write alone clears it, with reads held quiet | `J2534HealthTest.WriteFailureLatchesRequestFailedAndClears` (`ECUIO_FAKE_WRITE_STATUS`) |
| A write the vendor answers `STATUS_NOERROR` with no message written is a failed write: one "requests failing" episode, held past the quiet debounce, cleared by the first write sent | `J2534HealthTest.ZeroWrittenCountIsAFailedWrite` (`ECUIO_FAKE_WRITE_ZERO_COUNT`). With the count ignored (checked by hand, 2026-10-23), both backends failed: the session went quiet instead |
| A hard read failure outranks a failing write: exactly two episodes (write, then read), and continued failing writes don't downgrade the state | `J2534HealthTest.HardReadFailureOutranksRequestFailed` |
| A delivered frame that matches no channel ends a hard failure but reads as `ECUIO_ERR_VEHICLE_TIMEOUT`, not `ECUIO_OK`, with no episode, inside the quiet debounce window | `J2534HealthTest.UnmatchedDeliveredFrameGoesQuietNotHealthy` |
| The backoff grows under a sustained hard failure: at most 8 probe writes in 1.5 s, against ~15 with the old reset-to-base | `J2534HealthTest.BackoffGrowsUnderSustainedHardFailure` |
| A hard failure that ends on a silent bus reads as quiet within 5 s, with no new episode, and the probe returns to at least 6 writes a second; a few clean reads inside a hard failure end the backoff, and it starts again when the hard reads resume (at most 8 probes in 1.5 s), still one episode | `J2534HealthTest.HardFailureEndingInSilenceGoesQuietAtFullCadence`, `.BackoffRestartsWhenHardReadsResume`. Before the backoff cleared on a quiet read (checked by hand, 2026-10-23), the first failed on both backends: quiet took 26.4 s. Without the restart, the second saw 14 probes |
| Against the fake's VX mode, on both backends: empty reads that hold their 100 ms and return `ERR_BUFFER_EMPTY` take a passive session quiet in no less than 950 ms (1.42 s measured, as on the bench), with no episode; answered requests keep the link ok with every write's indication dropped; on a no-ACK bus, three writes are accepted, the fourth fails into one "requests failing" episode, no later write is accepted, and close returns within 2 s of a 500 ms blocked write | `J2534HealthTest.VxQuietBusDebouncesOnBufferEmptyReads`, `.VxAnsweredRequestsKeepTheLinkOk`, `.VxNoAckBusAcceptsThreeWritesThenRequestsFail`. With the fake's read hold removed, the quiet case reached quiet in 275–315 ms and failed (checked by hand, 2026-10-22) |
| Indications are skipped without ending a read, on both backends: on the VX each reply arrives in its own request's cycle, before the next write; a TX indication and a `START_OF_MESSAGE` indication queued ahead of a reply leave exactly one frame, the reply's | `J2534HealthTest.VxReplyArrivesInItsOwnRequestsCycle`, `.IndicationsAheadOfAReplyAreSkipped`. Against the read paths without the skip (checked by hand, 2026-10-22): in-proc, all 10 replies landed after the next request; on both backends the first-frame indication arrived as a second, empty frame |
| J2534 frame time: the first frame takes the host's time since attach, not the vendor's stamp; later frames the vendor's deltas, across the 32-bit wrap and across a gap of several wraps, which the host's clock counts; a vendor clock that steps back or far ahead falls back to the host's delta and never decreases | `J2534StreamTest.VendorClockAnchorsAtTheHostAndKeepsVendorDeltas`, `.VendorClockCarriesAcrossTheWrap`, `.VendorClockThatStepsFallsBackToTheHost` (session-free); `J2534HealthTest.VendorStampsBecomeSessionTime` (`/InProc`, `/Surrogate`, the surrogate half also cross-arch) and `J2534StreamTest.DirectLoadUriDeliversInjectedFramesExactly` through the worker. With the vendor's stamp passed through (checked by hand, 2026-10-22), all three worker cases failed. In the app under the fake's VX mode, 2026-10-22: Log times from 0 s in about 109 ms steps, against raw stamps near 2679 s |
| The backoff step (`j2534_backoff_next`, shared by all three worker modes) doubles from the base, never exceeds the 5 s cap, including the step that lands on it, holds at the cap, and doesn't wrap | `J2534StreamTest.BackoffStepDoublesToCapWithoutOvershoot` (session-free) |
| `ECUIO_ERR_REQUEST_FAILED` has its own status string | `J2534StreamTest.RequestFailedStatusHasDistinctString` |
| A failed connect says why ([ADR-0018](adr/0018-connect-failure-reporting.md)): a code per cause, and `ecuio_last_error_detail` in words, cleared by a success and kept per thread; a bad URI key, a bad value and an unknown profile each name themselves | `CoreTest.CreateFailuresNameTheirCause`, `.LastErrorDetailIsPerThread`, `.StatusStringValidValues`; the j2534 rejection tests (`J2534QueryParamTest.MalformedQueriesRejectedAtSessionCreate` and others) for the codes |
| A refused `PassThruOpen`, `PassThruConnect` or filter names the call, its status (by its J2534-1 name, or as vendor-specific, as for the VX's 34) and the vendor's `PassThruGetLastError` text, word for word alike on both backends; a missing DLL is `ECUIO_ERR_LOAD_FAILED` on both; the surrogate's own failures (no exe, other shared headers, a `PassThruOpen` that never returns, a DLL the child can't load) have their codes and words | `J2534ConnectTest` (`/InProc`, `/Surrogate`), `J2534SurrogateConnectTest`, `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout`, `.DeadDllFailsFastAtOpen`, `.MismatchedFingerprintRefusesOpenBeforeVendor` |
| Below the connect path, the loader seam's helpers give each cause its own code, alike on both backends: a NULL pointer `ECUIO_ERR_NULL_ARGUMENT`; a bad write length, ioctl length, SCONFIG array or filter shape `ECUIO_ERR_INVALID_ARGUMENT`, with no write reaching the fake; an ioctl outside the whitelist `ECUIO_ERR_NOT_IMPLEMENTED`; a refused write, `READ_VBATT`, `PassThruReadVersion` or second filter stop `ECUIO_ERR_VENDOR_REFUSED`; a surrogate that stops answering `ECUIO_ERR_SURROGATE_FAILED` | `J2534ConnectTest.HelperFailuresHaveOneCodePerCause` (`/InProc`, `/Surrogate`); `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest`, `.UnresponsiveChildFailsFastAfterTwoMissedDeadlines`. Against the old codes (checked by hand, 2026-10-23) it failed on both, and showed the backends disagreeing: a malformed SCONFIG array was a NULL argument in-proc and a transport failure through the surrogate |
| A connected session describes what it runs on ([ADR-0019](adr/0019-session-info.md)): backend, DLL architecture, protocol, baud and connect flags, channels, profile, and the adapter's battery and version readings, alike on both backends; a refused reading leaves its flag clear without failing the connect or setting a detail; a DLL without `PassThruReadVersion` reports no versions; a mock session names its backend and profile; the struct's layout is the same in both trees | `J2534ConnectTest.InfoDescribesTheAdapter`, `.InfoCarriesTheUrisBaudAndChannel`, `.RefusedReadingsLeaveTheirFlagsClear`, `.ADllWithoutReadVersionReportsNoVersions` (`/InProc`, `/Surrogate`; the surrogate half also cross-arch, 2026-10-22), `J2534SurrogateTest.ReadVersionRelaysTheThreeStrings`, `CoreTest.MockSessionInfoNamesTheBackend`; `c_abi` in both trees |
| The activity log's "Session:" line follows "Connected:", with every field's wording, and "no battery reading" or "no version reading" for a refused one | `BridgeTest.SessionInfoTextNamesTheAdapter`, `.ConnectLogsTheMockSession`, `.ConnectLogsTheAdaptersSession` (the fake in-proc); in the app, 2026-10-22 (the run-qttune driver), through the x64 fake in-proc and the x86 fake through the deployed surrogate |
| An ioctl result longer than the caller's buffer fills it and no more, and reports its whole length, on both backends | `FakeFixture.IoctlCopiesAtMostItsCapacity`, `J2534SurrogateTest.IoctlCopiesAtMostItsCapacity`. Against the old copies (checked by hand, 2026-10-22): in-proc left a 2-byte buffer untouched, and the surrogate client wrote all 4 bytes |
| The bridge returns and logs a failed connect as "<status>: <detail>", and Settings shows it in its error line | `BridgeTest.FailedConnectNamesItsCause`; in the app, 2026-10-22 (the run-qttune driver): `mock://demo?interval=10`, and the x86 fake through the app's deployed surrogate with `ECUIO_FAKE_OPEN_STATUS=34`, each in the error line and the activity log |
| A batch the vendor returns with `ERR_TIMEOUT` is delivered through the surrogate; an empty one maps to a vehicle timeout; both reads hold for their 50 ms timeout | `J2534SurrogateTest.SpecTimeoutReadDeliversPartialBatch` (spec-mode fake) |
| A vendor that returns a hard error with the message count untouched delivers no frames: on both backends the session reports a hard failure, dispatches nothing and recovers once reads are honest; through the surrogate, the next request after the poisoned read still correlates | `J2534HealthTest.UntouchedCountUnderHardErrorReportsUnhealthy`, `J2534SurrogateTest.HardErrorUntouchedCountDeliversNoFrames` (`ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`) |
| Signal freshness: `ageMs` is -1 before the first sample and a sane clock delta after; `updateValues` notifies `AgeMsRole`; `refreshAges()` notifies `AgeMsRole` alone, over every row, so a stalled row re-evaluates | `BridgeTest.AgeMsRoleEmittedAndRefreshesWhileStalled` |
| Link-state mapping: `ECUIO_OK` → `ok`, vehicle timeout → `quiet`, `ECUIO_ERR_REQUEST_FAILED` → `requests`, hard failure → `fault`, any other code → `degraded` | `BridgeTest.LinkStateMapsEveryPublishedCode` (session-free, through `QtTuneBridge::linkStateForError`) |
| Theme's link-state colour and label for every state the bridge publishes, with an unknown state shown as degraded; the chart trace colours are distinct, and none is a saturated hue within 20° of the danger colour | `QmlTest.theme` |
| Live's trace styles are keyed by model row: colours from `Theme.seriesColors`, solid within the palette, and past it no two rows that share a colour share a dash. Under `mock://demo` each row's swatch shows its row's trace colour, filled while plotted and hollow while not, the other rows unchanged. The banner's message for each link state, an unknown one as degraded | `QmlTest.liveview` |
| `SignalHub` finds a signal by the first of a `Tap`'s names the set provides, else null, mirrors the model's values, and follows a profile switch. Each Dashboard gauge reads its signal by the name the mock and `obd2` sets share. It says "no session", then "waiting for data" until the first 100 ms batch, then nothing, with the value at the signal's decimal places in its unit; after a switch to `?profile=raw`, "not in profile" | `QmlTest.dashboard` |
| Every link-health surface in the app's window (`Main`), in colour and words: the header pill, Home's status card, the Dashboard header, Live's banner, Settings' status label and the activity log's link line. Disconnected and ok (`mock://demo`) everywhere; quiet, fault and requests through the fake in-proc with its knob armed; a disconnect after a fault shows plain disconnected again | `QmlTest.linkhealth` (the fake's cases on Windows) |
| A binding error in the app's QML fails the page tests: they treat any warning from `qrc:/qt/qml/QtTune/` as a failure. That catches a removed delegate's binding, such as a Live row's swatch reading the palette at index -1 while a connect resets the model | `QmlTest.liveview`, `.dashboard`, `.linkhealth` (`failOnWarning`) |
| Dashboard: every gauge the mock set feeds sweeps under `mock://demo`; a gauge whose signal the profile lacks reads "not in profile" (the mock set feeds every gauge, so `?profile=raw` is what shows it); the grid collapses from 4 to 2 columns across 760 px and tightens its gaps at 600 px; fault and quiet states tint correctly; the temperature rings start at 0 °C, so a cold engine reads on the scale; unticking a row on Live leaves its gauge live; when the stacked arrangement would overflow a short, wide page it switches to the wide one (dials and a 2 × 2 tile block in one row, sized to the height), and the scroll bar shows while the page overflows | Manual smoke, 2026-10-20 (`40e51ce`; quiet and fault through the knob recipe below). The header, which takes its dot and label from `Theme`, was re-run 2026-10-21 (`55373f4` plus the reordered series palette): ok ("Live · mock://demo", green), quiet, requests, fault and disconnected, plus "not in profile" on every gauge under `?profile=raw`. Re-run later on 2026-10-21 for the rings and the untick (working tree on `d1c20c0`): coolant 53 °C and oil 24 °C on 0–130 and 0–160 scales under mock; with RPM unticked the tach kept moving and recorded a 7,000 peak; every gauge found its signal under `obd2` through the x64 fake. Re-run 2026-10-22 for the arrangements (`0a94049`, the run-qttune driver's `resize`): the wide one at 1280 × 650 (a 1080p laptop at 150 %) and 1280 × 800 with every gauge on screen, stacked at 1700 × 980 as before, narrow at 860 × 640 and 600 × 700 with the scroll bar shown. `SignalHub`'s lookup and each gauge's note are `QmlTest.dashboard`'s too (above); the gauges' drawing and the arrangements are manual only |
| Live: the list sizes to its rows up to a cap, then scrolls with the bar pinned on; each row's swatch matches its trace; rows past `Theme.seriesColors` reuse its colours with a dashed trace (under `obd2`, the last two rows); values at each def's decimal places; the chart prints each trace's value at its right-hand end, nudged apart, with a sample of its stroke, has a seconds axis, and gives each flat trace its own lane in list order; disconnected, a dim message and a Connect button | Manual smoke, 2026-10-21 (`55373f4` plus the reordered series palette): all eight mock rows fit and each swatch matches its trace; under `obd2` through the x64 fake, ten rows with the scroll bar pinned on, and MAF drawn dashed in row 1's colour. Re-run later on 2026-10-21 (working tree on `d1c20c0`): `745 RPM`, `49 °C`, `0.0 %`, `7.0 g/s` under mock, with the three flat mock traces in separate lanes; under the fake, all ten flat traces in their own lanes, top down in list order, Battery Voltage at `13.87 V`, and the two dashed rows' labels showing the dash. The swatches, row colours and dashes are `QmlTest.liveview`'s too (above). The chart's drawing (traces, labels, lanes, the time axis) is manual only; `BridgeTest` pins the `row` and `decimals` it keys on |
| Settings activity-log lines carry local `HH:MM:SS.mmm` stamps; after Disconnect and a new connect, the Log page holds only the new session's frames | Manual run, 2026-10-22 (working tree on `c029caa`, `mock://demo`, the run-qttune driver) |
| Recording end to end through the x86 surrogate: Settings shows "Recording to Documents\QtTune\recordings\…" with no account name, and the switch is disabled while connected; the folder held 55 frames and 55 values from the fake; that spawn's session log read "frame logging on" and carried its frame lines | Manual run, 2026-10-22 (working tree on `839b176`, the x86 fake through the app's deployed surrogate, the run-qttune driver; the test recording deleted afterwards). On the OBDX Pro VX, 2026-10-22 (`0a8beae`): one recording per connect across six connects, and frame logging on in each spawn's session log ([sheet](bench-sessions/2026-10-22.md)) |
| App chrome: the header holds the name, a link pill (state and URI, eliding first) and one button (Connect… opens Settings; Disconnect); the sidebar is one full-height panel; below 720 px the tab bar underlines the selected tab; Settings sizes the device list to its rows and gives the activity log the rest, and on a short window scrolls, the log keeping a 200 px minimum; Log shows `s.mmm` times, spaced uppercase payloads and a full 29-bit ID, and Pause holds the table while the count keeps rising | Manual smoke, 2026-10-21 (working tree on `d1c20c0`, `mock://demo` and the x64 fake, the run-qttune driver), at 1280 and 400 px; Settings re-run 2026-10-22 at 1280 × 650 (`0a94049`). Not checked: keyboard focus rings (the driver sends no keys) and an OS light colour scheme |
| Zoom: Settings' Display row steps `Theme.zoom` from 50 % to 200 %; the whole UI reflows at the zoomed size, a badge shows the level, and the zoom survives a restart | Manual smoke, 2026-10-22 (`0a94049`, the run-qttune driver): two steps down to 80 % reflowed the Dashboard at the larger layout size, its tiles showing their descriptions, a relaunch came back at 80 % (`HKCU\Software\QtTune\QtTune\ui`), and Reset showed "Zoom 100 %". Not checked by the driver: the Ctrl + / Ctrl − / Ctrl 0 shortcuts (it sends no keys) and the window's opening size on a 1280 × 720 screen |
| QML reaches the bridge as the `Bridge` singleton (`app/bridgesingleton.h`), not a context property: every page binds through it, and qmllint resolves its properties | Manual smoke, 2026-10-21: `mock://demo` streaming on Home, Dashboard, Live and Log, and the x86 fake through the app's own deployed surrogate (2000 RPM decoded, the spawn line naming the exe). `all_qmllint` went from 103 `[unqualified]` warnings to 7, all on LogPage's delegate roles |
| Link-health UI: quiet/fault/degraded banner, activity-log transitions, "—" cells, stale dimming, the arbId column | The banner, the other surfaces and the log lines are `QmlTest.linkhealth`'s, in-proc (above); Live's "—" cells, stale dimming and the arbId column are manual only. Manual smoke, 2026-10-20 (fake quiet/fault via `ECUIO_FAKE_READ_STATUS`; mock; recipe below). The first run recorded dimming and the arbId column as working when the code couldn't produce either (dimming keyed on value changes; the ID column bound a misspelled role). Both rows were re-run and passed after the fixes, the arbId row through the x86 surrogate. |
| Fault state on real hardware, clean teardown; 4-byte filter shape accepted with logged vendor status 0 | Informal bench observation 2026-10-20 ([sheet](bench-sessions/2026-10-20.md)), under the rules before `946bdf5`. The same no-ACK setup now latches `ECUIO_ERR_REQUEST_FAILED` instead (next row). |
| Requests-failing and quiet states on real hardware through the x86 surrogate: the banner at the first failed write, one episode, the backoff to its 5 s cap; quiet after ten empty reads (`ERR_BUFFER_EMPTY`) under `?profile=raw`; clean teardown on every connect | BENCH run on the OBDX Pro VX, 2026-10-22 (`0a8beae`, [sheet](bench-sessions/2026-10-22.md)) |
| The `obd2` set on a live vehicle through the x86 surrogate: the scheduler, `?poll=` and the three supported-PID bitmaps answered by the engine ECU; the values, decoded a second time from the logged bytes with the J1979 formulas, match the display, and RPM matched the tachometer; ok, quiet and recovery through an ignition cycle | Vehicle session, 2026-10-22 ([sheet](bench-sessions/2026-10-22-vehicle.md)) |

## Manual smoke — link-health UI

`QmlTest.linkhealth` runs the quiet, fault, requests and
disconnect-reset rows below on every surface, in-proc. The recipe
covers the rest: the surrogate path, the arbId column, dimming, a slow
teardown, and how it all looks. Recipe — run the app from a shell so the session
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
[bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)), and the
requests and quiet states on it on 2026-10-22
([bench-sessions/2026-10-22.md](bench-sessions/2026-10-22.md)). The
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

- **The in-proc backend has no real-hardware coverage.** The one
  J2534 adapter on hand, the OBDX Pro VX, ships an x86 DLL, so real
  vendor code has run only through the surrogate. In-proc, every
  vendor call has met only the x64 fake.
- **Real-hardware coverage is one adapter and one vehicle.** The
  quiet-bus BENCH sessions (2026-10-16, 2026-10-20, 2026-10-22) and
  the first vehicle session (2026-10-22,
  [session sheet](bench-sessions/2026-10-22-vehicle.md)) all ran the
  OBDX Pro VX through the surrogate, and the vehicle's engine ECU
  answered on 11-bit `0x7E8`. Still unverified against real hardware:
  VBATT and `PassThruReadVersion`, 29-bit addressing, more than one ECU answering, multi-frame
  (segmented ISO-TP) replies, a vendor read failure, and any other
  vendor's statuses (the VX never returned `ERR_TIMEOUT`, #11).
- **Link health is covered against the fake.** The tests above drive
  `ECUIO_FAKE_READ_STATUS`, `ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`,
  `ECUIO_FAKE_WRITE_STATUS` and `ECUIO_FAKE_WRITE_ZERO_COUNT` through
  the worker on both backends. Not
  covered:
  - a hard read failure from real hardware. Live runs on 2026-10-22
    drove the other states through the surrogate on the VX: requests
    failing at the bench, and ok, quiet and recovery on the vehicle.
    No vendor read error has been seen yet; the session log is the
    record when one is;
  - other vendors' read and write statuses. The VX's are known: an
    empty read returns `ERR_BUFFER_EMPTY` and data comes with
    `NOERROR`, never `ERR_TIMEOUT`; with nothing acknowledging, three
    writes are accepted and the fourth fails `ERR_FAILED`, and on a
    vehicle every write succeeded
    ([bench](bench-sessions/2026-10-22.md),
    [vehicle](bench-sessions/2026-10-22-vehicle.md)).
- **The fake's default read model is simpler than J2534's.** By
  default its `ReadMsgs` returns one message per call whatever
  `NumMsgs` and the timeout ask for, and `STATUS_NOERROR` with zero
  messages on an empty queue. The exact-count stream tests are pinned
  to that. Two opt-in modes hold a read until its batch fills or its
  timeout passes: J2534-1's (`ECUIO_FAKE_SPEC_READ_SEMANTICS`) and the
  VX's (`ECUIO_FAKE_VX`), which is the one with `ERR_BUFFER_EMPTY` on
  a worker read. Neither models filter gating. A vendor that leaves
  its outputs untouched on error is modelled
  (`ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`).
- **The fake never segments a reply.** Every reply is one message,
  so a `START_OF_MESSAGE` indication appears only where a test queues
  one by hand (`IndicationsAheadOfAReplyAreSkipped`).
- **Child survival after a refused op isn't asserted.** The surrogate
  child must stay alive after a vendor refusal (ADR-0014). Tests check
  the refusal's status, but `sur_close` returns `ECUIO_OK` whether or not
  the child is still alive, so an early exit would go unnoticed.
- **The client's skew paths aren't exercised.** No test child sends a
  `num_frames` or ioctl `data_len` past the wire limit. Reaching
  either needs a misbehaving child, such as a fault-injection knob in
  the surrogate.
- **Vendor conformance to J2534-1 v04.04 is known for one vendor.**
  The shared types header is verified against public references, and
  the VX's replies decode, so its struct layout matches. It departs
  from the read rule, though: a short batch comes back with
  `STATUS_NOERROR`, not `ERR_TIMEOUT`. Another vendor can depart
  elsewhere, and only its own device can show it
  ([ADR-0010](adr/0010-j2534-v0404-shared-types.md)).
- **Discovery tests check shape only.** They pass vacuously on a
  machine with no vendors installed (Known Issue #14).
- **The mobile apps have never run on a device or emulator** (Known
  Issue #2). CI compiles both kits. The simulator smoke shows the iOS
  app starts and draws, but nothing taps through its UI, and it skips
  ordinary pushes. No tests run on either mobile
  platform, because cross builds set `BUILD_TESTING=OFF`. The Android
  app has never been launched anywhere.
- **CI never launches the desktop app itself.** `test_qml` builds the
  app's window and pages from the same QML module, offscreen, on all
  three desktop platforms, but never runs the `qttune` executable, and
  nothing looks at a frame. The manual smoke tier covers that, on the
  dev machine.
- **QML tests read properties, not pixels.** They run offscreen on the
  software renderer and check what each item shows: its colour, text
  and visibility. The strip chart's drawing, the gauges, layout at each
  width, focus rings and the GPU backends are manual smoke only. Off
  Windows there's no fake vendor DLL, so only the disconnected and ok
  link states run there.

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
  hooks and its control page, `ecuio_j2534_inject_test_frame`,
  `ecuio_j2534_force_surrogate_for_testing`) are never called by app
  code.
- **When you change shared plumbing, pin the contract you changed, at
  the point you changed it.** For example,
  `DefaultConnectUsesIso15765At500k` was added alongside the query
  parameters.
