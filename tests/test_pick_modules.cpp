// File/layer: tests/test_pick_modules.cpp | test_obd2, Qt-free
// Testing: UNIT: PickModulesTest (the reasons a module is wanted and the
//          budget that opens them: ecuio_modules_wanted and
//          ecuio_module_in_range in request_modules.h,
//          ecuio_engine_modules_apply and _fill in request_engine_modules.h;
//          ADR-0065 step 5)

#include "test_obd2_support.h"

#include "../core/src/request_modules.h"

#include <cstdint>
#include <vector>

namespace {

constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_ENGINE_REASON_READ;
constexpr uint8_t kTalk = ECUIO_ENGINE_REASON_CONVERSATION;

/* n modules, budget modules live data may hold */
void engine(ecuio_request_engine& e, uint32_t n, uint32_t budget)
{
    ASSERT_TRUE(ecuio_engine_init(&e, n));
    e.module_budget = budget;
}

std::vector<uint8_t> states(const ecuio_request_engine& e)
{
    std::vector<uint8_t> out;
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        out.push_back(e.chans[c].module_state);
    }
    return out;
}

constexpr uint8_t O = ECUIO_MODULE_STATE_OPEN;
constexpr uint8_t N = ECUIO_MODULE_STATE_NO_ROOM;
constexpr uint8_t C = ECUIO_MODULE_STATE_CLOSED;
constexpr uint8_t G = ECUIO_MODULE_STATE_CLOSING;

/* A def on module rx asking 22 <did> */
ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did)
{
    ecuio_signal_def_t d = {};
    d.id = id;
    d.message_id = rx;
    d.mux_length = 3;
    d.mux_key = 0x620000u | did;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = 8;
    d.scale = 1.0;
    return d;
}

ecuio_signal_set_t dids_set(const std::vector<ecuio_signal_def_t>& defs)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(defs.size());
    s.defs = defs.data();
    return s;
}

} // namespace

/* 11 modules read, a budget of 9: RANGE first, then the picks' order;
 * the two the budget leaves have no room, never closed */
TEST(PickModulesTest, OverTheBudgetTheLaterModulesHaveNoRoom)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 11, 9));
    uint8_t reasons[11];
    for (uint8_t& r : reasons) {
        r = kRead;
    }
    reasons[10] = kRange | kRead;
    const uint8_t order[] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    ecuio_engine_modules_apply(&e, reasons, order, 10);
    EXPECT_EQ(states(e), (std::vector<uint8_t>{N, N, O, O, O, O, O, O, O, O,
                                               O}))
        << "module 10 by RANGE, then 9 down to 2 by the picks";
}

TEST(PickModulesTest, AConversationGoesAheadOfARead)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 3, 1));
    const uint8_t reasons[] = {kRead, kRead, kTalk};
    const uint8_t order[] = {0, 1};
    ecuio_engine_modules_apply(&e, reasons, order, 2);
    EXPECT_EQ(states(e), (std::vector<uint8_t>{N, N, O}));
}

/* A module holding a filter keeps it when a newcomer wants one, even one
 * picked ahead of it */
TEST(PickModulesTest, AModuleHoldingAFilterIsNotClosedForANewcomer)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 3, 2));
    uint8_t reasons[] = {kRead, kRead, 0};
    const uint8_t first[] = {0, 1};
    ecuio_engine_modules_apply(&e, reasons, first, 2);
    ASSERT_EQ(states(e), (std::vector<uint8_t>{O, O, C}));
    reasons[2] = kRead;
    const uint8_t later[] = {2, 0, 1};
    ecuio_engine_modules_apply(&e, reasons, later, 3);
    EXPECT_EQ(states(e), (std::vector<uint8_t>{O, O, N}));
}

/* A filter freed by a close opens the module that has waited longest */
TEST(PickModulesTest, AFreedFilterOpensTheModuleWaitingLongest)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 4, 1));
    uint8_t reasons[] = {kRead, 0, 0, 0};
    const uint8_t order[] = {0, 1, 2, 3};
    ecuio_engine_modules_apply(&e, reasons, order, 1);
    reasons[3] = kRead; /* waits first */
    ecuio_engine_modules_apply(&e, reasons, order, 4);
    reasons[1] = kRead; /* then this one */
    ecuio_engine_modules_apply(&e, reasons, order, 4);
    ASSERT_EQ(states(e), (std::vector<uint8_t>{O, N, C, N}));
    reasons[0] = 0;
    ecuio_engine_modules_apply(&e, reasons, order, 4);
    EXPECT_EQ(states(e)[0], G) << "closing, its filter held until it closes";
    EXPECT_EQ(ecuio_engine_modules_settle(&e, 0), 1u);
    ecuio_engine_modules_fill(&e);
    EXPECT_EQ(states(e), (std::vector<uint8_t>{C, N, C, O}));
}

/* No reason left: a module waiting for a filter closes at once */
TEST(PickModulesTest, AWaitingModuleNoLongerWantedCloses)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 2, 1));
    uint8_t reasons[] = {kRead, kRead};
    const uint8_t order[] = {0, 1};
    ecuio_engine_modules_apply(&e, reasons, order, 2);
    ASSERT_EQ(states(e), (std::vector<uint8_t>{O, N}));
    reasons[1] = 0;
    ecuio_engine_modules_apply(&e, reasons, order, 2);
    EXPECT_EQ(states(e), (std::vector<uint8_t>{O, C}));
}

/* Adding a module changes nothing for those before it: no count decides */
TEST(PickModulesTest, AddingAModuleClosesNothing)
{
    for (const uint32_t n : {4u, 5u}) {
        ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(engine(e, n, ECUIO_MAX_CHANNELS - 1));
        std::vector<uint8_t> reasons(n, 0);
        reasons[0] = kRange;
        reasons[1] = kRead;
        ecuio_engine_modules_apply(&e, reasons.data(), nullptr, 0);
        const std::vector<uint8_t> s = states(e);
        EXPECT_EQ(std::vector<uint8_t>(s.begin(), s.begin() + 4),
                  (std::vector<uint8_t>{O, O, C, C}))
            << n << " modules";
    }
}

/* The modules a pick reads follow its defs, wherever their modules stand
 * in the set: the same defs over two module orders read the same reply
 * IDs, in the picks' order */
TEST(PickModulesTest, PluginOrderNeverDecidesWhatIsOpen)
{
    const std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7B8, 0xD801), did_def(2, 0x7B9, 0xD802),
        did_def(3, 0x7E8, 0x1E1C)};
    const ecuio_signal_set_t set = dids_set(defs);
    const uint16_t ids[] = {2, 1};
    const uint8_t picked[] = {1, 1, 0};
    const uint32_t first[] = {0x7E8, 0x7B8, 0x7B9};
    const uint32_t second[] = {0x7B9, 0x7E8, 0x7B8};
    const ecuio_modules_read a =
        ecuio_modules_wanted(&set, first, 3, ids, 2, picked, nullptr, 3);
    const ecuio_modules_read b =
        ecuio_modules_wanted(&set, second, 3, ids, 2, picked, nullptr, 3);
    ASSERT_EQ(a.count, 2u);
    ASSERT_EQ(b.count, 2u);
    EXPECT_EQ(first[a.order[0]], 0x7B9u) << "the first pick's module first";
    EXPECT_EQ(first[a.order[1]], 0x7B8u);
    EXPECT_EQ(second[b.order[0]], 0x7B9u);
    EXPECT_EQ(second[b.order[1]], 0x7B8u);
}

/* A computed pick's leaves and an attached condition's signal read their
 * modules; a def that asks nothing (a broadcast one) reads none */
TEST(PickModulesTest, LeavesAndConditionsReadTheirModulesAndAPassiveDefNone)
{
    std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7B8, 0xD801), did_def(2, 0x7B9, 0xD802),
        did_def(3, 0x7BA, 0xD803), did_def(4, 0x7BB, 0xD804)};
    defs[3].mux_length = 0; /* a broadcast frame's: no request */
    const ecuio_signal_set_t set = dids_set(defs);
    const uint32_t rx[] = {0x7B8, 0x7B9, 0x7BA, 0x7BB};
    const uint8_t leaves[] = {1, 0, 0, 1}; /* marked, under no id */
    const uint8_t conditions[] = {0, 0, 1, 0};
    const ecuio_modules_read m =
        ecuio_modules_wanted(&set, rx, 4, nullptr, 0, leaves, conditions, 4);
    EXPECT_EQ(m.mask, (1u << 0) | (1u << 2));
    ASSERT_EQ(m.count, 2u);
    EXPECT_EQ(m.order[0], 0u);
    EXPECT_EQ(m.order[1], 2u);
}

/* The support report names what wants a module, held or not; CLOSED and
 * NO_ROOM never together */
TEST(PickModulesTest, TheSupportReportSaysWhyAModuleIsWanted)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(e, 4, 2));
    const uint8_t reasons[] = {kRange, kRange | kRead, kRead, 0};
    const uint8_t order[] = {2};
    ecuio_engine_modules_apply(&e, reasons, order, 1);
    ecuio_channel_support_t s[4];
    for (uint32_t c = 0; c < 4; ++c) {
        ecuio_engine_support(&e, c, &s[c]);
    }
    EXPECT_EQ(s[0].open_reasons, ECUIO_MODULE_REASON_RANGE);
    EXPECT_EQ(s[1].open_reasons,
              ECUIO_MODULE_REASON_RANGE | ECUIO_MODULE_REASON_READ);
    EXPECT_EQ(s[2].open_reasons, ECUIO_MODULE_REASON_READ);
    EXPECT_EQ(s[2].flags & (ECUIO_SUPPORT_NO_ROOM | ECUIO_SUPPORT_CLOSED),
              ECUIO_SUPPORT_NO_ROOM)
        << "wanted past the budget";
    EXPECT_EQ(s[3].open_reasons, 0u);
    EXPECT_EQ(s[3].flags & (ECUIO_SUPPORT_NO_ROOM | ECUIO_SUPPORT_CLOSED),
              ECUIO_SUPPORT_CLOSED);
    for (uint32_t c = 0; c < 2; ++c) {
        EXPECT_EQ(s[c].flags & (ECUIO_SUPPORT_NO_ROOM | ECUIO_SUPPORT_CLOSED),
                  0u);
    }
}

