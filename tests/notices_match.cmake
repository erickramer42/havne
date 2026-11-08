# File/layer: tests/notices_match.cmake | test-only, cmake -P
# Testing: HARNESS (ctest NoticesTest.TheApkCarriesEachNoticeAsItsSource,
#          NoticesTest.EachNoticeCarriesCoresData)
#
#   cmake -DROOT=<source dir> -DMODE=apk|core -P notices_match.cmake
#
# The APK's notices are committed copies under app/android/assets/havne/
# (Qt's Android build takes app/android as its package source, so the
# copies are there for it without a configure step): apk checks each is
# its source byte for byte, and that the folder holds nothing else.
# core checks that each notice a download ships carries core's data
# notices (core/THIRD_PARTY_NOTICES.txt from its first section on) word
# for word, since every app links core.

# A script run with -P takes no policies from the project, so it names its
# version itself: under CMake 3.x, if(IN_LIST) needs CMP0057 NEW.
cmake_minimum_required(VERSION 3.21)

if(MODE STREQUAL "apk")
    set(_assets "${ROOT}/app/android/assets/havne")
    set(_pairs
        "LICENSE.txt=LICENSE"
        "THIRD_PARTY_NOTICES.txt=packaging/notices/THIRD_PARTY_NOTICES.txt"
        "licenses/LGPL-3.0.txt=packaging/windows/licenses/LGPL-3.0.txt"
        "licenses/GPL-3.0.txt=packaging/windows/licenses/GPL-3.0.txt"
        "licenses/Apache-2.0.txt=packaging/notices/licenses/Apache-2.0.txt"
        "licenses/Qt-third-party.txt=packaging/windows/licenses/Qt-third-party.txt")
    set(_known "")
    set(_wrong "")
    foreach(_pair IN LISTS _pairs)
        string(REPLACE "=" ";" _parts "${_pair}")
        list(GET _parts 0 _copy)
        list(GET _parts 1 _source)
        list(APPEND _known "${_copy}")
        if(NOT EXISTS "${_assets}/${_copy}")
            list(APPEND _wrong "${_copy} is missing (copy it from ${_source})")
            continue()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                                "${_assets}/${_copy}" "${ROOT}/${_source}"
                        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _rc EQUAL 0)
            list(APPEND _wrong "${_copy} differs from ${_source} (copy it again)")
        endif()
    endforeach()
    file(GLOB_RECURSE _present RELATIVE "${_assets}" "${_assets}/*")
    foreach(_file IN LISTS _present)
        if(NOT _file IN_LIST _known)
            list(APPEND _wrong "${_file} has no source here (add it to this list or remove it)")
        endif()
    endforeach()
    if(_wrong)
        string(JOIN "\n  " _list ${_wrong})
        message(FATAL_ERROR "app/android/assets/havne/:\n  ${_list}")
    endif()
    list(LENGTH _pairs _count)
    message("app/android/assets/havne/: ${_count} files, each its source's bytes")
elseif(MODE STREQUAL "core")
    file(READ "${ROOT}/core/THIRD_PARTY_NOTICES.txt" _core)
    string(REPLACE "\r\n" "\n" _core "${_core}")
    # Core's header names its own package; its sections are what every
    # notice repeats
    string(FIND "${_core}" "\n\n\n" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "core/THIRD_PARTY_NOTICES.txt has no section after its header")
    endif()
    math(EXPR _at "${_at} + 3")
    string(SUBSTRING "${_core}" ${_at} -1 _sections)
    set(_missing "")
    foreach(_notice packaging/notices/THIRD_PARTY_NOTICES.txt
                    packaging/windows/THIRD_PARTY_NOTICES.txt)
        file(READ "${ROOT}/${_notice}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        string(FIND "${_text}" "${_sections}" _found)
        if(_found LESS 0)
            list(APPEND _missing "${_notice}")
        endif()
    endforeach()
    if(_missing)
        string(JOIN "\n  " _list ${_missing})
        message(FATAL_ERROR "without core/THIRD_PARTY_NOTICES.txt's sections word for word:\n  ${_list}")
    endif()
    message("each download's notice carries core's data notices")
else()
    message(FATAL_ERROR "MODE is apk or core")
endif()
