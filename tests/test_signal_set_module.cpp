// File/layer: tests/test_signal_set_module.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalSetModuleTest (test_signals)
//
// The module a def of a set is read on (ecuio_signal_set_def_module):
// enrolment's channel for a def a session asks, the reply ID for one
// nothing asks, a computed def's leaves when they share one.

#include <gtest/gtest.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <vector>

namespace {

constexpr uint32_t kNone = ECUIO_SIGNAL_MODULE_NONE;

/* A number def at rx asked by 22 <did> (a 62 <did> key) */
ecuio_signal_def_t did_def(uint16_t id, uint32_t rx, uint16_t did)
{
    ecuio_signal_def_t d{};
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

/* A def nothing asks: a frame at rx with no key */
ecuio_signal_def_t heard_def(uint16_t id, uint32_t rx)
{
    ecuio_signal_def_t d = did_def(id, rx, 0);
    d.mux_length = 0;
    d.mux_key = 0;
    return d;
}

/* A computed def whose program is computed[program] */
ecuio_signal_def_t calc_def(uint16_t id, uint16_t program)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.kind = ECUIO_VALUE_FLOAT;
    d.scale = 1.0;
    d.flags = ECUIO_DEF_FLAG_COMPUTED;
    d.table_first = program;
    return d;
}

/* A program reading the given def IDs */
ecuio_computed_t reads(std::vector<uint16_t> ids)
{
    ecuio_computed_t p{};
    for (const uint16_t id : ids) {
        p.input_ids[p.input_count] = id;
        p.steps[p.step_count++] =
            static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | p.input_count);
        if (p.input_count > 0) {
            p.steps[p.step_count++] = ECUIO_COMPUTED_ADD;
        }
        ++p.input_count;
    }
    return p;
}

/* Engine at 7E8 (asked on 7E0), body at 7EA */
const ecuio_channel_decl_t kTwo[] = {{0x7E8, 0x7E0, 0, 0},
                                     {0x7EA, 0x7E2, 0, 0}};

struct Set {
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_computed_t> computed;
    ecuio_signal_set_t set{};

    Set(std::vector<ecuio_signal_def_t> d, std::vector<ecuio_computed_t> c = {})
        : defs(std::move(d)), computed(std::move(c))
    {
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(defs.size());
        set.defs = defs.data();
        set.channel_count = 2;
        set.channels = kTwo;
        set.computed_count = static_cast<uint32_t>(computed.size());
        set.computed = computed.empty() ? nullptr : computed.data();
    }

    uint32_t module(size_t i) const
    {
        return ecuio_signal_set_def_module(&set, &defs[i]);
    }
};

} // namespace

// A def asked by its DID is read on the module whose reply ID is its
// frame's, as the session enrols its request
TEST(SignalSetModuleTest, ARequestedDefIsReadOnItsReplysModule)
{
    const Set s({did_def(30000, 0x7E8, 0xF40C), did_def(30001, 0x7EA, 0x1E1C),
                 did_def(30002, 0x7EA, 0x1E1D)});
    EXPECT_EQ(s.module(0), 0u);
    EXPECT_EQ(s.module(1), 1u);
    EXPECT_EQ(s.module(2), 1u);
}

// A requested def on no declared reply is asked on channel 0, so it is
// read there; one nothing asks is read only where its reply ID is
TEST(SignalSetModuleTest, ADefOnNoDeclaredReplyIsChannelZerosAsEnrolmentHasIt)
{
    const Set s({did_def(30000, 0x7EC, 0xF40C), heard_def(30001, 0x7EA),
                 heard_def(30002, 0x7EC)});
    EXPECT_EQ(s.module(0), 0u);
    EXPECT_EQ(s.module(1), 1u);
    EXPECT_EQ(s.module(2), kNone);
}

// obd2's Boost (MAP less baro, both 7E8) reads as the engine's; a chain
// whose leaves are all the body's reads as the body's
TEST(SignalSetModuleTest, AComputedDefTakesItsLeavesModuleWhenTheyShareOne)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    bool found = false;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        if (obd2->defs[i].id == ECUIO_OBD2_SIG_BOOST_ID) {
            found = true;
            EXPECT_EQ(ecuio_signal_set_def_module(obd2, &obd2->defs[i]), 0u);
        }
    }
    EXPECT_TRUE(found);
    /* d2 reads d1 and a body DID; d1 reads two body DIDs */
    const Set s({did_def(30000, 0x7E8, 0xF40C), did_def(30001, 0x7EA, 0x1E1C),
                 did_def(30002, 0x7EA, 0x1E1D), calc_def(30003, 0),
                 calc_def(30004, 1)},
                {reads({30001, 30002}), reads({30003, 30001})});
    EXPECT_EQ(s.module(3), 1u);
    EXPECT_EQ(s.module(4), 1u);
}

// Leaves on two modules, or one on none, give a computed def no module
TEST(SignalSetModuleTest, AComputedDefOnTwoModulesHasNone)
{
    const Set s({did_def(30000, 0x7E8, 0xF40C), did_def(30001, 0x7EA, 0x1E1C),
                 heard_def(30002, 0x7EC), calc_def(30003, 0),
                 calc_def(30004, 1), calc_def(30005, 2)},
                {reads({30001, 30000}), reads({30001, 30002}), reads({30003})});
    EXPECT_EQ(s.module(3), kNone);
    EXPECT_EQ(s.module(4), kNone);
    EXPECT_EQ(s.module(5), kNone);
}

// A def that isn't the set's, a set with no channel and NULLs have none
TEST(SignalSetModuleTest, AForeignDefHasNone)
{
    Set s({did_def(30000, 0x7E8, 0xF40C), did_def(30001, 0x7EA, 0x1E1C)});
    const ecuio_signal_def_t copy = s.defs[0];
    EXPECT_EQ(ecuio_signal_set_def_module(&s.set, &copy), kNone);
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    EXPECT_EQ(ecuio_signal_set_def_module(&s.set, &obd2->defs[0]), kNone);
    EXPECT_EQ(ecuio_signal_set_def_module(nullptr, &s.defs[0]), kNone);
    EXPECT_EQ(ecuio_signal_set_def_module(&s.set, nullptr), kNone);
    s.set.channel_count = 0;
    s.set.channels = nullptr;
    EXPECT_EQ(s.module(0), kNone);
}
