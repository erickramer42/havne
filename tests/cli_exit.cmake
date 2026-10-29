# File/layer: tests/cli_exit.cmake | test-only, cmake -P, every platform
# Testing: HARNESS (CliUsageTest): havne-cli's exit code for a command line
#
#   cmake -DCLI=<havne-cli> -DEXPECT=<code> "-DARGS=<a|b|...>" -P cli_exit.cmake
#
# ARGS separates the arguments with '|': a ';' in an add_test argument
# would split it there. Passes when the program exits with EXPECT and
# writes nothing to standard output (a usage error isn't a stream: no
# JSON lines).

string(REPLACE "|" ";" ARGS "${ARGS}")
execute_process(COMMAND ${CLI} ${ARGS}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT code STREQUAL EXPECT)
    message(FATAL_ERROR "havne-cli ${ARGS} exited ${code}, not ${EXPECT}\n${err}")
endif()
if(NOT out STREQUAL "")
    message(FATAL_ERROR "havne-cli ${ARGS} wrote to standard output:\n${out}")
endif()
