# core/CMakeLists.txt
add_library(ecuio-core STATIC
    src/core.cpp
    src/host_clock.cpp
    src/profile_registry.cpp
    src/mock_transport.cpp
    src/replay_transport.cpp
    src/j2534_loader.cpp
    src/j2534_transport.cpp
    src/elm327_transport.cpp
    src/elm327_port.cpp
    src/elm327_tcp.cpp
    src/elm327_stream.cpp
    src/elm327_log.cpp
    src/serial_ports.cpp
    src/socketcan_interfaces.cpp
    src/socketcan_transport.cpp
    src/signals.cpp
    src/signal_sets.cpp
    src/obd2_signal_sets.cpp
    src/uds_generic.cpp
    src/j2534_surrogate_client.cpp
)

target_include_directories(ecuio-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated   # generated version.h
)

app_enable_warnings(ecuio-core)

# bcrypt: BCryptGenRandom names the surrogate's pipe; ws2_32: a WiFi
# adapter's socket (elm327_tcp.cpp). A static library's private link
# still reaches whatever links core.
if(WIN32)
    target_link_libraries(ecuio-core PRIVATE bcrypt ws2_32)
endif()

# USDT probes (ADR-0034 §4, core/src/probes.h): systemtap's header-only
# <sys/sdt.h> (systemtap-sdt-dev), on Linux but not Android, whose NDK
# has none. Without it the probes compile to nothing. ECUIO_HAVE_SDT is
# a cache variable, so tests/ sees it too (ProbeNotesTest).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    include(CheckIncludeFileCXX)
    check_include_file_cxx(sys/sdt.h ECUIO_HAVE_SDT)
    if(ECUIO_HAVE_SDT)
        target_compile_definitions(ecuio-core PRIVATE ECUIO_HAVE_SDT=1)
    else()
        message(STATUS "No <sys/sdt.h> (systemtap-sdt-dev): core builds without its USDT probes")
    endif()
endif()

# Critical invariant: core never links Qt (ADR-0001). The check lives at
# the end of the root CMakeLists.txt, after every subdirectory has
# configured; the CI core-and-tests job also builds core with no Qt at all.
