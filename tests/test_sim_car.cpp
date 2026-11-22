// File/layer: tests/test_sim_car.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SimEcuTest, SimAskTest, SimTest (test_obd2)
//
// The simulated car's faults for each module (ADR-0065 section 10): the
// faults= grammar's aim at one module (@<reply ID>), each module's own
// periods and counts, through the unbound sim, and the order create
// judges an aim in; sessions=1, a profile's modules in the extended
// session.

#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "sim_ecu.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

ecuio_sim_config parsed(const char* uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri, &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << uri << ": " << why;
    return c;
}

std::string refusal(const char* uri)
{
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri(uri, &c, why, sizeof why),
              ecuio_sim_uri_result::bad)
        << uri;
    return why;
}

/* A def keyed 62 <did> at offset 0 on rx, one byte at byte 3 */
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
    std::snprintf(d.name, sizeof d.name, "Module %X", rx);
    return d;
}

/* A profile of n modules (request 0x700 + i, reply 0x740 + i), each with
 * DID 0x1000 + i, registered under name until the scope ends */
struct Modules {
    std::string name;
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    ecuio_signal_set_t set{};

    Modules(const char* n, uint32_t count) : name(n)
    {
        for (uint32_t i = 0; i < count; ++i) {
            ecuio_channel_decl_t c{};
            c.rx_id = 0x740u + i;
            c.tx_id = 0x700u + i;
            channels.push_back(c);
            defs.push_back(did_def(static_cast<uint16_t>(100 + i), c.rx_id,
                                   static_cast<uint16_t>(0x1000 + i)));
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(defs.size());
        set.defs = defs.data();
        set.channel_count = static_cast<uint32_t>(channels.size());
        set.channels = channels.data();
        ecuio_validation_t why{};
        EXPECT_EQ(ecuio_profile_register(name.c_str(), &set, &why), ECUIO_OK)
            << why.code << " at " << why.index;
    }
    ~Modules() { EXPECT_EQ(ecuio_profile_unregister(name.c_str()), ECUIO_OK); }
    Modules(const Modules&) = delete;
    Modules& operator=(const Modules&) = delete;
};

struct Unbound {
    ecuio_sim_t* sim = nullptr;
    explicit Unbound(const std::string& uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri.c_str(), 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    std::vector<ecuio_sim_reply_t> ask(uint32_t id, const Bytes& q)
    {
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, id, q.data(),
                                static_cast<uint32_t>(q.size()), ECUIO_SIM_NOW,
                                out.data(), static_cast<uint32_t>(out.size()),
                                &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        out.resize(n);
        return out;
    }

    /* Module i's own DID asked of it: whether its one reply came late */
    bool late(uint32_t i)
    {
        const std::vector<ecuio_sim_reply_t> r =
            ask(0x700u + i, {0x22, 0x10, static_cast<uint8_t>(i)});
        EXPECT_EQ(r.size(), 1u) << "module " << i;
        return !r.empty() && (r[0].flags & ECUIO_SIM_REPLY_LATE) != 0;
    }
};

std::set<uint32_t> reply_ids(const std::vector<ecuio_sim_reply_t>& r)
{
    std::set<uint32_t> ids;
    for (const ecuio_sim_reply_t& x : r) {
        ids.insert(x.reply_id);
    }
    return ids;
}

} // namespace

// @<reply ID> aims a fault at one module: kept apart from the faults for
// every module, in any case of hex, beside them in one list
TEST(SimEcuTest, AFaultAimsAtOneModuleByItsReplyId)
{
    const ecuio_sim_config c =
        parsed("mock://sim?faults=late:3@7ea,silent:5,pending:2@741");
    EXPECT_EQ(c.faults.silent, 5u);
    EXPECT_EQ(c.faults.late, 0u) << "an aimed fault is no module's but its own";
    EXPECT_EQ(c.faults.pending, 0u);
    ASSERT_EQ(c.aimed.count, 2u);
    EXPECT_EQ(c.aimed.items[0].reply_id, 0x7EAu);
    EXPECT_EQ(c.aimed.items[0].kind, ecuio_sim_fault_kind::late);
    EXPECT_EQ(c.aimed.items[0].every, 3u);
    EXPECT_EQ(c.aimed.items[1].reply_id, 0x741u);
    EXPECT_EQ(c.aimed.items[1].kind, ecuio_sim_fault_kind::pending);
    EXPECT_EQ(c.aimed.items[1].every, 2u);
    EXPECT_EQ(parsed("mock://sim?faults=late:3").aimed.count, 0u);
}

// Each refusal of an aim, in words that say what an item may be
TEST(SimEcuTest, AnAimsRefusalsNameTheirCause)
{
    const std::string shape =
        "faults: 'late:3@' isn't <fault>:<n>[@<reply ID>], the fault one of "
        "silent, late, pending, bad_sequence or stalled, n from 1 to 10000 "
        "and the reply ID 11-bit hex";
    EXPECT_EQ(refusal("mock://sim?faults=late:3@"), shape);
    for (const char* bad :
         {"mock://sim?faults=late:3@800", "mock://sim?faults=late:3@7EG",
          "mock://sim?faults=late:3@0007E8", "mock://sim?faults=late@7E8:3",
          "mock://sim?faults=late:@7E8", "mock://sim?faults=late:3@7E8@7E9",
          "mock://sim?faults=late:0@7E8"}) {
        EXPECT_NE(refusal(bad).find("isn't <fault>:<n>[@<reply ID>]"),
                  std::string::npos)
            << bad;
    }
    std::string many = "mock://sim?faults=late:1@700";
    for (int i = 1; i < static_cast<int>(ECUIO_SIM_AIMED_MAX); ++i) {
        char item[32];
        std::snprintf(item, sizeof item, ",late:1@%X", 0x700 + i);
        many += item;
    }
    EXPECT_EQ(parsed(many.c_str()).aimed.count, ECUIO_SIM_AIMED_MAX);
    many += ",late:1@7FF";
    EXPECT_EQ(refusal(many.c_str()),
              "faults: more than 32 faults aimed at a module");
}

// A module's periods: the faults for every module, then the ones aimed at
// it, each replacing that fault alone; the later of two in one scope
TEST(SimEcuTest, EachModulesFaultsAreTheAllAndItsOwnAims)
{
    const ecuio_sim_config c =
        parsed("mock://sim?faults=late:4,silent:9,late:2@741,stalled:3@741,"
               "late:5@741,pending:7@742,silent:11");
    const ecuio_sim_faults all = ecuio_sim_faults_for(c.faults, c.aimed, 0x740);
    EXPECT_EQ(all.late, 4u);
    EXPECT_EQ(all.silent, 11u) << "the later of two for every module";
    EXPECT_EQ(all.stalled, 0u);
    EXPECT_EQ(all.pending, 0u);
    const ecuio_sim_faults one = ecuio_sim_faults_for(c.faults, c.aimed, 0x741);
    EXPECT_EQ(one.late, 5u) << "the later aim replaces both";
    EXPECT_EQ(one.silent, 11u);
    EXPECT_EQ(one.stalled, 3u);
    EXPECT_EQ(one.pending, 0u);
    const ecuio_sim_faults other =
        ecuio_sim_faults_for(c.faults, c.aimed, 0x742);
    EXPECT_EQ(other.late, 4u);
    EXPECT_EQ(other.pending, 7u);
}

// A second faults= key adds to the first, as the timing keys do: its
// faults for every module beside the first's, its aims after them, and
// the aims' bound counted across both
TEST(SimEcuTest, ASecondFaultsKeyAddsToTheFirst)
{
    const ecuio_sim_config c =
        parsed("mock://sim?faults=silent:3@7E9,late:4&faults=late:5,pending:2");
    EXPECT_EQ(c.faults.late, 5u) << "the later of two for every module";
    EXPECT_EQ(c.faults.pending, 2u);
    ASSERT_EQ(c.aimed.count, 1u);
    EXPECT_EQ(c.aimed.items[0].reply_id, 0x7E9u);
    EXPECT_EQ(c.aimed.items[0].kind, ecuio_sim_fault_kind::silent);
    EXPECT_EQ(ecuio_sim_faults_for(c.faults, c.aimed, 0x7E9).silent, 3u);

    std::string many = "mock://sim?faults=late:1@700";
    for (uint32_t i = 1; i < ECUIO_SIM_AIMED_MAX; ++i) {
        char item[16];
        std::snprintf(item, sizeof item, ",late:1@%X", 0x700u + i);
        many += item;
    }
    EXPECT_EQ(parsed(many.c_str()).aimed.count, ECUIO_SIM_AIMED_MAX);
    EXPECT_EQ(refusal((many + "&faults=late:1@7FF").c_str()),
              "faults: more than 32 faults aimed at a module");
}

// The fate of one module's replies, from its own periods and counts
TEST(SimEcuTest, EachCountIsItsOwn)
{
    ecuio_sim_faults f;
    f.late = 2;
    ecuio_sim_counts a, b;
    const uint8_t did[3] = {0x22, 0x10, 0x00};
    EXPECT_EQ(ecuio_sim_next_fate(f, a, did, 3, 4), ecuio_sim_fate::intact);
    EXPECT_EQ(ecuio_sim_next_fate(f, b, did, 3, 4), ecuio_sim_fate::intact)
        << "b's first, not the second of a shared count";
    EXPECT_EQ(ecuio_sim_next_fate(f, a, did, 3, 4), ecuio_sim_fate::late);
    EXPECT_EQ(ecuio_sim_next_fate(f, b, did, 3, 4), ecuio_sim_fate::late);
    EXPECT_EQ(a.singles, 2u);
    EXPECT_EQ(b.singles, 2u);
}

// late:3 for every module: each module's third reply comes late, whatever
// the others were asked in between
TEST(SimAskTest, EachModuleCountsItsOwnFaults)
{
    Modules m("sim-car-counts", 3);
    Unbound u("mock://ask?profile=sim-car-counts&faults=late:3");
    ASSERT_NE(u.sim, nullptr);
    EXPECT_FALSE(u.late(0));
    EXPECT_FALSE(u.late(0));
    EXPECT_FALSE(u.late(1)) << "module 1's first: a shared count's third";
    EXPECT_FALSE(u.late(2));
    EXPECT_TRUE(u.late(0));
    EXPECT_FALSE(u.late(1));
    EXPECT_TRUE(u.late(1));
    EXPECT_FALSE(u.late(2));
    EXPECT_TRUE(u.late(2));
}

// silent:1@741 silences that module alone: asked directly or functionally,
// every other ECU answers
TEST(SimAskTest, AnAimedFaultBreaksOneModuleAlone)
{
    Modules m("sim-car-aimed", 3);
    Unbound u("mock://ask?profile=sim-car-aimed&faults=silent:1@741");
    ASSERT_NE(u.sim, nullptr);
    EXPECT_TRUE(u.ask(0x701, {0x22, 0x10, 0x01}).empty());
    EXPECT_EQ(u.ask(0x700, {0x22, 0x10, 0x00}).size(), 1u);
    EXPECT_EQ(u.ask(0x702, {0x22, 0x10, 0x02}).size(), 1u);
    /* 01 00 is the engine ECU's alone; each module answers its own DID,
     * the others' refusals unsaid */
    EXPECT_EQ(reply_ids(u.ask(0x7DF, {0x01, 0x00})), std::set<uint32_t>{0x7E8});
    for (uint8_t i = 0; i < 3; ++i) {
        const std::set<uint32_t> ids = reply_ids(u.ask(0x7DF, {0x22, 0x10, i}));
        EXPECT_EQ(ids.count(0x741u), 0u) << int(i);
        EXPECT_EQ(ids.size(), i == 1 ? 0u : 1u) << int(i);
    }
}

// An aim replaces its fault for that module: late:1 for every module,
// late:2@741 makes 741 late every second reply and the rest every reply
TEST(SimAskTest, AnAimReplacesItsFaultForThatModule)
{
    Modules m("sim-car-replace", 2);
    Unbound u("mock://ask?profile=sim-car-replace&faults=late:1,late:2@741");
    ASSERT_NE(u.sim, nullptr);
    for (int k = 0; k < 4; ++k) {
        EXPECT_TRUE(u.late(0)) << k;
        EXPECT_EQ(u.late(1), k % 2 == 1) << k;
    }
}

// A module asked on a reply ID, another module's or its own, is refused
// at create: the sim would hear its own replies as another node's and
// yield at the first request
TEST(SimAskTest, ARequestIdThatIsAReplyIdIsRefused)
{
    struct Shape {
        std::vector<ecuio_channel_decl_t> channels;
        const char* why;
    };
    const Shape shapes[] = {
        {{{0x740, 0x700, 0, 0}, {0x741, 0x740, 0, 0}},
         "profile 'sim-car-crossed': channel 1's request ID 0x740 is "
         "channel 0's reply ID: the simulated ECU would take its requests "
         "for another node's replies"},
        {{{0x740, 0x700, 0, 0}, {0x741, 0x741, 0, 0}},
         "profile 'sim-car-crossed': channel 1's request ID 0x741 is "
         "channel 1's reply ID: the simulated ECU would take its requests "
         "for another node's replies"},
    };
    for (const Shape& shape : shapes) {
        std::vector<ecuio_channel_decl_t> channels = shape.channels;
        std::vector<ecuio_signal_def_t> defs;
        for (const ecuio_channel_decl_t& c : channels) {
            defs.push_back(did_def(static_cast<uint16_t>(100 + defs.size()),
                                   c.rx_id, 0x1000));
        }
        ecuio_signal_set_t set{};
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(defs.size());
        set.defs = defs.data();
        set.channel_count = static_cast<uint32_t>(channels.size());
        set.channels = channels.data();
        ecuio_validation_t v{};
        ASSERT_EQ(ecuio_profile_register("sim-car-crossed", &set, &v), ECUIO_OK)
            << v.code << " at " << v.index;
        ecuio_sim_t* sim = nullptr;
        EXPECT_EQ(ecuio_sim_create_unbound("mock://ask?profile=sim-car-crossed",
                                           0, &sim),
                  ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(sim, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(), shape.why);
        ecuio_sim_destroy(sim);
        EXPECT_EQ(ecuio_profile_unregister("sim-car-crossed"), ECUIO_OK);
    }
}

// An aim at a reply ID no ECU of the sim answers on is refused with the
// IDs it does answer on; modules=2's and a profile's modules are aimed at
TEST(SimAskTest, AnAimNoModuleAnswersIsRefused)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound("mock://ask?faults=late:3@7EA", 0, &sim),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(sim, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "faults: late:3@7EA aims at a module the simulated car "
                 "hasn't; it answers on 7E8");
    EXPECT_EQ(ecuio_sim_create_unbound(
                  "mock://ask?modules=2&faults=bad_sequence:2@7E9", 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_sim_destroy(sim);
    sim = nullptr;

    Modules m("sim-car-refused", ECUIO_MAX_CHANNELS);
    EXPECT_EQ(
        ecuio_sim_create_unbound(
            "mock://ask?profile=sim-car-refused&faults=stalled:4@7EA", 0, &sim),
        ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "faults: stalled:4@7EA aims at a module the simulated car "
                 "hasn't; it answers on 7E8, 740, 741, 742, 743, 744, 745, "
                 "746 and 25 more");
    EXPECT_EQ(
        ecuio_sim_create_unbound(
            "mock://ask?profile=sim-car-refused&faults=late:2@75F", 0, &sim),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_sim_destroy(sim);
}

// sessions=1 gives a profile's modules the extended session: 10 03
// enters it and 3E 00 is answered, with no security access (27 refused
// as any service a module lacks); without the key 10 03 is refused, and
// the key needs a profile
TEST(SimAskTest, TheSessionsKeyGivesAProfilesModulesTheExtendedSession)
{
    EXPECT_TRUE(parsed("mock://ask?profile=x&sessions=1").sessions);
    EXPECT_FALSE(parsed("mock://ask?profile=x&sessions=0").sessions);
    EXPECT_FALSE(parsed("mock://ask?profile=x").sessions);
    EXPECT_EQ(refusal("mock://ask?sessions=1"),
              "sessions=1 gives a profile's modules the extended session: "
              "it needs profile=");
    EXPECT_EQ(refusal("mock://ask?profile=x&sessions=2"),
              "sessions: 0 or 1 (a profile's modules have the extended "
              "session too)");

    const auto reply = [](const std::vector<ecuio_sim_reply_t>& r) {
        return r.size() == 1 ? Bytes(r[0].data, r[0].data + r[0].length)
                             : Bytes{};
    };
    const Bytes entered = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    Modules m("sim-car-sessions", 3);
    {
        Unbound u("mock://ask?profile=sim-car-sessions");
        ASSERT_NE(u.sim, nullptr);
        EXPECT_EQ(reply(u.ask(0x701, {0x10, 0x03})), (Bytes{0x7F, 0x10, 0x12}))
            << "no session without the key";
    }
    Unbound u("mock://ask?profile=sim-car-sessions&sessions=1");
    ASSERT_NE(u.sim, nullptr);
    for (uint32_t i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(reply(u.ask(0x700 + i, {0x10, 0x03})), entered);
        EXPECT_EQ(reply(u.ask(0x700 + i, {0x3E, 0x00})), (Bytes{0x7E, 0x00}));
        EXPECT_EQ(reply(u.ask(0x700 + i, {0x27, 0x01})),
                  (Bytes{0x7F, 0x27, 0x11}))
            << "no security access";
        EXPECT_EQ(reply(u.ask(0x700 + i, {0x22, 0x10, static_cast<uint8_t>(i)}))
                      .size(),
                  4u)
            << "its DID still answers";
    }
    EXPECT_EQ(reply(u.ask(0x7E0, {0x10, 0x03})), entered)
        << "the engine ECU's own session";
}

// The aim is judged after the profile (its modules are known only then)
// and before the interface, on every platform: create's order
TEST(SimTest, AFaultsAimIsJudgedAfterTheProfileAndBeforeTheInterface)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create("mock://sim?profile=sim-car-none&faults=late:3@"
                               "7EA",
                               "nosuchcan9", &sim),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(
        ecuio_sim_create("mock://sim?faults=late:3@7EA", "nosuchcan9", &sim),
        ECUIO_ERR_BAD_URI)
        << "judged before NO_TRANSPORT off Linux and the interface on it";
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("aims at a module"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_EQ(sim, nullptr);
}
