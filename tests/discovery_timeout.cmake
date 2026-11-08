# File/layer: tests/discovery_timeout.cmake | test-only, cmake -P
# Testing: HARNESS (ctest DiscoveryTimeoutTest.EveryDiscoveryWaits)
#
#   cmake -DFILE=<tests/CMakeLists.txt> -P discovery_timeout.cmake
#
# gtest_discover_tests runs each binary at build time and gives up after
# 5 s by default, which a cold CI runner can take before the first line.
# Every call in FILE names a DISCOVERY_TIMEOUT of its own, of at least
# _least seconds as a plain number.

file(READ "${FILE}" _text)
string(REGEX MATCHALL "gtest_discover_tests\\([^)]*\\)" _calls "${_text}")
list(LENGTH _calls _count)
if(_count EQUAL 0)
    message(FATAL_ERROR "no gtest_discover_tests call in ${FILE}")
endif()
set(_least 60)
set(_short "")
foreach(_call IN LISTS _calls)
    set(_seconds 0)
    if(_call MATCHES "DISCOVERY_TIMEOUT[ \t\r\n]+([0-9]+)")
        set(_seconds "${CMAKE_MATCH_1}")
    endif()
    if(_seconds LESS _least)
        string(REGEX REPLACE "[ \t\r\n]+" " " _call "${_call}")
        list(APPEND _short "${_call}")
    endif()
endforeach()
if(_short)
    string(JOIN "\n  " _list ${_short})
    message(FATAL_ERROR "without a DISCOVERY_TIMEOUT of at least ${_least}:\n  ${_list}")
endif()
message("${_count} gtest_discover_tests calls, each with a DISCOVERY_TIMEOUT of at least ${_least}")
