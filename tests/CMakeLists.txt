# File/layer: tests/CMakeLists.txt | CMake 3.21+, all three CI platforms
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
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

# core/src for mock_drive_cycle.h, which is internal (test_obd2 precedent).
target_include_directories(test_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_core PRIVATE
    ecuio-core
    GTest::gtest_main
)

qttune_enable_warnings(test_core)
gtest_discover_tests(test_core)

# --- Signals tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_signals test_signals.cpp)

target_link_libraries(test_signals PRIVATE
    ecuio-core
    GTest::gtest_main
)

qttune_enable_warnings(test_signals)
gtest_discover_tests(test_signals)

# --- OBD-II set + scheduler tests: MUST stay Qt-free (UNIT tier) ---
# obd2_signal_sets.cpp is ALL-platform (signal data is transport-
# agnostic), and pid_scheduler.h is header-only, so this suite builds
# and runs in the headless Ubuntu job alongside test_core/test_signals.
# The explicit core/src include is required because pid_scheduler.h is
# INTERNAL — ecuio-core's PUBLIC include dirs cover only core/include,
# and the scheduler is deliberately not part of the public tree.
add_executable(test_obd2 test_obd2.cpp)

target_include_directories(test_obd2 PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(test_obd2 PRIVATE
    ecuio-core
    GTest::gtest_main
)

qttune_enable_warnings(test_obd2)
gtest_discover_tests(test_obd2)

# --- C ABI compilation check: MUST stay Qt-free, builds everywhere ---
# Compiles the public headers as strict C and links exported symbols
# from a C TU — catches C++-isms and name-mangling regressions an FFI
# consumer would hit. It is the only target that compiles headers as C:
# ISO C11 without compiler extensions, and -pedantic where the compiler
# has it, so a GNU-only construct in a header warns.
add_executable(test_c_abi test_c_abi.c)
set_target_properties(test_c_abi PROPERTIES C_EXTENSIONS OFF)
qttune_enable_warnings(test_c_abi)
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

# --- Bridge tests: only when the full app (and Qt) is available ---
if(NOT QTTUNE_BUILD_CORE_ONLY)

    find_package(Qt6 COMPONENTS Core REQUIRED)

    add_executable(test_bridge
        test_bridge.cpp
        ../app/qttunebridge.cpp
        ../app/sessionrecorder.cpp
    )

    set_target_properties(test_bridge PROPERTIES AUTOMOC ON)
    qttune_enable_warnings(test_bridge)

    target_include_directories(test_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app
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

    # Tell ctest/gtest_discover where Qt DLLs live at runtime
    get_filename_component(QT_BIN_DIR "${Qt6Core_DIR}/../../bin" REALPATH)
endif()

# TIMEOUT POLICY: every J2534 suite carries a per-test 30s ceiling.
# The slowest legitimate case (cross-arch TimedOutOp ~3.5s,
# BlockedVendorOpen ~2.5s) has near-10x headroom, so a slow CI VM
# cannot false-alarm, but a hang in vendor or pipe code (a blocking
# flush, a wedged child) becomes a bounded "Failed (Timeout)" instead
# of a wedged runner. Non-J2534 suites keep ctest defaults —
# no known hang surface there, and mock tests are sub-2s anyway.
set(J2534_TEST_TIMEOUT_SECONDS 30)

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
    qttune_enable_warnings(test_j2534_discovery)
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
    qttune_enable_warnings(fake_j2534)

    # LEGACY-spelling fixture: the fallback-vendor case — exports the
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
    qttune_enable_warnings(fake_j2534_legacy)
endif()

# test_bridge's connect and disconnect thread tests run a fake J2534
# session; they're compiled only on Windows
if(WIN32 AND NOT QTTUNE_BUILD_CORE_ONLY)
    target_compile_definitions(test_bridge PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_bridge fake_j2534)
endif()

# J2534 loader tests (Win32-only, HARNESS tier — real LoadLibraryEx
# against the fake vendor DLL; path baked in at generation time)
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
    qttune_enable_warnings(test_j2534_load)
    gtest_discover_tests(test_j2534_load
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 end-to-end stream tests (Win32-only, HARNESS tier)
if(WIN32)
    add_executable(test_j2534_stream
        ../tests/test_j2534_stream.cpp
    )
    # core/src for j2534_backoff.h, which is internal (test_obd2 precedent).
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
    qttune_enable_warnings(test_j2534_stream)
    gtest_discover_tests(test_j2534_stream
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 surrogate host tests (Win32-only, HARNESS tier — real process
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
    add_dependencies(test_j2534_surrogate ecuio-j2534-surrogate)

    # Deploy the surrogate exe next to the test exe: the client's
    # build_exe_path() resolves it by walking its own image directory
    # (same discipline as the baked fake-DLL path — no PATH games).
    add_custom_command(TARGET test_j2534_surrogate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ecuio-j2534-surrogate>
            $<TARGET_FILE_DIR:test_j2534_surrogate>
        COMMENT "Deploying surrogate exe next to test_j2534_surrogate"
    )

    qttune_enable_warnings(test_j2534_surrogate)
    gtest_discover_tests(test_j2534_surrogate
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 link-health tests (Win32-only, HARNESS tier): every case runs
# one session in-proc and one through the surrogate. In this tree the
# surrogate case uses the x64 surrogate by path (ECUIO_SURROGATE_EXE,
# set per case), so no second copy lands next to the test exe; the
# cross-arch run points it at the Win32 tree's instead.
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
    add_dependencies(test_j2534_health ecuio-j2534-surrogate)
    qttune_enable_warnings(test_j2534_health)
    gtest_discover_tests(test_j2534_health
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 connect-failure tests (Win32-only, HARNESS tier): why a connect
# failed, each case in-proc and through the surrogate, set up as for
# test_j2534_health.
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
    add_dependencies(test_j2534_connect ecuio-j2534-surrogate)
    qttune_enable_warnings(test_j2534_connect)
    gtest_discover_tests(test_j2534_connect
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()

# J2534 routing tests (Win32-only, HARNESS tier — backend selection by
# PE architecture: in-proc for x64 (CI), surrogate for x86 (dev-machine,
# env-gated via ECUIO_FAKE_DLL/ECUIO_SURROGATE_EXE))
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
    qttune_enable_warnings(test_j2534_routing)
    gtest_discover_tests(test_j2534_routing
        PROPERTIES TIMEOUT ${J2534_TEST_TIMEOUT_SECONDS}
    )
endif()
