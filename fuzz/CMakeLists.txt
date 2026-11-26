# File/layer: fuzz/CMakeLists.txt | libFuzzer harnesses
# ECUIO_BUILD_FUZZERS=ON instruments ecuio-core, the WAMR objects in it
# (fuzz_wasm_module fuzzes that loader) and the harnesses
# itself: coverage and ASan on all (UBSan too under Clang), and
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
if(BUILD_TESTING OR NOT HAVNE_BUILD_CORE_ONLY)
    message(FATAL_ERROR
        "ECUIO_BUILD_FUZZERS=ON instruments ecuio-core, which the tests "
        "and the app can't link: configure a separate tree with "
        "-DHAVNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=OFF.")
endif()

set(_ecuio_fuzzers fuzz_decode_signal fuzz_validate_set fuzz_uds_query
    fuzz_elm327_reply fuzz_replay_file fuzz_socketcan_wire fuzz_diagnostics
    fuzz_isotp fuzz_vin fuzz_dtc fuzz_sim fuzz_elm327_emulator
    fuzz_recording fuzz_candump fuzz_request_bus fuzz_json fuzz_signal_file
    fuzz_plugin_manifest fuzz_obdb_import fuzz_csv_import fuzz_export
    fuzz_computed fuzz_condition_file fuzz_plugin_wrap fuzz_wasm_module
    fuzz_security_store fuzz_sur_reply fuzz_signal_merge fuzz_conversation
    fuzz_guest fuzz_consent_store fuzz_dump_file fuzz_operation)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(_ecuio_fuzz_instrument
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    target_compile_options(ecuio-core PRIVATE ${_ecuio_fuzz_instrument})
    # UBSan's alignment check is left out for WAMR's objects, as WAMR's
    # own config_common.cmake sets it, until a release carries upstream's
    # pull request 5062 (third_party/wamr.cmake has the why). Named again
    # here after -fsanitize=undefined, which would put the check back;
    # SHELL: keeps CMake from dropping the repeat.
    target_compile_options(ecuio-wamr PRIVATE ${_ecuio_fuzz_instrument}
        "SHELL:-fno-sanitize=alignment")
    foreach(_t ${_ecuio_fuzzers})
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE ecuio-core)
        # core/src: fuzz_uds_query drives the builder and the query
        # split, fuzz_elm327_reply the ELM327 wire, fuzz_socketcan_wire
        # SocketCAN's, fuzz_diagnostics the request engine, fuzz_isotp
        # ISO-TP, fuzz_sim the simulated ECU, fuzz_recording a
        # recording's rows, fuzz_candump a candump log's lines,
        # fuzz_json the JSON reader, fuzz_csv_import a CSV and its
        # column map, fuzz_computed a computed program's machine,
        # fuzz_validate_set and fuzz_signal_file the frame decoder,
        # fuzz_wasm_module the sandbox, fuzz_security_store the
        # security store's reader, fuzz_consent_store the consent
        # store's, fuzz_dump_file a dump file's reader,
        # fuzz_operation a dump's run,
        # fuzz_sur_reply the surrogate client's
        # reading of the child's replies, fuzz_conversation one module's
        # conversation, fuzz_guest a module's bytes bound to a session,
        # all internal
        # (the tests' precedent)
        target_include_directories(${_t} PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
        target_compile_options(${_t} PRIVATE ${_ecuio_fuzz_instrument})
        target_link_options(${_t} PRIVATE -fsanitize=fuzzer,address,undefined)
        app_enable_warnings(${_t})
    endforeach()
elseif(MSVC)
    # The /fsanitize-coverage options are what /fsanitize=fuzzer implies,
    # without the libFuzzer link: core gets them, and only a harness
    # compiled with /fsanitize=fuzzer pulls in libFuzzer's main().
    foreach(_t ecuio-core ecuio-wamr)
        target_compile_options(${_t} PRIVATE
            /fsanitize=address
            /fsanitize-coverage=inline-8bit-counters
            /fsanitize-coverage=edge
            /fsanitize-coverage=trace-cmp
            /fsanitize-coverage=trace-div)
    endforeach()
    get_filename_component(_ecuio_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_t ${_ecuio_fuzzers})
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE ecuio-core)
        target_include_directories(${_t} PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
        target_compile_options(${_t} PRIVATE /fsanitize=address /fsanitize=fuzzer)
        target_link_options(${_t} PRIVATE /INCREMENTAL:NO)   # ASan refuses it
        # fopen/getenv on purpose (fuzz_replay_file's and
        # fuzz_recording's scratch files), as
        # in the tests
        target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        app_enable_warnings(${_t})
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

# fuzz_request_bus answers as the fakes' ECU (tests/fake_ecu.h, header-only)
if(TARGET fuzz_request_bus)
    target_include_directories(fuzz_request_bus PRIVATE ${CMAKE_SOURCE_DIR}/tests)
endif()

# fuzz_elm327_emulator drives havne-cli's emulator text (cli/, header-only,
# public headers only)
if(TARGET fuzz_elm327_emulator)
    target_include_directories(fuzz_elm327_emulator PRIVATE ${CMAKE_SOURCE_DIR}/cli)
endif()
