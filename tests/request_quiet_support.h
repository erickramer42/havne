// File/layer: tests/request_quiet_support.h | test_obd2, Qt-free
// Testing: UNIT: what RequestQuietTest's files share
//          (tests/test_request_quiet.cpp): the turns' adapter with a
//          module that sleeps, channels asked side by side and in
//          SocketCAN's order, and the steps as the rule gives them

#ifndef ECUIO_TESTS_REQUEST_QUIET_SUPPORT_H
#define ECUIO_TESTS_REQUEST_QUIET_SUPPORT_H

#include "request_turns_support.h"

#include <algorithm>

namespace request_quiet_test {

using namespace request_turns_test;

/* The times channel c's requests went out, from its kth on */
inline std::vector<int64_t> times_of(const Adapter& a, uint32_t c, size_t k = 0)
{
    std::vector<int64_t> out;
    for (const Sent& s : a.sent) {
        if (s.c == c) {
            out.push_back(s.at_us);
        }
    }
    out.erase(out.begin(),
              out.begin() + static_cast<ptrdiff_t>(std::min(k, out.size())));
    return out;
}

/* A channel's asks past from_us, and the worst gap from an ask past it */
struct Late {
    size_t asks = 0;
    int64_t worst = 0;
};

inline Late late_of(const std::vector<int64_t>& t, int64_t from_us)
{
    Late l;
    for (size_t i = 1; i < t.size(); ++i) {
        l.asks += t[i] > from_us ? 1 : 0;
        l.worst =
            t[i - 1] > from_us ? std::max(l.worst, t[i] - t[i - 1]) : l.worst;
    }
    return l;
}

inline size_t asked_between(const Adapter& a, uint32_t c, int64_t from,
                            int64_t to)
{
    size_t n = 0;
    for (const Sent& s : a.sent) {
        n += (s.c == c && s.at_us > from && s.at_us < to) ? 1 : 0;
    }
    return n;
}

/* The engine ECU's 4 rows on channel 0, answered in 30 ms; a module of
 * rows on channel 1 that answers only while awake */
struct Pair {
    std::unique_ptr<Adapter> a = adapter(2);
    bool awake = false;
    explicit Pair(int rows = 4)
    {
        dids(&a->e, 0, 4);
        dids(&a->e, 1, rows, 0x80);
        a->exchange_us = 30 * kMs;
        a->ecus = [this](uint32_t c, const Bytes& q) {
            return (c == 0 || awake) ? answer(q) : Bytes{};
        };
    }
};

/* Channels asked side by side (SocketCAN's and J2534's loops): each pass
 * expires the waits and asks each channel what it has due. Channel 0's 4
 * rows are answered 10 ms after each write; the others, 4 rows each,
 * answer nothing. */
struct SideBySide {
    ecuio_request_engine e;
    std::vector<std::vector<int64_t>> asked; /* each channel's writes */
    int64_t now = 0;
    int64_t answer0 = INT64_MAX;
    explicit SideBySide(uint32_t chans) : asked(chans)
    {
        EXPECT_TRUE(ecuio_engine_init(&e, chans));
        for (uint32_t c = 0; c < chans; ++c) {
            dids(&e, c, 4, static_cast<int>(0x10 * c));
        }
    }
    void answer_0()
    {
        const ecuio_request& q = e.requests[e.chans[0].outstanding];
        const Bytes r = answer(Bytes(q.bytes, q.bytes + q.len));
        (void)ecuio_engine_on_frame(&e, 0, r.data(),
                                    static_cast<uint16_t>(r.size()), now,
                                    nullptr);
        answer0 = INT64_MAX;
    }
    void until(int64_t t_us)
    {
        while (now < t_us) {
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                (void)ecuio_engine_expire(&e, c, now);
            }
            if (now >= answer0) {
                answer_0();
            }
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                const int32_t r = ecuio_engine_due(&e, c, now);
                if (r == ECUIO_ENGINE_NONE) {
                    continue;
                }
                ecuio_engine_sent(&e, c, r, true, now);
                asked[c].push_back(now);
                answer0 = c == 0 ? now + 10 * kMs : answer0;
            }
            const int64_t next = ecuio_engine_next_event_us(&e, now);
            const int64_t step = std::min(next, answer0 - now);
            ASSERT_NE(step, INT64_MAX);
            now += std::max<int64_t>(step, 1);
        }
    }
};

/* The steps as the rule gives them, written out so a mutant of the
 * engine's table is judged against them */
constexpr int64_t kSteps[] = {500 * kMs, 1000 * kMs, 2000 * kMs, 4000 * kMs,
                              5000 * kMs};

/* Channel 1's rows from the third: one at each step, each gap its step
 * and at most a pass more (the other channel's exchange, a wait) */
inline void expect_steps(const std::vector<int64_t>& t)
{
    for (size_t i = 1; i < t.size(); ++i) {
        const int64_t step = kSteps[std::min<size_t>(i, 4)];
        EXPECT_GE(t[i] - t[i - 1], step) << "gap " << i;
        EXPECT_LE(t[i] - t[i - 1], step + ECUIO_ENGINE_WAIT_US) << "gap " << i;
    }
}

/* Channel c's identification items whose outcome is o */
inline size_t items_of(const ecuio_request_engine& e, uint32_t c, uint8_t o)
{
    ecuio_channel_identity_t id{};
    ecuio_engine_identity(&e, c, &id);
    size_t n = 0;
    for (uint32_t i = 0; i < id.items; ++i) {
        n += id.list[i].outcome == o ? 1 : 0;
    }
    return n;
}

/* The engine ECU's 4 rows on channel 0, answered in 30 ms; on channel 1
 * a module with no rows (nothing picked of it), identification on, that
 * answers as ecu1 says */
inline std::unique_ptr<Adapter> rowless(const Ecus& ecu1)
{
    auto a = adapter(2);
    dids(&a->e, 0, 4);
    a->exchange_us = 30 * kMs;
    a->ecus = [ecu1](uint32_t c, const Bytes& q) {
        return c == 0 ? answer(q) : ecu1(c, q);
    };
    ecuio_engine_identify(&a->e);
    return a;
}

/* Side by side, passes until channel 1 has a request out */
inline void to_request_out(SideBySide& b)
{
    for (int i = 0; i < 100000 && b.e.chans[1].outstanding == ECUIO_ENGINE_NONE;
         ++i) {
        b.until(b.now + 1);
    }
}

/* SocketCAN's pass: one request at a time, the first due from the channel
 * after the last asked. Channel 1's 4 rows are answered 10 ms after each
 * write; the others, 4 rows each, answer nothing. */
struct Rotating {
    ecuio_request_engine e;
    std::vector<std::vector<int64_t>> asked; /* each channel's writes */
    int64_t now = 0;
    int64_t answer1 = INT64_MAX;
    uint32_t next_chan = 0;
    explicit Rotating(uint32_t chans) : asked(chans)
    {
        EXPECT_TRUE(ecuio_engine_init(&e, chans));
        for (uint32_t c = 0; c < chans; ++c) {
            dids(&e, c, 4, static_cast<int>(0x10 * c));
        }
    }
    void answer_1()
    {
        const ecuio_request& q = e.requests[e.chans[1].outstanding];
        const Bytes r = answer(Bytes(q.bytes, q.bytes + q.len));
        (void)ecuio_engine_on_frame(
            &e, 1, r.data(), static_cast<uint16_t>(r.size()), now, nullptr);
        answer1 = INT64_MAX;
    }
    /* The first request due from next_chan on, sent; false with none */
    bool send_due()
    {
        for (uint32_t k = 0; k < e.chan_count; ++k) {
            const uint32_t c = (next_chan + k) % e.chan_count;
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r != ECUIO_ENGINE_NONE) {
                ecuio_engine_sent(&e, c, r, true, now);
                asked[c].push_back(now);
                answer1 = c == 1 ? now + 10 * kMs : answer1;
                next_chan = (c + 1) % e.chan_count;
                return true;
            }
        }
        return false;
    }
    void until(int64_t t_us)
    {
        while (now < t_us) {
            for (uint32_t c = 0; c < e.chan_count; ++c) {
                (void)ecuio_engine_expire(&e, c, now);
            }
            if (now >= answer1) {
                answer_1();
            }
            if (send_due()) {
                now += kMs; /* the echo */
                continue;
            }
            const int64_t next = ecuio_engine_next_event_us(&e, now);
            const int64_t step = std::min(next, answer1 - now);
            ASSERT_NE(step, INT64_MAX);
            now += std::max<int64_t>(step, 1);
        }
    }
};

} // namespace request_quiet_test

#endif
