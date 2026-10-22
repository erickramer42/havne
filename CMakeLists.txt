# root/CMakeLists.txt
cmake_minimum_required(VERSION 3.21)

project(QtTune
    VERSION 0.2.0
    DESCRIPTION "Cross-platform ECU reading, logging, and tuning"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# QtTune version is the single source of truth; propagate to core + app
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include/qttune/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/qttune/version.h
    @ONLY
)

option(QTTUNE_BUILD_CORE_ONLY "Build core library and tests; skip Qt app" OFF)

# Compiler warnings for every first-party target: core, the surrogate,
# the app, the tests and the fake DLLs. They don't fail the build (no /WX
# or -Werror): CI compiles with GCC, MSVC and AppleClang, and one
# compiler's new warning shouldn't break the others' builds.
function(qttune_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_subdirectory(core)
add_subdirectory(surrogate)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NOT QTTUNE_BUILD_CORE_ONLY)
    add_subdirectory(app)
endif()

# Core never links Qt (ADR-0001). This runs after every subdirectory, so
# it sees links added from anywhere. It reads core's link lists, because
# Qt's imported targets are scoped to the directory that found them and a
# TARGET Qt6::Core test can't see them from here.
foreach(_qttune_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_qttune_libs qttune-core ${_qttune_prop})
    if(_qttune_libs AND _qttune_libs MATCHES "Qt[0-9]*::")
        message(FATAL_ERROR
            "qttune-core must not link Qt (${_qttune_prop}: ${_qttune_libs})")
    endif()
endforeach()
