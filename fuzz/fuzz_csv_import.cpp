// File/layer: fuzz/fuzz_csv_import.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_csv_import
//
// The CSV import's column map and reader (ADR-0054's appendix "the CSV
// import as built", core/src/csv_import.h) over arbitrary bytes: the first
// line is the column map's JSON, the rest the CSV, read from memory. A
// first line of "guess", or "guess" and a unit (s, ms, us), guesses the map
// from the CSV instead (its appendix "a column map guessed from the
// header", core/src/csv_guess.h) and imports with each map it gives.

#include "csv_guess.h"
#include "csv_import.h"
#include "json.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

namespace csv = ecuio_csv_import;
namespace guess = ecuio_map_guess;

struct sample {
    uint64_t host_us;
    size_t signal;
    bool text;
    double number;
    std::string value;
    bool operator==(const sample& o) const
    {
        /* a number compared by its bits: NaN never reaches a sink */
        return host_us == o.host_us && signal == o.signal && text == o.text &&
               memcmp(&number, &o.number, sizeof number) == 0 &&
               value == o.value;
    }
};

struct keeping_sink {
    std::vector<sample> samples;
    bool number(uint64_t host_us, size_t signal, double v)
    {
        samples.push_back({host_us, signal, false, v, {}});
        return true;
    }
    bool text(uint64_t host_us, size_t signal, const std::string& v)
    {
        samples.push_back({host_us, signal, true, 0.0, v});
        return true;
    }
    std::string failure() const { return {}; }
};

bool same(const csv::outcome& a, const csv::outcome& b)
{
    return a.lines == b.lines && a.rows == b.rows && a.values == b.values &&
           a.ignored == b.ignored && a.has_utc == b.has_utc &&
           a.utc_start_us == b.utc_start_us && a.duration_us == b.duration_us;
}

/* A map's text read, aborting on a map read that breaks its bounds */
bool read_map(const char* p, size_t n, csv::column_map& m)
{
    std::string why;
    if (!csv::parse_map(p, n, m, &why)) {
        if (why.empty()) {
            abort();
        }
        return false;
    }
    if (m.signals.empty() || m.signals.size() > csv::kSignalsMax ||
        m.missing.size() > csv::kMissingMax || m.header_row < 1 ||
        m.header_row > csv::kHeaderRowMax) {
        abort();
    }
    return true;
}

/* The import of body under m, twice: the two agree, and what it hands on
 * keeps the map's rules */
void import(const csv::column_map& m, const unsigned char* body, size_t body_n)
{
    csv::memory_source s1{body, body_n};
    keeping_sink k1;
    csv::outcome o1;
    std::string why1;
    const bool ok = csv::run(s1, m, k1, o1, &why1);
    csv::memory_source s2{body, body_n};
    keeping_sink k2;
    csv::outcome o2;
    std::string why2;
    /* Two reads of the same bytes agree: the import's second pass must */
    if (csv::run(s2, m, k2, o2, &why2) != ok || why1 != why2) {
        abort();
    }
    if (!ok) {
        if (why1.empty()) {
            abort();
        }
        return;
    }
    if (!same(o1, o2) || !(k1.samples == k2.samples)) {
        abort();
    }
    if (o1.values != k1.samples.size() ||
        (!o1.has_utc && o1.utc_start_us != 0)) {
        abort();
    }
    uint64_t prev = 0;
    for (const sample& x : k1.samples) {
        if (x.host_us < prev || x.host_us > o1.duration_us ||
            x.signal >= m.signals.size() ||
            x.text != m.signals[x.signal].text) {
            abort();
        }
        prev = x.host_us;
        if (x.text) {
            if (x.value.empty() || x.value.size() > csv::kTextMax ||
                !csv::utf8_valid(x.value.data(), x.value.size())) {
                abort();
            }
            for (const std::string& gone : m.missing) {
                if (x.value == gone) {
                    abort(); /* a missing text is no sample */
                }
            }
        } else if (!isfinite(x.number)) {
            abort();
        }
    }
}

/* The guess, twice: the two agree, the text is JSON, its map's slice is
 * one the import takes (once a time unit is in), and that map imports */
void guessed(const unsigned char* body, size_t body_n, const char* unit)
{
    guess::result r1;
    std::string why1;
    const bool ok = guess::guess(body, body_n, false, unit, r1, &why1);
    guess::result r2;
    std::string why2;
    if (guess::guess(body, body_n, false, unit, r2, &why2) != ok ||
        why1 != why2 || r1.text != r2.text || r1.map_offset != r2.map_offset ||
        r1.map_length != r2.map_length || r1.flags != r2.flags ||
        r1.columns != r2.columns || r1.left_out != r2.left_out ||
        r1.rows_read != r2.rows_read) {
        abort();
    }
    if (!ok) {
        if (why1.empty() || why1.find('\n') != std::string::npos) {
            abort();
        }
        return;
    }
    ecuio_json::Value v;
    ecuio_json::Error e;
    if (!ecuio_json::parse(r1.text.data(), r1.text.size(), &v, &e) ||
        v.find("map") == nullptr || v.find("columns") == nullptr ||
        v.find("notes") == nullptr ||
        r1.map_offset + r1.map_length > r1.text.size() || r1.columns < 1 ||
        r1.columns > csv::kSignalsMax || r1.rows_read < 1 ||
        r1.rows_read > guess::kSampleRows ||
        v.find("notes")->items.size() > guess::kNotesMax + 1) {
        abort();
    }
    const std::string slice = r1.text.substr(r1.map_offset, r1.map_length);
    csv::column_map m;
    if ((r1.flags & guess::kTimeUnitKnown) != 0) {
        /* The slice alone is the map, as the CLI prints and passes it */
        if (!read_map(slice.data(), slice.size(), m) ||
            m.signals.size() != r1.columns) {
            abort();
        }
        import(m, body, body_n);
        return;
    }
    /* No unit (none given, or a time a unit can't read): the map lacks
     * one, and the import refuses it until it's in */
    std::string why;
    csv::column_map none;
    if (csv::parse_map(slice.data(), slice.size(), none, &why)) {
        abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Enough for a map, a header and rows; a row's own bound is 64 KiB */
    if (size > 96 * 1024) {
        return 0;
    }
    const char* p = reinterpret_cast<const char*>(data);
    const char* nl = static_cast<const char*>(memchr(p, '\n', size));
    const size_t map_n = nl != nullptr ? static_cast<size_t>(nl - p) : size;
    const auto* body = reinterpret_cast<const unsigned char*>(
        nl != nullptr ? nl + 1 : p + size);
    const size_t body_n =
        size -
        static_cast<size_t>(reinterpret_cast<const uint8_t*>(body) - data);

    const std::string first(p, map_n);
    if (first.compare(0, 5, "guess") == 0) {
        const std::string rest = first.substr(5);
        if (rest.empty()) {
            guessed(body, body_n, nullptr);
        } else if (rest == " s" || rest == " ms" || rest == " us") {
            guessed(body, body_n, rest.c_str() + 1);
        }
        return 0;
    }
    csv::column_map m;
    if (!read_map(p, map_n, m)) {
        return 0;
    }
    import(m, body, body_n);
    return 0;
}
