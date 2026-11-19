// File/layer: tests/test_j2534_hold.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest (core/src/j2534_hold.h: the J2534 worker's
//          hold for Known Issue #42, against a bus of modules answering
//          with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/j2534_hold.h"
#include "../core/src/link_health.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace {

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
 * after first_us, each frame more 5 ms later; a module's reply_of gives
 * its answer (empty: silence; 7F xx 78 keeps the request out). The pass is
 * the worker's, j2534_hold_pass, or (plain) the pass a worker makes with
 * no hold, written out: each channel's wait ended and its due request
 * written in turn. */
struct Bus {
    ecuio_request_engine e;
    j2534_hold hold;
    bool plain = false;
    int64_t now = 0;
    int64_t first_us = 3 * kMs;
    std::function<Bytes(uint32_t, const ecuio_request&)> reply_of;
    std::vector<Exchange> exchanges;
    struct Due {
        uint32_t c;
        int64_t at;
        Bytes bytes;
        size_t exchange;
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
        if (!reply.empty()) {
            due.push_back({c, now + first_us + (frames - 1) * 5 * kMs, reply,
                           exchanges.size() - 1});
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
void two_modules(Bus& b, uint32_t chans = 2, uint32_t seg = 0)
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

/* Whether exchange a, a segmented one, overlaps any other channel's
 * exchange on the bus */
bool overlaps_another(const std::vector<Exchange>& xs, size_t a)
{
    for (size_t i = 0; i < xs.size(); ++i) {
        if (i != a && xs[i].c != xs[a].c && xs[i].sent_us < xs[a].ended_us &&
            xs[a].sent_us < xs[i].ended_us) {
            return true;
        }
    }
    return false;
}

/* Each channel's writes against a pass with no hold: the same requests in
 * the same order, as far as the shorter run goes, and at least min each */
void expect_dues_order(const Bus& held, const Bus& plain, size_t min)
{
    for (uint32_t c = 0; c < held.e.chan_count; ++c) {
        const std::vector<Bytes> a = held.written(c);
        const std::vector<Bytes> b = plain.written(c);
        ASSERT_GE(a.size(), min) << "channel " << c;
        ASSERT_GE(b.size(), min) << "channel " << c;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            ASSERT_EQ(a[i], b[i]) << "channel " << c << ", write " << i;
        }
    }
}

/* How many of channel c's writes were chosen requests given back, of
 * those what picks */
size_t released(const Bus& b, uint32_t c,
                const std::function<bool(const Bytes&)>& what)
{
    size_t n = 0;
    for (const Exchange& x : b.exchanges) {
        n += x.c == c && x.released && what(x.bytes) ? 1u : 0u;
    }
    return n;
}

/* A Service 01 ECU at J1979's lengths, every bitmap listing every PID and
 * the next, packs answered whole; a DID's answer two bytes; any other
 * service refused */
Bytes obd2_ecu(const ecuio_request& q)
{
    if (q.bytes[0] == 0x22) {
        return {0x62, q.bytes[1], q.bytes[2], 0x12, 0x34};
    }
    if (q.bytes[0] != 0x01) {
        return {0x7F, q.bytes[0], 0x11};
    }
    Bytes out = {0x41};
    for (uint8_t i = 1; i < q.len; ++i) {
        const uint8_t pid = q.bytes[i];
        out.push_back(pid);
        if (pid % 0x20 == 0) {
            const uint8_t bitmap[4] = {0xFF, 0xFF, 0xFF,
                                       uint8_t(pid == 0xE0 ? 0xFE : 0xFF)};
            out.insert(out.end(), bitmap, bitmap + 4);
        } else {
            out.insert(out.end(), ECUIO_OBD2_PID_BYTES[pid], uint8_t{0x2A});
        }
    }
    return out;
}

} // namespace

/* Once channel 0's reply is known to take three frames, each of its
 * exchanges is alone on the bus: no other channel's request is out while
 * it is (the first, not yet learned, goes as any) */
TEST(J2534HoldTest, ASegmentedExchangeIsAloneOnTheBus)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(2000 * kMs);
    size_t segmented = 0;
    bool learned = false;
    for (size_t i = 0; i < b.exchanges.size(); ++i) {
        if (b.exchanges[i].c != 0) {
            continue;
        }
        if (learned) {
            ++segmented;
            EXPECT_FALSE(overlaps_another(b.exchanges, i)) << "exchange " << i;
        }
        learned = true;
    }
    EXPECT_GT(segmented, 50u);
    for (const Exchange& x : b.exchanges) {
        EXPECT_TRUE(x.answered || x.ended_us == INT64_MAX)
            << "every request answered but the last ones out";
    }
}

/* The channels take turns: a held channel goes first when the hold ends,
 * so from channel 0's first reply (its frames learned) the writes
 * alternate 0, 1, 0, 1 */
TEST(J2534HoldTest, TheHeldChannelGoesNext)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(500 * kMs);
    ASSERT_EQ(b.exchanges[0].c, 0u);
    size_t from = 0;
    while (from < b.exchanges.size() &&
           b.exchanges[from].sent_us < b.exchanges[0].ended_us) {
        ++from;
    }
    const std::vector<uint32_t> cs = b.channels(from);
    ASSERT_GT(cs.size(), 20u);
    for (size_t i = 1; i < cs.size(); ++i) {
        EXPECT_NE(cs[i], cs[i - 1]) << "write " << from + i;
    }
}

/* The hold costs the segmented channel the other's exchange and nothing
 * more, whichever channel it is: with modules answering in 6 ms (past
 * the 5 ms floor), the two are asked equally often, once for each 16 ms
 * exchange and 6 ms one. With the segmented channel second in the pass,
 * its chosen request is what keeps the first from starting another. */
TEST(J2534HoldTest, EachChannelKeepsAPace)
{
    for (const uint32_t seg : {0u, 1u}) {
        Bus b;
        ASSERT_NO_FATAL_FAILURE(two_modules(b, 2, seg));
        b.first_us = 6 * kMs;
        b.until(1000 * kMs);
        size_t n[2] = {};
        for (const Exchange& x : b.exchanges) {
            ++n[x.c];
        }
        /* about 1000 / 22 each */
        EXPECT_GT(n[seg], 38u) << "segmented channel " << seg;
        EXPECT_LT(n[seg], 52u) << "segmented channel " << seg;
        EXPECT_LE(n[0] > n[1] ? n[0] - n[1] : n[1] - n[0], 1u)
            << "segmented channel " << seg;
    }
}

/* A chosen request waits only for the replies out: a module falling
 * silent holds it for that request's wait, and it is written then; gone
 * silent, the module's next requests keep nothing waiting */
TEST(J2534HoldTest, AChosenRequestWaitsForASilentModuleOnce)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(200 * kMs); /* channel 0's reply learned */
    b.reply_of = [](uint32_t c, const ecuio_request& q) {
        if (c == 1) {
            return Bytes{}; /* channel 1 falls silent */
        }
        Bytes out = {0x62, q.bytes[1], q.bytes[2]};
        out.insert(out.end(), 17u, uint8_t{0x2A});
        return out;
    };
    const size_t from = b.exchanges.size();
    b.until(1200 * kMs);
    const Exchange* silent = nullptr;
    const Exchange* first = nullptr;
    size_t zero = 0;
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        const Exchange& x = b.exchanges[i];
        silent = silent == nullptr && x.c == 1 ? &x : silent;
        first = first == nullptr && x.c == 0 ? &x : first;
        zero += x.c == 0 ? 1u : 0u;
    }
    ASSERT_NE(silent, nullptr);
    ASSERT_NE(first, nullptr);
    EXPECT_GE(first->sent_us, silent->ended_us) << "the first wait is held";
    /* then channel 0's 13 ms exchanges back to back: about 70 in 900 ms;
     * waiting out each of channel 1's 100 ms waits, about 9 */
    EXPECT_GT(zero, 50u);
}

/* Off (?hold=0) or with one channel, the hold is due itself: the same
 * writes at the same times as a worker with no hold */
TEST(J2534HoldTest, OffOrWithOneChannelTheHoldIsDue)
{
    for (const uint32_t chans : {2u, 1u}) {
        Bus plain;
        ASSERT_NO_FATAL_FAILURE(two_modules(plain, chans));
        plain.plain = true;
        plain.until(300 * kMs);
        Bus held;
        ASSERT_NO_FATAL_FAILURE(two_modules(held, chans));
        held.hold.on = chans == 1; /* one channel: on, and no hold */
        held.until(300 * kMs);
        ASSERT_EQ(held.exchanges.size(), plain.exchanges.size()) << chans;
        for (size_t i = 0; i < plain.exchanges.size(); ++i) {
            EXPECT_EQ(held.exchanges[i].c, plain.exchanges[i].c);
            EXPECT_EQ(held.exchanges[i].sent_us, plain.exchanges[i].sent_us);
        }
        if (chans == 2) {
            bool any = false;
            for (size_t i = 2; i < plain.exchanges.size(); ++i) {
                any = any || (plain.exchanges[i].c == 0 &&
                              overlaps_another(plain.exchanges, i));
            }
            EXPECT_TRUE(any) << "with no hold the exchanges overlap";
        }
    }
}

/* Three channels, one segmented: the two Single Frame channels may share
 * the bus with each other, never with the segmented one */
TEST(J2534HoldTest, SingleFramesShareTheBusButNotWithASegmentedReply)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b, 3));
    b.until(1000 * kMs);
    bool shared = false;
    bool learned = false;
    for (size_t i = 0; i < b.exchanges.size(); ++i) {
        const Exchange& x = b.exchanges[i];
        if (x.c == 0) {
            if (learned) {
                EXPECT_FALSE(overlaps_another(b.exchanges, i)) << i;
            }
            learned = true;
            continue;
        }
        for (const Exchange& y : b.exchanges) {
            shared =
                shared || (y.c != x.c && y.c != 0 && y.sent_us < x.ended_us &&
                           x.sent_us < y.ended_us);
        }
    }
    EXPECT_TRUE(shared) << "channels 1 and 2 overlap";
}

/* Channel 0 asks three rows, one long, with modules answering in 6 ms
 * (past the floor, so the long row often comes due with channel 1 out):
 * each channel writes due's requests in due's order, so no row is skipped
 * while a chosen one waits and none is asked twice when it is given back */
TEST(J2534HoldTest, EachChannelWritesItsRowsInDuesOrder)
{
    const auto make = [](Bus& b) {
        ASSERT_NO_FATAL_FAILURE(two_modules(b));
        b.first_us = 6 * kMs;
        for (const uint8_t k : {uint8_t{0x01}, uint8_t{0x02}}) {
            const uint8_t did[3] = {0x22, 0x1E, k};
            ASSERT_EQ(ecuio_engine_add(&b.e, 0, did, 3),
                      ecuio_engine_add_result::added);
        }
        b.reply_of = [](uint32_t c, const ecuio_request& q) {
            Bytes out = {0x62, q.bytes[1], q.bytes[2]};
            const bool long_one = c == 0 && q.bytes[2] == 0x00;
            out.insert(out.end(), long_one ? 17u : 2u, uint8_t{0x2A});
            return out;
        };
    };
    Bus held;
    Bus plain;
    ASSERT_NO_FATAL_FAILURE(make(held));
    ASSERT_NO_FATAL_FAILURE(make(plain));
    plain.plain = true;
    held.until(1000 * kMs);
    plain.until(1000 * kMs);
    expect_dues_order(held, plain, 50);
    EXPECT_GE(released(held, 0, [](const Bytes& q) { return q[2] == 0x00; }),
              1u)
        << "the long row waited as chosen";
}

/* A pack expected segmented (six PIDs, three frames) waits as chosen and
 * goes out whole: the same packs in the same order as with no hold */
TEST(J2534HoldTest, AChosenPackGoesOutWhole)
{
    const auto make = [](Bus& b) {
        ASSERT_TRUE(ecuio_engine_init(&b.e, 2));
        b.e.gap_us = 5 * kMs;
        b.e.pack = true;
        const uint8_t pids[] = {0x0C, 0x0D, 0x05, 0x0F, 0x11, 0x0B};
        for (const uint8_t pid : pids) {
            const uint8_t q[2] = {0x01, pid};
            ASSERT_EQ(ecuio_engine_add(&b.e, 0, q, 2),
                      ecuio_engine_add_result::added);
        }
        const uint8_t did[3] = {0x22, 0x1E, 0x80};
        ASSERT_EQ(ecuio_engine_add(&b.e, 1, did, 3),
                  ecuio_engine_add_result::added);
        ecuio_engine_probe(&b.e);
        b.reply_of = [](uint32_t, const ecuio_request& q) {
            return obd2_ecu(q);
        };
    };
    Bus held;
    Bus plain;
    ASSERT_NO_FATAL_FAILURE(make(held));
    ASSERT_NO_FATAL_FAILURE(make(plain));
    plain.plain = true;
    held.until(1000 * kMs);
    plain.until(1000 * kMs);
    expect_dues_order(held, plain, 30);
    EXPECT_GT(
        released(held, 0,
                 [](const Bytes& q) { return q[0] == 0x01 && q.size() == 7; }),
        5u)
        << "a six-PID pack waited as chosen";
}

/* Identification on two channels: every item is expected segmented, so
 * the channels take turns item by item, each list in its own order, and no
 * two exchanges share the bus */
TEST(J2534HoldTest, IdentificationOnTwoChannelsTakesTurns)
{
    const auto make = [](Bus& b) {
        ASSERT_TRUE(ecuio_engine_init(&b.e, 2));
        b.e.gap_us = 5 * kMs;
        ecuio_engine_identify(&b.e);
        b.reply_of = [](uint32_t, const ecuio_request& q) {
            if (q.len == 2 && q.bytes[0] == 0x09 && q.bytes[1] == 0x02) {
                Bytes vin = {0x49, 0x02, 0x01};
                vin.insert(vin.end(), 17u, uint8_t{0x41});
                return vin;
            }
            return Bytes{0x7F, q.bytes[0], 0x31};
        };
    };
    Bus held;
    Bus plain;
    ASSERT_NO_FATAL_FAILURE(make(held));
    ASSERT_NO_FATAL_FAILURE(make(plain));
    plain.plain = true;
    held.until(1000 * kMs);
    plain.until(1000 * kMs);
    expect_dues_order(held, plain, ECUIO_IDENT_COUNT);
    for (size_t i = 0; i < held.exchanges.size(); ++i) {
        EXPECT_FALSE(overlaps_another(held.exchanges, i)) << "exchange " << i;
    }
}

/* A one-shot expected segmented (a scanned F190) that comes due while
 * the other channel's request is out waits as chosen and goes out as
 * asked, once */
TEST(J2534HoldTest, AChosenOneShotGoesOutAsAsked)
{
    const auto make = [](Bus& b) {
        ASSERT_TRUE(ecuio_engine_init(&b.e, 2));
        b.e.gap_us = 5 * kMs;
        b.first_us = 6 * kMs;
        const uint8_t did[3] = {0x22, 0x1E, 0x80};
        ASSERT_EQ(ecuio_engine_add(&b.e, 1, did, 3),
                  ecuio_engine_add_result::added);
        ASSERT_TRUE(ecuio_engine_scan_add(&b.e, 0, 0xF190));
        b.reply_of = [](uint32_t, const ecuio_request& q) {
            Bytes out = {0x62, q.bytes[1], q.bytes[2]};
            out.insert(out.end(), q.bytes[1] == 0xF1 ? 17u : 2u, uint8_t{0x41});
            return out;
        };
    };
    Bus held;
    Bus plain;
    ASSERT_NO_FATAL_FAILURE(make(held));
    ASSERT_NO_FATAL_FAILURE(make(plain));
    plain.plain = true;
    /* channel 1 out first, so the scan's ask comes due beside it */
    held.e.chans[0].next_send_us = 2 * kMs;
    plain.e.chans[0].next_send_us = 2 * kMs;
    held.until(300 * kMs);
    plain.until(300 * kMs);
    expect_dues_order(held, plain, 2);
    const Exchange* first = nullptr;
    for (const Exchange& x : held.exchanges) {
        if (x.c == 0) {
            first = &x;
            break;
        }
    }
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->bytes, (Bytes{0x22, 0xF1, 0x90}));
    EXPECT_TRUE(first->released) << "the scan's ask waited as chosen";
}

/* A module whose long row falls silent holds the others for that one wait
 * and no more: out of step, its next requests hold nothing, so channel 1
 * keeps its pace */
TEST(J2534HoldTest, ASilentModuleHoldsTheOthersOnce)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(two_modules(b));
    b.until(200 * kMs); /* channel 0's reply learned */
    b.reply_of = [](uint32_t c, const ecuio_request& q) {
        if (c == 0) {
            return Bytes{}; /* channel 0 falls silent */
        }
        return Bytes{0x62, q.bytes[1], q.bytes[2], 0x12, 0x34};
    };
    const int64_t from = b.now;
    b.until(from + 1000 * kMs);
    size_t ones = 0;
    for (const Exchange& x : b.exchanges) {
        ones += x.c == 1 && x.sent_us >= from ? 1u : 0u;
    }
    /* a 3 ms exchange at the 5 ms floor: about 180 in the 900 ms left */
    EXPECT_GT(ones, 120u);
}

/* A chosen row the focus parked while it waited is dropped, not written;
 * a row that was parked when chosen (an unpicked session's check, asked
 * outside the rotation) still goes */
TEST(J2534HoldTest, AChosenRowGoesUnlessTheFocusParkedItMeanwhile)
{
    for (const bool parked_meanwhile : {true, false}) {
        ecuio_request_engine e;
        ASSERT_TRUE(ecuio_engine_init(&e, 2));
        const uint8_t a[3] = {0x22, 0x1E, 0x00};
        const uint8_t b[3] = {0x22, 0x1E, 0x80};
        ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3),
                  ecuio_engine_add_result::added);
        ASSERT_EQ(ecuio_engine_add(&e, 1, b, 3),
                  ecuio_engine_add_result::added);
        e.requests[0].reply_frames = 3;
        j2534_hold hold;
        uint32_t held = 0;
        ASSERT_EQ(j2534_hold_next(hold, &e, 1, 0, &held), 1);
        ecuio_engine_sent(&e, 1, 1, true, 0);
        if (parked_meanwhile) {
            EXPECT_EQ(j2534_hold_next(hold, &e, 0, 0, &held),
                      ECUIO_ENGINE_NONE);
            ASSERT_EQ(hold.chosen[0], 0) << "row 0 waits as chosen";
        } else {
            hold.chosen[0] = 0; /* a check row, chosen while parked */
            hold.rotating[0] = false;
        }
        e.requests[0].parked = 1;
        --e.chans[0].requests;
        const Bytes reply = {0x62, 0x1E, 0x80, 0x12, 0x34};
        (void)ecuio_engine_on_frame(&e, 1, reply.data(), 5, 4 * kMs, nullptr);
        const int32_t r = j2534_hold_next(hold, &e, 0, 4 * kMs, &held);
        EXPECT_EQ(r, parked_meanwhile ? ECUIO_ENGINE_NONE : 0)
            << (parked_meanwhile ? "parked meanwhile" : "parked when chosen");
        EXPECT_EQ(hold.chosen[0], ECUIO_ENGINE_NONE);
    }
}

/* A request in a pending wait (7F 22 78: 5.05 s) holds no one. Under
 * ?session=03 the other channel goes on asking and keeps its session;
 * waiting out the pending reply would stop its data for 5 s and lapse its
 * session past S3 */
TEST(J2534HoldTest, APendingReplyHoldsNoOneAndTheOthersKeepTheirSession)
{
    Bus b;
    ASSERT_TRUE(ecuio_engine_init(&b.e, 2));
    b.e.gap_us = 5 * kMs;
    ASSERT_TRUE(ecuio_engine_session(&b.e, ECUIO_SESSION_EXTENDED,
                                     ECUIO_ENGINE_KEEPALIVE_US));
    const uint8_t did[3] = {0x22, 0x1E, 0x80};
    ASSERT_EQ(ecuio_engine_add(&b.e, 1, did, 3),
              ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&b.e, 0, 0xF190)); /* segmented */
    b.reply_of = [](uint32_t, const ecuio_request& q) {
        switch (q.bytes[0]) {
        case 0x10:
            return Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
        case 0x3E:
            return Bytes{0x7E, 0x00};
        default:
            break;
        }
        if (q.bytes[1] == 0xF1) {
            return Bytes{0x7F, 0x22, 0x78}; /* its answer never comes */
        }
        return Bytes{0x62, q.bytes[1], q.bytes[2], 0x12, 0x34};
    };
    b.until(7000 * kMs);
    const Exchange* pending = nullptr;
    for (const Exchange& x : b.exchanges) {
        pending =
            pending == nullptr && x.c == 0 && x.bytes[1] == 0xF1 ? &x : pending;
    }
    ASSERT_NE(pending, nullptr);
    EXPECT_GE(pending->ended_us - pending->sent_us, 5000 * kMs);
    int64_t last = pending->sent_us;
    for (const Exchange& x : b.exchanges) {
        if (x.c == 1 && x.sent_us > pending->sent_us &&
            x.sent_us < pending->ended_us) {
            EXPECT_LT(x.sent_us - last, 50 * kMs) << "channel 1 held";
            last = x.sent_us;
        }
    }
    EXPECT_GT(last, pending->ended_us - 50 * kMs);
    EXPECT_EQ(b.e.chans[1].session_lost, 0u);
}

/* A held channel's due session request goes through: a hold never ages a
 * session past S3. The keep-alive waits behind a talk offered first unless
 * it is taken already (its write failed); an entry (10 03, or S3 passed)
 * doesn't. Not due, or with another
 * one-shot taken (due hands that out first), the channel is held. */
TEST(J2534HoldTest, AHeldChannelsDueSessionRequestGoesThrough)
{
    struct Variant {
        const char* name;
        bool keepalive_due;
        /* 0 none; 1-5 identifying to the talk out; 6 talk first; 7 talk
         * first with 3E 00 taken (its write failed) */
        int taken;
        int session; /* 0 entered; 1 entering; 2 S3 passed */
        uint8_t sid; /* what goes out, 0 held */
    };
    const Variant variants[] = {
        {"due", true, 0, 0, 0x3E},
        {"not due", false, 0, 0, 0},
        {"identifying", true, 1, 0, 0},
        {"diagnosing", true, 2, 0, 0},
        {"scanning", true, 3, 0, 0},
        {"clearing", true, 4, 0, 0},
        {"talk out", true, 5, 0, 0},
        {"talk first", true, 6, 0, 0},
        {"talk first, entering", false, 6, 1, 0x10},
        {"talk first, S3 passed", true, 6, 2, 0x10},
        {"talk first, 3E 00 taken", true, 7, 0, 0x3E},
    };
    for (const Variant& v : variants) {
        ecuio_request_engine e;
        ASSERT_TRUE(ecuio_engine_init(&e, 2));
        ASSERT_TRUE(ecuio_engine_session(&e, ECUIO_SESSION_EXTENDED,
                                         ECUIO_ENGINE_KEEPALIVE_US));
        const uint8_t a[3] = {0x22, 0x1E, 0x00};
        const uint8_t b[3] = {0x22, 0x1E, 0x80};
        ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3),
                  ecuio_engine_add_result::added);
        ASSERT_EQ(ecuio_engine_add(&e, 1, b, 3),
                  ecuio_engine_add_result::added);
        const int64_t now = 3000 * kMs;
        for (uint32_t c = 0; c < 2; ++c) {
            e.chans[c].session_step = ECUIO_SESSION_STATE_ENTERED;
        }
        ecuio_engine_channel& ch = e.chans[1];
        ch.last_sent_us = v.session == 2    ? now - ECUIO_ENGINE_S3_US
                          : v.keepalive_due ? now - ECUIO_ENGINE_KEEPALIVE_US
                                            : now - kMs;
        if (v.session == 1) {
            ch.session_step = ECUIO_SESSION_STATE_ENTERING;
        }
        e.requests[0].reply_frames = 3;
        ecuio_engine_sent(&e, 0, 0, true, now - kMs); /* segmented, out */
        ch.identifying = v.taken == 1;
        ch.diagnosing = v.taken == 2;
        ch.scanning = v.taken == 3;
        ch.clearing = v.taken == 4;
        ch.talk.out = v.taken == 5;
        if (v.taken == 7) {
            ch.sessioning = true;
            ch.once.bytes[0] = 0x3E;
            ch.once.len = 2;
        }
        if (v.taken >= 6) {
            const uint8_t pdu[3] = {0x22, 0xF1, 0x90};
            ASSERT_EQ(ecuio_engine_talk(ch.talk, e.pdu_max, pdu, 3, 3, true),
                      ecuio_talk_offer::taken);
        }
        j2534_hold hold;
        uint32_t held = 0;
        const int32_t r = j2534_hold_next(hold, &e, 1, now, &held);
        if (v.sid != 0) {
            ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << v.name;
            EXPECT_EQ(ch.once.bytes[0], v.sid) << v.name;
            EXPECT_EQ(held, 0u) << v.name;
        } else {
            EXPECT_EQ(r, ECUIO_ENGINE_NONE) << v.name;
            EXPECT_EQ(held, 2u) << v.name;
        }
    }
}

/* Two channels, channel 0's segmented row out at 0 and channel 1's row
 * due beside it */
void segmented_out(ecuio_request_engine& e)
{
    ASSERT_TRUE(ecuio_engine_init(&e, 2));
    e.gap_us = 5 * kMs;
    const uint8_t a[3] = {0x22, 0x1E, 0x00};
    const uint8_t b[3] = {0x22, 0x1E, 0x80};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 1, b, 3), ecuio_engine_add_result::added);
    e.requests[0].reply_frames = 3;
    ecuio_engine_sent(&e, 0, 0, true, 0);
}

/* A channel goes silent when its wait runs out in a pass and holds no one
 * then; a frame from it, or the link's recovery (the rescan's
 * identification asked again on every channel), makes it hold again */
TEST(J2534HoldTest, AChannelIsSilentUntilAFrameOrARecovery)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(segmented_out(e));
    j2534_hold hold;
    uint32_t held = 0;
    EXPECT_EQ(j2534_hold_next(hold, &e, 1, 10 * kMs, &held), ECUIO_ENGINE_NONE);
    EXPECT_EQ(held, 2u) << "in step, channel 0 holds";
    const auto nothing = [](uint32_t, int32_t) {};
    j2534_hold_pass(hold, &e, ECUIO_ENGINE_WAIT_US, [](uint32_t) {}, nothing);
    EXPECT_EQ(hold.silent, 1u) << "channel 0's wait ran out";
    for (int how = 0; how < 2; ++how) {
        hold.silent = 1;
        ecuio_engine_sent(&e, 0, 0, true, 200 * kMs);
        e.chans[1].outstanding = ECUIO_ENGINE_NONE;
        e.chans[1].next_send_us = 0;
        held = 0;
        EXPECT_EQ(j2534_hold_next(hold, &e, 1, 210 * kMs, &held), 1)
            << "silent, channel 0 holds no one";
        e.chans[1].outstanding = ECUIO_ENGINE_NONE;
        if (how == 0) {
            j2534_hold_answered(hold, 0);
        } else {
            j2534_hold_recovered(hold);
        }
        held = 0;
        EXPECT_EQ(j2534_hold_next(hold, &e, 1, 210 * kMs, &held),
                  ECUIO_ENGINE_NONE)
            << (how == 0 ? "after a frame" : "after a recovery");
        EXPECT_EQ(held, 2u);
    }
}

/* A silent channel's segmented row goes out at once, not chosen: kept
 * waiting it would hold the others until the bus drains */
TEST(J2534HoldTest, ASilentChannelsSegmentedRowIsNotChosen)
{
    for (const bool silent : {true, false}) {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(segmented_out(e));
        e.chans[0].outstanding = ECUIO_ENGINE_NONE;
        ecuio_engine_sent(&e, 1, 1, true, 0); /* a Single Frame out */
        j2534_hold hold;
        hold.silent = silent ? 1u : 0u;
        uint32_t held = 0;
        const int32_t r = j2534_hold_next(hold, &e, 0, 10 * kMs, &held);
        EXPECT_EQ(r, silent ? 0 : ECUIO_ENGINE_NONE);
        EXPECT_EQ(hold.chosen[0], silent ? ECUIO_ENGINE_NONE : 0);
    }
}

/* Off, the pass is the plain one to the link's counters: channel 0's
 * write fails before channel 1's wait is ended, so the latch keeps that
 * timeout from counting. On, every wait is ended first, so it counts. */
TEST(J2534HoldTest, OffThePassIsThePlainOneToTheLinksCounts)
{
    struct Run {
        std::atomic<int32_t> published{ECUIO_OK};
        std::atomic<uint32_t> episodes{0};
        link_health h{published, episodes};
        ecuio_request_engine e;
    };
    const auto make = [](Run& run) {
        ASSERT_NO_FATAL_FAILURE(segmented_out(run.e));
        run.e.requests[0].reply_frames = 0;
        run.e.chans[0].outstanding = ECUIO_ENGINE_NONE;
        ecuio_engine_sent(&run.e, 1, 1, true, 0); /* its wait ends at 100 */
    };
    const int64_t now = ECUIO_ENGINE_WAIT_US;
    const auto fail = [](Run& run, uint32_t c, int32_t r) {
        link_on_write(run.h, false);
        ecuio_engine_sent(&run.e, c, r, false, ECUIO_ENGINE_WAIT_US);
    };
    Run plain;
    ASSERT_NO_FATAL_FAILURE(make(plain));
    for (uint32_t c = 0; c < 2; ++c) {
        if (ecuio_engine_expire(&plain.e, c, now)) {
            link_on_request_timeout(plain.h);
        }
        const int32_t r = ecuio_engine_due(&plain.e, c, now);
        if (r != ECUIO_ENGINE_NONE) {
            fail(plain, c, r);
        }
    }
    for (const bool on : {false, true}) {
        Run run;
        ASSERT_NO_FATAL_FAILURE(make(run));
        j2534_hold hold;
        hold.on = on;
        j2534_hold_pass(
            hold, &run.e, now,
            [&](uint32_t) { link_on_request_timeout(run.h); },
            [&](uint32_t c, int32_t r) { fail(run, c, r); });
        const bool same = run.h.unanswered == plain.h.unanswered &&
                          run.h.quiet_runs == plain.h.quiet_runs &&
                          run.h.write_failed == plain.h.write_failed &&
                          run.published.load() == plain.published.load() &&
                          run.episodes.load() == plain.episodes.load();
        EXPECT_EQ(same, !on) << (on ? "on" : "off");
    }
    EXPECT_FALSE(plain.h.unanswered);
    EXPECT_TRUE(plain.h.write_failed);
}

/* A held channel, or one whose chosen request waits, waits on another's
 * reply: the worker's next event is that reply's wait, not the held
 * channel's floor (a read of 1 ms over and over) */
TEST(J2534HoldTest, HeldAndChosenChannelsWaitOnTheOthersReply)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(segmented_out(e));
    j2534_hold hold;
    const int64_t now = 10 * kMs;
    uint32_t held = 0;
    ASSERT_EQ(j2534_hold_next(hold, &e, 1, now, &held), ECUIO_ENGINE_NONE);
    j2534_hold_pass_end(hold, held);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 0);
    EXPECT_EQ(j2534_hold_next_event_us(hold, &e, now),
              ECUIO_ENGINE_WAIT_US - now)
        << "held";
    hold.held = 0;
    hold.chosen[1] = 1;
    EXPECT_EQ(j2534_hold_next_event_us(hold, &e, now),
              ECUIO_ENGINE_WAIT_US - now)
        << "chosen";
    hold.chosen[1] = ECUIO_ENGINE_NONE;
    hold.held = 3; /* nothing else left: the whole figure */
    EXPECT_EQ(j2534_hold_next_event_us(hold, &e, now), 0);
    hold.held = 2;
    hold.on = false;
    EXPECT_EQ(j2534_hold_next_event_us(hold, &e, now), 0) << "off";
}
