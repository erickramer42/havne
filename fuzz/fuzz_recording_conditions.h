// File/layer: fuzz/fuzz_recording_conditions.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (the conditions the reader should give)

#pragma once

#include "fuzz_recording_common.h"

namespace {

struct opened {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_recording_info_t info{};
    ecuio_recording_conditions_t conditions{};
    std::vector<ecuio_recording_condition_event_t> events;
};

/* What the reader should give of a recording's conditions, worked out from
 * the input apart from it: events.log's lines split as the reader splits
 * them (a line past the longest row keeps its head), format 3's header
 * conditions line, each activity line of kind cond through the row
 * parsers into a table of a set's most keys, and conditions.json's own
 * hash */
struct wanted {
    uint32_t flags = 0;
    uint64_t length = 0;
    std::string sha256;
    uint64_t events = 0;
    uint32_t skipped = 0;
    std::vector<rows::cond_line> kept;
};
wanted g_want;

/* A line's cond text: its time read and its kind cond */
bool cond_text(const std::string& l, const char** text, size_t* text_len)
{
    rows::event_line e;
    size_t kind_len = 0;
    return rows::parse_event_line(l.data(), l.size(), &e) &&
           rows::split_kind(e.rest, e.rest_len, text, text_len, &kind_len) &&
           rows::equals(e.rest, kind_len, "cond");
}

wanted want_conditions(const char* log, size_t log_len, bool log_present,
                       const char* json, size_t json_len, bool json_present)
{
    wanted w;
    if (json_present) {
        w.flags |= ECUIO_RECORDING_CONDITIONS_PRESENT;
        w.length = json_len;
    }
    bool format3 = false;
    bool in_header = false;
    std::vector<std::string> keys;
    size_t at = 0;
    uint64_t line_no = 0;
    while (log_present && at < log_len) {
        const auto* nl =
            static_cast<const char*>(memchr(log + at, '\n', log_len - at));
        const size_t end = nl != nullptr ? static_cast<size_t>(nl - log) : log_len;
        std::string l(log + at, end - at);
        at = nl != nullptr ? end + 1 : log_len;
        ++line_no;
        const bool too_long = l.size() > rows::kRowMax + 1;
        if (too_long) {
            l.resize(rows::kRowMax + 1);
        }
        if (nl != nullptr && !l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        const char* text = nullptr;
        size_t text_len = 0;
        if (nl == nullptr || too_long) {
            if (line_no > 1 && format3 && !in_header &&
                cond_text(l, &text, &text_len)) {
                ++w.skipped;
            }
            continue;
        }
        if (line_no == 1) {
            const size_t bom = rows::bom_length(l.data(), l.size());
            format3 =
                ecuio_recording_keys::has_enums(ecuio_recording_keys::format_of(
                    l.data() + bom, l.size() - bom));
            in_header = format3;
            if (format3) {
                continue;
            }
        }
        if (!format3) {
            break;
        }
        if (in_header) {
            std::string hex;
            const size_t key = strlen(rows::kConditionsKey);
            if (l.empty()) {
                in_header = false;
            } else if (rows::starts_with(l.data(), l.size(),
                                         rows::kConditionsKey) &&
                       rows::parse_named_file(l.data() + key, l.size() - key,
                                              rows::kConditionsName, &hex)) {
                w.sha256 = hex;
                w.flags |= ECUIO_RECORDING_CONDITIONS_NAMED;
            }
            continue;
        }
        if (!cond_text(l, &text, &text_len)) {
            continue;
        }
        rows::cond_line c;
        if (!rows::parse_cond_line(text, text_len, &c)) {
            ++w.skipped;
            continue;
        }
        if (std::find(keys.begin(), keys.end(), c.key) == keys.end()) {
            if (keys.size() == ECUIO_CONDITION_MAX_CONDITIONS) {
                ++w.skipped;
                continue;
            }
            keys.emplace_back(c.key);
        }
        if (++w.events <= ECUIO_RECORDING_MAX_CONDITION_EVENTS) {
            w.kept.push_back(c);
        }
    }
    if ((w.flags & ECUIO_RECORDING_CONDITIONS_NAMED) != 0 && json_present &&
        json_len <= ECUIO_CONDITION_FILE_MAX_BYTES &&
        ecuio_sha256::hex_of(json, json_len) == w.sha256) {
        w.flags |= ECUIO_RECORDING_CONDITIONS_MATCH;
    }
    return w;
}

/* The copy and the cond lines (ADR-0063 section 8): flags and hash in
 * their forms, each event kept in its fields' forms with zero reserved
 * words, as many as the count up to the cap, and none past them; and
 * every answer the one the input gives (g_want) */
void read_conditions(const ecuio_recording_t* r, opened& o)
{
    ecuio_recording_conditions_t& c = o.conditions;
    memset(&c, 0xA5, sizeof c);
    if (ecuio_recording_get_conditions(r, &c) != ECUIO_OK) {
        abort();
    }
    const uint32_t named = ECUIO_RECORDING_CONDITIONS_NAMED;
    const uint32_t present = ECUIO_RECORDING_CONDITIONS_PRESENT;
    const uint32_t match = ECUIO_RECORDING_CONDITIONS_MATCH;
    if ((c.flags & ~(named | present | match)) != 0 ||
        ((c.flags & match) != 0 &&
         (c.flags & (named | present)) != (named | present)) ||
        ((c.flags & present) == 0 && c.length != 0) ||
        !sha256_text(c.sha256, sizeof c.sha256) ||
        ((c.flags & named) != 0) != (c.sha256[0] != '\0')) {
        abort();
    }
    for (uint64_t w : c.reserved) {
        if (w != 0) {
            abort();
        }
    }
    const uint64_t count = o.info.condition_events;
    const bool cut = count > ECUIO_RECORDING_MAX_CONDITION_EVENTS;
    if (cut != ((o.info.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT) != 0)) {
        abort();
    }
    const uint64_t kept = cut ? ECUIO_RECORDING_MAX_CONDITION_EVENTS : count;
    o.events.resize(static_cast<size_t>(kept));
    for (uint64_t i = 0; i < kept; ++i) {
        ecuio_recording_condition_event_t& e = o.events[i];
        memset(&e, 0xA5, sizeof e);
        if (ecuio_recording_get_condition_event(r, i, &e) != ECUIO_OK ||
            !condition_key(e.key, sizeof e.key) ||
            e.state > ECUIO_CONDITION_ON || (e.flags & ~kCondFlags) != 0 ||
            e.reserved0 != 0 || e.reserved[0] != 0 || e.reserved[1] != 0) {
            abort();
        }
    }
    const wanted& w = g_want;
    if (c.flags != w.flags || c.length != w.length || w.sha256 != c.sha256 ||
        c.lines_skipped != w.skipped || count != w.events ||
        o.events.size() != w.kept.size()) {
        abort();
    }
    for (size_t i = 0; i < w.kept.size(); ++i) {
        const ecuio_recording_condition_event_t& e = o.events[i];
        const rows::cond_line& k = w.kept[i];
        if (strcmp(e.key, k.key) != 0 ||
            e.state != (k.on ? ECUIO_CONDITION_ON : ECUIO_CONDITION_OFF) ||
            e.flags != k.flags || !same_bits(e.value, k.value) ||
            e.term != k.term || e.host_us != k.host_us ||
            e.timestamp_us != k.t_us) {
            abort();
        }
    }
    ecuio_recording_condition_event_t past;
    memset(&past, 0x5A, sizeof past);
    const ecuio_recording_condition_event_t untouched = past;
    if (ecuio_recording_get_condition_event(r, kept, &past) !=
            ECUIO_ERR_INVALID_ARGUMENT ||
        memcmp(&past, &untouched, sizeof past) != 0) {
        abort();
    }
}

bool same_conditions(const opened& a, const opened& b)
{
    return memcmp(&a.conditions, &b.conditions, sizeof a.conditions) == 0 &&
           a.events.size() == b.events.size() &&
           (a.events.empty() ||
            memcmp(a.events.data(), b.events.data(),
                   a.events.size() * sizeof a.events[0]) == 0);
}

} // namespace
