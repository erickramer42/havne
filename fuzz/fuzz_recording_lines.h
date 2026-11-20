// File/layer: fuzz/fuzz_recording_lines.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (the row parsers on one line)

#pragma once

#include "fuzz_recording_common.h"

namespace {

void one_line(const char* p, size_t n)
{
    rows::signal_row a;
    rows::signal_row b;
    const char* why = nullptr;
    const bool ok = rows::parse_signal_row(p, n, a, &why);
    if (ok != rows::parse_signal_row(p, n, b, &why)) {
        abort();
    }
    if (ok) {
        if (a.name.empty() || a.name.size() > rows::kNameMax ||
            a.unit.size() > rows::kUnitMax || a.host_us != b.host_us ||
            a.name != b.name || a.value != b.value || a.unit != b.unit) {
            abort();
        }
        double x = 0.0;
        double y = 0.0;
        const bool num = rows::parse_number(a.value.data(), a.value.size(), &x);
        if (num != rows::parse_number(a.value.data(), a.value.size(), &y) ||
            (num && !same_bits(x, y))) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }

    uint64_t host_us = 0;
    std::string label;
    why = nullptr;
    if (!rows::parse_marker_row(p, n, &host_us, label, &why) &&
        why == nullptr) {
        abort();
    }

    rows::header_signal s;
    why = nullptr;
    if (rows::parse_header_signal(p, n, s, &why)) {
        if (s.name.empty() || s.name.size() > rows::kNameMax ||
            s.unit.size() > rows::kUnitMax || s.decimal_places > 3) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }
    if (s.text && s.enumeration) {
        abort(); /* one kind */
    }

    /* Format 4's signal line: a key of 1 to 31 bytes, then the same */
    std::string key;
    rows::header_signal ks;
    why = nullptr;
    if (ecuio_recording_keys::parse_keyed_signal(p, n, &key, ks, &why)) {
        if (key.empty() || key.size() > ECUIO_SIGNAL_KEY_MAX ||
            ks.name.empty() || ks.name.size() > rows::kNameMax ||
            ks.unit.size() > rows::kUnitMax || ks.decimal_places > 3) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }

    /* Format 3's label line: a name within 31 bytes, a label of 1 to 27 */
    rows::header_label hl;
    why = nullptr;
    if (rows::parse_header_label(p, n, hl, &why)) {
        if (hl.signal.empty() || hl.signal.size() > rows::kNameMax ||
            hl.label.empty() || hl.label.size() > rows::kLabelMax) {
            abort();
        }
    } else if (why == nullptr) {
        abort();
    }

    uint64_t utc = 0;
    if (rows::parse_utc(p, n, &utc) && utc % 1000 != 0) {
        abort(); /* milliseconds, as microseconds */
    }

    rows::event_line e;
    if (rows::parse_event_line(p, n, &e)) {
        if (e.rest < p || e.rest + e.rest_len != p + n) {
            abort();
        }
        const char* text = nullptr;
        size_t text_len = 0;
        size_t kind_len = 0;
        if (rows::split_kind(e.rest, e.rest_len, &text, &text_len, &kind_len) &&
            (kind_len == 0 || text + text_len != p + n)) {
            abort();
        }
    }
    uint64_t records = 0;
    uint64_t notes = 0;
    (void)rows::parse_end(p, n, &records, &notes);
    uint64_t frames = 0;
    (void)rows::parse_drop(p, n, &frames);
    /* A count read from a lost line is the count its text reads back as */
    uint64_t lost = 0;
    if (rows::parse_lost(p, n, &lost)) {
        std::string again;
        rows::append_lost_text(again, lost);
        uint64_t back = 0;
        if (!rows::parse_lost(again.data(), again.size(), &back) ||
            back != lost) {
            abort();
        }
    }

    /* A copy's line: 64 lowercase hex digits */
    std::string hex;
    if (rows::parse_named_file(p, n, rows::kConditionsName, &hex) &&
        !sha256_text(hex.c_str(), hex.size() + 1)) {
        abort();
    }

    /* A cond line: a key of 1 to 31 of a-z, 0-9 and _, known flag bits
     * alone, and two parses alike, NaN bits included */
    rows::cond_line c1;
    rows::cond_line c2;
    const bool cond = rows::parse_cond_line(p, n, &c1);
    if (cond != rows::parse_cond_line(p, n, &c2)) {
        abort();
    }
    if (cond && (!condition_key(c1.key, sizeof c1.key) ||
                 (c1.flags & ~kCondFlags) != 0 || strcmp(c1.key, c2.key) != 0 ||
                 c1.on != c2.on || c1.flags != c2.flags ||
                 !same_bits(c1.value, c2.value) || c1.term != c2.term ||
                 c1.host_us != c2.host_us || c1.t_us != c2.t_us)) {
        abort();
    }
}

/* Every line of the input, each a heap copy of its exact size, so ASan
 * sees a read past it */
void each_line(const uint8_t* data, size_t size)
{
    size_t start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i == size || data[i] == '\n') {
            const size_t n = i - start;
            std::vector<char> copy(data + start, data + start + n);
            one_line(copy.data(), n);
            start = i + 1;
        }
    }
}

} // namespace
