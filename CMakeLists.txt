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

add_subdirectory(core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NOT QTTUNE_BUILD_CORE_ONLY)
    add_subdirectory(app)
endif()
