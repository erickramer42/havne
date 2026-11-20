// File/layer: tests/j2534_hold_support.h | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest's bus (tests/test_j2534_hold.cpp,
//          tests/test_j2534_hold_key.cpp): modules answering a J2534
//          worker's pass with the time passed in

#ifndef ECUIO_TESTS_J2534_HOLD_SUPPORT_H
#define ECUIO_TESTS_J2534_HOLD_SUPPORT_H

#include <gtest/gtest.h>

#include "../core/src/j2534_hold.h"
#include "../core/src/link_health.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace j2534_hold_test {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;
constexpr int64_t kStepUs = 250;

/* One request on the bus: its channel, when it was written and when it
 * ended (answered, or its wait ran out; INT64_MAX while it is out),
 * whether it was answered and whether its reply took more than one
 * frame, its bytes, and whether it was a chosen request given back */
struct Exchange {
    uint32_t c;
    int64_t sent_us;
    int64_t ended_us;
    bool answered;
    bool segmented;
    Bytes bytes;
    bool released;
};

/* Modules answering a J2534 worker's pass: each request's first frame
 * after first_us (a module's own first_of_us when set), each frame more
 * 5 ms later; a module's reply_of gives its answer (empty: silence; 7F xx
 * 78 keeps the request out). With lose, Known Issue #42: a Single Frame
 * arriving while another module's segmented reply is on the bus (its
 * first frame to its last) is lost, whichever was written first, and
 * counted in lost. The pass is the worker's, j2534_hold_pass, or (plain)
 * the pass a worker makes with no hold, written out: each channel's wait
 * ended and its due request written in turn. */
struct Bus {
    ecuio_request_engine e;
    j2534_hold hold;
    bool plain = false;
    int64_t now = 0;
    int64_t first_us = 3 * kMs;
    int64_t first_of_us[ECUIO_MAX_CHANNELS] = {};
    bool lose = false;
    size_t lost = 0;
    std::function<Bytes(uint32_t, const ecuio_request&)> reply_of;
    std::vector<Exchange> exchanges;
    struct Due {
        uint32_t c;
        int64_t at;
        Bytes bytes;
        size_t exchange;
        int64_t from; /* a segmented reply's first frame, or -1 */
    };
    std::vector<Due> due;

    void deliver()
    {
        for (size_t i = 0; i < due.size();) {
            if (due[i].at > now) {
                ++i;
                continue;
            }
            const ecuio_reply r = ecuio_engine_on_frame(
                &e, due[i].c, due[i].bytes.data(),
                static_cast<uint16_t>(due[i].bytes.size()), now, nullptr);
            j2534_hold_answered(hold, due[i].c);
            if (r != ecuio_reply::pending) {
                exchanges[due[i].exchange].ended_us = now;
                exchanges[due[i].exchange].answered = true;
            }
            due.erase(due.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    void end_open(uint32_t c)
    {
        for (Exchange& x : exchanges) {
            if (x.c == c && x.ended_us == INT64_MAX) {
                x.ended_us = now;
            }
        }
    }

    void pass()
    {
        deliver();
        if (plain) {
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                if (ecuio_engine_expire(&e, c, now)) {
                    end_open(c);
                }
                const int32_t r = ecuio_engine_due(&e, c, now);
                if (r != ECUIO_ENGINE_NONE) {
                    write(c, r, false);
                }
            }
        } else {
            bool chosen[ECUIO_MAX_CHANNELS] = {};
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                chosen[c] = hold.chosen[c] != ECUIO_ENGINE_NONE;
            }
            j2534_hold_pass(
                hold, &e, now, [&](uint32_t c) { end_open(c); },
                [&](uint32_t c, int32_t r) { write(c, r, chosen[c]); });
        }
        now += kStepUs;
    }

    void write(uint32_t c, int32_t r, bool released)
    {
        const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
        const Bytes reply = reply_of(c, q);
        Bytes asked(q.bytes, q.bytes + q.len);
        ecuio_engine_sent(&e, c, r, true, now);
        const int64_t frames =
            reply.size() <= 7 ? 1 : 1 + (int64_t(reply.size()) - 6 + 6) / 7;
        exchanges.push_back(
            {c, now, INT64_MAX, false, frames > 1, std::move(asked), released});
        if (reply.empty()) {
            return;
        }
        const int64_t first =
            now + (first_of_us[c] != 0 ? first_of_us[c] : first_us);
        const int64_t at = first + (frames - 1) * 5 * kMs;
        if (lose && frames == 1 && under_another(c, at)) {
            ++lost;
            return;
        }
        if (lose && frames > 1) {
            drop_under(c, first, at);
        }
        due.push_back(
            {c, at, reply, exchanges.size() - 1, frames > 1 ? first : -1});
    }

    /* Whether another channel's segmented reply is on the bus at t */
    bool under_another(uint32_t c, int64_t t) const
    {
        for (const Due& d : due) {
            if (d.c != c && d.from >= 0 && d.from <= t && t <= d.at) {
                return true;
            }
        }
        return false;
    }

    /* Loses the other channels' Single Frames arriving in [from, to] */
    void drop_under(uint32_t c, int64_t from, int64_t to)
    {
        for (size_t i = 0; i < due.size();) {
            const Due& d = due[i];
            if (d.c != c && d.from < 0 && from <= d.at && d.at <= to) {
                ++lost;
                due.erase(due.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    void until(int64_t t_us)
    {
        while (now < t_us) {
            pass();
        }
    }

    /* The bytes channel c wrote, in order */
    std::vector<Bytes> written(uint32_t c) const
    {
        std::vector<Bytes> out;
        for (const Exchange& x : exchanges) {
            if (x.c == c) {
                out.push_back(x.bytes);
            }
        }
        return out;
    }

    std::vector<uint32_t> channels(size_t from = 0) const
    {
        std::vector<uint32_t> out;
        for (size_t i = from; i < exchanges.size(); ++i) {
            out.push_back(exchanges[i].c);
        }
        return out;
    }
};

/* Each channel asks one DID: channel seg's reply takes three frames, the
 * others' a Single Frame; the J2534 floor (5 ms) */
inline void two_modules(Bus& b, uint32_t chans = 2, uint32_t seg = 0)
{
    ASSERT_TRUE(ecuio_engine_init(&b.e, chans));
    b.e.gap_us = 5 * kMs;
    for (uint32_t c = 0; c < chans; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(0x80 * c)};
        ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                  ecuio_engine_add_result::added);
    }
    b.reply_of = [seg](uint32_t c, const ecuio_request& q) {
        Bytes out = {0x62, q.bytes[1], q.bytes[2]};
        out.insert(out.end(), c == seg ? 17u : 2u, uint8_t{0x2A});
        return out;
    };
}

} // namespace j2534_hold_test

#endif /* ECUIO_TESTS_J2534_HOLD_SUPPORT_H */
