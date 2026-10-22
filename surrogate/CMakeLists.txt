# File/layer: surrogate/CMakeLists.txt | Win32, Qt-free, links core headers only
# Testing: HARNESS — surrogate exe is spawned by qttune-j2534_sur_open and by
#          test_j2534_surrogate; verified end-to-end over pipe+ring+job objects.
#
# ARCHITECTURE STORY (the "both targets" design):
# - Whatever architecture THIS tree is configured as, the surrogate and the
#   fake DLL build in that same architecture. x64 tree -> x64 surrogate
#   (CI-runnable IPC test); Win32 tree (-A Win32) -> x86 surrogate + x86
#   fake, the true cross-arch case driven via QTTUNE_SURROGATE_EXE.
# - No cross-arch compilation happens inside a single configure pass; the
#   second architecture comes from a second build directory.
# - CI (x64 job) builds the all-x64 surrogate test; the x86 verification
#   round runs the same test suite from the Win32 configure, or on the dev
#   machine with QTTUNE_SURROGATE_EXE pointing at the Win32 artifacts.

if(NOT WIN32)
    return()   # surrogate is a Win32-only executable; no-op target elsewhere
endif()

add_executable(qttune-j2534-surrogate surrogate_main.cpp)

target_include_directories(qttune-j2534-surrogate PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

# Entry point is wmain() (the DLL path arrives as UTF-16); subsystem
# defaults to console, and the client spawns it with CREATE_NO_WINDOW.
set_target_properties(qttune-j2534-surrogate PROPERTIES
    OUTPUT_NAME "qttune-j2534-surrogate"
)
qttune_enable_warnings(qttune-j2534-surrogate)

# No qttune-core link needed: the surrogate resolves the vendor DLL's
# exports through GetProcAddress and speaks the POD protocol types from
# the shared headers. Keeping it dependency-light is deliberate — this
# binary must be deployable standalone next to the app exe.
