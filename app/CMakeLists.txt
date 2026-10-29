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

# The App QML module and the bridge it binds live in a static library
# of their own, so test_qml (tests/CMakeLists.txt) loads the same module
# the app does. Whatever links it also links app_uiplugin and imports
# the plugin with Q_IMPORT_QML_PLUGIN(AppPlugin), or the module's
# types are never registered.
qt_add_library(app_ui STATIC
    blepicker.cpp
    blepicker.h
    btconnect.cpp
    btconnect.h
    bluetoothdevices.cpp
    bluetoothdevices.h
    bridgesingleton.h
    carryover.cpp
    carryover.h
    sessionbridge.cpp
    sessionbridge.h
    latencystats.h
    sessionrecorder.cpp
    sessionrecorder.h
)

set_source_files_properties(qml/Theme.qml qml/ChartLayout.qml qml/VehicleBook.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(app_ui
    URI App
    VERSION 1.0
    # Ends in the URI, as qmllint expects of a library's module
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/App
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
        qml/VehicleBook.qml
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

target_include_directories(app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(app_ui PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
    ecuio-core
)
app_enable_warnings(app_ui)
# The phone's Bluetooth (ADR-0028 §2): the links behind core's app-fed
# streams, on Android only, so no desktop build ships it unverified.
# Elsewhere the Bluetooth singleton lists nothing.
if(ANDROID)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Bluetooth)
    target_sources(app_ui PRIVATE btstream.cpp btstream.h)
    target_link_libraries(app_ui PUBLIC Qt6::Bluetooth)
    target_compile_definitions(app_ui PUBLIC APP_BLUETOOTH)
    # The foreground service held while an adapter session is up
    # (ADR-0029 §2; its Java half is in android/src)
    target_sources(app_ui PRIVATE androidsession.cpp androidsession.h)
endif()
# MSVC's C4702 (unreachable code) fires inside Qt's inline headers, as
# the generated QML registration code instantiates them; none of it is
# this project's code.
if(MSVC)
    target_compile_options(app_ui PRIVATE /wd4702)
endif()

qt_add_executable(havne
    MANUAL_FINALIZATION
    main.cpp
)

target_link_libraries(havne PRIVATE
    app_ui
    app_uiplugin
)
app_enable_warnings(havne)
app_add_version_resource(havne havne "Havne ECU reader and logger")

if(WIN32)
    set_target_properties(havne PROPERTIES WIN32_EXECUTABLE TRUE)
    # The title bar's color (main.cpp, DwmSetWindowAttribute)
    target_link_libraries(havne PRIVATE dwmapi)
endif()
if(APPLE)
    set_target_properties(havne PROPERTIES MACOSX_BUNDLE TRUE)
endif()
# The app icon (packaging/icons/make_icons.py writes both). Android: an
# adaptive launcher icon, vector drawables in the package source's res/;
# Qt's manifest names it through QT_ANDROID_APP_ICON (Qt 6.9+).
# The package id is permanent: another one is another app (ADR-0001's
# appendix of 2026-10-28). The version name is project()'s, the version
# code major * 10000 + minor * 100 + patch, so each release installs
# over the one before.
if(ANDROID)
    math(EXPR _app_version_code
        "${Havne_VERSION_MAJOR} * 10000 + ${Havne_VERSION_MINOR} * 100 + ${Havne_VERSION_PATCH}")
    set_target_properties(havne PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
        QT_ANDROID_PACKAGE_NAME "io.github.erickramer42.havne"
        QT_ANDROID_VERSION_NAME "${Havne_VERSION}"
        QT_ANDROID_VERSION_CODE "${_app_version_code}")
endif()
# iOS: an asset catalog holding the 1024 px AppIcon, compiled by Xcode
if(IOS)
    set(app_asset_catalog "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets")
    target_sources(havne PRIVATE "${app_asset_catalog}")
    set_source_files_properties("${app_asset_catalog}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(havne PROPERTIES
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
endif()
# Core's TCP socket (elm327://<address>) needs INTERNET: declared here,
# not left to the permissions Qt adds by default (ADR-0028 §2).
# CI's android-build checks the merged manifest for it.
# Bluetooth needs CONNECT (a paired adapter, the paired list) and SCAN
# (BLE), the scan declared never for location. Android 12 (API 31) is the
# minimum, the first with those two: older releases want location for a
# scan, a path no phone here could test (ADR-0028's appendix of
# 2026-10-28). CI's android-build checks the manifest and the minimum.
if(ANDROID)
    qt_add_android_permission(havne NAME android.permission.INTERNET)
    qt_add_android_permission(havne NAME android.permission.BLUETOOTH_CONNECT)
    qt_add_android_permission(havne NAME android.permission.BLUETOOTH_SCAN
        ATTRIBUTES usesPermissionFlags neverForLocation)
    # The session's foreground service (ADR-0029 §2), type connectedDevice:
    # its two permissions, CHANGE_NETWORK_STATE so a WiFi adapter's session
    # qualifies without Bluetooth granted, and its notification
    qt_add_android_permission(havne NAME android.permission.FOREGROUND_SERVICE)
    qt_add_android_permission(havne
        NAME android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE)
    qt_add_android_permission(havne NAME android.permission.CHANGE_NETWORK_STATE)
    qt_add_android_permission(havne NAME android.permission.POST_NOTIFICATIONS)
    # The service's partial wake lock: with the screen off the CPU sleeps
    # otherwise, service or not (docs/bench-sessions/2026-10-28-android-9.md)
    qt_add_android_permission(havne NAME android.permission.WAKE_LOCK)
    set_target_properties(havne PROPERTIES QT_ANDROID_MIN_SDK_VERSION 31)
endif()

# QML debugging in Debug builds only. A build with QT_QML_DEBUG opens a
# debug server when launched with -qmljsdebugger, one that can run
# script in the app with no authentication, so no release carries it
# (packaging/windows/stage.ps1 checks the exe's imports).
if(QT_FEATURE_qml_debug)
    target_compile_definitions(havne PRIVATE $<$<CONFIG:Debug>:QT_QML_DEBUG>)
endif()

# --- Deploy Qt runtime DLLs next to the exe (Windows only) ---
if(WIN32)
    get_target_property(QT_WINDEPLOYQT_LOCATION Qt6::windeployqt IMPORTED_LOCATION)

    if(QT_WINDEPLOYQT_LOCATION)
        add_custom_command(TARGET havne POST_BUILD
            # Outside Debug, no qmltooling plugins: the debug server's
            # half, which nothing but a QT_QML_DEBUG build loads
            COMMAND ${QT_WINDEPLOYQT_LOCATION}
                    --no-compiler-runtime
                    "$<$<NOT:$<CONFIG:Debug>>:--skip-plugin-types;qmltooling>"
                    --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml
                    $<TARGET_FILE:havne>
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
    add_dependencies(havne ecuio-j2534-surrogate)
    add_custom_command(TARGET havne POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ecuio-j2534-surrogate>
                $<TARGET_FILE_DIR:havne>
        COMMENT "Deploying x64 J2534 surrogate next to havne.exe"
        VERBATIM
    )
endif()

# The x86 one:
# Resolution order:
#   1. HAVNE_SURROGATE_SOURCE cache variable (explicit override)
#   2. Auto-detect: conventional sibling build-win32 tree
#      (development.md two-tree layout), any config, Release preferred
#   3. Neither -> STATUS message, no copy; runtime ECUIO_SURROGATE_EXE
#      env override still works (checked first by the client).
set(HAVNE_SURROGATE_SOURCE "" CACHE PATH "Win32-tree ecuio-j2534-surrogate.exe to deploy; empty = auto-detect")

if(WIN32 AND HAVNE_SURROGATE_SOURCE STREQUAL "")
    file(GLOB _sur_found "${CMAKE_SOURCE_DIR}/build-win32/surrogate/*/ecuio-j2534-surrogate.exe")
    if(EXISTS "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
        list(INSERT _sur_found 0 "${CMAKE_SOURCE_DIR}/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe")
    endif()
    list(LENGTH _sur_found _sur_found_n)
    if(_sur_found_n GREATER 0)
        list(GET _sur_found 0 HAVNE_SURROGATE_SOURCE)
    endif()
endif()

if(WIN32 AND NOT HAVNE_SURROGATE_SOURCE STREQUAL "")
    if(NOT EXISTS "${HAVNE_SURROGATE_SOURCE}")
        message(FATAL_ERROR "HAVNE_SURROGATE_SOURCE not found: ${HAVNE_SURROGATE_SOURCE}")
    endif()
    message(STATUS "Deploying Win32 J2534 surrogate: ${HAVNE_SURROGATE_SOURCE}")
    add_custom_command(TARGET havne POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HAVNE_SURROGATE_SOURCE}"
                $<TARGET_FILE_DIR:havne>
        COMMENT "Deploying Win32 J2534 surrogate next to havne.exe"
        VERBATIM
    )
elseif(WIN32)
    message(STATUS "No build-win32 surrogate found; build the Win32 tree per development.md and reconfigure, or set HAVNE_SURROGATE_SOURCE, or export ECUIO_SURROGATE_EXE at runtime")
endif()

qt_finalize_executable(havne)
