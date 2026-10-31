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
add_executable(test_core test_core.cpp)

# core/src for mock_drive_cycle.h and link_health.h, which are internal
# (test_obd2 precedent).
target_include_directories(test_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_core PRIVATE
    ecuio-core
    GTest::gtest_main
)

# The scrubbed real-bus recording replay:// plays (ADR-0023)
target_compile_definitions(test_core PRIVATE
    ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
)

app_enable_warnings(test_core)
gtest_discover_tests(test_core)

# --- Signals tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_signals test_signals.cpp)

target_link_libraries(test_signals PRIVATE
    ecuio-core
    GTest::gtest_main
)

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
# and the readiness monitors' names
target_compile_definitions(test_obd2 PRIVATE
    ECUIO_OBD2_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obd2/service01.csv"
    ECUIO_OBD2_MONITORS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/obd2/monitors.csv"
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

# --- SocketCAN listening's rules: Qt-free, every platform (UNIT, ADR-0032) ---
# socketcan_wire.h is internal and portable (the kernel's values mirrored),
# so its rules test everywhere; the transport on a bus is test_vcan's.
add_executable(test_socketcan test_socketcan.cpp)
target_include_directories(test_socketcan PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test_socketcan PRIVATE ecuio-core GTest::gtest_main)
app_enable_warnings(test_socketcan)
gtest_discover_tests(test_socketcan)

# --- The simulated CAN bus: Qt-free, Linux only (SocketCAN) ---
# test_vcan.cpp: vcan_bus.h's fake ECU through the kernel's ISO-TP and a
# recording played onto vcan0. Each case skips without the interface;
# CI's linux-build brings it up and sets ECUIO_VCAN_REQUIRED=1.
# test_vcan_socketcan.cpp: the SocketCAN transport on that bus
# (ADR-0032, SocketCanBusTest); it holds the binary's new_counter.h.
# RUN_SERIAL: the cases share the bus, and an error frame passes every
# session's ID filter.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_vcan test_vcan.cpp test_vcan_socketcan.cpp)
    target_compile_definitions(test_vcan PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(test_vcan PRIVATE ecuio-core GTest::gtest_main Threads::Threads)
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
    add_test(NAME AbiReportTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/abi_report.py selftest)
    # packaging/sbom.py, the release's SBOM (ADR-0035 §5)
    add_test(NAME SbomTest.SelfTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/packaging/sbom.py selftest)
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
        app_cli_golden(MockDriveCycle mock.jsonl --ignore t_us
            -- $<TARGET_FILE:havne-cli> stream mock://golden --values --count 40)
        app_cli_golden(BadUriIsRefused bad-uri.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> stream "mock://golden?speed=2")
        app_cli_golden(UnknownProfileIsRefused unknown-profile.jsonl --exit 3
            -- $<TARGET_FILE:havne-cli> stream "mock://golden?profile=nonesuch")
        # A replay cut short under it stops early, and the stream ends
        # with it instead of waiting for a last frame that never comes
        add_test(NAME CliReplayTest.ACutRecordingEndsTheStream
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_recording_changed.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliReplayTest.ACutRecordingEndsTheStream PROPERTIES TIMEOUT 30)
        # A field past every reply is named on an absent line (ADR-0039 §6)
        add_test(NAME CliReplayTest.AnAbsentFieldIsNamed
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_absent.py
                    $<TARGET_FILE:havne-cli>)
        set_tests_properties(CliReplayTest.AnAbsentFieldIsNamed PROPERTIES TIMEOUT 30)
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
    # A stop from outside (ADR-0031 §6): SIGINT and SIGTERM on POSIX;
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

# Core's USDT probes (ADR-0034 §4) in each binary that links core and
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

    find_package(Qt6 6.12 COMPONENTS Core REQUIRED)   # app/CMakeLists.txt's minimum

    add_executable(test_bridge
        test_bridge.cpp
        ../app/sessionbridge.cpp
        ../app/sessionrecorder.cpp
        ../app/blepicker.cpp
        ../app/btconnect.cpp
        ../app/carryover.cpp
    )

    set_target_properties(test_bridge PROPERTIES AUTOMOC ON)
    app_enable_warnings(test_bridge)

    target_include_directories(test_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app
        ${CMAKE_SOURCE_DIR}/cli
    )

    # The engine-start fixture, which both decode paths read (ADR-0024)
    target_compile_definitions(test_bridge PRIVATE
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
    )

    target_link_libraries(test_bridge PRIVATE
        ecuio-core
        Qt6::Core
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

    gtest_discover_tests(test_bridge)

    # The GUI thread's cost per frame by profile size, a measurement for
    # docs/testing.md; built with the tests, never run by ctest
    add_executable(bench_bridge
        bench_bridge.cpp
        ../app/sessionbridge.cpp
        ../app/sessionrecorder.cpp
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
    # engine-start fixture feeds the Dashboard's peak hold (ADR-0023)
    target_compile_definitions(test_qml PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        ECUIO_REPLAY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay"
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
    gtest_discover_tests(test_j2534_surrogate
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
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
