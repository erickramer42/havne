// File/layer: core/src/recording_check.h | internal, C++17, Qt-free,
//             header-only; ALL platforms
// Testing: UNIT: CheckRulesTest (test_core); FUZZ: fuzz_recording
//
// The frames check (ADR-0049 section 6) as a stream: a checker is fed the
// events.log lines it takes first, then each frame in order, and settles
// each finding when the frame or line that ends it arrives. Its memory is
// a fixed handful of numbers per ID (its count, last time, a P-square
// median of its intervals, a rate drop under way, an ISO-TP message under
// way) plus the events.log lines it keeps (links and sleeps), never the
// frames.

#ifndef ECUIO_RECORDING_CHECK_H
#define ECUIO_RECORDING_CHECK_H

#include "recording_rows.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace ecuio_check {

constexpr uint64_t kGapUs = ECUIO_CHECK_DEFAULT_GAP_US;
constexpr uint32_t kRateDropX100 = ECUIO_CHECK_DEFAULT_RATE_DROP_X100;
constexpr uint64_t kSettleUs = ECUIO_CHECK_DEFAULT_SETTLE_US;
/* An ID's rate is judged only after this many intervals */
constexpr uint64_t kJudgedAfter = 16;
/* A slow interval this short is no rate drop: inside Live's 100 ms window
 * it changes nothing a reader sees, and a USB adapter's bursts hand the
 * host frames microseconds apart, so an ID's median there can be far
 * under any interval that matters */
constexpr uint64_t kDropFloorUs = 100000;

/* A streaming median: the P-square estimator (Jain and Chlamtac, 1985),
 * five markers, exact over the first five observations */
class p2_median
{
public:
    void add(double x)
    {
        if (count_ < 5) {
            q_[count_++] = x;
            std::sort(q_, q_ + count_);
            if (count_ == 5) {
                for (int i = 0; i < 5; ++i) {
                    n_[i] = i;
                    want_[i] = 2.0 * i * kP; /* 0, 1, 2, 3, 4 for p = 0.5 */
                }
            }
            return;
        }
        ++count_;
        int k = 0;
        if (x < q_[0]) {
            q_[0] = x;
        } else if (x >= q_[4]) {
            q_[4] = x;
            k = 3;
        } else {
            while (k < 3 && x >= q_[k + 1]) {
                ++k;
            }
        }
        for (int i = k + 1; i < 5; ++i) {
            ++n_[i];
        }
        static const double step[5] = {0, kP / 2, kP, (1 + kP) / 2, 1};
        for (int i = 0; i < 5; ++i) {
            want_[i] += step[i];
        }
        for (int i = 1; i <= 3; ++i) {
            const double d = want_[i] - n_[i];
            if ((d >= 1 && n_[i + 1] - n_[i] > 1) ||
                (d <= -1 && n_[i - 1] - n_[i] < -1)) {
                const int s = d > 0 ? 1 : -1;
                const double p = parabolic(i, s);
                if (q_[i - 1] < p && p < q_[i + 1]) {
                    q_[i] = p;
                } else {
                    q_[i] += s * (q_[i + s] - q_[i]) / (n_[i + s] - n_[i]);
                }
                n_[i] += s;
            }
        }
    }

    double value() const
    {
        if (count_ == 0) {
            return 0;
        }
        if (count_ < 5) {
            return count_ % 2 == 1 ? q_[count_ / 2]
                                   : (q_[count_ / 2 - 1] + q_[count_ / 2]) / 2;
        }
        return q_[2];
    }

    uint64_t count() const { return count_; }

private:
    static constexpr double kP = 0.5;

    double parabolic(int i, int s) const
    {
        const double a = n_[i] - n_[i - 1];
        const double b = n_[i + 1] - n_[i];
        return q_[i] + s / (n_[i + 1] - n_[i - 1]) *
                           ((a + s) * (q_[i + 1] - q_[i]) / b +
                            (b - s) * (q_[i] - q_[i - 1]) / a);
    }

    double q_[5] = {};
    double n_[5] = {};
    double want_[5] = {};
    uint64_t count_ = 0;
};

/* ISO 15765-4's IDs, the ones a diagnostic conversation uses: 0x7DF and
 * 0x7E0-0x7EF, and 29-bit 18DA<ta><sa> and 18DB33F1. Other IDs may carry
 * payloads whose first nibble reads as a PCI, so only these are checked. */
inline bool iso15765_id(uint32_t id, bool extended)
{
    if (extended) {
        const uint32_t top = id & 0x1FFF0000u;
        return top == 0x18DA0000u || id == 0x18DB33F1u;
    }
    return id == 0x7DF || (id >= 0x7E0 && id <= 0x7EF);
}

/* "<words> (state <code>, <n> episode[s])", the recorder's link line */
inline bool parse_link(const char* p, size_t n, int32_t* code,
                       uint64_t* episodes)
{
    const char* const lead = " (state ";
    const size_t lead_n = std::strlen(lead);
    size_t at = n;
    for (size_t i = n; i-- > 0;) {
        if (i + lead_n <= n && std::memcmp(p + i, lead, lead_n) == 0) {
            at = i;
            break;
        }
    }
    if (at == n) {
        return false;
    }
    const char* q = p + at + lead_n;
    const char* end = p + n;
    bool negative = false;
    if (q < end && *q == '-') {
        negative = true;
        ++q;
    }
    const char* digits = q;
    while (q < end && *q >= '0' && *q <= '9') {
        ++q;
    }
    uint64_t c = 0;
    if (!ecuio_recording_rows::parse_count(
            digits, static_cast<size_t>(q - digits), &c) ||
        c > 0x7FFFFFFFu || end - q < 2 || q[0] != ',' || q[1] != ' ') {
        return false;
    }
    q += 2;
    digits = q;
    while (q < end && *q >= '0' && *q <= '9') {
        ++q;
    }
    uint64_t e = 0;
    if (!ecuio_recording_rows::parse_count(
            digits, static_cast<size_t>(q - digits), &e)) {
        return false;
    }
    const size_t rest = static_cast<size_t>(end - q);
    const bool one = rest == 9 && std::memcmp(q, " episode)", 9) == 0;
    const bool many = rest == 10 && std::memcmp(q, " episodes)", 10) == 0;
    if (!one && !many) {
        return false;
    }
    *code = negative ? -static_cast<int32_t>(c) : static_cast<int32_t>(c);
    *episodes = e;
    return true;
}

/* "Asleep for N[.N] s: ...", both front ends' sleep line, in us */
inline bool parse_asleep(const char* p, size_t n, uint64_t* us)
{
    const char* const lead = "Asleep for ";
    const size_t lead_n = std::strlen(lead);
    if (n <= lead_n || std::memcmp(p, lead, lead_n) != 0) {
        return false;
    }
    size_t i = lead_n;
    const size_t start = i;
    while (i < n && ((p[i] >= '0' && p[i] <= '9') || p[i] == '.')) {
        ++i;
    }
    double s = 0;
    if (i == start || i + 2 > n || p[i] != ' ' || p[i + 1] != 's' ||
        !ecuio_recording_rows::parse_number(p + start, i - start, &s) ||
        !(s >= 0) || s > 1e9) {
        return false;
    }
    *us = static_cast<uint64_t>(std::llround(s * 1e6));
    return true;
}

/* An events.log line the check keeps: a link transition or a sleep */
struct event {
    uint64_t at_us;     /* the line's time */
    uint32_t kind;      /* ECUIO_CHECK_LINK or _ASLEEP */
    int32_t code;       /* a link's state */
    uint64_t episodes;  /* a link's */
    uint64_t length_us; /* a sleep's; the stretch is [at - length, at] */
};

/* The finding's sink: false stops the check */
using sink_fn = bool (*)(const ecuio_check_finding_t&, void*);

class checker
{
public:
    /* options already judged; events in time order; isotp: events.log
     * says the frame flags are complete */
    checker(const ecuio_check_options_t& o, std::vector<event> events,
            bool isotp, sink_fn sink, void* user)
        : gap_us_(o.gap_us != 0 ? o.gap_us : kGapUs),
          settle_us_(o.settle_us != 0 ? o.settle_us : kSettleUs),
          events_(std::move(events)), sink_(sink), user_(user),
          drop_x100_(o.rate_drop_x100 != 0 ? o.rate_drop_x100 : kRateDropX100),
          isotp_(isotp)
    {
        for (const event& e : events_) {
            if (e.kind == ECUIO_CHECK_ASLEEP) {
                const uint64_t len = (std::min)(e.length_us, e.at_us);
                sleeps_.push_back({e.at_us - len, e.at_us});
            }
        }
        std::sort(sleeps_.begin(), sleeps_.end());
    }

    /* One frame, in file order. False when the sink stopped the check. */
    bool frame(uint64_t host_us, uint32_t id, uint32_t flags,
               const uint8_t* data, uint32_t length)
    {
        if (!emit_events_until(host_us)) {
            return false;
        }
        ++summary_.frames;
        summary_.duration_us = (std::max)(summary_.duration_us, host_us);
        const bool overflow = (flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0;
        if (!overflow && overflow_frames_ != 0 && !settle_overflow()) {
            return false;
        }
        if (any_frame_) {
            const uint64_t quiet = host_us - last_any_us_;
            if (quiet > gap_us_ &&
                quiet - asleep_overlap(last_any_us_, host_us) > gap_us_) {
                if (!send(ECUIO_CHECK_GAP, 0, ECUIO_CHECK_FINDING_WHOLE_BUS,
                          last_any_us_, quiet, 0, 0)) {
                    return false;
                }
            }
            if (quiet > gap_us_) {
                quiet_start_ = last_any_us_; /* a gap or a sleep */
                quiet_seen_ = true;
            }
        } else {
            first_us_ = host_us;
            any_frame_ = true;
        }
        last_any_us_ = host_us;
        if (overflow) {
            if (overflow_frames_ == 0) {
                overflow_at_ = host_us;
            }
            overflow_last_ = host_us;
            ++overflow_frames_;
        }
        if ((flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            return true; /* its ID is the error's class, not a node's */
        }
        const bool extended = (flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
        const bool wire = isotp_ && iso15765_id(id, extended) &&
                          (flags & ECUIO_FRAME_FLAG_REASSEMBLED) == 0;
        /* On the wire a consecutive frame or a flow control is part of a
         * message: the ID's rate is its messages', as an adapter that
         * reassembles delivers them, the recordings the defaults were set
         * against */
        const bool message_part =
            wire && length > 0 &&
            ((data[0] >> 4) == 0x2 || (data[0] >> 4) == 0x3);
        const uint64_t key = (extended ? (uint64_t{1} << 32) : 0) | id;
        auto found = ids_.find(key);
        if (found == ids_.end()) {
            found = ids_.emplace(key, id_state{}).first;
            found->second.last_us = host_us;
            if (host_us > first_us_ + settle_us_ &&
                !send(ECUIO_CHECK_LATE_ID, id, extended_flag(extended), host_us,
                      0, 1, 0)) {
                return false;
            }
        } else if (!message_part &&
                   !interval(found->second, id, extended, host_us)) {
            return false;
        }
        id_state& s = found->second;
        if (!message_part) {
            s.last_us = host_us;
        }
        if (wire) {
            wire_seen_ = true;
            return isotp_frame(s, id, extended, host_us, data, length);
        }
        return true;
    }

    /* The end of frames.csv: what's still under way, and the events.log
     * lines after the last frame. False when the sink stopped it. */
    bool finish()
    {
        if (overflow_frames_ != 0 && !settle_overflow()) {
            return false;
        }
        for (auto& kv : ids_) {
            const bool extended = (kv.first >> 32) != 0;
            const uint32_t id = static_cast<uint32_t>(kv.first);
            if (!settle_drop(kv.second, id, extended)) {
                return false;
            }
        }
        for (auto& kv : ids_) {
            const bool extended = (kv.first >> 32) != 0;
            const uint32_t id = static_cast<uint32_t>(kv.first);
            if (kv.second.tp_open &&
                !settle_incomplete(kv.second, id, extended)) {
                return false;
            }
        }
        if (!emit_events_until(UINT64_MAX)) {
            return false;
        }
        complete();
        return true;
    }

    /* The summary so far; finish() completes its flags */
    const ecuio_check_summary_t& summary()
    {
        summary_.ids = static_cast<uint32_t>(ids_.size());
        return summary_;
    }

    /* The summary's ISO-TP flags and the events' duration, also after a
     * stop */
    void complete()
    {
        summary_.ids = static_cast<uint32_t>(ids_.size());
        if (!isotp_) {
            summary_.flags |=
                ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN;
        } else if (!wire_seen_) {
            summary_.flags |=
                ECUIO_CHECK_SKIPPED_ISOTP | ECUIO_CHECK_ISOTP_NO_WIRE;
        }
        for (const event& e : events_) {
            summary_.duration_us = (std::max)(summary_.duration_us, e.at_us);
        }
    }

    size_t ids() const { return ids_.size(); }

private:
    struct id_state {
        uint64_t last_us = 0;
        p2_median median;
        /* A rate drop under way: its first slow interval's start, the end
         * of its last, and how many */
        uint64_t drop_at = 0;
        uint64_t drop_end = 0;
        uint64_t drop_count = 0;
        /* An ISO-TP message under way */
        bool tp_open = false;
        uint64_t tp_at = 0;
        uint64_t tp_last = 0;
        uint64_t tp_expected = 0;
        uint64_t tp_received = 0;
        uint64_t tp_frames = 0;
        uint8_t tp_next_sn = 0;
    };

    static uint32_t extended_flag(bool extended)
    {
        return extended ? ECUIO_CHECK_FINDING_EXTENDED : 0;
    }

    bool send(uint32_t kind, uint32_t id, uint32_t flags, uint64_t at_us,
              uint64_t length_us, uint64_t count, int32_t code)
    {
        ecuio_check_finding_t f{};
        f.kind = kind;
        f.arbitration_id = id;
        f.flags = flags;
        f.at_us = at_us;
        f.length_us = length_us;
        f.count = count;
        f.code = code;
        if (kind < 8) {
            ++summary_.counts[kind];
        }
        return sink_ == nullptr || sink_(f, user_);
    }

    /* The events.log lines up to t, as findings */
    bool emit_events_until(uint64_t t)
    {
        while (next_event_ < events_.size() &&
               events_[next_event_].at_us <= t) {
            const event& e = events_[next_event_++];
            const bool ok =
                e.kind == ECUIO_CHECK_LINK
                    ? send(ECUIO_CHECK_LINK, 0, ECUIO_CHECK_FINDING_WHOLE_BUS,
                           e.at_us, 0, e.episodes, e.code)
                    : send(ECUIO_CHECK_ASLEEP, 0, ECUIO_CHECK_FINDING_WHOLE_BUS,
                           e.at_us - (std::min)(e.length_us, e.at_us),
                           (std::min)(e.length_us, e.at_us), 0, 0);
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    /* How much of [a, b] the sleeps cover: from the first sleep ending
     * after a, while they start before b */
    uint64_t asleep_overlap(uint64_t a, uint64_t b) const
    {
        auto it = std::upper_bound(
            sleeps_.begin(), sleeps_.end(), a,
            [](uint64_t t, const std::pair<uint64_t, uint64_t>& s) {
                return t < s.second;
            });
        uint64_t covered = 0;
        for (; it != sleeps_.end() && it->first < b; ++it) {
            const uint64_t lo = (std::max)(a, it->first);
            const uint64_t hi = (std::min)(b, it->second);
            if (hi > lo) {
                covered += hi - lo;
            }
        }
        return (std::min)(covered, b - a);
    }

    bool settle_overflow()
    {
        const uint64_t n = overflow_frames_;
        overflow_frames_ = 0;
        return send(ECUIO_CHECK_OVERFLOW, 0, ECUIO_CHECK_FINDING_WHOLE_BUS,
                    overflow_at_, overflow_last_ - overflow_at_, n, 0);
    }

    bool settle_drop(id_state& s, uint32_t id, bool extended)
    {
        if (s.drop_count == 0) {
            return true;
        }
        const uint64_t n = s.drop_count;
        s.drop_count = 0;
        return send(ECUIO_CHECK_RATE_DROP, id, extended_flag(extended),
                    s.drop_at, s.drop_end - s.drop_at, n, 0);
    }

    /* An ID's interval to host_us: judged against its median once it has
     * enough, unless a gap or a sleep lies in it */
    bool interval(id_state& s, uint32_t id, bool extended, uint64_t host_us)
    {
        const uint64_t gap = host_us - s.last_us;
        if ((quiet_seen_ && quiet_start_ >= s.last_us) ||
            asleep_overlap(s.last_us, host_us) > 0) {
            return settle_drop(s, id, extended);
        }
        const double median = s.median.value();
        const bool judged = s.median.count() >= kJudgedAfter && median > 0;
        s.median.add(static_cast<double>(gap));
        if (judged && gap >= kDropFloorUs &&
            static_cast<double>(gap) * 100.0 >
                median * static_cast<double>(drop_x100_)) {
            if (s.drop_count == 0) {
                s.drop_at = s.last_us;
            }
            s.drop_end = host_us;
            ++s.drop_count;
            return true;
        }
        return settle_drop(s, id, extended);
    }

    bool settle_incomplete(id_state& s, uint32_t id, bool extended)
    {
        s.tp_open = false;
        return send(ECUIO_CHECK_ISOTP, id, extended_flag(extended), s.tp_at,
                    s.tp_last - s.tp_at, s.tp_frames, 0);
    }

    /* A frame as it crossed the bus, on an ISO 15765-4 ID: its PCI */
    bool isotp_frame(id_state& s, uint32_t id, bool extended, uint64_t host_us,
                     const uint8_t* data, uint32_t length)
    {
        if (length == 0) {
            return true;
        }
        switch (data[0] >> 4) {
        case 0x0: /* a single frame */
            return !s.tp_open || settle_incomplete(s, id, extended);
        case 0x1: { /* a first frame */
            if (s.tp_open && !settle_incomplete(s, id, extended)) {
                return false;
            }
            if (length < 2) {
                return true;
            }
            uint64_t total = (uint64_t{data[0] & 0x0Fu} << 8) | data[1];
            uint32_t header = 2;
            if (total == 0 && length >= 6) { /* the 32-bit escape */
                total = (uint64_t{data[2]} << 24) | (uint64_t{data[3]} << 16) |
                        (uint64_t{data[4]} << 8) | data[5];
                header = 6;
            }
            s.tp_open = true;
            s.tp_at = host_us;
            s.tp_last = host_us;
            s.tp_expected = total;
            s.tp_received = length > header ? length - header : 0;
            s.tp_frames = 1;
            s.tp_next_sn = 1;
            if (s.tp_received >= s.tp_expected) {
                s.tp_open = false;
            }
            return true;
        }
        case 0x2: { /* a consecutive frame */
            const uint8_t sn = data[0] & 0x0Fu;
            if (!s.tp_open || sn != s.tp_next_sn) {
                s.tp_open = false;
                return send(ECUIO_CHECK_ISOTP, id, extended_flag(extended),
                            host_us, 0, 1, 0);
            }
            s.tp_received += length - 1;
            s.tp_last = host_us;
            ++s.tp_frames;
            s.tp_next_sn = static_cast<uint8_t>((sn + 1) & 0x0Fu);
            if (s.tp_received >= s.tp_expected) {
                s.tp_open = false;
            }
            return true;
        }
        default: /* flow control, or no PCI at all */
            return true;
        }
    }

    const uint64_t gap_us_;
    const uint64_t settle_us_;
    const std::vector<event> events_;
    /* The sleeps' stretches, [start, end], by start */
    std::vector<std::pair<uint64_t, uint64_t>> sleeps_;
    size_t next_event_ = 0;
    sink_fn sink_;
    void* user_;
    ecuio_check_summary_t summary_{};
    std::map<uint64_t, id_state> ids_;
    uint64_t first_us_ = 0;
    uint64_t last_any_us_ = 0;
    uint64_t quiet_start_ = 0; /* the latest gap's or sleep's start */
    uint64_t overflow_frames_ = 0;
    uint64_t overflow_at_ = 0;
    uint64_t overflow_last_ = 0;
    const uint32_t drop_x100_;
    const bool isotp_;
    bool any_frame_ = false;
    bool quiet_seen_ = false;
    bool wire_seen_ = false;
};

} // namespace ecuio_check

#endif /* ECUIO_RECORDING_CHECK_H */
