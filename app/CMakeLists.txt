# app/CMakeLists.txt
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

qt_finalize_executable(qttune)
