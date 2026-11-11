// File/layer: fuzz/fuzz_recording.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_recording (rows, a folder, the stretches' rules)

#include <ecuio/core.h>
#include <ecuio/recording.h>
#include <ecuio/signals.h>

#include "recording_aids.h" /* core/src: pure, the stretches' rules */
#include "recording_check.h" /* core/src: pure, iso15765_id */
#include "recording_rows.h" /* core/src: pure */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace rows = ecuio_recording_rows;

namespace {

bool same_bits(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

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

/* The scratch folder, one per process (libFuzzer's -jobs run several) */
std::string g_folder;

void remove_folder()
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(g_folder), ec);
}

const std::string& folder()
{
    if (g_folder.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) {
            tmp = getenv("TMPDIR");
        }
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        g_folder = base + "/ecuio-fuzz-recording-" +
                   std::to_string(static_cast<long>(ecuio_fuzz_getpid()));
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(g_folder),
                                            ec);
        atexit(remove_folder);
    }
    return g_folder;
}

void write_file(const char* name, const uint8_t* p, size_t n, bool present)
{
    const std::string path = folder() + "/" + name;
    if (!present) {
        remove(path.c_str());
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        abort();
    }
    if (n > 0 && fwrite(p, 1, n, f) != n) {
        abort();
    }
    fclose(f);
}

struct opened {
    ecuio_status_t rc = ECUIO_OK;
    std::string detail;
    ecuio_recording_info_t info{};
};

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
    for (uint32_t k = 0; k < o.info.signal_count; ++k) {
        ecuio_recording_signal_t s{};
        if (ecuio_recording_get_signal(r, k, &s) != ECUIO_OK ||
            memchr(s.name, '\0', sizeof s.name) == nullptr ||
            memchr(s.unit, '\0', sizeof s.unit) == nullptr ||
            s.decimal_places > 3) {
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
    }
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
 * After it: a byte of switches, eight of rules, two of range, three
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
                                    2.2250738585072014e-308};
    return kRules[b % (sizeof kRules / sizeof kRules[0])];
}

uint64_t rule_us(uint8_t b)
{
    static const uint64_t kUs[] = {0,      1,       1000,     100000,
                                   500000, 1000000, 10000000, UINT64_MAX};
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

void stretches(const uint8_t* data, size_t size)
{
    namespace aids = ecuio_aids;
    if (size < 14) {
        return;
    }
    const uint8_t switches = data[0];
    const aids::pull_rules pull{
        rule_value(data[1]), rule_us(data[2]), rule_us(data[3]),
        (switches & 4) != 0 ? rule_value(data[4]) : 0.0};
    const aids::cruise_rules cruise{rule_us(data[5]), rule_value(data[6]),
                                    rule_value(data[7]), rule_value(data[8])};
    const bool trim = (switches & 1) != 0;
    const bool throttled = (switches & 2) != 0;
    const uint32_t flags_in =
        (switches & 8) != 0 ? ECUIO_RECORDING_STRETCH_BY_LOAD : 0u;
    const uint8_t range[2] = {data[9], data[10]};
    size_t at = 14;
    aids::samples s[3];
    for (int k = 0; k < 3; ++k) {
        uint64_t t = 0;
        for (size_t n = 0; n < data[11 + k] && at + 1 < size; ++n, at += 2) {
            t = saturated(t, step_us(data[at]));
            s[k].push_back(aids::sample{t, sample_value(data[at + 1])});
        }
    }
    const aids::samples& gate = s[0];
    const aids::samples& speed = s[1];
    const aids::samples& measure = trim ? s[1] : s[2];

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

    /* Pulls: as long as the hold, risen, from a run's first sample, in
     * time order, the flags as given */
    std::vector<aids::stretch> pulls;
    aids::find_pulls(gate, measure, trim, pull, flags_in, pulls);
    for (size_t p = 0; p < pulls.size(); ++p) {
        const aids::stretch& x = pulls[p];
        if (x.kind != ECUIO_RECORDING_STRETCH_PULL || x.from_us > x.to_us ||
            x.to_us - x.from_us < pull.hold_us || !(x.end > x.start) ||
            x.flags != flags_in ||
            (p > 0 && x.from_us < pulls[p - 1].from_us)) {
            abort();
        }
        bool starts_a_run = false;
        for (const auto& run : runs) {
            starts_a_run = starts_a_run || gate[run.first].us == x.from_us;
        }
        if (!starts_a_run) {
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
    aids::cut_to(all, from_us, to_us, pull.hold_us, cruise.hold_us, measure,
                 speed);
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
            (is_pull && !(x.end > x.start))) {
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
    aids::find_pulls(gate, measure, trim, pull, flags_in, again);
    aids::find_cruises(speed, throttled ? &gate : nullptr, cruise, flags_in,
                       again);
    aids::cut_to(again, from_us, to_us, pull.hold_us, cruise.hold_us, measure,
                 speed);
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
    /* 'F' a folder; '3' a folder whose events.log starts with format 3's
     * first line, so its header's label lines are reached */
    const bool format3 = data[0] == '3';
    if (data[0] != 'F' && !format3) {
        each_line(data + 1, size - 1);
        return 0;
    }
    /* events.log, signals.csv, markers.csv, frames.csv: absent past the
     * cuts given */
    const uint8_t* parts[4] = {};
    size_t lens[4] = {};
    size_t count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= size && count < 4; ++i) {
        if (i == size || data[i] == 0x1E) {
            parts[count] = data + start;
            lens[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    std::string events;
    if (format3 && count > 0) {
        events = std::string(rows::kFormat3) + "\n" +
                 std::string(reinterpret_cast<const char*>(parts[0]), lens[0]);
        parts[0] = reinterpret_cast<const uint8_t*>(events.data());
        lens[0] = events.size();
    }
    write_file("events.log", parts[0], lens[0], count > 0);
    write_file("signals.csv", parts[1], lens[1], count > 1);
    write_file("markers.csv", parts[2], lens[2], count > 2);
    write_file("frames.csv", parts[3], lens[3], count > 3);
    /* signals.csv, then the frames through obd2 (packed replies split)
     * and the mock's set (a computed signal), each twice alike */
    const ecuio_signal_set_t* sets[] = {nullptr, ecuio_obd2_signal_set(),
                                        ecuio_mock_signal_set()};
    for (const ecuio_signal_set_t* set : sets) {
        const opened a = open_and_query(set);
        const opened b = open_and_query(set);
        if (a.rc != b.rc || a.detail != b.detail ||
            memcmp(&a.info, &b.info, sizeof a.info) != 0) {
            abort();
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
