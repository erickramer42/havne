// File/layer: core/src/export_csv.cpp | C++17, Qt-free; the long CSV export
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)

#include "export_engine.h"

#include "json.h" /* quoted */

#include <cstring>

namespace ecuio_export_detail {

bool recording_export::formula_start(const std::string& s)
{
    return !s.empty() && std::strchr("=+-@\t\r", s[0]) != nullptr;
}

bool recording_export::values(const std::string& folder, bool write)
{
    if (canceled(true)) {
        return false;
    }
    std::string why;
    file_ptr f(files::open_file(folder + "/signals.csv", false, &why));
    if (!f) {
        return fail("the folder has no signals.csv");
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool torn = false;
    uint64_t line = 0;
    rows::signal_row row;
    std::unordered_set<std::string> signals;
    while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
        if (canceled()) {
            return false;
        }
        ++line;
        if (line == 1) {
            if (write && folder_) {
                buf_ += buf.data(); /* the header, its BOM with it */
                buf_ += '\n';
            }
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
            r_.left_out[kIdentification] += write ? 0u : 1u;
            continue;
        }
        double v = 0.0;
        const bool number =
            rows::parse_number(row.value.data(), row.value.size(), &v);
        if (!raw_ && (!number || text_names_.count(row.name) != 0)) {
            r_.left_out[kTextValues] += write ? 0u : 1u;
            continue;
        }
        if (!write && !check_formula(row, number)) {
            return false;
        }
        if (!write) {
            ++r_.values;
            signals.insert(row.name);
            first_us_ = r_.values == 1 ? row.host_us : first_us_;
            last_us_ = row.host_us;
            continue;
        }
        if (folder_) {
            buf_ += buf.data();
            buf_ += '\n';
        } else {
            put_row(row);
        }
        ++written_;
        if (!flush(false)) {
            return false;
        }
    }
    if (!write) {
        r_.signal_count = signals.size();
    }
    return true;
}

bool recording_export::check_formula(const rows::signal_row& row, bool number) const
{
    const std::string& name = header_.shown(row.name);
    const char* what = nullptr;
    if (formula_start(name)) {
        what = "name";
    } else if (formula_start(row.name)) {
        what = "key";
    } else if (formula_start(row.unit)) {
        what = "unit";
    } else if (!number && formula_start(row.value)) {
        what = "text value";
    }
    if (what == nullptr) {
        return true;
    }
    ecuio_detail_set("the signal %s's %s starts as a spreadsheet formula "
                     "does (=, +, -, @, a tab or a carriage return): "
                     "nothing was written",
                     ecuio_json::quoted(name).c_str(), what);
    return false;
}

void recording_export::put_field(const std::string& s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos) {
        buf_ += s;
        return;
    }
    buf_ += '"';
    for (char c : s) {
        buf_ += c;
        if (c == '"') {
            buf_ += '"';
        }
    }
    buf_ += '"';
}

std::string recording_export::seconds(uint64_t us)
{
    char b[32];
    std::snprintf(b, sizeof b, "%llu.%06llu",
                  static_cast<unsigned long long>(us / 1000000u),
                  static_cast<unsigned long long>(us % 1000000u));
    return b;
}

void recording_export::put_row(const rows::signal_row& row)
{
    buf_ += seconds(row.host_us);
    buf_ += ',';
    put_field(header_.shown(row.name));
    buf_ += ',';
    put_field(row.unit);
    buf_ += ',';
    put_field(row.value);
    if (header_.keyed) {
        /* Two signals of one name are told apart by this column */
        buf_ += ',';
        put_field(row.name);
    }
    buf_ += '\n';
}

std::string recording_export::csv_comment() const
{
    const std::string span = r_.values == 0
                                 ? std::string()
                                 : ", " + seconds(first_us_) + " s to " +
                                       seconds(last_us_) + " s";
    char buf[384];
    std::snprintf(
        buf, sizeof buf,
        "# Havne %s, %s (ADR-0054): %llu values of %llu signals%s; left "
        "out: %llu values of identification signals, %llu of text "
        "signals\n",
        ecuio_version_string(),
        raw_ ? "a raw export, not scrubbed" : "a scrubbed export",
        static_cast<unsigned long long>(r_.values),
        static_cast<unsigned long long>(r_.signal_count), span.c_str(),
        static_cast<unsigned long long>(r_.left_out[kIdentification]),
        static_cast<unsigned long long>(r_.left_out[kTextValues]));
    return buf;
}

bool recording_export::write_csv(const std::string& folder)
{
    file_check_.reset(new scrub::check(values_));
    buf_ = csv_comment();
    buf_ += header_.keyed ? "time_s,signal,unit,value,key\n"
                          : "time_s,signal,unit,value\n";
    if (!values(folder, true) || !flush(true)) {
        return false;
    }
    if (written_ != r_.values) {
        return fail("the recording changed while it was exported");
    }
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
