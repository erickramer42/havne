# File/layer: third_party/wamr.cmake | WAMR's classic interpreter as objects for core
# Testing: WamrBuildTest (tests/test_wamr_build.cpp); every tree compiles it
#
# WAMR (the WebAssembly Micro Runtime, third_party/wamr, pinned by
# scripts/vendor_wamr.sh) runs a conversation plugin's module (ADR-0066
# section 2.1). The sources are listed here, not globbed and not through
# WAMR's own build scripts, so what core compiles is this file: the
# classic interpreter with instruction metering, and nothing that reaches
# a file, a socket, a clock or a second thread.
#
# app_wamr_objects(<target> [STATIC_RUNTIME]) makes an OBJECT library
# whose objects join core's archive. STATIC_RUNTIME is MSVC's /MT, for the
# shared core (core/CMakeLists.txt).

set(_wamr ${CMAKE_CURRENT_LIST_DIR}/wamr/core)

# Havne's own file among WAMR's: built with the project's warnings and
# checked by clang-tidy and CodeQL, which skip the vendored sources
set(_wamr_port ${CMAKE_CURRENT_LIST_DIR}/wamr_port.c)

set(_wamr_sources
    ${_wamr}/iwasm/common/wasm_runtime_common.c
    ${_wamr}/iwasm/common/wasm_native.c
    ${_wamr}/iwasm/common/wasm_exec_env.c
    ${_wamr}/iwasm/common/wasm_memory.c
    ${_wamr}/iwasm/common/wasm_loader_common.c
    ${_wamr}/iwasm/common/wasm_blocking_op.c
    ${_wamr}/iwasm/common/wasm_shared_memory.c
    ${_wamr}/iwasm/common/arch/invokeNative_general.c
    ${_wamr}/iwasm/interpreter/wasm_loader.c
    ${_wamr}/iwasm/interpreter/wasm_runtime.c
    ${_wamr}/iwasm/interpreter/wasm_interp_classic.c
    ${_wamr}/shared/utils/bh_assert.c
    ${_wamr}/shared/utils/bh_bitmap.c
    ${_wamr}/shared/utils/bh_common.c
    ${_wamr}/shared/utils/bh_hashmap.c
    ${_wamr}/shared/utils/bh_leb128.c
    ${_wamr}/shared/utils/bh_list.c
    ${_wamr}/shared/utils/bh_log.c
    ${_wamr}/shared/utils/bh_queue.c
    ${_wamr}/shared/utils/bh_vector.c
    ${_wamr}/shared/utils/runtime_timer.c
    ${_wamr}/shared/mem-alloc/mem_alloc.c
    ${_wamr}/shared/mem-alloc/ems/ems_alloc.c
    ${_wamr}/shared/mem-alloc/ems/ems_gc.c
    ${_wamr}/shared/mem-alloc/ems/ems_hmu.c
    ${_wamr}/shared/mem-alloc/ems/ems_kfc.c)

if(WIN32)
    set(_wamr_platform windows)
    list(APPEND _wamr_sources
        ${_wamr}/shared/platform/windows/platform_init.c
        ${_wamr}/shared/platform/windows/win_atomic.cpp
        ${_wamr}/shared/platform/windows/win_malloc.c
        ${_wamr}/shared/platform/windows/win_memmap.c
        ${_wamr}/shared/platform/windows/win_thread.c
        ${_wamr}/shared/platform/windows/win_time.c
        ${_wamr}/shared/platform/windows/win_util.c
        ${_wamr}/shared/platform/common/memory/mremap.c)
else()
    if(ANDROID)
        set(_wamr_platform android)
    elseif(APPLE)
        set(_wamr_platform darwin)   # iOS too: WAMR has no port of its own for it
    else()
        set(_wamr_platform linux)
    endif()
    list(APPEND _wamr_sources
        ${_wamr}/shared/platform/${_wamr_platform}/platform_init.c
        ${_wamr}/shared/platform/common/posix/posix_blocking_op.c
        ${_wamr}/shared/platform/common/posix/posix_malloc.c
        ${_wamr}/shared/platform/common/posix/posix_memmap.c
        ${_wamr}/shared/platform/common/posix/posix_sleep.c
        ${_wamr}/shared/platform/common/posix/posix_thread.c
        ${_wamr}/shared/platform/common/posix/posix_time.c
        ${_wamr}/shared/platform/common/memory/mremap.c)
endif()

# The switches, the same on every platform. WAMR's config.h gives a
# feature not named here its default.
set(_wamr_definitions
    WASM_ENABLE_INTERP=1
    # The fast interpreter's switch dispatch counts no instructions at this tag
    WASM_ENABLE_FAST_INTERP=0
    WASM_ENABLE_INSTRUCTION_METERING=1
    # One dispatch on every compiler: the switch, which is all MSVC builds
    WASM_ENABLE_LABELS_AS_VALUES=0
    WASM_ENABLE_AOT=0
    WASM_ENABLE_JIT=0
    WASM_ENABLE_FAST_JIT=0
    # No libc from the host: each would be a native a module calls unmetered
    WASM_ENABLE_LIBC_BUILTIN=0
    WASM_ENABLE_LIBC_WASI=0
    # Bulk memory, reference types, tail calls, exception handling and GC
    # stay off while metering is on: their instructions do work the budget
    # does not count (wamr_port.c refuses the build otherwise)
    WASM_ENABLE_BULK_MEMORY=0
    WASM_ENABLE_REF_TYPES=0
    WASM_ENABLE_SIMD=0
    WASM_ENABLE_SHARED_MEMORY=0
    WASM_ENABLE_THREAD_MGR=0
    WASM_ENABLE_MULTI_MODULE=0
    WASM_ENABLE_TAIL_CALL=0
    WASM_ENABLE_GC=0
    WASM_ENABLE_EXCE_HANDLING=0
    WASM_ENABLE_MEMORY64=0
    WASM_ENABLE_MULTI_MEMORY=0
    WASM_ENABLE_SHARED_HEAP=0
    WASM_ENABLE_DEBUG_INTERP=0
    WASM_ENABLE_MINI_LOADER=0
    WASM_ENABLE_LOAD_CUSTOM_SECTION=0
    # On, as upstream builds it: off leaves a struct with no member, which
    # MSVC's C refuses
    WASM_ENABLE_MODULE_INST_CONTEXT=1
    WASM_ENABLE_QUICK_AOT_ENTRY=0
    WASM_ENABLE_AOT_INTRINSICS=0
    WASM_ENABLE_SHRUNK_MEMORY=0
    # A module's memory comes from the embedder's allocator too, not from
    # pages mapped past it, so the embedder's caps count every byte. The
    # allocator's functions then take what the memory is for, and a
    # module's memory comes as the allocator gives it: the embedder zeroes it.
    WASM_MEM_ALLOC_WITH_USAGE=1
    # Nothing a module does prints
    WASM_ENABLE_LOG=0
    # Software bound checks: the hardware ones install signal or exception
    # handlers in the host process and map guard regions
    WASM_DISABLE_HW_BOUND_CHECK=1
    WASM_DISABLE_STACK_HW_BOUND_CHECK=1
    WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS=0
    WASM_HAVE_MREMAP=0
    BH_MALLOC=wasm_runtime_malloc
    BH_FREE=wasm_runtime_free
    BH_DEBUG=0)
if(WIN32)
    list(APPEND _wamr_definitions BH_PLATFORM_WINDOWS HAVE_STRUCT_TIMESPEC)
elseif(ANDROID)
    list(APPEND _wamr_definitions BH_PLATFORM_ANDROID)
elseif(APPLE)
    list(APPEND _wamr_definitions BH_PLATFORM_DARWIN)
else()
    list(APPEND _wamr_definitions BH_PLATFORM_LINUX)
endif()
if(MSVC)
    # config.h finds the target from GCC's and Clang's macros only
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND _wamr_definitions BUILD_TARGET_X86_64)
    else()
        list(APPEND _wamr_definitions BUILD_TARGET_X86_32)
    endif()
endif()

# For a source that embeds WAMR: where wasm_export.h is, and the
# definition it is compiled with. WAMR's header marks its functions as a
# DLL's on MSVC; here they are linked statically, so the mark is empty.
set(WAMR_INCLUDE_DIR ${_wamr}/iwasm/include)
set(WAMR_EMBED_DEFINITIONS "WASM_RUNTIME_API_EXTERN=")

function(app_wamr_objects target)
    add_library(${target} OBJECT ${_wamr_sources} ${_wamr_port})
    target_include_directories(${target} PRIVATE
        ${_wamr}/iwasm/include
        ${_wamr}/iwasm/common
        ${_wamr}/iwasm/interpreter
        ${_wamr}/shared/utils
        ${_wamr}/shared/mem-alloc
        ${_wamr}/shared/platform/include
        ${_wamr}/shared/platform/${_wamr_platform})
    target_compile_definitions(${target} PRIVATE ${_wamr_definitions})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        POSITION_INDEPENDENT_CODE ON)
    # Upstream's code is built without warnings, source by source: the
    # warnings-as-errors builds and verify's MSVC count stay first-party's,
    # and wamr_port.c gets the project's. /U_MSC_BUILD:
    # WAMR's headers read that macro only to mark its API dllexport, which
    # would export it from every program and library that links core.
    # -fno-sanitize=alignment does nothing without a sanitizer. With one,
    # it leaves UBSan's alignment check, and no other, out for WAMR's
    # objects: the classic interpreter keeps a frame's label stack, which
    # holds pointers, at 4-byte steps. WAMR's own UBSan build sets the same
    # (build-scripts/config_common.cmake); upstream's pull request 5062
    # aligns that stack. The flag goes when a WAMR release carries the fix.
    if(MSVC)
        target_compile_options(${target} PRIVATE /U_MSC_BUILD)
        set_source_files_properties(${_wamr_sources} PROPERTIES COMPILE_OPTIONS /W0)
        set_source_files_properties(${_wamr_port} PROPERTIES COMPILE_OPTIONS /W4)
    else()
        target_compile_options(${target} PRIVATE -fno-sanitize=alignment)
        set(_wamr_port_warnings -Wall -Wextra)
        if(HAVNE_WARNINGS_AS_ERRORS)
            list(APPEND _wamr_port_warnings -Werror)
        endif()
        set_source_files_properties(${_wamr_sources} PROPERTIES COMPILE_OPTIONS -w)
        set_source_files_properties(${_wamr_port} PROPERTIES
            COMPILE_OPTIONS "${_wamr_port_warnings}")
    endif()
    # WAMR finds how much native stack is left from the address of a local.
    # GCC's AddressSanitizer keeps such a local in its fake stack on the
    # heap (its stack-use-after-return detection), so every call into a
    # module would end as "native stack overflow". The parameter keeps
    # WAMR's locals on the real stack; it does nothing without ASan, and
    # Clang's ASan leaves that local where it is.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            --param=asan-use-after-return=0)
    endif()
    if("STATIC_RUNTIME" IN_LIST ARGN)
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
