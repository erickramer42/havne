// File/layer: core/src/recording_keys.h | internal, C++17, Qt-free,
//             every platform
// Testing: UNIT: RecorderTest, RecordingTest (test_core)
//
// What a recording keeps a signal under (ADR-0065 section 9): its key in
// format 4, its name before. The recorder, the reader and the exports
// share the format's predicates, a set's keys and the signal line's text.

#ifndef ECUIO_RECORDING_KEYS_H
#define ECUIO_RECORDING_KEYS_H

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "recording_rows.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_recording_keys {

/* A format whose header lines the readers parse */
inline bool has_header(uint32_t format)
{
    return format == ECUIO_RECORDING_FORMAT_CORE ||
           format == ECUIO_RECORDING_FORMAT_ENUMS ||
           format == ECUIO_RECORDING_FORMAT_KEYS;
}

/* A format with enum signals' labels, a signal file's line and the
 * conditions' lines: format 3's, kept by every later one */
inline bool has_enums(uint32_t format)
{
    return format == ECUIO_RECORDING_FORMAT_ENUMS ||
           format == ECUIO_RECORDING_FORMAT_KEYS;
}

/* events.log's first line as a format, ECUIO_RECORDING_FORMAT_UNKNOWN
 * for any other text (format 1 goes by its ending, the reader's to
 * judge) */
inline uint32_t format_of(const char* p, size_t n)
{
    namespace rows = ecuio_recording_rows;
    return rows::equals(p, n, rows::kFormat4)   ? ECUIO_RECORDING_FORMAT_KEYS
           : rows::equals(p, n, rows::kFormat3) ? ECUIO_RECORDING_FORMAT_ENUMS
           : rows::equals(p, n, rows::kFormat2)
               ? ECUIO_RECORDING_FORMAT_CORE
               : ECUIO_RECORDING_FORMAT_UNKNOWN;
}

/* The key each def of set is kept under (ecuio_signal_set_keys), in the
 * set's order; none for no set. file is the set's signal file or NULL.
 * ecuio_signal_set_keys' statuses; nothing thrown. */
ecuio_status_t set_keys(const ecuio_signal_set_t* set,
                        const ecuio_signal_file_t* file,
                        std::vector<ecuio_signal_key_t>* out) noexcept;

/* A format 4 signal line's text, "<key>,<name>,<kind>,...": the key,
 * 1 to ECUIO_SIGNAL_KEY_MAX bytes, then what parse_header_signal takes */
inline bool parse_keyed_signal(const char* text, size_t n, std::string* key,
                               ecuio_recording_rows::header_signal& s,
                               const char** why)
{
    namespace rows = ecuio_recording_rows;
    if (rows::holds_nul(text, n, why)) {
        return false;
    }
    const char* p = text;
    const char* end = text + n;
    bool last = false;
    if (!rows::next_field(p, end, *key, &last) || last) {
        *why = "the signal line isn't key,name,kind,unit,decimals,rank,group";
        return false;
    }
    if (key->empty() || key->size() > ECUIO_SIGNAL_KEY_MAX) {
        *why = "the signal's key is empty or longer than 31 bytes";
        return false;
    }
    return rows::parse_header_signal(p, static_cast<size_t>(end - p), s, why);
}

/* A signal line's text in a format with a header: *id is what
 * signals.csv names the signal by, its key in format 4 and its name
 * before; the rest goes into s */
inline bool parse_signal_line(uint32_t format, const char* text, size_t n,
                              std::string* id,
                              ecuio_recording_rows::header_signal& s,
                              const char** why)
{
    if (format == ECUIO_RECORDING_FORMAT_KEYS) {
        return parse_keyed_signal(text, n, id, s, why);
    }
    if (!ecuio_recording_rows::parse_header_signal(text, n, s, why)) {
        return false;
    }
    *id = s.name;
    return true;
}

} // namespace ecuio_recording_keys

#endif /* ECUIO_RECORDING_KEYS_H */
