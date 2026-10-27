# Testing

What the test suites cover, how much confidence each tier gives, and
where the gaps are. For build and run commands, see
[development.md](development.md#running-tests).

## Verification tiers

| Tier | Meaning | Where it runs |
|---|---|---|
| **Unit** | Core, signal and ABI logic with no I/O | CI on all three desktop platforms |
| **Bridge** | Qt bridge and models, headless (`Qt6::Core` only) against the real 50 ms mock worker | CI on Windows, macOS and Linux, and locally |
| **QML** | The app's QML module against a real bridge (Qt Quick Test, `test_qml`), offscreen on the software renderer. Pages are built and bound as in the app, and checked through their properties (color, text, visibility), not their pixels | CI on Windows, macOS and Linux, and locally. The link states past ok need the fake vendor DLL, so they run on Windows only |
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL | CI on Windows (same-arch, all x64) |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Fuzzing** | Three libFuzzer harnesses, `fuzz_decode_signal`, `fuzz_validate_set` and `fuzz_uds_query`, feed arbitrary defs, payloads, sets and `did=` queries to the decoder, the validators and the `uds-generic` parser under ASan (and UBSan with Clang) and check their contracts on every input. Not ctest cases: a time-bounded fuzzer would make a green suite stop meaning green ([fuzz/README.md](../fuzz/README.md)) | CI `fuzz` job (Linux, Clang), 60 s per harness, crashing inputs uploaded; locally with MSVC's libFuzzer in a tree of its own |
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on pull requests, and on request: a manual run (Actions → CI → Run workflow; `smoke_only` skips the other platform jobs), or `[ios-smoke]` in the commit message (`ios-simulator-smoke`, Intel macOS runner) |
| **Manual smoke** | Running the app: layout, Dashboard gauges, LiveView, strip chart, Settings picker against the live registry | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); informal quiet-bus follow-up 2026-10-20 ([note](bench-sessions/2026-10-20.md)); quiet-bus run of the vehicle session's URIs with recording, 2026-10-22 ([sheet](bench-sessions/2026-10-22.md)), and again on the laptop that goes to the car, 2026-10-25 ([sheet](bench-sessions/2026-10-25-bench.md)) |
| **Vehicle** | A live, read-only vehicle session | Two sessions on one car, its engine ECU on `0x7E8`: 2026-10-22, ignition off, engine off and idling ([session sheet](bench-sessions/2026-10-22-vehicle.md)); 2026-10-25, the same plus the request engine, the probe, identification, `uds-generic` and an ignition cycle ([session sheet](bench-sessions/2026-10-25-vehicle.md)); run sheet [vehicle-runsheet.md](bench-sessions/vehicle-runsheet.md) |

The smoke tiers earn their place: they have caught bugs no C++ test
could see. A strip-chart indexing bug lived in QML paint code, and the
iOS Simulator's narrow window showed an off-center status card and a
tab bar that named the Settings page "Setup".

## Suites

| Binary | Suites | Needs |
|---|---|---|
| `test_core` | `CoreTest` | Nothing (Qt-free) |
| `test_signals` | `SignalDecodeTest`, `SignalsTest` | Nothing (Qt-free) |
| `test_c_abi` (ctest name `c_abi`) | Every public header compiles as ISO C11 with extensions off (`-pedantic` on GCC and Clang) and links unmangled; layout asserts | A C compiler |
| `test_bridge` | `BridgeTest` | Qt Core. Compiles `app/qttunebridge.cpp` directly, so the bridge must stay QtCore-only. |
| `test_qml` | One ctest case per `tests/qml/tst_*.qml` file, `QmlTest.<name>`: `theme`, `chartlayout`, `liveview`, `dashboard`, `linkhealth`, `settings` | Qt Quick. Links the app's QML module (`qttune_ui`). The quiet, fault and requests cases need the fake DLL (Windows) and skip elsewhere. |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_health` | `J2534HealthTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_connect` | `J2534ConnectTest`, `J2534SurrogateConnectTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_surrogate` | `J2534SurrogateTest` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_routing` | `J2534RoutingTest` | Windows. The cross-arch case skips unless `ECUIO_FAKE_DLL` is set. |
| `test_obd2` | `Obd2SetTest`, `Obd2DecodeTest`, `Obd2MuxTest`, `RequestEngineTest` | Nothing (Qt-free) |

`ctest -N` lists every case. The current total is in
[STATUS.md](../STATUS.md).

## Coverage

The README's badge is line coverage measured by OpenCppCoverage over the
whole ctest suite, in CI's `win-coverage` job on every push, published
to [Codecov](https://codecov.io/gh/erickramer42/qttune) with a per-file
view. `scripts/coverage.sh` runs the same measurement locally
([development.md](development.md#running-tests)).

- **Counted:** the native code in `core/`, `surrogate/` (the x64 child,
  which the surrogate cases spawn) and `app/` (the bridge and the
  recorder). A line counts as covered when any test binary ran it.
- **Not counted:** QML and its JavaScript, which `test_qml` exercises
  but OpenCppCoverage can't see; `app/main.cpp`, which no test links;
  the x86 surrogate, because CI never runs the cross-arch
  configuration; and the tests and fake DLLs themselves. There's no
  branch coverage.
- **What it doesn't show:** a covered line ran, nothing more. The
  real-hardware and vehicle gaps below are outside what any coverage
  number measures. Most uncovered lines are failure branches the fake
  can't provoke: pipe I/O timeouts, a child's startup exit codes, a
  thread that fails to start.

## Performance

### Cost per frame, by profile size

`tests/bench_bridge.cpp` (target `bench_bridge`, built with the tests,
never run by ctest) measures each thread's work per frame through the
bridge, by the size of the session's profile:
- **On the worker** ([ADR-0024](adr/0024-decode-on-the-worker.md)):
  `onFrame` (the frame's 4 KB copy and the queued call), core's match
  of the frame against every def and decode into a batch (Known Issue
  #4), and the bridge's staging of that batch.
- **On the GUI thread:** the drain (the queued call, the frame log and
  the counters), and one 100 ms tick's update of a model of N rows.

Recording is off and no view is attached. The defs sit four to a CAN
ID, 16 bits each, as a passive-bus profile packs them, so each frame
decodes four values and is checked against every def. A raw session,
which decodes nothing, is the baseline.

```sh
build/tests/Release/bench_bridge.exe [frames per run, default 20000]
```

On the dev machine, 2026-10-27, with no heavy process running
(Release; each figure the median of three runs of 20,000 frames; the
ranges are across three such measurements):

| Defs | Worker: match and decode, µs a frame | GUI thread: one tick's update, µs |
|---|---|---|
| 9 | 0.08–0.09 | 0.3–0.7 |
| 100 | 0.26–0.28 | 1.8–2.3 |
| 250 | 0.50–0.53 | 4.4–5.7 |
| 500 | 0.92–0.94 | 8.9–9.1 |
| 1000 | 1.76–1.77 | 18.1–18.6 |

- **The match and decode cost about 1.7 ns a def a frame.** Against
  the bus: classic CAN at 500 kbit/s carries at most about 4,500
  eight-byte frames a second, so 1000 defs take about 8 ms of the
  worker's second, under 1 %. The first vehicle's request rotation ran
  about 26 frames a second. A CAN FD bus can run several times the
  classic rate: at 25,000 frames a second the same profile would take
  about 4.4 % (worked out from these numbers; no CAN FD bus was
  measured). It sits well inside the J2534 worker's 10 ms read slice.
- **The decode figure varies between sessions more than within one.**
  The three measurements above agree within 0.01 µs, but other
  sessions on the same machine read 1.37 to 2.16 µs at 1000 defs
  ([ADR-0024](adr/0024-decode-on-the-worker.md)), so read it as about
  1.4 to 2.2 ns a def a frame: 6 to 10 ms of the worker's second on a
  full classic bus, 3.4 to 5.4 % at the CAN FD rate above. The tick
  and the drain stay within their ranges across those sessions.
- **The 4 KB copy is the worker's largest cost up to about 500 defs:**
  `onFrame` (the copy and the queued call) takes 0.9 to 2.1 µs at any
  profile size, more than the match and decode until 1000 defs, where
  the decode passes it.
- **The GUI thread's cost per frame doesn't grow with the profile:**
  its drain stays under 0.4 µs from a raw session to 1000 defs, because
  the decode runs on the worker. The bridge's staging of a batch is
  near the bench's resolution at every size.
- **The tick's update is linear in the profile:** under 0.02 % of the
  GUI thread at 1000 defs and 10 Hz. Each window keeps every signal's
  newest, lowest and highest value
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)'s appendix).
- **The engine-start fixture under `obd2`** (7,074 frames, 297 s
  recorded, peaking at 1,261 rpm): at `speed=10` and at `max`, no frame
  dropped, and the strip chart's highest RPM is 1,261 at both.
- **Not measured:** a QML view attached to the models; text defs.

### Load test: the replay fixture through the bridge

`bench_bridge --load` replays the engine-start fixture (7,074 frames
over 297 s recorded, under `obd2`, RPM peaking at 1,261.25) through a
real `QtTuneBridge`: once at full speed, once at 100× (paced, about
3 s), and looping at full speed for 3 s. The full bench adds a 10× run
(about 30 s) after its per-profile table. Each run reports the bridge's
own delivery figures, the ones the activity log's "Delivery:" line
gives at every disconnect:
- **Worker frames/s:** frames through the worker's whole path: the
  replay's read, core's decode and the bridge's two callbacks.
- **Frame to GUI thread:** from `onFrame` on the worker to the frame
  log's handler on the GUI thread.
- **Value to model:** from `onValues` on the worker to the 100 ms tick
  that puts the value in the model the pages draw from.

Latencies are read at the top of a histogram bucket, at most 12.5 %
above the true value; the maximum is exact. QML's painting isn't
included: the bench runs headless.

Three runs on the dev machine, 2026-10-27, with no heavy process
running (ranges across the runs; milliseconds as median / p99):

| Run | Frames handled / dropped | Worker frames/s | Frame to GUI thread | Value to model | Chart's peak, rpm |
|---|---|---|---|---|---|
| Full speed, one pass (14–16 ms) | 7,074 / 0 | | 0.09–0.10 / 0.38–1.28 | 98–115 / 100–116 | 1,261.25 |
| 100×, one pass (2.98 s) | 7,074 / 0 | | 0.08–0.10 / 0.38–0.41, one run 28.7 | 49–57 / 110–113, one run 359 | 1,261.25 |
| 10×, one pass (29.75 s) | 7,074 / 0 | | 0.04–0.05 / 0.32–0.35 | 49 / 112–115 | 1,261.25 |
| Full speed, looping 3 s | 1.44–1.72 million / 0–955 | 417,000–474,000 | 0.24–0.26 / 1.15–1.28 | 90–98 / 111–113 | |

- **Throughput:** across thirteen runs that day (the three above and
  ten more), the worker sustains 375,000 to 565,000 frames a second,
  more than 80 times a fully loaded classic CAN bus (about 4,500
  frames a second).
- **At full speed the in-flight cap drops frames**
  ([ADR-0003](adr/0003-backpressure-bounds-in-flight-frames.md)): a
  one-pass run sends the 7,074 frames in about 15 ms, faster than the
  GUI thread drains them. Five of the ten further runs dropped 1,322
  to 6,074 of them; in the worst the GUI thread waited 22 ms and only
  the cap's 1,000 got through. One looping run in thirteen dropped
  955 of 1.72 million. Every value of a dropped frame still reaches
  the pages ([ADR-0024](adr/0024-decode-on-the-worker.md)): every
  one-pass run timed all 7,061 values and kept the peak.
- **One stall in thirteen runs:** in one 100× pass the GUI thread was
  held up once, up to 316 ms for a frame and 359 ms at p99 for a
  value. Nothing dropped and the peak held. The other twelve stayed
  under 0.6 ms at p99 to the GUI thread, and the stall's cause wasn't
  found. CI's bounds (500 ms and 1,000 ms at p99) have room for a stall
  that size, but not much.
- **Values wait for the tick, by design**
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)): paced, the
  median is about half a tick and p99 a tick plus the timer's jitter.
  At full speed a whole pass arrives within milliseconds of a tick and
  waits out the rest of it.
- **Frames reach the GUI thread** in under a quarter of a millisecond
  at the median, and within 1 to 3 ms at p99 even at full speed.
- **The limit it found:** at full speed the recording misses about two
  thirds of the values. The staging holds 16,384 values a window,
  164,000 a second, against 480,000 to 570,000 arriving. The pages lose nothing: each
  window keeps every signal's newest, lowest and highest value whatever
  the limit, and the activity log says when the recording misses some.
  A full classic CAN bus at four values a frame gives 18,000 a second.
- **In CI:** `win-build` and `linux-build` run `bench_bridge --load`
  on every push and put its table in the run's summary, so each run
  publishes a second and third machine's figures. The step fails on a
  broken invariant (a one-pass run must account for every frame, time
  every value and keep the 1,261.25 rpm peak) or a bound: at least
  10,000 frames a second through the worker, and at 100× a p99 under
  500 ms to the GUI thread and 1,000 ms to the model. The bounds sit
  far from the dev machine's figures because shared runners vary; they
  catch a regression of an order of magnitude, and smaller drift shows
  in the summaries. macOS doesn't run it: its runner wakes timers
  100–200 ms late.

### Connect cost: a surrogate per session

Every J2534 session spawns its surrogate
([ADR-0025](adr/0025-one-j2534-backend.md)). Against the x64 fake, a
create and close (`J2534ConnectTest.InfoCarriesTheUrisBaudAndChannel`,
which spawns, handshakes, opens, connects, reads the battery and the
versions, then closes) takes 18 to 28 ms, warm (30 repeats in one
process) or cold (five fresh processes), Release, 2026-10-26; in the
app's own process, before ADR-0025, it took 0 to 1 ms. A first run
after a build reads up to 89 ms, the exe's file not yet cached. A real
adapter's `PassThruOpen` adds its own time on top. Reads cost a pipe
round trip each, negligible at the transport's read cap
([ADR-0014](adr/0014-single-threaded-surrogate.md)).

## What is verified

| Capability | Verified by |
|---|---|
| Core builds with zero Qt dependencies | CI `core-and-tests` job (Ubuntu, no Qt installed), plus the configure-time link check in the root `CMakeLists.txt`. That check was verified 2026-10-20 to fire on an injected `Qt6::Core` link, in core-only and in full configures. |
| Core and the Qt-free tests run clean under ASan, UBSan and LeakSanitizer | CI `core-sanitizers` (Linux, blocking). Its first run, 2026-10-20 (`8dcba3a`), found no memory errors or UB, but 5 tests closed their session without destroying it. With those fixed, run #85 (`6991bdd`) passed 74/74. |
| Every public C ABI header (`core.h`, `signals.h`, `signal_sets.h`, `profile.h`, `j2534.h`, `j2534_surrogate.h`) is valid strict C, each included on its own first | `c_abi` |
| Callback registration: dedup, idempotent unregister, reentrant register/unregister during dispatch; a dispatch reaches every callback once at any list length, and the list as it stood when the frame arrived (one removed mid-dispatch still called, one added not) | `CoreTest`, `.DispatchReachesEveryCallbackOnceAtAnyListLength`, `.DispatchReachesTheListAsItStoodWhenTheFrameArrived` |
| No transport's worker allocates per frame, so no `bad_alloc` can leave a worker: a dispatch to up to four callbacks takes its snapshot on the stack, and past start-up the mock's, replay's (looping, the file reopened each pass, under `obd2`) and the J2534 worker's (through the surrogate, against the fake: the probe, identification and the rotation) frames cost no `operator new`, with a values callback registered, so the decode and its batch are counted too. `tests/new_counter.h` counts `operator new` per thread, read from a callback on the worker; a fifth callback is seen allocating, so the count isn't vacuous | `CoreTest.DispatchToAShortListAllocatesNothing`, `.WorkersAllocateNothingPerFrame`, `J2534AdoptionTest.TheWorkerAllocatesNothingPerFrame`. Each fails against the per-frame vector copy; the dispatch cases against a heap path that stops at four, four sent to the heap, and a dispatch over the live list; the worker case against a replay worker that reallocates its line buffer per row. Counts `operator new` only: `malloc` and `fopen` report failure as null, which the code checks |
| Callbacks run on the worker thread | `CoreTest`, through `ecuio_session_start` |
| Decoded values (ADR-0024): a frame's batch holds every def it decodes, in the set's order, equal to direct `ecuio_decode_*` calls, with the frame's time and ID; a def that can't decode is left out, and a frame that decodes nothing or a raw session delivers no batch. The values callback registers as the frame callback does, runs after the frame callbacks for each frame, and sees its list as it stood when the frame arrived, on and off the stack. Each text value has a slot of its own, counted at create from the set (unkeyed text defs plus, per key shape, the largest group sharing a key), and the slots start again each frame. A `uds-generic` scanned DID decodes from the set built at create. Every batch over the vehicle fixture under `obd2` and the mock's cycle equals the direct decode | `CoreTest.ValuesCallbackDeliversEachFramesDecodeWithItsTime`, `.ValuesCallbackRegistersAsTheFrameCallbackDoes`, `.ValuesFollowTheFrameCallbacks`, `.ValuesReachTheListAsItStoodWhenTheFrameArrived`, `.ValuesGiveEachTextValueASlotOfItsOwn`, `.ValuesOfAScannedDidComeFromTheSetBuiltAtCreate`, `.ValuesMatchDirectDecodesOfEveryFrame`. Each fails against a mutant: one slot for every text value, a slot count without the unkeyed defs, shape groups taken as the largest instead of added, slots that never restart, the frame callbacks skipped, the live values list, a batch without the frame's time, a failed decode delivered as 0 |
| The recording holds every value core decodes: over the vehicle fixture under `obd2`, the bridge's `signals.csv` equals, row for row, the batches core delivers to a session of its own | `BridgeTest.RecordingHoldsEveryValueCoreDecodes` |
| The bridge stages values from the worker (ADR-0024): a frame dropped at the in-flight cap keeps its values, so with the event loop held while 5,000 frames flood in, thousands drop, yet the recording holds every row's value in order and the row shows the last; past a window's sample limit the recording misses values, the activity log says so once and gives the count at disconnect, and the row still shows the newest; a disconnect drains the window it ends | `BridgeTest.ValuesSurviveFramesDroppedAtTheCap`, `.ValuesPastTheWindowsLimitAreCountedAndLogged`, `.ValuesStagedAtDisconnectAreRecorded`. Each fails against a mutant: values never staged, the newest lost past the limit, samples never recorded, no drain at disconnect, the limit never counted, the warning never logged, a staging never reset. The first also fails when the dropped-frame count is zeroed after the worker starts |
| Real-car recordings decode on the worker as they decoded in the car: each recording's `frames.csv`, replayed through the bridge under the profile and DIDs its connect used, records the values its own `signals.csv` holds. 24,632 values over 25 connects (the two vehicle sessions and the bench), 2026-10-26 | `BridgeTest.VehicleRecordingsReplayToTheirOwnValues`, local only: it skips unless `ECUIO_BENCH_RAW` names the recordings' folder (`Testing/bench-raw`, never committed). A mismatch names the recording, row and signal, never a value |
| Session lifecycle: idempotent close, close/destroy split | `CoreTest` |
| `ecuio_session_send` refuses a `data_length` past the frame's data (`INVALID_ARGUMENT`) and says it isn't built for one that fits; `ecuio_profile_lookup` answers an unknown name `UNKNOWN_PROFILE` | `CoreTest.SessionSendRefusesALengthPastTheFramesData`, `.ProfileLookupUnknownNameFailsWithoutTouchingOut`. Both fail against the codes they replaced |
| `ecuio_init` and `ecuio_shutdown` are optional: a session needs neither, and one open when shutdown runs keeps delivering | `CoreTest.InitShutdownIdempotent`, `.ShutdownLeavesAnOpenSessionWorking` |
| Signal decode: bit extraction, sign extension, error-code order (the kind and byte order before the bit parameters), the validator; results in `double`, exact for every 32-bit raw value; 16-bit payload lengths and start bytes past byte 255; binary32 floats and signed integers in both byte orders | `SignalDecodeTest` (including `.Uint32DecodesExactly`, `.PayloadLengthPastByte255IsNotNarrowed`, `.StartBytePastByte255Reaches`, `.FloatDecodesInBothByteOrders`, `.BigEndianIntSignExtends`), `SignalsTest` |
| The validator accepts exactly what the two decoders decode: unknown kinds and unknown byte orders are refused; a `FLOAT` must be 32 bits at bit 0, in either byte order; big-endian fields must be whole bytes; every reserved byte and `flags` must be zero; a set's layout version is checked before anything else, and its reserved fields must be zero; a set can't repeat a def ID; an empty set (no defs, maybe channels) is valid | `SignalsTest.ValidatorAcceptsExactlyWhatTheDecoderDecodes`, `.ValidatorRejectsKindsWithNoDecoder`, `.ValidatorRejectsUnknownByteOrder`, `.ValidatorRequiresFloatAs32BitsAtBitZero`, `.ValidatorRequiresByteAlignedBigEndian`, `.ValidatorRefusesEveryNonzeroReservedByte`, `.SetValidationChecksLayoutVersionFirst`, `.SetValidationRefusesNonzeroReservedFields`, `.SetValidationRejectsDuplicateIds`, `.SetValidationAcceptsAnEmptySet` |
| Text kinds: a `HEX` def gives the field's bytes as pairs and a `TEXT` def as ASCII with every other byte as `.`, from `start_byte` for `byte_length` bytes or to the end of the payload; the field must start inside the payload and end within it; the mux key gates it; each decoder refuses the other's kinds and the shapes it can't take, in the validator's order; nothing is written on failure; the text is cut to its buffer at a whole hex pair, and `ECUIO_DECODE_TEXT_MAX` holds a whole frame's hex | `SignalsTest.TextKindsDecodeToHexAndAscii`, `.TextDecodeChecksTheFieldAndTheMuxKey`, `.TextDecodeCutsToItsBuffer`. **Failed with raw bytes kept, with a trailing space left on a cut, and with an empty field decoding** |
| The validator takes a text def with no bit parameters and a `byte_length` within the frame, refuses `byte_length` on a numeric kind (code 17), and checks the kind before the shape | `SignalsTest.ValidatorChecksTextKinds`, `.ValidatorAcceptsExactlyWhatTheDecoderDecodes` (over both decoders). **Failed with the length cap and the bit-parameter rule removed** |
| A text row carries its latest text (`text`, `isText`), counts as arrived once one came, and is never plotted: not subscribed, not toggled, not in the chart's data; a numeric row ignores a text batch | `BridgeTest.TextRowsShowTheirTextAndNeverPlot`. **Failed with text rows subscribed and with the sample left out of the row's history** |
| A text def takes the bridge's whole path: decoded from the frame, staged, shown on its row and written to `signals.csv` (a `HEX` def over two bytes of the mock's telemetry) | `BridgeTest.TextSignalsReachTheRowAndTheRecording`. **Failed with the text never staged** |
| The def's layout: 120 bytes with no padding, every field's offset, and `ECUIO_SIGNAL_SET_LAYOUT`; the size and key offsets again in C, with `ecuio_validation_t`'s | `SignalsTest.SignalDefLayoutIsPinned`, `c_abi`; both also built and run in the Win32 tree by hand, 2026-10-22, since no suite runs them on x86 |
| Validation names the failing element: the def or channel at fault, the second of a duplicate pair, and `UINT32_MAX` for a pass or a code about the whole set. Registration hands the result back, with code 0 when it refused on the name or the set is NULL | `SignalsTest.SetValidationNamesTheFailingElement`, `CoreTest.ProfileRegisterReportsTheValidatorResult` |
| On arbitrary defs and payloads, the decoder returns only its defined codes, gives the same answer twice (NaN bits included), writes nothing unless it succeeds, and reads nothing past `payload_len`; a def the validator accepts fails only on the payload's bounds or its mux key. On arbitrary sets, the set validator returns only defined codes, and its `ecuio_validation_t` matches the return and names the element its code names. On arbitrary `?did=` text, the `uds-generic` builder answers OK or `BAD_URI` with a terminated detail, and a success is a valid set within every limit whose polled head derives one request per def and whose scanned DIDs all fit the engine's scan | `fuzz_decode_signal`, `fuzz_validate_set` (CI `fuzz` job; green on PR #17's run, `ce8284f`), `fuzz_uds_query` |
| The frame gate `ecuio_def_applies_to_frame` is a true predicate: 1 on a match, 0 for another ID, a missing or different mux key, or NULL | `SignalsTest.ApplyGateIsTruePredicate`, `Obd2MuxTest` |
| Mux keys are 1 to 3 bytes, read first byte most significant in either byte order, and every key byte must match. A 4-byte key is refused by the validator (code 9) and the decoder (code 3) alike, and the gate never applies it; a key wider than its length is refused (11), and a key at any `mux_offset` passes (decode refuses one past the payload it's given) | `SignalDecodeTest.MuxKeyReadsFirstByteMostSignificantInEitherOrder`, `.ThreeByteMuxKeyGatesAndDecodes`, `SignalsTest.ValidatorBoundsMuxKeysToThreeBytes` |
| Display strings: every quantity the mock and `obd2` sets both carry has one name, unit and decimal places in both (the names the Dashboard's `Tap`s look up); temperatures are a UTF-8 degree sign, never `degC` | `SignalsTest.MockAndObd2ShareDisplayNamesAndUnits` |
| Mock frames decode to physically plausible values | `CoreTest.MockFramesDecodeAgainstMockSignalSet` |
| The mock worker keeps real time: over 4 s its frames' timestamps average one 50 ms tick apart, within the bound its deadline schedule guarantees (one interval per run between stalls and restarts, spread over the gaps), so the drive cycle plays at the speed its physics assume. A host that stalls so often the bound can't see a worker 10 % slow skips (the macos-build runner) | `CoreTest.MockFramesKeepTheTickInterval`. A fixed 50 ms sleep after each tick (62.6 ms on Windows) and a schedule 10 % slow (55.0 ms) each failed it against a bound of 51.6 (2026-10-26). On Linux a fixed sleep overshoots by a fraction of a millisecond, which the test can't tell from a deadline |
| Mock drive cycle, stepped deterministically: every phase in order; the 2nd- and 3rd-gear pulls hold the fuel-cut limiter exactly 5 s, rpm past it on first contact and then sawing under it while road speed holds, the blade open, load at 95 % or more, MAF on its WOT plateau and lambda lean, the cycle mostly in cut; the 3rd-gear pull is longer and faster and heats the coolant and oil, while the intake air stays within 10 °C of ambient; a 100 km/h cruise in 4th at 2850–3100 rpm in closed loop; the cold start's fast idle near 1430 rpm on extra air, then 850–900 rpm; the launch slips the clutch above 3000 rpm and spins the tires in 1st only; the drag run crosses 402 m in 11–15 s at 150–200 km/h through 4th; only the first loop runs cold (the thermostat stays shut, the oil trails the coolant), the coolant never falls below 85 °C from the third, and in the sixth it holds 85–97 °C at cruise, peaks at 100–105 °C during the pulls and the quarter mile with the fan cutting in, the oil holds 100–125 °C and ends the loop above the coolant, and a quarter mile moves it 2–9 °C; at every tick of three loops, rpm is locked to wheel speed in gear (within the crank reading's jitter), never below 1000 rpm in gear, no more than 50 km/h with the clutch in, MAF matches rpm and load, lambda is lean exactly when fuel is cut, the rev limit cuts fuel the tick after rpm meets it in any phase, and no temperature moves more than 0.5 °C a tick; the encoded rpm never reads the same for more than half a second, and away from shifts, the limiter and the launch it never steps more than 150 rpm a tick, phase changes included; every tick round-trips through the mock set without clamping | `CoreTest.MockDriveCycle*`. Each new check failed against its mutation (checked by hand, 2026-10-24): the limiter's load dipping with the saw, the clutch going in at 3000 rpm, lambda ignoring fuel cut, a lift off the limiter left fueled at the limit, the idle without its hunting, the crank reading without its jitter, the engine falling to idle unlimited when the clutch goes in. The warm-loop checks failed against a stronger radiator, no piston heat into the oil, a lighter sump, less heat into the coolant, a fan out of reach, a thermostat opening at 70 °C, and the oil's friction heat counted on top of the fuel's (2026-10-25) |
| Frames marshal from the worker to the GUI thread | `BridgeTest.FramesMarshalFromWorkerToGuiThread` |
| Signal updates are coalesced to 10 Hz whatever the frame rate; within a window the row shows the newest sample, numeric and text, and the chart gets the window's lowest, highest and newest at their frames' session times, each once, in time order; a row's age counts from its value's arrival, not its sample time; the vehicle fixture's 1,261 rpm peak reaches the chart | `BridgeTest.SignalBatchesAreCoalesced`, `.AWindowShowsItsNewestAndChartsItsEnvelope` (batches handed to `onValues` as the worker hands them), `.AWindowChartsItsLowestHighestAndNewestInTimeOrder`, `.TheChartKeepsTheFixturesPeak`. Each fails against a mutant: a lowest or highest that never moves, samples with no frame time, age from the sample time, repeats kept, points out of time order. The model finds a row by id; a wrong row and a text batch reaching a numeric row each fail the bridge tests |
| History ring eviction, `valuesUpdated` semantics; `chartData()` reports each series' model row, which keys its trace color | `BridgeTest` |
| A subscription only chooses what the chart plots: an unsubscribed row keeps taking values and history, so the Dashboard stays live | `BridgeTest.UnsubscribedSignalKeepsUpdating` |
| Display precision: `DecimalsRole` and `chartData()`'s `decimals` carry each def's `decimal_places` | `BridgeTest.DecimalsComeFromTheDefAndReachTheChart` |
| Display units ([ADR-0022](adr/0022-display-units.md)): imperial turns km/h, degrees C, kPa, g/s and km into mph, degrees F, psi (one more decimal place), lb/min (one more, capped at 3) and mi, each factor exact; every other unit, and everything under metric, passes through; a NULL or an unknown system writes nothing; the struct's layout | `SignalsTest.DisplayConvert*`, `.DisplayValueLayoutIsPinned`; `c_abi` in both trees |
| The unit system converts each row's value, unit and decimals and the chart's whole history, one redraw per change, and metric gives back exactly what was decoded; the bridge takes 0 or 1 only, and a recording under imperial keeps km/h | `BridgeTest.UnitSystemConvertsRowsAndChartAndKeepsHistory`, `.UnitSystemShowsImperialAndRecordsDecodedUnits`. Each failed against its mutation (checked by hand, 2026-10-24): rows keeping the decoded unit; the chart's history unconverted |
| Under imperial the Dashboard's gauges read mph, degrees F and lb/min, their scale ends and zones follow (the speedometer to 160 mph), units with no imperial form stay, and a change of units clears the held peaks; Settings' Units row sets the bridge's unit system, connected or not | `QmlTest.dashboard`, `QmlTest.settings`. The dashboard case failed with the helper not reading `Bridge.unitSystem` (2026-10-24). The choice kept across a restart: manual smoke, 2026-10-24 |
| The signal role names LiveView and `SignalHub` take as required properties (`name`, `unit`, `value`, `subscribed`, `hasValue`, `ageMs`, `decimals`) | `BridgeTest.SignalRoleNamesMatchQmlBindings` |
| An async connect starts its session only on the GUI thread; a bridge destroyed before the completion runs destroys the unstarted session | `BridgeTest.DestroyedMidConnectLeavesNoRunningSession` (a probe callback sees no frames before the completion) |
| A connect a handler starts while another is being applied is refused, so the first session stays the bridge's: from an emit inside the apply (sync), and from `connectingChanged` as the async completion clears it | `BridgeTest.AConnectFromAHandlerDuringTheApplyIsRefused`, `.AConnectFromConnectingChangedIsRefused`. Each failed with two "Connected:" lines against the half of the fix it pins (2026-10-26): `connected` set after the apply's first emit; `connecting` cleared before the apply |
| The vendor calls run off the GUI thread: an async connect's open, and a disconnect's teardown. Disconnect drops the UI state at once, refuses a connect until the teardown ends, and logs `Disconnecting:` then `Disconnected:`; a bridge destroyed mid-teardown waits for it | `BridgeTest.AsyncConnectOpensOffGuiThread`, `.DisconnectTearsDownOffGuiThread`, `.DestroyedMidDisconnectWaitsForTeardown` (Windows only: the fake's open and disconnect delays stall the call, so a call made on the GUI thread would hold it past the bound, and the control page counts the teardown's disconnect) |
| Streaming continues past 10k cumulative frames; drops are counted and reported | `BridgeTest.StreamingContinuesPastTenThousandFrames`, `.BackpressureDropsAreCountedAndReported` |
| Log order toggle (newest first / oldest first). Rows are in arrival order, not by frame timestamp, and a connect starts an empty table while a disconnect keeps the rows | `BridgeTest.FrameLogOrdersNewestFirstByDefault`, `.FrameLogTogglesToOldestFirst`, `.FrameLogOrdersByArrivalNotTimestamp`, `.ConnectStartsAnEmptyFrameLog` |
| The frame log's ring at its cap: the newest 10k rows in arrival order either way, each with its own frame's bytes and length, the oldest leaving from its end and the new frame arriving at the other, still in order after going round oldest-first; the frame counters reach QML on the 100 ms tick, not per frame, and end exact at disconnect | `BridgeTest.FullFrameLogKeepsTheNewestInOrder`, `.FrameCountsAreReportedOnTheTick`. Each fails against a slot that ignores the oldest, a removal at the newest end, and counters emitted per frame. A frame cost about 2 µs at the cap with its dispatch included (20,000 frames, no view attached, 2026-10-24); the GUI thread's drain alone is under [Performance](#performance). Log page smoke under `mock://demo`: newest and oldest first, the tail following, Pause and Clear |
| The frame log's rows hold a payload up to 64 bytes inline and a longer one in a heap block only while the row is shown: a ring of 200-byte frames rounded by 27-byte ones holds no heap bytes, 64 bytes stays inline and 65 spills; Clear keeps the slots and frees the blocks, and a ring cleared before it filled grows only as far as it's used | `BridgeTest.FrameLogHoldsLongPayloadsOnlyWhileShown` (the model directly, through its slot and heap-byte seams). Each of five mutations fails it: a short frame keeping the block, Clear dropping the ring or keeping the blocks, growth by the cap alone, and the boundary at 63. Log page smoke, 2026-10-25: under `mock://demo` before and after Clear, and the fake's 203-byte `62 F1 A0` replies between 5-byte `62 1E 1C` ones, each row with its own length and bytes |
| The activity log's "Delivery:" line at disconnect: frames received, dropped at the in-flight cap and still queued, then how long frames took to reach the GUI thread and values the model (median, p99, max), the line naming exactly the figures the bridge kept. Over the engine-start fixture at full speed every frame is accounted for and every handled frame and staged value timed. A quantile reads at the top of its bucket: exact below 16 µs, at most 12.5 % high past that, the maximum exact | `BridgeTest.LatencyStatsReadsQuantilesWithinABucket`, `.DeliveryTextNamesFramesDropsAndTimes`, `.DisconnectLogsTheDelivery`; the line's place by `.DisconnectTearsDownOffGuiThread` and `.RecordingWritesFramesValuesAndEvents` (`events.log`). The fixture case failed with either timing left out (2026-10-26). App smoke under `mock://demo`, 2026-10-26: "Delivery: 98 frames, none dropped; to the GUI thread in median 95 µs, p99 8.7 ms, max 8.7 ms; 819 values, to the model in median 53 ms, p99 109 ms, max 109 ms" |
| Session recording ([ADR-0017](adr/0017-session-recording.md)): off by default; with it on, one folder per connect holding every accepted frame (paused or not, host time never going backwards), each decoded value and the activity lines from `Connected:` through `Disconnected:`; the switch holds during a session; recording sets `ECUIO_SUR_LOG_FRAMES` in the OS environment the surrogate inherits and removes it at the next unrecorded connect, leaving a launch value alone | `BridgeTest.RecordingIsOffByDefault`, `.RecordingWritesFramesValuesAndEvents`, `.RecordingSwitchHoldsDuringASession`, `.RecordingTurnsOnSurrogateFrameLogging` |
| Replay ([ADR-0023](adr/0023-replay-transport.md)): each recorded row goes out with its ID, flags and bytes (an empty payload and a whole 4,096-byte message included), stamped with the recording's session time rebased to the first row; a backward stamp (a raw adapter clock wrapping) is timed by its host step; `loop=1` carries the stamps on, never decreasing; one second of recording takes about a second at speed 1 and a tenth at 10; each frame is due at its time from the start of playback, so frames a 400 ms stall held up go out at once and 200 short steps still take about a second, at speed 1 and 10; a close during an hour-long gap returns at once; the end of the file sends nothing more, and a row that turned bad after attach, or a file deleted before start, ends playback with `ECUIO_ERR_RECORDING_CHANGED` and one episode in the error state, which a clean end leaves at `ECUIO_OK`; the session info names the backend, frames, span, speed and loop | `CoreTest.ReplaySendsTheRecordedFramesAtTheirRebasedTimes`, `.ReplayTimesABackwardStampByTheHostClock`, `.ReplayLoopsWithStampsThatNeverDecrease`, `.ReplayPacesByTheRecordingsTimeOverTheSpeed`, `.ReplayCatchesUpAfterALateFrameAndNeverDrifts`, `.ReplayClosesAtOnceDuringAGap`, `.ReplayStopsAtARowThatTurnedBadAfterAttach`, `.ReplayOfARecordingThatWentAwaySaysSo`; also in the x86 tree. Each failed against its mutation (2026-10-25): no host fallback, stamps not rebased, the speed ignored, no pacing, loop stamps restarting, a sleep for the condition wait (the close hung), a bad row sent anyway. Each wait timed from the last send instead of the start (2026-10-26) passes `.ReplayPacesByTheRecordingsTimeOverTheSpeed` and fails `.ReplayCatchesUpAfterALateFrameAndNeverDrifts`: 3.5 s on the condition variable, whose short waits Windows rounds up, and 1.4–1.6 s busy-waited, where only the stall shows |
| Replay refuses what isn't a recording, at create: anything but `frames.csv`'s header row, an empty file, no frames, and each malformed field with its line number (`LOAD_FAILED`); a relative or empty path, and every malformed `speed` and `loop` value, and `loop=1` on a recording whose frames all share one time (`BAD_URI`; played once, it plays); either line ending reads; a folder means its `frames.csv`; `raw` is the default profile, and a recorded reply decodes under `?profile=obd2` | `CoreTest.ReplayRefusesABadRecordingWithItsLineNumber`, `.ReplayJudgesItsPathAndQuery`, `.ReplayRefusesToLoopARecordingWithNoSpan`, `.ReplayDecodesUnderTheProfileItNames`. The header check, the CRLF strip and the span check each failed against their mutation |
| A real ECU's replies, replayed: the second vehicle session's engine start (`tests/fixtures/replay/2026-10-25-engine-start`, 7,074 frames over 297 s, its six identification replies removed) decodes under `obd2` to the sheet's values: RPM peaking at 1,261.25, the module voltage dipping to 9.564 V, coolant 60 to 86 °C, the first RPM 3.45 s in; the fixture holds no identification reply | `CoreTest.ReplayOfTheVehicleFixtureDecodesTheSheetsValues` ([sheet](bench-sessions/2026-10-25-vehicle.md), Phase 4). App smoke, 2026-10-25: the fixture at `?profile=obd2&speed=10` drove the Dashboard and Live, the header showed `replay://…/2026-10-25-engine-start?…`, and the log read the "Session:" line and "Replay finished: 7074 frames" 29.7 s later. The Dashboard's RPM peak read 1,195, not 1,261: at 10× the 100 ms window kept a later value over the peak (2026-10-26, before each window kept its envelope; the chart now keeps it, `BridgeTest.TheChartKeepsTheFixturesPeak`) |
| The round trip ADR-0017 promised: a recorded `mock://` session replayed at full speed under `mock` delivers every frame with none dropped (the first included, though a replay sends it the moment the session starts) and logs "Replay finished" once; its own recording matches the first row for row (ID, flags, bytes, the times rebased) and value for value. The replay's header label keeps the recording's folder name and drops the rest of the path | `BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded`, `.ReplayUriLabelKeepsTheRecordingsName`, `.SessionInfoTextDescribesAReplay`. With `onFrame` gated on the connect being applied, as before, the round trip failed 3 runs in 3: frames sent before the apply were lost |
| A replay whose recording changes under it stops and says why: a looping replay whose file is rewritten reaches the `stopped` link state with `ECUIO_ERR_RECORDING_CHANGED` and one episode, logs "Replay stopped after N frames: the recording changed after it was opened" once, and never "Replay finished"; Theme shows `stopped` amber as "Replay stopped", and Live's banner "Replay stopped — the recording changed after it was opened" | `BridgeTest.ReplayThatStopsEarlySaysSo`, `.LinkStateMapsEveryPublishedCode`; `QmlTest.theme`, `QmlTest.liveview` (`test_bannerText`). Each failed against its mutation (2026-10-26): the worker not publishing the code (the two core cases and the bridge case), no `stopped` mapping, no log line, and the file opened unshared with `_wfopen_s` (the bridge case can't rewrite it)
| The frame role names the Log page binds (`timestampUs`, `dlc`, `extended`, `payloadHex`, `arbId`), read through `Bridge.model` | `BridgeTest.FrameRoleNamesMatchLogPageBindings` |
| Log payloads are the frame's own length as spaced uppercase hex (`34 12 AA 55`) | `BridgeTest.DispatchedShortFrameHonorsDataLength` |
| Log Pause: nothing is appended while paused, while frames still count and decode; disconnect clears it and a new session starts unpaused | `BridgeTest.LogPauseStopsFrameLogNotDecode` |
| Discovery: dual-root, dual-WOW64 sweep, dedup, PE architecture annotation; where `ECUIO_EXPECT_J2534_DEVICES` says the machine has drivers, at least that many are found (`scripts/verify.sh` sets 1 on Windows, so the dev machine's installed driver has to turn up) | `J2534DiscoveryTest`, plus manual smoke against real installed drivers (OBDX Pro VT and VX) |
| Bridge discovery marshaling, including the architecture field | `BridgeTest.DiscoveredDevicesHaveWellFormedEntries` |
| Loader: `LoadLibraryEx`, nine-export resolution, `PassThruOpen`, clean unwind on a missing DLL | `FakeFixture` |
| Teardown call order against the vendor API | `FakeFixture.OpenCloseCycleHitsVendorApi`, plus surrogate teardown |
| `j2534://` session end to end with exact counts, payload fidelity and FIFO order | `J2534StreamTest` |
| `?baud=` and `?flags=` reach `PassThruConnect`; defaults kept; malformed URIs rejected before any DLL load | `J2534QueryParamTest` |
| 29-bit channels connect with `CAN_29BIT_ID` (from `?filter=`/`?txid=` or a profile), on top of `?flags=`; a profile mixing 11- and 29-bit channels is rejected at create | `J2534QueryParamTest.Wide29BitChannelSetsConnectFlag`, `.ProfileChannelWidthDecidesConnectFlag` |
| Surrogate: spawn under the job object, pipe handshake, batched `OP_READ` delivery, dead DLL fails fast, clean teardown | `J2534SurrogateTest` |
| Write, ioctl and filter relay through the surrogate, including whitelist refusal and malformed filter shapes | `J2534SurrogateTest`, `J2534RoutingTest.X64WriteIoctlFilterRoundTrips` |
| Routing by PE architecture ([ADR-0025](adr/0025-one-j2534-backend.md)): an x64 DLL loads in the x64 surrogate, never in the test's process; an x86 one in the x86 surrogate; a missing file or a file that isn't a PE is refused with no spawn; a PE for another machine (ARM64) goes to the x64 surrogate, whose load refuses it | `J2534RoutingTest.X64FakeSelectsTheX64SurrogateAndRoundTrips`, `.MissingDllIsRefusedBeforeAnySpawn`, `J2534ConnectTest.MissingDllFailsToLoad`; in the app (`run-qttune`, 2026-10-26): the x64 fake's session log names `ecuio-j2534-surrogate-x64.exe`, the x86 fake's `ecuio-j2534-surrogate.exe`, and both sessions' lines read "through the surrogate" |
| Paths are UTF-8 to UTF-16: a DLL in a directory named in Greek and CJK (outside every ANSI code page) is sniffed and loads in the surrogate child, same-arch and cross-arch. With `LoadLibraryExA` the load fails (checked by hand, 2026-10-21, on code page 1252) | `FakeFixture.NonAsciiDllPathLoads`, `J2534SurrogateTest.NonAsciiDllPathLoadsInTheChild` |
| A surrogate's variable (`ECUIO_SURROGATE_EXE`, `ECUIO_SURROGATE_X64_EXE`) only relocates it: a relative path, another exe's name or a missing file fails the open; the spawn line names the exe used and the variable that chose it | `J2534SurrogateTest.SurrogateExeOverrideIsRestricted` (the x64 one, and the x86 one cross-arch), `.ClientAndChildLinesShareTheSessionLog` |
| Cross-arch: x64 test → x86 surrogate → x86 fake, loader round trip plus full session lifecycle | `J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` (dev machine) |
| Settings picker: device list, refresh, manual URI connect and disconnect, error surfacing | Manual smoke (Windows, live registry) |
| Settings composes the URI: a preset sets the base, the profile row its query (no scheme's keys added or taken: `raw` composes no `filter=` on any base, a replay's included), the DID field `did=` as typed minus whitespace; a new base keeps the profile and DIDs; a hand-written URI is left as typed and sets the row and the DID field from its `profile=` and `did=` (a name the row doesn't offer lights no button), the next click keeps its other keys in place (`identify=0`, `poll=`, a typed `filter=`, through every choice), and the vehicle run sheet's typed URIs (bare, `raw` with its filter, `?poll=`, a bitmap read) pass through untouched; the DID field refuses `&`, `=`, `?` and `#` at the keyboard; core's refusal (no DIDs, a bad type) shows in the error line; a composed `uds-generic` URI connects through the mock with one row per DID, and the choices are disabled while connected; while a connect is in flight (the fake's open stalled by `ECUIO_FAKE_OPEN_DELAY_MS`, Windows) the device rows, the field, the presets, the profile row and Connect are all disabled, and the field is still what was connected once it lands | `QmlTest.settings`; the rows, the hint and the DID field on screen at 1280 × 650 in a manual run, 2026-10-23 (the run-qttune driver) |
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
| A reply to another request (the child answers `OP_CONNECT` twice, `ECUIO_SUR_TEST_REPLY_TWICE`) marks the child unresponsive: the next call fails on the mismatch, and a write after it fails at once without reaching the child | `J2534SurrogateTest.RequestIdMismatchMakesTheChildUnresponsive`, also cross-arch. Against a client that only failed the call (checked 2026-10-24), the write reached the child and nothing was marked |
| A relative DLL path is refused at create (`BAD_URI`), before any load; drive paths with either separator still reach the loader | `J2534QueryParamTest.RelativeDllPathsAreRefused`. Before the check (2026-10-24), `j2534://fake_j2534.dll` opened a session: the bare name loaded the fake through the DLL search order |
| The loader refuses a relative `dll_path` (`LOAD_FAILED`, a bare name, `.\` and a drive-less root) before anything opens it, with the fake in the current directory the child inherits; discovery leaves out a relative `FUNCTIONLIBRARY` | `J2534ConnectTest.ARelativeDllPathIsRefusedBeforeAnyLoad`, also cross-arch. Without the check (2026-10-25) both backends of the time loaded and opened the fake from the current directory. Discovery's skip: `J2534DiscoveryTest.DiscoveryReturnsValidVendorNames` against `win-coverage`'s relative entry |
| A child that exits during `OP_OPEN` (failed DLL load) is reported by its exit code, not as a blocked vendor | Session logs read by hand, 2026-10-20 (`DeadDllFailsFastAtOpen`, `BlockedVendorOpenFailsAtHandshakeTimeout`) |
| A missing surrogate exe is reported before any spawn, as "no surrogate (<exe>)" and why: its variable naming no file, or none next to the app; nothing falls back to loading the DLL in the app | `J2534SurrogateConnectTest.MissingSurrogateExeIsASurrogateFailure` (the x64 one; the x86 one cross-arch). None next to the app: in the app, 2026-10-22, with the deployed copy moved aside (before the check, it read "couldn't start the surrogate: CreateProcess (Windows error 2)"). Every test binary has a surrogate beside it, so no test covers that case |
| Spec-spelling export resolution (`PassThruIoctl` primary, legacy `PassThruIoCtl` fallback), in the child | `FakeFixture.SpecSpellingPassThruIoctlResolvesAndWorks` (spec fake, primary path), `.LegacySpellingPassThruIoCtlFallsBackAndWorks` (the legacy-spelling fake) |
| Real-adapter lifecycle: OBDX Pro VX loads through the x86 surrogate, opens with USB power (vendor status 0), clean spawn/exit with disconnect, async connect keeps the UI responsive | BENCH session 2026-10-16, phase 1 |
| The 8-byte FLOW_CONTROL filter shape (prep #1, `fc_len == mask_len` workaround) is accepted by a real vendor — behaviorally, since the child didn't yet log filter ops. The 4-byte shape that replaced it was accepted on 2026-10-20 (row below). Quiet-bus read loop at <1% CPU, zero spurious frames | BENCH session 2026-10-16, phase 2 |
| Single-threaded surrogate child ([ADR-0014](adr/0014-single-threaded-surrogate.md)): the whole surrogate and routing suite on the redesigned child, same-arch and cross-arch (x64 test → x86 surrogate → x86 fake) | `J2534SurrogateTest`, `J2534RoutingTest` (2026-10-19, both trees rebuilt: full suite 136/136, cross-arch subset 15/15) |
| A transaction that times out doesn't put later replies out of step | `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest` |
| An `OP_READ` answered past its deadline: the next transaction drains the late reply and drops its frame, logged with the count ("drain_pending: dropped 1 frame(s)"), and the next read gets its own reply's frame | `J2534SurrogateTest.LateReadReplyIsDroppedAndLogged` (`ECUIO_FAKE_READ_DELAY_MS`). With the log line removed (checked by hand, 2026-10-23), it failed |
| Device and channel ID 0 are real IDs in the child: with a fake that hands out 0 for both, a frame reads once connected, and a close with no disconnect first still disconnects channel 0 and closes device 0 | `J2534SurrogateTest.DeviceAndChannelIdZeroAreRealIds` (`ECUIO_FAKE_ZERO_IDS`). With the child's flags replaced by `ID != 0` (checked by hand, 2026-10-23): in the read gate, no frame arrived; at shutdown, neither call was made |
| Profile registry: string-keyed lookup, null-set-is-success ("raw"), unknown names fail, last-registration wins; registration refuses an invalid set or a malformed name, and reports the validator's result; the built-ins can't be replaced or unregistered; unregister is idempotent and leaves existing sessions on their set | `CoreTest.Profile*` |
| Registration refuses a set whose defs derive more requests than the engine's table holds (`ECUIO_PROFILE_MAX_REQUESTS`), naming the first def past it; defs read from one reply count once | `CoreTest.ProfileRegisterRefusesMoreRequestsThanTheEngineHolds`. With the check removed (checked by hand, 2026-10-23), it failed |
| `mock://` shares the query grammar: only `profile`, and every malformed form (a bare `?`, an empty segment, a missing `=`, an uppercase key) fails at create | `CoreTest.MockUriQueryIsStrict` |
| Profile selection: `mock://`→mock set, `j2534://`→obd2, `?profile=` override honored, unknown names rejected at session_create before any DLL load | `CoreTest.MockSessionSelectsMockSet` et al., `J2534StreamTest.ProfileOverrideMockSelectsMockSet`, `BridgeTest.RawProfileLeavesSignalModelEmpty` |
| `obd2` signal set: mux grammar, decoder round-trips against canned wire bytes, validator including channel-decl rejection (duplicates, zero/illegal IDs, over-capacity) | `Obd2SetTest`, `Obd2DecodeTest` |
| The request engine, with the time passed in: only a 2-byte `41 <PID>` or a 3-byte `62 <DID>` key at offset 0 enrols a request (`01 <PID>`, `22 <DID>`), once per reply and channel, in def order, on the channel of the def's `message_id`; only services 0x01, 0x09 and 0x22 enter the table; it holds 128 and names the first def past them; each channel walks its own rotation with one request outstanding; the next waits for the reply and the 20 ms floor, or the 100 ms wait; positive, negative and pending replies correlate by service and key, pending holding the request for 5.05 s; an unsent request leaves nothing outstanding | `RequestEngineTest` (`test_obd2`) |
| Requests through the worker: rotation in set order and its wrap; each request goes to the channel of its def, Service 01 and 0x22 side by side; a `62 <DID>` def asks for its DID, 7 bytes on the wire, and decodes the reply; a backlog is delivered beside the replies | `J2534AdoptionTest.Obd2SchedulerRotatesPidsInSetOrder`, `.RequestsGoToTheirDefsChannel`, `.DidDefRequestsItsDidAndDecodesTheReply`, `.RequestModeDeliversABacklogBesideItsReplies`. With `62 <DID>` keys enrolling nothing (checked by hand, 2026-10-23), the routing and DID cases failed |
| The request engine's pacing: requests follow their replies down to the floor (20–55 a second against the fake, which answers at once; 32 measured); a negative reply ends its request as a positive one does; a request whose reply comes after the wait times out at 100 ms (6–12 a second; 9 measured), and the late replies are still delivered; "response pending" holds a request for its reply; two channels each wait only for their own reply; on the VX, replies are seen within the 10 ms read slice | `J2534HealthTest.RequestsFollowRepliesDownToTheFloor`, `.NegativeReplyCompletesItsRequest`, `.UnansweredRequestWaitsOutItsWait`, `.PendingReplyHoldsTheRequestForItsAnswer`, `.ChannelsWaitOnlyForTheirOwnReplies`, `.VxRepliesAreSeenWithinASlice`. Each failed against the mutation of the engine it pins (checked by hand, 2026-10-23): no floor; no correlation; negative replies ignored; pending completing the request; one request across all channels; a 100 ms slice |
| Passive mode drains a burst: 16 frames posted at once all arrive within 250 ms (30–48 ms measured) | `J2534HealthTest.PassiveModeDrainsABurstWithoutWaiting`. With a 20 ms sleep after each frame (checked by hand, 2026-10-23), it failed |
| Replies longer than a CAN-FD frame arrive whole: a 203-byte DID reply, ISO 15765-2's longest message on classic CAN (4095 bytes), and one that fills `PASSTHRU_MSG.Data` (4121 bytes), which arrives as the frame's `ECUIO_FRAME_DATA_MAX` (4096) bytes marked `ECUIO_FRAME_FLAG_TRUNCATED`; every data byte as the fake filled it | `J2534HealthTest.LongRepliesArriveWhole` (the fake's DIDs `0xF1A0`–`0xF1A2`). **Failed against a 64-byte cut and with the flag never set** on both backends, and through the surrogate with the child sending no frames |
| A read's batch crosses the surrogate pipe whole: sixteen 4099-byte messages in one `OP_READ`, about 66 KB, every byte as the vendor gave it | `J2534SurrogateTest.ABatchOfLongMessagesCrossesThePipeWhole` (spec-mode fake). **Failed with the child sending no frames, with the client keeping one, and with the copy cut at 64 bytes** |
| Both read paths copy a vendor's message through one function: `RxStatus` and the stamp as they came, `Data` up to its `DataSize` and never past `PASSTHRU_MSG.Data`, whatever size a vendor reports | `J2534StreamTest.ReceivedMessageCopyStopsAtTheEndOfData`. **Failed with the copy one byte short** |
| The probe's walk, with the time passed in ([ADR-0015](adr/0015-request-engine.md) §4): before its rotation, a channel with Service 01 requests asks `01 00`, then each next bitmap while the last bit is set, on the floor's pacing; bits read as J1979 lays them out (Wikipedia's worked example, `BE 1F A8 13`); a clear bit drops its PID, and a clear last bit every later PID; on the first vehicle's recorded bitmaps only 0x5C goes; silence, a refused bitmap (0x12) or one too short ends the walk with nothing judged past what was answered, and 0x11 there drops every PID; the 1 s budget ends it with the PIDs it didn't reach kept; it resumes at the first unanswered bitmap and never repeats a finished chain; only channels asking for PIDs walk, and one left with nothing sends nothing | `RequestEngineTest.ProbeWalksTheBitmapsBeforeTheRotation`, `.TheFirstVehiclesBitmapsDropOnly0x5C`, `.SilenceEndsTheWalkAndDropsNothing`, `.ARefusedBitmapEndsTheWalk`, `.TheBudgetEndsTheWalk`, `.TheWalkResumesWhereItStopped`, `.OnlyChannelsAskingForPidsWalk`. Each failed against the mutation it pins (checked by hand, 2026-10-23): the bitmaps dropping nothing; nothing dropped past a chain's end; bits read least significant first; timeouts dropping; no budget; a resumed walk starting at 0x00; a finished chain walked again |
| Negative replies with the probe on: 0x12 and 0x31 drop their request, 0x33 and 0x7F theirs (security access, another session), 0x11 every request of its service on that channel; 0x22, 0x21, 0x10, 0x13 and 0x14 drop nothing; with the probe off nothing drops; after a timeout a negative reply, which names no PID or DID, drops nothing until a positive reply puts the channel back in step | `RequestEngineTest.NegativeRepliesDropOnAnswersThatLast`, `.ServiceNotSupportedDropsTheServiceOnItsChannel`, `.WithoutTheProbeNothingIsDropped`, `.ALateNegativeReplyDropsNothing`. Each failed against its mutation (2026-10-23): every code dropping; 0x11 dropping only its request; no in-step rule |
| The probe through the worker, against the fake: a bare `obd2` URI asks `01 00`, `01 20`, `01 40`, then the rotation, and drops nothing, with the report's bitmaps equal to bytes worked out by hand from the fake's PIDs; in a set of four, the bitmaps alone drop 0x0B, 0x5D and 0x61 while 0x0C keeps being asked; a DID refused with `7F 22 31` leaves after its first answer; `?probe=0` sends the set as written, refusals and all; a connect while nothing answers keeps every PID through the quiet bus, and once the link is ok the walk resumes and drops what the bitmaps mark | `J2534AdoptionTest.ProbeWalksTheChainBeforeTheRotation`, `.ProbeDropsWhatTheBitmapsMarkUnsupported`, `.NegativeReplyDropsTheDidTheEcuRefuses`, `.ProbeOffSendsTheSetAsWritten`, `.ProbeResumesWhenTheLinkRecovers` (`ECUIO_FAKE_UNSUPPORTED_NRC`). Each failed against its mutation (2026-10-23): no walk after a recovery; `?probe=0` ignored; timeouts dropping; the bitmaps dropping nothing; bits least significant first; negative replies dropping nothing. With `?poll=` probing, `PollOverridesSchedulerRotation`, `RequestLoopWritesMode01AndReceivesStrippedEcho` and `J2534HealthTest.NegativeReplyCompletesItsRequest` (both backends) failed |
| The DID scan, with the time passed in ([ADR-0020](adr/0020-uds-generic.md)): scanned DIDs take turns with the rotation, a channel with nothing to rotate scans every turn; a positive reply puts the DID in the rotation (counted but not polled when the table is full), a negative one counts it refused, a timeout leaves it unanswered and a rescan asks it again; an unsent scan request stays due; a DID is kept once per channel and the scan stops at `ECUIO_ENGINE_SCAN_MAX`; the report's counts and `ECUIO_SUPPORT_SCAN_COMPLETE` | `RequestEngineTest.ScanTakesTurnsWithTheRotationAndKeepsWhatItFinds`, `.ScanAloneRunsEveryTurnAndRescanAsksTheUnansweredAgain`, `.ScanFoundWithAFullTableIsCountedButNotPolled`, `.UnsentScanRequestStaysDue`, `.ScanAddKeepsADidOncePerChannelAndStopsAtItsLimit` |
| `uds-generic`'s `?did=` grammar and the set it builds: single DIDs polled on 7E8/7E0 as `HEX` defs keyed `62 <DID>` from byte 3, each type's kind and width (big-endian, raw: `12 34` is 0x1234 as `u16`, `FF` is −1 as `i8`), an `<ecu>:` prefix adding a channel asked 8 below or on the 29-bit swapped pair, and taking a range with or without a type, ranges scanned and repeats kept once with the polled defs first, every refusal's words (shape, type, ECU, range order, 129 to poll, 1025 to scan, 5 ECUs), the limits themselves accepted, and the registry's empty template set | `UdsGenericTest.SingleDidsArePolledOnTheDefaultEcu`, `.TypesSetTheKindAndWidth`, `.EcuPrefixNamesTheRespondingEcu`, `.EcuPrefixTakesARange`, `.RangesAreScannedAndRepeatsKeptOnce`, `.RefusalsNameTheirCause`, `.TemplateSetIsEmptyAndValid` |
| `uds-generic` by URI: create builds a set of the session's own (two sessions, two sets), `?did=` with another profile and `?profile=uds-generic` without it are `BAD_URI` with their words, the profile's name is judged first; the mock answers each DID every tick from the ECU its def names, a growing count as a number and `MOCK <DID> #<tick>` as text | `CoreTest.UdsGenericBuildsItsSetFromTheUri`, `.DidKeyBelongsToUdsGeneric`, `.MockAnswersTheSessionsDids` |
| `uds-generic` through the worker, against the fake: a listed DID is asked `22 1E 1C` on 0x7E0 and its reply decodes through the URI's def to 0x1234; `7E9:1E1C` asks on 0x7E1 and is answered from 0x7E9; a range of nine is scanned in order, the one DID the fake knows is found and then polled, the eight refused are counted, and the report reads complete with no walk; malformed `?did=` forms fail at create | `J2534AdoptionTest.UdsGenericPollsItsDidsAndDecodesTheReply`, `.UdsGenericEcuPrefixAsksThatEcu`, `.UdsGenericScanFindsWhatTheEcuAnswers`, `J2534QueryParamTest.MalformedQueriesRejectedAtSessionCreate` |
| The ECU line's scan clause: "scan: 9 DIDs, 1 found, 8 refused", "(N polled)" when the table filled, "N unanswered" while some are | `BridgeTest.ChannelSupportTextNamesTheEcuAndWhatItDropped` |
| A text row on screen: under `mock://demo?profile=uds-generic&did=F190:text,1E1C:u16` the text row's value label shows the mock's text and its plot box is disabled, while the number row beside it formats a number and can be plotted | `QmlTest.liveview` (`test_textRowShowsTextAndCannotPlot`) |
| Identification, with the time passed in ([ADR-0021](adr/0021-vehicle-identification.md)): the list is given once per channel and asked in order, taking turns with the rotation and ahead of the scan; a positive reply's data becomes the item's text (a VIN as ASCII, two NUL-padded CALIDs joined by ", ", a CVN as hex), a negative one its code, a timeout leaves it unanswered until a rescan asks it again, nothing joins the rotation, and the report reads complete once every item has an outcome; a channel with nothing to rotate identifies every turn and then has nothing to send; the text rule itself (printable or padding is ASCII, trailing blanks dropped, anything else hex, cut at a whole pair); every request's display name | `RequestEngineTest.IdentificationTakesTurnsAndKeepsWhatTheEcuSaid`, `.IdentificationAloneRunsEveryTurnAndThenRests`, `.IdentityTextIsAsciiOrHexAndCutToTheField`, `.IdentityRequestsAreNamed` |
| Identification through the worker, against the fake: the rotation's first request, then `09 02`, then the rotation, then `09 04`; the report completes with the fake's FAKE VIN, CALID, CVN, name and text DIDs and two DIDs refused `7F 22 31`; `09 02` is written once; nothing is dropped from the rotation; `?identify=0` and `?poll=` report no items and write only the rotation; `?identify=` takes `0` or `1` only, named with the keys; the struct's layout is the same in both trees | `J2534AdoptionTest.IdentificationAsksTheListOnceAndReportsIt`, `.IdentifyOffSendsNoIdentificationRequest`, `J2534QueryParamTest.MalformedQueriesRejectedAtSessionCreate`, `.RefusedQueriesNameTheirCause`; `c_abi` in both trees. With the engine handed a six-byte copy of the reply (2026-10-23), the first and the bridge case below failed on the text |
| The activity log names what each ECU is, once complete: answered items by name and text, refused requests with their codes, the unanswered count, a request outside the list by its bytes, an empty answer, a silent ECU; through the fake, one line with every FAKE value and the two refusals | `BridgeTest.ChannelIdentityTextNamesWhatTheEcuSaid`, `.ConnectLogsWhatTheEcuSupports` (the fake, through the x64 surrogate) |
| What each channel learned is reported (`ecuio_session_get_channel_support`), and a session with no channels has none: every index is out of range; the struct's layout is the same in both trees | The stream cases above; `CoreTest.MockSessionHasNoChannelSupport`; `c_abi` in both trees. With the worker publishing nothing (2026-10-23), four stream cases and `BridgeTest.ConnectLogsWhatTheEcuSupports` failed |
| The activity log names each ECU and what it supports, once per update: the wording for the first vehicle's bitmaps, a chain answered to its end, one unanswered, and drops for every reason, past the eight a report lists; through the fake, one line, "ECU 0x7E8: supports 10 PIDs; 10 requests, none dropped" | `BridgeTest.ChannelSupportTextNamesTheEcuAndWhatItDropped`, `.ConnectLogsWhatTheEcuSupports` (the fake, through the x64 surrogate). With the bridge logging on every tick (2026-10-23), the second failed |
| The URI query is split once, in create: an unknown key fails on every scheme, named with the keys the scheme takes, before any value is read; more than 16 pairs fail; `?probe=` and `?identify=` take `0` or `1` only | `J2534QueryParamTest.MalformedQueriesRejectedAtSessionCreate`, `.RefusedQueriesNameTheirCause`, `CoreTest.CreateFailuresNameTheirCause`, `.MockUriQueryIsStrict`. With create's key check removed (2026-10-23), all four failed; with room for 32 pairs, the 17-pair cases did |
| Channel derivation: a bare `j2534://` URI under `obd2` registers the 7E8 filter with exact mask/pattern shape, sends to 7E0, and starts; a lone `?txid=` keeps the derived 7E8 filter but rewrites the request target | `J2534AdoptionTest.BareJ2534UriDerivesObd2ChannelAndStarts`, `.TxidOverrideRewritesDerivedTransmitTarget` |
| The link-health cases run through the surrogate (this tree's x64 one; the x86 one in the cross-arch run), driven through the fake's control page; each checks the fake opened in a process other than the test's. A vendor DLL that crashes mid-session (`ECUIO_FAKE_EXIT_ON_READ`) ends its child only: the session reports a hard failure, the child has exited, and the next session connects | `J2534HealthTest`, also cross-arch; `.AVendorDllThatCrashesEndsOnlyItsChild` (with the knob ignored, it fails, 2026-10-26) |
| Link health: a hard read failure records `ECUIO_ERR_TRANSPORT_HARD_FAILURE` with one episode per outage and recovers through the probe write; a quiet bus debounces to `ECUIO_ERR_VEHICLE_TIMEOUT` with no episode | `J2534HealthTest.HardFailureSetsCanonicalErrorAndRecovers`, `.QuietBusDebouncesToVehicleTimeoutWithoutEpisode` |
| Write failures: a failed write latches `ECUIO_ERR_REQUEST_FAILED` with one episode; empty reads past the quiet debounce don't move it; a successful write alone clears it, with reads held quiet | `J2534HealthTest.WriteFailureLatchesRequestFailedAndClears` (`ECUIO_FAKE_WRITE_STATUS`) |
| A write the vendor answers `STATUS_NOERROR` with no message written is a failed write: one "requests failing" episode, held past the quiet debounce, cleared by the first write sent | `J2534HealthTest.ZeroWrittenCountIsAFailedWrite` (`ECUIO_FAKE_WRITE_ZERO_COUNT`). With the count ignored (checked by hand, 2026-10-23), both backends failed: the session went quiet instead |
| A hard read failure outranks a failing write: exactly two episodes (write, then read), and continued failing writes don't downgrade the state | `J2534HealthTest.HardReadFailureOutranksRequestFailed` |
| Reads that recover while writes still fail step a hard failure down to "requests failing", with no new episode, and the first write sent returns it to ok | `J2534HealthTest.CleanReadsWhileWritesFailStepDownToRequestsFailing`. Without the step-down (checked by hand, 2026-10-23), it failed on both backends: the adapter fault stuck |
| A frame that matches no channel counts as one quiet read, not quiet at once: 300 ms after a stray that follows a reply the link still reads ok, and it goes quiet when the run ends, with no episode | `J2534HealthTest.UnmatchedFrameCountsAsAQuietRead`. With the stray setting quiet at once (checked by hand, 2026-10-23), it failed on both backends |
| With nothing left to ask (a `uds-generic` scan of two DIDs the fake refuses, identification off), empty reads count toward quiet as in passive mode: the link goes quiet about a second after the scan, with no episode and no write, and a frame from the ECU returns it to ok | `J2534HealthTest.NothingLeftToAskGoesQuiet`. With only timed-out requests counting (checked 2026-10-24), it failed on both backends: the link stayed ok |
| The backoff grows under a sustained hard failure: at most 8 probe writes in 1.5 s, against ~15 with the old reset-to-base | `J2534HealthTest.BackoffGrowsUnderSustainedHardFailure` |
| A hard failure that ends on a silent bus reads as quiet within 5 s, with no new episode, and the probe returns to at least 6 writes a second; a few clean reads inside a hard failure end the backoff, and it starts again when the hard reads resume (at most 8 probes in 1.5 s), still one episode | `J2534HealthTest.HardFailureEndingInSilenceGoesQuietAtFullCadence`, `.BackoffRestartsWhenHardReadsResume`. Before the backoff cleared on a quiet read (checked by hand, 2026-10-23), the first failed on both backends: quiet took 26.4 s. Without the restart, the second saw 14 probes |
| Against the fake's VX mode: empty reads that hold their 100 ms and return `ERR_BUFFER_EMPTY` take a passive session quiet in no less than 950 ms (1.42 s measured, as on the bench), with no episode; answered requests keep the link ok with every write's indication dropped; on a no-ACK bus, three writes are accepted, the fourth fails into one "requests failing" episode, no later write is accepted, and close returns within 2 s of a 500 ms blocked write | `J2534HealthTest.VxQuietBusDebouncesOnBufferEmptyReads`, `.VxAnsweredRequestsKeepTheLinkOk`, `.VxNoAckBusAcceptsThreeWritesThenRequestsFail`. With the fake's read hold removed, the quiet case reached quiet in 275–315 ms and failed (checked by hand, 2026-10-22) |
| Indications are skipped without ending a read: a read that meets a TX indication returns the reply queued behind it from the same call; on the VX each reply arrives before the next request; a TX indication and a `START_OF_MESSAGE` indication queued ahead of a reply leave exactly one frame, the reply's | `J2534HealthTest.ReadPastAnIndicationReturnsTheReplyBehindIt`, `.VxReplyArrivesBeforeTheNextRequest`, `.IndicationsAheadOfAReplyAreSkipped`. With each read path stopping at an indication (checked by hand, 2026-10-23), the first failed on both backends. Against the read paths without the skip (checked by hand, 2026-10-22), the first-frame indication arrived as a second, empty frame |
| J2534 frame time: the first frame takes the host's time since attach, not the vendor's stamp; later frames the vendor's deltas, across the 32-bit wrap and across a gap of several wraps, which the host's clock counts; a vendor clock that steps back or far ahead falls back to the host's delta and never decreases | `J2534StreamTest.VendorClockAnchorsAtTheHostAndKeepsVendorDeltas`, `.VendorClockCarriesAcrossTheWrap`, `.VendorClockThatStepsFallsBackToTheHost` (session-free); `J2534HealthTest.VendorStampsBecomeSessionTime` (also cross-arch) and `J2534StreamTest.DirectLoadUriDeliversInjectedFramesExactly` through the worker. With the vendor's stamp passed through (checked by hand, 2026-10-22), all three worker cases failed. In the app under the fake's VX mode, 2026-10-22: Log times from 0 s in about 109 ms steps, against raw stamps near 2679 s |
| The backoff step (`j2534_backoff_next`, shared by all three worker modes) doubles from the base, never exceeds the 5 s cap, including the step that lands on it, holds at the cap, and doesn't wrap | `J2534StreamTest.BackoffStepDoublesToCapWithoutOvershoot` (session-free) |
| `ECUIO_ERR_REQUEST_FAILED` has its own status string | `J2534StreamTest.RequestFailedStatusHasDistinctString` |
| A failed connect says why ([ADR-0018](adr/0018-connect-failure-reporting.md)): a code per cause, and `ecuio_last_error_detail` in words, cleared by a success and kept per thread; a bad URI key, a bad value and an unknown profile each name themselves | `CoreTest.CreateFailuresNameTheirCause`, `.LastErrorDetailIsPerThread`, `.StatusStringValidValues`; the j2534 rejection tests (`J2534QueryParamTest.MalformedQueriesRejectedAtSessionCreate` and others) for the codes |
| A refused `PassThruOpen`, `PassThruConnect` or filter names the call, its status (in decimal and hex, then by its J2534-1 name or as vendor-specific, as for the VX's 34, 0x22) and the vendor's `PassThruGetLastError` text, word for word; a file that can't be a DLL is `ECUIO_ERR_LOAD_FAILED` with no spawn, and a PE the child can't load from it; the surrogate's own failures (no exe, other shared headers, a `PassThruOpen` that never returns, a DLL the child can't load) have their codes and words | `J2534ConnectTest`, `J2534SurrogateConnectTest`, `J2534SurrogateTest.BlockedVendorOpenFailsAtHandshakeTimeout`, `.DeadDllFailsFastAtOpen`, `.MismatchedFingerprintRefusesOpenBeforeVendor` |
| Below the connect path, the loader seam's helpers give each cause its own code: a NULL pointer `ECUIO_ERR_NULL_ARGUMENT`; a bad write length, ioctl length, SCONFIG array or filter shape `ECUIO_ERR_INVALID_ARGUMENT`, with no write reaching the fake; an ioctl outside the whitelist `ECUIO_ERR_NOT_IMPLEMENTED`; a refused write, `READ_VBATT`, `PassThruReadVersion` or second filter stop `ECUIO_ERR_VENDOR_REFUSED`; a surrogate that stops answering `ECUIO_ERR_SURROGATE_FAILED` | `J2534ConnectTest.HelperFailuresHaveOneCodePerCause`; `J2534SurrogateTest.TimedOutOpDoesNotDesyncNextRequest`, `.UnresponsiveChildFailsFastAfterTwoMissedDeadlines`. Each fails against the codes it replaced (checked by hand, 2026-10-23) |
| A connected session describes what it runs on ([ADR-0019](adr/0019-session-info.md)): backend, DLL architecture, protocol, baud and connect flags, channels, profile, and the adapter's battery and version readings; a refused reading leaves its flag clear without failing the connect or setting a detail; a DLL without `PassThruReadVersion` reports no versions; a mock session names its backend and profile; the struct's layout is the same in both trees | `J2534ConnectTest.InfoDescribesTheAdapter`, `.InfoCarriesTheUrisBaudAndChannel`, `.RefusedReadingsLeaveTheirFlagsClear`, `.ADllWithoutReadVersionReportsNoVersions` (also cross-arch), `J2534SurrogateTest.ReadVersionRelaysTheThreeStrings`, `CoreTest.MockSessionInfoNamesTheBackend`; `c_abi` in both trees |
| The activity log's "Session:" line follows "Connected:", with every field's wording, and "no battery reading" or "no version reading" for a refused one | `BridgeTest.SessionInfoTextNamesTheAdapter`, `.ConnectLogsTheMockSession`, `.ConnectLogsTheAdaptersSession` (the fake, through the x64 surrogate); in the app (the run-qttune driver), 2026-10-26, the x64 and x86 fakes each through its own surrogate |
| An ioctl result longer than the caller's buffer fills it and no more, and reports its whole length | `FakeFixture.IoctlCopiesAtMostItsCapacity`, `J2534SurrogateTest.IoctlCopiesAtMostItsCapacity`. Both fail against the copies they replaced (checked by hand, 2026-10-22), one of which left a short buffer untouched and the other overran it |
| The bridge returns and logs a failed connect as "<status>: <detail>", and Settings shows it in its error line | `BridgeTest.FailedConnectNamesItsCause`; in the app, 2026-10-22 (the run-qttune driver): `mock://demo?interval=10`, and the x86 fake through the app's deployed surrogate with `ECUIO_FAKE_OPEN_STATUS=34`, each in the error line and the activity log |
| A batch the vendor returns with `ERR_TIMEOUT` is delivered through the surrogate; an empty one maps to a vehicle timeout; both reads hold for their 50 ms timeout | `J2534SurrogateTest.SpecTimeoutReadDeliversPartialBatch` (spec-mode fake) |
| A vendor that returns a hard error with the message count untouched delivers no frames: the session reports a hard failure, dispatches nothing and recovers once reads are honest; through the surrogate, the next request after the poisoned read still correlates | `J2534HealthTest.UntouchedCountUnderHardErrorReportsUnhealthy`, `J2534SurrogateTest.HardErrorUntouchedCountDeliversNoFrames` (`ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`) |
| Signal freshness: `ageMs` is -1 before the first sample and a sane clock delta after; `updateValues` notifies `AgeMsRole`; `refreshAges()` notifies `AgeMsRole` alone, over every row, so a stalled row re-evaluates | `BridgeTest.AgeMsRoleEmittedAndRefreshesWhileStalled` |
| Link-state mapping: `ECUIO_OK` → `ok`, vehicle timeout → `quiet`, `ECUIO_ERR_REQUEST_FAILED` → `requests`, hard failure → `fault`, `ECUIO_ERR_RECORDING_CHANGED` → `stopped`, any other code → `degraded` | `BridgeTest.LinkStateMapsEveryPublishedCode` (session-free, through `QtTuneBridge::linkStateForError`) |
| Theme's link-state color and label for every state the bridge publishes, with an unknown state shown as degraded; the chart trace colors are distinct, and none is a saturated hue within 20° of the danger color | `QmlTest.theme` |
| Live's trace styles are keyed by model row: colors from `Theme.seriesColors`, solid within the palette, and past it no two rows that share a color share a dash. Under `mock://demo` each row's swatch shows its row's trace color, filled while plotted and hollow while not, the other rows unchanged. The banner's message for each link state, an unknown one as degraded | `QmlTest.liveview` |
| The strip chart's layout, on synthetic series: nothing to draw without a time span; the plot leaves room for the widest label, capped at 40 % of the width; each trace scales to its own range, widened to a minimum span of displayed digits (two units over a twenty-unit span take a tenth of the height, 0.4 V of 2.0 V a fifth), while a trace that never moves by half a digit takes a lane of its own; a gap past the threshold breaks a trace into segments and a shorter pause doesn't; the time ticks step 1, 2 or 5 s back from the newest sample, with no caption where there's no room; value labels are nudged a line apart and kept inside the plot; more than fit close up to the text's height, and past that as many as fit are labeled, picked evenly down the stack with its ends kept, while every trace still draws | `QmlTest.chartlayout` (`ChartLayout`, no bridge). The closing up and the thinning each failed against its mutation, and so did keeping the first labels instead of an even pick (2026-10-25) |
| `SignalHub` finds a signal by the first of a `Tap`'s names the set provides, else null, mirrors the model's values, and follows a profile switch. Each Dashboard gauge reads its signal by the name the mock and `obd2` sets share. It says "no session", then "waiting for data" until the first 100 ms batch, then nothing, with the value at the signal's decimal places in its unit; after a switch to `?profile=raw`, "not in profile" | `QmlTest.dashboard` |
| A Dashboard gauge's peak and low holds take each 100 ms window's highest and lowest, not its newest value: the engine-start fixture replayed at 100× leaves the tach holding the recording's 1,261.25 rpm | `QmlTest.dashboard`; it failed with the holds reading the newest value (2026-10-26) |
| Every link-health surface in the app's window (`Main`), in color and words: the header pill, Home's status card, the Dashboard header, Live's banner, Settings' status label and the activity log's link line. Disconnected and ok (`mock://demo`) everywhere; quiet, fault and requests through the fake with its knob armed; a disconnect after a fault shows plain disconnected again. The pill and the Dashboard header show a direct-load URI cut to its DLL's file name (`j2534://…/fake_j2534.dll`, the query kept), Settings the whole URI | `QmlTest.linkhealth` (the fake's cases on Windows); the cut itself, `BridgeTest.UriLabelCutsADirectLoadPathToItsFileName`. **Both failed with the label passing every URI through** |
| A binding error in the app's QML fails the page tests: they treat any warning from `qrc:/qt/qml/QtTune/` as a failure. That catches a removed delegate's binding, such as a Live row's swatch reading the palette at index -1 while a connect resets the model | `QmlTest.liveview`, `.dashboard`, `.linkhealth`, `.settings` (`failOnWarning`) |
| Dashboard: every gauge the mock set feeds sweeps under `mock://demo`; a gauge whose signal the profile lacks reads "not in profile" (the mock set feeds every gauge, so `?profile=raw` is what shows it); the grid collapses from 4 to 2 columns across 760 px and tightens its gaps at 600 px; fault and quiet states tint correctly; the temperature rings start at 0 °C, so a cold engine reads on the scale; unticking a row on Live leaves its gauge live; when the stacked arrangement would overflow a short, wide page it switches to the wide one (dials and a 2 × 2 tile block in one row, sized to the height), and the scroll bar shows while the page overflows | Manual smoke, 2026-10-20 (`40e51ce`; quiet and fault through the knob recipe below). The header, which takes its dot and label from `Theme`, was re-run 2026-10-21 (`55373f4` plus the reordered series palette): ok ("Live · mock://demo", green), quiet, requests, fault and disconnected, plus "not in profile" on every gauge under `?profile=raw`. Re-run later on 2026-10-21 for the rings and the untick (working tree on `d1c20c0`): coolant 53 °C and oil 24 °C on 0–130 and 0–160 scales under mock; with RPM unticked the tach kept moving and recorded a 7,000 peak; every gauge found its signal under `obd2` through the x64 fake. Re-run 2026-10-22 for the arrangements (`0a94049`, the run-qttune driver's `resize`): the wide one at 1280 × 650 (a 1080p laptop at 150 %) and 1280 × 800 with every gauge on screen, stacked at 1700 × 980 as before, narrow at 860 × 640 and 600 × 700 with the scroll bar shown. `SignalHub`'s lookup and each gauge's note are `QmlTest.dashboard`'s too (above); the gauges' drawing and the arrangements are manual only |
| Live: the list sizes to its rows up to a cap, then scrolls with the bar pinned on; each row's swatch matches its trace; rows past `Theme.seriesColors` reuse its colors with a dashed trace (under `obd2`, the last two rows); values at each def's decimal places; the chart prints each trace's value at its right-hand end, nudged apart, with a sample of its stroke, has a seconds axis, and gives each flat trace its own lane in list order; disconnected, a dim message and a Connect button | Manual smoke, 2026-10-21 (`55373f4` plus the reordered series palette): all eight mock rows fit and each swatch matches its trace; under `obd2` through the x64 fake, ten rows with the scroll bar pinned on, and MAF drawn dashed in row 1's color. Re-run later on 2026-10-21 (working tree on `d1c20c0`): `745 RPM`, `49 °C`, `0.0 %`, `7.0 g/s` under mock, with the three flat mock traces in separate lanes; under the fake, all ten flat traces in their own lanes, top down in list order, Battery Voltage at `13.87 V`, and the two dashed rows' labels showing the dash. The swatches, row colors and dashes are `QmlTest.liveview`'s too (above). The chart's drawing (traces, labels, lanes, the time axis) is manual only; `BridgeTest` pins the `row` and `decimals` it keys on |
| Settings activity-log lines carry local `HH:MM:SS.mmm` stamps; after Disconnect and a new connect, the Log page holds only the new session's frames | Manual run, 2026-10-22 (working tree on `c029caa`, `mock://demo`, the run-qttune driver) |
| Recording end to end through the x86 surrogate: Settings shows "Recording to Documents\QtTune\recordings\…" with no account name, and the switch is disabled while connected; the folder held 55 frames and 55 values from the fake; that spawn's session log read "frame logging on" and carried its frame lines | Manual run, 2026-10-22 (working tree on `839b176`, the x86 fake through the app's deployed surrogate, the run-qttune driver; the test recording deleted afterwards). On the OBDX Pro VX, 2026-10-22 (`0a8beae`): one recording per connect across six connects, and frame logging on in each spawn's session log ([sheet](bench-sessions/2026-10-22.md)) |
| App chrome: the header holds the name, a link pill (state and URI, a direct-load path cut to the DLL's name; eliding first) and one button (Connect… opens Settings; Disconnect); the sidebar is one full-height panel; below 720 px the tab bar underlines the selected tab; Settings sizes the device list to its rows and gives the activity log the rest, and on a short window scrolls, the log keeping a 200 px minimum; Log shows `s.mmm` times, spaced uppercase payloads and a full 29-bit ID, and Pause holds the table while the count keeps rising | Manual smoke, 2026-10-21 (working tree on `d1c20c0`, `mock://demo` and the x64 fake, the run-qttune driver), at 1280 and 400 px; Settings re-run 2026-10-22 at 1280 × 650 (`0a94049`). Not checked: keyboard focus rings (the driver sends no keys) and an OS light color scheme |
| Zoom: Settings' Display row steps `Theme.zoom` from 50 % to 200 %; the whole UI reflows at the zoomed size, a badge shows the level, and the zoom survives a restart | Manual smoke, 2026-10-22 (`0a94049`, the run-qttune driver): two steps down to 80 % reflowed the Dashboard at the larger layout size, its tiles showing their descriptions, a relaunch came back at 80 % (`HKCU\Software\QtTune\QtTune\ui`), and Reset showed "Zoom 100 %". Not checked by the driver: the Ctrl + / Ctrl − / Ctrl 0 shortcuts (it sends no keys) and the window's opening size on a 1280 × 720 screen |
| Live's strip chart stays sharp under zoom: its Canvas paints at the zoomed size and is scaled back, so its labels and traces read as crisp as the list's text; nine labels in a short plot stay inside it | Manual smoke, 2026-10-25 (the run-qttune driver, 1600 × 900, all nine mock signals plotted). At 200 % on a 100 % monitor, before the change, the chart's labels were visibly blocky beside the list's values and the top label was cut at the plot's edge; after it, at 200 % on a 150 % monitor, the labels match the list's sharpness and sit inside the plot, and at 100 % all nine show a line apart |
| On a short page (a small window, a large zoom) Live's list gives up rows, down to one, so the chart stays whole on the page at its minimum height or more; a tall page shows every row | `QmlTest.liveview` (`test_chartStaysOnAShortPage`, pages 330, 450, 600 and 1000 px tall under mock). With the list's earlier caps (2026-10-25) the chart ended at 379 px on the 330 px page and past the margin on the 450 px one. Manual smoke the same day at 1600 × 900: at 200 % four rows and a scroll bar above the whole chart, time axis included; at 100 % all nine rows |
| QML reaches the bridge as the `Bridge` singleton (`app/bridgesingleton.h`), not a context property: every page binds through it, and qmllint resolves its properties | Manual smoke, 2026-10-21: `mock://demo` streaming on Home, Dashboard, Live and Log, and the x86 fake through the app's own deployed surrogate (2000 RPM decoded, the spawn line naming the exe). `all_qmllint` went from 103 `[unqualified]` warnings to 7, all on LogPage's delegate roles |
| Link-health UI: quiet/fault/degraded banner, activity-log transitions, "—" cells, stale dimming, the arbId column | The banner, the other surfaces and the log lines are `QmlTest.linkhealth`'s (above); Live's "—" cells, stale dimming and the arbId column are manual only. Manual smoke, 2026-10-20 (fake quiet/fault via `ECUIO_FAKE_READ_STATUS`; mock; recipe below). The arbId row ran through the x86 surrogate. |
| Fault state on real hardware, clean teardown; 4-byte filter shape accepted with logged vendor status 0 | Informal bench observation 2026-10-20 ([sheet](bench-sessions/2026-10-20.md)), under that build's link rules; under the current rules the same no-ACK setup latches `ECUIO_ERR_REQUEST_FAILED` (next row). |
| Requests-failing and quiet states on real hardware through the x86 surrogate: the banner at the first failed write, one episode, the backoff to its 5 s cap; quiet after ten empty reads (`ERR_BUFFER_EMPTY`) under `?profile=raw`; clean teardown on every connect | BENCH run on the OBDX Pro VX, 2026-10-22 (`0a8beae`, [sheet](bench-sessions/2026-10-22.md)) |
| The `obd2` set on a live vehicle through the x86 surrogate: its rotation, `?poll=` and the three supported-PID bitmaps answered by the engine ECU; the values, decoded a second time from the logged bytes with the J1979 formulas, match the display, and RPM matched the tachometer; ok, quiet and recovery through an ignition cycle | Vehicle session, 2026-10-22 ([sheet](bench-sessions/2026-10-22-vehicle.md)) |
| The v0.3.1 release zip, as a user gets it: its SHA-256 matches `SHA256SUMS.txt`, `gh attestation verify` passes, and after SmartScreen's "Run anyway" the extracted `qttune.exe` runs `mock://demo`; with no VX attached, connecting `j2534://OBDX Pro - VX` fails with "the adapter's driver refused: PassThruOpen returned status 34 (vendor-specific)" (v0.3.1's wording); with the VX on USB and no car, the session opens through the x86 surrogate (battery 0.0 V, the VX's firmware and DLL versions), the supported-PID query goes unanswered and the link goes to requests failing; disconnects are clean | BENCH, 2026-10-25, the owner's `-rc1` dry run and the `v0.3.1` download (the activity log read back in the session, not committed), on the dev machine, which has Visual Studio. The owner also ran the `v0.3.1` zip on a PC without Visual Studio: the simulated vehicle drove, the Log page showed frames and Live filled with mock values; no adapter was connected there (Gaps) |

## Manual smoke — link-health UI

`QmlTest.linkhealth` runs the quiet, fault, requests and
disconnect-reset rows below on every surface, through the x64
surrogate. The recipe covers the rest: the x86 surrogate, the arbId
column, dimming, a slow teardown, and how it all looks. Recipe — run
the app from a shell so the session inherits the knobs at spawn (the
child snapshots the environment, so a knob changed in it mid-session
doesn't reach a running session):

| Goal | Environment | URI | Expected |
|---|---|---|---|
| Quiet banner | `ECUIO_FAKE_READ_STATUS=9` | `j2534://<fake.dll>?profile=raw&filter=0x7E8` | Amber "Vehicle not responding" after ~1.2 s; no frames, empty signal list (raw profile) |
| Fault banner | `ECUIO_FAKE_READ_STATUS=7` | same | Red "Adapter fault" banner with episode count; the knob holds for the session: the child took the environment at spawn |
| Requests banner | `ECUIO_FAKE_WRITE_STATUS=7` | `j2534://<fake.dll>` (bare: the `obd2` rotation keeps writing) | At the first failed write: amber "Requests failing — check the vehicle connection and ignition" on LiveView, "Requests failing" with an amber dot on Home and the Dashboard header, an amber Settings label, and "Link: requests failing (1 episode(s))" in the activity log. **Verified 2026-10-21** (x64 fake): every surface as listed; values stay "—" |
| Frames + arbId column | — | `j2534://<fake.dll>?profile=raw&filter=0x7E8&poll=0x0C` | Frames on LogPage with `0x7E8` in the ID column, DLC 4, payload `41 0C 1F 40` (2000 RPM canned); link ok, no banner. **Verified 2026-10-20** using the Win32-tree fake (`build-win32/tests/Release/fake_j2534.dll`). The session log confirmed it ran through the x86 surrogate, so this row also covers surrogate routing, the strip/stamp path and poll mode in the app. It doesn't exercise `?poll=` outranking a profile's rotation: `raw` has none. |
| Dimming baseline | — | `mock://demo` | Values stream at full opacity, steady ones (IAT) included, because every mock signal arrives every 50 ms. Only a signal that stops arriving dims, 3 s after its last sample. **Verified 2026-10-20:** nothing dims on mock. |
| Disconnect reset | after any of the above | Disconnect | Home dot and Settings label show plain disconnected, not the last session's fault/quiet |
| Slow teardown | `ECUIO_FAKE_DISCONNECT_DELAY_MS=6000` | `j2534://<fake.dll>`, then Disconnect | For the 6 s: "Disconnecting…" on the header pill, the Settings label and button, LiveView's status line and the header button; Connect and the presets disabled; `Disconnecting:` in the activity log. Then "No session", Connect enabled, `Disconnected:` in the log, and a reconnect works. **Verified 2026-10-21** with the x64 fake, and through the x86 surrogate (`-x86` fake, a 20000 ms stall): the session log marked the child unresponsive, `Disconnected:` followed about 6 s after `Disconnecting:`, and the window answered UI Automation throughout |

Every row has passed in the app: the quiet, fault, arbId, dimming and
disconnect rows on 2026-10-20, and the requests and slow-teardown rows
on 2026-10-21, with the quiet, fault and disconnect rows re-run against
the current Home, Settings and Dashboard header. On real hardware the
OBDX Pro VX has shown the fault state
([bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)) and the
requests and quiet states
([bench-sessions/2026-10-22.md](bench-sessions/2026-10-22.md)).

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
- **The release zip has run on a PC without Visual Studio only on
  mock data.** There, v0.3.1's zip ran `mock://demo`, and frames and
  Live's values arrived. No adapter has been connected on such a PC,
  so neither surrogate has run on one. v0.3.2's zip, which adds the
  x64 surrogate, hasn't run there at all.
  Each surrogate links the C runtime statically, and `stage.ps1`
  checks that every DLL `qttune.exe` imports ships in the zip or is
  part of Windows, but both checks run on the dev machine.
- **GCC and Clang warnings are seen only in CI logs.** Every target
  builds with warnings on, and the local MSVC count is 0, but no GCC
  or Clang build runs on the dev machine and warnings don't fail CI.

- **The x64 surrogate has no real-hardware coverage.** The one J2534
  adapter on hand, the OBDX Pro VX, ships an x86 DLL, so real vendor
  code has run only through the x86 surrogate. The x64 one, which every
  x64 vendor DLL would load in, has met only the x64 fake (in CI on
  every push).
- **Real-hardware coverage is one adapter and one vehicle.** The
  quiet-bus BENCH sessions (2026-10-16, 2026-10-20, 2026-10-22,
  2026-10-25) and both vehicle sessions on one car (2026-10-22,
  2026-10-25: [session sheet](bench-sessions/2026-10-25-vehicle.md))
  all ran the OBDX Pro VX through the surrogate, and the car's engine
  ECU answered on 11-bit `0x7E8`. Still unverified against real
  hardware: 29-bit addressing, more than one ECU answering, and any
  other vendor's statuses (the VX never returned `ERR_TIMEOUT`).
- **The request engine has met one ECU.** On the car (2026-10-25) it
  paced Service 01 at about 26 requests a second with every reply but
  one in 7,196 in before the next request, and Service 0x22 at about
  13. Two channels at once, a "response pending" reply and a late
  negative reply are tested against the fake only: the car answered
  from one ECU, sent no `78`, and refused only in step.
- **The probe has met one car.** It walked six bitmaps there and
  dropped only `01 5C`, as the first session's bitmaps predicted, and
  a cold connect dropped nothing. A car that refuses the bitmap
  request itself, or answers it with a negative reply, hasn't been
  seen.
- **`uds-generic` has met one ECU's default session.** The scan of
  `F180-F1FF` and three text DIDs ran on the car; every refusal was
  `7F 22 31`. Other refusal codes (`11`, `33`), an `<ecu>:` prefix on
  a second ECU, and DIDs outside the identification range are tested
  against the fake only.
- **Identification has met one ECU.** On the car every item settled
  (four Service 09 items and two DIDs answered, seven DIDs refused),
  and its replies of up to 27 bytes arrived whole. An ECU with several
  calibration IDs, or one that leaves an item unanswered while awake,
  hasn't been seen.
- **Link health is covered against the fake.** The tests above drive
  `ECUIO_FAKE_READ_STATUS`, `ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`,
  `ECUIO_FAKE_WRITE_STATUS` and `ECUIO_FAKE_WRITE_ZERO_COUNT` through
  the worker, in the surrogate. Not
  covered:
  - a hard read failure from real hardware, beyond one. Live runs
    drove every state through the surrogate on the VX: requests
    failing at the bench, ok, quiet and recovery on the vehicle, and
    on 2026-10-25, after the laptop resumed from hibernation, a read
    returning vendor status 27 ("adapter fault", the backoff at its
    cap, a clean disconnect). A failure of that kind mid-session,
    with the adapter still enumerated, hasn't been seen;
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
  its outputs untouched on error is modeled
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
- **Discovery sees a seeded registry only in CI's coverage job.** On a
  machine with no vendors installed the discovery test checks only the
  shape of an empty result; `scripts/verify.sh` sets
  `ECUIO_EXPECT_J2534_DEVICES=1` on Windows so the dev machine's
  installed driver has to be found. `win-coverage` registers the fake
  DLL in both registry views (`REG_EXPAND_SZ`, an environment-variable
  path) beside three entries discovery must skip (an empty
  `FUNCTIONLIBRARY`, a number, a relative path) and demands it, but a
  test failing under the coverage tool shows only in the job's summary
  line, not as a failed job; `win-build` has no
  vendors and never sees a non-empty result.
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
  software renderer and check what each item shows: its color, text
  and visibility. The strip chart's layout is arithmetic the tests run
  (`ChartLayout`), but its drawing, the gauges, layout at each
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
  `InjectFrame` and its control page, `ecuio_j2534_inject_test_frame`)
  are never called by app code.
- **When you change shared plumbing, pin the contract you changed, at
  the point you changed it.** For example,
  `DefaultConnectUsesIso15765At500k` was added alongside the query
  parameters.
