# File/layer: app/CMakeLists.txt | CMake 3.21+, Qt 6.12+/Quick
# Testing: HARNESS — build-time behavior; verify by launching the app with
#     NO env vars and connecting the fake direct-load URI; the surrogate
#     must spawn from the exe dir. Verify the no-variable case still builds
#     clean (contributors without a win32 tree). Check CI invariant: this
#     file is app-side only, core stays Qt-free/untouched. Release builds
#     carry no QML debugging: stage.ps1 fails a package whose exe imports
#     QQmlDebuggingEnabler or that holds qmltooling.

# 6.12 is the version CI and local builds use (macOS needs it), and
# REQUIRES sets every Qt policy up to it to NEW, QTP0004 among them.
find_package(Qt6 6.12 REQUIRED COMPONENTS Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.12)

# The QtTune QML module and the bridge it binds live in a static library
# of their own, so test_qml (tests/CMakeLists.txt) loads the same module
# the app does. Whatever links it also links qttune_uiplugin and imports
# the plugin with Q_IMPORT_QML_PLUGIN(QtTunePlugin), or the module's
# types are never registered.
qt_add_library(qttune_ui STATIC
    bridgesingleton.h
    qttunebridge.cpp
    qttunebridge.h
    latencystats.h
    sessionrecorder.cpp
    sessionrecorder.h
)

set_source_files_properties(qml/Theme.qml qml/ChartLayout.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(qttune_ui
    URI QtTune
    VERSION 1.0
    # Ends in the URI, as qmllint expects of a library's module
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/QtTune
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
        qml/ChartLayout.qml
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

target_include_directories(qttune_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qttune_ui PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
    ecuio-core
)
qttune_enable_warnings(qttune_ui)
# MSVC's C4702 (unreachable code) fires inside Qt's inline headers, as
# the generated QML registration code instantiates them; none of it is
# this project's code.
if(MSVC)
    target_compile_options(qttune_ui PRIVATE /wd4702)
endif()

qt_add_executable(qttune
    MANUAL_FINALIZATION
    main.cpp
)

target_link_libraries(qttune PRIVATE
    qttune_ui
    qttune_uiplugin
)
qttune_enable_warnings(qttune)

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

# QML debugging in Debug builds only. A build with QT_QML_DEBUG opens a
# debug server when launched with -qmljsdebugger, one that can run
# script in the app with no authentication, so no release carries it
# (packaging/windows/stage.ps1 checks the exe's imports).
if(QT_FEATURE_qml_debug)
    target_compile_definitions(qttune PRIVATE $<$<CONFIG:Debug>:QT_QML_DEBUG>)
endif()

# --- Deploy Qt runtime DLLs next to the exe (Windows only) ---
if(WIN32)
    get_target_property(QT_WINDEPLOYQT_LOCATION Qt6::windeployqt IMPORTED_LOCATION)

    if(QT_WINDEPLOYQT_LOCATION)
        add_custom_command(TARGET qttune POST_BUILD
            # Outside Debug, no qmltooling plugins: the debug server's
            # half, which nothing but a QT_QML_DEBUG build loads
            COMMAND ${QT_WINDEPLOYQT_LOCATION}
                    --no-compiler-runtime
                    "$<$<NOT:$<CONFIG:Debug>>:--skip-plugin-types;qmltooling>"
                    --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml
                    $<TARGET_FILE:qttune>
            COMMENT "Deploying Qt runtime DLLs"
            VERBATIM
            COMMAND_EXPAND_LISTS   # the Debug case expands to no argument
        )
    else()
        message(WARNING "windeployqt not found - manual deployment required")
    endif()
endif()

# --- Deploy the J2534 surrogates next to the exe (Windows only) ---
# Every vendor DLL loads in a surrogate of its own architecture
# (ADR-0025): x64 ones in this tree's ecuio-j2534-surrogate-x64.exe,
# copied first, and x86 ones in the build-win32 tree's
# ecuio-j2534-surrogate.exe. The client picks each by name, so the two
# can't be mixed up.
if(WIN32 AND TARGET ecuio-j2534-surrogate)
    add_dependencies(qttune ecuio-j2534-surrogate)
    add_custom_command(TARGET qttune POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ecuio-j2534-surrogate>
                $<TARGET_FILE_DIR:qttune>
        COMMENT "Deploying x64 J2534 surrogate next to qttune.exe"
        VERBATIM
    )
endif()

# The x86 one:
# Resolution order:
#   1. QTTUNE_SURROGATE_SOURCE cache variable (explicit override)
#   2. Auto-detect: conventional sibling build-win32 tree
#      (development.md two-tree layout), any config, Release preferred
#   3. Neither -> STATUS message, no copy; runtime ECUIO_SURROGATE_EXE
#      env override still works (checked first by the client).
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
