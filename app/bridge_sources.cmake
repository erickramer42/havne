# File/layer: app/bridge_sources.cmake | CMake, included by app/ and tests/
# Testing: HARNESS: build-time; app_ui, test_bridge and bench_bridge each
#     compile this list, so a file missing from it fails all three links.

# SessionBridge's own sources: the files that define the class and its
# models. Absolute paths, since tests/ compiles them from its own directory.
# A new bridge source joins this list alone.
set(APP_BRIDGE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/bridge_awake.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_conditions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_connect.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_diagnostics.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_diagnostics_entries.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_diagnostics_text.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_files.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_profiles.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_recorder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_recordings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_sessioninfo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_store.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_support.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridge_together.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bridgeformats.h
    ${CMAKE_CURRENT_LIST_DIR}/bridgeseams.h
    ${CMAKE_CURRENT_LIST_DIR}/bridgestate.h
    ${CMAKE_CURRENT_LIST_DIR}/bridgetext.h
    ${CMAKE_CURRENT_LIST_DIR}/choicerows.h
    ${CMAKE_CURRENT_LIST_DIR}/consentflow.cpp
    ${CMAKE_CURRENT_LIST_DIR}/consentflow.h
    ${CMAKE_CURRENT_LIST_DIR}/framelistmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/framelistmodel.h
    ${CMAKE_CURRENT_LIST_DIR}/hostclock.h
    ${CMAKE_CURRENT_LIST_DIR}/sessionbridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sessionbridge.h
    ${CMAKE_CURRENT_LIST_DIR}/signallistmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/signallistmodel.h
    ${CMAKE_CURRENT_LIST_DIR}/signalplot.cpp
)
