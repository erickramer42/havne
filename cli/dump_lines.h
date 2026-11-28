#ifndef HAVNE_CLI_DUMP_LINES_H
#define HAVNE_CLI_DUMP_LINES_H

#include "jsonl.h"

#include "ecuio/dump.h"
#include "ecuio/operation.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cli {

constexpr size_t kDumpLineBytes = 2048;

inline const char* dump_phase_word(uint32_t phase)
{
    static const char* const words[] = {
        "none",    "checking",  "preconditions", "unlocking",
        "reading", "comparing", "saving",        "leaving",
        "done",    "erasing",   "writing",       "verifying"};
    return phase < sizeof words / sizeof words[0] ? words[phase] : "unknown";
}

inline const char* dump_end_word(uint32_t end)
{
    static const char* const words[] = {"none",
                                        "saved",
                                        "canceled",
                                        "refused",
                                        "no_answer",
                                        "differ",
                                        "identity_changed",
                                        "unlock_refused",
                                        "session_lost",
                                        "link_lost",
                                        "file",
                                        "bad_reply",
                                        "engine_running",
                                        "moving",
                                        "voltage_low",
                                        "voltage_high",
                                        "written",
                                        "check_failed",
                                        "recovery_needed"};
    return end < sizeof words / sizeof words[0] ? words[end] : "unknown";
}

struct DumpWarning {
    uint32_t bit;
    const char* word;
};

constexpr DumpWarning kDumpWarnings[] = {
    {ECUIO_OPERATION_CHECK_VOLTS_LOW, "volts_low"},
    {ECUIO_OPERATION_CHECK_VOLTS_HIGH, "volts_high"},
    {ECUIO_OPERATION_CHECK_VOLTS_SAG, "volts_sag"},
    {ECUIO_OPERATION_CHECK_IDENT_UNREAD, "ident_unread"},
    {ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE, "not_held_awake"},
    {ECUIO_OPERATION_CHECK_ON_BATTERY, "on_battery"},
};

inline std::string dump_volts(uint32_t mv)
{
    char t[16];
    std::snprintf(t, sizeof t, "%u.%u", mv / 1000u, (mv % 1000u) / 100u);
    return t;
}

inline std::string dump_warning_words(uint32_t bit,
                                      const ecuio_operation_state_t& s)
{
    const bool read = (s.checks & ECUIO_OPERATION_CHECK_VOLTS) != 0;
    switch (bit) {
    case ECUIO_OPERATION_CHECK_VOLTS_LOW:
        return read ? "the battery is at " + dump_volts(s.volts_mv) +
                          " V: keep a charger on"
                    : "the battery is low: keep a charger on";
    case ECUIO_OPERATION_CHECK_VOLTS_HIGH:
        return read ? "the supply is at " + dump_volts(s.volts_mv) +
                          " V: check the charger"
                    : "the supply is high: check the charger";
    case ECUIO_OPERATION_CHECK_VOLTS_SAG:
        return read ? "the battery is at " + dump_volts(s.volts_min_mv) +
                          " V: the car may not start"
                    : "the battery sagged: the car may not start";
    case ECUIO_OPERATION_CHECK_IDENT_UNREAD:
        return "an identifier went unanswered after the module's session "
               "was left";
    case ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE:
        return "this computer may sleep during the read: keep it awake";
    case ECUIO_OPERATION_CHECK_ON_BATTERY:
        return "this computer is on battery: keep it on mains power";
    default:
        return "";
    }
}

inline const char* dump_end_note(const ecuio_operation_state_t& s)
{
    if (s.end == ECUIO_OPERATION_END_FILE &&
        std::strstr(s.words, "exists already") != nullptr) {
        return "a different file already holds this name; nothing was "
               "replaced";
    }
    return nullptr;
}

inline bool dump_said_yes(const char* line)
{
    return std::strncmp(line, "yes", 3) == 0 &&
           (line[3] == '\0' || line[3] == '\n' || line[3] == '\r');
}

inline bool dump_start_is_usage(ecuio_status_t st)
{
    return st == ECUIO_ERR_CONSENT_REQUIRED;
}

inline void dump_set_words(ecuio_operation_state_t& s, const char* words)
{
    std::snprintf(s.words, sizeof s.words, "%s", words);
}

inline std::string dump_name_hash(const char* name)
{
    const char ext[] = ".havnedump";
    const size_t n = std::strlen(name);
    const size_t e = sizeof ext - 1;
    if (n < 64 + e || std::strcmp(name + n - e, ext) != 0) {
        return "";
    }
    return std::string(name + n - e - 64, 64);
}

inline void dump_text_or_null(Line& l, const char* s)
{
    if (s == nullptr || *s == '\0') {
        l.raw("null");
    } else {
        l.string(s, std::strlen(s));
    }
}

inline void warning_line(Line& l, const char* word, const std::string& words)
{
    l.raw("{\"type\":\"warning\"");
    l.key("check");
    l.string(word, std::strlen(word));
    l.key("words");
    l.string(words.data(), words.size());
    l.ch('}');
}

inline void phase_line(Line& l, const ecuio_operation_state_t& s,
                       const char* step_name)
{
    l.raw("{\"type\":\"phase\"");
    l.key("phase");
    const char* w = dump_phase_word(s.phase);
    l.string(w, std::strlen(w));
    l.key("step");
    l.u64(s.step);
    l.key("name");
    dump_text_or_null(l, step_name);
    l.key("pass");
    l.u64(s.pass);
    l.ch('}');
}

inline uint32_t dump_percent(const ecuio_operation_state_t& s)
{
    return s.bytes_total == 0
               ? 0u
               : static_cast<uint32_t>(s.bytes_done * 100u / s.bytes_total);
}

inline void progress_line(Line& l, const ecuio_operation_state_t& s)
{
    l.raw("{\"type\":\"progress\"");
    l.key("bytes_done");
    l.u64(s.bytes_done);
    l.key("bytes_total");
    l.u64(s.bytes_total);
    l.key("percent");
    l.u64(dump_percent(s));
    l.key("waiting_ms");
    l.u64(s.waiting_ms);
    l.ch('}');
}

inline void dump_volts_keys(Line& l, const ecuio_operation_state_t& s)
{
    const bool volts = (s.checks & ECUIO_OPERATION_CHECK_VOLTS) != 0;
    l.key("volts_mv");
    volts ? l.u64(s.volts_mv) : l.raw("null");
    l.key("volts_min_mv");
    volts ? l.u64(s.volts_min_mv) : l.raw("null");
    l.key("rpm");
    (s.checks & ECUIO_OPERATION_CHECK_RPM) ? l.u64(s.rpm) : l.raw("null");
    l.key("kmh");
    (s.checks & ECUIO_OPERATION_CHECK_KMH) ? l.u64(s.kmh) : l.raw("null");
}

/* Room: kDumpLineBytes plus Line::string_bytes(path) */
inline void dump_line(Line& l, const ecuio_operation_state_t& s,
                      const std::string& path, int verified)
{
    const bool saved = s.end == ECUIO_OPERATION_END_SAVED;
    l.raw("{\"type\":\"dump\"");
    l.key("end");
    const char* w = dump_end_word(s.end);
    l.string(w, std::strlen(w));
    l.key("words");
    dump_text_or_null(l, s.words);
    l.key("step");
    l.u64(s.step);
    l.key("name");
    dump_text_or_null(l, saved ? s.name : nullptr);
    l.key("path");
    dump_text_or_null(l, saved ? path.c_str() : nullptr);
    l.key("sha256");
    const std::string hash = saved ? dump_name_hash(s.name) : "";
    dump_text_or_null(l, hash.c_str());
    l.key("bytes");
    l.u64(s.bytes_total / 2u);
    l.key("verified");
    l.raw(verified < 0 ? "null" : verified ? "true" : "false");
    l.key("nrc");
    if (s.nrc != 0) {
        l.hex_bytes(&s.nrc, 1);
    } else {
        l.raw("null");
    }
    l.key("differ_offset");
    s.end == ECUIO_OPERATION_END_DIFFER ? l.u64(s.differ_offset)
                                        : l.raw("null");
    dump_volts_keys(l, s);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_DUMP_LINES_H
