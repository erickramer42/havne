// File/layer: tests/test_j2534_hold_order.cpp | test_obd2, Qt-free
// Testing: UNIT: J2534HoldTest's order (core/src/j2534_hold.h: the
//          Single Frame channels first, the segmented ones by their next
//          row's age), against the hold's bus of modules

#include "j2534_hold_support.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace j2534_hold_test;

namespace {

/* A module's rows: n of them, segmented or Single Frames, as DIDs of its
 * own (22 1E <c><k>) or Service 01 PIDs (01 <pid>, from pids) */
struct Module {
    uint32_t rows;
    bool segmented;
    std::vector<uint8_t> pids;
};

/* The hold's bus with each module on a channel of its own, in the order
 * given, every reply answered as its module's rows are */
void modules(Bus& b, const std::vector<Module>& ms)
{
    const auto chans = static_cast<uint32_t>(ms.size());
    ASSERT_TRUE(ecuio_engine_init(&b.e, chans));
    b.e.gap_us = 5 * kMs;
    for (uint32_t c = 0; c < chans; ++c) {
        for (const uint8_t pid : ms[c].pids) {
            const uint8_t q[2] = {0x01, pid};
            ASSERT_EQ(ecuio_engine_add(&b.e, c, q, 2),
                      ecuio_engine_add_result::added);
        }
        for (uint32_t k = 0; k < ms[c].rows; ++k) {
            const uint8_t did[3] = {0x22, 0x1E,
                                    static_cast<uint8_t>(16 * c + k)};
            ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                      ecuio_engine_add_result::added);
        }
    }
    b.reply_of = [ms](uint32_t c, const ecuio_request& q) {
        Bytes out(q.bytes, q.bytes + q.len);
        out[0] = static_cast<uint8_t>(out[0] + 0x40);
        out.insert(out.end(), ms[c].segmented ? 17u : 2u, uint8_t{0x2A});
        return out;
    };
}

/* Each table row's writes from from_us on, by its channel and bytes, a
 * row never written among them */
std::map<std::pair<uint32_t, Bytes>, size_t> per_row(const Bus& b,
                                                     int64_t from_us)
{
    std::map<std::pair<uint32_t, Bytes>, size_t> n;
    for (uint32_t i = 0; i < b.e.count; ++i) {
        const ecuio_request& q = b.e.requests[i];
        n[{q.channel, Bytes(q.bytes, q.bytes + q.len)}] = 0;
    }
    for (const Exchange& x : b.exchanges) {
        if (x.sent_us >= from_us) {
            ++n[{x.c, x.bytes}];
        }
    }
    return n;
}

/* Whether the counts of the rows keep() picks are within a quarter of
 * each other, the fewest at least min */
void expect_even(const std::map<std::pair<uint32_t, Bytes>, size_t>& n,
                 const std::function<bool(uint32_t, const Bytes&)>& keep,
                 size_t min, const std::string& what)
{
    size_t lo = SIZE_MAX;
    size_t hi = 0;
    for (const auto& row : n) {
        if (keep(row.first.first, row.first.second)) {
            lo = std::min(lo, row.second);
            hi = std::max(hi, row.second);
        }
    }
    EXPECT_GE(lo, min) << what;
    EXPECT_LE(4 * hi, 5 * lo) << what << ": " << lo << " to " << hi;
}

} // namespace

/* A segmented module of three rows beside one of one row and a Single
 * Frame module, in every placement: the segmented channels go by their
 * next row's age, so each segmented row is asked about as often as every
 * other (the three-row module takes three turns in four), where equal
 * turns ask each of its rows a third as often as the one-row module's */
TEST(J2534HoldTest, UnequalModulesShareTheBusBySignal)
{
    const Module three = {3, true, {}};
    const Module one = {1, true, {}};
    const Module single = {1, false, {}};
    std::vector<uint32_t> place = {0, 1, 2};
    do {
        std::vector<Module> ms(3);
        ms[place[0]] = three;
        ms[place[1]] = one;
        ms[place[2]] = single;
        Bus b;
        ASSERT_NO_FATAL_FAILURE(modules(b, ms));
        b.until(3000 * kMs);
        const auto n = per_row(b, 200 * kMs);
        const std::string what = "three on " + std::to_string(place[0]) +
                                 ", one on " + std::to_string(place[1]);
        expect_even(
            n, [&](uint32_t c, const Bytes&) { return ms[c].segmented; }, 30,
            what);
        for (size_t i = 0; i < b.exchanges.size(); ++i) {
            const Exchange& x = b.exchanges[i];
            for (size_t k = 0; x.segmented && k < b.exchanges.size(); ++k) {
                const Exchange& y = b.exchanges[k];
                ASSERT_FALSE(k != i && y.c != x.c && x.sent_us > 200 * kMs &&
                             y.sent_us < x.ended_us && x.sent_us < y.ended_us)
                    << what << ", exchange " << i;
            }
        }
    } while (std::next_permutation(place.begin(), place.end()));
}

/* A segmented module of four rows beside one of one row: the four-row
 * module's next row is the older most of the time, so it writes twice
 * running, which no turn-taking order does; its floor (5 ms from the
 * write) is past by its own exchange's end */
TEST(J2534HoldTest, ASegmentedChannelGoesAgainWhileItsRowIsTheOldest)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(modules(b, {{4, true, {}}, {1, true, {}}}));
    b.until(1000 * kMs);
    size_t twice = 0;
    for (size_t i = 1; i < b.exchanges.size(); ++i) {
        twice += b.exchanges[i].c == 0 && b.exchanges[i - 1].c == 0 ? 1 : 0;
    }
    const auto n = per_row(b, 200 * kMs);
    expect_even(n, [](uint32_t, const Bytes&) { return true; }, 10, "rows");
    EXPECT_GT(twice, 10u);
}

/* A segmented module of Service 01 PIDs, two fast and six slow, beside
 * one of four DIDs (one class): a channel is ranked by its next fast
 * row, so the slow rows ride on their channel's own turns (one in three)
 * and every fast row is asked about as often as every DID. Ranked by the
 * slow row at a slow turn, the module would go early at each, its fast
 * rows asked more often than the DIDs */
TEST(J2534HoldTest, ASlowRowDoesNotRankItsChannel)
{
    const std::vector<uint8_t> pids = {0x0C, 0x0D, 0x04, 0x05,
                                       0x0F, 0x10, 0x11, 0x2F};
    for (const bool first : {true, false}) {
        Bus b;
        const Module obd = {0, true, pids};
        const Module dids = {4, true, {}};
        ASSERT_NO_FATAL_FAILURE(modules(b, first ? std::vector<Module>{obd, dids}
                                                 : std::vector<Module>{dids, obd}));
        ecuio_engine_classify(&b.e,
                              [](uint8_t pid) { return pid == 0x0C || pid == 0x0D; });
        b.until(4000 * kMs);
        const uint32_t obd_c = first ? 0 : 1;
        const auto n = per_row(b, 200 * kMs);
        expect_even(
            n,
            [&](uint32_t c, const Bytes& q) {
                return c != obd_c || q[1] == 0x0C || q[1] == 0x0D;
            },
            40, first ? "obd2 first" : "obd2 second");
        /* six slow rows share one write in three: about 7 each */
        for (const auto& row : n) {
            if (row.first.first == obd_c && row.first.second[1] == 0x2F) {
                EXPECT_GE(row.second, 5u) << "a slow row is still asked";
            }
        }
    }
}

/* A Single Frame module of six rows, a segmented one of four and a Single
 * Frame one of one row, replies 6 ms after the write (past the 5 ms floor,
 * so a Single Frame channel is due again as its reply lands). The six-row
 * module takes an extra turn ahead of the chosen segmented row while its
 * next row is the older, so its rows are asked about as often as the
 * segmented module's; at most one extra turn for each segmented exchange,
 * and none for the one-row module, whose row is never the older */
TEST(J2534HoldTest, ABigSingleFrameModuleTakesAnExtraTurn)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(
        modules(b, {{6, false, {}}, {4, true, {}}, {1, false, {}}}));
    b.first_us = 6 * kMs;
    b.until(3000 * kMs);
    const auto n = per_row(b, 200 * kMs);
    expect_even(n, [](uint32_t c, const Bytes&) { return c < 2; }, 20, "rows");
    size_t ones[3] = {};
    size_t twice = 0;
    for (const Exchange& x : b.exchanges) {
        if (x.sent_us < 200 * kMs) {
            continue;
        }
        if (x.c == 1) {
            ones[0] = ones[2] = 0;
            continue;
        }
        ++ones[x.c];
        EXPECT_LE(ones[0], 2u) << "the six-row module at " << x.sent_us;
        EXPECT_LE(ones[2], 1u) << "the one-row module at " << x.sent_us;
        twice += ones[0] == 2 ? 1 : 0;
    }
    EXPECT_GT(twice, 20u) << "the six-row module took no extra turn";
}

namespace {

/* What expect_none_ahead counted: channel 0's segmented writes of a known
 * length, and the chosen rows given back while another channel's waited */
struct Ahead {
    size_t known_segmented = 0;
    size_t given_back_beside = 0;
};

/* Runs b to 3 s and checks that nothing goes ahead of an older chosen row:
 * a request whose reply came back segmented (once its row's length is
 * known), or any row's first ask (its length unknown), written while
 * another channel's row was chosen, is itself a chosen row given back,
 * at least as old as each one still waiting. With firsts false a first ask
 * is left out: a Service 01 PID's counts one frame until it is answered */
Ahead expect_none_ahead(Bus& b, const std::string& what, bool firsts = true)
{
    const uint32_t chans = b.e.chan_count;
    std::vector<std::vector<Bytes>> seen(chans);
    Ahead n;
    while (b.now < 3000 * kMs) {
        std::vector<int32_t> chosen(b.hold.chosen, b.hold.chosen + chans);
        std::vector<int64_t> age(chans, 0);
        for (uint32_t k = 0; k < chans; ++k) {
            age[k] = chosen[k] != ECUIO_ENGINE_NONE
                         ? j2534_hold_age_us(b.hold, &b.e, k, b.now)
                         : 0;
        }
        const size_t from = b.exchanges.size();
        b.pass();
        for (size_t i = from; i < b.exchanges.size(); ++i) {
            const Exchange& x = b.exchanges[i];
            std::vector<Bytes>& s = seen[x.c];
            const bool known = std::find(s.begin(), s.end(), x.bytes) != s.end();
            s.push_back(x.bytes);
            const bool judged = known ? x.segmented : firsts;
            for (uint32_t k = 0; judged && k < chans; ++k) {
                if (k == x.c || chosen[k] == ECUIO_ENGINE_NONE) {
                    continue;
                }
                const bool older = x.released && age[x.c] >= age[k];
                EXPECT_TRUE(older)
                    << what << ": channel " << x.c << " at " << x.sent_us
                    << (known ? "" : ", a first ask,") << " ahead of channel "
                    << k << "'s older chosen row";
                n.given_back_beside += older ? 1 : 0;
            }
            n.known_segmented += x.c == 0 && x.segmented && known ? 1 : 0;
        }
    }
    return n;
}

/* Module 0 of Service 01 PIDs at J1979's lengths (those from 0x68 on
 * slow; by default six fast in one frame and three slow in two), beside a
 * segmented module of segmented_rows rows and a Single Frame one */
void slow_pids(Bus& b,
               const std::vector<uint8_t>& pids = {0x04, 0x05, 0x0C, 0x0D,
                                                   0x0F, 0x10, 0x68, 0x69,
                                                   0x6E},
               uint32_t segmented_rows = 1)
{
    ASSERT_NO_FATAL_FAILURE(modules(
        b, {{0, false, pids}, {segmented_rows, true, {}}, {1, false, {}}}));
    ecuio_engine_classify(&b.e, [](uint8_t pid) { return pid < 0x68; });
    const auto plain = b.reply_of;
    b.reply_of = [plain](uint32_t c, const ecuio_request& q) {
        if (c != 0) {
            return plain(c, q);
        }
        Bytes out = {0x41, q.bytes[1]};
        out.insert(out.end(), ECUIO_OBD2_PID_BYTES[q.bytes[1]], uint8_t{0x2A});
        return out;
    };
}

} // namespace

/* An invariant pin (it holds under equal turns too), three modules: no
 * segmented request (once its length is known), nor a row's first ask
 * (its length unknown, which takes no extra turn), goes ahead of an older
 * chosen row. Module 0 is six rows answering in turn one frame and three,
 * beside a segmented module of two rows and a Single Frame one, replies
 * at 6 ms; then slow_pids at 6 ms: an extra turn a fast row earns may meet
 * a slow row, which is then kept chosen, not written */
TEST(J2534HoldTest, NoLongOrUnaskedRequestGoesAheadOfAChosenOne)
{
    {
        Bus b;
        ASSERT_NO_FATAL_FAILURE(
            modules(b, {{6, false, {}}, {2, true, {}}, {1, false, {}}}));
        b.first_us = 6 * kMs;
        const auto plain = b.reply_of;
        b.reply_of = [plain](uint32_t c, const ecuio_request& q) {
            Bytes out = plain(c, q);
            if (c == 0 && q.bytes[2] % 2 == 0) {
                out.insert(out.end(), 15u, uint8_t{0x2A});
            }
            return out;
        };
        EXPECT_GT(expect_none_ahead(b, "mixed rows").known_segmented, 30u)
            << "the mixed module's long rows were asked";
    }
    {
        Bus b;
        ASSERT_NO_FATAL_FAILURE(slow_pids(b));
        b.first_us = 6 * kMs;
        EXPECT_GT(expect_none_ahead(b, "slow rows", false).known_segmented, 50u)
            << "the slow rows were asked";
    }
}

/* Module 0 of six fast Service 01 PIDs in one frame and one slow in two,
 * beside a segmented module of three rows and a Single Frame one answering
 * at 10 ms (the others at 6), so its reply is often still out when module
 * 0 takes an extra turn. That turn meets the slow row (asked at every slow
 * turn, so younger than the segmented module's rows), kept chosen beside
 * the segmented module's chosen row: j2534_hold_order ranks both by age
 * when the bus clears, so the older goes first and the younger waits */
TEST(J2534HoldTest, AChosenRowFromAnExtraTurnGoesOnlyAsTheOlder)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(
        slow_pids(b, {0x04, 0x05, 0x0C, 0x0D, 0x0F, 0x10, 0x68}, 3));
    b.first_us = 6 * kMs;
    b.first_of_us[2] = 10 * kMs;
    const Ahead n = expect_none_ahead(b, "a late Single Frame", false);
    EXPECT_GT(n.known_segmented, 30u) << "the slow row was asked";
    EXPECT_GT(n.given_back_beside, 0u) << "two rows were chosen at once";
}

/* A module of six rows answering in turn one frame and three, a Single
 * Frame one of six rows and a segmented one of two, replies at 6 ms. An
 * extra turn goes only to a row answered in one frame, so a module whose
 * next row is segmented isn't let past the chosen row only to have that
 * row kept chosen beside it: at most one row is chosen after every pass */
TEST(J2534HoldTest, OneRowAtMostIsChosen)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(
        modules(b, {{6, false, {}}, {6, false, {}}, {2, true, {}}}));
    b.first_us = 6 * kMs;
    const auto plain = b.reply_of;
    b.reply_of = [plain](uint32_t c, const ecuio_request& q) {
        Bytes out = plain(c, q);
        if (c == 0 && q.bytes[2] % 2 == 0) {
            out.insert(out.end(), 15u, uint8_t{0x2A});
        }
        return out;
    };
    size_t passes = 0;
    while (b.now < 3000 * kMs) {
        b.pass();
        uint32_t chosen = 0;
        for (uint32_t k = 0; k < 3; ++k) {
            chosen += b.hold.chosen[k] != ECUIO_ENGINE_NONE ? 1 : 0;
        }
        ASSERT_LE(chosen, 1u) << "at " << b.now;
        passes += chosen;
    }
    EXPECT_GT(passes, 100u) << "rows waited as chosen";
}

/* A segmented module of four rows with a scan of 200 DIDs to ask (each
 * refused in one frame), beside a segmented module of eight rows and a
 * Single Frame one. The scan takes turns with its module's rotation, so
 * that module is ranked by the older of its last write and its next row:
 * its rows are asked about as often as the other's, and the scan goes on */
TEST(J2534HoldTest, AModuleWithAScanKeepsItsRowsPace)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(
        modules(b, {{4, true, {}}, {8, true, {}}, {1, false, {}}}));
    for (uint16_t k = 0; k < 200; ++k) {
        ASSERT_TRUE(ecuio_engine_scan_add(&b.e, 0, static_cast<uint16_t>(0xD000 + k)));
    }
    const auto plain = b.reply_of;
    b.reply_of = [plain](uint32_t c, const ecuio_request& q) {
        if (q.len == 3 && q.bytes[1] == 0xD0) {
            return Bytes{0x7F, 0x22, 0x31};
        }
        return plain(c, q);
    };
    b.until(3000 * kMs);
    const auto n = per_row(b, 200 * kMs);
    expect_even(
        n, [](uint32_t c, const Bytes& q) { return c < 2 && q[1] == 0x1E; }, 10,
        "rows");
    size_t scanned = 0;
    for (const Exchange& x : b.exchanges) {
        scanned += x.bytes.size() == 3 && x.bytes[1] == 0xD0 ? 1 : 0;
    }
    EXPECT_GT(scanned, 30u) << "the scan went on";
}

/* Two segmented modules of one row each and a Single Frame one; then four
 * rows of a known segmented length join each segmented module, never
 * asked, as a scan's rows do. They rank oldest and tie, so the two
 * modules take turns by the rotation past the last segmented write: no
 * module asks three of them in a row */
TEST(J2534HoldTest, NewRowsTieAndTakeTurns)
{
    Bus b;
    ASSERT_NO_FATAL_FAILURE(
        modules(b, {{1, true, {}}, {1, true, {}}, {1, false, {}}}));
    b.until(300 * kMs);
    for (uint32_t c = 0; c < 2; ++c) {
        for (uint8_t k = 1; k <= 4; ++k) {
            const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(16 * c + k)};
            ASSERT_EQ(ecuio_engine_add(&b.e, c, did, 3),
                      ecuio_engine_add_result::added);
            b.e.requests[b.e.count - 1].reply_frames = 3;
        }
    }
    const size_t from = b.exchanges.size();
    b.until(600 * kMs);
    std::vector<uint32_t> fresh;
    for (size_t i = from; i < b.exchanges.size(); ++i) {
        const Exchange& x = b.exchanges[i];
        if (x.c < 2 && x.bytes[2] % 16 != 0) {
            fresh.push_back(x.c);
        }
    }
    ASSERT_GE(fresh.size(), 8u);
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_FALSE(fresh[i] == fresh[i - 1] && fresh[i] == fresh[i - 2])
            << "new row " << i << " on channel " << fresh[i];
    }
}
