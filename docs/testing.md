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
| **HARNESS** | The J2534 stack end to end against the in-tree fake vendor DLL; the ELM327 transport against the in-tree fake ELM327, behind its port opener, a loopback socket, an app stream and (Linux, macOS) a pseudo-terminal | J2534: CI on Windows (same-arch, all x64). ELM327: CI on every desktop platform |
| **Cross-arch** | HARNESS with an x64 test process driving an x86 surrogate and an x86 fake | Dev machine only; needs environment variables ([development.md](development.md#running-tests)) |
| **Fuzzing** | libFuzzer harnesses, one for each parser of input from outside (signal defs and payloads, sets, `did=` queries, ELM327 replies, recordings' rows and folders, SocketCAN's netlink and frames, an ECU's diagnostics replies, ISO-TP, VINs, trouble codes, the simulated ECU's URI and requests, the ELM327 emulator's text, candump logs, JSON, signal files, OBDb signal sets), and one stateful harness, a requesting session's engine and ISO-TP against an ECU on a simulated bus, all run under ASan (and UBSan with Clang) and checking their contracts on every input; [fuzz/README.md](../fuzz/README.md) lists each and what it checks. Not ctest cases: a time-bounded fuzzer would make a green suite stop meaning green ([fuzz/README.md](../fuzz/README.md)) | CI `fuzz` job (Linux, Clang), 60 s per harness (the JSON, signal-file, plugin and OBDb harnesses started from the seeds `fuzz/seeds.py` builds from the committed fixtures), crashing inputs uploaded; locally with MSVC's libFuzzer in a tree of its own |
| **Simulator smoke** | The iOS app installs and launches on the iOS Simulator, QML loads, and the app is still running 45 s later. Nothing drives the UI. A pass doesn't prove the app drew anything, so look at the screenshots. | CI on request: a manual run with `ios_smoke` ticked (`smoke_only` instead skips the other platform jobs), or `[ios-smoke]` in the commit message (`ios-simulator-smoke`, Intel macOS runner) |
| **Manual smoke** | Running the app: layout, Dashboard gauges, LiveView, strip chart, Settings picker against the live registry and the machine's serial ports | Dev machine |
| **BENCH** | A real vendor DLL and adapter, no vehicle | First formal session complete, 2026-10-16, quiet bus ([session sheet](bench-sessions/2026-10-16.md)); informal quiet-bus follow-up 2026-10-20 ([note](bench-sessions/2026-10-20.md)); quiet-bus run of the vehicle session's URIs with recording, 2026-10-22 ([sheet](bench-sessions/2026-10-22.md)), and again on the laptop that goes to the car, 2026-10-25 ([sheet](bench-sessions/2026-10-25-bench.md)); the VX as an ELM327 over USB, no bus, 2026-10-27: refused at `AT Z` ([sheet](bench-sessions/2026-10-27-elm327-bench.md)), then every phase passed, a cable pull included ([sheet](bench-sessions/2026-10-27-elm327-bench-2.md)); the later sessions, the SocketCAN desk bus among them, are listed in [bench-prep.md](bench-sessions/bench-prep.md) |
| **Vehicle** | A live vehicle session | Sessions on one car, its engine ECU on `0x7E8`: 2026-10-22, ignition off, engine off and idling ([session sheet](bench-sessions/2026-10-22-vehicle.md)); 2026-10-25, the same plus the request engine, the probe, identification, `uds-generic` and an ignition cycle ([session sheet](bench-sessions/2026-10-25-vehicle.md)); 2026-10-27, through the ELM327 transport: the OBDLink MX+ over Bluetooth through a cold start and idle, the values as in the J2534 sessions, and the VX in ELM mode through a start and idle ([session sheet](bench-sessions/2026-10-27-elm327-vehicle.md)); 2026-10-31, the full `obd2` set and the trouble codes through both adapters ([VX](bench-sessions/2026-10-31-vehicle.md), [MX+](bench-sessions/2026-10-31-elm327-vehicle.md)); 2026-11-01, several PIDs per request, more than one ECU, DID scans, the timer A/B and the first clears, through the VX ([session sheet](bench-sessions/2026-11-01-vehicle.md)) and the MX+ relayed by the test phone ([session sheet](bench-sessions/2026-11-01-android.md)); 2026-11-02, the VX re-runs of that day's fixes ([session sheet](bench-sessions/2026-11-02-vehicle.md)); 2026-11-03, a real CAN controller under SocketCAN listening beside the VX ([session sheet](bench-sessions/2026-11-03-socketcan-car.md)); 2026-11-04 and 2026-11-05, both ECUs in one session through both adapters, and listening beside the VX ([session sheet](bench-sessions/2026-11-04-two-ecu-car.md), [re-run](bench-sessions/2026-11-05-two-ecu-car-rerun.md)); 2026-11-06, a signal file's DIDs on two modules ([session sheet](bench-sessions/2026-11-06-own-signals-car.md)). Run sheets [vehicle-runsheet.md](bench-sessions/vehicle-runsheet.md) and [elm327-vehicle-runsheet.md](bench-sessions/elm327-vehicle-runsheet.md) |

The smoke tiers earn their place: they have caught bugs no C++ test
could see. A strip-chart indexing bug lived in QML paint code, and the
iOS Simulator's narrow window showed an off-center status card and a
tab bar that named the Settings page "Setup".

`scripts/verify.sh --changed` picks the steps from the paths a branch
changes, and `--suite` runs named suites, to shorten the edit-and-check
loop. Neither counts as verification: the table in
`scripts/verify_scope.py` is conservative, but only the plain
`scripts/verify.sh` runs every tier the change could reach.
`VerifyScopeTest.SelfTest` pins that table.

## Suites

| Binary | Suites | Needs |
|---|---|---|
| `test_core` | `CoreTest`, `VinTest`, `JsonTest`, `RecorderTest`, `RecordingRowsTest`, `RecordingTest`, `ConditionSessionTest`, `ConditionRecordingTest`, `ArmedRecorderTest`, `CheckRulesTest`, `RecordingCheckTest`, `TempNameTest`, `ObdbImportTest`, `Sha256Test`, `ScrubFramesTest`, `CsvExportTest`, `FolderExportTest`, `HealthTest`, `CsvImportTest`, `CsvGuessTest`, `QueueTest`, `RecordingFramesTest`, `FramesReadTest`, `DefIndexTest`, `CandumpTest`, `ImportTest`, `ScrubTest`, `ExportTest` | Nothing (Qt-free). Scratch files and folders are named for the process and the test (`tests/temp_name.h`, which `test_elm327` and `test_obd2` use too), so runs from two checkouts at once never share one. `QueueTest` (`tests/test_queue.cpp`) feeds sessions that never start through dispatch, and runs a `mock://` worker with readers on threads of their own for the close cases. `RecorderTest` records sessions it feeds by hand into folders of its own in the temporary directory; `RecordingTest` writes recordings for the reader there, an hour of the replay fixture's values among them (`tests/recording_hour.h`); `CheckRulesTest` feeds the frames check row by row, and `RecordingCheckTest` checks folders, the replay fixture with one of each finding put in among them |
| `test_signals` | `SignalDecodeTest`, `SignalsTest`, `SignalTablesTest`, `ComputedTest`, `SignalFileTest`, `PluginTest`, `PluginWrapTest`, `ConditionFileTest`, `ConditionEvalTest` | Nothing (Qt-free) |
| `test_c_abi` (ctest name `c_abi`) | Every public header compiles as ISO C11 with extensions off (`-pedantic` on GCC and Clang) and links unmangled; layout asserts | A C compiler |
| `test_c_abi_shared` (ctest name `c_abi_shared`) | `tests/test_c_abi.c` again, linked against the shared core: each call resolves through its export table (internal hooks left out) | The shared core (`HAVNE_SHARED_CORE`, off on Android and iOS) |
| `test_bridge` | `BridgeTest`, `RecordingViewTest`, `ReadOnceTest`, `FrameFilterTest`, `ObdbImportTest`, `RecordingExportTest`, `ViewerFramesTest` | Qt Core and Qt Network. Compiles its `app/` sources directly (`sessionbridge.cpp`, `recordingview.cpp`, `recordingimport.cpp`, `healthreport.cpp`, `framefilter.cpp`, the stores and the rest `tests/CMakeLists.txt` lists), so they must stay free of Qt Gui and Quick. Its `main` puts `HAVNE_SETTINGS_DIR` in a scratch folder, so no case's signal or plugin store is the user's. `RecordingViewTest` opens `tests/fixtures/recording/` and scratch folders |
| `test_qml` | One ctest case per `tests/qml/tst_*.qml` file, `QmlTest.<name>` (`QmlTest.theme` for `tst_theme.qml`); the slowest files (`tst_dashboard.qml`, `tst_dashedit.qml`, `tst_driveview.qml`, `tst_phone.qml`) run as several entries each, `QmlTest.<name>-1of<n>` to `-<n>of<n>` (three, five for `tst_phone.qml` and four for `tst_dashedit.qml`), every nth case, so a citation of `QmlTest.phone` or the others in this document names the file's entries | Qt Quick. Links the app's QML module (`app_ui`). The quiet, fault and requests cases need the fake DLL (Windows) and skip elsewhere. |
| `test_j2534_discovery` | `J2534DiscoveryTest` | Windows |
| `test_j2534_load` | `FakeFixture` | Windows and the fake DLL |
| `test_j2534_stream` | `J2534StreamTest`, `J2534QueryParamTest`, `J2534AdoptionTest` | Windows and the fake DLL |
| `test_j2534_health` | `J2534HealthTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_connect` | `J2534ConnectTest`, `J2534SurrogateConnectTest` | Windows, the fake DLL and the x64 surrogate beside it |
| `test_j2534_surrogate` | `J2534SurrogateTest` | Windows, the fake DLL and the surrogate exe |
| `test_j2534_routing` | `J2534RoutingTest` | Windows. The cross-arch case skips unless `ECUIO_FAKE_DLL` is set. |
| `test_obd2` | `Obd2SetTest`, `Obd2DecodeTest`, `Obd2MuxTest`, `Obd2ReplyTest`, `RequestEngineTest`, `UdsGenericTest`, `Obd2TableTest`, `FakeEcuTest`, `DiagnosticsTest`, `ClearTest`, `DtcTitleTest`, `InspectionTest`, `SimEcuTest`, `SimDidsTest`, `SimAskTest`, `SimRecordingTest`, `PidSupportTest`, `RequestTimingTest`, `SessionControlTest` | Nothing (Qt-free). `Obd2TableTest` reads the Service 01 table, `tests/fixtures/obd2/service01.csv` |
| `test_elm327` | `Elm327ProtocolTest`, `Elm327SessionTest`, `Elm327LogTest`, `SerialPortsTest`, `Elm327TcpTest`, `Elm327StreamTest`, and on Linux and macOS `Elm327PortTest` | Nothing (Qt-free). The session cases put the fake ELM327 (`tests/fake_elm327.h`) behind the transport's port opener; the port cases put it on a pseudo-terminal, the TCP cases behind a listener on 127.0.0.1, the stream cases behind a registered app-fed stream with a thread playing the app |
| `test_vcan` | `VcanTest`, `SocketCanBusTest`, `SocketCanRequestTest`, `SocketCanFaultTest`, `IsoTpConformanceTest`, `SimBusTest` | Linux (Qt-free), and `vcan0` up with `can-isotp` loaded; each case skips without them, and fails under `ECUIO_VCAN_REQUIRED=1` (CI's `linux-build`). `VcanTest`: the simulated bus itself (`tests/vcan_bus.h`), the fakes' ECU answering through the kernel's ISO-TP, and a recording played onto the bus. `SocketCanBusTest`: the `socketcan://` transport on that bus (ADR-0032); its interface-down case needs root and runs again under `sudo` on CI (`ECUIO_VCAN_ROOT_REQUIRED=1`). `SocketCanRequestTest`: `socketcan://` requesting through core's ISO-TP against that ECU (ADR-0040). `SocketCanFaultTest`: the same against the test car's recording as the ECU (`tests/vcan_recording_ecu.h`), its replies broken at the ISO-TP level (ADR-0041 section 4). `IsoTpConformanceTest`: core's ISO-TP against the kernel's |
| `test_socketcan` | `SocketCanTest`, `IsoTpTest`, `SimTest` | Nothing (Qt-free), every platform. SocketCAN's rules with no bus (`core/src/socketcan_wire.h`): interface names, `?filter=`, canned netlink messages and the listen-only judgment, kernel frames, the clock, and the URI's refusals; on Linux also the loopback and a missing interface. ISO-TP's receiver and sender (`core/src/isotp.h`, ADR-0040) |
| (ctest only) | `GoldenTest.SelfTest`, `AbiReportTest.SelfTest`, `ApiCheckTest.SelfTest`, `VpicWmiTest.SelfTest`, `DtcTitlesTest.SelfTest`, `RecordingCompareTest.SelfTest`, `SchemaKeysTest.EachObjectsKeysAreItsLoaders`, `ExportsTest.SelfTest` | Python 3 (left out without it). The golden-output comparator (`tests/golden.py`), the ABI check's judge of an abidiff report (`scripts/abi_report.py`) and its comparison of the headers (`scripts/api_check.py`), the VIN table's generator (`scripts/vpic_wmi.py`) and the trouble-code titles' (`scripts/dtc_titles.py`) and the recording folders' comparator (`tests/recording_compare.py`), and the shared core's export and release rules (`scripts/exports_check.py`), each over its own cases; `tests/schema_keys.py` compares each `schema/` object's keys with the `only_keys` call that checks it, and the loaders' word lists with the schemas' enums |
| `test_cli` | `CliLineTest`, `CliRingTest`, `CliSleepTest`, `CliDiagnosticsTest`, `CliVinTest`, `CliDtcTest`, `CliInspectionTest`, `CliSimTest`, `CliMarkLinesTest`, `CliViewTest`, `CliInfoTest`, `CliSignalFileTest`, `CliPluginTest`, `CliHealthTest`, `CliScanTest`, `CliClearTest`, `CliCanTest`, `CliTimingTest`, `CliRefusedTest`, `CliRecordingSignalsTest`, `CliViewFramesTest`, `CliElm327Test`, `CliTransferTest`, `CliPartialTest`, `CliFramesTest`, `CliSessionTest` | Nothing (Qt-free). `havne-cli`'s line formatting its worker-to-output ring, its sleep rule, its `diagnostics` line and its `vin`, `identity`, `dtc` and `inspection` lines, the lines typed into `stream --record`, `view`'s grammar, axis and lines, `info`'s lines, `frames`' arguments and lines, a signal file's lines and a URI's profile, and a plugin's lines and the rule a fixture passes by (`cli/`, header-only; it links core for the last two) |
| (ctest only) | `CliGoldenTest`, `CliUsageTest`, `CliSignalTest`, `CliReplayTest`, `CliRecordTest`, `CliViewRunTest`, `CliObdbImportTest`, `CliPluginRunTest`, `CliElm327RunTest`, `CliClearRunTest`, `CliTransferRunTest`, `CliHealthRunTest`, `CliExitTest`, `CliConnectTest`, `CliPathTest`, `CliRefusedRunTest`, `SchemaFixturesTest` | `havne-cli` run as a child process. `CliTransferRunTest` (`tests/cli_transfer.py`, Python 3) imports a candump log, reads it through obd2, exports it as pcapng and reads it back with Python's `struct`; its tshark case (skipped without tshark) has tshark's ISO-TP dissector reassemble the made-up frames, and its candump case (Linux) logs the simulated car on `vcan0` with `candump -L` and compares the import with a direct session. `CliElm327RunTest` (`tests/cli_elm327.py`, Python 3) talks ELM327 to `sim --elm327` as a host does, reads it through core's own `elm327://` session, and through python-OBD when installed (skip 77 otherwise). `CliClearRunTest` (`tests/cli_clear.py`, Python 3) clears the fake DLL's ECU on Windows (running, standing, refused, moving, the word on moving) and the simulated ECU on `vcan0` on Linux (skip 77 without it). `SchemaFixturesTest` (`tests/schema_fixtures.py`, the `jsonschema` package from `.github/jsonschema-requirements.txt`; skipped without it unless `ECUIO_SCHEMA_REQUIRED=1`, as CI's `core-and-tests` sets) validates every committed signal file, manifest, data file and fixture file against `schema/`, and gets one verdict from the schema and `havne-cli`'s checks for each refused file and edge case. `CliPluginRunTest` (`tests/cli_plugin_check.py`, Python 3, Linux) runs `plugins check --on vcan0` on every plugin in `plugins/` (skipped without vcan0 unless `ECUIO_VCAN_REQUIRED`, serial as `test_vcan` is). `CliObdbImportTest` (`tests/cli_obdb_import.py`, Python 3) imports the synthetic set in `tests/fixtures/obdb/` and compares the lines with a golden and the file written with `tests/golden/obdb/`, and an `--out` that can't be written. `CliRecordTest` (`tests/cli_record.py`, Python 3) records through `stream --record` and compares the folder with `tests/golden/recording/` (`tests/recording_compare.py`); `CliViewRunTest` (`tests/cli_view.py`, Python 3) views `tests/fixtures/recording/`. The golden runs need Python 3 and compare its whole output with `tests/golden/cli/`; the usage cases run under `cmake -P` (`tests/cli_exit.cmake`); the signal case is SIGINT and SIGTERM on Linux and macOS (`tests/cli_signal.sh`), Ctrl+Break on Windows (`tests/cli_ctrl_break.py`, Python 3; skipped without a console) |
| (ctest only) | `PythonTest.<file>`, one per `python/tests/test_*.py` | The `havne` package under pytest (`python/tests/run.py`: exit 77, a skip, without pytest or when every case in a file skipped; a failure instead under `ECUIO_PYTEST_REQUIRED=1`, as CI's `core-and-tests`, `win-build`, `macos-build` and `linux-build` set), against the tree's own shared core (`HAVNE_LIBRARY`). Registered where the tests' Python has the tree's pointer size: the x64 trees, and `build-win32` when configured with a 32-bit Python, which `scripts/verify.sh`'s cross-arch step runs against the x86 core, fake and surrogate. `test_did_scan` (the mock everywhere; on `vcan0` the simulated ECU's second module against the unbound sim's own answers) and `test_isotp_fuzz` (Linux, `vcan0`: 200 cases, one replayed alone, no ECU, the ECU closed mid-run, a second ECU already on the bus, `lo` refused) run `RUN_SERIAL` with `test_vcan` |
| (ctest only) | `WheelTest.SelfTest`, `WheelTest.Installed` | `packaging/python/make_wheel.py selftest` (made-up PE, ELF and Mach-O files and a made-up package: the tags, each refusal, `RECORD`, two builds byte for byte). `tests/wheel_installed.py` (64-bit trees with the shared core, not the sanitizer trees): the tree's own library packed twice to the same bytes, then `packaging/python/check_wheel.py`, release.yml's own check: a fresh venv, `pip install --no-index`, `HAVNE_LIBRARY` and the surrogates' variables unset, the library in `site-packages` (Linux's single `libecuio.so.<soname>`), `read_pids.py mock://`, and on Windows the fake DLL through the wheel's x64 surrogate and the Win32 tree's fake through its x86 one where `build-win32` exists (not in CI); exit 77 without venv or pip, a failure under `ECUIO_PYTEST_REQUIRED=1` |
| (ctest only) | `DiscoveryTimeoutTest.EveryDiscoveryWaits`, `RequiredTest.ThePythonTestsAreRegistered` | CMake alone. `tests/discovery_timeout.cmake`: every `gtest_discover_tests` call in `tests/CMakeLists.txt` names `DISCOVERY_TIMEOUT 60`, since discovery runs each binary at build time and a cold runner can take more than the default 5 s. `tests/python_registered.cmake`: configure registered at least one `PythonTest` case, and `WheelTest.Installed` where a 64-bit tree without sanitizers should, so a probe that failed can't leave them out unseen: a failure under `ECUIO_PYTEST_REQUIRED=1` (CI's test jobs), a skip otherwise |
| (ctest only) | `ExampleTest.isotp_fuzz_cli`, `ExampleTest.notebook` | `tests/example_isotp_fuzz.py <havne-cli>` (Linux, `vcan0`, `RUN_SERIAL`): `python/examples/isotp_fuzz.py --ecu-cli`, so `havne-cli sim` is the ECU and must end with exit 0 and its end line not failed; outside pytest, so `core-sanitizers` runs it on its ASan `havne-cli`; skips without `vcan0` unless `ECUIO_VCAN_REQUIRED=1`; its own bound (240 s) under ctest's, dumping the fuzzer's stack on a hang. `tests/example_notebook.py`: `python/examples/ecu_states.ipynb` run top to bottom through `nbconvert` in a kernelspec of the test's own (the test's Python), its last cell's check included; the run's modules imported first in a child Python under a bound of their own (600 s), so a cold read from a busy disk never reads as a hung notebook, then the notebook under 240 s; exit 77 without scikit-learn, numpy, nbconvert, nbclient or ipykernel, which no CI job installs, so it runs locally (`build-py-venv`) |

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
  real-hardware and vehicle gaps ([in detail](testing-inventory.md#gaps-in-detail)) are outside what any coverage
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
