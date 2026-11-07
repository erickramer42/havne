# File/layer: tests/CMakeLists.txt | CMake 3.21+, all three CI platforms
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # GoogleTest's own install rules would put it in `cmake --install`'s
    # tree beside the core package (ADR-0035)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# The tests fill fixed buffers with strncpy/strcpy (one deliberately
# leaves a name unterminated) and read the fake's knobs with getenv.
# MSVC's CRT deprecation warning (C4996) on those is noise here.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# --- Core tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_core test_core.cpp test_json.cpp test_obdb_import.cpp
    test_recorder.cpp test_recording.cpp test_recording_check.cpp
    test_recording_frames.cpp test_sha256.cpp)

# core/src for mock_drive_cycle.h and link_health.h, which are internal
# (test_obd2 precedent).
target_include_directories(test_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_core PRIVATE
    ecuio-core
    GTest::gtest_main
)

# The scrubbed real-bus recording replay:// plays (ADR-0023), and the
# signal file a recording copies (ADR-0051 section 4)
target_compile_definitions(test_core PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    ECUIO_SIGNAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals"
    ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
)

app_enable_warnings(test_core)
gtest_discover_tests(test_core)

# --- The reader's open, timed (ADR-0049 section 9): a measurement, not in
# ctest. Qt-free, so an NDK tree builds it alone for the test phone
# (docs/testing.md "Performance") ---
add_executable(bench_recording bench_recording.cpp)
target_include_directories(bench_recording PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(bench_recording PRIVATE ecuio-core)
target_compile_definitions(bench_recording PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
)
app_enable_warnings(bench_recording)

# --- Signals tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_signals test_signals.cpp test_signal_tables.cpp test_signal_file.cpp
    test_plugin.cpp)

# test_signal_tables.cpp checks computed.h, the programs' checker and
# stack machine, which are internal
target_include_directories(test_signals PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_signals PRIVATE
    ecuio-core
    GTest::gtest_main
)
# test_plugin.cpp loads the repository's example plugin
target_compile_definitions(test_signals PRIVATE
    ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins")

app_enable_warnings(test_signals)
gtest_discover_tests(test_signals)

# --- OBD-II set + request engine tests: MUST stay Qt-free (UNIT tier) ---
# obd2_signal_sets.cpp is ALL-platform (signal data is transport-
# agnostic), and request_engine.h is header-only, so this suite builds
# and runs in the headless Ubuntu job alongside test_core/test_signals.
# The explicit core/src include is required because request_engine.h is
# INTERNAL: ecuio-core's PUBLIC include dirs cover only core/include,
# and the engine is deliberately not part of the public tree.
add_executable(test_obd2 test_obd2.cpp)

target_include_directories(test_obd2 PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_obd2 PRIVATE
    ecuio-core
    GTest::gtest_main
)

# The Service 01 table the obd2 set and the fakes' ECU are checked against,
# the readiness monitors' names, and the replay fixture the simulated ECU
# loads
target_compile_definitions(test_obd2 PRIVATE
    ECUIO_OBD2_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obd2/service01.csv"
    ECUIO_OBD2_MONITORS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obd2/monitors.csv"
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
)

app_enable_warnings(test_obd2)
gtest_discover_tests(test_obd2)

# --- ELM327 tests: MUST stay Qt-free (UNIT and HARNESS tiers, ADR-0026) ---
# test_elm327.cpp: elm327_protocol.h, header-only and internal like
# request_engine.h, which it also includes to hand it the replies.
# test_elm327_session.cpp: the transport against the fake ELM327 behind
# internal.h's port opener, and on POSIX the OS's port over a
# pseudo-terminal (elm327_port.h); it holds the binary's new_counter.h.
# test_serial_ports.cpp: serial-port discovery (serial.h, serial_ports.h).
add_executable(test_elm327 test_elm327.cpp test_elm327_session.cpp
    test_serial_ports.cpp)
target_include_directories(test_elm327 PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)
target_link_libraries(test_elm327 PRIVATE
    ecuio-core
    GTest::gtest_main
)
if(WIN32)
    # Elm327TcpTest's loopback listener
    target_link_libraries(test_elm327 PRIVATE ws2_32)
endif()
app_enable_warnings(test_elm327)
gtest_discover_tests(test_elm327 PROPERTIES TIMEOUT 30)

# --- SocketCAN's rules: Qt-free, every platform (UNIT, ADR-0032, ADR-0040) ---
# socketcan_wire.h is internal and portable (the kernel's values mirrored),
# so its rules test everywhere; the transport on a bus is test_vcan's.
# test_isotp.cpp: isotp.h, the ISO-TP that SocketCAN requests run on.
add_executable(test_socketcan test_socketcan.cpp test_isotp.cpp)
target_include_directories(test_socketcan PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test_socketcan PRIVATE ecuio-core GTest::gtest_main)
# the simulated ECU loads the replay fixture (SimTest, Linux)
target_compile_definitions(test_socketcan PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay")
app_enable_warnings(test_socketcan)
gtest_discover_tests(test_socketcan)

# --- The simulated CAN bus: Qt-free, Linux only (SocketCAN) ---
# test_vcan.cpp: vcan_bus.h's fake ECU through the kernel's ISO-TP and a
# recording played onto vcan0. Each case skips without the interface;
# CI's linux-build brings it up and sets ECUIO_VCAN_REQUIRED=1.
# test_vcan_socketcan.cpp: the SocketCAN transport on that bus
# (ADR-0032, SocketCanBusTest); it holds the binary's new_counter.h.
# test_vcan_requests.cpp: socketcan:// requesting through ISO-TP against
# that ECU (ADR-0040, SocketCanRequestTest), and against the test car's
# recording as the ECU, its replies broken at the ISO-TP level
# (vcan_recording_ecu.h, ADR-0041 section 4, SocketCanFaultTest).
# test_vcan_isotp.cpp: core's ISO-TP against the kernel's
# (IsoTpConformanceTest).
# RUN_SERIAL: the cases share the bus, and an error frame passes every
# session's ID filter.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_vcan test_vcan.cpp test_vcan_socketcan.cpp
                   test_vcan_requests.cpp test_vcan_isotp.cpp)
    target_compile_definitions(test_vcan PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(test_vcan PRIVATE ecuio-core GTest::gtest_main Threads::Threads)
    # core/src: isotp.h, pure and internal (test_socketcan's precedent)
    target_include_directories(test_vcan PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
    app_enable_warnings(test_vcan)
    gtest_discover_tests(test_vcan PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

# --- Golden-output comparator: Python 3, no packages ---
# golden.py compares JSON-lines output with a committed golden file,
# dropping the keys a run changes; its self-test pins those rules. Left
# out where no Python 3 is found.
find_package(Python3 COMPONENTS Interpreter QUIET)
# scripts/abi_report.py, the ABI check's judge of an abidiff report
# (growth into a struct's reserved room passes, a break fails): its
# self-test runs over reports of real mutants, on every platform.
if(Python3_Interpreter_FOUND)
    add_test(NAME GoldenTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py selftest)
    # recording_compare.py, golden.py's like for recording folders
    # (ADR-0049 section 9): what a run changes left out
    add_test(NAME RecordingCompareTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recording_compare.py selftest)
    add_test(NAME AbiReportTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/abi_report.py selftest)
    # scripts/api_check.py, the ABI check's source half: the declarations
    add_test(NAME ApiCheckTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/api_check.py selftest)
    # packaging/sbom.py, the release's SBOM (ADR-0035 section 5)
    add_test(NAME SbomTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/packaging/sbom.py selftest)
    # scripts/vpic_wmi.py, the VIN table's generator (ADR-0043 section 2)
    add_test(NAME VpicWmiTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/scripts/vpic_wmi.py --selftest)
    # scripts/dtc_titles.py, the trouble-code titles' generator (ADR-0044)
    add_test(NAME DtcTitlesTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/scripts/dtc_titles.py --selftest)
    # schema/'s objects against the loaders' only_keys sets and word lists
    # (ADR-0051, ADR-0052): a key on one side alone fails
    add_test(NAME SchemaKeysTest.EachObjectsKeysAreItsLoaders
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/schema_keys.py
                ${CMAKE_SOURCE_DIR})
endif()

# --- havne-cli (ADR-0031): Qt-free, every desktop platform ---
# test_cli.cpp: its line formatting and output ring (cli/, header-only;
# the diagnostics line names codes and monitors through core).
# The golden runs: the program as a child process, its whole output
# against tests/golden/cli/ ("core", the version, ignored; mock's t_us
# too, which is the host's clock). Rewrite one after a deliberate change
# with the same command plus --update, and read its diff.
if(TARGET havne-cli)
    add_executable(test_cli test_cli.cpp)
    target_include_directories(test_cli PRIVATE ${CMAKE_SOURCE_DIR}/cli)
    find_package(Threads REQUIRED)
    target_link_libraries(test_cli PRIVATE ecuio-core GTest::gtest_main Threads::Threads)
    app_enable_warnings(test_cli)
    gtest_discover_tests(test_cli PROPERTIES TIMEOUT 30)

    if(Python3_Interpreter_FOUND)
        set(_cli_golden ${CMAKE_CURRENT_SOURCE_DIR}/golden/cli)
        set(_cli_replay "replay://${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start")
        function(app_cli_golden name golden)
            add_test(NAME CliGoldenTest.${name}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py run
                        --golden ${_cli_golden}/${golden} --ignore core ${ARGN})
            set_tests_properties(CliGoldenTest.${name} PROPERTIES TIMEOUT 60)
        endfunction()
        # The real car's engine start through obd2, from outside the
        # process: its first frames and values, then the run to its end
        app_cli_golden(ReplayThroughObd2 replay-obd2.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?profile=obd2&speed=max"
               --values --count 300)
        # A ring of a few lines and a slow reader, so a replay at max speed
        # meets a full ring at once: it must wait, and its end line count
        # every frame and no drop
        app_cli_golden(ReplayRunsToItsLastFrame replay-end.jsonl --last 3 --slow
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?profile=obd2&speed=max"
               --values)
        set_tests_properties(CliGoldenTest.ReplayRunsToItsLastFrame PROPERTIES
            ENVIRONMENT HAVNE_CLI_RING_BYTES=40000)
        # Service 01 replies carrying several PIDs (ADR-0039 Appendix A),
        # written by hand: PID 55 alone with bank 1's byte, then packed
        # replies, one holding 55 at that length, and one that won't split
        app_cli_golden(ReplaySplitsPackedReplies replay-packed.jsonl
            -- $<TARGET_FILE:havne-cli> stream
               "replay://${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/packed-replies?profile=obd2&speed=max"
               --values)
        # The same engine start with faults injected (ADR-0041), seeded so
        # they're the same each run: the stream still ends, and its end line
        # counts what ?faults= did
        app_cli_golden(ReplayWithFaultsEndsAndCountsThem replay-faults.jsonl --last 3
            -- $<TARGET_FILE:havne-cli> stream
               "${_cli_replay}?profile=obd2&speed=max&faults=drop:10,overflow:5,corrupt:2,truncate:2,stall:1:5&seed=3"
               --values)
        # Two seconds of the engine start (ADR-0023's appendix "a stretch
        # replayed"): its first frame stamped 0, the session line counting
        # the stretch's frames, the end at its last
        app_cli_golden(ReplayPlaysAStretch replay-stretch.jsonl
            -- $<TARGET_FILE:havne-cli> stream
               "${_cli_replay}?profile=obd2&speed=max&from=100&to=102"
               --values)
        app_cli_golden(MockDriveCycle mock.jsonl --ignore t_us
            -- $<TARGET_FILE:havne-cli> stream mock://golden --values --count 40)
        # A typed VIN's decode (ADR-0043), made up around FAKE: the model
        # year a letter in position 7 places, the make by vPIC's code
        app_cli_golden(AVinIsDecoded vin.jsonl
            -- $<TARGET_FILE:havne-cli> vin "1fa fake10m0000001")
        # A typed trouble code's title (ADR-0044), spaces and lower case taken
        app_cli_golden(ADtcIsNamed dtc.jsonl
            -- $<TARGET_FILE:havne-cli> dtc "p0 301")
        # --until-scanned (ADR-0020) on a session with no did= range: it
        # ends before it starts, its reason saying why
        app_cli_golden(NothingToScanEndsAtOnce nothing-to-scan.jsonl
            -- $<TARGET_FILE:havne-cli> stream mock://golden --until-scanned)
        app_cli_golden(BadUriIsRefused bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> stream "mock://golden?speed=2")
        app_cli_golden(UnknownProfileIsRefused unknown-profile.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> stream "mock://golden?profile=nonesuch")
        # The simulated ECU's URI is checked before any bus, on every
        # platform (ADR-0046)
        app_cli_golden(SimRefusesABadUri sim-bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> sim "mock://sim?faults=silent:0" --on vcan0)
        app_cli_golden(SimRefusesARelativeRecording sim-relative-recording.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> sim "replay://recordings/run?loop=1" --on vcan0)
        # The ELM327 emulator (ADR-0055) parses its URI as the bus's sim
        # does, before it listens: the same refusal, every platform
        app_cli_golden(SimElm327RefusesABadUri sim-bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> sim "mock://sim?faults=silent:0"
               --elm327 tcp:0)
        # The emulator on 127.0.0.1, a port the system picks: a host's
        # ELM327 exchange, core's own elm327:// session, a second module's
        # DID, a plugin's DIDs, and python-OBD (skipped without it)
        function(app_cli_elm327 name mode)
            add_test(NAME CliElm327RunTest.${name}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_elm327.py
                        ${mode} $<TARGET_FILE:havne-cli>
                        ${CMAKE_SOURCE_DIR}/plugins/example-sim)
            set_tests_properties(CliElm327RunTest.${name} PROPERTIES
                TIMEOUT 120 SKIP_RETURN_CODE 77)
        endfunction()
        app_cli_elm327(AHostSpeaksElm327ToIt adapter)
        app_cli_elm327(CoresOwnSessionReadsTheSimulatedCar session)
        app_cli_elm327(ASecondModulesDidAnswers modules)
        app_cli_elm327(APluginsDidsAnswerOnBothEcus plugin)
        app_cli_elm327(PythonObdReadsIt python-obd)
        # A clear (ADR-0038's appendix of 2026-11-06): the mock's report
        # first, its clear at the key-on's 0 rpm, the report read again;
        # and with the word on the engine, its 0 km/h read too
        app_cli_golden(ClearClearsTheMock clear-mock.jsonl
            -- $<TARGET_FILE:havne-cli> clear mock://golden --yes)
        app_cli_golden(ClearWithTheWordOnTheEngine clear-mock-running.jsonl
            -- $<TARGET_FILE:havne-cli> clear mock://golden --yes --engine-running)
        app_cli_golden(ClearRefusesABadUri bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> clear "mock://golden?speed=2" --yes)
        # A replay cut short under it stops early, and the stream ends
        # with it instead of waiting for a last frame that never comes
        add_test(NAME CliReplayTest.ACutRecordingEndsTheStream
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_recording_changed.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliReplayTest.ACutRecordingEndsTheStream PROPERTIES TIMEOUT 30)
        # A field past every reply is named on an absent line (ADR-0039 section 6)
        add_test(NAME CliReplayTest.AnAbsentFieldIsNamed
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_absent.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliReplayTest.AnAbsentFieldIsNamed PROPERTIES TIMEOUT 30)
        # A DID its ECU refuses is named on a refused line (ADR-0051 section
        # 3): the fake DLL's ECU on Windows, the simulated ECU on vcan0 on
        # Linux (skipped without it unless ECUIO_VCAN_REQUIRED, and serial:
        # the simulated ECU answers 0x7E0 for the whole bus)
        if(WIN32)
            add_test(NAME CliRefusedRunTest.ADidTheFakeEcuRefusesIsNamed
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        fake $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliRefusedRunTest.ADidTheFakeEcuRefusesIsNamed
                PROPERTIES TIMEOUT 60)
            # The refusals come before the bitmap's drops in the report, and
            # those past its list are counted
            add_test(NAME CliRefusedRunTest.ARefusalPastTheBitmapsDropsIsNamed
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        fake-bitmap $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            add_test(NAME CliRefusedRunTest.RefusalsPastTheListAreCounted
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        fake-more $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliRefusedRunTest.ARefusalPastTheBitmapsDropsIsNamed
                CliRefusedRunTest.RefusalsPastTheListAreCounted PROPERTIES TIMEOUT 60)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliRefusedRunTest.ADidTheSimulatedEcuRefusesIsNamed
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        sim $<TARGET_FILE:havne-cli>)
            set_tests_properties(CliRefusedRunTest.ADidTheSimulatedEcuRefusesIsNamed
                PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
        # A clear through core's gate against the fake DLL's ECU (Windows)
        # or the simulated ECU on vcan0 (Linux; skipped without it unless
        # ECUIO_VCAN_REQUIRED, and serial: it answers 0x7E0 for the bus)
        if(WIN32)
            foreach(_case EngineRunningIsNotCleared EngineOffIsCleared
                    ARefusalIsNamed MovingIsNotCleared TheWordOnMovingClears)
                add_test(NAME CliClearRunTest.${_case}
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_clear.py
                            ${_case} $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
                set_tests_properties(CliClearRunTest.${_case} PROPERTIES TIMEOUT 90)
            endforeach()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliClearRunTest.TheSimulatedEcuIsCleared
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_clear.py
                        TheSimulatedEcuIsCleared $<TARGET_FILE:havne-cli>)
            set_tests_properties(CliClearRunTest.TheSimulatedEcuIsCleared
                PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
        # Every path havne-cli takes, under a folder named outside any one
        # code page: on Windows its arguments come as UTF-16
        add_test(NAME CliPathTest.ANonAsciiPathWorks
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_paths.py
                    $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signal-files/daily-driver-extras.json
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obdb/example-car.json
                    ${CMAKE_SOURCE_DIR}/plugins/example-sim)
        set_tests_properties(CliPathTest.ANonAsciiPathWorks PROPERTIES TIMEOUT 120)
        # A last row cut short is left out and counted (ADR-0049 section 7)
        add_test(NAME CliReplayTest.ATornLastRowIsLeftOut
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_torn.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliReplayTest.ATornLastRowIsLeftOut PROPERTIES TIMEOUT 30)
        # A replay reads with its recording's own signals.json (ADR-0051
        # section 4): no --signals, the copy's values; its hash broken, raw
        add_test(NAME CliReplayTest.AReplayReadsWithItsOwnSignalFile
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_replay_signals.py
                    $<TARGET_FILE:havne-cli> "${_cli_replay}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json)
        set_tests_properties(CliReplayTest.AReplayReadsWithItsOwnSignalFile PROPERTIES TIMEOUT 60)
        # stream --record (ADR-0049): the engine start recorded to its last
        # frame, its folder against tests/golden/recording/ and its frames
        # against the fixture's (rewrite both with the command plus
        # --update); lines typed are markers; a folder that can't be made
        # fails the stream
        set(_cli_record ${CMAKE_CURRENT_SOURCE_DIR}/cli_record.py)
        add_test(NAME CliRecordTest.AReplayIsRecordedAsCoreRecordsIt
            COMMAND ${Python3_EXECUTABLE} ${_cli_record} replay $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start
                    ${_cli_golden}/replay-record.jsonl
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        add_test(NAME CliRecordTest.TypedLinesAreMarkers
            COMMAND ${Python3_EXECUTABLE} ${_cli_record} marks $<TARGET_FILE:havne-cli>)
        add_test(NAME CliRecordTest.AFolderThatCantBeMadeFails
            COMMAND ${Python3_EXECUTABLE} ${_cli_record} fails $<TARGET_FILE:havne-cli>)
        # A recording through --signals holds the file as signals.json, and
        # a replay of it through that copy decodes the same (ADR-0051
        # section 4)
        add_test(NAME CliRecordTest.ARecordingHoldsItsSignalFile
            COMMAND ${Python3_EXECUTABLE} ${_cli_record} signals $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json)
        # and through --plugin, its variant's files merged into one
        # (ADR-0052 section 4)
        add_test(NAME CliRecordTest.APluginsRecordingHoldsItsVariantsFile
            COMMAND ${Python3_EXECUTABLE} ${_cli_record} plugin $<TARGET_FILE:havne-cli>
                    ${CMAKE_SOURCE_DIR}/plugins/example-sim)
        set_tests_properties(CliRecordTest.AReplayIsRecordedAsCoreRecordsIt
            CliRecordTest.TypedLinesAreMarkers CliRecordTest.AFolderThatCantBeMadeFails
            CliRecordTest.ARecordingHoldsItsSignalFile
            CliRecordTest.APluginsRecordingHoldsItsVariantsFile
            PROPERTIES TIMEOUT 60)
        # view (ADR-0049 section 5) on two recordings of one pull, written
        # by hand (tests/fixtures/recording/: pull-late's starts 10 s later
        # into it, its UTC start 90 s later, a marker 0.5 s before each):
        # what one holds, the two aligned on the pull's rise (with the
        # values at a time), its marker and their UTC starts, a rise its
        # hysteresis keeps from being found, and a folder with no
        # recording; then every sample counted once at many zooms, and the
        # aligned pulls' columns equal
        set(_cli_pulls ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording)
        app_cli_golden(ViewShowsWhatARecordingHolds view-one.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early --columns 12)
        # Signal files (ADR-0051), synthetic: what signals check reads
        # from one (each of its signals, the units that never convert),
        # one it refuses and one that isn't there, each with the
        # loader's words and never the path; then the replay fixture
        # through a file that extends obd2, its profile added to the URI:
        # its own numbers, an enum's label and a computed value among
        # obd2's, and a refused file ending stream as a connect would
        set(_cli_signals ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals)
        app_cli_golden(SignalsCheckReadsAFile signals-check.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/extras.json)
        app_cli_golden(SignalsCheckRefusesAFile signals-refused.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/refused.json)
        app_cli_golden(SignalsCheckNamesAMissingFile signals-missing.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/none.json)
        app_cli_golden(StreamDecodesThroughASignalFile stream-signals.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max&from=100&to=102"
               --signals ${_cli_signals}/extras.json --values --count 40)
        app_cli_golden(StreamRefusesABadSignalFile stream-signals-refused.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max"
               --signals ${_cli_signals}/refused.json --values)
        # Plugins (ADR-0052): the example and the test packages listed,
        # checked with their fixtures unrun (the vcan run is
        # CliPluginRunTest's), the variants a made-up VIN proposes, and the
        # example's base variant read from the mock
        set(_cli_example ${CMAKE_SOURCE_DIR}/plugins/example-sim)
        set(_cli_plugins ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plugins)
        app_cli_golden(PluginsListSaysWhatEachHolds plugins-list.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> plugins list ${_cli_example}
               ${_cli_plugins}/packed.json ${_cli_plugins}/none)
        app_cli_golden(PluginsCheckReadsTheExample plugins-check.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_example})
        app_cli_golden(PluginsCheckWarnsOfNoFixtures plugins-check-packed.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/packed.json)
        app_cli_golden(PluginsCheckSaysALongInterfacesName plugins-check-long-on.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/packed.json
               --on vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv)
        app_cli_golden(PluginsCheckNamesARefusedFixtureFile
            plugins-check-bad-fixture.jsonl --exit 1
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/bad-fixture)
        app_cli_golden(PluginsCheckRefusesWhatIsntAPlugin plugins-refused.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_signals}/extras.json)
        app_cli_golden(PluginsMatchProposesTheVariants plugins-match.jsonl
            -- $<TARGET_FILE:havne-cli> plugins match ${_cli_example}
               --vin ZZZFAKE11K0000001)
        app_cli_golden(StreamReadsThroughAPluginsVariant stream-plugin.jsonl
            --ignore t_us
            -- $<TARGET_FILE:havne-cli> stream
               "mock://x?profile=plugin.example-sim.base"
               --plugin ${_cli_example} --values --count 6)
        # schema/ (ADR-0051, ADR-0052) through the jsonschema package:
        # every committed signal file, manifest, data file and fixture file
        # validates, tests/fixtures/schema/'s refused ones fail it and
        # havne-cli's check alike, and edge cases get one verdict from
        # both; skipped without jsonschema unless ECUIO_SCHEMA_REQUIRED
        add_test(NAME SchemaFixturesTest.TheSchemasAndTheLoadersAgree
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/schema_fixtures.py
                    ${CMAKE_SOURCE_DIR} $<TARGET_FILE:havne-cli>)
        set_tests_properties(SchemaFixturesTest.TheSchemasAndTheLoadersAgree
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        # Every plugin in the repository with its fixtures answered by the
        # simulated ECU on vcan0 and decoded back (ADR-0052 section 8), and
        # a test plugin giving one signal a value in each of two files;
        # skipped without vcan0 unless ECUIO_VCAN_REQUIRED, and serial, as
        # test_vcan is: the simulated ECU answers 0x7E0 for the whole bus
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliPluginRunTest.EveryPluginsFixturesPass
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/cli_plugin_check.py
                        $<TARGET_FILE:havne-cli> ${CMAKE_SOURCE_DIR}/plugins
                        ${_cli_plugins}/repeat)
            set_tests_properties(CliPluginRunTest.EveryPluginsFixturesPass
                PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
        # An OBDb signal set, synthetic (ADR-0051 section 7): imported for a
        # model year, its notes and the file written whole, beside nothing
        # else, against committed ones; an --out that can't be written
        # writes nothing and names no path; a file that isn't a set refused
        set(_cli_obdb ${CMAKE_CURRENT_SOURCE_DIR}/cli_obdb_import.py)
        set(_obdb_set ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obdb/example-car.json)
        add_test(NAME CliObdbImportTest.ASetConvertsToTheCommittedFile
            COMMAND ${Python3_EXECUTABLE} ${_cli_obdb} convert $<TARGET_FILE:havne-cli>
                    ${_obdb_set} ${_cli_golden}/obdb-import.jsonl
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/obdb/example-car.json)
        add_test(NAME CliObdbImportTest.AnOutThatCantBeWrittenWritesNothing
            COMMAND ${Python3_EXECUTABLE} ${_cli_obdb} unwritable
                    $<TARGET_FILE:havne-cli> ${_obdb_set})
        set_tests_properties(CliObdbImportTest.ASetConvertsToTheCommittedFile
            CliObdbImportTest.AnOutThatCantBeWrittenWritesNothing PROPERTIES TIMEOUT 60)
        app_cli_golden(SignalsImportRefusesWhatIsntAnObdbSet obdb-not-a-set.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> signals import obdb ${_cli_signals}/extras.json
               --out never-written.json)
        # A format 3 recording (written by hand): an enum's line carries its
        # labels, its columns its raw numbers
        app_cli_golden(ViewShowsAnEnumsLabels view-enum.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/fuel-status --columns 4)
        app_cli_golden(ViewAlignsTwoRecordingsOnARise view-rise.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               ${_cli_pulls}/pull-late --align-on rise:RPM:4000 --hysteresis 500
               --from -2 --to 8 --columns 10 --at 2 --signal RPM --signal VIN)
        app_cli_golden(ViewAlignsOnAMarker view-marker.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               ${_cli_pulls}/pull-late --align-on marker:pull --columns 8
               --signal Throttle)
        app_cli_golden(ViewAlignsOnTheUtcStarts view-utc.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               ${_cli_pulls}/pull-late --align-on utc --columns 4 --signal Coolant)
        app_cli_golden(ViewLeavesOutARecordingItCantAlign view-unaligned.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               --align-on rise:RPM:2010 --hysteresis 30 --columns 2 --signal RPM)
        app_cli_golden(ViewRefusesAFolderWithNoRecording view-no-recording.jsonl
            --exit 3 -- $<TARGET_FILE:havne-cli> view ${_cli_pulls})
        # view --frames (ADR-0049's appendix of 2026-11-06): a SocketCAN
        # listener's wire frames through the profile its header names, the
        # replay fixture (frames.csv alone) through obd2, the packed
        # replies through a signal file extending obd2, and a folder that
        # names no profile
        set(_cli_frames ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay)
        app_cli_golden(ViewFramesDecodesAListenersWireFrames view-frames.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/findings --frames
               --columns 4)
        app_cli_golden(ViewFramesDecodesARecordingWithNoSignals
            view-frames-replay.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_frames}/2026-10-25-engine-start
               --frames --profile obd2 --columns 3 --signal RPM
               --signal "Vehicle Speed")
        app_cli_golden(ViewFramesDecodesThroughASignalFile
            view-frames-signals.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_frames}/packed-replies
               --frames --signals
               ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signal-files/daily-driver-extras.json
               --columns 2)
        app_cli_golden(ViewFramesRefusesAFolderThatNamesNoProfile
            view-frames-no-profile.jsonl
            --exit 3 -- $<TARGET_FILE:havne-cli> view ${_cli_frames}/packed-replies
               --frames)
        add_test(NAME CliViewRunTest.ColumnsCountEverySampleOnce
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py once
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-early
                    ${_cli_pulls}/pull-late)
        add_test(NAME CliViewRunTest.AlignedPullsAgree
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py agree
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-early
                    ${_cli_pulls}/pull-late)
        set_tests_properties(CliViewRunTest.ColumnsCountEverySampleOnce
            CliViewRunTest.AlignedPullsAgree PROPERTIES TIMEOUT 60)
        # info (ADR-0049 section 6) on a recording with one of each finding
        # (tests/fixtures/recording/findings, wire frames written by hand: a
        # gap, a rate drop, a sleep, a late ID, an overflow, a cut First
        # Frame and the link's two lines), the same with the thresholds
        # raised, the replay fixture (frames.csv alone: the reader can't
        # open it, the check still runs) and a folder with no frames.csv
        app_cli_golden(InfoListsWhatsWrongWithARecording info-findings.jsonl
            -- $<TARGET_FILE:havne-cli> info ${_cli_pulls}/findings)
        app_cli_golden(InfoTakesTheThresholds info-thresholds.jsonl
            -- $<TARGET_FILE:havne-cli> info ${_cli_pulls}/findings
               --gap 3 --rate-drop 20 --settle 20)
        app_cli_golden(InfoChecksFramesTheReaderCantOpen info-frames-only.jsonl
            -- $<TARGET_FILE:havne-cli> info
               ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start)
        app_cli_golden(InfoRefusesAFolderWithNoFrames info-no-frames.jsonl
            --exit 3 -- $<TARGET_FILE:havne-cli> info ${_cli_pulls}/pull-early)
        # Ctrl+Break while the fake DLL's disconnect hangs: the recording
        # ends whole without waiting for it (skipped where there's no
        # console, as CliSignalTest is)
        if(WIN32)
            add_test(NAME CliRecordTest.AnInterruptedCloseThatHangsStillEndsTheRecording
                COMMAND ${Python3_EXECUTABLE} ${_cli_record} hang $<TARGET_FILE:havne-cli>
                        $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliRecordTest.AnInterruptedCloseThatHangsStillEndsTheRecording
                PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
    endif()
    function(app_cli_usage name args)
        add_test(NAME CliUsageTest.${name}
            COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:havne-cli> -DEXPECT=2
                    "-DARGS=${args}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_exit.cmake)
    endfunction()
    app_cli_usage(NoArgumentsIsUsage "")
    app_cli_usage(NoUriIsUsage "stream")
    app_cli_usage(AnUnknownCommandIsUsage "listen")
    app_cli_usage(AnUnknownOptionIsUsage "stream|mock://x|--fast")
    app_cli_usage(ACountOfZeroIsUsage "stream|mock://x|--count|0")
    app_cli_usage(TwoUrisIsUsage "stream|mock://a|mock://b")
    app_cli_usage(UntilScannedWithoutAUriIsUsage "stream|--until-scanned")
    app_cli_usage(ALabelWithoutRecordIsUsage "stream|mock://x|--label|lap")
    app_cli_usage(SignalsWithoutAFileIsUsage "stream|mock://x|--signals")
    app_cli_usage(SignalsForAnotherProfileIsUsage
        "stream|mock://x?profile=obd2|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json")
    app_cli_usage(SignalsCheckWithoutAFileIsUsage "signals|check")
    app_cli_usage(SignalsWithoutCheckIsUsage "signals")
    app_cli_usage(SignalsAnUnknownWordIsUsage "signals|lint|x.json")
    app_cli_usage(SignalsCheckTwoFilesIsUsage "signals|check|a.json|b.json")
    app_cli_usage(SignalsImportWithoutOutIsUsage "signals|import|obdb|a.json")
    app_cli_usage(SignalsImportFromAnotherFormatIsUsage
        "signals|import|dbc|a.json|--out|b.json")
    app_cli_usage(SignalsImportAYearBeforeOBDIIIsUsage
        "signals|import|obdb|a.json|--out|b.json|--year|1979")
    app_cli_usage(SignalsImportAnUnknownOptionIsUsage
        "signals|import|obdb|a.json|--out|b.json|--fast")
    app_cli_usage(SimSignalsWithoutAFileIsUsage "sim|mock://sim|--on|vcan0|--signals")
    set(_usage_example ${CMAKE_SOURCE_DIR}/plugins/example-sim)
    app_cli_usage(PluginsWithoutAWordIsUsage "plugins")
    app_cli_usage(PluginsAnUnknownWordIsUsage "plugins|lint|x")
    app_cli_usage(PluginsListWithoutAPackageIsUsage "plugins|list")
    app_cli_usage(PluginsListAnOptionIsUsage "plugins|list|x|--fast")
    app_cli_usage(PluginsCheckWithoutAPackageIsUsage "plugins|check")
    app_cli_usage(PluginsCheckAnUnknownOptionIsUsage "plugins|check|x|--of|vcan0")
    app_cli_usage(PluginsCheckOnWithoutAnInterfaceIsUsage "plugins|check|x|--on")
    app_cli_usage(PluginsCheckAFlagForThePackageIsUsage "plugins|check|--on")
    app_cli_usage(PluginsCheckHelpForThePackageIsUsage "plugins|check|--help")
    app_cli_usage(PluginsMatchAFlagForThePackageIsUsage
        "plugins|match|--x|--vin|ZZZFAKE11K0000001")
    app_cli_usage(PluginsMatchWithoutAVinIsUsage "plugins|match|x")
    app_cli_usage(PluginsMatchNotAVinIsUsage "plugins|match|x|--vin|FAKEJ2534VIN00001")
    app_cli_usage(PluginWithoutAPackageIsUsage "stream|mock://x|--plugin")
    app_cli_usage(PluginWithoutItsVariantIsUsage
        "stream|mock://x|--plugin|${_usage_example}")
    app_cli_usage(PluginForAnotherProfileIsUsage
        "stream|mock://x?profile=obd2|--plugin|${_usage_example}")
    app_cli_usage(PluginAndSignalsIsUsage
        "stream|mock://x?profile=plugin.example-sim.base|--plugin|${_usage_example}|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json")
    app_cli_usage(SimPluginWithoutItsVariantIsUsage
        "sim|mock://sim|--on|vcan0|--plugin|${_usage_example}")
    app_cli_usage(RecordWithoutAFolderIsUsage "stream|mock://x|--record")
    app_cli_usage(VinWithoutOneIsUsage "vin")
    app_cli_usage(NotAVinIsUsage "vin|FAKEJ2534VIN00001")
    app_cli_usage(TwoVinsIsUsage "vin|1FAFAKE10M0000001|1FAFAKE10M0000001")
    app_cli_usage(DtcWithoutOneIsUsage "dtc")
    app_cli_usage(NotADtcIsUsage "dtc|P5301")
    app_cli_usage(TwoDtcsIsUsage "dtc|P0301|P0420")
    app_cli_usage(SimWithoutAnInterfaceIsUsage "sim|mock://sim")
    app_cli_usage(SimWithoutAUriIsUsage "sim|--on|vcan0")
    app_cli_usage(SimSecondsOfZeroIsUsage "sim|mock://sim|--on|vcan0|--seconds|0")
    app_cli_usage(TwoSimUrisIsUsage "sim|mock://a|mock://b|--on|vcan0")
    app_cli_usage(SimOnAndElm327IsUsage "sim|mock://sim|--on|vcan0|--elm327|tcp:0")
    app_cli_usage(SimElm327DeskBusIsUsage "sim|mock://sim|--elm327|tcp:0|--desk-bus")
    app_cli_usage(SimElm327WithoutAnAddressIsUsage "sim|mock://sim|--elm327")
    app_cli_usage(SimElm327NotTcpIsUsage "sim|mock://sim|--elm327|udp:35000")
    app_cli_usage(SimElm327APortPastTheRangeIsUsage "sim|mock://sim|--elm327|tcp:65536")
    app_cli_usage(SimElm327AShortAddressIsUsage "sim|mock://sim|--elm327|tcp:127.0.1:35000")
    app_cli_usage(SimElm327AHostNameIsUsage "sim|mock://sim|--elm327|tcp:localhost:35000")
    app_cli_usage(ClearWithoutYesIsUsage "clear|mock://x")
    app_cli_usage(ClearWithoutAUriIsUsage "clear|--yes")
    app_cli_usage(ClearTwoUrisIsUsage "clear|mock://a|mock://b|--yes")
    app_cli_usage(ClearMovingWithoutRunningIsUsage "clear|mock://x|--yes|--moving")
    app_cli_usage(ClearAnUnknownOptionIsUsage "clear|mock://x|--yes|--force")
    # view's refusals come before any recording opens, but for a name no
    # recording has, which comes after the opens and before any line
    app_cli_usage(ViewWithoutARecordingIsUsage "view")
    app_cli_usage(ViewColumnsOfZeroIsUsage "view|x|--columns|0")
    app_cli_usage(ViewTooManyColumnsIsUsage "view|x|--columns|10001")
    app_cli_usage(ViewAnUnknownAlignmentIsUsage "view|x|--align-on|sideways")
    app_cli_usage(ViewARiseWithoutALevelIsUsage "view|x|--align-on|rise:RPM")
    app_cli_usage(ViewHysteresisWithoutARiseIsUsage
        "view|x|--align-on|marker:pull|--hysteresis|5")
    app_cli_usage(ViewANegativeHysteresisIsUsage
        "view|x|--align-on|rise:RPM:1|--hysteresis|-1")
    app_cli_usage(ViewFromNotBeforeToIsUsage "view|x|--from|5|--to|5")
    app_cli_usage(ViewSecondsThatArentAreUsage "view|x|--at|soon")
    app_cli_usage(ViewAnUnknownOptionIsUsage "view|x|--fast")
    set(_cli_pull_early ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/pull-early)
    app_cli_usage(ViewASignalNoRecordingHasIsUsage
        "view|${_cli_pull_early}|--signal|Boost")
    app_cli_usage(ViewAlignedOnASignalNoRecordingHasIsUsage
        "view|${_cli_pull_early}|--align-on|rise:Boost:1")
    app_cli_usage(ViewARiseOnTextIsUsage "view|${_cli_pull_early}|--align-on|rise:VIN:1")
    app_cli_usage(ViewFromAfterTheDefaultEndIsUsage
        "view|${_cli_pull_early}|--from|100")
    app_cli_usage(ViewAProfileWithoutFramesIsUsage "view|x|--profile|obd2")
    app_cli_usage(ViewSignalsWithoutFramesIsUsage "view|x|--signals|a.json")
    app_cli_usage(ViewAPluginWithoutFramesIsUsage "view|x|--plugin|a")
    app_cli_usage(ViewSignalsAndAPluginIsUsage
        "view|x|--frames|--signals|a.json|--plugin|b")
    app_cli_usage(ViewAProfileThatIsntTheSignalFilesIsUsage
        "view|${_cli_pull_early}|--frames|--profile|obd2|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signal-files/daily-driver-extras.json")
    app_cli_usage(ViewAProfileThatIsntAVariantIsUsage
        "view|${_cli_pull_early}|--frames|--profile|obd2|--plugin|${CMAKE_SOURCE_DIR}/plugins/example-sim")
    app_cli_usage(InfoWithoutARecordingIsUsage "info")
    app_cli_usage(InfoTwoRecordingsIsUsage "info|x|y")
    app_cli_usage(InfoAGapOfZeroIsUsage "info|x|--gap|0")
    app_cli_usage(InfoARateDropOfOneIsUsage "info|x|--rate-drop|1")
    app_cli_usage(InfoSettleThatIsntSecondsIsUsage "info|x|--settle|soon")
    app_cli_usage(InfoAnUnknownOptionIsUsage "info|x|--fast")
    # A connect held past the CLI's deadline ends it (ADR-0031's appendix
    # of 2026-11-01): the fake DLL holds PassThruOpen longer than the
    # deadline
    if(WIN32)
        add_test(NAME CliConnectTest.AConnectPastItsDeadlineEnds
            COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:havne-cli>
                    -DDLL=$<TARGET_FILE:fake_j2534>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_connect_deadline.cmake)
        set_tests_properties(CliConnectTest.AConnectPastItsDeadlineEnds
            PROPERTIES TIMEOUT 30)
    endif()
    # A stop from outside (ADR-0031 section 6): SIGINT and SIGTERM on POSIX;
    # on Windows Ctrl+Break to a child in a process group of its own
    # (Ctrl+C can't be sent to one), skipped where there's no console
    if(UNIX)
        add_test(NAME CliSignalTest.InterruptEndsTheStreamCleanly
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli_signal.sh $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliSignalTest.InterruptEndsTheStreamCleanly PROPERTIES TIMEOUT 30)
    elseif(WIN32 AND Python3_Interpreter_FOUND)
        add_test(NAME CliSignalTest.CtrlBreakEndsTheStreamCleanly
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_ctrl_break.py
                    $<TARGET_FILE:havne-cli> "${_cli_replay}?profile=obd2")
        set_tests_properties(CliSignalTest.CtrlBreakEndsTheStreamCleanly PROPERTIES
            TIMEOUT 30 SKIP_RETURN_CODE 77)
    endif()
endif()

# --- C ABI compilation check: MUST stay Qt-free, builds everywhere ---
# Compiles the public headers as strict C and links exported symbols
# from a C TU: catches C++-isms and name-mangling regressions an FFI
# consumer would hit. It is the only target that compiles headers as C:
# ISO C11 without compiler extensions, and -pedantic where the compiler
# has it, so a GNU-only construct in a header warns.
add_executable(test_c_abi test_c_abi.c)
set_target_properties(test_c_abi PROPERTIES C_EXTENSIONS OFF)
app_enable_warnings(test_c_abi)
if(NOT MSVC)
    target_compile_options(test_c_abi PRIVATE -pedantic)
endif()

target_include_directories(test_c_abi PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_c_abi PRIVATE
    ecuio-core
)

add_test(NAME c_abi COMMAND test_c_abi)
set_tests_properties(c_abi PROPERTIES LABELS "abi")

# Core's USDT probes (ADR-0034 section 4) in each binary that links core and
# ships or runs here: their ELF notes, which is all a tracer reads.
# Only where core was built with them (Linux with <sys/sdt.h>).
if(ECUIO_HAVE_SDT)
    find_program(APP_READELF readelf REQUIRED)
    set(_probe_binaries test_core)
    if(TARGET havne-cli)
        list(APPEND _probe_binaries havne-cli)
    endif()
    foreach(bin IN LISTS _probe_binaries)
        add_test(NAME ProbeNotesTest.${bin}
            COMMAND ${CMAKE_COMMAND} -DREADELF=${APP_READELF}
                    -DBINARY=$<TARGET_FILE:${bin}>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/probe_notes.cmake)
    endforeach()
endif()

# --- Bridge tests: only when the full app (and Qt) is available ---
if(NOT HAVNE_BUILD_CORE_ONLY)

    find_package(Qt6 6.12 COMPONENTS Core Network REQUIRED)   # app/CMakeLists.txt's minimum

    add_executable(test_bridge
        test_bridge.cpp
        test_recording_view.cpp
        ../app/recordingview.cpp
        ../app/sessionbridge.cpp
        ../app/readonce.cpp
        ../app/pluginstore.cpp
        ../app/signalstore.cpp
        ../app/signalfilter.cpp
        ../app/blepicker.cpp
        ../app/btconnect.cpp
        ../app/carryover.cpp
        ../app/adapterrelay.cpp
        fake_bt_socket.h
    )

    set_target_properties(test_bridge PROPERTIES AUTOMOC ON)
    app_enable_warnings(test_bridge)

    # core/src for pid_support.h: core's judgment of the unsupported
    # signals, fed to the model (test_core's precedent)
    target_include_directories(test_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app
        ${CMAKE_SOURCE_DIR}/cli
        ${CMAKE_SOURCE_DIR}/core/src
    )

    # The engine-start fixture, which both decode paths read (ADR-0024),
    # and the recordings RecordingViewTest opens (ADR-0049)
    target_compile_definitions(test_bridge PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
        ECUIO_RECORDING_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording"
        ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
    )

    target_link_libraries(test_bridge PRIVATE
        ecuio-core
        Qt6::Core
        Qt6::Network
        GTest::gtest
    )

    # Copy Qt DLLs at build time
    if(WIN32)
        add_custom_command(TARGET test_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:test_bridge>
                    $<TARGET_FILE_DIR:test_bridge>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying Qt runtime DLLs to test_bridge output dir"
        )
    endif()

    # Listing its cases loads Qt's DLLs first, which on a loaded machine
    # took past the default 5 s and failed the build
    gtest_discover_tests(test_bridge DISCOVERY_TIMEOUT 60)

    # The GUI thread's cost per frame by profile size, a measurement for
    # docs/testing.md; built with the tests, never run by ctest
    add_executable(bench_bridge
        bench_bridge.cpp
        ../app/sessionbridge.cpp
        ../app/readonce.cpp
        ../app/pluginstore.cpp
        ../app/signalstore.cpp
        ../app/signalfilter.cpp
        ../app/carryover.cpp
    )
    set_target_properties(bench_bridge PROPERTIES AUTOMOC ON)
    app_enable_warnings(bench_bridge)
    target_include_directories(bench_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../app
                                                    ${CMAKE_SOURCE_DIR}/cli)
    target_link_libraries(bench_bridge PRIVATE ecuio-core Qt6::Core)
    # The engine-start fixture, replayed as the load test (ADR-0023)
    target_compile_definitions(bench_bridge PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    )
    if(WIN32)
        add_custom_command(TARGET bench_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:bench_bridge> $<TARGET_FILE_DIR:bench_bridge>
            COMMAND_EXPAND_LISTS
        )
    endif()

    # Tell ctest/gtest_discover where Qt DLLs live at runtime
    get_filename_component(QT_BIN_DIR "${Qt6Core_DIR}/../../bin" REALPATH)

    # --- QML tests (Qt Quick Test): the app's App module, linked
    # from app/ (which the root adds after this directory), against a
    # real bridge. Each qml/tst_*.qml file is a ctest case of its own,
    # QmlTest.<name>, in its own process; offscreen and on the software
    # renderer, so no display or GPU is needed.
    find_package(Qt6 6.12 COMPONENTS QuickTest REQUIRED)

    add_executable(test_qml test_qml.cpp)
    set_target_properties(test_qml PROPERTIES AUTOMOC ON)
    app_enable_warnings(test_qml)
    target_link_libraries(test_qml PRIVATE
        app_ui
        app_uiplugin
        Qt6::QuickTest
    )
    # Run with no -input, it runs every file in this directory. The
    # engine-start fixture feeds the Dashboard's peak hold (ADR-0023); the
    # Recordings page opens the recording fixtures (ADR-0049)
    target_compile_definitions(test_qml PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
        ECUIO_RECORDING_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording"
        ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
    )

    # Windows: deployed like the app, into a directory of its own (the
    # other tests' copied Qt DLLs would otherwise point Qt at plugins
    # and QML imports that aren't there), with the offscreen platform
    # plugin, so it runs from a shell as it does under ctest
    if(WIN32)
        set_target_properties(test_qml PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qmltest"
        )
        add_custom_command(TARGET test_qml POST_BUILD
            COMMAND Qt6::windeployqt
                    --no-compiler-runtime
                    --include-plugins qoffscreen
                    --qmldir ${CMAKE_SOURCE_DIR}/app/qml
                    --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml
                    $<TARGET_FILE:test_qml>
            COMMENT "Deploying Qt runtime next to test_qml"
            VERBATIM
        )
    endif()

    set(_qml_test_env QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software)
    if(WIN32)
        # The offscreen platform reads fonts from a directory, not the
        # system's font list
        file(TO_CMAKE_PATH "$ENV{SystemRoot}/Fonts" _qml_fonts)
        list(APPEND _qml_test_env "QT_QPA_FONTDIR=${_qml_fonts}")
    endif()
    file(GLOB _qml_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_*.qml)
    foreach(_qml_test IN LISTS _qml_tests)
        get_filename_component(_qml_name ${_qml_test} NAME_WE)
        string(REGEX REPLACE "^tst_" "" _qml_name ${_qml_name})
        add_test(NAME QmlTest.${_qml_name} COMMAND test_qml -input ${_qml_test})
        set_tests_properties(QmlTest.${_qml_name} PROPERTIES
            ENVIRONMENT "${_qml_test_env}"
            TIMEOUT 60
        )
    endforeach()
endif()

# TIMEOUT POLICY: every J2534 suite carries a per-test 30s ceiling.
# The slowest legitimate case (cross-arch TimedOutOp ~3.5s,
# BlockedVendorOpen ~2.5s) has near-10x headroom, so a slow CI VM
# cannot false-alarm, but a hang in vendor or pipe code (a blocking
# flush, a wedged child) becomes a bounded "Failed (Timeout)" instead
# of a wedged runner. Non-J2534 suites keep ctest defaults:
# no known hang surface there, and mock tests are sub-2s anyway.
set(J2534_TEST_TIMEOUT_SECONDS 30)

# Every J2534 session runs in a surrogate (ADR-0025), found next to the
# running exe as in the app: each test that opens one gets this tree's
# (x64) surrogate beside it. The cross-arch run names the x86 one with
# ECUIO_SURROGATE_EXE instead.
function(app_deploy_surrogate target)
    if(TARGET ecuio-j2534-surrogate)
        add_dependencies(${target} ecuio-j2534-surrogate)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ecuio-j2534-surrogate>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Deploying the surrogate next to ${target}"
        )
    endif()
endfunction()

# J2534 discovery tests (Win32-only)
if(WIN32)
    add_executable(test_j2534_discovery
        ../tests/test_j2534_discovery.cpp
    )
    target_link_libraries(test_j2534_discovery PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    set_target_properties(test_j2534_discovery PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    app_enable_warnings(test_j2534_discovery)
    gtest_discover_tests(test_j2534_discovery
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# Fake J2534 DLL target (Windows only, HARNESS tier)
if(WIN32)
    add_library(fake_j2534 SHARED
        fake_j2534.cpp
        fake_j2534.def
    )

    target_compile_definitions(fake_j2534 PRIVATE
        TEST_HOOKS_ENABLED
    )

    target_include_directories(fake_j2534 PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )

    set_target_properties(fake_j2534 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "fake_j2534"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    )
    app_enable_warnings(fake_j2534)

    # LEGACY-spelling fixture: the fallback-vendor case, exports the
    # legacy 'PassThruIoCtl' spelling via def alias while the shared
    # source defines the spec spelling. The everyday fake_j2534 above
    # exports the spec spelling directly, so primary resolution is what
    # every default test exercises and THIS fixture is what pins the
    # fallback resolver.
    add_library(fake_j2534_legacy SHARED
        fake_j2534.cpp
        fake_j2534_legacy.def
    )

    target_compile_definitions(fake_j2534_legacy PRIVATE
        TEST_HOOKS_ENABLED
    )

    target_include_directories(fake_j2534_legacy PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )

    set_target_properties(fake_j2534_legacy PROPERTIES
        PREFIX ""
        OUTPUT_NAME "fake_j2534_legacy"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    )
    app_enable_warnings(fake_j2534_legacy)
endif()

# test_bridge's connect and disconnect thread tests run a fake J2534
# session, and test_qml's link-health cases drive the UI through one;
# they're compiled only on Windows
if(WIN32 AND NOT HAVNE_BUILD_CORE_ONLY)
    target_compile_definitions(test_bridge PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_bridge fake_j2534)
    app_deploy_surrogate(test_bridge)
    target_compile_definitions(test_qml PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_qml fake_j2534)
    app_deploy_surrogate(test_qml)
endif()

# J2534 loader tests (Win32-only, HARNESS tier: open, exports and close
# against the fake vendor DLL, through the surrogate; path baked in at
# generation time)
if(WIN32)
    add_executable(test_j2534_load
        ../tests/test_j2534_load.cpp
    )
    target_link_libraries(test_j2534_load PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_load PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
        FAKE_J2534_LEGACY_DLL_PATH="$<TARGET_FILE:fake_j2534_legacy>"
    )
    add_dependencies(test_j2534_load fake_j2534)
    add_dependencies(test_j2534_load fake_j2534_legacy)
    app_deploy_surrogate(test_j2534_load)
    app_enable_warnings(test_j2534_load)
    gtest_discover_tests(test_j2534_load
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 end-to-end stream tests (Win32-only, HARNESS tier)
if(WIN32)
    add_executable(test_j2534_stream
        ../tests/test_j2534_stream.cpp
    )
    # core/src for j2534_clock.h, which is internal (test_obd2 precedent).
    target_include_directories(test_j2534_stream PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src
    )
    target_link_libraries(test_j2534_stream PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_stream PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_stream fake_j2534)
    app_deploy_surrogate(test_j2534_stream)
    app_enable_warnings(test_j2534_stream)
    gtest_discover_tests(test_j2534_stream
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 surrogate host tests (Win32-only, HARNESS tier: real process
# spawn: named pipe + shared ring + kill-on-close job, end to end)
# Arch note: in this tree's configure this is the all-same-arch IPC test
# (CI-valid); the true cross-arch case (x64 test -> x86 surrogate -> x86
# fake) runs on a dev machine with ECUIO_SURROGATE_EXE pointing at
# artifacts from a second, Win32 (-A Win32) configure of the same tree.
if(WIN32)
    add_executable(test_j2534_surrogate
        test_j2534_surrogate.cpp
    )
    target_link_libraries(test_j2534_surrogate PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_surrogate PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_surrogate fake_j2534)
    # The client's resolve_exe_path() finds it in its own image
    # directory (same discipline as the baked fake-DLL path: no PATH
    # games).
    app_deploy_surrogate(test_j2534_surrogate)

    app_enable_warnings(test_j2534_surrogate)
    # RUN_SERIAL: EarlierSessionLogIsKept needs the surrogate's fixed log
    # name, which any other test's surrogate takes first under ctest -j
    # (it then skips, and verify.sh's cross-arch step fails on a skip).
    # The suite is about 14 s; CMake 3.22's TEST_FILTER could serialize
    # that one case alone.
    gtest_discover_tests(test_j2534_surrogate
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS} RUN_SERIAL TRUE
    )
endif()

# J2534 link-health tests (Win32-only, HARNESS tier): every case runs a
# session through the surrogate: in this tree the x64 one beside the
# test, in the cross-arch run the Win32 tree's.
if(WIN32)
    add_executable(test_j2534_health
        test_j2534_health.cpp
    )
    target_link_libraries(test_j2534_health PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_health PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
        SURROGATE_EXE_PATH="$<TARGET_FILE:ecuio-j2534-surrogate>"
    )
    add_dependencies(test_j2534_health fake_j2534)
    app_deploy_surrogate(test_j2534_health)
    app_enable_warnings(test_j2534_health)
    gtest_discover_tests(test_j2534_health
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 connect-failure tests (Win32-only, HARNESS tier): why a connect
# failed, set up as for test_j2534_health.
if(WIN32)
    add_executable(test_j2534_connect
        test_j2534_connect.cpp
    )
    target_link_libraries(test_j2534_connect PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_connect PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
        SURROGATE_EXE_PATH="$<TARGET_FILE:ecuio-j2534-surrogate>"
    )
    add_dependencies(test_j2534_connect fake_j2534)
    add_dependencies(test_j2534_connect fake_j2534_legacy)   # beside the fake
    app_deploy_surrogate(test_j2534_connect)
    app_enable_warnings(test_j2534_connect)
    gtest_discover_tests(test_j2534_connect
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 routing tests (Win32-only, HARNESS tier: the surrogate chosen by
# PE architecture: the x64 one for x64 (CI), the x86 one for x86
# (dev-machine, env-gated via ECUIO_FAKE_DLL/ECUIO_SURROGATE_EXE))
if(WIN32)
    add_executable(test_j2534_routing
        test_j2534_routing.cpp
    )
    target_link_libraries(test_j2534_routing PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_routing PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_routing fake_j2534)
    app_deploy_surrogate(test_j2534_routing)
    app_enable_warnings(test_j2534_routing)
    gtest_discover_tests(test_j2534_routing
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# Reply timing against the fakes' ECU through each transport this
# platform has (ADR-0042 section 4), a measurement for docs/testing.md;
# built with the tests, never run by ctest
find_package(Threads REQUIRED)
add_executable(bench_timing bench_timing.cpp)
target_link_libraries(bench_timing PRIVATE ecuio-core Threads::Threads)
app_enable_warnings(bench_timing)
if(WIN32)
    target_compile_definitions(bench_timing PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(bench_timing fake_j2534)
    app_deploy_surrogate(bench_timing)
    target_link_libraries(bench_timing PRIVATE winmm)   # timeBeginPeriod
endif()
