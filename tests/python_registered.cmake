# File/layer: tests/python_registered.cmake | test-only, cmake -P
# Testing: HARNESS (ctest RequiredTest.ThePythonTestsAreRegistered)
#
#   cmake -DPYTHON_TESTS=<n> -DWHEEL=<0|1> -DWHEEL_EXPECTED=<0|1> -P python_registered.cmake
#
# The PythonTest.* and WheelTest.Installed cases are registered only
# where configure found what they need (the shared core, a Python of the
# tree's pointer size), so a probe that failed would leave them out and
# a run would pass without them. Under ECUIO_PYTEST_REQUIRED=1 (CI's
# test jobs) none registered, or no wheel test where a 64-bit tree
# without sanitizers should have one, fails; without it the case is
# skipped (SKIP_REGULAR_EXPRESSION "skipped:").

set(_missing "")
if(NOT PYTHON_TESTS GREATER 0)
    list(APPEND _missing "no PythonTest.* case")
endif()
if(WHEEL_EXPECTED AND NOT WHEEL)
    list(APPEND _missing "no WheelTest.Installed")
endif()
if(NOT _missing)
    message("${PYTHON_TESTS} PythonTest.* cases; WheelTest.Installed ${WHEEL}")
    return()
endif()
string(JOIN " and " _why ${_missing})
if("$ENV{ECUIO_PYTEST_REQUIRED}" STREQUAL "1")
    message(FATAL_ERROR "${_why} registered, and ECUIO_PYTEST_REQUIRED=1: "
                        "configure left them out (the shared core off, or no "
                        "Python of the tree's pointer size)")
endif()
message("skipped: ${_why} registered (ECUIO_PYTEST_REQUIRED=1 fails it)")
