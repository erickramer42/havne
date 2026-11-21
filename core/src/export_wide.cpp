// File/layer: core/src/export_wide.cpp | C++17, Qt-free; the wide CSV export
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)

#include "export_engine.h"

#include "json.h" /* quoted */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ecuio_export_detail {

namespace {

/* A number to d decimal places (0 to 3) with a '.', whatever the
 * locale: rounded half away from zero, no "-0.0"; not finite, "" (no
 * cell). At 9e15 or more scaled, %.12g as signals.csv writes it, so a cell
 * is never empty for being large */
std::string fixed_text(double v, int d)
{
    static const double kPow10[] = {1.0, 10.0, 100.0, 1000.0};
    if (!std::isfinite(v) || d < 0 || d > 3) {
        return std::string();
    }
    const double scaled = std::round(v * kPow10[d]);
    if (std::fabs(scaled) >= 9.0e15) {
        std::string as_recorded;
        rows::append_value(as_recorded, v);
        return as_recorded;
    }
    const long long n = static_cast<long long>(scaled);
    const unsigned long long a = n < 0
                                     ? 0ull - static_cast<unsigned long long>(n)
                                     : static_cast<unsigned long long>(n);
    std::string digits = std::to_string(a);
    if (d > 0) {
        while (digits.size() < static_cast<size_t>(d) + 1) {
            digits.insert(digits.begin(), '0');
        }
        digits.insert(digits.size() - static_cast<size_t>(d), 1, '.');
    }
    return n < 0 ? "-" + digits : digits;
}

/* The digits after a recorded value's point, capped at 3; an exponent
 * form counts as 3 */
uint8_t decimals_of(const std::string& value)
{
    if (value.find_first_of("eE") != std::string::npos) {
        return 3;
    }
    const size_t dot = value.find('.');
    if (dot == std::string::npos) {
        return 0;
    }
    const size_t after = value.size() - dot - 1;
    return static_cast<uint8_t>(after > 3 ? 3 : after);
}

} // namespace

bool recording_export::too_many_columns()
{
    ecuio_detail_set("the wide CSV is limited to %u columns: pick the "
                     "columns to keep",
                     static_cast<unsigned>(kWideColumnsMax));
    return false;
}

size_t recording_export::add_column(const std::string& name)
{
    wide_column c;
    c.name = name;
    c.shown = name;
    for (size_t k = 0; k < header_.signals.size(); ++k) {
        if (header_.ids[k] == name) {
            const rows::header_signal& hs = header_.signals[k];
            c.in_header = true;
            c.shown = hs.name;
            c.unit = hs.unit;
            c.decimals = hs.decimal_places;
            break;
        }
    }
    columns_.push_back(c);
    index_[name] = columns_.size() - 1;
    return columns_.size() - 1;
}

bool recording_export::check_column(const wide_column& c)
{
    rows::signal_row row;
    row.name = c.name;
    row.unit = c.unit;
    row.value = "0";
    return check_formula(row, true);
}

std::string recording_export::csv_field(const std::string& s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (char ch : s) {
        out += ch;
        if (ch == '"') {
            out += '"';
        }
    }
    out += '"';
    return out;
}

bool recording_export::wide_survey(const std::string& folder)
{
    if (o_.column_count > kWideColumnsMax) {
        return too_many_columns();
    }
    std::vector<std::string> picked;
    picked.reserve(o_.column_count);
    for (uint32_t i = 0; i < o_.column_count; ++i) {
        std::string id;
        if (!rs::column_id(header_, o_.columns_utf8[i], &id)) {
            /* a name several signals have: the find's words stand */
            status_ = ECUIO_ERR_INVALID_ARGUMENT;
            return false;
        }
        picked.push_back(std::move(id));
    }
    /* The header's focus line is the recording's, so a pick it can't
     * give is a bad recording (LOAD_FAILED), not the caller's */
    const bool from_header = picked.empty();
    if (from_header) {
        picked = header_.focus;
    }
    const auto reject = [this, from_header](const char* fmt,
                                            const std::string& a) {
        return from_header ? fail(fmt, a) : refuse(fmt, a);
    };
    fixed_columns_ = !picked.empty();
    for (const std::string& name : picked) {
        if (index_.count(name) != 0) {
            return reject("the column %s is named twice",
                          ecuio_json::quoted(header_.shown(name)));
        }
        if (!raw_ && text_names_.count(name) != 0) {
            return reject("the signal %s is a text signal, never a column",
                          ecuio_json::quoted(header_.shown(name)));
        }
        if (!raw_ && ident_names_.count(name) != 0) {
            return reject("the signal %s is an identification signal, "
                          "never a column",
                          ecuio_json::quoted(header_.shown(name)));
        }
        add_column(name);
    }
    if (!fixed_columns_) {
        /* The header's number signals first, in its order */
        for (size_t k = 0; k < header_.signals.size(); ++k) {
            const std::string& id = header_.ids[k];
            if (!header_.signals[k].text && index_.count(id) == 0 &&
                (raw_ || (text_names_.count(id) == 0 &&
                          ident_names_.count(id) == 0))) {
                if (columns_.size() >= kWideColumnsMax) {
                    return too_many_columns();
                }
                add_column(id);
            }
        }
    }
    if (!wide_walk(folder, true, nullptr)) {
        return false;
    }
    if (!fixed_columns_) {
        /* A header signal with no sample in range isn't a column */
        std::vector<wide_column> kept;
        index_.clear();
        for (const wide_column& c : columns_) {
            if (c.samples > 0) {
                index_[c.name] = kept.size();
                kept.push_back(c);
            }
        }
        columns_.swap(kept);
    }
    for (wide_column& c : columns_) {
        if (!check_column(c)) {
            return false;
        }
        c.usual_us = c.deltas.count > 0 ? c.deltas.quantile(0.5) : 0;
        c.silence_us = (std::max)(kSilenceUs, kSilenceTimes * c.usual_us);
        std::string unit = c.unit;
        ecuio_display_value_t dv{};
        if (ecuio_display_convert(c.unit.c_str(), c.decimals, 0.0, o_.units,
                                  &dv) == ECUIO_DISPLAY_OK) {
            unit = dv.unit;
        }
        c.head = unit.empty() ? c.shown : c.shown + " (" + unit + ")";
    }
    /* A head cell that two columns share gains each one's key, so a
     * spreadsheet tells them apart; a head cell of its own stays bare */
    std::unordered_map<std::string, uint32_t> heads;
    for (const wide_column& c : columns_) {
        ++heads[c.head];
    }
    for (wide_column& c : columns_) {
        const bool shared = header_.keyed && heads[c.head] > 1;
        c.head = csv_field(shared ? c.head + " [" + c.name + "]" : c.head);
    }
    mark_sparse();
    r_.signal_count = columns_.size();
    return true;
}

void recording_export::mark_sparse()
{
    std::vector<uint64_t> all;
    for (const wide_column& c : columns_) {
        if (c.usual_us > 0) {
            all.push_back(c.usual_us);
        }
    }
    if (all.empty()) {
        return;
    }
    std::sort(all.begin(), all.end());
    /* For a column in all, the rest's lower median is the entry at
     * (n - 2) / 2 when the column is above it (its removal leaves it in
     * place); one at or below it is never over the rest's, which is at
     * least as large. A one-sample column's rest is all */
    const uint64_t in_all = all.size() >= 2 ? all[(all.size() - 2) / 2] : 0;
    const uint64_t beside = all[(all.size() - 1) / 2];
    const uint64_t span = last_us_ - first_us_;
    for (wide_column& c : columns_) {
        if (c.samples == 1) {
            c.sparse = span > rows::kSparseTimes * beside;
        } else if (c.usual_us > 0 && all.size() >= 2) {
            c.sparse = c.usual_us > rows::kSparseTimes * in_all;
        }
    }
}

std::string recording_export::wide_head() const
{
    std::string h = kUtf8Bom; /* Excel reads a CSV without one as ANSI */
    h += "Time (ms)";
    for (const wide_column& c : columns_) {
        h += ',';
        h += c.head;
    }
    h += "\r\n";
    return h;
}

std::string recording_export::cell_text(const wide_column& c, double v) const
{
    ecuio_display_value_t dv{};
    if (ecuio_display_convert(c.unit.c_str(), c.decimals, v, o_.units,
                              &dv) != ECUIO_DISPLAY_OK) {
        return fixed_text(v, c.decimals);
    }
    return fixed_text(dv.value, dv.decimal_places);
}

bool recording_export::wide_walk(const std::string& folder, bool survey,
                                 std::vector<wide_rows>* engines)
{
    if (canceled(true)) {
        return false;
    }
    std::string why;
    file_ptr f(files::open_file(folder + "/signals.csv", false, &why));
    if (!f) {
        return fail("the folder has no signals.csv");
    }
    if (!survey) {
        for (wide_rows& e : *engines) {
            e.start(columns_, first_us_);
        }
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool torn = false;
    uint64_t line = 0;
    rows::signal_row row;
    std::string text;
    while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
        if (canceled()) {
            return false;
        }
        ++line;
        if (line == 1) {
            continue;
        }
        const char* bad = nullptr;
        if (too_long ||
            !rows::parse_signal_row(buf.data(), std::strlen(buf.data()),
                                    row, &bad)) {
            ecuio_detail_set("signals.csv line %llu: %s",
                             static_cast<unsigned long long>(line),
                             too_long ? "the row is too long" : bad);
            return false;
        }
        if (!in_range(row.host_us)) {
            continue;
        }
        if (!raw_ && ident_names_.count(row.name) != 0) {
            r_.left_out[kIdentification] += survey ? 1u : 0u;
            continue;
        }
        double v = 0.0;
        const bool number =
            rows::parse_number(row.value.data(), row.value.size(), &v);
        if (!number || (!raw_ && text_names_.count(row.name) != 0)) {
            r_.left_out[kTextValues] += survey ? 1u : 0u;
            continue;
        }
        auto it = index_.find(row.name);
        if (it == index_.end()) {
            if (fixed_columns_ || !survey) {
                continue;
            }
            if (columns_.size() >= kWideColumnsMax) {
                return too_many_columns();
            }
            add_column(row.name);
            it = index_.find(row.name);
        }
        wide_column& c = columns_[it->second];
        if (survey) {
            if (c.samples == 0 && c.unit.empty()) {
                c.unit = row.unit;
            }
            if (c.samples > 0 && row.host_us >= c.last_us) {
                c.deltas.add(static_cast<int64_t>(row.host_us - c.last_us));
            }
            if (!c.in_header) {
                c.decimals = (std::max)(c.decimals, decimals_of(row.value));
            }
            c.last_us = row.host_us;
            ++c.samples;
            first_us_ = surveyed_ == 0 ? row.host_us : first_us_;
            last_us_ = row.host_us;
            ++surveyed_;
            continue;
        }
        ++seen_;
        text = cell_text(c, v);
        for (wide_rows& e : *engines) {
            e.sample(it->second, row.host_us, text, columns_);
            if (e.sink != nullptr && !flush(false)) {
                return false;
            }
        }
    }
    if (!survey) {
        for (wide_rows& e : *engines) {
            e.finish(last_us_, columns_);
        }
    }
    return true;
}

bool recording_export::write_wide(const std::string& folder)
{
    file_check_.reset(new scrub::check(values_));
    buf_ = wide_head();
    std::vector<wide_rows> engines(1);
    engines[0].row_ms = o_.row_ms;
    engines[0].sink = &buf_;
    if (!wide_walk(folder, false, &engines) || !flush(true)) {
        return false;
    }
    if (seen_ != surveyed_) {
        return fail("the recording changed while it was exported");
    }
    r_.values = engines[0].cells;
    r_.rows = engines[0].rows;
    if (!raw_) {
        const uint32_t found = file_check_->finish();
        if (found != 0) {
            r_.failed_class = found;
            return fail("the check found %s in the export's file: nothing "
                        "was written",
                        scrub::failed_words(found));
        }
    }
    return true;
}

} // namespace ecuio_export_detail
