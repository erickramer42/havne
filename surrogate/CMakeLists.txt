# File/layer: surrogate/CMakeLists.txt | Win32, Qt-free, links core headers only
# Testing: HARNESS: surrogate exe is spawned by ecuio_j2534_sur_open and by
#          test_j2534_surrogate; verified end-to-end over pipe+ring+job objects,
#          in the x64 tree and cross-arch (the x86 build, static CRT, through
#          scripts/verify.sh). The release workflow checks its imports name
#          no CRT DLL.
#
# ARCHITECTURE STORY (the "both targets" design, ADR-0025):
# - Whatever architecture THIS tree is configured as, the surrogate and the
#   fake DLL build in that same architecture. x64 tree ->
#   ecuio-j2534-surrogate-x64.exe, which hosts every x64 vendor DLL and
#   ships next to the app; Win32 tree (-A Win32) ->
#   ecuio-j2534-surrogate.exe, for x86 DLLs, + the x86 fake, the
#   cross-arch case driven via ECUIO_SURROGATE_EXE.
# - No cross-arch compilation happens inside a single configure pass; the
#   second architecture comes from a second build directory.
# - CI (x64 job) builds the all-x64 surrogate test; the x86 verification
#   round runs the same test suite from the Win32 configure, or on the dev
#   machine with ECUIO_SURROGATE_EXE pointing at the Win32 artifacts.

if(NOT WIN32)
    return()   # surrogate is a Win32-only executable; no-op target elsewhere
endif()

add_executable(ecuio-j2534-surrogate surrogate_main.cpp)

target_include_directories(ecuio-j2534-surrogate PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_BINARY_DIR}/generated   # sur_fingerprint.h
)

# Entry point is wmain() (the DLL path arrives as UTF-16); subsystem
# defaults to console, and the client spawns it with CREATE_NO_WINDOW.
# The client picks the exe by the DLL's architecture, by name.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_sur_name "ecuio-j2534-surrogate-x64")
    set(_sur_bits "64-bit")
else()
    set(_sur_name "ecuio-j2534-surrogate")
    set(_sur_bits "32-bit")
endif()
set_target_properties(ecuio-j2534-surrogate PROPERTIES
    OUTPUT_NAME "${_sur_name}"
)
app_enable_warnings(ecuio-j2534-surrogate)
app_add_version_resource(ecuio-j2534-surrogate "${_sur_name}"
    "Havne helper for ${_sur_bits} J2534 drivers")

# No ecuio-core link needed: the surrogate resolves the vendor DLL's
# exports through GetProcAddress and speaks the POD protocol types from
# the shared headers. Keeping it dependency-light is deliberate: this
# binary must be deployable standalone next to the app exe.
#
# The C runtime is linked in statically (/MT) for the same reason. The
# x86 surrogate ships in the x64 app's folder, where the app-local CRT
# DLLs are x64; an x86 vcruntime140.dll can't sit beside them. Nothing
# here links ecuio-core (/MD), so the two runtimes never meet (LNK2038).
set_property(TARGET ecuio-j2534-surrogate PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
