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
