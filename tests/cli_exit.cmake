# File/layer: tests/cli_exit.cmake | test-only, cmake -P, every platform
# Testing: HARNESS (CliUsageTest): havne-cli's exit code for a command line
#
#   cmake -DCLI=<havne-cli> -DEXPECT=<code> "-DARGS=<a|b|...>"
#         ["-DEXPECT_ERR=<words>"] -P cli_exit.cmake
#
# ARGS separates the arguments with '|': a ';' in an add_test argument
# would split it there. Passes when the program exits with EXPECT and
# writes nothing to standard output (a usage error isn't a stream: no
# JSON lines), and with EXPECT_ERR when standard error holds those words,
# so the refusal is the one the case means. A command line that streams
# instead of ending is killed after 20 s and fails, so a usage check that
# stops refusing can't hang the run.
#
# An empty element ("a||b", a trailing '|') reaches the program as an
# empty argument: the arguments are split by hand and each goes into the
# call as a bracket argument, since an unquoted list expansion drops
# empty elements. An ARGS of "" is no arguments at all.

set(quoted "")
set(rest "${ARGS}")
while(NOT rest STREQUAL "")
    string(FIND "${rest}" "|" bar)
    if(bar EQUAL -1)
        set(piece "${rest}")
        set(rest "")
    else()
        string(SUBSTRING "${rest}" 0 ${bar} piece)
        math(EXPR after "${bar} + 1")
        string(SUBSTRING "${rest}" ${after} -1 rest)
        if(rest STREQUAL "")
            # a trailing '|' ends on an empty argument
            string(APPEND quoted " [==[${piece}]==]")
            set(piece "")
        endif()
    endif()
    string(APPEND quoted " [==[${piece}]==]")
endwhile()
string(REPLACE "|" " " ARGS "${ARGS}")
cmake_language(EVAL CODE "execute_process(COMMAND [==[${CLI}]==]${quoted}
    TIMEOUT 20
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)")
if(NOT code STREQUAL EXPECT)
    message(FATAL_ERROR "havne-cli ${ARGS} exited ${code}, not ${EXPECT}\n${err}")
endif()
if(NOT out STREQUAL "")
    message(FATAL_ERROR "havne-cli ${ARGS} wrote to standard output:\n${out}")
endif()
if(DEFINED EXPECT_ERR)
    string(FIND "${err}" "${EXPECT_ERR}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR
            "havne-cli ${ARGS}: standard error lacks '${EXPECT_ERR}':\n${err}")
    endif()
endif()
