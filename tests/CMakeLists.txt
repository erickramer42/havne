# tests/CMakeLists.txt
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

# --- Core tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_core test_core.cpp)

target_link_libraries(test_core PRIVATE
    qttune-core
    GTest::gtest_main
)

gtest_discover_tests(test_core) 

# --- Signals tests: MUST stay Qt-free (Ubuntu CI job has no Qt installed) ---
add_executable(test_signals test_signals.cpp)

target_link_libraries(test_signals PRIVATE
    qttune-core
    GTest::gtest_main
)

gtest_discover_tests(test_signals)

# --- C ABI compilation check: MUST stay Qt-free, builds everywhere ---
# Compiles the public headers as strict C and links exported symbols
# from a C TU — catches C++-isms and name-mangling regressions an FFI
# consumer would hit. Reviewer finding: nothing else compiled headers as C.
add_executable(test_c_abi test_c_abi.c)

target_include_directories(test_c_abi PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_c_abi PRIVATE
    qttune-core
)

add_test(NAME c_abi COMMAND test_c_abi)
set_tests_properties(c_abi PROPERTIES LABELS "abi")

# --- Bridge tests: only when the full app (and Qt) is available ---
if(NOT QTTUNE_BUILD_CORE_ONLY)

    find_package(Qt6 COMPONENTS Core REQUIRED)

    add_executable(test_bridge
        test_bridge.cpp
        ../app/qttunebridge.cpp
    )

    set_target_properties(test_bridge PROPERTIES AUTOMOC ON)

    target_include_directories(test_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app
    )

    target_link_libraries(test_bridge PRIVATE
        qttune-core
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

# J2534 discovery tests (Win32-only)
if(WIN32)
    add_executable(test_j2534_discovery
        ../tests/test_j2534_discovery.cpp
    )
    target_link_libraries(test_j2534_discovery PRIVATE
        qttune-core
        GTest::gtest_main
    )
    set_target_properties(test_j2534_discovery PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    gtest_discover_tests(test_j2534_discovery)
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
endif()

# J2534 loader tests (Win32-only, HARNESS tier — real LoadLibraryEx
# against the fake vendor DLL; path baked in at generation time)
if(WIN32)
    add_executable(test_j2534_load
        ../tests/test_j2534_load.cpp
    )
    target_link_libraries(test_j2534_load PRIVATE
        qttune-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_load PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_load fake_j2534)
    gtest_discover_tests(test_j2534_load)
endif()

# J2534 end-to-end stream tests (Win32-only, HARNESS tier)
if(WIN32)
    add_executable(test_j2534_stream
        ../tests/test_j2534_stream.cpp
    )
    target_link_libraries(test_j2534_stream PRIVATE
        qttune-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_stream PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_stream fake_j2534)
    gtest_discover_tests(test_j2534_stream)
endif()

# J2534 surrogate host tests (Win32-only, HARNESS tier — real process
# spawn: named pipe + shared ring + kill-on-close job, end to end)
# Arch note: in this tree's configure this is the all-same-arch IPC test
# (CI-valid); the true cross-arch case (x64 test -> x86 surrogate -> x86
# fake) runs on a dev machine with QTTUNE_SURROGATE_EXE pointing at
# artifacts from a second, Win32 (-A Win32) configure of the same tree.
if(WIN32)
    add_executable(test_j2534_surrogate
        test_j2534_surrogate.cpp
    )
    target_link_libraries(test_j2534_surrogate PRIVATE
        qttune-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_surrogate PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_surrogate fake_j2534)
    add_dependencies(test_j2534_surrogate qttune-j2534-surrogate)

    # Deploy the surrogate exe next to the test exe: the client's
    # build_exe_path() resolves it by walking its own image directory
    # (same discipline as the baked fake-DLL path — no PATH games).
    add_custom_command(TARGET test_j2534_surrogate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:qttune-j2534-surrogate>
            $<TARGET_FILE_DIR:test_j2534_surrogate>
        COMMENT "Deploying surrogate exe next to test_j2534_surrogate"
    )

    gtest_discover_tests(test_j2534_surrogate)
endif()

# J2534 routing tests (Win32-only, HARNESS tier — backend selection by
# PE architecture: in-proc for x64 (CI), surrogate for x86 (dev-machine,
# env-gated via QTTUNE_FAKE_DLL/QTTUNE_SURROGATE_EXE))
if(WIN32)
    add_executable(test_j2534_routing
        test_j2534_routing.cpp
    )
    target_link_libraries(test_j2534_routing PRIVATE
        qttune-core
        GTest::gtest_main
    )
    target_compile_definitions(test_j2534_routing PRIVATE
        FAKE_J2534_DLL_PATH="$<TARGET_FILE:fake_j2534>"
    )
    add_dependencies(test_j2534_routing fake_j2534)
    gtest_discover_tests(test_j2534_routing)
endif()