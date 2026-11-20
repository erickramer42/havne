// File/layer: fuzz/fuzz_recording.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (rows, a folder, the stretches' rules)

#include "fuzz_recording_common.h"
#include "fuzz_recording_lines.h"

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

/* The closed span over a signal's every sample (ecuio_recording_stats):
 * the open's count, and for a number its range, with the extremes' times
 * among its samples'; a text signal has its count alone */
void whole_stats(const ecuio_recording_t* r, uint32_t k,
                 const ecuio_recording_signal_t& s)
{
    ecuio_recording_stats_t st;
    memset(&st, 0xA5, sizeof st);
    if (ecuio_recording_stats(r, &k, 1, 0, s.last_us + 1, &st) != ECUIO_OK ||
        st.samples != s.samples || st.flags != 0 || st.reserved0 != 0 ||
        st.reserved[0] != 0 || st.reserved[1] != 0 || st.reserved[2] != 0) {
        abort();
    }
    if ((s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
        if (!std::isnan(st.lowest) || !std::isnan(st.highest) ||
            !std::isnan(st.mean) || st.lowest_us != ECUIO_RECORDING_NONE ||
            st.highest_us != ECUIO_RECORDING_NONE) {
            abort();
        }
        return;
    }
    /* With no value but NaNs the open's range reads 0 and the stats' NaN */
    const double lowest = std::isnan(st.lowest) ? 0.0 : st.lowest;
    const double highest = std::isnan(st.highest) ? 0.0 : st.highest;
    if (lowest != s.lowest || highest != s.highest) {
        abort();
    }
    for (uint64_t at : {st.lowest_us, st.highest_us}) {
        if (at != ECUIO_RECORDING_NONE && (at < s.first_us || at > s.last_us)) {
            abort();
        }
    }
}

/* A step at a time (ecuio_recording_step): forward from the start each
 * step lands later, among the signal's times, the first on its first
 * sample and the last on its last, a time per sample at most; backward
 * from past the end is its last sample, and before its first is none */
void steps_through(const ecuio_recording_t* r, uint32_t k,
                   const ecuio_recording_signal_t& s)
{
    constexpr uint32_t kMostSteps = 512;
    uint64_t t = 0;
    uint64_t at = 0;
    uint32_t steps = 0;
    bool ended = false;
    while (steps < kMostSteps) {
        if (ecuio_recording_step(r, k, t, ECUIO_RECORDING_STEP_FORWARD, &at) !=
            ECUIO_OK) {
            abort();
        }
        if (at == ECUIO_RECORDING_NONE) {
            ended = true;
            break;
        }
        /* A sample at 0 is the start itself, which no step lands on */
        if (at <= t || at < s.first_us || at > s.last_us ||
            (steps == 0 && s.first_us > 0 && at != s.first_us)) {
            abort();
        }
        t = at;
        ++steps;
    }
    if (ended && (steps > s.samples || t != s.last_us)) {
        abort();
    }
    if (ecuio_recording_step(r, k, s.last_us + 1, ECUIO_RECORDING_STEP_BACKWARD,
                             &at) != ECUIO_OK ||
        at != s.last_us ||
        ecuio_recording_step(r, k, s.first_us, ECUIO_RECORDING_STEP_BACKWARD,
                             &at) != ECUIO_OK ||
        at != ECUIO_RECORDING_NONE) {
        abort();
    }
}

/* The navigation aids over the whole recording: a role's signal is a
 * number of the recording's or none; the stretches by core's rules lie
 * in the recording, in time order, of a known kind and flags, on a
 * signal of its own, their reserved words zero; the count is the same
 * asked for alone and with room, and nothing past it is written */
void aids(const ecuio_recording_t* r, const ecuio_recording_info_t& info)
{
    for (uint32_t role = ECUIO_RECORDING_ROLE_ENGINE_SPEED - 1;
         role <= ECUIO_RECORDING_ROLE_ENGINE_LOAD + 1; ++role) {
        uint32_t index = 0;
        const ecuio_status_t rc = ecuio_recording_role_signal(r, role, &index);
        if (role < ECUIO_RECORDING_ROLE_ENGINE_SPEED ||
            role > ECUIO_RECORDING_ROLE_ENGINE_LOAD) {
            if (rc != ECUIO_ERR_INVALID_ARGUMENT) {
                abort();
            }
            continue;
        }
        if (rc != ECUIO_OK) {
            abort();
        }
        if (index != UINT32_MAX) {
            ecuio_recording_signal_t s{};
            if (index >= info.signal_count ||
                ecuio_recording_get_signal(r, index, &s) != ECUIO_OK ||
                (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                abort();
            }
        }
    }
    ecuio_stretch_rules_t rules{};
    rules.kinds = ECUIO_STRETCH_PULLS | ECUIO_STRETCH_CRUISES;
    rules.throttle = rules.engine_load = UINT32_MAX;
    rules.vehicle_speed = rules.engine_speed = UINT32_MAX;
    uint32_t count = UINT32_MAX;
    if (info.duration_us == 0) {
        /* No span: the range is refused, whatever the folder holds */
        if (ecuio_recording_stretches(r, &rules, 0, 0, nullptr, 0, &count) !=
            ECUIO_ERR_INVALID_ARGUMENT) {
            abort();
        }
        return;
    }
    if (ecuio_recording_stretches(r, &rules, 0, info.duration_us, nullptr, 0,
                                  &count) != ECUIO_OK ||
        count > 1000000) {
        abort();
    }
    constexpr uint32_t kUntouched = 0xA5A5A5A5u;
    std::vector<ecuio_recording_stretch_t> out(count + 1);
    out[count].kind = kUntouched;
    uint32_t again = UINT32_MAX;
    if (ecuio_recording_stretches(r, &rules, 0, info.duration_us, out.data(),
                                  count + 1, &again) != ECUIO_OK ||
        again != count || out[count].kind != kUntouched) {
        abort();
    }
    constexpr uint32_t kFlags = ECUIO_RECORDING_STRETCH_BY_LOAD |
                                ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED |
                                ECUIO_RECORDING_STRETCH_NO_THROTTLE |
                                ECUIO_RECORDING_STRETCH_CUT_START |
                                ECUIO_RECORDING_STRETCH_CUT_END;
    uint64_t before = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_recording_stretch_t& x = out[i];
        if ((x.kind != ECUIO_RECORDING_STRETCH_PULL &&
             x.kind != ECUIO_RECORDING_STRETCH_CRUISE) ||
            x.signal >= info.signal_count || x.from_us > x.to_us ||
            x.to_us > info.duration_us || x.from_us < before ||
            (x.flags & ~kFlags) != 0 || x.reserved0 != 0 ||
            x.reserved[0] != 0 || x.reserved[1] != 0) {
            abort();
        }
        before = x.from_us;
    }
}

/* The folder through signals.csv (set NULL), or through its frames.csv
 * decoded with set (ecuio_recording_open_frames: ISO-TP on the wire
 * frames, the marks a second is read again from) */
opened open_and_query(const ecuio_signal_set_t* set)
{
    opened o;
    ecuio_recording_t* r = nullptr;
    o.rc = set != nullptr
               ? ecuio_recording_open_frames(folder().c_str(), set, nullptr,
                                             nullptr, &r)
               : ecuio_recording_open(folder().c_str(), nullptr, nullptr, &r);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr) {
        abort();
    }
    o.detail = detail;
    if (o.rc != ECUIO_OK) {
        if (r != nullptr || o.rc != ECUIO_ERR_LOAD_FAILED) {
            abort();
        }
        return o;
    }
    if (r == nullptr || ecuio_recording_get_info(r, &o.info) != ECUIO_OK ||
        ((o.info.flags & ECUIO_RECORDING_INFO_FRAMES) != 0) !=
            (set != nullptr)) {
        abort();
    }
    read_conditions(r, o);
    for (uint32_t k = 0; k < o.info.signal_count; ++k) {
        ecuio_recording_signal_t s{};
        if (ecuio_recording_get_signal(r, k, &s) != ECUIO_OK ||
            memchr(s.name, '\0', sizeof s.name) == nullptr ||
            memchr(s.unit, '\0', sizeof s.unit) == nullptr ||
            s.decimal_places > 3) {
            abort();
        }
        /* A key only where the recording keeps one: terminated, zero
         * after it, and the find gives this signal for it */
        const size_t key_len = strnlen(s.key, sizeof s.key);
        const bool keyed = o.info.format == ECUIO_RECORDING_FORMAT_KEYS ||
                           (o.info.flags & ECUIO_RECORDING_INFO_FRAMES) != 0;
        if (key_len == sizeof s.key || (key_len > 0) != keyed) {
            abort();
        }
        for (size_t z = key_len; z < sizeof s.key; ++z) {
            if (s.key[z] != '\0') {
                abort();
            }
        }
        uint32_t found = UINT32_MAX;
        if (keyed &&
            (ecuio_recording_find_signal(r, s.key, &found) != ECUIO_OK ||
             found != k)) {
            abort();
        }
        /* Labels only on an enum, at most a def's, raw ascending, each
         * terminated and found again by its raw value */
        uint32_t nl = UINT32_MAX;
        static ecuio_enum_entry_t labels[ECUIO_ENUM_MAX_ENTRIES + 1];
        if (ecuio_recording_get_labels(r, k, labels, ECUIO_ENUM_MAX_ENTRIES + 1,
                                       &nl) != ECUIO_OK ||
            nl > ECUIO_ENUM_MAX_ENTRIES ||
            (nl > 0 && (s.flags & ECUIO_RECORDING_SIGNAL_ENUM) == 0) ||
            (s.flags & ECUIO_RECORDING_SIGNAL_ENUM &&
             s.flags & ECUIO_RECORDING_SIGNAL_TEXT)) {
            abort();
        }
        for (uint32_t i = 0; i < nl; ++i) {
            const void* end = memchr(labels[i].label, '\0', sizeof labels[i].label);
            if (end == nullptr || end == labels[i].label ||
                (i > 0 && labels[i].raw <= labels[i - 1].raw)) {
                abort();
            }
            const char* found = ecuio_recording_enum_label(r, k, labels[i].raw);
            if (found == nullptr || strcmp(found, labels[i].label) != 0) {
                abort();
            }
        }
        if (s.samples == 0) {
            if (s.first_us != ECUIO_RECORDING_NONE) {
                abort();
            }
            continue;
        }
        if (s.first_us > s.last_us || s.last_us > o.info.duration_us) {
            abort();
        }
        /* One column over everything holds every sample, from the
         * summaries or the raw rows alike */
        ecuio_recording_column_t col{};
        const uint64_t t1 = s.last_us + 1;
        if (ecuio_recording_query(r, &k, 1, 0, t1, &col, 1) != ECUIO_OK ||
            col.samples != (s.samples > UINT32_MAX ? UINT32_MAX : s.samples)) {
            abort();
        }
        const bool text = (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        if (!text && !std::isnan(s.lowest) &&
            (col.lowest != s.lowest || col.highest != s.highest)) {
            abort();
        }
        /* Narrow columns over the first second agree with the wide one
         * on the count there */
        ecuio_recording_column_t cols[16];
        if (ecuio_recording_query(r, &k, 1, s.first_us, s.first_us + 1000, cols,
                                  16) != ECUIO_OK ||
            cols[0].samples == 0) {
            abort();
        }
        /* Half-second columns over a short recording read every second
         * of it again: the same count and range as the open found */
        if (t1 <= 64000000) {
            const auto n = static_cast<uint32_t>((t1 + 499999) / 500000);
            std::vector<ecuio_recording_column_t> half(n);
            if (ecuio_recording_query(r, &k, 1, 0, n * 500000ull, half.data(),
                                      n) != ECUIO_OK) {
                abort();
            }
            uint64_t sum = 0;
            double lo = NAN;
            double hi = NAN;
            for (const ecuio_recording_column_t& c : half) {
                sum += c.samples;
                if (c.samples > 0 && !text) {
                    lo = std::isnan(lo) || c.lowest < lo ? c.lowest : lo;
                    hi = std::isnan(hi) || c.highest > hi ? c.highest : hi;
                }
            }
            if (sum != s.samples ||
                (!text && sum > 0 && (lo != s.lowest || hi != s.highest))) {
                abort();
            }
        }
        char buf[64];
        uint64_t at = 0;
        if (ecuio_recording_text_at(r, k, s.last_us, buf, sizeof buf, &at) !=
                ECUIO_OK ||
            at > s.last_us || at < s.first_us) {
            abort();
        }
        if (!text) {
            ecuio_recording_find_t f{};
            f.kind = ECUIO_RECORDING_FIND_THRESHOLD;
            f.signal = k;
            f.direction = ECUIO_RECORDING_FIND_RISING;
            f.level = s.highest;
            f.hysteresis = 0.0;
            if (std::isinf(f.level)) {
                f.level = 0.0;
            }
            uint64_t found = 0;
            if (ecuio_recording_find(r, &f, 0, &found) != ECUIO_OK ||
                (found != ECUIO_RECORDING_NONE &&
                 (found < s.first_us || found > s.last_us))) {
                abort();
            }
        }
        whole_stats(r, k, s);
        steps_through(r, k, s);
    }
    aids(r, o.info);
    for (uint32_t m = 0; m < o.info.marker_count; ++m) {
        ecuio_recording_marker_t mk{};
        if (ecuio_recording_get_marker(r, m, &mk) != ECUIO_OK ||
            mk.label_utf8 == nullptr ||
            strlen(mk.label_utf8) != mk.label_length) {
            abort();
        }
    }
    ecuio_recording_close(r);
    return o;
}

struct checked {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_check_summary_t summary{};
    uint64_t passed[8] = {};
    uint64_t seen = 0;
    uint64_t last_end = 0;   /* the latest at_us + length_us passed */
    uint64_t stop_after = 0; /* 0: never stop */
};

int on_finding(const ecuio_check_finding_t* f, void* user)
{
    auto* c = static_cast<checked*>(user);
    if (f->kind < ECUIO_CHECK_GAP || f->kind > ECUIO_CHECK_ASLEEP ||
        f->reserved[0] != 0 || f->reserved[1] != 0) {
        abort();
    }
    const bool whole = (f->flags & ECUIO_CHECK_FINDING_WHOLE_BUS) != 0;
    if ((whole && f->arbitration_id != 0) ||
        (whole && (f->flags & ECUIO_CHECK_FINDING_EXTENDED) != 0) ||
        (f->kind != ECUIO_CHECK_LINK && f->code != 0) ||
        f->at_us + f->length_us < f->at_us) {
        abort();
    }
    if (f->kind == ECUIO_CHECK_ISOTP &&
        !ecuio_check::iso15765_id(f->arbitration_id,
                                  (f->flags & ECUIO_CHECK_FINDING_EXTENDED) !=
                                      0)) {
        abort();
    }
    ++c->passed[f->kind];
    ++c->seen;
    if (f->at_us + f->length_us > c->last_end) {
        c->last_end = f->at_us + f->length_us;
    }
    return c->stop_after != 0 && c->seen >= c->stop_after ? 1 : 0;
}

checked check_folder(uint64_t stop_after)
{
    checked c;
    c.stop_after = stop_after;
    c.rc = ecuio_recording_check(folder().c_str(), nullptr, nullptr, on_finding,
                                 &c, &c.summary);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr) {
        abort();
    }
    c.detail = detail;
    if (c.rc == ECUIO_ERR_LOAD_FAILED) {
        return c;
    }
    if (c.rc != ECUIO_OK && !(c.rc == ECUIO_ERR_CANCELED && stop_after != 0 &&
                              c.seen == stop_after)) {
        abort();
    }
    for (uint32_t k = 1; k < 8; ++k) {
        if (c.summary.counts[k] != c.passed[k]) {
            abort();
        }
    }
    if (c.summary.counts[0] != 0 || c.summary.torn_rows > 1 ||
        c.summary.ids > c.summary.frames) {
        abort();
    }
    return c;
}

/* One row as ecuio_recording_read_frames handed it on */
struct frame_row {
    uint64_t host_us;
    uint64_t timestamp_us;
    uint32_t id;
    uint32_t flags;
    uint32_t length;
    uint64_t sum; /* the bytes' FNV-1a */
    bool operator==(const frame_row& o) const
    {
        return host_us == o.host_us && timestamp_us == o.timestamp_us &&
               id == o.id && flags == o.flags && length == o.length &&
               sum == o.sum;
    }
};

struct read_back {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_frames_read_t summary{};
    std::vector<frame_row> rows;
    size_t stop_after = 0; /* 0: never stop */
};

int on_frame_row(void* user, const ecuio_frame_t* f, uint64_t host_us)
{
    auto* r = static_cast<read_back*>(user);
    if (f->data_length > ECUIO_FRAME_DATA_MAX || f->reserved != 0) {
        abort();
    }
    uint64_t sum = 14695981039346656037ULL;
    for (uint32_t i = 0; i < f->data_length; ++i) {
        sum = (sum ^ f->data[i]) * 1099511628211ULL;
    }
    for (uint32_t i = f->data_length; i < ECUIO_FRAME_DATA_MAX; ++i) {
        if (f->data[i] != 0) {
            abort();
        }
    }
    r->rows.push_back(frame_row{host_us, f->timestamp_us, f->arbitration_id,
                                f->flags, f->data_length, sum});
    return r->stop_after != 0 && r->rows.size() >= r->stop_after ? 1 : 0;
}

read_back read_frames(uint64_t from_us, uint64_t to_us, size_t stop_after)
{
    read_back r;
    r.stop_after = stop_after;
    r.rc = ecuio_recording_read_frames(folder().c_str(), from_us, to_us,
                                       on_frame_row, &r, &r.summary);
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512 ||
        strchr(detail, '\n') != nullptr ||
        strstr(detail, "ecuio-fuzz-recording") != nullptr ||
        r.summary.rows != r.rows.size() || r.summary.torn_rows > 1 ||
        r.summary.reserved0 != 0 || r.summary.reserved[0] != 0 ||
        r.summary.reserved[1] != 0) {
        abort();
    }
    r.detail = detail;
    if (r.rc != ECUIO_OK && r.rc != ECUIO_ERR_LOAD_FAILED &&
        !(r.rc == ECUIO_ERR_CANCELED && stop_after != 0 &&
          r.rows.size() == stop_after)) {
        abort();
    }
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (r.rows[i].host_us < from_us ||
            (to_us != 0 && r.rows[i].host_us > to_us) ||
            (i > 0 && r.rows[i].host_us < r.rows[i - 1].host_us)) {
            abort();
        }
    }
    return r;
}

/* The whole file read, then stretches of it: each the whole read's rows
 * in its range, found by the search as a plain scan finds them */
void read_frames_back(const uint8_t* data, size_t size)
{
    const read_back whole = read_frames(0, 0, 0);
    const read_back again = read_frames(0, 0, 0);
    if (whole.rc != again.rc || whole.detail != again.detail ||
        !(whole.rows == again.rows)) {
        abort();
    }
    if (whole.rc != ECUIO_OK || whole.rows.empty()) {
        return;
    }
    const size_t n = whole.rows.size();
    const size_t a = (size > 1 ? data[size - 1] : 0) % n;
    const size_t b = a + (size > 2 ? data[size - 2] : 0) % (n - a);
    const uint64_t from_us = whole.rows[a].host_us + (data[0] & 1);
    const uint64_t to_us = whole.rows[b].host_us;
    if (to_us != 0 && to_us < from_us) {
        return;
    }
    const read_back part = read_frames(from_us, to_us, 0);
    if (part.rc != ECUIO_OK) {
        abort();
    }
    std::vector<frame_row> want;
    for (const frame_row& row : whole.rows) {
        if (row.host_us >= from_us && (to_us == 0 || row.host_us <= to_us)) {
            want.push_back(row);
        }
    }
    if (!(part.rows == want)) {
        abort();
    }
    const read_back stopped = read_frames(0, 0, 1);
    if (stopped.rc != ECUIO_ERR_CANCELED || stopped.rows.size() != 1 ||
        !(stopped.rows[0] == whole.rows[0])) {
        abort();
    }
}

/* --- The stretches' rules (recording_aids.h), an input starting 'S' ---
 * After it: a byte of switches, ten of rules, two of range, three
 * sample counts, then two bytes a sample (a time step and a value) for
 * the gate, the speed and the engine speed in turn. Times never go back,
 * as the reader feeds them; values reach NaN, both infinities, -0.0, the
 * double's edges, the floor and the level themselves. */

double rule_value(uint8_t b)
{
    /* What the reader passes on: 0 or more, never NaN, inf allowed */
    static const double kRules[] = {0.0,
                                    0.5,
                                    1.0,
                                    2.0,
                                    3.0,
                                    50.0,
                                    56.32704,
                                    80.0,
                                    100.0,
                                    1e300,
                                    INFINITY,
                                    4.9e-324,
                                    2.2250738585072014e-308,
                                    750.0,
                                    10.0};
    return kRules[b % (sizeof kRules / sizeof kRules[0])];
}

uint64_t rule_us(uint8_t b)
{
    static const uint64_t kUs[] = {
        0, 1, 1000, 100000, 500000, 1000000, 10000000, UINT64_MAX, 1500000};
    return kUs[b % (sizeof kUs / sizeof kUs[0])];
}

double sample_value(uint8_t b)
{
    static const double kEdges[] = {NAN,
                                    INFINITY,
                                    -INFINITY,
                                    -0.0,
                                    1.7976931348623157e308,
                                    -1.7976931348623157e308,
                                    2.2250738585072014e-308,
                                    4.9e-324,
                                    56.32704,
                                    80.0,
                                    1e-300,
                                    -1.0,
                                    100.0,
                                    0.0,
                                    79.999999999999986,
                                    1e300};
    return b >= 240 ? kEdges[b - 240] : b * 0.5;
}

uint64_t step_us(uint8_t b)
{
    if (b < 200) {
        return b * 10000ull; /* 0 to 2 s */
    }
    return b < 255 ? (b - 200) * 1000000ull : (1ull << 62);
}

uint64_t saturated(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

bool high(double v, double level)
{
    return v >= level;
}

/* The largest climb the slow way: every pair of samples in (from, to]
 * and the one in force at from, the later above the earlier */
double climb_by_pairs(const ecuio_aids::samples& s, uint64_t from, uint64_t to)
{
    double best = 0.0;
    size_t first = 0;
    for (size_t i = 0; i < s.size() && s[i].us <= from; ++i) {
        first = i; /* the one in force at from */
    }
    for (size_t i = first; i < s.size() && s[i].us <= to; ++i) {
        for (size_t j = i + 1; j < s.size() && s[j].us <= to; ++j) {
            if (s[j].value - s[i].value > best) { /* false for a NaN */
                best = s[j].value - s[i].value;
            }
        }
    }
    return best;
}

/* What a pull's rules ask of it, worked out apart from pull_rises */
bool rises(const ecuio_aids::stretch& x, bool trim,
           const ecuio_aids::samples* engine_speed,
           const ecuio_aids::pull_rules& rules)
{
    if (std::isnan(x.start) || std::isnan(x.end) ||
        (trim && !(x.end > x.start))) {
        return false;
    }
    if (engine_speed != nullptr) {
        return climb_by_pairs(*engine_speed, x.from_us, x.to_us) >=
               rules.min_rise;
    }
    return x.end - x.start >= rules.min_gain;
}

void stretches(const uint8_t* data, size_t size)
{
    namespace aids = ecuio_aids;
    if (size < 16) {
        return;
    }
    const uint8_t switches = data[0];
    const aids::pull_rules pull{
        rule_value(data[1]), rule_us(data[2]),
        rule_us(data[3]),    (switches & 4) != 0 ? rule_value(data[4]) : 0.0,
        rule_value(data[9]), rule_value(data[10])};
    const aids::cruise_rules cruise{rule_us(data[5]), rule_value(data[6]),
                                    rule_value(data[7]), rule_value(data[8])};
    const bool trim = (switches & 1) != 0;
    const bool throttled = (switches & 2) != 0;
    const bool top_speed_end = (switches & 32) != 0;
    const uint32_t flags_in =
        (switches & 8) != 0 ? ECUIO_RECORDING_STRETCH_BY_LOAD : 0u;
    const uint8_t range[2] = {data[11], data[12]};
    size_t at = 16;
    aids::samples s[3];
    for (int k = 0; k < 3; ++k) {
        uint64_t t = 0;
        for (size_t n = 0; n < data[13 + k] && at + 1 < size; ++n, at += 2) {
            t = saturated(t, step_us(data[at]));
            s[k].push_back(aids::sample{t, sample_value(data[at + 1])});
        }
    }
    const aids::samples& gate = s[0];
    const aids::samples& speed = s[1];
    const aids::samples& measure = trim ? s[1] : s[2];
    /* The recording's engine speed, or none: the gain floor then */
    const aids::samples* engine_speed = (switches & 16) != 0 ? &s[2] : nullptr;

    /* Runs: in range, in order, apart, each end at the level, and every
     * sample at the level inside one */
    const auto runs = aids::runs_at_or_above(gate, pull.level, pull.gap_us);
    size_t covered = 0;
    for (size_t r = 0; r < runs.size(); ++r) {
        const auto& run = runs[r];
        if (run.first > run.second || run.second >= gate.size() ||
            (r > 0 && run.first <= runs[r - 1].second) ||
            !high(gate[run.first].value, pull.level) ||
            !high(gate[run.second].value, pull.level)) {
            abort();
        }
    }
    for (size_t i = 0; i < gate.size(); ++i) {
        if (!high(gate[i].value, pull.level)) {
            continue;
        }
        while (covered < runs.size() && runs[covered].second < i) {
            ++covered;
        }
        if (covered == runs.size() || runs[covered].first > i) {
            abort();
        }
    }

    /* Pulls: as long as the hold, risen as the rules ask, from a run's
     * first sample, in time order, the flags as given */
    std::vector<aids::stretch> pulls;
    aids::find_pulls(gate, measure, trim, top_speed_end, engine_speed, pull,
                     flags_in, pulls);
    for (size_t p = 0; p < pulls.size(); ++p) {
        const aids::stretch& x = pulls[p];
        if (x.kind != ECUIO_RECORDING_STRETCH_PULL || x.from_us > x.to_us ||
            x.to_us - x.from_us < pull.hold_us ||
            !rises(x, trim, engine_speed, pull) || x.flags != flags_in ||
            (p > 0 && x.from_us < pulls[p - 1].from_us)) {
            abort();
        }
        /* Runs may share a first time (a step of 0), so any run counts */
        bool within_a_run = false;
        bool ends_at_the_lift = false;
        for (const auto& run : runs) {
            if (gate[run.first].us == x.from_us &&
                x.to_us <= gate[run.second].us) {
                within_a_run = true;
                ends_at_the_lift =
                    ends_at_the_lift || gate[run.second].us == x.to_us;
            }
        }
        /* The lift ends a pull unless top speed does, and then only with
         * a speed measure, never past the lift */
        if (!within_a_run || (!(top_speed_end && trim) && !ends_at_the_lift)) {
            abort();
        }
    }

    /* Cruises: as long as the hold, one after another, a mean in both
     * ends at the floor or above (a mean of values at or above it) */
    std::vector<aids::stretch> cruises;
    aids::find_cruises(speed, throttled ? &gate : nullptr, cruise, flags_in,
                       cruises);
    for (size_t c = 0; c < cruises.size(); ++c) {
        const aids::stretch& x = cruises[c];
        if (x.kind != ECUIO_RECORDING_STRETCH_CRUISE || x.from_us > x.to_us ||
            x.to_us - x.from_us < cruise.hold_us || x.flags != flags_in ||
            !same_bits(x.start, x.end) || std::isnan(x.start) ||
            x.start < cruise.floor * (1 - 1e-12) ||
            (c > 0 && x.from_us < cruises[c - 1].to_us)) {
            abort();
        }
    }

    /* The cut: within the range, sorted, the cut flagged at its edge,
     * still the hold long, and every stretch inside the range kept */
    uint64_t span = 0;
    for (const auto& v : s) {
        span = (std::max)(span, v.empty() ? 0 : v.back().us);
    }
    const uint64_t from_us = span / 256 * range[0];
    const uint64_t to_us = saturated(from_us, span / 256 * range[1] + 1);
    std::vector<aids::stretch> all = pulls;
    all.insert(all.end(), cruises.begin(), cruises.end());
    size_t inside = 0;
    for (const aids::stretch& x : all) {
        inside += x.from_us >= from_us && x.to_us <= to_us ? 1 : 0;
    }
    aids::cut_to(all, from_us, to_us, pull, trim, engine_speed, cruise.hold_us,
                 measure, speed);
    size_t untouched = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        const aids::stretch& x = all[i];
        const bool is_pull = x.kind == ECUIO_RECORDING_STRETCH_PULL;
        if (x.from_us < from_us || x.to_us > to_us || x.from_us > x.to_us ||
            (i > 0 && x.from_us < all[i - 1].from_us) ||
            x.to_us - x.from_us < (is_pull ? pull.hold_us : cruise.hold_us) ||
            ((x.flags & ECUIO_RECORDING_STRETCH_CUT_START) != 0 &&
             x.from_us != from_us) ||
            ((x.flags & ECUIO_RECORDING_STRETCH_CUT_END) != 0 &&
             x.to_us != to_us) ||
            (is_pull && !rises(x, trim, engine_speed, pull))) {
            abort();
        }
        const uint32_t cuts =
            ECUIO_RECORDING_STRETCH_CUT_START | ECUIO_RECORDING_STRETCH_CUT_END;
        untouched += (x.flags & cuts) == 0 ? 1 : 0;
    }
    if (untouched != inside) {
        abort();
    }

    /* Pure: the same input, the same stretches */
    std::vector<aids::stretch> again;
    aids::find_pulls(gate, measure, trim, top_speed_end, engine_speed, pull,
                     flags_in, again);
    aids::find_cruises(speed, throttled ? &gate : nullptr, cruise, flags_in,
                       again);
    aids::cut_to(again, from_us, to_us, pull, trim, engine_speed,
                 cruise.hold_us, measure, speed);
    if (again.size() != all.size()) {
        abort();
    }
    for (size_t i = 0; i < all.size(); ++i) {
        if (again[i].from_us != all[i].from_us ||
            again[i].to_us != all[i].to_us || again[i].flags != all[i].flags ||
            !same_bits(again[i].start, all[i].start) ||
            !same_bits(again[i].end, all[i].end)) {
            abort();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 262144) {
        return 0;
    }
    if (data[0] == 'S') {
        stretches(data + 1, size - 1);
        return 0;
    }
    /* 'F' a folder; '3' or '4' a folder whose events.log starts with
     * that format's first line, so its header's label lines (and format
     * 4's keyed signal lines) are reached */
    const bool format3 = data[0] == '3' || data[0] == '4';
    if (data[0] != 'F' && !format3) {
        each_line(data + 1, size - 1);
        return 0;
    }
    /* events.log, signals.csv, markers.csv, frames.csv, conditions.json:
     * absent past the cuts given */
    const uint8_t* parts[5] = {};
    size_t lens[5] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 5; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    std::string events;
    if (format3 && count > 0) {
        events = std::string(data[0] == '4' ? rows::kFormat4 : rows::kFormat3) +
                 "\n" +
                 std::string(reinterpret_cast<const char*>(parts[0]), lens[0]);
        parts[0] = reinterpret_cast<const uint8_t*>(events.data());
        lens[0] = events.size();
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    write_file("frames.csv", parts[3], lens[3], count > 3);
    write_file("conditions.json", parts[4], lens[4], count > 4);
    g_want = want_conditions(reinterpret_cast<const char*>(parts[0]), lens[0],
                             count > 0,
                             reinterpret_cast<const char*>(parts[4]), lens[4],
                             count > 4);
    /* signals.csv, then the frames through obd2 (packed replies split),
     * the mock's set (a computed signal) and a float, each twice alike;
     * every open that succeeds reads the same conditions from events.log */
    const ecuio_signal_set_t* sets[] = {nullptr, ecuio_obd2_signal_set(),
                                        ecuio_mock_signal_set(), float_set()};
    opened first_ok;
    bool any_ok = false;
    for (const ecuio_signal_set_t* set : sets) {
        const opened a = open_and_query(set);
        const opened b = open_and_query(set);
        if (a.rc != b.rc || a.detail != b.detail ||
            memcmp(&a.info, &b.info, sizeof a.info) != 0 ||
            !same_conditions(a, b)) {
            abort();
        }
        if (a.rc != ECUIO_OK) {
            continue;
        }
        if (any_ok &&
            (!same_conditions(a, first_ok) ||
             a.info.condition_events != first_ok.info.condition_events)) {
            abort();
        }
        if (!any_ok) {
            first_ok = a;
            any_ok = true;
        }
    }
    const checked x = check_folder(0);
    const checked y = check_folder(0);
    if (x.rc != y.rc || x.detail != y.detail ||
        memcmp(&x.summary, &y.summary, sizeof x.summary) != 0) {
        abort();
    }
    if (x.rc == ECUIO_OK) {
        if (x.last_end > x.summary.duration_us) {
            abort();
        }
        if (x.seen > 0) {
            const checked z = check_folder(1);
            if (z.rc != ECUIO_ERR_CANCELED || z.seen != 1) {
                abort();
            }
        }
    }
    read_frames_back(data, size);
    return 0;
}
