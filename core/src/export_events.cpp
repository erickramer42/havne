// File/layer: core/src/export_events.cpp | C++17, Qt-free; the folder export's events.log
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest,
//          ExportCancelTest, ConditionRecordingTest (test_core)

#include "export_engine.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace ecuio_export_detail {

bool recording_export::kept_caller(const std::string& text, std::string* out)
{
    if (rows::starts_with(text.data(), text.size(), "Havne ") ||
        rows::starts_with(text.data(), text.size(), "havne-cli ") ||
        rows::starts_with(text.data(), text.size(),
                          "surrogate frame logging  ")) {
        *out = text;
        return true;
    }
    if (rows::starts_with(text.data(), text.size(), "vehicle  ")) {
        *out = "vehicle  <vehicle>";
        return true;
    }
    return false;
}

bool recording_export::plan_events(const std::string& folder)
{
    if (canceled(true)) {
        return false;
    }
    std::string why;
    file_ptr f(files::open_file(folder + "/events.log", false, &why));
    if (!f) {
        return true;
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool header = true;
    bool first = true;
    while (csv::read_line(f.get(), buf, &too_long)) {
        if (canceled()) {
            return false;
        }
        if (too_long) {
            return fail("events.log holds a line longer than a row's");
        }
        const std::string line = buf.data();
        if (first) {
            first = false;
            if (line != rows::kFormat2 && line != rows::kFormat3 &&
                line != rows::kFormat4) {
                /* Another format's log: a file the export doesn't
                 * know */
                ++r_.files_left_out;
                return true;
            }
            has_events_ = true;
            format3_ = line != rows::kFormat2;
            events_head_.push_back(line);
            continue;
        }
        if (header) {
            if (line.empty()) {
                header = false;
            } else {
                head_line(line);
            }
            continue;
        }
        if (!line.empty()) {
            activity_line(line);
        }
    }
    put_held();
    put_focus_held();
    return true;
}

void recording_export::head_line(const std::string& line)
{
    const char* p = line.data();
    const size_t n = line.size();
    if (raw_) {
        events_head_.push_back(line);
        return;
    }
    static const char* const kKept[] = {
        "started  ",   "utc  ",  "core  ",   "profile  ", "frame flags  ",
        "focus  ",     "link  ", "signal  ", "label  ",   "columns  ",
        "min sweep  ", "pick  ", "trigger  "};
    for (const char* k : kKept) {
        if (rows::starts_with(p, n, k)) {
            events_head_.push_back(line);
            return;
        }
    }
    /* A copy's line only as the name and hash the readers take: any
     * other text there is unchecked */
    const std::pair<const char*, const char*> copies[] = {
        {rows::kSignalFileKey, rows::kSignalFileName},
        {rows::kConditionsKey, rows::kConditionsName}};
    for (const auto& copy : copies) {
        if (rows::starts_with(p, n, copy.first)) {
            const size_t k = std::strlen(copy.first);
            std::string hex;
            const masked_copy& c =
                std::strcmp(copy.second, rows::kSignalFileName) == 0
                    ? signals_copy_
                    : conditions_copy_;
            if (!rows::parse_named_file(p + k, n - k, copy.second, &hex)) {
                ++events_left_;
            } else if (c.masks > 0 && hex == c.source_sha) {
                /* the masked copy's own hash, so a reader takes it */
                events_head_.push_back(std::string(copy.first) + copy.second +
                                       "," + c.sha);
            } else {
                events_head_.push_back(line);
            }
            return;
        }
    }
    if (rows::starts_with(p, n, rows::kPluginKey) ||
        rows::starts_with(p, n, rows::kConversationKey) ||
        rows::starts_with(p, n, rows::kConsentKey) ||
        rows::starts_with(p, n, rows::kModuleKey)) {
        events_head_.push_back(line);
        return;
    }
    if (rows::starts_with(p, n, "uri  ")) {
        events_head_.push_back("uri  " + scrub::uri(line.substr(5)));
        return;
    }
    std::string text;
    if (rows::starts_with(p, n, "caller  ") &&
        kept_caller(line.substr(8), &text)) {
        events_head_.push_back("caller  " + text);
        return;
    }
    ++events_left_;
}

void recording_export::activity_line(const std::string& line)
{
    rows::event_line e;
    const char* text = nullptr;
    size_t text_len = 0;
    size_t kind_len = 0;
    if (!rows::parse_event_line(line.data(), line.size(), &e) ||
        !rows::split_kind(e.rest, e.rest_len, &text, &text_len,
                          &kind_len)) {
        ++events_left_;
        return;
    }
    const std::string kind(e.rest, kind_len);
    const std::string head(line.data(),
                           static_cast<size_t>(e.rest - line.data()));
    if (kind == "cond" && format3_) {
        cond_activity(line, head, text, text_len, e.ms);
        return;
    }
    if (kind == "end") {
        uint64_t records = 0;
        uint64_t notes = 0;
        range_starts();
        if (raw_) {
            events_body_.push_back(line);
            return;
        }
        if (!rows::parse_end(text, text_len, &records, &notes)) {
            ++events_left_;
            return;
        }
        /* The counts from the last ": ", the why the caller's words */
        const std::string all(text, text_len);
        const size_t at = all.rfind(": ");
        events_body_.push_back(head + "end  <why>" + all.substr(at));
        return;
    }
    /* A focus change names signals, as the header's focus line does */
    if (kind == "focus" || kind == "pick") {
        focus_activity(line, e, text, text_len);
        return;
    }
    if (!in_range(e.ms * 1000)) {
        return;
    }
    if (raw_ || kind == "link" || kind == "drop" || kind == "asleep" ||
        kind == "module" || kind == "operation") {
        range_starts();
        events_body_.push_back(line);
        return;
    }
    ++events_left_; /* a note, the caller's text, or a kind unknown */
}

void recording_export::range_starts()
{
    if (held_at_ == SIZE_MAX) {
        held_at_ = events_body_.size();
    }
}

void recording_export::cond_activity(const std::string& line, const std::string& head,
                                     const char* text, size_t text_len, uint64_t ms)
{
    rows::cond_line c;
    if (!rows::parse_cond_line(text, text_len, &c)) {
        if (!in_range(ms * 1000)) {
            return;
        }
        if (raw_) {
            range_starts();
            events_body_.push_back(line);
        } else {
            ++events_left_;
        }
        return;
    }
    if (c.host_us < o_.from_us) {
        hold_before(c, line);
        return;
    }
    if (!in_range(c.host_us)) {
        return;
    }
    range_starts();
    if (raw_) {
        events_body_.push_back(line);
        return;
    }
    std::string out = head + "cond  ";
    rows::append_cond_text(out, c);
    events_body_.push_back(out);
}

void recording_export::hold_before(const rows::cond_line& c, const std::string& line)
{
    uint32_t i = 0;
    while (i < held_keys_ && std::strcmp(held_[i].key, c.key) != 0) {
        ++i;
    }
    if (i == held_keys_) {
        if (held_keys_ == ECUIO_CONDITION_MAX_CONDITIONS) {
            return;
        }
        ++held_keys_;
    }
    held_[i] = c;
    if (!held_seen_ || c.host_us >= held_last_.host_us) {
        held_last_ = c;
        held_clock_ = line.substr(0, 12);
        held_seen_ = true;
    }
}

void recording_export::focus_activity(const std::string& line, const rows::event_line& e,
                                      const char* text, size_t text_len)
{
    if (e.ms * 1000 < o_.from_us) {
        if (!rows::starts_with(text, text_len, "more: ")) {
            focus_held_.clear();
        }
        focus_held_.emplace_back(e.rest, e.rest_len);
        focus_held_clock_ = line.substr(0, 12);
        focus_held_ms_ = e.ms;
        return;
    }
    if (!in_range(e.ms * 1000)) {
        return;
    }
    range_starts();
    events_body_.push_back(line);
}

std::string recording_export::clock_after(const std::string& clock, uint64_t ms)
{
    uint64_t h = 0, m = 0, s = 0, z = 0;
    const char* p = clock.data();
    if (clock.size() != 12 || !rows::parse_count(p, 2, &h) ||
        !rows::parse_count(p + 3, 2, &m) ||
        !rows::parse_count(p + 6, 2, &s) ||
        !rows::parse_count(p + 9, 3, &z)) {
        return clock;
    }
    constexpr uint64_t kDayMs = 86400000;
    const uint64_t at =
        ((h * 3600 + m * 60 + s) * 1000 + z + ms % kDayMs) % kDayMs;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%03llu",
                  static_cast<unsigned long long>(at / 3600000),
                  static_cast<unsigned long long>(at / 60000 % 60),
                  static_cast<unsigned long long>(at / 1000 % 60),
                  static_cast<unsigned long long>(at % 1000));
    return buf;
}

void recording_export::put_held()
{
    if (o_.from_us == 0 || !held_seen_) {
        return;
    }
    const uint64_t from = o_.from_us;
    const uint64_t step = from - held_last_.host_us;
    const uint64_t ms = from / 1000 + (from % 1000 >= 500 ? 1 : 0);
    char seconds[48];
    std::snprintf(seconds, sizeof seconds, "  +%llu.%03llu  cond  ",
                  static_cast<unsigned long long>(ms / 1000),
                  static_cast<unsigned long long>(ms % 1000));
    const std::string head =
        clock_after(held_clock_, from / 1000 - held_last_.host_us / 1000) +
        seconds;
    std::vector<std::string> lines;
    for (uint32_t i = 0; i < held_keys_; ++i) {
        rows::cond_line c = held_[i];
        if (!c.on) {
            continue;
        }
        c.flags = (c.flags & rows::kCondStale) | rows::kCondHeld;
        c.host_us = from;
        c.t_us = UINT64_MAX - held_last_.t_us < step
                     ? UINT64_MAX
                     : held_last_.t_us + step;
        std::string out = head;
        rows::append_cond_text(out, c);
        lines.push_back(out);
    }
    const size_t at = held_at_ == SIZE_MAX ? events_body_.size() : held_at_;
    events_body_.insert(events_body_.begin() + static_cast<ptrdiff_t>(at),
                        lines.begin(), lines.end());
}

void recording_export::put_focus_held()
{
    if (o_.from_us == 0 || focus_held_.empty()) {
        return;
    }
    const uint64_t from = o_.from_us;
    const uint64_t ms = from / 1000 + (from % 1000 >= 500 ? 1 : 0);
    char seconds[48];
    std::snprintf(seconds, sizeof seconds, "  +%llu.%03llu  ",
                  static_cast<unsigned long long>(ms / 1000),
                  static_cast<unsigned long long>(ms % 1000));
    const std::string head =
        clock_after(focus_held_clock_, from / 1000 - focus_held_ms_) +
        seconds;
    std::vector<std::string> lines;
    lines.reserve(focus_held_.size());
    for (const std::string& rest : focus_held_) {
        lines.push_back(head + rest);
    }
    const size_t at = held_at_ == SIZE_MAX ? events_body_.size() : held_at_;
    events_body_.insert(events_body_.begin() + static_cast<ptrdiff_t>(at),
                        lines.begin(), lines.end());
}

bool recording_export::plan_rows(const std::string& folder, const char* name, bool markers,
                                 std::vector<std::string>& out, bool* present)
{
    if (canceled(true)) {
        return false;
    }
    std::string why;
    file_ptr f(files::open_file(folder + "/" + name, false, &why));
    *present = static_cast<bool>(f);
    if (!f) {
        return true;
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool torn = false;
    uint64_t line = 0;
    std::string field;
    while (csv::read_line(f.get(), buf, &too_long, &torn) && !torn) {
        if (canceled()) {
            return false;
        }
        ++line;
        if (line == 1) {
            out.emplace_back(buf.data());
            continue;
        }
        const size_t n = std::strlen(buf.data());
        uint64_t host_us = 0;
        const char* bad = "the row is too long";
        bool ok = !too_long;
        if (ok && markers) {
            ok = rows::parse_marker_row(buf.data(), n, &host_us, field,
                                        &bad);
        } else if (ok) {
            /* t_s, seconds to the ms, then the counts */
            const char* comma =
                static_cast<const char*>(std::memchr(buf.data(), ',', n));
            double t = 0.0;
            ok = comma != nullptr &&
                 rows::parse_number(buf.data(),
                                    static_cast<size_t>(comma - buf.data()),
                                    &t) &&
                 t >= 0.0 && t < 1e12;
            bad = "t_s isn't a time in seconds";
            host_us = static_cast<uint64_t>(std::llround(t * 1e6));
        }
        if (!ok) {
            ecuio_detail_set("%s line %llu: %s", name,
                             static_cast<unsigned long long>(line), bad);
            return false;
        }
        if (in_range(host_us)) {
            out.emplace_back(buf.data());
        }
    }
    return true;
}

} // namespace ecuio_export_detail
