# File/layer: tests/discovery_timeout.cmake | test-only, cmake -P
# Testing: HARNESS (ctest DiscoveryTimeoutTest.EveryDiscoveryWaits)
#
#   cmake -DFILE=<tests/CMakeLists.txt> -P discovery_timeout.cmake
#
# gtest_discover_tests runs each binary at build time and gives up after
# 5 s by default, which a cold CI runner can take before the first line.
# Every call in FILE names a DISCOVERY_TIMEOUT of its own.

file(READ "${FILE}" _text)
string(REGEX MATCHALL "gtest_discover_tests\\([^)]*\\)" _calls "${_text}")
list(LENGTH _calls _count)
if(_count EQUAL 0)
    message(FATAL_ERROR "no gtest_discover_tests call in ${FILE}")
endif()
set(_bare "")
foreach(_call IN LISTS _calls)
    if(NOT _call MATCHES "DISCOVERY_TIMEOUT")
        string(REGEX REPLACE "[ \t\r\n]+" " " _call "${_call}")
        list(APPEND _bare "${_call}")
    endif()
endforeach()
if(_bare)
    string(JOIN "\n  " _list ${_bare})
    message(FATAL_ERROR "without a DISCOVERY_TIMEOUT:\n  ${_list}")
endif()
message("${_count} gtest_discover_tests calls, each with a DISCOVERY_TIMEOUT")
