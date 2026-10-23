# File/layer: fuzz/CMakeLists.txt | libFuzzer harnesses
# ECUIO_BUILD_FUZZERS=ON instruments ecuio-core and the harnesses
# itself: coverage and ASan on both (UBSan too under Clang), and
# libFuzzer's main() linked into the harnesses only, so no other
# executable in the tree can pick it up. Two toolchains:
#   - Clang (clang/clang++, the CI 'fuzz' job on Linux);
#   - MSVC, whose libFuzzer (/fsanitize=fuzzer, experimental since
#     VS 2022 17.0) needs nothing beyond the build tools, so a local run
#     on Windows works from the Visual Studio generator.
# AppleClang ships no libFuzzer, and clang-cl links through link.exe,
# which takes no -fsanitize, so both are refused here.
# The harnesses are deliberately NOT ctest tests: a bounded-time fuzzer
# in the default pass would make a green suite stop meaning green.

# Instrumented core can only link into a harness: anything else would
# miss libFuzzer's coverage callbacks. So the fuzzers get a tree of
# their own.
if(BUILD_TESTING OR NOT QTTUNE_BUILD_CORE_ONLY)
    message(FATAL_ERROR
        "ECUIO_BUILD_FUZZERS=ON instruments ecuio-core, which the tests "
        "and the app can't link: configure a separate tree with "
        "-DQTTUNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=OFF.")
endif()

set(_ecuio_fuzzers fuzz_decode_signal fuzz_validate_set)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(_ecuio_fuzz_instrument
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    target_compile_options(ecuio-core PRIVATE ${_ecuio_fuzz_instrument})
    foreach(_t ${_ecuio_fuzzers})
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE ecuio-core)
        target_compile_options(${_t} PRIVATE ${_ecuio_fuzz_instrument})
        target_link_options(${_t} PRIVATE -fsanitize=fuzzer,address,undefined)
        qttune_enable_warnings(${_t})
    endforeach()
elseif(MSVC)
    # The /fsanitize-coverage options are what /fsanitize=fuzzer implies,
    # without the libFuzzer link: core gets them, and only a harness
    # compiled with /fsanitize=fuzzer pulls in libFuzzer's main().
    target_compile_options(ecuio-core PRIVATE
        /fsanitize=address
        /fsanitize-coverage=inline-8bit-counters
        /fsanitize-coverage=edge
        /fsanitize-coverage=trace-cmp
        /fsanitize-coverage=trace-div)
    get_filename_component(_ecuio_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_t ${_ecuio_fuzzers})
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE ecuio-core)
        target_compile_options(${_t} PRIVATE /fsanitize=address /fsanitize=fuzzer)
        target_link_options(${_t} PRIVATE /INCREMENTAL:NO)   # ASan refuses it
        qttune_enable_warnings(${_t})
        # The ASan runtime is a DLL beside cl.exe; copied next to the
        # harness so it runs from any shell.
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ecuio_msvc_bin}/$<IF:$<CONFIG:Debug>,clang_rt.asan_dbg_dynamic-x86_64.dll,clang_rt.asan_dynamic-x86_64.dll>"
                "$<TARGET_FILE_DIR:${_t}>")
    endforeach()
else()
    message(FATAL_ERROR
        "ECUIO_BUILD_FUZZERS=ON needs Clang (clang/clang++) or MSVC; found "
        "${CMAKE_CXX_COMPILER_ID}. AppleClang ships no libFuzzer, and "
        "clang-cl links through link.exe. See fuzz/README.md.")
endif()
