# core/CMakeLists.txt
add_library(ecuio-core STATIC
    src/core.cpp
    src/frame_decoder.cpp
    src/dtc_titles.cpp
    src/export.cpp
    src/host_clock.cpp
    src/import.cpp
    src/json.cpp
    src/obdb_import.cpp
    src/profile_registry.cpp
    src/recorder.cpp
    src/recording_check.cpp
    src/recording_reader.cpp
    src/scrub.cpp
    src/scrub_frames.cpp
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
    src/sim.cpp
    src/socketcan_interfaces.cpp
    src/socketcan_transport.cpp
    src/signals.cpp
    src/signal_file.cpp
    src/plugin.cpp
    src/signal_sets.cpp
    src/obd2_signal_sets.cpp
    src/uds_generic.cpp
    src/vin.cpp
    src/j2534_surrogate_client.cpp
)

# The public headers: the C API an outside program compiles against.
# One list for what's installed, what scripts/api_check.py compares
# with a release (it reads these lines) and what the shared core
# exports (scripts/ecuio_exports.py); the generated version.h joins the
# install. A new public header joins it; internal.h, signal_sets.h,
# j2534_surrogate.h, j2534_ptypes.h and surrogate_protocol.h never do.
set(ECUIO_PUBLIC_HEADERS
    include/ecuio/core.h
    include/ecuio/j2534.h
    include/ecuio/obdb.h
    include/ecuio/plugin.h
    include/ecuio/profile.h
    include/ecuio/recording.h
    include/ecuio/serial.h
    include/ecuio/signals.h
    include/ecuio/sim.h
    include/ecuio/socketcan.h
    include/ecuio/stream.h
    include/ecuio/vin.h
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

# USDT probes (ADR-0034 section 4, core/src/probes.h): systemtap's header-only
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

# The shared core (HAVNE_SHARED_CORE, the root CMakeLists.txt; ADR-0053's
# appendix "the shared core as built"): libecuio.so, libecuio.dylib or
# ecuio.dll, the archive linked whole, exporting the list
# scripts/ecuio_exports.py makes from ECUIO_PUBLIC_HEADERS at build time
# (generated/ecuio/: .def for link.exe, a version script for GNU ld, an
# exported symbols list for Apple's ld), so it lets out every declared
# function and nothing else; ExportsTest holds it to that. Whole, so it
# carries every object the archive has, as a program linking the archive
# gets the ones it calls. The soname follows CHANGELOG.md's promise:
# before 1.0 a minor version may break the ABI, so 0.x names its minor
# (libecuio.so.0.6, compatibility_version 0.6) and 1.0 on its major
# (libecuio.so.1). Windows names no version: the DLL is loaded by path.
if(HAVNE_SHARED_CORE)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "HAVNE_SHARED_CORE=ON needs Python 3 to make the shared core's "
            "export list from the public headers: install it, point "
            "Python3_EXECUTABLE at it, or configure with -DHAVNE_SHARED_CORE=OFF.")
    endif()
    set(_ecuio_gen ${CMAKE_BINARY_DIR}/generated/ecuio)
    set(_ecuio_lists ${_ecuio_gen}/ecuio.exports ${_ecuio_gen}/ecuio.def
        ${_ecuio_gen}/ecuio.map ${_ecuio_gen}/ecuio.exp)
    list(TRANSFORM ECUIO_PUBLIC_HEADERS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/
        OUTPUT_VARIABLE _ecuio_headers)
    add_custom_command(OUTPUT ${_ecuio_lists}
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/ecuio_exports.py
            ${_ecuio_gen} ${_ecuio_headers}
        DEPENDS ${PROJECT_SOURCE_DIR}/scripts/ecuio_exports.py
            ${PROJECT_SOURCE_DIR}/scripts/api_check.py ${_ecuio_headers}
        COMMENT "The shared core's export list, from the public headers"
        VERBATIM)
    # A cache variable, so tests/ finds the headers
    set(ECUIO_PUBLIC_HEADER_PATHS "${_ecuio_headers}" CACHE INTERNAL "")

    # src/shared_core.cpp is empty: a library needs a source of its own
    add_library(ecuio SHARED src/shared_core.cpp ${_ecuio_gen}/ecuio.exports)
    set_source_files_properties(${_ecuio_gen}/ecuio.exports PROPERTIES HEADER_FILE_ONLY ON)
    target_link_libraries(ecuio PRIVATE ecuio-core)
    target_include_directories(ecuio INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)
    app_enable_warnings(ecuio)
    if(Havne_VERSION_MAJOR EQUAL 0)
        set(_ecuio_soversion ${Havne_VERSION_MAJOR}.${Havne_VERSION_MINOR})
    else()
        set(_ecuio_soversion ${Havne_VERSION_MAJOR})
    endif()
    # Mach-O: compatibility_version is SOVERSION's, current_version VERSION's
    set_target_properties(ecuio PROPERTIES
        OUTPUT_NAME ecuio
        VERSION ${Havne_VERSION}
        SOVERSION ${_ecuio_soversion})
    if(MSVC)
        target_sources(ecuio PRIVATE ${_ecuio_gen}/ecuio.def)
        target_link_options(ecuio PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:ecuio-core>")
    elseif(APPLE)
        set_source_files_properties(${_ecuio_gen}/ecuio.exp PROPERTIES HEADER_FILE_ONLY ON)
        target_sources(ecuio PRIVATE ${_ecuio_gen}/ecuio.exp)
        target_link_options(ecuio PRIVATE
            "LINKER:-force_load,$<TARGET_FILE:ecuio-core>"
            "LINKER:-exported_symbols_list,${_ecuio_gen}/ecuio.exp")
        set_target_properties(ecuio PROPERTIES LINK_DEPENDS ${_ecuio_gen}/ecuio.exp)
    else()
        set_source_files_properties(${_ecuio_gen}/ecuio.map PROPERTIES HEADER_FILE_ONLY ON)
        target_sources(ecuio PRIVATE ${_ecuio_gen}/ecuio.map)
        target_link_options(ecuio PRIVATE
            "LINKER:--whole-archive,$<TARGET_FILE:ecuio-core>,--no-whole-archive"
            "LINKER:--version-script=${_ecuio_gen}/ecuio.map")
        set_target_properties(ecuio PROPERTIES LINK_DEPENDS ${_ecuio_gen}/ecuio.map)
    endif()
    # A shared object's code must be position independent, the archive's
    # objects included (Windows' DLLs relocate without it)
    if(NOT WIN32)
        set_target_properties(ecuio-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

# Critical invariant: core never links Qt (ADR-0001). The check lives at
# the end of the root CMakeLists.txt, after every subdirectory has
# configured; the CI core-and-tests job also builds core with no Qt at all.

# The core package (ADR-0035 section 2): the archive, the public headers, a
# CMake package (find_package(ecuio) gives ecuio::core) and a pkg-config
# file, laid out by `cmake --install` for packaging/linux/
# make-core-packages.sh. Every path is relative to the prefix, so the
# tarball works wherever it's unpacked. The internal headers stay out.
if(NOT ANDROID AND NOT IOS)
    include(CMakePackageConfigHelpers)
    set_target_properties(ecuio-core PROPERTIES EXPORT_NAME core)
    install(TARGETS ecuio-core EXPORT ecuio ARCHIVE DESTINATION lib)
    install(FILES ${ECUIO_PUBLIC_HEADERS}
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
    # The data core carries from others (ADR-0043, ADR-0044): the trouble
    # codes' titles under their MIT notice, vPIC's makes
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt
        DESTINATION share/doc/havne-core RENAME THIRD_PARTY_NOTICES)
endif()
