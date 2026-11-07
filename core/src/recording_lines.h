// File/layer: core/src/recording_lines.h | C++17, Qt-free; a recording's
//             lines as core writes them (ADR-0049)
// Testing: UNIT: RecorderTest, CsvImportTest (test_core)
//
// The writer's side of recording_rows.h: a CSV field quoted as the
// recorder quotes it, and events.log's signal line, shared by the
// recorder and the CSV import so both write one form, which the reader's
// parse_header_signal takes.

#ifndef ECUIO_RECORDING_LINES_H
#define ECUIO_RECORDING_LINES_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace ecuio_recording_lines {

/* A CSV field, quoted only when it has to be, as the app's recorder
 * quotes it; a line break, which no line or row may hold, a space */
inline std::string csv_quoted(const char* text, size_t len)
{
    std::string f(text, len);
    for (char& c : f) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    if (f.find_first_of(",\"") == std::string::npos) {
        return f;
    }
    std::string q = "\"";
    for (char c : f) {
        if (c == '"') {
            q += '"';
        }
        q += c;
    }
    q += '"';
    return q;
}

inline std::string csv_quoted(const std::string& s)
{
    return csv_quoted(s.data(), s.size());
}

/* "signal  <name>,<number|text|enum>,<unit>,<decimals>,<rank>,<group>\n",
 * name and unit already quoted */
inline std::string signal_line(const std::string& quoted_name,
                               const char* kind_word,
                               const std::string& quoted_unit,
                               unsigned decimals, unsigned rank, unsigned group)
{
    return "signal  " + quoted_name + "," + kind_word + "," + quoted_unit +
           "," + std::to_string(decimals) + "," + std::to_string(rank) + "," +
           std::to_string(group) + "\n";
}

} // namespace ecuio_recording_lines

#endif /* ECUIO_RECORDING_LINES_H */
