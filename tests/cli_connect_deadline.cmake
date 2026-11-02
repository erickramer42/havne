# File/layer: tests/cli_connect_deadline.cmake | test-only, cmake -P, Windows
# Testing: HARNESS (CliConnectTest): havne-cli ends a connect that runs
#          past its deadline
#
#   cmake -DCLI=<havne-cli> -DDLL=<fake_j2534.dll> -P cli_connect_deadline.cmake
#
# The fake DLL holds PassThruOpen 4 s in the surrogate
# (ECUIO_FAKE_OPEN_DELAY_MS), inside core's own 8 s window, and
# HAVNE_CLI_CONNECT_MS gives the connect 0.5 s: the CLI must write the
# error line and exit 3 before the open returns, not stream once it does.

set(ENV{HAVNE_CLI_CONNECT_MS} 500)
set(ENV{ECUIO_FAKE_OPEN_DELAY_MS} 4000)
string(TIMESTAMP started "%s")
execute_process(COMMAND ${CLI} stream "j2534://${DLL}" --seconds 1
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 20)
string(TIMESTAMP ended "%s")
math(EXPR took "${ended} - ${started}")
if(NOT code STREQUAL "3")
    message(FATAL_ERROR "havne-cli exited ${code}, not 3\n${out}\n${err}")
endif()
if(NOT out MATCHES "\"type\":\"error\"" OR
   NOT out MATCHES "the connect didn't finish within 1 s")
    message(FATAL_ERROR "no deadline error line:\n${out}")
endif()
if(took GREATER 3)
    message(FATAL_ERROR "havne-cli took ${took} s: it waited for the open")
endif()
