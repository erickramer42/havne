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
    qttunebridge.cpp
    qttunebridge.h
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
        qml/DashboardPage.qml
        qml/LogPage.qml
        qml/SettingsPage.qml
        qml/NavButton.qml
        qml/Theme.qml
        qml/LiveViewPage.qml
)

target_link_libraries(qttune PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    qttune-core
)

set_target_properties(qttune PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

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
# Opt-in via cache variable (configure with):
#   -DQTTUNE_SURROGATE_SOURCE=<path to build-win32 .../qttune-j2534-surrogate.exe>
# Unset = no copy; the app behaves exactly as before (QTTUNE_SURROGATE_EXE
# env override, checked first, still wins at runtime).
set(QTTUNE_SURROGATE_SOURCE "" CACHE PATH
    "Path to the Win32-tree qttune-j2534-surrogate.exe to deploy next to the app")

if(WIN32 AND NOT QTTUNE_SURROGATE_SOURCE STREQUAL "")
    if(NOT EXISTS "${QTTUNE_SURROGATE_SOURCE}")
        message(FATAL_ERROR
            "QTTUNE_SURROGATE_SOURCE points at a nonexistent file:\n"
            "  ${QTTUNE_SURROGATE_SOURCE}\n"
            "Build build-win32 first (development.md), or unset the variable.")
    endif()
    add_custom_command(TARGET qttune POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QTTUNE_SURROGATE_SOURCE}"
                $<TARGET_FILE_DIR:qttune>
        COMMENT "Deploying Win32 J2534 surrogate next to qttune.exe"
        VERBATIM
    )
elseif(WIN32)
    message(STATUS
        "QTTUNE_SURROGATE_SOURCE not set: the Win32 J2534 surrogate will "
        "not be deployed next to the app. Set it (build-win32 surrogate "
        "exe path) to deploy it, or export QTTUNE_SURROGATE_EXE at runtime.")
endif()

qt_finalize_executable(qttune)
