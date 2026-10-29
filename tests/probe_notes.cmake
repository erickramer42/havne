# File/layer: tests/probe_notes.cmake | test-only, cmake -P, Linux with <sys/sdt.h>
# Testing: UNIT (ProbeNotesTest): core's USDT probes are in a binary that
#          links it (ADR-0034 §4)
#
#   cmake -DREADELF=<readelf> -DBINARY=<file> -P probe_notes.cmake
#
# Passes when the binary's .note.stapsdt notes hold each of core's probes
# under provider "ecuio" (core/src/probes.h). A tracer finds a probe only
# through its note, so a probe compiled out, or a build that lost
# ECUIO_HAVE_SDT, fails here.

execute_process(COMMAND ${READELF} --notes ${BINARY}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE notes
    ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "readelf --notes ${BINARY} exited ${code}\n${err}")
endif()
foreach(probe session_start session_close frame)
    if(NOT notes MATCHES "Provider: ecuio[ \t\r\n]+Name: ${probe}[ \t\r\n]")
        message(FATAL_ERROR "${BINARY} has no USDT probe ecuio:${probe}\n${notes}")
    endif()
endforeach()
message(STATUS "${BINARY}: ecuio:session_start, session_close and frame")
