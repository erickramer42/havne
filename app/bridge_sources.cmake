# File/layer: app/bridge_sources.cmake | CMake, included by app/ and tests/
# Testing: HARNESS: build-time; app_ui, test_bridge and bench_bridge each
#     compile this list, so a file missing from it fails all three links.

# SessionBridge's own sources: the files that define the class and its
# models. Absolute paths, since tests/ compiles them from its own directory.
# A new bridge source joins this list alone.
set(APP_BRIDGE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/sessionbridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sessionbridge.h
)
