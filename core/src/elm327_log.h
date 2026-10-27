// File/layer: core/src/elm327_log.h | internal, C++17, Qt-free
// Testing: HARNESS — Elm327SessionTest.TheWireLog* (test_elm327): the
//          file and its lines, request lines only with frame logging, a
//          refusal's reason, a dropped link, a reset, the newest 50 kept;
//          .TheWorkerAllocatesNothingPerFrame with the requests' lines
//          on. UNIT — Elm327LogTest: the kept name and the text's escapes
//
// The ELM327 transport's wire log (ADR-0026): one file per connect in
// the temporary directory, ecuio-elm327.<YYYYMMDD-HHMMSS-mmm>.log, named
// for its opening in local time, the newest 50 kept. What the adapter
// was sent and said during the setup and the readings, what it said
// unasked, and the port's failures go in always; every request and every
// line of its reply too with ECUIO_SUR_LOG_FRAMES=1, the switch a
// recording sets for the surrogate's frame lines, each prompt with its
// time after the command. Each line is stamped with local time and the
// seconds since the log opened, and written whole and unbuffered, with
// no allocation, so the worker can write it.

#ifndef ECUIO_ELM327_LOG_H
#define ECUIO_ELM327_LOG_H

#include <chrono>
#include <cstddef>
#include <cstdint>

/* Earlier connects' logs kept beside the newest. */
constexpr int ELM327_KEPT_LOGS = 50;

/* The open file: a HANDLE on Windows, a descriptor elsewhere */
#ifdef _WIN32
using elm327_log_file = void*;
constexpr std::nullptr_t ELM327_LOG_NONE = nullptr;
#else
using elm327_log_file = int;
constexpr elm327_log_file ELM327_LOG_NONE = -1;
#endif

struct elm327_log {
    std::chrono::steady_clock::time_point opened{};
    elm327_log_file file = ELM327_LOG_NONE;
    bool requests = false; /* ECUIO_SUR_LOG_FRAMES=1: the requests' lines */
};

inline bool elm327_log_is_open(const elm327_log& log)
{
    return log.file != ELM327_LOG_NONE;
}

/* Whether name is one this log keeps: ecuio-elm327., the stamp, an
 * optional -2 to -9 for a second log in the same millisecond, .log.
 * Only such names are pruned. */
bool elm327_log_kept_name(const char* name);

/* Opens a new log and prunes the oldest past ELM327_KEPT_LOGS. Without a
 * usable directory there is no log, and nothing fails. */
void elm327_log_open(elm327_log& log);

/* One stamped line, cut to fit; nothing without a log. */
void elm327_log_write(elm327_log& log, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Text as the log shows it: printable ASCII as is, any other byte as
 * \xHH, cut to fit out (cap bytes, always terminated). */
void elm327_log_text(const char* in, size_t n, char* out, size_t cap);

void elm327_log_close(elm327_log& log);

#endif /* ECUIO_ELM327_LOG_H */
