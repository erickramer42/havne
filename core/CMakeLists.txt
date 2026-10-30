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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>   # generated version.h
        $<INSTALL_INTERFACE:include>
)

app_enable_warnings(ecuio-core)

# The workers are std::threads; a program linking the archive needs the
# thread library too (glibc before 2.34 kept it apart from libc)
find_package(Threads REQUIRED)
target_link_libraries(ecuio-core PRIVATE Threads::Threads)

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

# The core package (ADR-0035 §2): the archive, the public headers, a
# CMake package (find_package(ecuio) gives ecuio::core) and a pkg-config
# file, laid out by `cmake --install` for packaging/linux/
# make-core-packages.sh. Every path is relative to the prefix, so the
# tarball works wherever it's unpacked. The internal headers stay out.
if(NOT ANDROID AND NOT IOS)
    include(CMakePackageConfigHelpers)
    set_target_properties(ecuio-core PROPERTIES EXPORT_NAME core)
    install(TARGETS ecuio-core EXPORT ecuio ARCHIVE DESTINATION lib)
    install(FILES
            include/ecuio/core.h
            include/ecuio/j2534.h
            include/ecuio/profile.h
            include/ecuio/serial.h
            include/ecuio/signals.h
            include/ecuio/socketcan.h
            include/ecuio/stream.h
            ${CMAKE_BINARY_DIR}/generated/ecuio/version.h
        DESTINATION include/ecuio)
    install(EXPORT ecuio NAMESPACE ecuio:: FILE ecuioTargets.cmake
        DESTINATION lib/cmake/ecuio)
    configure_package_config_file(ecuioConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ecuioConfig.cmake
        INSTALL_DESTINATION lib/cmake/ecuio)
    # Before 1.0 a minor version may break the ABI (CHANGELOG.md)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ecuioConfigVersion.cmake
        VERSION ${Havne_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/ecuioConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/ecuioConfigVersion.cmake
        DESTINATION lib/cmake/ecuio)
    configure_file(ecuio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ecuio.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ecuio.pc DESTINATION lib/pkgconfig)
    install(FILES ${PROJECT_SOURCE_DIR}/LICENSE
        DESTINATION share/doc/havne-core RENAME copyright)
endif()
