# File/layer: tests/cli_exit.cmake | test-only, cmake -P, every platform
# Testing: HARNESS (CliUsageTest): havne-cli's exit code for a command line
#
#   cmake -DCLI=<havne-cli> -DEXPECT=<code> "-DARGS=<a|b|...>" -P cli_exit.cmake
#
# ARGS separates the arguments with '|': a ';' in an add_test argument
# would split it there. Passes when the program exits with EXPECT and
# writes nothing to standard output (a usage error isn't a stream: no
# JSON lines). A command line that streams instead of ending is killed
# after 20 s and fails, so a usage check that stops refusing can't hang
# the run.

string(REPLACE "|" ";" ARGS "${ARGS}")
execute_process(COMMAND ${CLI} ${ARGS}
    TIMEOUT 20
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT code STREQUAL EXPECT)
    message(FATAL_ERROR "havne-cli ${ARGS} exited ${code}, not ${EXPECT}\n${err}")
endif()
if(NOT out STREQUAL "")
    message(FATAL_ERROR "havne-cli ${ARGS} wrote to standard output:\n${out}")
endif()
