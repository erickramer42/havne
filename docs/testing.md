# Testing

What is tested, how, and what isn't. The table below has a row for each area of the product and a column for each verification tier; the sections after it say what each tier is, list the suites and keep the coverage. The rows of what is verified are in [testing-inventory.md](testing-inventory.md), and the performance measurements in [performance.md](performance.md). For build and run commands, see [development.md](development.md#running-tests); for the current test count, [STATUS.md](../STATUS.md).

## Areas against tiers

A mark means the tier runs for that area: `yes` (a few words say where, if it is not everywhere), `-` (the tier could reach the area and doesn't), and `n/a` (the tier doesn't apply by its definition). The tiers are defined in [Verification tiers](#verification-tiers).

| Area | Unit | Bridge | QML | HARNESS | Cross-arch | Fuzzing | Simulator smoke | Manual smoke | BENCH | Vehicle | Details |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core, the C ABI and the sanitizers | yes | n/a | n/a | n/a | n/a | yes | n/a | n/a | n/a | n/a | [Suites](#suites), [What is verified](#what-is-verified) |
| Mock and replay | yes | yes | yes | n/a | n/a | yes (replay reader) | n/a | yes (`mock://demo`) | n/a | n/a | [What is verified](#what-is-verified) |
| ELM327 | yes (protocol) | yes | yes (URI and Bluetooth picker cases) | yes (every desktop platform) | n/a | yes | n/a | yes (serial-port picker) | yes (VX in ELM mode) | yes (MX+, VX; one car) | [Suites](#suites), [Gaps](#gaps) |
| SocketCAN | yes (no bus) | yes | yes (Settings) | vcan (Linux CI) | n/a | yes | n/a | n/a | yes (desk bus) | yes (listening, one car) | [Suites](#suites), [Gaps](#gaps) |
| J2534 and the surrogate | - | - | yes (Windows, link states) | yes (Windows, x64) | yes (dev machine) | yes (the pipe's replies) | n/a | yes (link-health recipe) | yes (VX, x86 DLL) | yes (VX, one car) | [Manual smoke](#manual-smoke-link-health-ui), [Gaps](#gaps) |
| Decoding, signal files and plugins | yes | yes | yes | n/a | n/a | yes | n/a | - | - (the bench sessions had no bus) | yes (one car) | [What is verified](#what-is-verified) |
| Request engine and diagnostics (probe, identification, trouble codes, clearing, extended session) | yes | yes | yes (diagnostics page) | yes (the fakes' ECU) | n/a | yes (engine, ISO-TP, replies) | n/a | - | yes | yes (one ECU, or two, per feature) | [Gaps](#gaps) |
| Recording, exports and imports | yes | yes | yes | n/a | n/a | yes (rows, folders, candump) | n/a | - | yes (recorded on the sessions) | yes (log over ELM327 only) | [What is verified](#what-is-verified) |
| Viewer | yes | yes | yes | n/a | n/a | - | n/a | - | n/a | n/a | [What is verified](#what-is-verified) |
| Live and Dashboard | yes (layout file corpus) | yes | yes | n/a | n/a | - | yes (screenshots) | yes (gauges, strip chart) | n/a | n/a | [Gaps](#gaps) |
| Conditions and alerts | yes | yes | yes | yes (Windows, fake DLL) | n/a | yes (condition files) | n/a | - | - | - | [What is verified](#what-is-verified) |
| `havne-cli` | yes | n/a | n/a | n/a | n/a | - | n/a | n/a | yes (desk-bus sessions) | yes (the sessions ran it) | [Suites](#suites) |
| Python bindings and the wheel | yes (pytest through ctest) | n/a | n/a | n/a | yes (the x86 shared core's exports) | - | n/a | n/a | n/a | n/a | [Suites](#suites), [Gaps](#gaps) |
| Windows, Linux and macOS builds and the release zip | yes | yes | yes | yes | yes (Windows) | n/a | n/a | yes | n/a | n/a | [Coverage](#coverage), [Gaps](#gaps) |
| Android and iOS | - (cross builds set `BUILD_TESTING=OFF`) | - | yes (at a phone's sizes, on the desktop) | n/a | n/a | n/a | yes (iOS, on request) | - | n/a | yes (Android phone, by hand, one car) | [Gaps](#gaps) |

## Gaps

What isn't verified, and why it stays that way. Each line is the gap and the reason. The evidence and dates are in [Gaps in detail](testing-inventory.md#gaps-in-detail).

**Hardware and vehicles**
- Real hardware has met the adapters on hand and one car: no other vendor's J2534 driver, no second car, no 29-bit addressing, no BLE adapter. They aren't on hand.
- The x64 surrogate has run only against the x64 fake: the one J2534 adapter on hand ships an x86 DLL.
- Each diagnostic feature has met one ECU, or two in one session: identification, trouble codes (none stored), clearing (none stored), the probe, `uds-generic` (default session), the extended session, listening beside another tester (Known Issue #41). The car is one.
- "Response pending" and other refusal codes are tested against the fakes only: the car never sent them.
- A plugin hasn't met a car, and the analysis log has run on a car over ELM327 only, not J2534 or SocketCAN.
- Another vendor's J2534 read and write statuses, and a hard read failure mid-session with the adapter still enumerated, haven't been seen: only the VX is on hand.

**What CI can't run**
- The mobile apps have no automated run on a device or emulator (Known Issue #2): cross builds skip tests; the Android app is checked by its run sheet, the iOS app by the Simulator smoke, which nothing taps through.
- Android's document picker, and a provider that fails only after it holds the whole document, run only on a phone.
- A sleep in session time has run on Android only: no CI runner or WSL VM can sleep.
- The Win32 serial port has no automated test: Windows has no built-in virtual COM pair, so it is checked at BENCH and on the vehicle.
- CI never launches the desktop app, and the QML tests read properties, not pixels: drawing, gauges, layout at each width, focus rings and the GPU backends are manual smoke only.
- The cross-arch configuration never runs in CI: it runs on the dev machine.
- The release zip has run on a PC without Visual Studio only on mock data; no adapter has been connected on such a PC.
- ThreadSanitizer, the Python notebook example and Discovery's registry read (it needs elevation) run by hand or not at all: no CI job carries them.

**Test seams and code**
- The surrogate pipe's protections are tested on the pipe alone: nothing connects as another user or looks for a handle leaked to the child.
- The fake is simpler than J2534: its default read model, no segmented replies, no filter gating.
- The scrub can't see a VIN sent raw in frames that aren't ISO-TP, and can refuse a folder copy whose marker label looks like a VIN.
- The Python declarations' signedness isn't checked: the generated types are held to the headers' sizes and offsets only.
- Warnings fail a build only where `HAVNE_WARNINGS_AS_ERRORS` is set (CI's GCC and Clang jobs): MSVC's never do, and a runner's newer compiler can fail a commit that passed.

## Verification tiers

| Tier | Meaning | Where it runs |
|---|---|---|
| **Unit** | Core, signal and ABI logic with no I/O | CI on all three desktop platforms |
| **Bridge** | Qt bridge and models, headless (`Qt6::Core` only) against the real 50 ms mock worker | CI on Windows, macOS and Linux, and locally |
| **QML** | The app's QML module against a real bridge (Qt Quick Test, `test_qml`), offscreen on the software renderer. Pages are built and bound as in the app, and checked through their properties (color, text, visibility), not their pixels | CI on Windows, macOS and Linux, and locally. The link states past ok need the fake vendor DLL, so they run on Windows only |
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL; the ELM327 transport against the in-tree fake ELM327, behind its port opener, a loopback socket, an app stream and (Linux, macOS) a pseudo-terminal | J2534: CI on Windows (same-arch, all x64). ELM327: CI on every desktop platform |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Fuzzing** | libFuzzer harnesses, one for each parser of input from outside and one stateful harness (a requesting session's engine and ISO-TP against an ECU on a simulated bus), all run under ASan (and UBSan with Clang) and checking their contracts on every input. Not ctest cases: a time-bounded fuzzer would make a green suite stop meaning green. [fuzz/README.md](../fuzz/README.md) lists each harness and what it checks | CI `fuzz` job (Linux, Clang), 60 s per harness (the JSON, signal-file, plugin and OBDb harnesses started from the seeds `fuzz/seeds.py` builds from the committed fixtures), crashing inputs uploaded; locally with MSVC's libFuzzer in a tree of its own |
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on request: a manual run with `ios_smoke` ticked (`smoke_only` instead skips the other platform jobs), or `[ios-smoke]` in the commit message (`ios-simulator-smoke`, Intel macOS runner) |
| **Manual smoke** | Running the app: layout, Dashboard gauges, LiveView, strip chart, Settings picker against the live registry and the machine's serial ports | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | Sessions on the laptop, the first complete on 2026-10-16 with a quiet bus; the sheets and what each confirmed are in [bench-prep.md](bench-sessions/bench-prep.md) |
| **Vehicle** | A live vehicle session | Sessions on one car, its engine ECU on `0x7E8`, from 2026-10-22; the sheets are in [bench-prep.md](bench-sessions/bench-prep.md), and the run sheets are [vehicle-runsheet.md](bench-sessions/vehicle-runsheet.md) and [elm327-vehicle-runsheet.md](bench-sessions/elm327-vehicle-runsheet.md) |

The smoke tiers earn their place: they have caught bugs no C++ test
could see. A strip-chart indexing bug lived in QML paint code, and the
iOS Simulator's narrow window showed an off-center status card and a
tab bar that named the Settings page "Setup".

`scripts/verify.sh --changed` picks the steps from the paths a branch
changes, and `--suite` runs named suites, to shorten the edit-and-check
loop. Neither counts as verification: the table in
`scripts/verify_scope.py` is conservative, but only the plain
`scripts/verify.sh` runs every tier the change could reach.
`VerifyScopeTest.SelfTest` pins that table. `scripts/docs_only.py`'s
shorter list, which decides that CI runs no tests for a change
([ci.md](ci.md)), has its own `DocsOnlyTest.SelfTest`, which also holds
it inside that table.

## Suites

| Binary | Suites | Needs |
|---|---|---|
| `test_core` | `CoreTest`, `VinTest`, `JsonTest`, `RecorderTest`, `MergeSessionTest`, `RecordingRowsTest`, `RecordingTest`, `ConditionSessionTest`, `ConditionRecordingTest`, `ArmedRecorderTest`, `CheckRulesTest`, `RecordingCheckTest`, `TempNameTest`, `ObdbImportTest`, `Sha256Test`, `ScrubFramesTest`, `CsvExportTest`, `FolderExportTest`, `HealthTest`, `CsvImportTest`, `CsvGuessTest`, `QueueTest`, `RecordingFramesTest`, `FramesReadTest`, `DefIndexTest`, `CandumpTest`, `ImportTest`, `ScrubTest`, `ExportTest`, `BenchLoadJudgeTest` (the load test's floors and judge) | Nothing (Qt-free) |
| `test_signals` | `SignalDecodeTest`, `SignalsTest`, `SignalTablesTest`, `ComputedTest`, `SignalFileTest`, `SignalFileFormat3Test`, `SignalNeedsTest`, `MergedTextTest`, `MergeTest`, `PluginTest`, `PluginWrapTest`, `ConditionFileTest`, `ConditionEvalTest` | Nothing (Qt-free) |
| `test_c_abi` (ctest name `c_abi`) | Every public header compiles as ISO C11 with extensions off (`-pedantic` on GCC and Clang) and links unmangled; layout asserts | A C compiler |
| `test_c_abi_shared` (ctest name `c_abi_shared`) | `tests/test_c_abi*.c` again, linked against the shared core: each call resolves through its export table (internal hooks left out) | The shared core (`HAVNE_SHARED_CORE`, off on Android and iOS) |
| `test_bridge` | `BridgeTest`, `RecordingViewTest`, `ReadOnceTest`, `FrameFilterTest`, `ObdbImportTest`, `RecordingExportTest`, `ViewerFramesTest` | Qt Core and Qt Network |
| `test_qml` | One ctest case per `tests/qml/tst_*.qml` file, `QmlTest.<name>` (`QmlTest.theme` for `tst_theme.qml`); a theme split over several files (`tst_dashboard{,_pages,_panel}.qml`, `tst_driveview{,_sheet,_platform}.qml` and the like) is one entry for each file, so a citation of `QmlTest.dashboard{,_pages,_panel}` or `QmlTest.driveview{,_sheet,_platform}` in this document names the family's entries. `tst_dashedit.qml` and the `tst_dashedit_*.qml` files are one entry each (`QmlTest.dashedit`, `QmlTest.dashedit_panel`, ...). `tst_dashedit*.qml`, `tst_settings*.qml`, `tst_phone_*.qml`, `tst_recordings*.qml`, `tst_dashboard*.qml`, `tst_dashboardbook*.qml`, `tst_driveview*.qml`, `tst_chartlayout*.qml` and `tst_liveview*.qml` derive from a kit file (`DashEditKit.qml`, `SettingsKit.qml`, `PhoneKit.qml`, `RecordingsKit.qml`, `DashboardKit.qml`, `DashboardBookKit.qml`, `DriveViewKit.qml`, `ChartLayoutKit.qml`, `LiveViewKit.qml`) that holds their shared helpers (`tst_phone_<part>.qml` is the entry `QmlTest.phone_<part>`); a kit's name never starts with `tst_`, so it is not an entry. | Qt Quick; the quiet, fault and requests cases need the fake DLL (Windows) and skip elsewhere |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_health` | `J2534HealthTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_connect` | `J2534ConnectTest`, `J2534SurrogateConnectTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_surrogate` | `J2534SurrogateTest` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_routing` | `J2534RoutingTest` | Windows; the cross-arch case skips unless `ECUIO_FAKE_DLL` is set |
| `test_sandbox` | `WamrBuildTest`, `WasmCheckTest`, `SandboxTest` | Nothing (Qt-free, internal headers); the cross-arch step runs it against the x86 core |
| `test_obd2` | `Obd2SetTest`, `Obd2DecodeTest`, `Obd2MuxTest`, `Obd2ReplyTest`, `RequestEngineTest`, `UdsGenericTest`, `Obd2TableTest`, `FakeEcuTest`, `DiagnosticsTest`, `ClearTest`, `DtcTitleTest`, `InspectionTest`, `SimEcuTest`, `SimDidsTest`, `SimAskTest`, `SimRecordingTest`, `PidSupportTest`, `RequestTimingTest`, `SessionControlTest` | Nothing (Qt-free) |
| `test_elm327` | `Elm327ProtocolTest`, `Elm327SessionTest`, `Elm327LogTest`, `SerialPortsTest`, `Elm327TcpTest`, `Elm327StreamTest`, and on Linux and macOS `Elm327PortTest` | Nothing (Qt-free) |
| `test_vcan` | `VcanTest`, `SocketCanBusTest`, `SocketCanRequestTest`, `SocketCanFaultTest`, `IsoTpConformanceTest`, `SimBusTest` | Linux, `vcan0` up and `can-isotp` loaded; each case skips without them, and fails under `ECUIO_VCAN_REQUIRED=1` (CI's `linux-build`) |
| `test_socketcan` | `SocketCanTest`, `IsoTpTest`, `SimTest` | Nothing (Qt-free), every platform |
| (ctest only) | `GoldenTest.SelfTest`, `AbiReportTest.SelfTest`, `ApiCheckTest.SelfTest`, `VpicWmiTest.SelfTest`, `DtcTitlesTest.SelfTest`, `RecordingCompareTest.SelfTest`, `SchemaKeysTest.EachObjectsKeysAreItsLoaders`, `ExportsTest.SelfTest`, `PlainTextPinTest.SelfTest`, `PlainTextPinTest.EveryTextSetsItsFormat`, `TidyRatchetTest.SelfTest` (the size baseline's ratchet), `FileLengthTest.SelfTest` (`scripts/filelength_check.py`, the file-length baseline; skips without git) | Python 3 (left out without it) |
| `test_cli` | `CliLineTest`, `CliRingTest`, `CliSleepTest`, `CliDiagnosticsTest`, `CliVinTest`, `CliDtcTest`, `CliInspectionTest`, `CliSimTest`, `CliMarkLinesTest`, `CliViewTest`, `CliInfoTest`, `CliSignalFileTest`, `CliPluginTest`, `CliHealthTest`, `CliScanTest`, `CliClearTest`, `CliCanTest`, `CliTimingTest`, `CliRefusedTest`, `CliRecordingSignalsTest`, `CliViewFramesTest`, `CliElm327Test`, `CliTransferTest`, `CliPartialTest`, `CliFramesTest`, `CliSessionTest` | Nothing (Qt-free) |
| (ctest only) | `CliGoldenTest`, `CliUsageTest`, `CliSignalTest`, `CliReplayTest`, `CliRecordTest`, `CliViewRunTest`, `CliObdbImportTest`, `CliPluginRunTest`, `CliElm327RunTest`, `CliClearRunTest`, `CliTransferRunTest`, `CliHealthRunTest`, `CliExitTest`, `CliConnectTest`, `CliPathTest`, `CliRefusedRunTest`, `SchemaFixturesTest` | `havne-cli` run as a child process; `CliTransferRunTest` needs Python 3 |
| (ctest only) | `PythonTest.<file>`, one per `python/tests/test_*.py` | The `havne` package under pytest (exit 77, a skip, without pytest), against the tree's own shared core |
| (ctest only) | `WheelTest.SelfTest`, `WheelTest.Installed` | Python 3, and for `WheelTest.Installed` a 64-bit tree with the shared core that is not a sanitizer tree |
| (ctest only) | `DiscoveryTimeoutTest.EveryDiscoveryWaits`, `RequiredTest.ThePythonTestsAreRegistered` | CMake alone |
| (ctest only) | `ExampleTest.isotp_fuzz_cli`, `ExampleTest.notebook` | Python 3; `ExampleTest.isotp_fuzz_cli` needs Linux and `vcan0` (a skip without it unless `ECUIO_VCAN_REQUIRED=1`), `ExampleTest.notebook` scikit-learn, numpy, nbconvert, nbclient and ipykernel (a skip without them; no CI job installs them) |

The full cells (what each suite feeds and the knobs it reads) are in
[Suites in detail](testing-inventory.md#suites-in-detail).

`ctest -N` lists every case. The current total is in
[STATUS.md](../STATUS.md).

## Coverage

The README's badge is line coverage measured by OpenCppCoverage over the
whole ctest suite, in CI's `win-coverage` job (three legs, on pushes to
`main` and on manual runs, never pull requests), published
to [Codecov](https://codecov.io/gh/erickramer42/havne) with a per-file
view once all three legs have uploaded. `scripts/coverage.sh` runs the same measurement locally
([development.md](development.md#running-tests)).

- **Counted:** the native code in `core/`, `surrogate/` (the x64 child,
  which the surrogate cases spawn) and `app/` (the bridge and the
  recorder). A line counts as covered when any test binary ran it.
- **Not counted:** QML and its JavaScript, which `test_qml` exercises
  but OpenCppCoverage can't see; `app/main.cpp`, which no test links;
  the x86 surrogate, because CI never runs the cross-arch
  configuration; and the tests and fake DLLs themselves. There's no
  branch coverage. Two cases are left out of the coverage run (the
  reason is in `scripts/coverage.sh`): they fail under the tool on CI's
  runners and pass natively.
- **What it doesn't show:** a covered line ran, nothing more. The
  real-hardware and vehicle [gaps](#gaps) are outside what any coverage
  number measures. Most uncovered lines are failure branches the fake
  can't provoke: pipe I/O timeouts, a child's startup exit codes, a
  thread that fails to start.

## Performance

The cost per frame by profile size, the load test, a recording's cost, opening and drawing one, reply timing and the connect cost are measurements, not tiers: they are in [performance.md](performance.md).

## What is verified

The rows, with their tests and evidence, are in [testing-inventory.md](testing-inventory.md), one
heading for each area. What each area pins:

**[Core, the C ABI and the sanitizers](testing-inventory.md#core-the-c-abi-and-the-sanitizers).**
Core builds with no Qt (the Ubuntu job's core-only configure, and a configure-time link check) and
runs clean under ASan, UBSan and LeakSanitizer; every public header is valid strict C. Pinned:
callback registration and threads, no allocation per frame in a worker, a frame's decoded batch, the
session's lifecycle and clock, and the bridge's marshaling to the GUI thread (coalescing at 10 Hz,
history, the in-flight cap, the logs, link-state mapping).

**[Mock and replay](testing-inventory.md#mock-and-replay).** The mock's drive cycle, timing and
plausible values, and what the demo sends and says it never sends, are pinned, as is the replay
transport: each recorded row out with its ID, flags and timing, faults, a stretch, a torn last row,
what it refuses at create, a recording changed under it, and the round trip from a recorded mock
session.

**[ELM327](testing-inventory.md#elm327).** The wire (the datasheet's words, values and commands,
ISO-TP reassembly) is tested with no adapter; a session runs against the fake ELM327 behind the port
opener, a pseudo-terminal, a TCP listener and an app-fed stream. Pinned: requests and replies, link
health, connect refusals, passive listening, the wire log and serial-port discovery.

**[SocketCAN](testing-inventory.md#socketcan).** The transport with no bus (interface names,
filters, netlink and kernel frames, timestamps, the listen-only judgment, ISO-TP) and, on `vcan0`, a
session hearing the bus as it is, an interface going down, requests through core's ISO-TP and the
kernel's, and the simulated car played onto the bus. CAN FD's ESI flag is kept.

**[J2534 and the surrogate](testing-inventory.md#j2534-and-the-surrogate).** Discovery, the loader,
teardown order, a whole session through the fake DLL, `?baud=`, `?flags=` and 29-bit channels, and
the surrogate's spawn, handshake, relay, routing by architecture, late and skewed replies, refusals
and logs. The link-health states and their banners are pinned on every surface, and the cross-arch
round trip runs on the dev machine. Real-adapter runs are BENCH rows.

**[Decoding, signal files and plugins](testing-inventory.md#decoding-signal-files-and-plugins).**
Signal decode and the validator, text and enumerated kinds, computed signals, mux keys, display
strings, precision and units, the profile registry, the `obd2` set and its Service 01 table, signal
files and their schemas, the OBDb import, plugins and the one store, and finding a signal among
many. Decoder and parser fuzzing covers arbitrary defs and payloads.

**[Request engine and diagnostics](testing-inventory.md#request-engine-and-diagnostics).** The
engine with time passed in and through the workers: rate classes, pacing, several PIDs per request,
`?poll=`, the probe, the DID scan, `uds-generic`, the extended session, reply timing, two ECUs in
one session, identification, trouble codes and their titles, inspection readiness, the simulated ECU
and clearing, with their log lines and pages. The default pick is asked as a focus.

**[Recording, exports and imports](testing-inventory.md#recording-exports-and-imports).** The
recorder, the reader and its frames, the frames check and a vehicle's baseline, the CSV, wide CSV
and folder exports with scrubbing, the health report, candump and CSV imports and a column map
guessed, cancels, and the Recordings page's list, rows and Import. A recording holds every value
core decodes, and Mark and a save through a provider are pinned.

**[Viewer](testing-inventory.md#viewer).** The viewer's model, its chart (raw rows by the rows a
frame draws, dashed second run, markers, findings, cursor, a label cut that keeps the number whole,
text that fits at 200 to 600 px), statistics and steps, stretches by their rules, Jump and the
Stretches panel, comparisons and two recordings lined up on their UTC starts, the frames table,
cancels, and Export.

**[Live and Dashboard](testing-inventory.md#live-and-dashboard).** Live's list, strip chart, trace
styles, pause and the maximized chart; the Dashboard's layout file, gauges, colors, holds and joined
panel; the Activity page's log and Frames view; the drive view and Home; and the look (Theme,
palettes, the type, controls, buttons, zoom, accessibility). The QML tests fail on a binding error.
They read properties, not pixels.

**[Conditions and alerts](testing-inventory.md#conditions-and-alerts).** Conditions
([ADR-0063](adr/0063-conditions.md)): the loader's every refusal with its line, a focus keeping an
attached condition's signals asked, and a recording's lost condition events counted by the reader,
`view` and `info`, Python and the viewer.

**[havne-cli](testing-inventory.md#havne-cli).** The public-headers-only program (ADR-0031): its
lines, ring and exit codes (a usage error exits 2, a missed connect deadline 3, a stop from outside
0), `log`, `stream --record`, `view`, `info`, `signals check`, `plugins`, `diagnostics`, `vin`,
`clear` and `--pick ranked`, paths outside any one code page, and the golden-output comparator.

**[Python bindings and the wheel](testing-inventory.md#python-bindings-and-the-wheel).** The shared
core exports exactly what the public headers declare, in the x64 tree and the x86 one. The pull
queue, the `havne` package (its declarations are the headers', byte for byte), the wheel built and
installed in a fresh venv, the Done-when's script, exports, sizing and the health report, and the
offline lookups, field for field against `havne- cli`.

**[Windows, Linux and macOS builds and the release
zip](testing-inventory.md#windows-linux-and-macos-builds-and-the-release-zip).** The full app builds
and ctest passes on Windows (MSVC 2022), macOS (AppleClang) and Linux (GCC), the last also on bare
metal. Pinned: the Linux AppImage, the core packages and their reproducibility, the SBOM, the
downloads' notices, a release's PDBs, the CLI beside `havne.exe`, and the v0.3.1 release zip as a
user gets it.

**[Android and iOS](testing-inventory.md#android-and-ios).** The Android APK builds and packages,
and the release's APK is signed or refused. The phone's Bluetooth, background sessions, open dialogs
and small items are pinned as far as the desktop reaches, and every page is checked at a phone's
width and density. The iOS device build compiles, and the app launches on the Simulator and draws.

## Manual smoke: link-health UI

`QmlTest.linkhealth` runs the quiet, fault, requests and
disconnect-reset rows below on every surface, through the x64
surrogate. The recipe covers the rest: the x86 surrogate, the arbId
column, dimming, a slow teardown, and how it all looks. Recipe: run
the app from a shell so the session inherits the knobs at spawn (the
child snapshots the environment, so a knob changed in it mid-session
doesn't reach a running session):

| Goal | Environment | URI | Expected |
|---|---|---|---|
| Quiet banner | `ECUIO_FAKE_READ_STATUS=9` | `j2534://<fake.dll>?profile=raw&filter=0x7E8` | Amber "Vehicle not responding" after ~1.2 s; no frames, empty signal list (raw profile) |
| Fault banner | `ECUIO_FAKE_READ_STATUS=7` | same | Red "Adapter fault" banner with episode count; the knob holds for the session: the child took the environment at spawn |
| Requests banner | `ECUIO_FAKE_WRITE_STATUS=7` | `j2534://<fake.dll>` (bare: the `obd2` rotation keeps writing) | At the first failed write: amber "Requests failing: check the vehicle connection and ignition" on LiveView, "Requests failing" with an amber dot on Home and the Dashboard header, an amber Settings label, and "Link: requests failing (1 episode(s))" in the activity log. **Verified 2026-10-21** (x64 fake): every surface as listed; values stay "—" |
| Frames + arbId column | none | `j2534://<fake.dll>?profile=raw&filter=0x7E8&poll=0x0C` | Frames on LogPage (developer mode, its Frames view) with `0x7E8` in the ID column, DLC 4, payload `41 0C 1F 40` (2000 RPM canned); link ok, no banner. **Verified 2026-10-20** using the Win32-tree fake (`build-win32/tests/Release/fake_j2534.dll`). The session log confirmed it ran through the x86 surrogate, so this row also covers surrogate routing, the strip/stamp path and poll mode in the app. It doesn't exercise `?poll=` outranking a profile's rotation: `raw` has none. |
| Dimming baseline | none | `mock://demo` | Values stream at full opacity, steady ones (IAT) included, because every mock signal arrives every 50 ms. Only a signal that stops arriving dims, 3 s after its last sample. **Verified 2026-10-20:** nothing dims on mock. |
| Disconnect reset | after any of the above | Disconnect | Home dot and Settings label show plain disconnected, not the last session's fault/quiet |
| Slow teardown | `ECUIO_FAKE_DISCONNECT_DELAY_MS=6000` | `j2534://<fake.dll>`, then Disconnect | For the 6 s: "Disconnecting..." on the header pill, the Settings label and button, LiveView's status line and the header button; Connect and the presets disabled; `Disconnecting:` in the activity log. Then "No session", Connect enabled, `Disconnected:` in the log, and a reconnect works. **Verified 2026-10-21** with the x64 fake, and through the x86 surrogate (`-x86` fake, a 20000 ms stall): the session log marked the child unresponsive, `Disconnected:` followed about 6 s after `Disconnecting:`, and the window answered UI Automation throughout |

Every row has passed in the app: the quiet, fault, arbId, dimming and
disconnect rows on 2026-10-20, and the requests and slow-teardown rows
on 2026-10-21, with the quiet, fault and disconnect rows re-run against
the current Home, Settings and Dashboard header. On real hardware the
OBDX Pro VX has shown the fault state
([bench-sessions/2026-10-20.md](bench-sessions/2026-10-20.md)) and the
requests and quiet states
([bench-sessions/2026-10-22.md](bench-sessions/2026-10-22.md)).

## Conventions

- **Exact counts against the deterministic fake.** It emits only what
  a test injected or what it echoes back, so a fabricated or dropped
  frame fails the test.
- **When you change shared plumbing, pin the contract you changed, at
  the point you changed it.** For example,
  `DefaultConnectUsesIso15765At500k` was added alongside the query
  parameters.
