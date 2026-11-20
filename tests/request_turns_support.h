// File/layer: tests/request_turns_support.h | test_obd2, Qt-free
// Testing: UNIT: what RequestTurnsTest's files share
//          (tests/test_request_turns.cpp,
//          tests/test_request_turns_key.cpp): an adapter that asks one
//          channel at a time with the time passed in, and its ECUs

#ifndef ECUIO_TESTS_REQUEST_TURNS_SUPPORT_H
#define ECUIO_TESTS_REQUEST_TURNS_SUPPORT_H

#include <gtest/gtest.h>

#include "../core/src/obd2_reply.h"
#include "../core/src/request_engine.h"
#include "../core/src/request_turns.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <vector>

namespace request_turns_test {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

/* What a channel's ECU answers a request: the reply, empty for silence */
using Ecus = std::function<Bytes(uint32_t, const Bytes&)>;

struct Sent {
    uint32_t c;
    Bytes bytes;
    int64_t at_us;
};

/* An adapter that asks one channel at a time, as the ELM327 worker does:
 * each pass expires the waits, takes a turn, sends and reads the answer,
 * an answered exchange taking exchange_us and silence the engine's wait */
struct Adapter {
    ecuio_request_engine e;
    ecuio_engine_turns turns;
    int64_t now = 0;
    int64_t exchange_us = 90 * kMs;
    uint32_t held = 0;
    int64_t last_wait_us = 0;
    Ecus ecus;
    /* A write that fails: the request is taken back, the try costs an
     * exchange */
    std::function<bool(uint32_t, const Bytes&)> unsent;
    std::vector<Sent> sent;

    void pass()
    {
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            (void)ecuio_engine_expire(&e, c, now);
        }
        uint32_t c = 0;
        int64_t wait = 0;
        const int32_t r =
            ecuio_engine_take_turn(&e, turns, held, now, &c, &wait);
        if (r == ECUIO_ENGINE_NONE) {
            last_wait_us = wait;
            const int64_t next =
                wait > 0 ? wait : ecuio_engine_next_event_us(&e, now);
            now += (next == 0 || next == INT64_MAX) ? kMs : next;
            return;
        }
        const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
        const Bytes bytes(q.bytes, q.bytes + q.len);
        if (unsent && unsent(c, bytes)) {
            ecuio_engine_sent(&e, c, r, false, now);
            now += exchange_us;
            return;
        }
        ecuio_engine_sent(&e, c, r, true, now);
        sent.push_back({c, bytes, now});
        const Bytes reply = ecus(c, bytes);
        if (reply.empty()) {
            now += ECUIO_ENGINE_WAIT_US;
            return;
        }
        now += exchange_us;
        (void)ecuio_engine_on_frame(&e, c, reply.data(),
                                    static_cast<uint16_t>(reply.size()), now,
                                    nullptr);
    }

    /* Passes until n requests have gone out in all */
    void asks(size_t n)
    {
        for (int i = 0; sent.size() < n && i < 100000; ++i) {
            pass();
        }
        ASSERT_GE(sent.size(), n);
    }

    /* Passes until the time is past t_us */
    void until(int64_t t_us)
    {
        for (int i = 0; now < t_us && i < 100000; ++i) {
            pass();
        }
    }

    /* The channels of the requests sent, in order, from the ith on */
    std::vector<uint32_t> channels(size_t from = 0) const
    {
        std::vector<uint32_t> out;
        for (size_t i = from; i < sent.size(); ++i) {
            out.push_back(sent[i].c);
        }
        return out;
    }
};

inline std::unique_ptr<Adapter> adapter(uint32_t chans)
{
    std::unique_ptr<Adapter> a(new Adapter);
    EXPECT_TRUE(ecuio_engine_init(&a->e, chans));
    return a;
}

inline void add(ecuio_request_engine* e, uint32_t c, const Bytes& q)
{
    ASSERT_EQ(ecuio_engine_add(e, c, q.data(), static_cast<uint8_t>(q.size())),
              ecuio_engine_add_result::added);
}

/* n DIDs on channel c, from 1E00 + first on */
inline void dids(ecuio_request_engine* e, uint32_t c, int n, int first = 0)
{
    for (int i = first; i < first + n; ++i) {
        add(e, c, {0x22, 0x1E, static_cast<uint8_t>(i)});
    }
}

/* An ECU answering every DID and every Service 01 request: J1979's
 * lengths, packs whole, each bitmap listing every PID and the next
 * bitmap, but the one at last */
inline Bytes answer(const Bytes& q, uint8_t last = 0xE0)
{
    if (q.size() == 3 && q[0] == 0x22) {
        return {0x62, q[1], q[2], 0x12, 0x34};
    }
    if (q.size() < 2 || q[0] != 0x01) {
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }
    Bytes out = {0x41};
    for (size_t i = 1; i < q.size(); ++i) {
        const uint8_t pid = q[i];
        out.push_back(pid);
        if (pid % 0x20 == 0) {
            const uint8_t end = pid == last ? 0xFE : 0xFF;
            out.insert(out.end(), {0xFF, 0xFF, 0xFF, end});
        } else {
            out.insert(out.end(), ECUIO_OBD2_PID_BYTES[pid], uint8_t{0x2A});
        }
    }
    return out;
}

/* answer(), and the extended session's entry and tester present */
inline Bytes in_session(const Bytes& q)
{
    if (q.size() == 2 && q[0] == 0x10) {
        return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
    }
    if (q.size() == 2 && q[0] == 0x3E) {
        return {0x7E, 0x00};
    }
    return answer(q);
}

/* Runs of one channel in a list of channels: {channel, length} */
inline std::vector<std::pair<uint32_t, int>>
runs(const std::vector<uint32_t>& cs)
{
    std::vector<std::pair<uint32_t, int>> out;
    for (uint32_t c : cs) {
        if (out.empty() || out.back().first != c) {
            out.push_back({c, 0});
        }
        ++out.back().second;
    }
    return out;
}

} // namespace request_turns_test

#endif /* ECUIO_TESTS_REQUEST_TURNS_SUPPORT_H */
