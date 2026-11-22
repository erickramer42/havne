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
add_executable(test_core test_core.cpp test_core_dispatch.cpp
    test_core_drive_cycle.cpp test_core_clock_profile.cpp test_core_replay.cpp
    test_core_replay_faults.cpp test_core_values.cpp test_core_chains.cpp
    test_core_demo_pids.cpp test_core_link.cpp test_core_vin_focus.cpp
    test_json.cpp test_obdb_import.cpp test_bench_load_judge.cpp
    test_recorder.cpp test_recorder_replay.cpp test_recorder_focus.cpp
    test_recorder_attach.cpp test_recorder_file.cpp
    test_recording.cpp test_recording_reader.cpp
    test_recording_hour.cpp test_recording_stats.cpp
    test_recording_stretches.cpp test_recording_check.cpp
    test_recording_frames.cpp test_candump_import.cpp test_export.cpp
    test_export_csv.cpp test_export_folder.cpp test_export_range.cpp
    test_export_frames.cpp test_export_cancel.cpp
    test_recording_keys.cpp
    test_sha256.cpp test_queue.cpp test_health.cpp test_health_recording.cpp
    test_health_baseline.cpp test_csv_import.cpp test_csv_import_recording.cpp
    test_frames_read.cpp test_csv_guess.cpp test_csv_guess_rows.cpp
    test_csv_guess_call.cpp test_recording_aids.cpp
    test_condition_session.cpp test_condition_session_link.cpp
    test_condition_recording.cpp test_condition_recording_events.cpp
    test_armed_recorder.cpp test_pick_only.cpp test_query_keys.cpp
    test_key_window_setup.cpp test_security_store.cpp
    test_security_store_file.cpp)

# core/src for mock_drive_cycle.h and link_health.h, which are internal
# (test_obd2 precedent).
target_include_directories(test_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_core PRIVATE
    ecuio-core
    GTest::gtest_main
)

# The scrubbed real-bus recording replay:// plays (ADR-0023), the
# signal file a recording copies (ADR-0051 section 4), and a recording of
# values alone (the health report, ADR-0054); the made-up loggers' CSV
# files an import reads (ADR-0054 section 5)
target_compile_definitions(test_core PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    ECUIO_SIGNAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals"
    ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
    ECUIO_GOLDEN_RECORDING="${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2"
    ECUIO_IMPORT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import"
)

app_enable_warnings(test_core)
gtest_discover_tests(test_core DISCOVERY_TIMEOUT 60)
# Discovery runs each test binary at build time, and its default 5 s
# is less than a cold runner can take: every call names 60 s, which
# tests/discovery_timeout.cmake checks across this file
add_test(NAME DiscoveryTimeoutTest.EveryDiscoveryWaits
    COMMAND ${CMAKE_COMMAND} -DFILE=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/discovery_timeout.cmake)

# The notices each download carries (LGPL-3.0 section 4): the APK's are
# committed copies of their sources, and every notice holds core's data
# notices (tests/notices_match.cmake); make-appimage.sh checks the
# AppImage's as it packs
add_test(NAME NoticesTest.TheApkCarriesEachNoticeAsItsSource
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DMODE=apk
            -P ${CMAKE_CURRENT_SOURCE_DIR}/notices_match.cmake)
add_test(NAME NoticesTest.EachNoticeCarriesCoresData
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DMODE=core
            -P ${CMAKE_CURRENT_SOURCE_DIR}/notices_match.cmake)
add_test(NAME NoticesTest.EachCitedLicenseIsInTheTree
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DMODE=licenses
            -P ${CMAKE_CURRENT_SOURCE_DIR}/notices_match.cmake)
add_test(NAME NoticesTest.EachBundledFontIsNamed
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DMODE=fonts
            -P ${CMAKE_CURRENT_SOURCE_DIR}/notices_match.cmake)

# --- The reader's open, timed (ADR-0049 section 9): a measurement, not in
# ctest. Qt-free, so an NDK tree builds it alone for the test phone
# (docs/performance.md) ---
add_executable(bench_recording bench_recording.cpp)
target_include_directories(bench_recording PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(bench_recording PRIVATE ecuio-core)
target_compile_definitions(bench_recording PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
)
app_enable_warnings(bench_recording)

# --- Signals tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_signals test_signals.cpp test_signals_text.cpp test_signal_tables.cpp test_computed.cpp test_signal_file.cpp test_signal_file_gauge.cpp test_signal_expr.cpp test_signal_expr_checks.cpp
    test_signal_eval.cpp
    test_signal_keys.cpp
    test_computed_grammar.cpp
    test_plugin.cpp test_plugin_variants.cpp
    test_plugin_car.cpp test_plugin_wrap.cpp test_plugin_put.cpp
    test_plugin_least_core.cpp test_plugin_conversation.cpp test_plugin_pages.cpp
    test_plugin_conversation_rules.cpp test_plugin_conversation_gate.cpp test_base64.cpp
    test_condition_file.cpp test_condition_file_resolve.cpp
    test_condition_eval.cpp)

# test_signal_tables.cpp and test_computed.cpp check computed.h, the
# programs' checker and stack machine, which are internal
target_include_directories(test_signals PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_signals PRIVATE
    ecuio-core
    GTest::gtest_main
)
# test_plugin.cpp loads the repository's example plugin
target_compile_definitions(test_signals PRIVATE
    ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
    ECUIO_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

app_enable_warnings(test_signals)
gtest_discover_tests(test_signals DISCOVERY_TIMEOUT 60)

# --- OBD-II set + request engine tests: MUST stay Qt-free (UNIT tier) ---
# obd2_signal_sets.cpp is ALL-platform (signal data is transport-
# agnostic), and request_engine.h is header-only, so this suite builds
# and runs in the headless Ubuntu job alongside test_core/test_signals.
# The explicit core/src include is required because request_engine.h is
# INTERNAL: ecuio-core's PUBLIC include dirs cover only core/include,
# and the engine is deliberately not part of the public tree.
add_executable(test_obd2 test_obd2.cpp
    test_obd2_engine.cpp
    test_obd2_walk.cpp
    test_obd2_scan.cpp
    test_obd2_identification.cpp
    test_obd2_diagnostics.cpp
    test_obd2_clear.cpp
    test_obd2_packing.cpp
    test_obd2_pid_support.cpp
    test_obd2_sim_ecu.cpp
    test_obd2_sim_dids.cpp
    test_obd2_sim_ask.cpp
    test_obd2_focus.cpp
    test_session_control.cpp test_session_control_late.cpp
    test_request_table.cpp test_tier2_ceiling.cpp test_sim_security.cpp
    test_engine_talk.cpp test_key_window.cpp test_request_turns.cpp
    test_request_turns_key.cpp test_request_turns_session.cpp
    test_request_pack.cpp test_reply_segmented.cpp test_request_quiet.cpp
    test_j2534_hold.cpp test_j2534_hold_session.cpp test_j2534_hold_key.cpp
    test_j2534_hold_order.cpp test_j2534_hold_absent.cpp test_sim_car.cpp
    test_sim_timing.cpp
    test_scan_retry.cpp test_security_access.cpp)

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
gtest_discover_tests(test_obd2 DISCOVERY_TIMEOUT 60)

# --- ELM327 tests: MUST stay Qt-free (UNIT and HARNESS tiers, ADR-0026) ---
# test_elm327.cpp: elm327_protocol.h, header-only and internal like
# request_engine.h, which it also includes to hand it the replies.
# test_elm327_session.cpp: the transport against the fake ELM327 behind
# internal.h's port opener, and on POSIX the OS's port over a
# pseudo-terminal (elm327_port.h); it is in parts by theme
# (test_elm327_session_*.cpp, helpers in test_elm327_support.h), and
# test_elm327_session_alloc.cpp holds the binary's new_counter.h.
# test_serial_ports.cpp: serial-port discovery (serial.h, serial_ports.h).
# test_elm327_talk.cpp: a talk through the test seam, against the fake.
# test_elm327_turns.cpp: which ECU asks next (request_turns.h), against it.
# test_elm327_pack.cpp: packs sized by their reply's frames, against it.
add_executable(test_elm327 test_elm327.cpp test_elm327_session.cpp
    test_elm327_session_diag.cpp test_elm327_session_close.cpp
    test_elm327_session_scan.cpp test_elm327_session_listen.cpp
    test_elm327_session_uri.cpp test_elm327_session_ports.cpp
    test_elm327_session_stream.cpp test_elm327_session_links.cpp
    test_elm327_session_alloc.cpp
    test_serial_ports.cpp test_elm327_talk.cpp test_elm327_turns.cpp
    test_elm327_pack.cpp test_elm327_gate.cpp)
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
gtest_discover_tests(test_elm327 DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 30)

# --- The sandbox: Qt-free, every platform (UNIT, ADR-0066 section 2) ---
# test_wamr_build.cpp asks the vendored WAMR itself (third_party/wamr.cmake)
# what core relies on; its header comes from WAMR_INCLUDE_DIR.
# test_wasm_check.cpp, test_sandbox.cpp and test_sandbox_budget.cpp:
# core/src's wasm_check.h and wasm_sandbox.h, internal. verify.sh's
# cross-arch step builds and runs them all against the x86 core, which CI
# never builds.
add_executable(test_sandbox test_wamr_build.cpp test_wasm_check.cpp
    test_sandbox.cpp test_sandbox_budget.cpp)
target_include_directories(test_sandbox PRIVATE ${WAMR_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/core/src)
target_compile_definitions(test_sandbox PRIVATE ${WAMR_EMBED_DEFINITIONS})
target_link_libraries(test_sandbox PRIVATE ecuio-core GTest::gtest_main)
app_enable_warnings(test_sandbox)
gtest_discover_tests(test_sandbox DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

# --- SocketCAN's rules: Qt-free, every platform (UNIT, ADR-0032, ADR-0040) ---
# socketcan_wire.h is internal and portable (the kernel's values mirrored),
# so its rules test everywhere; the transport on a bus is test_vcan's.
# test_isotp.cpp: isotp.h, the ISO-TP that SocketCAN requests run on.
add_executable(test_socketcan test_socketcan.cpp test_socketcan_clock.cpp
    test_isotp.cpp test_socketcan_send.cpp)
target_include_directories(test_socketcan PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test_socketcan PRIVATE ecuio-core GTest::gtest_main)
# the simulated ECU loads the replay fixture (SimTest, Linux)
target_compile_definitions(test_socketcan PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay")
app_enable_warnings(test_socketcan)
gtest_discover_tests(test_socketcan DISCOVERY_TIMEOUT 60)

# --- The simulated CAN bus: Qt-free, Linux only (SocketCAN) ---
# test_vcan.cpp: vcan_bus.h's fake ECU through the kernel's ISO-TP and a
# recording played onto vcan0. Each case skips without the interface;
# CI's linux-build brings it up and sets ECUIO_VCAN_REQUIRED=1.
# test_vcan_socketcan.cpp: the SocketCAN transport on that bus
# (ADR-0032, SocketCanBusTest); it holds the binary's new_counter.h.
# test_vcan_requests.cpp: socketcan:// requesting through ISO-TP against
# that ECU (ADR-0040, SocketCanRequestTest), and against the test car's
# recording as the ECU, its replies broken at the ISO-TP level
# (vcan_recording_ecu.h, ADR-0041 section 4, SocketCanFaultTest); it is in
# parts by theme (test_vcan_requests_*.cpp, helpers in
# test_vcan_requests_support.h).
# test_vcan_isotp.cpp: core's ISO-TP against the kernel's
# (IsoTpConformanceTest).
# test_vcan_talk.cpp: a request longer than a frame through a requesting
# session, to the kernel's ISO-TP and the simulated ECU's (ADR-0066
# section 2.4, SocketCanTalkTest).
# test_vcan_turns.cpp: two ECUs' requests overlapping, so a busy ECU's
# pace doesn't wait on another's (SocketCanTurnsTest).
# RESOURCE_LOCK vcan0: the cases share the bus, and an error frame passes
# every session's ID filter; every vcan0 user takes the lock, so they run
# one at a time while the rest of the suite runs beside them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_vcan test_vcan.cpp test_vcan_socketcan.cpp
                   test_vcan_requests.cpp test_vcan_requests_focus.cpp
                   test_vcan_requests_close.cpp
                   test_vcan_requests_simbus.cpp test_vcan_isotp.cpp
                   test_vcan_talk.cpp test_vcan_turns.cpp test_vcan_car.cpp)
    target_compile_definitions(test_vcan PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
        ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(test_vcan PRIVATE ecuio-core GTest::gtest_main Threads::Threads)
    # core/src: isotp.h, pure and internal (test_socketcan's precedent)
    target_include_directories(test_vcan PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
    app_enable_warnings(test_vcan)
    gtest_discover_tests(test_vcan DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 30 RESOURCE_LOCK vcan0)
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
    # The names a CTest include sets properties on still exist (the include
    # ignores a missing one)
    add_test(NAME CtestIncludeNamesTest.TheJ2534TimeoutNamesExist
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ctest_include_names.py)
    set_tests_properties(CtestIncludeNamesTest.TheJ2534TimeoutNamesExist PROPERTIES TIMEOUT 60)
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
    # scripts/verify_scope.py, which steps verify.sh --changed takes
    add_test(NAME VerifyScopeTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/verify_scope.py selftest)
    # scripts/docs_only.py, which diffs CI runs and the release gate takes as docs alone
    add_test(NAME DocsOnlyTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/docs_only.py selftest)
    # scripts/tidy-size-ratchet.sh over made-up size lines and baselines
    # (skip 77 where no bash is found)
    add_test(NAME TidyRatchetTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tidy_ratchet_selftest.py)
    set_tests_properties(TidyRatchetTest.SelfTest PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # scripts/filelength_check.py, the file-length ratchet (skip 77 where no git is found)
    add_test(NAME FileLengthTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/filelength_check.py selftest)
    set_tests_properties(FileLengthTest.SelfTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # packaging/sbom.py, the release's SBOM (ADR-0035 section 5)
    add_test(NAME SbomTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/packaging/sbom.py selftest)
    # scripts/vendored_check.py: third_party/wamr against its committed
    # list of hashes, with no network. A change to that folder is a patch
    # beside it, never an edit in it (third_party/README.md)
    add_test(NAME VendoredTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/vendored_check.py selftest)
    add_test(NAME VendoredTest.WamrIsItsListedBytes
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/vendored_check.py check)
    # packaging/python/make_wheel.py, the havne wheel (ADR-0053 section 6):
    # made-up PE, ELF and Mach-O files and a made-up package
    add_test(NAME WheelTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/packaging/python/make_wheel.py selftest)
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
    # Every Text and Label in app/qml sets textFormat: a text's <img> loads
    # a file: URL's host with no interceptor asked, so outside text is plain
    add_test(NAME PlainTextPinTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/plaintext_pin.py --selftest)
    add_test(NAME PlainTextPinTest.EveryTextSetsItsFormat
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/plaintext_pin.py
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
    add_executable(test_cli test_cli.cpp test_cli_view.cpp test_cli_files.cpp
        test_cli_signals.cpp test_cli_elm327.cpp test_cli_transfer.cpp
        test_cli_health.cpp test_cli_partial.cpp test_cli_frames.cpp
        test_cli_log.cpp test_cli_layout.cpp test_cli_conditions.cpp
        test_cli_expr.cpp test_cli_pages.cpp test_cli_pairs.cpp
        test_cli_summary.cpp test_cli_emulator.cpp)
    target_include_directories(test_cli PRIVATE ${CMAKE_SOURCE_DIR}/cli)
    # The layout corpus havne-cli's check and the app's DashboardBook both read
    target_compile_definitions(test_cli PRIVATE
        ECUIO_LAYOUT_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/layouts")
    find_package(Threads REQUIRED)
    target_link_libraries(test_cli PRIVATE ecuio-core GTest::gtest_main Threads::Threads)
    app_enable_warnings(test_cli)
    # The layout corpus nests 1024 levels: havne-cli's stack (cli/CMakeLists.txt)
    if(MSVC)
        target_link_options(test_cli PRIVATE /STACK:8388608)
    endif()
    gtest_discover_tests(test_cli DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 30)

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
        # A session of more modules than its info lists itself: the
        # session line names each one (ADR-0065)
        app_cli_golden(ASessionLineNamesEveryModule stream-modules.jsonl --ignore t_us
            -- $<TARGET_FILE:havne-cli> stream
               "mock://golden?profile=uds-generic&diagnose=0&did=7E8:D100:u8,7E9:D101:u8,7EA:D102:u8,7EB:D103:u8,7EC:D104:u8,7ED:D105:u8"
               --count 8)
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
        app_cli_elm327(AHostThatComesBackAtOnceIsNeverRefused reconnect)
        app_cli_elm327(CoresOwnSessionReadsTheSimulatedCar session)
        app_cli_elm327(ASecondModulesDidAnswers modules)
        app_cli_elm327(APluginsDidsAnswerOnBothEcus plugin)
        app_cli_elm327(AFaultAimedAtOneModuleSilencesItAlone aimed)
        app_cli_elm327(TheMadeUpCarsModulesAllAnswer car)
        set_tests_properties(CliElm327RunTest.TheMadeUpCarsModulesAllAnswer PROPERTIES TIMEOUT 180)
        app_cli_elm327(PythonObdReadsIt python-obd)
        set_tests_properties(CliElm327RunTest.PythonObdReadsIt PROPERTIES COST 13)
        # A clear (ADR-0038's appendix of 2026-11-06): the mock's report
        # first, its clear at the key-on's 0 rpm, the report read again;
        # and with the word on the engine, its 0 km/h read too
        app_cli_golden(ClearClearsTheMock clear-mock.jsonl
            -- $<TARGET_FILE:havne-cli> clear mock://golden --yes)
        app_cli_golden(ClearWithTheWordOnTheEngine clear-mock-running.jsonl
            -- $<TARGET_FILE:havne-cli> clear mock://golden --yes --engine-running)
        app_cli_golden(ClearRefusesABadUri bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> clear "mock://golden?speed=2" --yes)
        # export and import (ADR-0054): a candump log in, read through
        # obd2 and out to pcapng, read back by struct; the refusals; and
        # tshark's reading where it's installed (skip 77)
        foreach(_mode roundtrip refusals tshark)
            add_test(NAME CliTransferRunTest.${_mode}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer.py
                        ${_mode} $<TARGET_FILE:havne-cli>)
            set_tests_properties(CliTransferRunTest.${_mode} PROPERTIES
                TIMEOUT 60 SKIP_RETURN_CODE 77)
        endforeach()
        # The CSV export of a recording core wrote, read back by Python's
        # csv module row for row against its signals.csv
        add_test(NAME CliTransferRunTest.csv
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer.py
                    csv $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliTransferRunTest.csv PROPERTIES TIMEOUT 60)
        # Folder copies, each read and replayed as its source: the
        # findings fixture, a recording of values alone, a mock's
        add_test(NAME CliTransferRunTest.folder
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer.py
                    folder $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/findings
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliTransferRunTest.folder PROPERTIES TIMEOUT 60)
        # An export to pcapng, a folder copy, a CSV's sizing, a wide CSV and
        # its sizing during their named columns' check, and an import of a
        # candump log and of a CSV, each interrupted mid-run: an interrupted
        # end, nothing left behind
        add_test(NAME CliTransferRunTest.interrupt
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer_interrupt.py
                    interrupt $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliTransferRunTest.interrupt PROPERTIES
            TIMEOUT 120 SKIP_RETURN_CODE 77)
        # A second interrupt, with the first held off, ends an export, an
        # import and a recording's health report at once: the end line,
        # exit 1
        add_test(NAME CliTransferRunTest.interrupt-twice
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer_interrupt.py
                    interrupt-twice $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliTransferRunTest.interrupt-twice PROPERTIES
            TIMEOUT 120 SKIP_RETURN_CODE 77)
        # Interrupts once core's call has returned change nothing: the
        # result line whole, exit 0, the file in place
        add_test(NAME CliTransferRunTest.interrupt-late
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer_interrupt.py
                    interrupt-late $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliTransferRunTest.interrupt-late PROPERTIES
            TIMEOUT 120 SKIP_RETURN_CODE 77)
        # The made-up loggers' CSVs imported by their column maps and
        # viewed; a missing map, a refused one, a folder already there
        add_test(NAME CliTransferRunTest.csv-import
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer_import.py
                    csv-import $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import/csv)
        set_tests_properties(CliTransferRunTest.csv-import PROPERTIES TIMEOUT 60)
        # The same CSVs with no map: guessed, printed as committed, imported;
        # a time with no unit and a file with no header refused
        add_test(NAME CliTransferRunTest.csv-guess
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer_import.py
                    csv-guess $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import/csv)
        set_tests_properties(CliTransferRunTest.csv-guess PROPERTIES TIMEOUT 60)
        # The health report (ADR-0054): the mock's, a session whose
        # reports are never asked, the simulated car as an ELM327 adapter
        # on loopback; a recording of values alone, a range past its end
        # and its baseline named
        add_test(NAME CliHealthRunTest.session
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_health.py
                    session $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliHealthRunTest.session PROPERTIES TIMEOUT 180 COST 39)
        add_test(NAME CliHealthRunTest.recording
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_health.py
                    recording $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2)
        set_tests_properties(CliHealthRunTest.recording PROPERTIES TIMEOUT 60)
        # Output that fails only at its flush (standard output a file open
        # for reading) is exit 1 with words, for each command that writes
        # a few lines and returns
        add_test(NAME CliExitTest.AFailedWriteIsExit1
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_write_fail.py
                    $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/pull-early
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/findings
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start)
        set_tests_properties(CliExitTest.AFailedWriteIsExit1 PROPERTIES TIMEOUT 120)
        # A candump -L log of the simulated car on vcan0 imported and read
        # through obd2: its values the direct session's (Linux; skipped
        # without vcan0 or can-utils unless ECUIO_VCAN_REQUIRED; serial:
        # the sim answers 0x7E0 for the bus)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliTransferRunTest.candump
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_transfer.py
                        candump $<TARGET_FILE:havne-cli>)
            set_tests_properties(CliTransferRunTest.candump PROPERTIES
                TIMEOUT 90 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0)
        endif()
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
            # With ?session=03 the stream names each module's session
            # (ADR-0060): the engine's entered, a second module's refused
            add_test(NAME CliSessionRunTest.TheStreamNamesEachModulesSession
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        session $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliSessionRunTest.TheStreamNamesEachModulesSession
                PROPERTIES TIMEOUT 60)
            # The vehicle line comes once the VIN is in, the rest of the
            # identification still out (ADR-0027 section 3)
            add_test(NAME CliVinRunTest.TheStreamNamesTheVehicleWhileItsIdentificationIsOut
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        vehicle $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliVinRunTest.TheStreamNamesTheVehicleWhileItsIdentificationIsOut
                PROPERTIES TIMEOUT 60)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliRefusedRunTest.ADidTheSimulatedEcuRefusesIsNamed
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_refused.py
                        sim $<TARGET_FILE:havne-cli>)
            set_tests_properties(CliRefusedRunTest.ADidTheSimulatedEcuRefusesIsNamed
                PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0)
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
                PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0)
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
        # A recording's conditions.json a replay can't take (ADR-0063
        # section 8): changed, removed, refused by the loader, or the
        # recording unopened; each said on stderr, the replay judging none
        add_test(NAME CliReplayTest.ACopyOfConditionsItCantTakeIsSaid
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cli_replay_conditions.py
                    $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/conditions)
        set_tests_properties(CliReplayTest.ACopyOfConditionsItCantTakeIsSaid
            PROPERTIES TIMEOUT 60)
        # --conditions on stream and log (ADR-0063): a condition on a PID
        # the mock never sends said not received on a later conditions
        # line; a log's recording holding the copy and every event
        set(_cli_conditions_run ${CMAKE_CURRENT_SOURCE_DIR}/cli_conditions_run.py)
        add_test(NAME CliConditionsRunTest.ASignalNeverReceivedIsSaidOnALaterLine
            COMMAND ${Python3_EXECUTABLE} ${_cli_conditions_run} unread
                    $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/conditions)
        add_test(NAME CliConditionsRunTest.ALogRecordsItsConditions
            COMMAND ${Python3_EXECUTABLE} ${_cli_conditions_run} log
                    $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/conditions)
        set_tests_properties(
            CliConditionsRunTest.ASignalNeverReceivedIsSaidOnALaterLine
            CliConditionsRunTest.ALogRecordsItsConditions
            PROPERTIES TIMEOUT 60)
        # A live stream whose reader stalls drops condition lines, and the
        # end line counts each one: the lines received and the count are
        # the recording's cond lines (about 60 s: the rev limiter at 51-56 s)
        # The script bounds a wedged child at 150 s itself: ctest's bound is
        # past that, so a wedge ends in the script's words
        add_test(NAME CliConditionsRunTest.ADroppedEventIsCountedOnTheEndLine
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cli_conditions_drop.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliConditionsRunTest.ADroppedEventIsCountedOnTheEndLine
            PROPERTIES TIMEOUT 200 COST 70)
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
        # log (ADR-0056 section 7): the mock logged for 2 s with two picks,
        # the log line's period shortened by the test knob; the folder's
        # focus line, and an export with no --signal taking those columns
        set(_cli_log ${CMAKE_CURRENT_SOURCE_DIR}/cli_log.py)
        add_test(NAME CliLogRunTest.TheLogRecordsThePicksAndTheExportTakesThem
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} demo $<TARGET_FILE:havne-cli>)
        # Its ends: a folder that can't be made, a replay's last frame, an
        # interrupt (skip 77 without a console on Windows), the output
        # closed under it and under stream --record (the recording whole
        # either way)
        add_test(NAME CliLogRunTest.AMinimumTimeBetweenSweepsLimitsTheRows
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} pace
                $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliLogRunTest.AMinimumTimeBetweenSweepsLimitsTheRows
            PROPERTIES TIMEOUT 120)
        # --pick ranked (ADR-0056's default pick): log and stream ask the
        # ranked signals and a recording names them on its pick line
        add_test(NAME CliLogRunTest.PickRankedAsksTheRankedSignals
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} pick
                $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliLogRunTest.PickRankedAsksTheRankedSignals
            PROPERTIES TIMEOUT 120)
        # A set past a rotation is asked only its picks (ADR-0065): the
        # session line, stream's and log's words and log's refusal
        add_test(NAME CliLogRunTest.APickOnlySessionSaysNothingIsAsked
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} pick-only
                $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliLogRunTest.APickOnlySessionSaysNothingIsAsked
            PROPERTIES TIMEOUT 120)
        add_test(NAME CliLogRunTest.AFolderThatCantBeMadeEndsTheLog
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} fails $<TARGET_FILE:havne-cli>)
        add_test(NAME CliLogRunTest.AReplayEndsTheLogAtItsLastFrame
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} replay $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start)
        add_test(NAME CliLogRunTest.TheSummaryAgreesWithTheRecording
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} summary $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start)
        add_test(NAME CliLogRunTest.AnArmedLogWritesAFolderPerWindow
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} armed $<TARGET_FILE:havne-cli>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start)
        add_test(NAME CliLogRunTest.AnInterruptEndsTheLogWhole
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} interrupt $<TARGET_FILE:havne-cli>)
        add_test(NAME CliLogRunTest.AClosedOutputEndsLogAndStreamQuietly
            COMMAND ${Python3_EXECUTABLE} ${_cli_log} closed-pipe $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliLogRunTest.TheLogRecordsThePicksAndTheExportTakesThem
            CliLogRunTest.AFolderThatCantBeMadeEndsTheLog
            CliLogRunTest.AReplayEndsTheLogAtItsLastFrame
            CliLogRunTest.TheSummaryAgreesWithTheRecording
            CliLogRunTest.AnArmedLogWritesAFolderPerWindow
            CliLogRunTest.AClosedOutputEndsLogAndStreamQuietly
            PROPERTIES TIMEOUT 60)
        set_tests_properties(CliLogRunTest.AnInterruptEndsTheLogWhole
            PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        if(WIN32)
            # A focus the session can't ask (a ?poll= session, a pick that
            # isn't the polled PID) against the fake DLL
            add_test(NAME CliLogRunTest.AFocusTheSessionCantAskIsAnError
                COMMAND ${Python3_EXECUTABLE} ${_cli_log} focus-refused
                        $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>)
            set_tests_properties(CliLogRunTest.AFocusTheSessionCantAskIsAnError
                PROPERTIES TIMEOUT 60)
            # An extended session's state: the session line stream writes
            add_test(NAME CliLogRunTest.ALogWritesTheSessionLineStreamDoes
                COMMAND ${Python3_EXECUTABLE} ${_cli_log} session
                        $<TARGET_FILE:havne-cli> $<TARGET_FILE:fake_j2534>
                        ${CMAKE_CURRENT_SOURCE_DIR}/golden/cli/log-session.jsonl)
            set_tests_properties(CliLogRunTest.ALogWritesTheSessionLineStreamDoes
                PROPERTIES TIMEOUT 60)
        endif()
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
        # A dashboard layout file's check (ADR-0057's appendix on the layout
        # file): one that reads and names its signal files, one of the other
        # form (refused, exit 1), a file with no form named and no --form
        # (read as desktop, saying so) and a file that isn't there
        set(_cli_layouts ${CMAKE_CURRENT_SOURCE_DIR}/data/layouts)
        app_cli_golden(LayoutCheckReadsAFileAndNamesItsSignalFiles layout-check.jsonl
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/desktop-ok-with-files.json
               --form desktop)
        app_cli_golden(LayoutCheckRefusesTheOtherForm layout-check-other-form.jsonl --exit 1
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/phone-file-on-desktop.json
               --form desktop)
        app_cli_golden(LayoutCheckTakesTheFilesFormOrAssumesDesktop layout-check-no-form.jsonl
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/format1-no-form.json)
        app_cli_golden(LayoutCheckCountsWhatLoadingDrops layout-check-dropped.jsonl
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/page-dropped.json)
        app_cli_golden(LayoutCheckNamesAMissingFile layout-check-missing.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/none.json)
        # The Dashboard's presets (app/layouts, compiled into the app): each
        # file reads whole for its form, nothing dropped, no plugin's file
        foreach(_form phone desktop)
            foreach(_preset standard temperatures showcase)
                add_test(NAME CliLayoutPresetTest.${_form}_${_preset}
                    COMMAND havne-cli layout check
                            ${CMAKE_SOURCE_DIR}/app/layouts/${_form}/${_preset}.json
                            --form ${_form})
                set_tests_properties(CliLayoutPresetTest.${_form}_${_preset} PROPERTIES
                    TIMEOUT 60
                    PASS_REGULAR_EXPRESSION
                        "\"ok\":true.*\"dropped\":.\"pages\":0,\"cells\":0..*\"needs_files\":..,")
            endforeach()
        endforeach()
        # The app's read before any rule: a file of exactly the bound, its BOM
        # counted as the app counts it, reads; one byte more is refused, and
        # so is a file in Latin-1
        set(_cli_layout_files ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/layouts)
        app_cli_golden(LayoutCheckReadsAFileOfTheBoundWithABom layout-check-at-the-bound.jsonl
            -- $<TARGET_FILE:havne-cli> layout check
               ${_cli_layout_files}/bom-at-the-bound.json)
        # The deepest nesting the app reads, on havne-cli's own main thread
        app_cli_golden(LayoutCheckReadsTheDeepestNesting layout-check-nesting.jsonl
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layouts}/nesting-1024.json)
        app_cli_golden(LayoutCheckCountsABomInTheBound layout-check-over.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> layout check
               ${_cli_layout_files}/bom-over-the-bound.json)
        app_cli_golden(LayoutCheckRefusesAFileThatIsntUtf8 layout-check-latin1.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> layout check ${_cli_layout_files}/latin1.json)
        app_cli_golden(SignalsCheckReadsAFile signals-check.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/extras.json)
        app_cli_golden(SignalsCheckSaysEachGauge signals-check-gauges.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/gauges.json)
        app_cli_golden(StreamListsEachGauge stream-gauges.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max&from=100&to=102"
               --signals ${_cli_signals}/gauges.json --values --count 1)
        app_cli_golden(SignalsCheckRefusesAFile signals-refused.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/refused.json)
        app_cli_golden(SignalsCheckNamesAMissingFile signals-missing.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/none.json)
        # A file standing in for obd2's MAP: the base counts what is left of
        # obd2's, and Boost reads the file's def; one standing a text in for
        # baro is refused, naming Boost
        app_cli_golden(SignalsCheckCountsWhatIsLeftOfTheBase signals-check-replaces.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/replaces.json)
        app_cli_golden(SignalsCheckNamesTheComputedSignalAReplacementFeeds
            signals-replaces-refused.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/replaces-text.json)
        # An expression against a loaded file (ADR-0062): what it reads
        # and its value from the values given; where and why one is
        # refused, as a new signal and in a signal's place; why there is
        # no value; the format a call needs in a format 1 file; the names
        # an expression can read; a plugin variant's own signals; a file
        # that isn't there
        app_cli_golden(SignalsExprReadsAnExpressionAndWorksItOut signals-expr.jsonl
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/chain.json
               "margin / 2 + oil" --value coolant=95 --value oil=110@1)
        app_cli_golden(SignalsExprSaysWhereAnExpressionIsRefused
            signals-expr-refused.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/chain.json
               "max(coolant, oli)")
        app_cli_golden(SignalsExprRefusesAnEditThatReadsItself
            signals-expr-circle.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/chain.json
               --key hottest "margin + 1")
        app_cli_golden(SignalsExprSaysWhyThereIsNoValue signals-expr-stale.jsonl
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/chain.json
               hot --value coolant=120@3.5 --value oil=90)
        app_cli_golden(SignalsExprNamesTheFormatACallNeeds signals-expr-format.jsonl
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/extras.json
               "max(obd2.rpm, road_speed) / 1000"
               --value obd2.pid_0c=2500 --value road_speed=40)
        app_cli_golden(SignalsExprListsTheNamesAnExpressionReads signals-expr-inputs.jsonl
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/chain.json)
        app_cli_golden(SignalsExprReadsAPluginsVariant signals-expr-plugin.jsonl
            -- $<TARGET_FILE:havne-cli> signals expr
               --plugin ${CMAKE_SOURCE_DIR}/plugins/example-sim --variant my2019
               "clutch_slip / obd2.rpm" --value clutch_slip=30 --value obd2.rpm=1500)
        app_cli_golden(SignalsExprNamesAMissingFile signals-expr-missing.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> signals expr --signals ${_cli_signals}/none.json
               "a + b")
        # A condition file through the loader (ADR-0063): what it holds;
        # how its references meet obd2's set, the mock's and a signal
        # file's (a level in another unit, a text signal, an enumeration
        # above a level, a signal of another file and a condition
        # switched off among them); one the loader refuses, one that
        # isn't there, a profile nothing registered and a signal file
        # refused, each in words and never the path
        set(_cli_conditions ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/conditions)
        app_cli_golden(ConditionsCheckReadsAFile conditions-check.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/example.json)
        app_cli_golden(ConditionsCheckResolvesAgainstObd2 conditions-check-obd2.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/example.json
               --profile obd2)
        app_cli_golden(ConditionsCheckResolvesAgainstTheMock conditions-check-mock.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check --profile mock
               ${_cli_conditions}/example.json)
        app_cli_golden(ConditionsCheckResolvesAgainstASignalFile
            conditions-check-signals.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/desk.json
               --signals ${_cli_signals}/extras.json)
        app_cli_golden(ConditionsCheckResolvesAgainstNoSignals conditions-check-raw.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/desk.json
               --profile raw)
        app_cli_golden(ConditionsCheckRefusesAFile conditions-refused.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/refused.json)
        app_cli_golden(ConditionsCheckNamesAMissingFile conditions-missing.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/none.json)
        app_cli_golden(ConditionsCheckNamesAnUnknownProfile
            conditions-unknown-profile.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/example.json
               --profile obd3)
        app_cli_golden(ConditionsCheckRefusesASignalFile conditions-refused-signals.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/example.json
               --signals ${_cli_signals}/refused.json)
        # A plugin variant's own signals: its only variant or the one
        # named, a key only the later variant has, a package that isn't
        # there
        set(_cli_conditions_plugin ${CMAKE_SOURCE_DIR}/plugins/example-sim)
        app_cli_golden(ConditionsCheckResolvesAgainstAPluginsVariant
            conditions-check-plugin.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/plugin.json
               --plugin ${_cli_conditions_plugin} --variant my2019)
        app_cli_golden(ConditionsCheckResolvesAgainstAnotherVariant
            conditions-check-plugin-base.jsonl
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/plugin.json
               --variant base --plugin ${_cli_conditions_plugin})
        app_cli_golden(ConditionsCheckNamesAMissingPackage
            conditions-missing-plugin.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> conditions check ${_cli_conditions}/plugin.json
               --plugin ${_cli_conditions}/none.json)
        # Conditions judged in a session (ADR-0063 step 3): the replay
        # fixture's engine start under obd2 (a level crossed, a hold ended
        # at its own time, an input read too seldom going stale and back,
        # a link term the session can't judge); the mock, where only the
        # reference it provides binds; keys bound through the session's
        # signal file; a file the loader refuses and one that isn't there,
        # each before the connect
        app_cli_golden(StreamJudgesConditionsOnAReplay stream-conditions.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?profile=obd2&speed=max"
               --conditions ${_cli_conditions}/replay.json --count 300)
        app_cli_golden(StreamJudgesConditionsOnTheMock stream-conditions-mock.jsonl
            --ignore t_us
            -- $<TARGET_FILE:havne-cli> stream mock://x
               --conditions ${_cli_conditions}/replay.json --count 3)
        app_cli_golden(StreamBindsConditionsThroughItsSignalFile
            stream-conditions-signals.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max"
               --signals ${_cli_signals}/extras.json
               --conditions ${_cli_conditions}/desk.json --count 1)
        app_cli_golden(StreamRefusesAConditionFile stream-conditions-refused.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> stream mock://x
               --conditions ${_cli_conditions}/refused.json)
        app_cli_golden(StreamNamesAMissingConditionFile stream-conditions-missing.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> stream mock://x
               --conditions ${_cli_conditions}/none.json)
        # A replay of a recording judged by its own copy of the set (ADR-0063
        # section 8): the events the recording's cond lines hold; another
        # file named takes its place
        app_cli_golden(ReplayJudgesItsRecordingsOwnConditions
            replay-own-conditions.jsonl
            -- $<TARGET_FILE:havne-cli> stream
               "replay://${_cli_pulls}/conditions?profile=obd2&speed=max")
        app_cli_golden(ReplayJudgesTheConditionsItIsGiven
            replay-named-conditions.jsonl
            -- $<TARGET_FILE:havne-cli> stream
               "replay://${_cli_pulls}/conditions?profile=obd2&speed=max"
               --conditions ${_cli_conditions}/replay.json --count 3)
        app_cli_golden(StreamDecodesThroughASignalFile stream-signals.jsonl
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max&from=100&to=102"
               --signals ${_cli_signals}/extras.json --values --count 40)
        app_cli_golden(StreamRefusesABadSignalFile stream-signals-refused.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> stream "${_cli_replay}?speed=max"
               --signals ${_cli_signals}/refused.json --values)
        # Format 2 (ADR-0062): a chain two computed signals deep read back,
        # a call refused in a format 1 file, and the example in examples/
        # read and run through a hand-written replay of MAP and baro
        # replies (a boost of 0, 50 and 100 kPa, a ratio of 1, 1.5 and 2)
        app_cli_golden(SignalsCheckReadsAChain signals-check-chain.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/chain.json)
        app_cli_golden(SignalsCheckRefusesACallInFormat1 signals-refused-format.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> signals check ${_cli_signals}/call-in-format-1.json)
        app_cli_golden(SignalsCheckReadsTheExample signals-check-example.jsonl
            -- $<TARGET_FILE:havne-cli> signals check ${CMAKE_SOURCE_DIR}/examples/boost-ratio.json)
        app_cli_golden(StreamComputesTheExamplesChain stream-example.jsonl
            -- $<TARGET_FILE:havne-cli> stream
               "replay://${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/boost-ratio?speed=max"
               --signals ${CMAKE_SOURCE_DIR}/examples/boost-ratio.json --values)
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
        app_cli_golden(PluginsCheckWarnsOfAFormat2FileOnAnOlderCore
            plugins-check-format2.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/format2)
        # A plugin's Dashboard pages (ADR-0052's appendix on pages): a page
        # line for each, through the layout check, and the requires_core
        # line its older requires.core earns
        app_cli_golden(PluginsCheckJudgesAPluginsPages plugins-check-pages.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/pages)
        # A key no variant has, a cell the layout check drops and a second
        # page of one name: each page fails, so the check does
        app_cli_golden(PluginsCheckFailsAPageThatWontRead plugins-check-pages-bad.jsonl
            --exit 1
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/pages-bad.json)
        # A format 2 plugin with the conversation tier (ADR-0066): what its
        # manifest lets its module cause to be sent, after the plugin line;
        # then the same plugin as a folder with its module a file, a
        # changed module, a put and a drop refused, and every bound at once
        app_cli_golden(PluginsCheckSaysWhatAConversationDeclares
            plugins-check-conversation.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_plugins}/conversation.json)
        add_test(NAME CliPluginConversationTest.AFolderReadsAsItsPackedForm
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cli_plugin_conversation.py
                    $<TARGET_FILE:havne-cli> ${_cli_plugins}/conversation.json)
        set_tests_properties(CliPluginConversationTest.AFolderReadsAsItsPackedForm
            PROPERTIES TIMEOUT 120)
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
                PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0)
            # python/examples/isotp_fuzz.py against havne-cli's sim on
            # vcan0 (ADR-0053's appendix "the examples as built"): outside
            # pytest, so the sanitizer tree runs it on its ASan havne-cli
            add_test(NAME ExampleTest.isotp_fuzz_cli
                COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/example_isotp_fuzz.py
                        $<TARGET_FILE:havne-cli>)
            set_tests_properties(ExampleTest.isotp_fuzz_cli
                PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0 LABELS "python")
        endif()
        # An OBDb signal set, synthetic (ADR-0051 section 7): imported for a
        # model year, its notes and the file written whole, beside nothing
        # else, against committed ones; an --out that can't be written
        # writes nothing and names no path; other writers' partials of the
        # same --out are left alone; a file that isn't a set refused
        set(_cli_obdb ${CMAKE_CURRENT_SOURCE_DIR}/cli_obdb_import.py)
        set(_obdb_set ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obdb/example-car.json)
        add_test(NAME CliObdbImportTest.ASetConvertsToTheCommittedFile
            COMMAND ${Python3_EXECUTABLE} ${_cli_obdb} convert $<TARGET_FILE:havne-cli>
                    ${_obdb_set} ${_cli_golden}/obdb-import.jsonl
                    ${CMAKE_CURRENT_SOURCE_DIR}/golden/obdb/example-car.json)
        add_test(NAME CliObdbImportTest.AnOutThatCantBeWrittenWritesNothing
            COMMAND ${Python3_EXECUTABLE} ${_cli_obdb} unwritable
                    $<TARGET_FILE:havne-cli> ${_obdb_set})
        add_test(NAME CliObdbImportTest.OtherWritersPartialsAreLeftAlone
            COMMAND ${Python3_EXECUTABLE} ${_cli_obdb} beside
                    $<TARGET_FILE:havne-cli> ${_obdb_set})
        set_tests_properties(CliObdbImportTest.ASetConvertsToTheCommittedFile
            CliObdbImportTest.AnOutThatCantBeWrittenWritesNothing
            CliObdbImportTest.OtherWritersPartialsAreLeftAlone PROPERTIES TIMEOUT 60)
        app_cli_golden(SignalsImportRefusesWhatIsntAnObdbSet obdb-not-a-set.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> signals import obdb ${_cli_signals}/extras.json
               --out never-written.json)
        # A packed plugin made and changed (ADR-0064 Q9): plugins wrap, put
        # and drop and signals import obdb --plugin, each command's lines
        # and package against committed ones; each refusal's words, with
        # nothing left; each variant's license in plugins check, an OBDb
        # variant's share-alike line among them; on Linux's vcan0 a
        # wrapped file decodes as the file alone (skipped without vcan0
        # unless ECUIO_VCAN_REQUIRED, and serial, as test_vcan is)
        set(_cli_make ${CMAKE_CURRENT_SOURCE_DIR}/cli_plugin_make.py)
        set(_cli_made ${CMAKE_CURRENT_SOURCE_DIR}/golden/plugins)
        add_test(NAME CliPluginMakeTest.EachCommandsLinesAndPackageAreCommitted
            COMMAND ${Python3_EXECUTABLE} ${_cli_make} golden $<TARGET_FILE:havne-cli>
                    ${_cli_signals} ${_obdb_set} ${CMAKE_CURRENT_SOURCE_DIR}/golden)
        add_test(NAME CliPluginMakeTest.ARefusalSaysWhyAndLeavesNothing
            COMMAND ${Python3_EXECUTABLE} ${_cli_make} refused $<TARGET_FILE:havne-cli>
                    ${_cli_signals})
        set_tests_properties(CliPluginMakeTest.EachCommandsLinesAndPackageAreCommitted
            CliPluginMakeTest.ARefusalSaysWhyAndLeavesNothing PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME CliPluginMakeTest.AWrappedFileDecodesAsTheFileOnVcan
                COMMAND ${Python3_EXECUTABLE} ${_cli_make} vcan $<TARGET_FILE:havne-cli>
                        ${_cli_signals})
            set_tests_properties(CliPluginMakeTest.AWrappedFileDecodesAsTheFileOnVcan
                PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RESOURCE_LOCK vcan0)
        endif()
        app_cli_golden(PluginsCheckNamesEachVariantsLicense plugins-check-licenses.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_made}/plugins-put-renamed.json)
        app_cli_golden(PluginsCheckSaysAnObdbVariantIsShareAlike plugins-check-obdb.jsonl
            -- $<TARGET_FILE:havne-cli> plugins check ${_cli_made}/obdb-import-plugin.json)
        # A column map with a typo refused in core's words, naming the key
        # and never a path, before any folder is made
        set(_cli_csv ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import/csv)
        app_cli_golden(ImportCsvRefusesAMapsUnknownKey import-csv-unknown-key.jsonl
            --exit 3
            -- $<TARGET_FILE:havne-cli> import csv ${_cli_csv}/logger-a/log.csv
               never-made --map ${_cli_csv}/unknown-key.json)
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
        # A vehicle's baseline (ADR-0049's appendix of 2026-11-06): two key
        # ons at 0 rpm and their starts 7 s apart (start-early's unit
        # "RPM", start-late's "rpm"), lined up on their engine starts, and
        # a pull whose engine runs from its first sample, found nowhere
        app_cli_golden(ViewAlignsOnTheEngineStarts view-engine-start.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/start-early
               ${_cli_pulls}/start-late ${_cli_pulls}/pull-early
               --align-on engine-start --from -6 --to 4 --columns 10
               --at 0.5 --signal RPM)
        # A span's statistics (the chart's ADR, section 5): two pulls
        # aligned on their rise, two spans, a text signal's samples alone
        app_cli_golden(ViewSpanGivesEachSignalsStatistics view-span.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               ${_cli_pulls}/pull-late --align-on rise:RPM:4000 --hysteresis 500
               --from -2 --to 8 --columns 2 --signal RPM --signal VIN
               --span -1:1 --span 0:5)
        # The navigation aids (ADR-0056's appendix): the two pulls aligned on
        # their rise, found by throttle with engine speed's rise (the
        # fixtures have no speed); a hand-written cruise, a 3 km/h step
        # between two; and its floor in mph, which only the second clears
        app_cli_golden(ViewStretchesFindsThePulls view-stretches-pulls.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-early
               ${_cli_pulls}/pull-late --align-on rise:RPM:4000 --hysteresis 500
               --columns 2 --signal RPM --stretches)
        # A road log's two pulls (pull-twice: a stab at the throttle and a
        # part-throttle rise that are none), its second laid on
        # pull-early's first: the pulls' starts at 0, each column's mean,
        # and the second recording's delta from the first
        app_cli_golden(ViewAlignsOnEachRecordingsChosenPull view-align-pull.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/pull-twice
               ${_cli_pulls}/pull-early --align-on pull:2,1 --from -1 --to 3
               --columns 8 --signal RPM --stretches pulls)
        app_cli_golden(ViewStretchesFindsTwoCruises view-stretches-cruise.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/cruise --columns 1
               --stretches cruises)
        app_cli_golden(ViewStretchesTakeTheirLevelsInImperial
            view-stretches-imperial.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/cruise --columns 1
               --stretches --units imperial --stretch-level cruise-floor=51
               --role "speed=Vehicle Speed")
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
        # A folder that isn't there: the reader's refusal (no frames.csv),
        # not a profile to name, as the app's viewer says
        app_cli_golden(ViewFramesRefusesAMissingFolder
            view-frames-missing-folder.jsonl
            --exit 3 -- $<TARGET_FILE:havne-cli> view
               ${CMAKE_CURRENT_BINARY_DIR}/never-there --frames)
        # A signal named by its key or by a name one signal has (ADR-0065
        # section 9): view and export on a recording with two signals of
        # one name, log on the mock
        add_test(NAME CliKeysRunTest.ViewAndExportTakeAKeyOrANameOneSignalHas
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_keys.py recording
                    $<TARGET_FILE:havne-cli>)
        add_test(NAME CliKeysRunTest.LogTakesAKeyOrANameOneSignalHas
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_keys.py log
                    $<TARGET_FILE:havne-cli>)
        add_test(NAME CliKeysRunTest.AComputedSignalGoesByItsAuthorsKey
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_keys.py file
                    $<TARGET_FILE:havne-cli>)
        add_test(NAME CliKeysRunTest.ADeltaPairsByKeyElseByANameEachHasOnce
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_keys.py delta
                    $<TARGET_FILE:havne-cli>)
        add_test(NAME CliViewRunTest.ColumnsCountEverySampleOnce
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py once
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-early
                    ${_cli_pulls}/pull-late)
        add_test(NAME CliViewRunTest.AlignedPullsAgree
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py agree
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-early
                    ${_cli_pulls}/pull-late)
        add_test(NAME CliViewRunTest.SpanStatisticsAreTheSamples
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py stats
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-early
                    ${_cli_pulls}/pull-late)
        add_test(NAME CliViewRunTest.ColumnMeansAndDeltasAreTheSamples
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py means
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.ADeltaIsBetweenOneNameInOneUnit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py deltas
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.ARecordingShiftedHalfASecondDiffersInNoColumn
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py shifted
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.ASpanEndingAtZeroHoldsTheSampleThere
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py instant
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.APullPastARecordingsPullsLeavesItOffTheAxis
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py past
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.StretchesTakesItsWordBeforeAFolderOfThatName
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py named
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.ASampleAtTheAxisEndIsInTheLastColumn
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py end
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        add_test(NAME CliViewRunTest.AnInterruptEndsEachSearch
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_view.py interrupt
                    $<TARGET_FILE:havne-cli> ${_cli_pulls}/pull-twice
                    ${_cli_pulls}/pull-early)
        set_tests_properties(CliViewRunTest.AnInterruptEndsEachSearch
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        set_tests_properties(CliViewRunTest.ColumnsCountEverySampleOnce
            CliViewRunTest.ASampleAtTheAxisEndIsInTheLastColumn
            CliViewRunTest.AlignedPullsAgree
            CliViewRunTest.SpanStatisticsAreTheSamples
            CliViewRunTest.ColumnMeansAndDeltasAreTheSamples
            CliViewRunTest.ADeltaIsBetweenOneNameInOneUnit
            CliViewRunTest.ARecordingShiftedHalfASecondDiffersInNoColumn
            CliViewRunTest.ASpanEndingAtZeroHoldsTheSampleThere
            CliViewRunTest.APullPastARecordingsPullsLeavesItOffTheAxis
            CliViewRunTest.StretchesTakesItsWordBeforeAFolderOfThatName
            PROPERTIES TIMEOUT 60)
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
        # A recording with a set of conditions (ADR-0063 section 8;
        # tests/fixtures/recording/conditions, its cond lines a replay's of
        # its own frames): the opened line names the set, and each cond
        # line is a condition line, in view and in info
        app_cli_golden(InfoListsARecordingsConditionEvents info-conditions.jsonl
            -- $<TARGET_FILE:havne-cli> info ${_cli_pulls}/conditions)
        app_cli_golden(ViewListsARecordingsConditionEvents view-conditions.jsonl
            -- $<TARGET_FILE:havne-cli> view ${_cli_pulls}/conditions
               --columns 3)
        # That recording as a recorder that lost three events wrote it
        # (conditions-lost: their cond lines and the firing's marker gone,
        # a drop line counting them): the opened line's events_lost
        app_cli_golden(InfoCountsTheConditionEventsARecordingLost
            info-conditions-lost.jsonl
            -- $<TARGET_FILE:havne-cli> info ${_cli_pulls}/conditions-lost)
        # A stretch of the replay fixture's frames as recorded (ADR-0049's
        # appendix "frames read back"), and a folder that isn't there
        app_cli_golden(FramesReadsAStretchAsRecorded frames-stretch.jsonl
            -- $<TARGET_FILE:havne-cli> frames
               ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/2026-10-25-engine-start
               --from 100 --to 101)
        # The wide CSV's size per row interval, no file written (ADR-0056's
        # appendix "the viewer's Export as built")
        app_cli_golden(ExportEstimateSizesTheWideCsv export-estimate.jsonl
            -- $<TARGET_FILE:havne-cli> export
               ${CMAKE_CURRENT_SOURCE_DIR}/golden/recording/replay-obd2
               --format csv --estimate)
        app_cli_golden(FramesRefusesAMissingFolder frames-missing-folder.jsonl
            --exit 3 -- $<TARGET_FILE:havne-cli> frames
               ${CMAKE_CURRENT_BINARY_DIR}/never-there)
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
    # The same, with the refusal's words on standard error: for a case
    # another usage error could pass
    function(app_cli_usage_says name args words)
        add_test(NAME CliUsageTest.${name}
            COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:havne-cli> -DEXPECT=2
                    "-DARGS=${args}" "-DEXPECT_ERR=${words}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_exit.cmake)
    endfunction()
    app_cli_usage(NoArgumentsIsUsage "")
    app_cli_usage(NoUriIsUsage "stream")
    app_cli_usage(AnUnknownCommandIsUsage "listen")
    app_cli_usage(AnUnknownOptionIsUsage "stream|mock://x|--fast")
    app_cli_usage(ACountOfZeroIsUsage "stream|mock://x|--count|0")
    app_cli_usage(TwoUrisIsUsage "stream|mock://a|mock://b")
    app_cli_usage(UntilScannedWithoutAUriIsUsage "stream|--until-scanned")
    app_cli_usage(ALabelWithoutRecordIsUsage "stream|mock://x|--label|lap")
    app_cli_usage(LogWithoutAUriIsUsage "log")
    app_cli_usage(LogMinSweepWithoutASignalIsUsage "log|mock://x|out|--min-sweep|250")
    app_cli_usage(LogMinSweepPastItsMostIsUsage "log|mock://x|out|--signal|RPM|--min-sweep|60001")
    app_cli_usage(LogMinSweepOfZeroIsUsage "log|mock://x|out|--signal|RPM|--min-sweep|0")
    app_cli_usage_says(LogPickOtherThanRankedIsUsage "log|mock://x|out|--pick|all"
        "--pick takes 'ranked'")
    app_cli_usage_says(StreamPickOtherThanRankedIsUsage "stream|mock://x|--pick|all"
        "--pick takes 'ranked'")
    app_cli_usage_says(StreamPickWithoutAWordIsUsage "stream|mock://x|--pick"
        "--pick takes 'ranked'")
    app_cli_usage(LogWithoutAFolderIsUsage "log|mock://x")
    app_cli_usage(LogAThirdWordIsUsage "log|mock://x|a|b")
    app_cli_usage(LogSignalWithoutANameIsUsage "log|mock://x|f|--signal")
    app_cli_usage_says(LogAnEmptySignalIsUsage "log|mock://x|f|--signal|"
        "--signal takes a signal's name")
    # The names are checked against the session's set after the connect,
    # before any line: a typo and a repeat are usage with nothing written
    app_cli_usage(LogAnUnknownSignalIsUsage "log|mock://x|f|--signal|Nothing")
    app_cli_usage(LogASignalTwiceIsUsage "log|mock://x|f|--signal|RPM|--signal|RPM")
    app_cli_usage(LogAnExportOptionIsUsage "log|mock://x|f|--layout|wide")
    app_cli_usage(LogSecondsOfZeroIsUsage "log|mock://x|f|--seconds|0")
    # The cap is 366 days of seconds: one more is usage, for every command
    # that counts --seconds out
    app_cli_usage(LogSecondsPastTheCapIsUsage "log|mock://x|f|--seconds|31622401")
    app_cli_usage(StreamSecondsPastTheCapIsUsage "stream|mock://x|--seconds|31622401")
    app_cli_usage(StreamSecondsThatWrapAreUsage "stream|mock://x|--seconds|18446744073710")
    app_cli_usage(SimSecondsPastTheCapIsUsage "sim|mock://x|--elm327|tcp:0|--seconds|31622401")
    # play exists only on Linux (cli/common.h's HAVNE_CLI_PLAY); elsewhere
    # its every form is the one refusal
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        app_cli_usage_says(PlaySecondsPastTheCapIsUsage
            "play|mock://x|--to|vcan0|--seconds|31622401"
            "--seconds takes a whole number from 1 to 31622400")
        app_cli_usage_says(PlayWithoutAnInterfaceIsUsage "play|mock://x"
            "play needs a URI and --to <interface>")
        app_cli_usage_says(PlayACountOfZeroIsUsage "play|mock://x|--to|vcan0|--count|0"
            "--count takes a whole number above 0")
    else()
        app_cli_usage_says(PlayNeedsLinux
            "play|mock://x|--to|vcan0" "play needs Linux")
    endif()
    app_cli_usage(LogSignalsAndAPluginIsUsage "log|mock://x|f|--signals|a.json|--plugin|p")
    app_cli_usage(SignalsWithoutAFileIsUsage "stream|mock://x|--signals")
    app_cli_usage(SignalsForAnotherProfileIsUsage
        "stream|mock://x?profile=obd2|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json")
    app_cli_usage(ConditionsWithoutCheckIsUsage "conditions")
    app_cli_usage(ConditionsAnUnknownWordIsUsage "conditions|lint|c.json")
    app_cli_usage_says(ConditionsCheckWithoutAFileIsUsage "conditions|check"
        "conditions check takes a condition file")
    app_cli_usage_says(ConditionsCheckTwoFilesIsUsage "conditions|check|a.json|b.json"
        "conditions check takes one file")
    app_cli_usage_says(ConditionsCheckSignalsWithoutAFileIsUsage
        "conditions|check|a.json|--signals" "--signals takes one signal file")
    app_cli_usage_says(ConditionsCheckProfileWithoutANameIsUsage
        "conditions|check|a.json|--profile" "--profile takes one profile name")
    app_cli_usage_says(ConditionsCheckSignalsAndAProfileIsUsage
        "conditions|check|a.json|--signals|s.json|--profile|obd2"
        "one of --signals, --profile and --plugin")
    app_cli_usage_says(ConditionsCheckAnUnknownOptionIsUsage
        "conditions|check|a.json|--package|p" "unexpected '--package'")
    app_cli_usage_says(ConditionsCheckAVariantWithoutAPluginIsUsage
        "conditions|check|a.json|--variant|base" "--variant names a --plugin's variant")
    app_cli_usage_says(ConditionsCheckAPluginOfSeveralVariantsNeedsOne
        "conditions|check|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/conditions/example.json|--plugin|${CMAKE_SOURCE_DIR}/plugins/example-sim"
        "--plugin needs --variant to name one of its variants: base, my2019")
    app_cli_usage_says(ConditionsCheckAVariantThePluginLacksIsUsage
        "conditions|check|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/conditions/example.json|--plugin|${CMAKE_SOURCE_DIR}/plugins/example-sim|--variant|my2020"
        "--plugin needs --variant to name one of its variants: base, my2019")
    app_cli_usage_says(StreamConditionsWithoutAFileIsUsage
        "stream|mock://x|--conditions" "--conditions takes one condition file")
    app_cli_usage_says(StreamTwoConditionFilesIsUsage
        "stream|mock://x|--conditions|a.json|--conditions|b.json"
        "--conditions takes one condition file")
    app_cli_usage_says(StreamAnEmptyConditionFileIsUsage
        "stream|mock://x|--conditions|" "--conditions takes one condition file")
    app_cli_usage_says(LogConditionsWithoutAFileIsUsage
        "log|mock://x|f|--conditions" "--conditions takes one condition file")
    app_cli_usage_says(LogTwoConditionFilesIsUsage
        "log|mock://x|f|--conditions|a.json|--conditions|b.json"
        "--conditions takes one condition file")
    app_cli_usage_says(LogAnEmptyConditionFileIsUsage
        "log|mock://x|f|--conditions|" "--conditions takes one condition file")
    app_cli_usage_says(LogArmedWithoutConditionsIsUsage
        "log|mock://x|f|--armed" "--armed goes with --conditions")
    app_cli_usage_says(SignalsCheckWithoutAFileIsUsage "signals|check"
        "signals takes check <file>")
    app_cli_usage_says(SignalsWithoutCheckIsUsage "signals"
        "signals takes check <file>")
    app_cli_usage_says(SignalsAnUnknownWordIsUsage "signals|lint|x.json"
        "signals takes check <file>")
    app_cli_usage_says(SignalsCheckTwoFilesIsUsage "signals|check|a.json|b.json"
        "signals takes check <file>")
    app_cli_usage_says(SignalsExprWithoutAFileIsUsage "signals|expr|a + b"
        "signals expr takes --signals")
    app_cli_usage_says(SignalsExprAFileAndAPluginIsUsage
        "signals|expr|--signals|a.json|--plugin|p" "signals expr takes --signals")
    app_cli_usage_says(SignalsExprTwoExpressionsIsUsage
        "signals|expr|--signals|a.json|a|b" "signals expr takes one expression")
    app_cli_usage_says(SignalsExprAKeyWithoutAnExpressionIsUsage
        "signals|expr|--signals|a.json|--key|boost" "--key goes with an expression")
    app_cli_usage_says(SignalsExprAKeyThatIsNoKeyIsUsage
        "signals|expr|--signals|a.json|a|--key|Boost" "--key takes a signal's key")
    app_cli_usage_says(SignalsExprAValueWithoutAnExpressionIsUsage
        "signals|expr|--signals|a.json|--value|a=1" "--value goes with an expression")
    app_cli_usage_says(SignalsExprAValueWithoutANumberIsUsage
        "signals|expr|--signals|a.json|a|--value|a" "--value takes")
    app_cli_usage_says(SignalsExprANameGivenTwoValuesIsUsage
        "signals|expr|--signals|a.json|a|--value|a=1|--value|a=2"
        "--value gives 'a' two values")
    app_cli_usage_says(SignalsExprAVariantWithoutAPluginIsUsage
        "signals|expr|--signals|a.json|--variant|base"
        "--variant names a --plugin's variant")
    app_cli_usage_says(SignalsExprAnUnknownOptionIsUsage
        "signals|expr|--signals|a.json|--fast"
        "unexpected '--fast': an expression that starts with -- goes in parentheses")
    # A value's name is read in the file, so these load one
    set(_cli_expr_chain "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/chain.json")
    app_cli_usage_says(SignalsExprAValueForNoSignalIsUsage
        "signals|expr|--signals|${_cli_expr_chain}|oil|--value|oli=1"
        "--value oli: the expression reads 'oli', which no signal is")
    app_cli_usage_says(SignalsExprAValueForAComputedSignalIsUsage
        "signals|expr|--signals|${_cli_expr_chain}|hot|--value|margin=1"
        "--value margin: the signal is computed")
    app_cli_usage_says(SignalsExprOneSignalGivenTwoValuesIsUsage
        "signals|expr|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json|obd2.rpm|--value|obd2.rpm=1|--value|obd2.pid_0c=2"
        "--value gives one signal two values, as 'obd2.rpm' and 'obd2.pid_0c'")
    app_cli_usage_says(SignalsExprAPluginOfSeveralVariantsNeedsOne
        "signals|expr|--plugin|${CMAKE_SOURCE_DIR}/plugins/example-sim|oil_temp"
        "--plugin needs --variant to name one of its variants: base, my2019")
    app_cli_usage_says(SignalsExprAVariantThePluginLacksIsUsage
        "signals|expr|--plugin|${CMAKE_SOURCE_DIR}/plugins/example-sim|--variant|my2020|oil_temp"
        "--plugin needs --variant to name one of its variants: base, my2019")
    app_cli_usage_says(SignalsImportWithoutOutIsUsage "signals|import|obdb|a.json"
        "signals import obdb needs --out <file>")
    app_cli_usage_says(SignalsImportFromAnotherFormatIsUsage
        "signals|import|dbc|a.json|--out|b.json"
        "signals takes check <file>")
    app_cli_usage_says(SignalsImportAYearBeforeOBDIIIsUsage
        "signals|import|obdb|a.json|--out|b.json|--year|1979"
        "--year takes a model year, 1980 to 2100")
    app_cli_usage_says(SignalsImportAnUnknownOptionIsUsage
        "signals|import|obdb|a.json|--out|b.json|--fast"
        "unexpected '--fast'")
    app_cli_usage_says(SignalsImportAWmiWithoutAPluginIsUsage
        "signals|import|obdb|a.json|--out|b.json|--wmi|ZZZ"
        "--wmi and --years go with --plugin")
    app_cli_usage_says(SignalsImportYearsWithoutAPluginIsUsage
        "signals|import|obdb|a.json|--out|b.json|--wmi|ZZZ|--years|2015-2020"
        "--wmi and --years go with --plugin")
    app_cli_usage_says(SignalsImportYearsWithoutAWmiIsUsage
        "signals|import|obdb|a.json|--out|b.json|--plugin|--years|2015-2020"
        "--years goes with --wmi")
    app_cli_usage_says(SignalsImportAWmiThatIsntOneIsUsage
        "signals|import|obdb|a.json|--out|b.json|--plugin|--wmi|ZZI"
        "--wmi takes manufacturer codes")
    app_cli_usage_says(SignalsImportYearsBackwardsIsUsage
        "signals|import|obdb|a.json|--out|b.json|--plugin|--wmi|ZZZ|--years|2020-2015"
        "--years takes <from>-<to>")
    app_cli_usage_says(SignalsImportAWmiWithoutCodesIsUsage
        "signals|import|obdb|a.json|--out|b.json|--plugin|--wmi"
        "unexpected '--wmi'")
    app_cli_usage(SimSignalsWithoutAFileIsUsage "sim|mock://sim|--on|vcan0|--signals")
    set(_usage_example ${CMAKE_SOURCE_DIR}/plugins/example-sim)
    app_cli_usage(PluginsWithoutAWordIsUsage "plugins")
    app_cli_usage_says(PluginsAnUnknownWordIsUsage "plugins|lint|x"
        "plugins takes list <pkg>")
    app_cli_usage(PluginsListWithoutAPackageIsUsage "plugins|list")
    app_cli_usage_says(PluginsListAnOptionIsUsage "plugins|list|x|--fast"
        "unexpected '--fast'")
    app_cli_usage(PluginsCheckWithoutAPackageIsUsage "plugins|check")
    app_cli_usage_says(PluginsCheckAnUnknownOptionIsUsage
        "plugins|check|x|--of|vcan0" "plugins check takes <pkg>")
    app_cli_usage_says(PluginsCheckAnEmptyInterfaceIsUsage "plugins|check|x|--on|"
        "plugins check takes <pkg>")
    app_cli_usage(PluginsCheckOnWithoutAnInterfaceIsUsage "plugins|check|x|--on")
    app_cli_usage(PluginsCheckAFlagForThePackageIsUsage "plugins|check|--on")
    app_cli_usage(PluginsCheckHelpForThePackageIsUsage "plugins|check|--help")
    app_cli_usage(PluginsMatchAFlagForThePackageIsUsage
        "plugins|match|--x|--vin|ZZZFAKE11K0000001")
    app_cli_usage(PluginsMatchWithoutAVinIsUsage "plugins|match|x")
    app_cli_usage(PluginsMatchNotAVinIsUsage "plugins|match|x|--vin|FAKEJ2534VIN00001")
    app_cli_usage_says(PluginsWrapWithoutAnOutIsUsage "plugins|wrap|a.json"
        "plugins wrap needs --out <pkg>")
    app_cli_usage(PluginsWrapAnOutWithoutAFileIsUsage "plugins|wrap|a.json|--out")
    app_cli_usage(PluginsWrapAFlagForTheFileIsUsage "plugins|wrap|--out|b.json")
    app_cli_usage(PluginsWrapAnUnknownOptionIsUsage
        "plugins|wrap|a.json|--out|b.json|--fast")
    app_cli_usage(PluginsWrapTwoOutsIsUsage
        "plugins|wrap|a.json|--out|b.json|--out|c.json")
    app_cli_usage(PluginsPutWithoutItsFileIsUsage "plugins|put|a.json|--out|b.json")
    app_cli_usage(PluginsPutWithoutAnOutIsUsage
        "plugins|put|a.json|f.json|--variant|v")
    app_cli_usage(PluginsPutAnIdIsUsage
        "plugins|put|a.json|f.json|--id|x|--out|b.json")
    app_cli_usage(PluginsPutTwoVariantsIsUsage
        "plugins|put|a.json|f.json|--variant|v|--variant|w|--out|b.json")
    app_cli_usage(PluginsDropWithoutAVariantIsUsage "plugins|drop|a.json|--out|b.json")
    app_cli_usage(PluginsDropAFileIsUsage
        "plugins|drop|a.json|f.json|--variant|v|--out|b.json")
    app_cli_usage(PluginWithoutAPackageIsUsage "stream|mock://x|--plugin")
    app_cli_usage(PluginWithoutItsVariantIsUsage
        "stream|mock://x|--plugin|${_usage_example}")
    app_cli_usage(PluginForAnotherProfileIsUsage
        "stream|mock://x?profile=obd2|--plugin|${_usage_example}")
    app_cli_usage(PluginAndSignalsIsUsage
        "stream|mock://x?profile=plugin.example-sim.base|--plugin|${_usage_example}|--signals|${CMAKE_CURRENT_SOURCE_DIR}/fixtures/signals/extras.json")
    app_cli_usage(SimPluginWithoutItsVariantIsUsage
        "sim|mock://sim|--on|vcan0|--plugin|${_usage_example}")
    app_cli_usage_says(SimSignalsAndAPluginIsUsage
        "sim|mock://sim|--on|vcan0|--signals|a.json|--plugin|p"
        "--signals and --plugin each give the profile")
    app_cli_usage(RecordWithoutAFolderIsUsage "stream|mock://x|--record")
    app_cli_usage(VinWithoutOneIsUsage "vin")
    app_cli_usage(NotAVinIsUsage "vin|FAKEJ2534VIN00001")
    app_cli_usage(LayoutWithoutACheckIsUsage "layout")
    app_cli_usage(LayoutCheckWithoutAFileIsUsage "layout|check")
    app_cli_usage(LayoutCheckAFormThatIsntOneIsUsage "layout|check|x.json|--form|tablet")
    app_cli_usage(LayoutCheckAFormWithoutItsWordIsUsage "layout|check|x.json|--form")
    app_cli_usage(LayoutCheckAnUnknownOptionIsUsage "layout|check|x.json|--fast")
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
    app_cli_usage_says(ViewWithoutARecordingIsUsage "view"
        "view needs a recording's folder")
    app_cli_usage_says(ViewColumnsOfZeroIsUsage "view|x|--columns|0"
        "--columns takes 1 to 10000")
    app_cli_usage_says(ViewTooManyColumnsIsUsage "view|x|--columns|10001"
        "--columns takes 1 to 10000")
    app_cli_usage_says(ViewAnUnknownAlignmentIsUsage "view|x|--align-on|sideways"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewARiseWithoutALevelIsUsage "view|x|--align-on|rise:RPM"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewHysteresisWithoutARiseIsUsage
        "view|x|--align-on|marker:pull|--hysteresis|5"
        "--hysteresis goes with --align-on rise: or fall:")
    app_cli_usage_says(ViewANegativeHysteresisIsUsage
        "view|x|--align-on|rise:RPM:1|--hysteresis|-1"
        "--hysteresis takes 0 or more")
    app_cli_usage_says(ViewHysteresisWithAnEngineStartIsUsage
        "view|x|--align-on|engine-start|--hysteresis|400"
        "--hysteresis goes with --align-on rise: or fall:")
    app_cli_usage_says(ViewAnEngineStartOnASignalIsUsage "view|x|--align-on|engine-start:RPM"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewAPullListThatIsEmptyIsUsage "view|x|--align-on|pull:"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewPullZeroIsUsage "view|x|--align-on|pull:0"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewAPullThatIsntANumberIsUsage "view|x|--align-on|pull:first"
        "--align-on takes rise:<signal>:<level>")
    app_cli_usage_says(ViewAPullForEachOfAnotherCountIsUsage
        "view|x|y|--align-on|pull:1,2,3" "one for each of the 2")
    app_cli_usage_says(ViewHysteresisWithAPullIsUsage
        "view|x|--align-on|pull:1|--hysteresis|5"
        "--hysteresis goes with --align-on rise: or fall:")
    app_cli_usage_says(ViewFromNotBeforeToIsUsage "view|x|--from|5|--to|5"
        "--from must come before --to")
    app_cli_usage_says(ViewSecondsThatArentAreUsage "view|x|--at|soon"
        "--at takes seconds")
    app_cli_usage_says(ViewASpanWithoutItsEndIsUsage "view|x|--span|5"
        "--span takes A:B")
    app_cli_usage_says(ViewASpanBackwardsIsUsage "view|x|--span|5:3"
        "--span takes A:B")
    app_cli_usage_says(ViewASpanOfNoLengthIsUsage "view|x|--span|5:5"
        "--span takes A:B")
    app_cli_usage_says(ViewASpanOfWordsIsUsage "view|x|--span|soon:later"
        "--span takes A:B")
    app_cli_usage_says(ViewASpanWithThreePartsIsUsage "view|x|--span|1:2:3"
        "--span takes A:B")
    app_cli_usage_says(ViewAStretchLevelWithoutStretchesIsUsage
        "view|x|--stretch-level|pull-gap=1"
        "--stretch-level, --role, --pull-end and --units go with")
    app_cli_usage_says(ViewARoleWithoutStretchesIsUsage "view|x|--role|rpm=RPM"
        "--stretch-level, --role, --pull-end and --units go with")
    app_cli_usage_says(ViewUnitsWithoutStretchesIsUsage "view|x|--units|imperial"
        "--stretch-level, --role, --pull-end and --units go with")
    app_cli_usage_says(ViewAnUnknownStretchLevelIsUsage
        "view|x|--stretches|--stretch-level|pull-speed=1"
        "--stretch-level takes <key>=<value>")
    app_cli_usage_says(ViewANegativeStretchLevelIsUsage
        "view|x|--stretches|--stretch-level|pull-gap=-1"
        "--stretch-level takes <key>=<value>")
    app_cli_usage_says(ViewANegativePullRiseIsUsage
        "view|x|--stretches|--stretch-level|pull-rise=-1"
        "--stretch-level takes <key>=<value>")
    app_cli_usage_says(ViewAPullGainOfWordsIsUsage
        "view|x|--align-on|pull:1|--stretch-level|pull-gain=some"
        "--stretch-level takes <key>=<value>")
    app_cli_usage_says(ViewAnUnknownRoleIsUsage "view|x|--stretches|--role|brake=Pedal"
        "--role takes throttle=, load=, speed= or rpm=")
    app_cli_usage_says(ViewAPullEndThatIsntOneIsUsage
        "view|x|--stretches|--pull-end|top" "--pull-end takes lift or top-speed")
    app_cli_usage_says(ViewAPullEndWithoutStretchesIsUsage
        "view|x|--pull-end|lift" "--pull-end and --units go with")
    app_cli_usage_says(ViewAPullEndWithoutAWordIsUsage "view|x|--stretches|--pull-end"
        "unexpected '--pull-end'")
    # pulls, cruises or all after --stretches is its word, never a folder
    app_cli_usage_says(ViewStretchesAndItsWordNameNoFolder "view|--stretches|all"
        "view needs a recording's folder")
    app_cli_usage_says(ViewStretchesPullsNamesNoFolder "view|--stretches|pulls"
        "view needs a recording's folder")
    app_cli_usage_says(ViewUnitsThatArentAreUsage "view|x|--stretches|--units|furlongs"
        "--units takes metric or imperial")
    app_cli_usage_says(ViewAnUnknownOptionIsUsage "view|x|--fast"
        "unexpected '--fast'")
    set(_cli_pull_early ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording/pull-early)
    app_cli_usage(ViewASignalNoRecordingHasIsUsage
        "view|${_cli_pull_early}|--signal|Boost")
    app_cli_usage(ViewAlignedOnASignalNoRecordingHasIsUsage
        "view|${_cli_pull_early}|--align-on|rise:Boost:1")
    app_cli_usage(ViewARiseOnTextIsUsage "view|${_cli_pull_early}|--align-on|rise:VIN:1")
    app_cli_usage(ViewFromAfterTheDefaultEndIsUsage
        "view|${_cli_pull_early}|--from|100")
    app_cli_usage(ViewASpanOutsideTheAxisIsUsage
        "view|${_cli_pull_early}|--span|1:1000")
    app_cli_usage(ViewARoleNoRecordingHasIsUsage
        "view|${_cli_pull_early}|--stretches|--role|speed=Speed")
    app_cli_usage(ViewARoleOnTextIsUsage
        "view|${_cli_pull_early}|--stretches|--role|throttle=VIN")
    app_cli_usage_says(ViewAProfileWithoutFramesIsUsage "view|x|--profile|obd2"
        "--profile, --signals and --plugin go with --frames")
    app_cli_usage_says(ViewSignalsWithoutFramesIsUsage "view|x|--signals|a.json"
        "--profile, --signals and --plugin go with --frames")
    app_cli_usage_says(ViewAPluginWithoutFramesIsUsage "view|x|--plugin|a"
        "--profile, --signals and --plugin go with --frames")
    app_cli_usage_says(ViewSignalsAndAPluginIsUsage
        "view|x|--frames|--signals|a.json|--plugin|b"
        "--signals or --plugin, not both")
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
    app_cli_usage(FramesWithoutARecordingIsUsage "frames")
    app_cli_usage(FramesTwoRecordingsIsUsage "frames|x|y")
    app_cli_usage(FramesToBeforeFromIsUsage "frames|x|--from|2|--to|1")
    app_cli_usage(FramesToZeroIsUsage "frames|x|--to|0")
    app_cli_usage(FramesFromThatIsntSecondsIsUsage "frames|x|--from|soon")
    app_cli_usage(FramesANegativeFromIsUsage "frames|x|--from|-1")
    app_cli_usage(FramesAnUnknownOptionIsUsage "frames|x|--raw")
    # export and import (ADR-0054): refused before any file is read
    app_cli_usage(ExportWithoutAFileIsUsage "export|rec")
    app_cli_usage(ExportWithoutAFormatIsUsage "export|rec|out")
    app_cli_usage(ExportAnUnknownFormatIsUsage "export|rec|out|--format|blf")
    app_cli_usage(ExportToBeforeFromIsUsage
        "export|rec|out|--format|pcapng|--from|5|--to|4")
    app_cli_usage(ExportANegativeFromIsUsage "export|rec|out|--format|pcapng|--from|-1")
    app_cli_usage(ExportAnUnknownOptionIsUsage "export|rec|out|--format|pcapng|--scrub")
    app_cli_usage(ExportEstimateWithAFileIsUsage "export|rec|out|--format|csv|--estimate")
    app_cli_usage(ExportEstimateWithoutAFormatIsUsage "export|rec|--estimate")
    app_cli_usage(ExportEstimateOfPcapngIsUsage "export|rec|--format|pcapng|--estimate")
    app_cli_usage(ExportEstimateOfTheLongFormIsUsage "export|rec|--format|csv|--layout|long|--estimate")
    app_cli_usage(ImportWithoutCandumpIsUsage "import|bus.log|rec")
    # health (ADR-0054): refused before a session opens or a file is read
    app_cli_usage(HealthWithoutAFileIsUsage "health|mock://x")
    app_cli_usage(HealthWithoutAFormatIsUsage "health|mock://x|out")
    app_cli_usage(HealthAnUnknownFormatIsUsage "health|mock://x|out|--format|pdf")
    app_cli_usage(HealthARangeOnASessionIsUsage
        "health|mock://x|out|--format|json|--from|1")
    app_cli_usage(HealthSecondsOnARecordingIsUsage
        "health|rec|out|--format|json|--seconds|5")
    app_cli_usage(HealthSignalsAndAPluginIsUsage
        "health|mock://x|out|--format|json|--signals|a.json|--plugin|p")
    app_cli_usage(ImportWithoutAFolderIsUsage "import|candump|bus.log")
    app_cli_usage(ImportAnUnknownOptionIsUsage "import|candump|bus.log|rec|--map|m.json")
    app_cli_usage(ImportCsvMapAndATimeUnitIsUsage
        "import|csv|log.csv|rec|--map|m.json|--time-unit|s")
    app_cli_usage(ImportCsvATimeUnitThatIsntOneIsUsage
        "import|csv|log.csv|rec|--time-unit|h")
    app_cli_usage(ImportCsvPrintMapWithAFolderIsUsage
        "import|csv|log.csv|rec|--print-map")
    app_cli_usage(ImportCsvPrintMapAndAMapIsUsage
        "import|csv|log.csv|--print-map|--map|m.json")
    app_cli_usage(ImportCandumpATimeUnitIsUsage
        "import|candump|bus.log|rec|--time-unit|s")
    app_cli_usage(ImportCsvWithAnInterfaceIsUsage
        "import|csv|log.csv|rec|--map|m.json|--interface|can0")
    app_cli_usage(ImportCsvWithoutAFolderIsUsage "import|csv|log.csv|--map|m.json")
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
        # log writes nothing before its connect: the deadline's error line
        # comes after a start line of its own
        add_test(NAME CliConnectTest.ALogsConnectPastItsDeadlineEndsAfterAStartLine
            COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:havne-cli>
                    -DDLL=$<TARGET_FILE:fake_j2534> -DVERB=log
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_connect_deadline.cmake)
        set_tests_properties(CliConnectTest.ALogsConnectPastItsDeadlineEndsAfterAStartLine
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

# The shared core (HAVNE_SHARED_CORE, ADR-0053's appendix "the shared
# core as built"): test_c_abi.c again, linked against the library rather
# than the archive, so each call resolves through its export table
# (internal.h's hooks left out: ECUIO_C_ABI_SHARED); and ExportsTest
# (scripts/exports_check.py): the library exports the public headers'
# functions and nothing else, read with the platform's own tool, and
# everything a header hands out has its release there. Absent, not
# failing, where the option is off (Android, iOS, a fuzzing tree).
if(HAVNE_SHARED_CORE)
    add_executable(test_c_abi_shared test_c_abi.c)
    set_target_properties(test_c_abi_shared PROPERTIES C_EXTENSIONS OFF)
    app_enable_warnings(test_c_abi_shared)
    if(NOT MSVC)
        target_compile_options(test_c_abi_shared PRIVATE -pedantic)
    endif()
    target_compile_definitions(test_c_abi_shared PRIVATE ECUIO_C_ABI_SHARED=1)
    target_include_directories(test_c_abi_shared PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(test_c_abi_shared PRIVATE ecuio)
    if(WIN32)
        # ecuio.dll beside the test: Windows finds a DLL in the exe's folder
        add_custom_command(TARGET test_c_abi_shared POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:test_c_abi_shared> $<TARGET_FILE_DIR:test_c_abi_shared>
            COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME c_abi_shared COMMAND test_c_abi_shared)
    set_tests_properties(c_abi_shared PROPERTIES LABELS "abi")

    if(MSVC)
        # dumpbin ships beside link.exe, in the toolset's own folder (the
        # x86 one in a Win32 tree)
        get_filename_component(_app_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(APP_DUMPBIN dumpbin HINTS "${_app_linker_dir}" NO_DEFAULT_PATH)
        set(_app_exports_format dumpbin)
        set(_app_exports_tool "${APP_DUMPBIN}")
    elseif(APPLE)
        set(_app_exports_format macho)
        set(_app_exports_tool "${CMAKE_NM}")
    else()
        set(_app_exports_format elf)
        set(_app_exports_tool "${CMAKE_NM}")
    endif()
    if(NOT _app_exports_tool)
        message(FATAL_ERROR "HAVNE_SHARED_CORE: no ${_app_exports_format} tool to read the "
            "shared core's exports with (dumpbin beside link.exe, or CMAKE_NM)")
    endif()
    add_test(NAME ExportsTest.TheLibraryExportsTheDeclarations
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/exports_check.py exports
            $<TARGET_FILE:ecuio> ${_app_exports_format} ${_app_exports_tool}
            ${ECUIO_PUBLIC_HEADER_PATHS})
    add_test(NAME ExportsTest.EachAllocationHasItsRelease
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/exports_check.py releasers
            ${ECUIO_PUBLIC_HEADER_PATHS})
    add_test(NAME ExportsTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/exports_check.py selftest)
    set_tests_properties(ExportsTest.TheLibraryExportsTheDeclarations
        ExportsTest.EachAllocationHasItsRelease ExportsTest.SelfTest PROPERTIES LABELS "abi")
endif()

# The havne Python package (python/, ADR-0053 and its appendix "the havne
# package as built"): each python/tests/test_*.py a ctest of its own,
# PythonTest.<file>, under pytest through python/tests/run.py, which
# exits 77 (a skip) without pytest or when every case in the file
# skipped. The package loads this tree's shared core (HAVNE_LIBRARY); on
# Windows the J2534 case finds the fake DLL and this tree's surrogate
# (x64, or build-win32's x86) through the environment, as a wheel's
# package sets the surrogates from its own folder. test_vcan shares vcan0
# with test_vcan's suites: RESOURCE_LOCK vcan0. Only where the tests' Python has
# the tree's pointer size: a 64-bit Python can't load build-win32's x86
# core, so that tree registers them only when configured with a 32-bit
# Python (verify.sh's cross-arch step, HAVNE_PYTHON32).
set(_app_python_pointer 0)
if(HAVNE_SHARED_CORE AND Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import struct; print(struct.calcsize('P'))"
        OUTPUT_VARIABLE _app_python_pointer OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _app_python_rc)
    if(NOT _app_python_rc EQUAL 0)
        set(_app_python_pointer 0)
    endif()
endif()
if(_app_python_pointer EQUAL CMAKE_SIZEOF_VOID_P)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_app_surrogate_var ECUIO_SURROGATE_X64_EXE)
    else()
        set(_app_surrogate_var ECUIO_SURROGATE_EXE)
    endif()
    file(GLOB _app_python_tests CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/python/tests/test_*.py)
    foreach(_app_py IN LISTS _app_python_tests)
        get_filename_component(_app_name ${_app_py} NAME_WE)
        add_test(NAME PythonTest.${_app_name}
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/python/tests/run.py ${_app_py}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/python/tests)
        set(_app_env "HAVNE_LIBRARY=$<TARGET_FILE:ecuio>" "PYTHONDONTWRITEBYTECODE=1")
        if(WIN32)
            list(APPEND _app_env "HAVNE_FAKE_J2534_DLL=$<TARGET_FILE:fake_j2534>"
                 "${_app_surrogate_var}=$<TARGET_FILE:ecuio-j2534-surrogate>")
        endif()
        if(TARGET havne-cli)   # test_read_pids' ELM327 emulator
            list(APPEND _app_env "HAVNE_CLI=$<TARGET_FILE:havne-cli>")
        endif()
        set_tests_properties(PythonTest.${_app_name} PROPERTIES
            ENVIRONMENT "${_app_env}" SKIP_RETURN_CODE 77 TIMEOUT 180 LABELS "python")
        # The cases that need vcan0 take its lock and the "vcan" label, which
        # linux-build's run of them selects (-L vcan): a new one is named here only
        if(_app_name MATCHES "^test_(vcan|did_scan|isotp_fuzz)$")
            set_tests_properties(PythonTest.${_app_name} PROPERTIES
                RESOURCE_LOCK vcan0 LABELS "python;vcan")
        endif()
    endforeach()

    # python/examples/ecu_states.ipynb run top to bottom (tests/example_notebook.py);
    # skips without scikit-learn, nbconvert or ipykernel, as in CI. TIMEOUT
    # covers a cold import of SciPy and scikit-learn from a busy disk
    add_test(NAME ExampleTest.notebook
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/example_notebook.py)
    set_tests_properties(ExampleTest.notebook PROPERTIES
        ENVIRONMENT "HAVNE_LIBRARY=$<TARGET_FILE:ecuio>;PYTHONDONTWRITEBYTECODE=1"
        SKIP_RETURN_CODE 77 TIMEOUT 900 COST 38 LABELS "python")

    # The wheel itself (tests/wheel_installed.py): made from this tree's
    # shared core, installed in a fresh venv and used from outside the
    # tree. 64-bit trees only, since no x86 wheel ships. On Windows the x86
    # surrogate is build-win32's when there is one (HAVNE_SURROGATE_SOURCE
    # or the sibling tree, as the app's copy finds it). Not in a sanitizer
    # tree (core-sanitizers): its library needs the sanitizers' runtimes,
    # which make_wheel.py refuses as no wheel can count on them.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize")
        set(_app_wheel_args --library $<TARGET_FILE:ecuio>)
        if(WIN32)
            list(APPEND _app_wheel_args
                --surrogate-x64 $<TARGET_FILE:ecuio-j2534-surrogate>
                --fake-dll $<TARGET_FILE:fake_j2534>)
            set(_app_wheel_x86 "${HAVNE_SURROGATE_SOURCE}")
            if(NOT _app_wheel_x86 AND EXISTS
                    "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
                set(_app_wheel_x86 "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
            endif()
            if(_app_wheel_x86)
                list(APPEND _app_wheel_args --surrogate-x86 ${_app_wheel_x86})
                # the Win32 tree's fake beside it, run through the wheel's x86 surrogate
                get_filename_component(_app_wheel_win32 "${_app_wheel_x86}/../../.." ABSOLUTE)
                if(EXISTS "${_app_wheel_win32}/tests/Release/fake_j2534.dll")
                    list(APPEND _app_wheel_args --fake-dll-x86 "${_app_wheel_win32}/tests/Release/fake_j2534.dll")
                endif()
            endif()
        endif()
        add_test(NAME WheelTest.Installed
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/wheel_installed.py ${_app_wheel_args})
        set_tests_properties(WheelTest.Installed PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "python")
        set(_app_wheel_registered 1)
    endif()
    list(LENGTH _app_python_tests _app_python_count)
endif()
# What the probes above left out, judged at test time: under
# ECUIO_PYTEST_REQUIRED=1 a tree with no PythonTest.* case, or a 64-bit
# tree without sanitizers with no WheelTest.Installed, fails
# (tests/python_registered.cmake); elsewhere it says so and skips
if(NOT DEFINED _app_python_count)
    set(_app_python_count 0)
endif()
if(NOT DEFINED _app_wheel_registered)
    set(_app_wheel_registered 0)
endif()
set(_app_wheel_expected 0)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize")
    set(_app_wheel_expected 1)
endif()
add_test(NAME RequiredTest.ThePythonTestsAreRegistered
    COMMAND ${CMAKE_COMMAND} -DPYTHON_TESTS=${_app_python_count}
            -DWHEEL=${_app_wheel_registered} -DWHEEL_EXPECTED=${_app_wheel_expected}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/python_registered.cmake)
set_tests_properties(RequiredTest.ThePythonTestsAreRegistered PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:" LABELS "python")

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

    find_package(Qt6 6.12 COMPONENTS Core Gui Network REQUIRED)   # app/CMakeLists.txt's minimum

    # APP_BRIDGE_SOURCES: the bridge's own sources, which test_bridge and
    # bench_bridge compile as app_ui does
    include(${CMAKE_SOURCE_DIR}/app/bridge_sources.cmake)

    add_executable(test_bridge
        test_bridge.cpp
        test_bridge_session.cpp
        test_bridge_bluetooth.cpp
        test_bridge_frames.cpp
        test_bridge_frames_filter.cpp
        test_bridge_readonce.cpp
        test_bridge_expr.cpp
        test_bridge_obdb.cpp
        test_bridge_health.cpp
        test_bridge_replay.cpp
        test_bridge_replay_carryover.cpp
        test_bridge_replay_end.cpp
        test_bridge_diagnostics.cpp
        test_bridge_signalfiles.cpp
        test_bridge_signalfiles_plugin.cpp
        test_bridge_signalfiles_picks.cpp
        test_bridge_live.cpp
        test_bridge_fake.cpp
        test_bridge_fake_pick.cpp
        test_bridge_signals.cpp
        test_bridge_signals_trace.cpp
        test_bridge_ticks.cpp
        test_bridge_recorder.cpp
        test_bridge_recorder_armed.cpp
        test_bridge_conditions.cpp
        test_bridge_conditions_firing.cpp
        test_bridge_conditions_stale.cpp
        test_bridge_conditions_tones.cpp
        test_bridge_conditions_alloc.cpp
        test_recording_view.cpp
        test_recording_view_stretches.cpp
        test_recording_view_stats.cpp
        test_recording_view_open.cpp
        test_recording_view_passes.cpp
        test_recording_view_conditions.cpp
        test_recording_view_keys.cpp
        test_viewer_frames.cpp
        test_recording_import.cpp
        test_recording_import_map.cpp
        test_recording_export.cpp
        test_recording_export_estimate.cpp
        test_one_store.cpp
        test_one_store_update.cpp
        test_one_store_index.cpp
        test_one_store_wrap.cpp
        test_bridge_variantshape.cpp
        test_bridge_plugincopies.cpp
        test_bridge_pluginpages.cpp
        test_bridge_gate.cpp
        test_store_migration.cpp
        test_store_migration_marks.cpp
        test_store_removal.cpp
        ../app/recordingview.cpp
        ../app/recordingview_asks.cpp
        ../app/recordingview_queries.cpp
        ../app/recordingview_stretches.cpp
        ../app/recordingview_conditions.cpp
        ../app/recordingview_internal.h
        ../app/recording_frameset.cpp
        ../app/recording_pairs.cpp
        ../app/recordingexport.cpp
        ../app/recordingframes.cpp
        ${APP_BRIDGE_SOURCES}
        ../app/recordingimport.cpp
        ../app/healthreport.cpp
        ../app/framefilter.cpp
        ../app/obdbimport.cpp
        ../app/readonce.cpp
        ../app/exprcheck.cpp
        ../app/pluginstore.cpp
        ../app/pluginstore_pages.cpp
        ../app/pluginstore_import.cpp
        ../app/pluginstore_picks.cpp
        ../app/variantshape.cpp
        ../app/alerttone.cpp
        ../app/conditionmodel.cpp
        ../app/conditionstore.cpp
        ../app/storemigration.cpp
        ../app/signalfilter.cpp
        ../app/blepicker.cpp
        ../app/btconnect.cpp
        ../app/carryover.cpp
        ../app/adapterrelay.cpp
        ../app/windowtheme.cpp
        bridge_support.h
        old_state.h
        fake_bt_socket.h
        fake_theme.h
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
    # the recordings RecordingViewTest opens (ADR-0049) and the files it
    # imports (ADR-0054)
    target_compile_definitions(test_bridge PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
        ECUIO_RECORDING_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording"
        ECUIO_IMPORT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import"
        ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
        ECUIO_OBDB_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obdb/example-car.json"
        ECUIO_VEHICLEBOOK_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/vehiclebook"
    )

    target_link_libraries(test_bridge PRIVATE
        ecuio-core
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        GTest::gtest
    )
    # windowtheme.cpp colors the title bar through dwmapi
    if(WIN32)
        target_link_libraries(test_bridge PRIVATE dwmapi)
    endif()

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
    # took past the default 5 s and failed the build. The "qt" label marks
    # this block's tests, which linux-build runs
    gtest_discover_tests(test_bridge DISCOVERY_TIMEOUT 60 PROPERTIES LABELS "qt")
    # The dense-recording cases generate and read a million-row file, so
    # each counts as two jobs against ctest's -j; the include runs after
    # the discovery's, which defines the tests
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                 ${CMAKE_CURRENT_SOURCE_DIR}/bridge_processors.cmake)

    # The GUI thread's cost per frame by profile size, a measurement for
    # docs/performance.md; built with the tests, never run by ctest
    add_executable(bench_bridge
        bench_bridge.cpp
        ${APP_BRIDGE_SOURCES}
        ../app/recordingimport.cpp
        ../app/healthreport.cpp
        ../app/framefilter.cpp
        ../app/obdbimport.cpp
        ../app/readonce.cpp
        ../app/pluginstore.cpp
        ../app/pluginstore_pages.cpp
        ../app/pluginstore_import.cpp
        ../app/pluginstore_picks.cpp
        ../app/variantshape.cpp
        ../app/alerttone.cpp
        ../app/conditionmodel.cpp
        ../app/conditionstore.cpp
        ../app/storemigration.cpp
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
    # Recordings page opens the recording fixtures (ADR-0049) and lists a
    # CSV fixture's import (ADR-0054)
    target_compile_definitions(test_qml PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
        ECUIO_RECORDING_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recording"
        ECUIO_IMPORT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import"
        ECUIO_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins"
        ECUIO_OBDB_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obdb/example-car.json"
        ECUIO_VEHICLEBOOK_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/vehiclebook"
        ECUIO_LAYOUT_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/layouts"
    )
    # Settings keeps only a real adapter's connect, so its cases connect to
    # havne-cli's ELM327 emulator on every platform
    # (Harness.startElm327Emulator)
    if(TARGET havne-cli)
        target_compile_definitions(test_qml PRIVATE
            HAVNE_CLI_PATH="$<TARGET_FILE:havne-cli>")
        add_dependencies(test_qml havne-cli)
    endif()

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
    # Seconds a file's entry took in a full run's
    # CTestCostData.txt, so a tree with no history starts the long ones first
    set(_qml_cost_dashboard 8)
    set(_qml_cost_dashboard_pages 54)
    set(_qml_cost_dashboard_panel 19)
    set(_qml_cost_dashedit 28)
    set(_qml_cost_dashedit_panel 17)
    set(_qml_cost_dashedit_gesture 28)
    set(_qml_cost_dashedit_color 29)
    set(_qml_cost_dashedit_cells 18)
    set(_qml_cost_dashedit_undo 38)
    set(_qml_cost_dashedit_layout 23)
    set(_qml_cost_driveview 28)
    set(_qml_cost_driveview_sheet 17)
    set(_qml_cost_driveview_platform 20)
    set(_qml_cost_phone_fit 35)
    set(_qml_cost_phone_viewer 35)
    set(_qml_cost_phone_frame 35)
    set(_qml_cost_phone_headers 25)
    set(_qml_cost_phone_connect 40)
    set(_qml_cost_recordings 4)
    set(_qml_cost_recordings_export 3)
    set(_qml_cost_recordings_layout 3)
    set(_qml_cost_recordings_cursors 4)
    set(_qml_cost_recordings_panes 3)
    set(_qml_cost_recordings_stretches 4)
    set(_qml_cost_recordings_compare 3)
    set(_qml_cost_recordings_baseline 3)
    set(_qml_cost_diagnostics 7)
    set(_qml_cost_accessibility 1)
    set(_qml_cost_alerts 7)
    set(_qml_cost_chartlayout_panes 1)
    set(_qml_cost_dashboardbook_files 1)
    set(_qml_cost_dashboardbook_writes 1)
    set(_qml_cost_dashedit_plugpages 4)
    set(_qml_cost_dashedit_side 5)
    set(_qml_cost_documentpicker 3)
    set(_qml_cost_home 6)
    set(_qml_cost_kinds 4)
    set(_qml_cost_linkhealth 5)
    set(_qml_cost_liveview 14)
    set(_qml_cost_liveview_pause 11)
    set(_qml_cost_log 25)
    set(_qml_cost_recordings_keys 3)
    set(_qml_cost_settings 6)
    set(_qml_cost_settings_devices 9)
    set(_qml_cost_settings_listobdb 6)
    set(_qml_cost_settings_plugins 6)
    set(_qml_cost_settings_vehicle 10)
    set(_qml_cost_theme 6)
    set(_qml_cost_alertband 1)
    set(_qml_cost_buttons 1)
    set(_qml_cost_chartfonts 1)
    set(_qml_cost_chartlayout 1)
    set(_qml_cost_dashboardbook 1)
    set(_qml_cost_driveplatform 1)
    set(_qml_cost_fields 1)
    set(_qml_cost_gauges 1)
    set(_qml_cost_segments 1)
    set(_qml_cost_sessionlabel 1)
    set(_qml_cost_settings_computed 2)
    set(_qml_cost_settings_own 5)
    set(_qml_cost_settings_rows 1)
    set(_qml_cost_settings_store 4)
    set(_qml_cost_settings_unloadable 1)
    set(_qml_cost_settings_uri 4)
    set(_qml_cost_signalform 1)
    set(_qml_cost_storemove 1)
    set(_qml_cost_vehiclebook 1)
    file(GLOB _qml_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_*.qml)
    foreach(_qml_test IN LISTS _qml_tests)
        get_filename_component(_qml_name ${_qml_test} NAME_WE)
        string(REGEX REPLACE "^tst_" "" _qml_name ${_qml_name})
        # A file's bound is a hang guard, not a measurement: each case
        # waits for what it checks with tryVerify, and the recordings
        # files open a 10,200-frame recording, which a machine building
        # beside the suite took past 60 s in all
        add_test(NAME QmlTest.${_qml_name} COMMAND test_qml -input ${_qml_test})
        set_tests_properties(QmlTest.${_qml_name} PROPERTIES
            ENVIRONMENT "${_qml_test_env}"
            TIMEOUT 180
            PROCESSORS 2
            LABELS "qt"
        )
        if(DEFINED _qml_cost_${_qml_name})
            set_tests_properties(QmlTest.${_qml_name} PROPERTIES
                COST ${_qml_cost_${_qml_name}})
        endif()
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
    gtest_discover_tests(test_j2534_discovery DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# Fake J2534 DLL target (Windows only, HARNESS tier)
if(WIN32)
    add_library(fake_j2534 SHARED
        fake_j2534.cpp
        fake_j2534_read.cpp
        fake_j2534_write.cpp
        fake_j2534_ioctl.cpp
        fake_j2534.def
    )

    target_compile_definitions(fake_j2534 PRIVATE
        TEST_HOOKS_ENABLED
    )

    # core/src: sim_security.h, the simulated ECU's security access that
    # the fakes' ECU answers with (fake_ecus.h)
    target_include_directories(fake_j2534 PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/src
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
        fake_j2534_read.cpp
        fake_j2534_write.cpp
        fake_j2534_ioctl.cpp
        fake_j2534_legacy.def
    )

    target_compile_definitions(fake_j2534_legacy PRIVATE
        TEST_HOOKS_ENABLED
    )

    target_include_directories(fake_j2534_legacy PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/src
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
    gtest_discover_tests(test_j2534_load DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 end-to-end stream tests (Win32-only, HARNESS tier)
if(WIN32)
    add_executable(test_j2534_stream
        ../tests/test_j2534_stream.cpp
        ../tests/test_j2534_stream_requests.cpp
        ../tests/test_j2534_stream_rotation.cpp
        ../tests/test_j2534_stream_probe.cpp
        ../tests/test_j2534_stream_session.cpp
        ../tests/test_j2534_stream_clear.cpp
        ../tests/test_j2534_stream_stamps.cpp
        ../tests/test_j2534_stream_focus.cpp
        ../tests/test_j2534_stream_condition.cpp
        ../tests/test_j2534_clock.cpp
        ../tests/test_j2534_pack.cpp
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
    gtest_discover_tests(test_j2534_stream DISCOVERY_TIMEOUT 60
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
        test_j2534_surrogate_open.cpp
        test_j2534_surrogate_wedged.cpp
        test_j2534_surrogate_reads.cpp
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
    gtest_discover_tests(test_j2534_surrogate DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS} RUN_SERIAL TRUE
    )
endif()

# J2534 link-health tests (Win32-only, HARNESS tier): every case runs a
# session through the surrogate: in this tree the x64 one beside the
# test, in the cross-arch run the Win32 tree's. test_j2534_talk.cpp's
# cases (a request longer than a frame, through the test seam) and
# test_j2534_car.cpp's (18 modules on the fake) run the same way.
if(WIN32)
    add_executable(test_j2534_health
        test_j2534_health.cpp
        test_j2534_health_vx.cpp
        test_j2534_health_replies.cpp
        test_j2534_health_overflow.cpp
        test_j2534_hold_bus.cpp
        test_j2534_talk.cpp
        test_j2534_car.cpp
    )
    target_link_libraries(test_j2534_health PRIVATE
        ecuio-core
        GTest::gtest_main
    )
    # core/src: sim_security.h, whose key the talk cases compute
    target_include_directories(test_j2534_health PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src
    )
    target_compile_definitions(test_j2534_health PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
        SURROGATE_EXE_PATH="$<TARGET_FILE:ecuio-j2534-surrogate>"
    )
    add_dependencies(test_j2534_health fake_j2534)
    app_deploy_surrogate(test_j2534_health)
    app_enable_warnings(test_j2534_health)
    gtest_discover_tests(test_j2534_health DISCOVERY_TIMEOUT 60
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
    # core/src for surrogate_log_names.h: the out-of-memory loop seeds the
    # session log's folder with the names the client prunes
    target_include_directories(test_j2534_connect PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src
    )
    add_dependencies(test_j2534_connect fake_j2534)
    add_dependencies(test_j2534_connect fake_j2534_legacy)   # beside the fake
    app_deploy_surrogate(test_j2534_connect)
    app_enable_warnings(test_j2534_connect)
    gtest_discover_tests(test_j2534_connect DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
    # The out-of-memory loop outlasts the ceiling's margin under load; the
    # include runs after the discovery's, which defines the test
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                 ${CMAKE_CURRENT_SOURCE_DIR}/j2534_timeouts.cmake)
endif()

# J2534 routing tests (Win32-only, HARNESS tier: the surrogate chosen by
# PE architecture: the x64 one for x64 (CI), the x86 one for x86
# (dev-machine, env-gated via ECUIO_FAKE_DLL/ECUIO_SURROGATE_EXE))
if(WIN32)
    add_executable(test_j2534_routing
        test_j2534_routing.cpp
        test_j2534_pipe.cpp
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
    gtest_discover_tests(test_j2534_routing DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# Reply timing against the fakes' ECU through each transport this
# platform has (ADR-0042 section 4), a measurement for docs/performance.md;
# built with the tests, never run by ctest
find_package(Threads REQUIRED)
# The request engine on a simulated clock (tests/pacing_bench.h):
# test_pacing gates the pacing on the model's figures; bench_pacing
# prints them for docs/performance.md (not in ctest)
add_executable(test_pacing test_pacing.cpp test_pacing_quiet.cpp
                           test_pacing_session.cpp test_pacing_stall.cpp)
target_include_directories(test_pacing PRIVATE ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/cli)
target_link_libraries(test_pacing PRIVATE ecuio-core GTest::gtest_main)
app_enable_warnings(test_pacing)
gtest_discover_tests(test_pacing DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
add_executable(bench_pacing bench_pacing.cpp)
target_include_directories(bench_pacing PRIVATE ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/cli)
target_link_libraries(bench_pacing PRIVATE ecuio-core)
app_enable_warnings(bench_pacing)

add_executable(bench_timing bench_timing.cpp)
target_link_libraries(bench_timing PRIVATE ecuio-core Threads::Threads)
# core/src: fake_ecus.h's sim_security.h
target_include_directories(bench_timing PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
app_enable_warnings(bench_timing)
if(WIN32)
    target_compile_definitions(bench_timing PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(bench_timing fake_j2534)
    app_deploy_surrogate(bench_timing)
    target_link_libraries(bench_timing PRIVATE winmm)   # timeBeginPeriod
endif()
