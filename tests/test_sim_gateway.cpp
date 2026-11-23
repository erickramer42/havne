// File/layer: tests/test_sim_gateway.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SimGatewayTest (test_obd2)
//
// The simulated gateway's routing (ADR-0046's appendix "a gateway node",
// core/src/sim_gateway.h and sim_gateway_route.h): the classes, the
// route= grammar and its precedence, gateway= and behind= judged at
// create, a refusal as the gateway's own reply, a withheld request
// reaching no module, functional requests, the gateway's own services
// and the replies' bound, all through the unbound sim at the case's time.

#include "sim_gateway_support.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace sim_gateway_test;

namespace {

constexpr uint32_t kGw = 0x6F0;
constexpr uint32_t kGwReply = 0x6F8;
constexpr uint32_t kModule = 0x7E1;
constexpr uint32_t kModuleReply = 0x7E9;

const std::string kBase = "mock://gw?modules=2&gateway=6F0:6F8&behind=7E9";

const Bytes kD100 = {0x22, 0xD1, 0x00};
const Bytes kD100Answer = {0x62, 0xD1, 0x00, 0x82};

/* What the parsed URI's gateway does with service for the module on id */
ecuio_sim_route outcome(const ecuio_sim_config& c, uint32_t id, uint8_t service,
                        uint8_t* nrc = nullptr)
{
    uint8_t code = 0;
    const ecuio_sim_route r =
        ecuio_sim_gateway_outcome(c.gateway, id, service, &code);
    if (nrc != nullptr) {
        *nrc = code;
    }
    return r;
}

} // namespace

// Each service byte the plan lists is in its class; any other is other
TEST(SimGatewayTest, EachServiceHasItsClass)
{
    const std::vector<std::vector<uint8_t>> classes = {
        {0x01, 0x02, 0x03, 0x06, 0x07, 0x09, 0x0A, 0x19, 0x21, 0x22, 0x23,
         0x24},
        {0x10, 0x27, 0x28, 0x3E, 0x85},
        {0x04, 0x08, 0x14, 0x2C, 0x2E, 0x2F, 0x31, 0x3D},
        {0x11, 0x34, 0x35, 0x36, 0x37, 0x38}};
    int listed[256] = {};
    for (size_t c = 0; c < classes.size(); ++c) {
        for (const uint8_t sid : classes[c]) {
            EXPECT_EQ(ecuio_sim_gateway_class(sid), c) << std::hex << int{sid};
            listed[sid] = 1;
        }
    }
    for (int sid = 0; sid < 256; ++sid) {
        if (listed[sid] == 0) {
            EXPECT_EQ(ecuio_sim_gateway_class(static_cast<uint8_t>(sid)), 4)
                << std::hex << sid;
        }
    }
    EXPECT_STREQ(ecuio_sim_gateway_class_name(0), "reads");
    EXPECT_STREQ(ecuio_sim_gateway_class_name(4), "other");
}

// route= by class, by service byte and aimed at one module: the most
// specific entry wins, a later entry replaces its aim's earlier one, a
// second route= key adds, and anything not named forwards
TEST(SimGatewayTest, RouteTakesAClassAServiceAndAModule)
{
    const ecuio_sim_config none = parsed(kBase);
    for (int sid = 1; sid <= 0xBF; ++sid) {
        EXPECT_EQ(outcome(none, kModuleReply, static_cast<uint8_t>(sid)),
                  ecuio_sim_route::forward)
            << std::hex << sid;
    }

    const ecuio_sim_config c =
        parsed("mock://gw?profile=sim-gw-unused&gateway=6F0:6F8&behind=740,741"
               "&route=reads:refuse,write:silent,22:forward,19:refuse.31@740,"
               "reads:silent@741&route=prog:refuse.7F,reads:refuse.21");
    uint8_t nrc = 0;
    EXPECT_EQ(outcome(c, 0x742, 0x01, &nrc), ecuio_sim_route::refuse)
        << "the gateway's class";
    EXPECT_EQ(nrc, 0x21) << "the second key's reads replaced the first's";
    EXPECT_EQ(outcome(c, 0x742, 0x22), ecuio_sim_route::forward)
        << "a service for the gateway beats its class";
    EXPECT_EQ(outcome(c, 0x742, 0x2E), ecuio_sim_route::silent);
    EXPECT_EQ(outcome(c, 0x742, 0x36, &nrc), ecuio_sim_route::refuse);
    EXPECT_EQ(nrc, 0x7F);
    EXPECT_EQ(outcome(c, 0x742, 0x10), ecuio_sim_route::forward)
        << "a class not named forwards";
    EXPECT_EQ(outcome(c, 0x740, 0x19, &nrc), ecuio_sim_route::refuse)
        << "a service for the module beats everything";
    EXPECT_EQ(nrc, 0x31);
    EXPECT_EQ(outcome(c, 0x740, 0x22), ecuio_sim_route::forward)
        << "740 has no entry of its own for 22";
    EXPECT_EQ(outcome(c, 0x741, 0x22), ecuio_sim_route::silent)
        << "a class for the module beats a service for the gateway";
    EXPECT_EQ(outcome(c, 0x741, 0x19), ecuio_sim_route::silent)
        << "740's 19 isn't 741's";
    EXPECT_EQ(outcome(c, 0x741, 0x2E), ecuio_sim_route::silent);

    const ecuio_sim_config again =
        parsed(kBase + "&route=22:refuse@7E9,22:silent@7E9");
    EXPECT_EQ(again.gateway.entry_count, 1u) << "the same aim twice is one";
    EXPECT_EQ(outcome(again, kModuleReply, 0x22), ecuio_sim_route::silent);
}

// .<NRC> is two hex digits 10-FE but 78, on refuse alone
TEST(SimGatewayTest, ACodeIsTenToFeButNot78)
{
    for (int code = 0; code < 256; ++code) {
        char text[8];
        std::snprintf(text, sizeof text, "%02X", code);
        const std::string uri = kBase + "&route=22:refuse." + text;
        const bool taken = code >= 0x10 && code <= 0xFE && code != 0x78;
        EXPECT_EQ(refusal(uri).empty(), taken) << text;
        if (taken) {
            uint8_t nrc = 0;
            outcome(parsed(uri), kModuleReply, 0x22, &nrc);
            EXPECT_EQ(nrc, code);
        }
    }
    EXPECT_NE(refusal(kBase + "&route=22:refuse.78").find("faults=pending"),
              std::string::npos);
    for (const char* bad : {"22:silent.22", "22:forward.22", "22:refuse.2",
                            "22:refuse.222", "22:refuse.", "22:refuse.zz"}) {
        EXPECT_NE(
            refusal(kBase + "&route=" + bad).find("only refuse and gated take"),
            std::string::npos)
            << bad;
    }
}

TEST(SimGatewayTest, RouteRefusalsNameTheirCause)
{
    const char* const shapes[] = {
        "reading:refuse", "22:block", "00:refuse", "C0:refuse", "2:refuse",
        "222:refuse",     "22",       ":refuse",   "22:",       "22:locked"};
    for (const char* bad : shapes) {
        const std::string why = refusal(kBase + "&route=" + bad);
        EXPECT_NE(why.find("isn't <class or service>:<outcome>"),
                  std::string::npos)
            << bad << ": " << why;
    }
    EXPECT_EQ(refusal(kBase + "&route=22:refuse,"), "route: an empty entry");
    EXPECT_EQ(refusal(kBase + "&route=22:refuse,,19:silent"),
              "route: an empty entry");
    EXPECT_NE(refusal(kBase + "&route=22:refuse@").find("@ names a module"),
              std::string::npos);
    EXPECT_NE(refusal(kBase + "&route=22:refuse@800").find("@ names a module"),
              std::string::npos);
    EXPECT_EQ(refusal(kBase + "&route=22:refuse@7EA"),
              "route: @7EA names no module behind the gateway (behind=)");

    std::string many = kBase + "&route=";
    for (int sid = 1; sid <= 64; ++sid) {
        char item[16];
        std::snprintf(item, sizeof item, "%s%02X:refuse", sid == 1 ? "" : ",",
                      sid);
        many += item;
    }
    EXPECT_TRUE(refusal(many).empty()) << "64 entries";
    EXPECT_NE(refusal(many + ",41:refuse").find("more than 64 entries"),
              std::string::npos);
    EXPECT_TRUE(refusal(many + ",01:silent").empty())
        << "a repeat replaces, counting no more";
}

// gateway=, behind= and gwopt= read alone, and what needs what
TEST(SimGatewayTest, TheGatewaysKeysAndWhatEachNeeds)
{
    const ecuio_sim_config one = parsed("mock://gw?gateway=1");
    EXPECT_TRUE(one.gateway.given);
    EXPECT_TRUE(one.gateway.default_pair);
    EXPECT_EQ(one.gateway.request_id, 0x6F0u);
    EXPECT_EQ(one.gateway.reply_id, 0x6F8u);
    const ecuio_sim_config named = parsed("mock://gw?gateway=600:7f8");
    EXPECT_FALSE(named.gateway.default_pair);
    EXPECT_EQ(named.gateway.request_id, 0x600u);
    EXPECT_EQ(named.gateway.reply_id, 0x7F8u);

    for (const char* bad : {"2", "6F0", "6F0:", ":6F8", "800:6F8", "6F0:6F8:1",
                            "6F0;6F8", "1FFF:6F8", "", "0x6F0:6F8"}) {
        EXPECT_NE(refusal(std::string("mock://gw?gateway=") + bad)
                      .find("gateway: <request ID>:<reply ID>"),
                  std::string::npos)
            << bad;
    }
    for (const char* bad : {"7DF:6F8", "6F0:7DF", "6F0:6F0"}) {
        EXPECT_NE(refusal(std::string("mock://gw?gateway=") + bad)
                      .find("the request and reply IDs differ"),
                  std::string::npos)
            << bad;
    }

    EXPECT_EQ(parsed(kBase + "&behind=7E9").gateway.behind_count, 1u)
        << "a later behind= replaces the earlier";
    EXPECT_EQ(refusal(kBase + ",7E9"), "behind: names a module twice");
    EXPECT_NE(refusal("mock://gw?gateway=1&behind=7E9,").find("behind: the "),
              std::string::npos);
    EXPECT_NE(refusal("mock://gw?gateway=1&behind=800").find("behind: the "),
              std::string::npos);
    EXPECT_NE(refusal("mock://gw?gateway=1&behind=0").find("behind: the "),
              std::string::npos);
    std::string full = "mock://gw?gateway=1&behind=";
    for (int i = 0; i < 33; ++i) {
        char id[8];
        std::snprintf(id, sizeof id, "%s%X", i == 0 ? "" : ",", 0x740 + i);
        full += id;
        if (i == 31) {
            EXPECT_TRUE(refusal(full).empty()) << "32 modules";
        }
    }
    EXPECT_EQ(refusal(full), "behind: more than 32 modules");

    const ecuio_sim_config both = parsed(kBase + "&gwopt=aloud,unsuppressed");
    EXPECT_TRUE(both.gateway.aloud);
    EXPECT_TRUE(both.gateway.unsuppressed);
    EXPECT_NE(refusal(kBase + "&gwopt=loud").find("gwopt: keep-any"),
              std::string::npos);
    EXPECT_EQ(refusal(kBase + "&gwopt=unsuppressed"),
              "gwopt: unsuppressed says ISO 14229-1's suppressed codes aloud: "
              "it needs aloud");

    for (const char* alone :
         {"mock://gw?modules=2&behind=7E9", "mock://gw?route=22:refuse",
          "mock://gw?gwopt=aloud"}) {
        EXPECT_EQ(refusal(alone),
                  "behind, route, sa, sa_delay and gwopt are a gateway's: "
                  "they need gateway=")
            << alone;
    }
    EXPECT_TRUE(refusal("mock://gw?gateway=6F0:6F8").empty())
        << "a gateway with nothing behind it";
    EXPECT_NE(refusal("mock://gw?bogus=1")
                  .find("values, modules, sessions, profile, gateway, "
                        "behind, route, sa, sa_delay, gwopt"),
              std::string::npos);

#ifdef _WIN32
    const std::string replay = "replay://C:/rec/frames.csv";
#else
    const std::string replay = "replay:///rec/frames.csv";
#endif
    for (const char* key :
         {"gateway=1", "behind=7E9", "route=22:refuse", "gwopt=aloud"}) {
        EXPECT_EQ(refusal(replay + "?" + key),
                  "a gateway is mock://'s: a replay ECU answers as the "
                  "engine ECU alone, with no module to put behind one")
            << key;
    }
}

// behind= is judged against the modules once the profile is known, and
// the gateway's IDs against every node's
TEST(SimGatewayTest, BehindAndTheIdsAreJudgedAgainstTheModules)
{
    std::string why;
    EXPECT_EQ(create_status("mock://gw?modules=2&gateway=1&behind=7E8", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "behind: the engine ECU (7E8) can't be behind the gateway; "
                   "the modules answer on 7E9");
    EXPECT_EQ(create_status("mock://gw?modules=2&gateway=1&behind=7EA", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "behind: 7EA is no module of the simulated car's; the "
                   "modules answer on 7E9");
    EXPECT_EQ(create_status("mock://gw?gateway=1&behind=7E9", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "behind: 7E9 is no module of the simulated car's; the "
                   "modules answer on none (modules=2 or profile= adds them)");
    {
        const Profile p("sim-gw-ten", modules(10));
        EXPECT_EQ(create_status("mock://gw?profile=sim-gw-ten&gateway=1&"
                                "behind=760",
                                &why),
                  ECUIO_ERR_BAD_URI);
        EXPECT_EQ(why, "behind: 760 is no module of the simulated car's; the "
                       "modules answer on 740, 741, 742, 743, 744, 745, 746, "
                       "747 and 2 more");
        EXPECT_EQ(create_status("mock://gw?profile=sim-gw-ten&gateway=1&"
                                "behind=749,740",
                                &why),
                  ECUIO_OK)
            << why;
    }

    EXPECT_EQ(create_status("mock://gw?modules=2&gateway=7E1:6F8", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "gateway: 7E1:6F8 meets a module's ID 7E1");
    EXPECT_EQ(create_status("mock://gw?gateway=6F0:7E8", &why),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(why, "gateway: 6F0:7E8 meets a module's ID 7E8");
    {
        const Profile p("sim-gw-clash", {{0x6F8, 0x6F7}});
        EXPECT_EQ(
            create_status("mock://gw?profile=sim-gw-clash&gateway=1", &why),
            ECUIO_ERR_BAD_URI);
        EXPECT_EQ(why, "gateway=1's 6F0:6F8 meets a module's ID 6F8: give the "
                       "gateway's IDs (gateway=<request ID>:<reply ID>)");
        EXPECT_EQ(create_status("mock://gw?profile=sim-gw-clash&gateway=600:"
                                "608&behind=6F8",
                                &why),
                  ECUIO_OK)
            << why;
    }
    EXPECT_EQ(create_status("mock://gw?gateway=1&faults=silent:2@6F8", &why),
              ECUIO_OK)
        << "the gateway's reply ID takes an aim: " << why;
    EXPECT_EQ(create_status("mock://gw?faults=silent:2@6F8", &why),
              ECUIO_ERR_BAD_URI)
        << "with no gateway it doesn't";
}

// A refusal goes on the module's reply ID as the gateway's reply (22 by
// default, or the entry's code); silent sends nothing; forward is the
// module's own answer
TEST(SimGatewayTest, ARefusalIsTheGatewaysOnTheModulesId)
{
    Sim s(kBase + "&route=22:refuse,19:refuse.11@7E9,09:silent");
    const std::vector<Reply> r = s.ask(kModule, kD100);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].id, kModuleReply);
    EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_GATEWAY);
    EXPECT_EQ(r[0].data, (Bytes{0x7F, 0x22, 0x22}));
    EXPECT_EQ(s.one(kModule, {0x19, 0x02, 0xFF}), (Bytes{0x7F, 0x19, 0x11}));
    EXPECT_TRUE(s.ask(kModule, {0x09, 0x02}).empty());
    const std::vector<Reply> forwarded = s.ask(kModule, {0x3E, 0x00});
    ASSERT_EQ(forwarded.size(), 1u);
    EXPECT_EQ(forwarded[0].flags, 0u) << "the module's own";
    EXPECT_EQ(forwarded[0].data, (Bytes{0x7E, 0x00}));
    EXPECT_EQ(s.one(0x7E0, {0x09, 0x02}).at(0), 0x49)
        << "the engine ECU isn't behind it";

    const ecuio_sim_stats_t st = s.stats();
    EXPECT_EQ(st.requests, 5u);
    EXPECT_EQ(st.replies, 4u);
    EXPECT_EQ(st.refused, 2u);

    Sim plain("mock://gw?modules=2");
    EXPECT_EQ(plain.one(kModule, kD100), kD100Answer)
        << "the module answers it without the gateway";
}

// A withheld request moves none of the module's fault counts, draws or
// busy time: its next forwarded reply comes as if it had never been sent
TEST(SimGatewayTest, AWithheldRequestNeverReachesTheModule)
{
    const std::string keys =
        "&route=19:refuse@7E9,23:silent&faults=late:2@7E9&delay=2000:"
        "40000@7E9";
    Sim alone(kBase + keys);
    Sim between(kBase + keys);
    for (int lap = 0; lap < 6; ++lap) {
        alone.now = between.now = lap * 1000000;
        const std::vector<Reply> a = alone.ask(kModule, kD100);
        EXPECT_EQ(between.one(kModule, {0x19, 0x02, 0xFF}),
                  (Bytes{0x7F, 0x19, 0x22}));
        EXPECT_TRUE(between.ask(kModule, {0x23, 0x00, 0x00}).empty());
        const std::vector<Reply> b = between.ask(kModule, kD100);
        ASSERT_EQ(a.size(), 1u);
        ASSERT_EQ(b.size(), 1u);
        EXPECT_EQ(b[0].delay_us, a[0].delay_us) << "lap " << lap;
        EXPECT_EQ(b[0].flags, a[0].flags) << "lap " << lap;
        EXPECT_EQ((a[0].flags & ECUIO_SIM_REPLY_LATE) != 0, lap % 2 == 1);
    }

    Sim busy(kBase + "&route=19:refuse&delay=100000:100000@7E9&busy=21@7E9");
    EXPECT_EQ(busy.one(kModule, kD100), kD100Answer);
    busy.now = 50000;
    EXPECT_EQ(busy.one(kModule, {0x19, 0x02, 0xFF}), (Bytes{0x7F, 0x19, 0x22}))
        << "the gateway answers ahead of the module's busy time";
    EXPECT_EQ(busy.one(kModule, kD100), (Bytes{0x7F, 0x22, 0x21}))
        << "the module still busy with its own reply";
    busy.now = 150000;
    EXPECT_EQ(busy.one(kModule, {0x19, 0x02, 0xFF}), (Bytes{0x7F, 0x19, 0x22}));
    EXPECT_EQ(busy.one(kModule, kD100), kD100Answer)
        << "the withheld request started no busy time of the module's";
}

// The gateway's faults and timing are its own, aimed at its reply ID: they
// break its refusals and leave the module's answers alone
TEST(SimGatewayTest, TheGatewaysFaultsAndTimingAreItsOwn)
{
    Sim s(kBase + "&route=19:refuse&faults=silent:2@6F8&delay=7000:7000@6F8");
    for (int i = 1; i <= 4; ++i) {
        const std::vector<Reply> r = s.ask(kModule, {0x19, 0x02, 0xFF});
        if (i % 2 == 0) {
            EXPECT_TRUE(r.empty()) << "the gateway's second " << i;
        } else {
            ASSERT_EQ(r.size(), 1u);
            EXPECT_EQ(r[0].delay_us, 7000);
        }
        const std::vector<Reply> m = s.ask(kModule, kD100);
        ASSERT_EQ(m.size(), 1u) << "the module's own count, untouched";
        EXPECT_EQ(m[0].delay_us, 0);
    }
    Sim pending(kBase + "&route=19:refuse&faults=pending:1@6F8");
    const std::vector<Reply> r = pending.ask(kModule, {0x19, 0x02, 0xFF});
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].id, kModuleReply);
    EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_PENDING | ECUIO_SIM_REPLY_GATEWAY);
    EXPECT_EQ(r[0].data, (Bytes{0x7F, 0x19, 0x78}));
    EXPECT_EQ(r[1].data, (Bytes{0x7F, 0x19, 0x22}));
    EXPECT_EQ(r[1].flags, ECUIO_SIM_REPLY_GATEWAY);
}

// On 0x7DF a module whose entry forwards answers and one withheld is
// silent; gwopt=aloud refuses for each module withheld, keeping ISO
// 14229-1's suppressed codes and the silent outcome silent, and
// gwopt=unsuppressed says the codes too
TEST(SimGatewayTest, AFunctionalRequestReachesOnlyWhatForwards)
{
    Sim quiet(kBase + "&route=reads:refuse,22:forward,23:silent");
    const std::vector<Reply> forwarded = quiet.ask(0x7DF, kD100);
    ASSERT_EQ(forwarded.size(), 1u) << "the engine ECU's 31 is suppressed";
    EXPECT_EQ(forwarded[0].id, kModuleReply);
    EXPECT_EQ(forwarded[0].data, kD100Answer);
    const std::vector<Reply> withheld = quiet.ask(0x7DF, {0x09, 0x02});
    ASSERT_EQ(withheld.size(), 1u);
    EXPECT_EQ(withheld[0].id, 0x7E8u) << "the engine ECU's VIN alone";

    const Profile p("sim-gw-three", modules(3));
    const std::string three =
        "mock://gw?profile=sim-gw-three&gateway=6F0:6F8&behind=740,741,742";
    Sim aloud(three +
              "&route=22:refuse,23:silent,19:refuse.11@741&gwopt=aloud");
    const std::vector<Reply> each = aloud.ask(0x7DF, {0x22, 0x10, 0x00});
    ASSERT_EQ(each.size(), 3u);
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(each[i].id, 0x740u + i);
        EXPECT_EQ(each[i].flags, ECUIO_SIM_REPLY_GATEWAY);
        EXPECT_EQ(each[i].data, (Bytes{0x7F, 0x22, 0x22}));
    }
    EXPECT_TRUE(aloud.ask(0x7DF, {0x23, 0x00, 0x00}).empty())
        << "silent stays silent";
    EXPECT_TRUE(aloud.ask(0x7DF, {0x19, 0x02, 0xFF}).empty())
        << "741's 11 suppressed; the others forward 19 and refuse it 11";

    for (const uint8_t code : Bytes{0x11, 0x12, 0x31, 0x7E, 0x7F, 0x21, 0x33}) {
        char text[4];
        std::snprintf(text, sizeof text, "%02X", code);
        const bool suppressed = code != 0x21 && code != 0x33;
        Sim kept(three + "&route=22:refuse." + text + "&gwopt=aloud");
        EXPECT_EQ(kept.ask(0x7DF, {0x22, 0x10, 0x00}).size(),
                  suppressed ? 0u : 3u)
            << text;
        Sim said(three + "&route=22:refuse." + text +
                 ",23:silent&gwopt=aloud,unsuppressed");
        const std::vector<Reply> r = said.ask(0x7DF, {0x22, 0x10, 0x00});
        ASSERT_EQ(r.size(), 3u) << text;
        EXPECT_EQ(r[0].data, (Bytes{0x7F, 0x22, code}));
        EXPECT_TRUE(said.ask(0x7DF, {0x23, 0x00, 0x00}).empty())
            << "silent stays silent unsuppressed too";
    }
    Sim physical(three + "&route=22:refuse.11");
    EXPECT_EQ(physical.one(0x700, {0x22, 0x10, 0x00}),
              (Bytes{0x7F, 0x22, 0x11}))
        << "a physical request's 11 is said";
}

// On its own request ID the gateway answers session control and tester
// present, refuses 27 with no security access and every other service
// with 11, and hears no functional request
TEST(SimGatewayTest, TheGatewayAnswersItsOwnServices)
{
    Sim s("mock://gw?gateway=6F0:6F8");
    const std::vector<Reply> entered = s.ask(kGw, {0x10, 0x03});
    ASSERT_EQ(entered.size(), 1u);
    EXPECT_EQ(entered[0].id, kGwReply);
    EXPECT_EQ(entered[0].flags, 0u);
    EXPECT_EQ(entered[0].data, (Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_EQ(s.one(kGw, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_TRUE(s.ask(kGw, {0x3E, 0x80}).empty());
    EXPECT_EQ(s.one(kGw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x11}));
    EXPECT_EQ(s.one(kGw, {0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x11}));
    EXPECT_EQ(s.one(kGw, {0x10, 0x02}), (Bytes{0x7F, 0x10, 0x12}));
    EXPECT_EQ(s.one(kGw, {0x10, 0x01}),
              (Bytes{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    const uint64_t asked = s.stats().requests;
    const std::vector<Reply> functional = s.ask(0x7DF, {0x01, 0x00});
    ASSERT_EQ(functional.size(), 1u);
    EXPECT_EQ(functional[0].id, 0x7E8u);
    EXPECT_EQ(s.stats().requests, asked + 1) << "the engine ECU alone heard it";

    Sim one("mock://gw?gateway=1");
    EXPECT_EQ(one.one(0x6F0, {0x3E, 0x00}), (Bytes{0x7E, 0x00}))
        << "gateway=1 answers as 6F8";
    EXPECT_EQ(one.ask(0x6F0, {0x3E, 0x00}).at(0).id, 0x6F8u);
}

// 32 modules behind an aloud gateway, each refusal behind a 7F xx 78, and
// the engine ECU's pending reply: ECUIO_SIM_MAX_REPLIES, every one kept
TEST(SimGatewayTest, ThirtyTwoModulesBehindAnAloudGatewayFitTheReplies)
{
    const Profile p("sim-gw-full", modules(32));
    std::string uri = "mock://gw?profile=sim-gw-full&gateway=6F0:6F8&behind=";
    for (uint32_t i = 0; i < 32; ++i) {
        char id[8];
        std::snprintf(id, sizeof id, "%s%X", i == 0 ? "" : ",", 0x740 + i);
        uri += id;
    }
    Sim s(uri + "&route=reads:refuse&gwopt=aloud&faults=pending:1");
    const std::vector<Reply> r = s.ask(0x7DF, {0x01, 0x0C});
    ASSERT_EQ(r.size(), ECUIO_SIM_MAX_REPLIES);
    EXPECT_EQ(r[0].id, 0x7E8u);
    EXPECT_EQ(r[1].id, 0x7E8u);
    for (uint32_t i = 0; i < 32; ++i) {
        EXPECT_EQ(r[2 + 2 * i].id, 0x740u + i);
        EXPECT_EQ(r[2 + 2 * i].data, (Bytes{0x7F, 0x01, 0x78}));
        EXPECT_EQ(r[3 + 2 * i].data, (Bytes{0x7F, 0x01, 0x22}));
        EXPECT_NE(r[3 + 2 * i].flags & ECUIO_SIM_REPLY_GATEWAY, 0u);
    }
}
