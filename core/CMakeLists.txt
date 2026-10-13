# core/CMakeLists.txt
add_library(qttune-core STATIC
    src/core.cpp
    src/mock_transport.cpp
    src/j2534_loader.cpp
    src/j2534_transport.cpp
    src/signals.cpp  
    src/signal_sets.cpp
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

# Critical invariant for CI: core must compile with NO Qt present.
if(TARGET Qt6::Core)
    message(FATAL_ERROR "qttune-core must not link Qt")
endif()
