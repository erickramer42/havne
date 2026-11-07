// File/layer: fuzz/fuzz_csv_import.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_csv_import
//
// The CSV import's column map and reader (ADR-0054's appendix "the CSV
// import as built", core/src/csv_import.h) over arbitrary bytes: the first
// line is the column map's JSON, the rest the CSV, read from memory.

#include "csv_import.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

namespace csv = ecuio_csv_import;

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
    csv::column_map m;
    std::string why;
    if (!csv::parse_map(p, map_n, m, &why)) {
        if (why.empty()) {
            abort();
        }
        return 0;
    }
    if (m.signals.empty() || m.signals.size() > csv::kSignalsMax ||
        m.missing.size() > csv::kMissingMax || m.header_row < 1 ||
        m.header_row > csv::kHeaderRowMax) {
        abort();
    }
    const auto* body = reinterpret_cast<const unsigned char*>(
        nl != nullptr ? nl + 1 : p + size);
    const size_t body_n =
        size -
        static_cast<size_t>(reinterpret_cast<const uint8_t*>(body) - data);

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
        return 0;
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
    return 0;
}
