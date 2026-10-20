# core/CMakeLists.txt
add_library(qttune-core STATIC
    src/core.cpp
    src/profile_registry.cpp
    src/mock_transport.cpp
    src/j2534_loader.cpp
    src/j2534_transport.cpp
    src/signals.cpp  
    src/signal_sets.cpp
    src/obd2_signal_sets.cpp
    src/j2534_surrogate_client.cpp
)

target_include_directories(qttune-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated   # generated version.h
)

target_compile_options(qttune-core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# Critical invariant: core never links Qt (ADR-0001). The check lives at
# the end of the root CMakeLists.txt, after every subdirectory has
# configured; the CI core-and-tests job also builds core with no Qt at all.
