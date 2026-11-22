// File/layer: tests/test_request_table.cpp | test_obd2, Qt-free
// Testing: UNIT: RequestTableTest (the request engine's table sized from
//          the set, the count for each module, a table that asks only its
//          picks and reply timing's pool; ADR-0065 section 3)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"

#include <ecuio/signal_sets.h>

#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr int64_t kMs = 1000;
constexpr uint32_t kRxA = 0x7E8;
constexpr uint32_t kRxB = 0x7E9;
constexpr uint32_t kRx[2] = {kRxA, kRxB};
constexpr uint32_t kMost = ECUIO_MODULE_MAX_REQUESTS;
constexpr uint32_t kPool = ECUIO_PROFILE_MAX_REQUESTS;

/* A def read from the reply to 22 <did> of the ECU at rx */
ecuio_signal_def_t did_def(uint32_t rx, uint16_t did)
{
    ecuio_signal_def_t d = {};
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    return d;
}

/* n defs, each a DID of its own from first on, asked of the ECU at rx */
void add_dids(std::vector<ecuio_signal_def_t>& defs, uint32_t rx, uint32_t n,
              uint16_t first = 0x1000)
{
    for (uint32_t i = 0; i < n; ++i) {
        defs.push_back(did_def(rx, static_cast<uint16_t>(first + i)));
    }
}

ecuio_signal_set_t set_of(const std::vector<ecuio_signal_def_t>& defs)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(defs.size());
    s.defs = defs.data();
    return s;
}

/* A set of two modules, each asked its most: twice what a rotation holds */
std::vector<ecuio_signal_def_t> two_full_modules()
{
    std::vector<ecuio_signal_def_t> defs;
    add_dids(defs, kRxA, kMost);
    add_dids(defs, kRxB, kMost);
    return defs;
}

/* The table of a set past a rotation, as attach builds it */
void pick_only_table(ecuio_request_engine* e,
                     const std::vector<ecuio_signal_def_t>& defs)
{
    const ecuio_signal_set_t set = set_of(defs);
    std::vector<uint32_t> rows(defs.size());
    ecuio_derived derived;
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, rows.data(), &derived),
              ecuio_plan_result::ok);
    ASSERT_GT(derived.total, kPool);
    ASSERT_TRUE(ecuio_engine_init(e, 2, derived.total));
    ecuio_engine_pick_only(e);
    ASSERT_TRUE(ecuio_engine_derive_planned(e, &set, kRx, 2, rows.data()));
    ASSERT_EQ(e->count, derived.total);
}

/* A focus keeping the given table rows */
void keep_rows(ecuio_request_engine* e, const std::vector<uint32_t>& rows)
{
    std::memset(e->marks.data(), 0, e->marks.size());
    for (uint32_t r : rows) {
        e->marks[r] = 1;
    }
    ecuio_engine_focus(e, e->marks.data());
}

/* Channel c's next request at now, sent and answered a millisecond
 * later; its table row, or what due gave when that is no row. now moves
 * past the floor between requests. */
int32_t ask(ecuio_request_engine* e, uint32_t c, int64_t& now,
            bool answered = true)
{
    const int32_t r = ecuio_engine_due(e, c, now);
    if (r < 0) {
        return r;
    }
    ecuio_engine_sent(e, c, r, true, now);
    if (answered) {
        const ecuio_request& q = e->requests[r];
        const uint8_t reply[5] = {0x62, q.bytes[1], q.bytes[2], 0x12, 0x34};
        EXPECT_EQ(ecuio_engine_on_frame(e, c, reply, sizeof reply, now + kMs,
                                        nullptr),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
    } else {
        now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(e, c, now));
    }
    return r;
}

uint32_t in_rotation(const ecuio_request_engine& e)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < e.count; ++i) {
        n += ecuio_engine_in_rotation(e.requests[i]) ? 1u : 0u;
    }
    return n;
}

} // namespace

/* Init gives the table its room, and what is sized with it; a row past
 * the room is refused as full */
TEST(RequestTableTest, InitSizesTheTableAndARowPastItIsRefused)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1, 3));
    EXPECT_EQ(e.requests.size(), 3u);
    EXPECT_EQ(e.timing_of.size(), 3u);
    EXPECT_EQ(e.timing_rows.size(), 3u);
    EXPECT_EQ(e.marks.size(), 3u);
    for (uint16_t did = 0x1000; did < 0x1003; ++did) {
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(did >> 8),
                                static_cast<uint8_t>(did)};
        EXPECT_EQ(ecuio_engine_add(&e, 0, req, 3),
                  ecuio_engine_add_result::added);
    }
    const uint8_t fourth[3] = {0x22, 0x10, 0x03};
    EXPECT_EQ(ecuio_engine_add(&e, 0, fourth, 3),
              ecuio_engine_add_result::full);
    const uint8_t again[3] = {0x22, 0x10, 0x00};
    EXPECT_EQ(ecuio_engine_add(&e, 0, again, 3),
              ecuio_engine_add_result::duplicate)
        << "a request already there is no new row";
    EXPECT_EQ(e.count, 3u);

    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    EXPECT_EQ(e.requests.size(), size_t{kPool})
        << "the room a caller that names none gets";
    EXPECT_EQ(e.count, 0u) << "init starts the table over";

    ASSERT_TRUE(ecuio_engine_init(&e, 1, 0));
    EXPECT_EQ(ecuio_engine_add(&e, 0, fourth, 3),
              ecuio_engine_add_result::full);

    /* The pool is never larger than a rotation, whatever the room */
    ASSERT_TRUE(ecuio_engine_init(&e, 1, kPool + 500));
    EXPECT_EQ(e.requests.size(), size_t{kPool} + 500);
    EXPECT_EQ(e.timing_rows.size(), size_t{kPool});
}

/* A table that asks its whole set gives each request its timing row as
 * it joins, parked or not, so the row is the table's and the focus's end
 * can ask every request; with the rows spent it takes no more */
TEST(RequestTableTest, ATableAskedWholeGivesEachRequestItsRowAsItJoins)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1, kPool + 500));
    for (uint32_t i = 0; i < kPool; ++i) {
        const uint16_t did = static_cast<uint16_t>(0x1000 + i);
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(did >> 8),
                                static_cast<uint8_t>(did)};
        ASSERT_EQ(ecuio_engine_add(&e, 0, req, 3),
                  ecuio_engine_add_result::added)
            << i;
        ASSERT_EQ(e.timing_of[i], i);
        ASSERT_EQ(e.timing_rows[i], i);
    }
    EXPECT_EQ(e.timing_taken, kPool);
    const uint8_t past[3] = {0x22, 0x90, 0x00};
    EXPECT_EQ(ecuio_engine_add(&e, 0, past, 3), ecuio_engine_add_result::full)
        << "room in the table, none in a rotation";
    EXPECT_EQ(e.count, kPool);
    EXPECT_EQ(e.untimed, 0u);

    /* A request that joins under a focus is parked and has its row */
    ASSERT_TRUE(ecuio_engine_init(&e, 1, 8));
    const uint8_t a[3] = {0x22, 0x10, 0x00};
    const uint8_t b[3] = {0x22, 0x10, 0x01};
    const uint8_t c[3] = {0x22, 0x10, 0x02};
    ASSERT_EQ(ecuio_engine_add(&e, 0, a, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, b, 3), ecuio_engine_add_result::added);
    keep_rows(&e, {1});
    ASSERT_EQ(ecuio_engine_add(&e, 0, c, 3), ecuio_engine_add_result::added);
    EXPECT_EQ(e.requests[2].parked, 1u);
    EXPECT_EQ(e.timing_of[2], 2u);
    EXPECT_TRUE(e.focus_joined);
    ecuio_engine_unfocus(&e);
    EXPECT_FALSE(e.focused);
    EXPECT_EQ(in_rotation(e), 3u);
    EXPECT_EQ(e.chans[0].requests, 3u);
}

/* The plan counts, with no table, exactly the rows derive enrols: one
 * for each distinct request and channel, in def order, each def given
 * the row of its request */
TEST(RequestTableTest, APlanCountsWhatDeriveEnrols)
{
    std::vector<ecuio_signal_def_t> defs;
    add_dids(defs, kRxA, 3);               /* rows 0, 1, 2 */
    defs.push_back(did_def(kRxA, 0x1000)); /* a second def of row 0 */
    ecuio_signal_def_t pid = {};
    pid.message_id = kRxA;
    pid.mux_length = 2;
    pid.mux_key = 0x410C;
    defs.push_back(pid); /* row 3: 01 0C */
    ecuio_signal_def_t passive = {};
    passive.message_id = 0x3E8;
    defs.push_back(passive);                /* asks nothing */
    add_dids(defs, kRxB, 2);                /* rows 4, 5: the same DIDs, */
                                            /* another module */
    defs.push_back(did_def(0x7EF, 0x1002)); /* no channel's: channel 0, */
                                            /* where it is row 2's */
    const ecuio_signal_set_t set = set_of(defs);

    std::vector<uint32_t> rows(defs.size(), 77);
    ecuio_derived derived;
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, rows.data(), &derived),
              ecuio_plan_result::ok);
    EXPECT_EQ(derived.total, 6u);
    EXPECT_EQ(derived.module[0], 4u);
    EXPECT_EQ(derived.module[1], 2u);
    EXPECT_EQ(derived.overflow_def, ECUIO_ROW_NONE);
    EXPECT_EQ(rows,
              (std::vector<uint32_t>{0, 1, 2, 0, 3, ECUIO_ROW_NONE, 4, 5, 2}));

    ecuio_request_engine derives;
    ASSERT_TRUE(ecuio_engine_init(&derives, 2, derived.total));
    ASSERT_TRUE(ecuio_engine_derive(&derives, &set, kRx, 2, nullptr));
    ecuio_request_engine planned;
    ASSERT_TRUE(ecuio_engine_init(&planned, 2, derived.total));
    ASSERT_TRUE(
        ecuio_engine_derive_planned(&planned, &set, kRx, 2, rows.data()));
    ASSERT_EQ(derives.count, derived.total);
    ASSERT_EQ(planned.count, derived.total);
    EXPECT_EQ(std::memcmp(derives.requests.data(), planned.requests.data(),
                          derived.total * sizeof(ecuio_request)),
              0);
    for (size_t i = 0; i < defs.size(); ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (!ecuio_engine_request_for_def(&defs[i], req, &len)) {
            continue;
        }
        const ecuio_request& r = derives.requests[rows[i]];
        EXPECT_EQ(r.len, len) << i;
        EXPECT_EQ(std::memcmp(r.bytes, req, len), 0) << i;
        EXPECT_EQ(r.channel, ecuio_engine_channel_for(&defs[i], kRx, 2)) << i;
    }
    EXPECT_EQ(planned.chans[0].requests, 4u);
    EXPECT_EQ(planned.chans[1].requests, 2u);

    /* The built-in set, whose defs share replies: the same table too */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    std::vector<uint32_t> obd2_rows(obd2->count);
    ASSERT_EQ(ecuio_engine_plan(obd2, &kRxA, 1, obd2_rows.data(), &derived),
              ecuio_plan_result::ok);
    ASSERT_TRUE(ecuio_engine_init(&derives, 1));
    ASSERT_TRUE(ecuio_engine_derive(&derives, obd2, &kRxA, 1, nullptr));
    ASSERT_TRUE(ecuio_engine_init(&planned, 1, derived.total));
    ASSERT_TRUE(ecuio_engine_derive_planned(&planned, obd2, &kRxA, 1,
                                            obd2_rows.data()));
    ASSERT_EQ(derives.count, derived.total);
    ASSERT_EQ(planned.count, derived.total);
    EXPECT_EQ(std::memcmp(derives.requests.data(), planned.requests.data(),
                          derived.total * sizeof(ecuio_request)),
              0);
    EXPECT_LT(derived.total, obd2->count) << "its fields share replies";

    /* No set, no defs, no rows wanted, no channel */
    EXPECT_EQ(ecuio_engine_plan(nullptr, kRx, 2, nullptr, &derived),
              ecuio_plan_result::ok);
    EXPECT_EQ(derived.total, 0u);
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, nullptr, &derived),
              ecuio_plan_result::ok);
    EXPECT_EQ(derived.total, 6u);
    ASSERT_EQ(ecuio_engine_plan(&set, nullptr, 0, nullptr, &derived),
              ecuio_plan_result::ok);
    EXPECT_EQ(derived.total, 4u) << "every def on channel 0: the DIDs once";
    EXPECT_EQ(derived.module[0], 4u);
}

/* Each module holds ECUIO_MODULE_MAX_REQUESTS and a set as many modules
 * of that as it declares: the plan stops at the first def past its own
 * module's most and names it */
TEST(RequestTableTest, APlanStopsAtTheFirstDefPastItsModulesMost)
{
    std::vector<ecuio_signal_def_t> defs = two_full_modules();
    defs.push_back(did_def(kRxA, 0x1000)); /* a repeat: no new request */
    ecuio_signal_set_t set = set_of(defs);
    ecuio_derived derived;
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, nullptr, &derived),
              ecuio_plan_result::ok);
    EXPECT_EQ(derived.total, 2 * kMost);
    EXPECT_EQ(derived.module[0], kMost);
    EXPECT_EQ(derived.module[1], kMost);
    /* The same defs with no channel are one module's */
    ASSERT_EQ(ecuio_engine_plan(&set, nullptr, 0, nullptr, &derived),
              ecuio_plan_result::ok)
        << "the two modules ask the same DIDs, which one channel asks once";
    EXPECT_EQ(derived.total, kMost);

    const uint32_t past = static_cast<uint32_t>(defs.size());
    defs.push_back(did_def(kRxB, 0x9000)); /* module B's 1025th */
    defs.push_back(did_def(kRxA, 0x9001));
    set = set_of(defs);
    std::vector<uint32_t> rows(defs.size());
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, rows.data(), &derived),
              ecuio_plan_result::module_full);
    EXPECT_EQ(derived.overflow_def, past);
    EXPECT_EQ(derived.total, 2 * kMost) << "the count stops there";

    std::vector<ecuio_signal_def_t> one;
    add_dids(one, kRxA, kMost + 1);
    set = set_of(one);
    ASSERT_EQ(ecuio_engine_plan(&set, nullptr, 0, nullptr, &derived),
              ecuio_plan_result::module_full);
    EXPECT_EQ(derived.overflow_def, kMost);
}

/* derive_planned stops, saying so, when the table's room runs out */
TEST(RequestTableTest, DerivePlannedStopsWhenTheRoomRunsOut)
{
    std::vector<ecuio_signal_def_t> defs;
    add_dids(defs, kRxA, 5);
    const ecuio_signal_set_t set = set_of(defs);
    std::vector<uint32_t> rows(defs.size());
    ecuio_derived derived;
    ASSERT_EQ(ecuio_engine_plan(&set, kRx, 2, rows.data(), &derived),
              ecuio_plan_result::ok);
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 2, 3));
    EXPECT_FALSE(ecuio_engine_derive_planned(&e, &set, kRx, 2, rows.data()));
    EXPECT_EQ(e.count, 3u);
}

/* A set past a rotation's most: every request parked and asked for
 * nothing until a focus keeps its row, which takes its timing row then;
 * the focus's end parks them all again and the rows stay */
TEST(RequestTableTest, APickOnlyTableAsksNothingUntilAFocusKeepsARow)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    EXPECT_TRUE(e->pick_only);
    EXPECT_TRUE(e->focused);
    EXPECT_EQ(in_rotation(*e), 0u);
    EXPECT_EQ(e->timing_taken, 0u);
    EXPECT_EQ(e->chans[0].requests, 0u);
    EXPECT_EQ(e->chans[1].requests, 0u);
    int64_t now = 0;
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(e.get(), 1, now), ECUIO_ENGINE_NONE);
    EXPECT_TRUE(ecuio_engine_unpicked(e.get()))
        << "its silence is no quiet link";
    EXPECT_EQ(ecuio_engine_next_event_us(e.get(), now), INT64_MAX);

    /* Rows 5 and 7 are module A's, row kMost + 476 module B's */
    const uint32_t b_row = kMost + 476;
    keep_rows(e.get(), {7, 5, b_row});
    EXPECT_FALSE(ecuio_engine_unpicked(e.get()));
    EXPECT_EQ(in_rotation(*e), 3u);
    EXPECT_EQ(e->chans[0].requests, 2u);
    EXPECT_EQ(e->chans[1].requests, 1u);
    EXPECT_EQ(e->timing_taken, 3u);
    EXPECT_EQ(e->timing_of[5], 0u) << "in table order";
    EXPECT_EQ(e->timing_of[7], 1u);
    EXPECT_EQ(e->timing_of[b_row], 2u);
    EXPECT_EQ(e->timing_rows[2], b_row);
    EXPECT_EQ(e->timing_of[6], ECUIO_TIMING_NONE);
    EXPECT_EQ(ask(e.get(), 0, now), 5);
    EXPECT_EQ(ask(e.get(), 0, now), 7);
    EXPECT_EQ(ask(e.get(), 0, now), 5);
    EXPECT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(b_row));
    EXPECT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(b_row));

    /* The focus's end: nothing asked, not the whole set */
    ecuio_engine_unfocus(e.get());
    EXPECT_TRUE(e->focused) << "it never asks its whole set";
    EXPECT_EQ(in_rotation(*e), 0u);
    EXPECT_EQ(e->chans[0].requests, 0u);
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(e->timing_taken, 3u) << "a row is kept";
    EXPECT_TRUE(ecuio_engine_unpicked(e.get()));

    /* Picks that were all dropped were asked: their silence counts */
    keep_rows(e.get(), {5});
    ecuio_engine_drop(e.get(), 5, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    EXPECT_EQ(in_rotation(*e), 0u);
    EXPECT_FALSE(ecuio_engine_unpicked(e.get()));
    ecuio_engine_unfocus(e.get());

    /* The next focus: a request asked before keeps its row, a new one
     * takes the next */
    keep_rows(e.get(), {7, 9});
    EXPECT_EQ(e->timing_of[7], 1u);
    EXPECT_EQ(e->timing_of[9], 3u);
    EXPECT_EQ(e->timing_taken, 4u);
    EXPECT_EQ(in_rotation(*e), 2u);
    EXPECT_EQ(e->untimed, 0u);

    /* A request dropped before a focus kept it is never asked, so it
     * takes no row, though the focus counts it kept */
    ecuio_engine_drop(e.get(), 11, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    keep_rows(e.get(), {7, 11, 13});
    EXPECT_EQ(e->kept, 3u);
    EXPECT_EQ(in_rotation(*e), 2u);
    EXPECT_EQ(e->timing_of[11], ECUIO_TIMING_NONE);
    EXPECT_EQ(e->timing_of[13], 4u) << "the next row is the next asked one's";
    EXPECT_EQ(e->timing_taken, 5u);
    EXPECT_EQ(e->untimed, 0u);
    EXPECT_EQ(e->timing_of[5], 0u) << "one dropped after it was asked keeps "
                                      "its row";

    /* A table asked whole is never unpicked, a focus on it or not */
    auto whole = std::make_unique<ecuio_request_engine>();
    ASSERT_TRUE(ecuio_engine_init(whole.get(), 1, 4));
    const uint8_t req[3] = {0x22, 0x10, 0x00};
    ASSERT_EQ(ecuio_engine_add(whole.get(), 0, req, 3),
              ecuio_engine_add_result::added);
    EXPECT_FALSE(ecuio_engine_unpicked(whole.get()));
    const uint8_t none[4] = {};
    ecuio_engine_focus(whole.get(), none);
    EXPECT_FALSE(ecuio_engine_unpicked(whole.get()));
}

/* Reply timing records at a request's pool row, never its table row */
TEST(RequestTableTest, TimingRecordsAtThePoolRow)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    auto timing = std::make_unique<ecuio_timing_table>();
    e->timing = timing.get();
    keep_rows(e.get(), {700, 900});
    ASSERT_EQ(e->timing_of[700], 0u);
    ASSERT_EQ(e->timing_of[900], 1u);
    int64_t now = 0;
    ASSERT_EQ(ask(e.get(), 0, now), 700);
    ASSERT_EQ(ask(e.get(), 0, now), 900);
    ASSERT_EQ(ask(e.get(), 0, now, false), 700);
    EXPECT_EQ(timing->rows[0].answered, 1u);
    EXPECT_EQ(timing->rows[0].timeouts, 1u);
    EXPECT_EQ(timing->rows[0].latency.count, 1u);
    EXPECT_EQ(timing->rows[1].answered, 1u);
    EXPECT_EQ(timing->rows[1].timeouts, 0u);
    EXPECT_EQ(timing->rows[700].answered + timing->rows[700].timeouts +
                  timing->rows[900].answered,
              0u)
        << "nothing at the table's own indexes";

    /* A refusal is the row's too */
    ASSERT_EQ(ecuio_engine_due(e.get(), 0, now), 900);
    ecuio_engine_sent(e.get(), 0, 900, true, now);
    const uint8_t refused[3] = {0x7F, 0x22, 0x22};
    EXPECT_EQ(ecuio_engine_on_frame(e.get(), 0, refused, sizeof refused,
                                    now + kMs, nullptr),
              ecuio_reply::negative);
    EXPECT_EQ(timing->rows[1].refused, 1u);
}

/* The last module's answering bit is its own: set by its frame, cleared
 * by its row's silence, and never channel 15's, which shares its low
 * bits */
TEST(RequestTableTest, TheLastModulesAnsweringBitIsItsOwn)
{
    constexpr uint32_t last = ECUIO_MAX_CHANNELS - 1;
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_TRUE(ecuio_engine_init(e.get(), ECUIO_MAX_CHANNELS));
    const uint8_t a[3] = {0x22, 0x10, 0x01};
    const uint8_t b[3] = {0x22, 0x20, 0x01};
    ASSERT_EQ(ecuio_engine_add(e.get(), 15, a, 3),
              ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(e.get(), last, b, 3),
              ecuio_engine_add_result::added);
    int64_t now = 0;
    ASSERT_EQ(ask(e.get(), last, now), 1);
    EXPECT_EQ(e->answering, 1u << last);
    ASSERT_EQ(ask(e.get(), 15, now), 0);
    EXPECT_EQ(e->answering, (1u << last) | (1u << 15));
    ASSERT_EQ(ask(e.get(), last, now, false), 1);
    EXPECT_EQ(e->answering, 1u << 15) << "channel 31's wait ran out";
}

/* A check's reply is timed only at a row a focus once kept: a row no
 * pick has kept has no pool row, and the check takes none, so a pick-only
 * table's checks spend no pool on rows nobody picked */
TEST(RequestTableTest, ACheckIsTimedOnlyAtARowAFocusOnceKept)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    auto timing = std::make_unique<ecuio_timing_table>();
    e->timing = timing.get();
    int64_t now = 0;
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_EQ(ask(e.get(), 0, now), 0);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_EQ(ask(e.get(), 0, now, false), 0);
    EXPECT_EQ(e->timing_taken, 0u);
    EXPECT_EQ(e->timing_of[0], ECUIO_TIMING_NONE);
    EXPECT_EQ(timing->rows[0].answered + timing->rows[0].timeouts, 0u);

    keep_rows(e.get(), {kMost});
    keep_rows(e.get(), {});
    ASSERT_TRUE(ecuio_engine_unpicked(e.get()));
    ASSERT_EQ(e->timing_of[kMost], 0u);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    ASSERT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(kMost));
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    ASSERT_EQ(ask(e.get(), 1, now, false), static_cast<int32_t>(kMost));
    EXPECT_EQ(timing->rows[0].answered, 1u);
    EXPECT_EQ(timing->rows[0].timeouts, 1u);
    EXPECT_EQ(e->timing_taken, 1u);
}

/* Past the pool a kept request is asked all the same, with no timing
 * row, and counted once however often a focus keeps it */
TEST(RequestTableTest, PastThePoolAKeptRequestIsAskedUntimed)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    auto timing = std::make_unique<ecuio_timing_table>();
    e->timing = timing.get();

    /* Module A's every request: the whole pool */
    std::vector<uint32_t> all_a;
    for (uint32_t i = 0; i < kMost; ++i) {
        all_a.push_back(i);
    }
    keep_rows(e.get(), all_a);
    ASSERT_EQ(e->timing_taken, kPool);
    EXPECT_EQ(e->untimed, 0u);

    /* Ten of module B's and one of A's */
    std::vector<uint32_t> next = {3};
    for (uint32_t i = 0; i < 10; ++i) {
        next.push_back(kMost + i);
    }
    keep_rows(e.get(), next);
    EXPECT_EQ(e->untimed, 10u);
    EXPECT_EQ(e->timing_taken, kPool);
    EXPECT_EQ(in_rotation(*e), 11u);
    EXPECT_EQ(e->chans[1].requests, 10u);
    EXPECT_EQ(e->timing_of[kMost], ECUIO_TIMING_SPENT);
    EXPECT_EQ(e->timing_of[3], 3u);

    const auto recorded = [&timing] {
        uint64_t n = 0;
        for (uint32_t i = 0; i < kPool; ++i) {
            const timing_row& r = timing->rows[i];
            n += r.answered + r.refused + r.timeouts + r.latency.count;
        }
        return n;
    };
    int64_t now = 0;
    ASSERT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(kMost));
    ASSERT_EQ(ask(e.get(), 1, now, false), static_cast<int32_t>(kMost + 1));
    EXPECT_EQ(recorded(), 0u) << "asked, answered, timed out: nothing timed";
    ASSERT_EQ(ask(e.get(), 0, now), 3);
    EXPECT_EQ(timing->rows[3].answered, 1u);
    EXPECT_EQ(recorded(), 2u) << "the answer and its latency, row 3's alone";

    keep_rows(e.get(), next);
    EXPECT_EQ(e->untimed, 10u) << "counted once";
    ecuio_engine_unfocus(e.get());
    keep_rows(e.get(), {kMost, kMost + 20});
    EXPECT_EQ(e->untimed, 11u);
}

/* The engine keeps which channels' ECUs answer: a frame from one sets
 * its bit, a wait that runs out on it clears it. With nothing picked, a
 * wait running out counts toward no timeout while another module
 * answers. */
TEST(RequestTableTest, AnAnsweringBitEndsOnlyAtATableRequestsSilence)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    EXPECT_EQ(e->answering, 0u);
    EXPECT_FALSE(ecuio_engine_unpicked_answering(e.get()))
        << "nothing picked, and nothing heard";

    int64_t now = 0;
    keep_rows(e.get(), {5, kMost + 2});
    EXPECT_EQ(ask(e.get(), 0, now), 5);
    EXPECT_EQ(e->answering, 1u);
    EXPECT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(kMost + 2));
    EXPECT_EQ(e->answering, 3u);
    EXPECT_FALSE(ecuio_engine_unpicked_answering(e.get()))
        << "with picks every timeout counts";
    EXPECT_EQ(ask(e.get(), 1, now, false), static_cast<int32_t>(kMost + 2));
    EXPECT_EQ(e->answering, 1u) << "module B's wait ran out";

    ecuio_engine_unfocus(e.get());
    EXPECT_TRUE(ecuio_engine_unpicked_answering(e.get()))
        << "module A's last request was answered";

    /* A frame that answers nothing still shows its ECU there */
    const uint8_t stray[3] = {0x7F, 0x22, 0x31};
    EXPECT_EQ(
        ecuio_engine_on_frame(e.get(), 1, stray, sizeof stray, now, nullptr),
        ecuio_reply::none);
    EXPECT_EQ(e->answering, 3u);

    /* Each module's check unanswered: no module answers */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    EXPECT_EQ(ask(e.get(), 0, now, false), 0);
    EXPECT_TRUE(ecuio_engine_unpicked_answering(e.get())) << "module B still";
    EXPECT_EQ(ask(e.get(), 1, now, false), static_cast<int32_t>(kMost + 3))
        << "past the pick module B left unanswered";
    EXPECT_EQ(e->answering, 0u);
    EXPECT_FALSE(ecuio_engine_unpicked_answering(e.get()));

    /* An identification item's wait running out says nothing of its
     * ECU, which may not serve the item: the bit stays */
    (void)ecuio_engine_on_frame(e.get(), 0, stray, sizeof stray, now, nullptr);
    ASSERT_EQ(e->answering, 1u);
    ecuio_engine_identify(e.get());
    for (int item = 0; item < 3; ++item) {
        ASSERT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_ONCE);
        ecuio_engine_sent(e.get(), 0, ECUIO_ENGINE_ONCE, true, now);
        now += ECUIO_ENGINE_WAIT_US;
        ASSERT_TRUE(ecuio_engine_expire(e.get(), 0, now));
        EXPECT_EQ(e->answering, 1u) << "item " << item;
    }
    EXPECT_TRUE(ecuio_engine_unpicked_answering(e.get()));
}

/* A check asks one of a channel's requests that aren't dropped once,
 * parked or not, moving on past one left unanswered, and is no part of
 * the rotation: no sweep counts it, no row is kept or takes a pool row
 * for it, and nothing follows it. A pick or a recovery before it goes
 * out withdraws it. */
TEST(RequestTableTest, ACheckAsksAParkedRequestOnceOutsideTheRotation)
{
    const std::vector<ecuio_signal_def_t> defs = two_full_modules();
    auto e = std::make_unique<ecuio_request_engine>();
    ASSERT_NO_FATAL_FAILURE(pick_only_table(e.get(), defs));
    int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);

    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ecuio_engine_due(e.get(), 1, now), ECUIO_ENGINE_NONE)
        << "the other channel has none";
    EXPECT_EQ(ask(e.get(), 0, now), 0) << "the channel's first request";
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE) << "once";
    EXPECT_TRUE(ecuio_engine_unpicked(e.get()));
    EXPECT_EQ(e->kept, 0u);
    EXPECT_EQ(in_rotation(*e), 0u);
    EXPECT_EQ(e->chans[0].requests, 0u);
    EXPECT_EQ(e->chans[0].sweep_asked, 0u);
    EXPECT_EQ(e->timing_taken, 0u) << "a check is timed nowhere";
    EXPECT_EQ(e->requests[0].parked, 1u);
    EXPECT_EQ(e->answering, 1u);

    /* Module B's first is its own; a dropped request is passed by */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    EXPECT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(kMost));
    ecuio_engine_drop(e.get(), 0, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    ecuio_engine_drop(e.get(), 1, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now), 2);

    /* Asked for twice before it goes out, it goes out once: the request
     * the last check had answered. Its wait run out, the next check asks
     * the one after it, so a request the ECU is silent on isn't the only
     * one ever tried */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now, false), 2);
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now, false), 3);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now), 4);

    /* A check the driver took back (a failed write, a reply an adapter
     * cut short) is the next check's again */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_EQ(ecuio_engine_due(e.get(), 0, now), 4);
    ecuio_engine_sent(e.get(), 0, 4, false, now);
    now += ECUIO_ENGINE_GAP_US;
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE)
        << "taken back, it waits for the next check";
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now, false), 4);
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    EXPECT_EQ(ask(e.get(), 0, now), 5);

    /* Past the channel's last request the turn starts over */
    e->chans[1].check_from = e->count - 1;
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    EXPECT_EQ(ask(e.get(), 1, now, false), static_cast<int32_t>(e->count - 1));
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    EXPECT_EQ(ask(e.get(), 1, now), static_cast<int32_t>(kMost));

    /* A pick taken before a check goes out withdraws it: the rotation
     * alone is asked, and the check doesn't come back with the pick gone */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    keep_rows(e.get(), {7, 9});
    EXPECT_EQ(ask(e.get(), 0, now), 7);
    EXPECT_FALSE(e->chans[0].check_due);
    EXPECT_EQ(ask(e.get(), 0, now), 9);
    EXPECT_EQ(ask(e.get(), 0, now), 7);
    ecuio_engine_unfocus(e.get());
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);

    /* So does the link's recovery (the worker's rescan) */
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 0));
    ASSERT_TRUE(ecuio_engine_ask_check(e.get(), 1));
    ecuio_engine_rescan(e.get());
    EXPECT_FALSE(e->chans[0].check_due);
    EXPECT_FALSE(e->chans[1].check_due);
    EXPECT_EQ(ecuio_engine_due(e.get(), 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(e.get(), 1, now), ECUIO_ENGINE_NONE);

    /* No channel, or none of its requests left: nothing to ask */
    EXPECT_FALSE(ecuio_engine_ask_check(e.get(), 2));
    auto one = std::make_unique<ecuio_request_engine>();
    ASSERT_TRUE(ecuio_engine_init(one.get(), 2, 4));
    const uint8_t req[3] = {0x22, 0x10, 0x00};
    ASSERT_EQ(ecuio_engine_add(one.get(), 0, req, 3),
              ecuio_engine_add_result::added);
    EXPECT_FALSE(ecuio_engine_ask_check(one.get(), 1)) << "a channel with none";
    ecuio_engine_drop(one.get(), 0, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    EXPECT_FALSE(ecuio_engine_ask_check(one.get(), 0))
        << "its only one dropped";
    EXPECT_FALSE(one->chans[0].check_due);
}

/* The peek names each class's next row and moves no cursor: due then
 * hands out the class's row the peek named, and stamps it once sent */
TEST(RequestTableTest, ThePeekNamesTheNextRowOfEachClass)
{
    ecuio_request_engine e;
    ASSERT_TRUE(ecuio_engine_init(&e, 1));
    for (const uint8_t pid : {uint8_t{0x0C}, uint8_t{0x05}, uint8_t{0x0D}}) {
        const uint8_t q[2] = {0x01, pid};
        ASSERT_EQ(ecuio_engine_add(&e, 0, q, 2),
                  ecuio_engine_add_result::added);
    }
    ecuio_engine_classify(&e, [](uint8_t pid) { return pid != 0x05; });
    int64_t now = kMs;
    for (int i = 0; i < 9; ++i) {
        const int32_t fast = ecuio_engine_peek_of(&e, 0, false);
        const int32_t slow = ecuio_engine_peek_of(&e, 0, true);
        ASSERT_EQ(ecuio_engine_peek_of(&e, 0, false), fast) << i;
        ASSERT_EQ(slow, 1) << "the slow class's one row";
        const int32_t r = ecuio_engine_due(&e, 0, now);
        ASSERT_GE(r, 0) << i;
        EXPECT_EQ(r, e.requests[r].slow != 0 ? slow : fast) << i;
        ecuio_engine_sent(&e, 0, r, true, now);
        EXPECT_EQ(e.asked_us[r], now) << i;
        const uint8_t reply[4] = {0x41, e.requests[r].bytes[1], 0x2A, 0x2A};
        (void)ecuio_engine_on_frame(&e, 0, reply, 4, now + kMs, nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
}
