// File/layer: core/src/csv_reader.h | internal, not ABI-exported; included by csv_import.h alone
// C++17, Qt-free
// Testing: UNIT: CsvImportTest (test_core); FUZZ: fuzz_csv_import
//
// The CSV import's RFC 4180 reader over any byte source (csv_import.h).

#ifndef ECUIO_CSV_READER_H
#define ECUIO_CSV_READER_H

#include "csv_map.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ecuio_csv_import {

/* ---- the RFC 4180 reader ---- */

/* Source: int get() returning a byte (0 to 255) or -1 at the end */
template <class Source> class reader
{
public:
    reader(Source& src, char delimiter) : src_(src), delim_(delimiter) {}

    /* The physical lines read so far (a line break, or the last line's
     * end) */
    uint64_t lines() const { return lines_; }

    /* From the next record on, an unquoted field loses the spaces and tabs
     * at its ends (" " is then empty), as a logger that writes ", "
     * between fields pads them. The import and the guess turn it on for
     * the rows below the header, so a header keeps its text and a map
     * naming it still finds it. Padding before a quoted field's opening
     * quote and after its closing one is taken in every record, the
     * header's too; the quoted text itself is kept exactly (RFC 4180). */
    void trim_padding(bool on) { padding_ = on; }

    /* Skips a preamble row, as text: false at the end, with *why set on
     * a refusal */
    bool skip_line(std::string* why)
    {
        size_t bytes = 0;
        bool any = false;
        for (int c = take(); c >= 0; c = take()) {
            any = true;
            if (c == 0) {
                return fail(why, "the file holds a NUL byte");
            }
            if (c == '\n') {
                ++lines_;
                return true;
            }
            if (++bytes > kRowBytesMax) {
                return fail(why, "the row is longer than 64 KiB");
            }
        }
        if (any) {
            ++lines_;
        }
        return why->empty() && any;
    }

    /* The next record's fields: 1 a record (*line_no its first line), 0
     * the end, -1 a refusal (*why "line N: ...") */
    int next(std::vector<std::string>& fields, uint64_t* line_no,
             std::string* why)
    {
        for (;;) {
            fields.clear();
            *line_no = lines_ + 1;
            int c = take();
            if (c < 0) {
                return 0;
            }
            if (c == '\n' || (c == '\r' && peek_lf())) {
                /* a blank line */
                if (c == '\r') {
                    (void)take();
                }
                ++lines_;
                continue;
            }
            size_t bytes = 0;
            std::string field;
            bool quoted = false;
            bool closed = false; /* a quoted field's closing quote */
            bool at_start = true;
            for (;;) {
                if (c < 0) {
                    if (quoted && !closed) {
                        return refuse(why, *line_no,
                                      "a quoted field is never closed");
                    }
                    ++lines_;
                    break;
                }
                if (c == 0) {
                    return refuse(why, lines_ + 1, "the row holds a NUL byte");
                }
                if (++bytes > kRowBytesMax) {
                    return refuse(why, *line_no,
                                  "the row is longer than 64 KiB");
                }
                const char ch = static_cast<char>(c);
                if (quoted && !closed) {
                    if (ch == '"') {
                        if (peek() == '"') {
                            (void)take();
                            ++bytes;
                            field += '"';
                        } else {
                            closed = true;
                        }
                    } else {
                        if (ch == '\n') {
                            ++lines_;
                        }
                        field += ch;
                    }
                } else if (ch == delim_ || ch == '\n' || ch == '\r') {
                    if (ch == '\r') {
                        if (!peek_lf()) {
                            return refuse(why, lines_ + 1,
                                          "a carriage return without a line "
                                          "feed");
                        }
                        (void)take();
                    }
                    if (!push(fields, field, quoted, *line_no, why)) {
                        return -1;
                    }
                    field.clear();
                    quoted = closed = false;
                    at_start = true;
                    if (ch != delim_) {
                        ++lines_;
                        return 1;
                    }
                    c = take();
                    continue;
                } else if (closed) {
                    if (!pad(ch)) {
                        return refuse(why, lines_ + 1,
                                      "text after a quoted field's closing "
                                      "quote");
                    }
                } else if (ch == '"') {
                    if (!at_start && !only_padding(field)) {
                        return refuse(why, lines_ + 1,
                                      "a quote inside a field that isn't "
                                      "quoted");
                    }
                    field.clear();
                    quoted = true;
                } else {
                    field += ch;
                }
                at_start = false;
                c = take();
            }
            if (!push(fields, field, quoted, *line_no, why)) {
                return -1;
            }
            return 1;
        }
    }

private:
    /* The source's first three bytes looked at once: a UTF-8 BOM there is
     * dropped, anything else handed on as read */
    int raw()
    {
        if (!started_) {
            started_ = true;
            while (pend_n_ < 3) {
                const int c = src_.get();
                if (c < 0) {
                    break;
                }
                pend_[pend_n_++] = c;
            }
            if (pend_n_ == 3 && pend_[0] == 0xEF && pend_[1] == 0xBB &&
                pend_[2] == 0xBF) {
                pend_n_ = 0;
            }
        }
        if (pend_at_ < pend_n_) {
            return pend_[pend_at_++];
        }
        return src_.get();
    }

    int take()
    {
        if (have_) {
            have_ = false;
            return held_;
        }
        return raw();
    }

    int peek()
    {
        if (!have_) {
            held_ = raw();
            have_ = true;
        }
        return held_;
    }

    bool peek_lf() { return peek() == '\n'; }

    static bool pad(char c) { return c == ' ' || c == '\t'; }

    static bool only_padding(const std::string& s)
    {
        for (char c : s) {
            if (!pad(c)) {
                return false;
            }
        }
        return true;
    }

    bool push(std::vector<std::string>& fields, std::string& field, bool quoted,
              uint64_t line_no, std::string* why)
    {
        if (padding_ && !quoted) {
            size_t a = 0;
            size_t b = field.size();
            while (a < b && pad(field[a])) {
                ++a;
            }
            while (b > a && pad(field[b - 1])) {
                --b;
            }
            field = field.substr(a, b - a);
        }
        if (fields.size() == kColumnsMax) {
            return refuse(why, line_no, "the row has more than 1024 columns") >=
                   0;
        }
        if (!utf8_valid(field.data(), field.size())) {
            return refuse(why, line_no, "the row isn't valid UTF-8") >= 0;
        }
        fields.push_back(field);
        return true;
    }

    static bool fail(std::string* why, const char* words)
    {
        *why = words;
        return false;
    }

    static int refuse(std::string* why, uint64_t line, const char* words)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf,
                      "line %llu: ", static_cast<unsigned long long>(line));
        *why = std::string(buf) + words;
        return -1;
    }

    Source& src_;
    char delim_;
    uint64_t lines_ = 0;
    bool started_ = false;
    int pend_[3] = {};
    int pend_n_ = 0;
    int pend_at_ = 0;
    bool have_ = false;
    int held_ = -1;
    bool padding_ = false;
};

} // namespace ecuio_csv_import

#endif /* ECUIO_CSV_READER_H */
