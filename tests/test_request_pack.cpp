// File/layer: tests/test_request_pack.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestPackTest (a Service 01 pack sized by its reply's
//          CAN frames, ?packframes=, ADR-0039 Appendix A; the engine
//          stepped with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/isotp.h"
#include "../core/src/obd2_rates.h"
#include "../core/src/obd2_reply.h"
#include "../core/src/request_engine.h"
#include "../core/src/request_params.h"
#include "ecuio/signal_sets.h"
#include "fake_ecus.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

/* What channel c's ECU answers a request: the reply, empty for silence */
using Ecus = std::function<Bytes(uint32_t, const Bytes&)>;

struct Sent {
    uint32_t c;
    Bytes bytes;
    Bytes reply;
};

/* Each channel asked in turn, each request answered at once (silence
 * waits out the wait), the floor between a channel's requests */
struct Driver {
    ecuio_request_engine e;
    int64_t now = 0;
    Ecus ecus;
    std::vector<Sent> sent;

    void pass()
    {
        bool asked = false;
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            (void)ecuio_engine_expire(&e, c, now);
            const int32_t r = ecuio_engine_due(&e, c, now);
            if (r == ECUIO_ENGINE_NONE) {
                continue;
            }
            asked = true;
            const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
            const Bytes bytes(q.bytes, q.bytes + q.len);
            EXPECT_EQ(r == ECUIO_ENGINE_PACK, bytes.size() > 2);
            ecuio_engine_sent(&e, c, r, true, now);
            const Bytes reply = ecus(c, bytes);
            sent.push_back({c, bytes, reply});
            if (!reply.empty()) {
                (void)ecuio_engine_on_frame(&e, c, reply.data(),
                                            static_cast<uint16_t>(reply.size()),
                                            now, nullptr);
            }
        }
        now += asked ? ECUIO_ENGINE_GAP_US : ECUIO_ENGINE_WAIT_US;
    }

    void asks(size_t n)
    {
        for (int i = 0; sent.size() < n && i < 100000; ++i) {
            pass();
        }
        ASSERT_GE(sent.size(), n);
    }

    /* Channel c's requests, from the ith sent on */
    std::vector<Bytes> of(uint32_t c, size_t from = 0) const
    {
        std::vector<Bytes> out;
        for (size_t i = from; i < sent.size(); ++i) {
            if (sent[i].c == c) {
                out.push_back(sent[i].bytes);
            }
        }
        return out;
    }
};

std::unique_ptr<Driver> driver(uint32_t chans, uint8_t frames)
{
    std::unique_ptr<Driver> d(new Driver);
    EXPECT_TRUE(ecuio_engine_init(&d->e, chans));
    d->e.pack = true;
    d->e.pack_frames = frames;
    return d;
}

void add(ecuio_request_engine* e, uint32_t c, uint8_t pid)
{
    const uint8_t q[2] = {0x01, pid};
    ASSERT_EQ(ecuio_engine_add(e, c, q, 2), ecuio_engine_add_result::added);
}

/* An ECU answering Service 01 in J1979's form, each PID at len's length
 * (J1979's where len has none), packs whole; each bitmap lists every PID
 * and the next bitmap, but the one at last */
Bytes answer(const Bytes& q, const std::map<uint8_t, uint8_t>& len = {},
             uint8_t last = 0x00)
{
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
            continue;
        }
        const auto it = len.find(pid);
        const uint8_t n =
            it != len.end() ? it->second : ECUIO_OBD2_PID_BYTES[pid];
        out.insert(out.end(), n, uint8_t{0x2A});
    }
    return out;
}

/* The fakes' ECU (tests/fake_ecu.h), which answers packs in J1979's form */
Bytes fake_answer(const Bytes& q)
{
    unsigned char out[fake_ecu::kReplyMax] = {};
    size_t n = 0;
    if (!fake_ecu::answer(q.data(), q.size(), -1, out, &n)) {
        return {};
    }
    return Bytes(out, out + n);
}

/* The obd2 set's channel as the setup builds it (its classes and its
 * duplicate pairs), packing on at frames, answered by the fakes' ECU */
std::unique_ptr<Driver> obd2(uint8_t frames)
{
    std::unique_ptr<Driver> d = driver(1, frames);
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    EXPECT_TRUE(
        ecuio_engine_derive(&d->e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(&d->e, ecuio_obd2_pid_fast);
    ecuio_engine_set_duplicates(&d->e, &ECUIO_OBD2_DUPLICATE_PIDS[0][0],
                                sizeof(ECUIO_OBD2_DUPLICATE_PIDS) /
                                    sizeof(ECUIO_OBD2_DUPLICATE_PIDS[0]));
    ecuio_engine_probe(&d->e);
    d->ecus = [](uint32_t, const Bytes& q) { return fake_answer(q); };
    return d;
}

bool bitmap(const Bytes& q)
{
    return q.size() == 2 && q[0] == 0x01 && q[1] % 0x20 == 0;
}

/* PID pid's request on channel c is slow */
bool slow(const ecuio_request_engine& e, uint32_t c, uint8_t pid)
{
    for (uint32_t i = 0; i < e.count; ++i) {
        const ecuio_request& r = e.requests[i];
        if (r.channel == c && r.len == 2 && r.bytes[0] == 0x01 &&
            r.bytes[1] == pid) {
            return r.slow != 0;
        }
    }
    return false;
}

/* The PIDs of one class, in the order asked, packs flattened */
std::vector<uint8_t> class_order(const ecuio_request_engine& e, uint32_t c,
                                 const std::vector<Bytes>& sent, bool of_slow)
{
    std::vector<uint8_t> out;
    for (const Bytes& q : sent) {
        if (bitmap(q) || slow(e, c, q[1]) != of_slow) {
            continue;
        }
        out.insert(out.end(), q.begin() + 1, q.end());
    }
    return out;
}

} // namespace

/* Under packframes=1 a pack holds six record bytes (a reply of 7, one
 * Single Frame): the PID that would pass them stops the pack and heads
 * the next, so the packs go around the rotation in set order */
TEST(RequestPackTest, APackStopsBeforeThePidThatPassesTheLimit)
{
    auto d = driver(1, 1);
    const uint8_t pids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11};
    for (const uint8_t pid : pids) {
        add(&d->e, 0, pid);
    }
    ecuio_engine_probe(&d->e);
    d->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    /* the bitmap, then eight alone: no length known yet */
    ASSERT_NO_FATAL_FAILURE(d->asks(9));
    ASSERT_NO_FATAL_FAILURE(d->asks(9 + 7));
    const std::vector<Bytes> want = {
        {0x01, 0x04, 0x05, 0x0B}, /* 2 + 2 + 2 */
        {0x01, 0x0C, 0x0D},       /* 3 + 2: 0F's 2 would make 7 */
        {0x01, 0x0F, 0x10},       /* 2 + 3 */
        {0x01, 0x11, 0x04, 0x05}, /* around the rotation */
        {0x01, 0x0B, 0x0C},       /* 2 + 3 */
        {0x01, 0x0D, 0x0F},       /* 2 + 2: 10's 3 would make 7 */
        {0x01, 0x10, 0x11},       /* 3 + 2 */
    };
    EXPECT_EQ(d->of(0, 9), want);
    for (size_t i = 9; i < d->sent.size(); ++i) {
        EXPECT_LE(d->sent[i].reply.size(), 7u) << "one Single Frame";
    }
    ecuio_channel_support_t s{};
    ecuio_engine_support(&d->e, 0, &s);
    EXPECT_TRUE(s.flags & ECUIO_SUPPORT_PACKED);
}

/* On the obd2 set, at every limit: each pack's reply fits its frames and
 * stops only at J1979's six PIDs or where the class's next PID would pass
 * the limit; each class is asked round and round in its own order, so no
 * PID is left out or asked twice in a lap; and a limit no pack reaches is
 * no limit */
TEST(RequestPackTest, EveryPackFitsItsLimitAndEveryRowIsAskedEachLap)
{
    std::vector<Bytes> unlimited;
    for (uint8_t frames = 0; frames <= ECUIO_REQUEST_PACK_FRAMES_MAX;
         ++frames) {
        SCOPED_TRACE("packframes " + std::to_string(frames));
        auto d = obd2(frames);
        ASSERT_NO_FATAL_FAILURE(d->asks(600));
        const ecuio_request_engine& e = d->e;
        const ecuio_engine_channel& ch = e.chans[0];
        ASSERT_FALSE(ch.unpacked);
        const size_t from = 300; /* past the walk and the first laps */
        size_t packs = 0;
        for (size_t i = from; i < d->sent.size(); ++i) {
            const Sent& s = d->sent[i];
            ASSERT_FALSE(s.reply.empty());
            if (s.bytes.size() <= 2) {
                continue;
            }
            ++packs;
            if (frames != 0) {
                EXPECT_LE(isotp_frames(static_cast<uint32_t>(s.reply.size())),
                          frames)
                    << "pack " << i;
            }
        }
        EXPECT_GT(packs, 0u);
        for (const bool of_slow : {false, true}) {
            const std::vector<uint8_t> order =
                class_order(e, 0, d->of(0, from), of_slow);
            const std::set<uint8_t> kept(order.begin(), order.end());
            ASSERT_GT(order.size(), 3 * kept.size());
            for (size_t k = kept.size(); k < order.size(); ++k) {
                ASSERT_EQ(order[k], order[k - kept.size()])
                    << (of_slow ? "slow" : "fast") << " class, ask " << k;
            }
        }
        /* each pack ends at six PIDs, or its class's next would pass */
        const std::vector<Bytes> sent = d->of(0, from);
        for (size_t i = 0; frames != 0 && i + 1 < sent.size(); ++i) {
            const Bytes& p = sent[i];
            if (p.size() <= 2 || p.size() == 1 + ECUIO_ENGINE_PACK_MAX) {
                continue;
            }
            const bool s = slow(e, 0, p[1]);
            size_t n = i + 1;
            while (n < sent.size() &&
                   (bitmap(sent[n]) || slow(e, 0, sent[n][1]) != s)) {
                ++n;
            }
            if (n == sent.size()) {
                break;
            }
            uint32_t reply = 1;
            for (size_t k = 1; k < p.size(); ++k) {
                reply += 1u + ch.pid_len[p[k]];
            }
            const uint8_t next = sent[n][1];
            EXPECT_GT(isotp_frames(reply + 1u + ch.pid_len[next]), frames)
                << "pack " << i << " stopped before PID " << int{next};
        }
        if (frames == 0) {
            unlimited = d->of(0);
        } else if (frames == ECUIO_REQUEST_PACK_FRAMES_MAX) {
            EXPECT_EQ(d->of(0), unlimited) << "15 frames is no limit";
        }
    }
}

/* A PID whose own reply passes the limit (68's seven data bytes, a reply
 * of 9, under packframes=1) goes alone, never dropped, never packed */
TEST(RequestPackTest, APidLongerThanTheLimitGoesAlone)
{
    auto d = driver(1, 1);
    const uint8_t pids[] = {0x04, 0x68, 0x05, 0x0B};
    for (const uint8_t pid : pids) {
        add(&d->e, 0, pid);
    }
    ecuio_engine_probe(&d->e);
    d->ecus = [](uint32_t, const Bytes& q) { return answer(q, {}, 0x60); };
    /* four bitmaps, then four alone */
    ASSERT_NO_FATAL_FAILURE(d->asks(8 + 12));
    size_t alone = 0;
    for (const Bytes& q : d->of(0, 8)) {
        if (q == Bytes{0x01, 0x68}) {
            ++alone;
        } else {
            EXPECT_EQ(std::count(q.begin(), q.end(), uint8_t{0x68}), 0);
        }
    }
    EXPECT_GE(alone, 3u);
    for (uint32_t i = 0; i < d->e.count; ++i) {
        EXPECT_EQ(d->e.requests[i].dropped, 0u);
    }
}

/* A small pack failing is a pack failing: three in a row unpack the
 * channel, and a recovery packs it again within the limit */
TEST(RequestPackTest, TheFallbackCountsSmallPacksAsPacks)
{
    auto d = driver(1, 1);
    const uint8_t pids[] = {0x04, 0x05, 0x0B, 0x0D};
    for (const uint8_t pid : pids) {
        add(&d->e, 0, pid);
    }
    ecuio_engine_probe(&d->e);
    bool refuse_packs = true;
    d->ecus = [&refuse_packs](uint32_t, const Bytes& q) {
        return refuse_packs && q.size() > 2 ? Bytes{} : answer(q);
    };
    ASSERT_NO_FATAL_FAILURE(d->asks(5 + 12));
    size_t packs = 0;
    for (const Bytes& q : d->of(0)) {
        packs += q.size() > 2 ? 1 : 0;
    }
    EXPECT_EQ(packs, ECUIO_ENGINE_PACK_FAILURES);
    EXPECT_TRUE(d->e.chans[0].unpacked);

    refuse_packs = false;
    ecuio_engine_rescan(&d->e);
    const size_t before = d->sent.size();
    ASSERT_NO_FATAL_FAILURE(d->asks(before + 12));
    size_t again = 0;
    for (size_t i = before; i < d->sent.size(); ++i) {
        if (d->sent[i].bytes.size() > 2) {
            ++again;
            EXPECT_LE(d->sent[i].reply.size(), 7u);
        }
    }
    EXPECT_GT(again, 0u);
    EXPECT_FALSE(d->e.chans[0].unpacked);
}

/* One limit, two modules: each channel's packs follow the lengths its
 * own ECU showed, so the same rows pack differently (channel 1's ECU
 * answers 0C with one data byte where J1979 gives two: a shorter reply
 * teaches its length, ADR-0039 Appendix A's amendment) */
TEST(RequestPackTest, TwoChannelsSizeTheirPacksByTheirOwnLengths)
{
    auto d = driver(2, 1);
    const uint8_t pids[] = {0x0C, 0x0D, 0x04, 0x05};
    for (uint32_t c = 0; c < 2; ++c) {
        for (const uint8_t pid : pids) {
            add(&d->e, c, pid);
        }
    }
    ecuio_engine_probe(&d->e);
    const std::map<uint8_t, uint8_t> short_0c = {{0x0C, 1}};
    d->ecus = [&short_0c](uint32_t c, const Bytes& q) {
        return c == 0 ? answer(q) : answer(q, short_0c);
    };
    /* each channel's bitmap, then its four alone */
    ASSERT_NO_FATAL_FAILURE(d->asks(2 * 5 + 8));
    EXPECT_EQ(d->e.chans[0].pid_len[0x0C], 2u);
    EXPECT_EQ(d->e.chans[1].pid_len[0x0C], 1u) << "learned short";
    const std::vector<Bytes> zero = d->of(0, 10);
    const std::vector<Bytes> one = d->of(1, 10);
    ASSERT_GE(zero.size(), 2u);
    ASSERT_GE(one.size(), 2u);
    EXPECT_EQ(zero[0], (Bytes{0x01, 0x0C, 0x0D}))
        << "3 + 2: 04's 2 would make 7";
    EXPECT_EQ(one[0], (Bytes{0x01, 0x0C, 0x0D, 0x04})) << "2 + 2 + 2";
}

/* A sent pack stamps each of its rows with the write's time (the J2534
 * hold's overdue order reads a channel's next row's), and a write that
 * failed stamps none */
TEST(RequestPackTest, ASentPackStampsEachOfItsRows)
{
    auto d = driver(1, 0);
    for (const uint8_t pid : {uint8_t{0x0C}, uint8_t{0x0D}, uint8_t{0x05}}) {
        add(&d->e, 0, pid);
    }
    ecuio_engine_probe(&d->e);
    d->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    int64_t at = -1;
    for (int i = 0; i < 1000 && at < 0; ++i) {
        const int64_t now = d->now;
        d->pass();
        if (!d->sent.empty() && d->sent.back().bytes.size() > 2) {
            at = now;
        }
    }
    ASSERT_GE(at, 0) << "no pack was asked";
    const Bytes pack = d->sent.back().bytes;
    for (uint32_t i = 0; i < d->e.count; ++i) {
        const ecuio_request& r = d->e.requests[i];
        if (std::find(pack.begin() + 1, pack.end(), r.bytes[1]) != pack.end()) {
            EXPECT_EQ(d->e.asked_us[i], at) << "PID " << int{r.bytes[1]};
        }
    }

    const int64_t later = d->now + ECUIO_ENGINE_WAIT_US;
    (void)ecuio_engine_expire(&d->e, 0, later);
    const int32_t r = ecuio_engine_due(&d->e, 0, later);
    ASSERT_NE(r, ECUIO_ENGINE_NONE);
    ecuio_engine_sent(&d->e, 0, r, false, later);
    for (uint32_t i = 0; i < d->e.count; ++i) {
        EXPECT_LT(d->e.asked_us[i], later) << "row " << i;
    }
}

/* Out of step, a late pack's reply that starts with the PID of a row out
 * alone answers the row but teaches it nothing: its frames and its PID's
 * length stay the ones its own replies showed */
TEST(RequestPackTest, ALatePacksReplyTeachesARowOutAloneNothing)
{
    auto d = driver(1, 0);
    const uint8_t pids[] = {0x0C, 0x10, 0x14, 0x15, 0x16, 0x1F};
    for (const uint8_t pid : pids) {
        add(&d->e, 0, pid);
    }
    ecuio_engine_probe(&d->e);
    d->ecus = [](uint32_t, const Bytes& q) { return answer(q); };
    for (int i = 0;
         i < 1000 && (d->sent.empty() || d->sent.back().bytes.size() <= 2);
         ++i) {
        d->pass();
    }
    ASSERT_GT(d->sent.back().bytes.size(), 2u) << "no pack was asked";
    ecuio_request_engine& e = d->e;
    int64_t now = d->now;
    for (uint32_t failed = 0; failed < ECUIO_ENGINE_PACK_FAILURES;) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            now += ECUIO_ENGINE_GAP_US;
            continue;
        }
        ASSERT_EQ(r, ECUIO_ENGINE_PACK) << "pack " << failed;
        ecuio_engine_sent(&e, 0, r, true, now);
        now += ECUIO_ENGINE_WAIT_US + 1;
        ASSERT_TRUE(ecuio_engine_expire(&e, 0, now));
        ++failed;
    }
    ASSERT_TRUE(e.chans[0].unpacked);
    ASSERT_FALSE(e.chans[0].in_step);
    int32_t r = ECUIO_ENGINE_NONE;
    for (int i = 0; i < 100 && r == ECUIO_ENGINE_NONE; ++i) {
        now += ECUIO_ENGINE_GAP_US;
        r = ecuio_engine_due(&e, 0, now);
    }
    ASSERT_GE(r, 0) << "a row alone";
    const uint8_t pid = e.requests[r].bytes[1];
    const uint8_t frames = e.requests[r].reply_frames;
    const uint8_t pid_len = e.chans[0].pid_len[pid];
    ASSERT_EQ(frames, 1);
    ASSERT_EQ(pid_len, ECUIO_OBD2_PID_BYTES[pid]);
    ecuio_engine_sent(&e, 0, r, true, now);

    Bytes late_pack = {0x01, pid};
    for (const uint8_t other : pids) {
        if (other != pid) {
            late_pack.push_back(other);
        }
    }
    const Bytes late = answer(late_pack);
    ASSERT_GT(isotp_frames(static_cast<uint32_t>(late.size())), 1u);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, late.data(),
                                    static_cast<uint16_t>(late.size()), now + 1,
                                    nullptr),
              ecuio_reply::positive);
    EXPECT_TRUE(e.chans[0].in_step);
    EXPECT_EQ(e.requests[r].reply_frames, frames);
    EXPECT_EQ(e.chans[0].pid_len[pid], pid_len);
}
