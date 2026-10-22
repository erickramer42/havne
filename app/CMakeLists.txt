# File/layer: app/CMakeLists.txt | CMake 3.21+, Qt 6.5+/Quick
# Testing: HARNESS — build-time behavior; verify by launching the app with
#     NO env vars and connecting the fake direct-load URI; the surrogate
#     must spawn from the exe dir. Verify the no-variable case still builds
#     clean (contributors without a win32 tree). Check CI invariant: this
#     file is app-side only, core stays Qt-free/untouched.

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

qt_policy(SET QTP0004 NEW)

qt_add_executable(qttune
    MANUAL_FINALIZATION
    main.cpp
    bridgesingleton.h
    qttunebridge.cpp
    qttunebridge.h
    sessionrecorder.cpp
    sessionrecorder.h
)

set_source_files_properties(qml/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(qttune
    URI QtTune
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/HomePage.qml
        qml/ThemedButton.qml
        qml/PageHeader.qml
        qml/DashboardPage.qml
        qml/LogPage.qml
        qml/SettingsPage.qml
        qml/NavButton.qml
        qml/Theme.qml
        qml/LiveViewPage.qml
        qml/SignalHub.qml
        qml/GaugeBase.qml
        qml/GaugeArc.qml
        qml/GaugeCard.qml
        qml/AnalogGauge.qml
        qml/RingGauge.qml
        qml/DigitalGauge.qml
        qml/BarGauge.qml
        qml/ShiftLightBar.qml
)

target_link_libraries(qttune PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    ecuio-core
)
qttune_enable_warnings(qttune)
# MSVC's C4702 (unreachable code) fires inside Qt's inline headers, as
# the generated QML registration code instantiates them; none of it is
# this project's code.
if(MSVC)
    target_compile_options(qttune PRIVATE /wd4702)
endif()

if(WIN32)
    set_target_properties(qttune PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    set_target_properties(qttune PROPERTIES MACOSX_BUNDLE TRUE)
endif()
# Custom Android package template (manifest, icons). Only set once the
# directory exists, so androiddeployqt is never pointed at a missing one.
if(ANDROID AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android")
    set_target_properties(qttune PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
endif()

# Icon helpers later: qt_add_resources, ios/macos asset catalogs, etc.

if(QT_FEATURE_qml_debug)
    target_compile_definitions(qttune PRIVATE QT_QML_DEBUG)
endif()

# --- Deploy Qt runtime DLLs next to the exe (Windows only) ---
if(WIN32)
    get_target_property(QT_WINDEPLOYQT_LOCATION Qt6::windeployqt IMPORTED_LOCATION)

    if(QT_WINDEPLOYQT_LOCATION)
        add_custom_command(TARGET qttune POST_BUILD
            COMMAND ${QT_WINDEPLOYQT_LOCATION}
                    --no-compiler-runtime
                    --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml
                    $<TARGET_FILE:qttune>
            COMMENT "Deploying Qt runtime DLLs"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found - manual deployment required")
    endif()
endif()

# --- Deploy the Win32 J2534 surrogate next to the exe (Windows only) ---
# The surrogate that matters for real vendor DLLs is the x86 one from the
# build-win32 tree. The x64 tree also builds a surrogate (for the
# same-arch test suites), so this MUST NOT use $<TARGET_FILE:...> — copying
# the x64 one next to the app would make build_exe_path() spawn it for an
# x86 vendor DLL and die at LoadLibrary with no env var to blame.
#
# Resolution order:
#   1. QTTUNE_SURROGATE_SOURCE cache variable (explicit override)
#   2. Auto-detect: conventional sibling build-win32 tree
#      (development.md two-tree layout), any config, Release preferred
#   3. Neither -> STATUS message, no copy; runtime ECUIO_SURROGATE_EXE
#      env override still works (checked first by the client).
# --- Deploy the Win32 J2534 surrogate next to the exe (Windows only) ---
# The x86 surrogate from build-win32 hosts x86 vendor DLLs; this tree's
# own surrogate is x64 and MUST NOT be copied here. Resolution: explicit
# QTTUNE_SURROGATE_SOURCE, else auto-detect in sibling build-win32, else
# no copy (runtime ECUIO_SURROGATE_EXE env override still works).
set(QTTUNE_SURROGATE_SOURCE "" CACHE PATH "Win32-tree ecuio-j2534-surrogate.exe to deploy; empty = auto-detect")

if(WIN32 AND QTTUNE_SURROGATE_SOURCE STREQUAL "")
    file(GLOB _sur_found "${CMAKE_SOURCE_DIR}/build-win32/surrogate/*/ecuio-j2534-surrogate.exe")
    if(EXISTS "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
        list(INSERT _sur_found 0 "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
    endif()
    list(LENGTH _sur_found _sur_found_n)
    if(_sur_found_n GREATER 0)
        list(GET _sur_found 0 QTTUNE_SURROGATE_SOURCE)
    endif()
endif()

if(WIN32 AND NOT QTTUNE_SURROGATE_SOURCE STREQUAL "")
    if(NOT EXISTS "${QTTUNE_SURROGATE_SOURCE}")
        message(FATAL_ERROR "QTTUNE_SURROGATE_SOURCE not found: ${QTTUNE_SURROGATE_SOURCE}")
    endif()
    message(STATUS "Deploying Win32 J2534 surrogate: ${QTTUNE_SURROGATE_SOURCE}")
    add_custom_command(TARGET qttune POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QTTUNE_SURROGATE_SOURCE}"
                $<TARGET_FILE_DIR:qttune>
        COMMENT "Deploying Win32 J2534 surrogate next to qttune.exe"
        VERBATIM
    )
elseif(WIN32)
    message(STATUS "No build-win32 surrogate found; build the Win32 tree per development.md and reconfigure, or set QTTUNE_SURROGATE_SOURCE, or export ECUIO_SURROGATE_EXE at runtime")
endif()

qt_finalize_executable(qttune)
