// File/layer: tests/test_obd2_sim_ask.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* ---------- The unbound sim (ADR-0055) ---------- */

namespace {

/* An unbound sim, destroyed at the end */
struct Unbound {
    ecuio_sim_t* sim = nullptr;
    explicit Unbound(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim, &s), ECUIO_OK);
        return s;
    }
};

/* Each reply to req on id at t (the caller's microseconds) */
std::vector<ecuio_sim_reply_t> sim_ask(ecuio_sim_t* sim, uint32_t id,
                                       std::initializer_list<uint8_t> req,
                                       int64_t t = 0)
{
    const std::vector<uint8_t> q(req);
    std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
    uint32_t n = 0;
    EXPECT_EQ(ecuio_sim_ask(sim, id, q.data(), static_cast<uint32_t>(q.size()),
                            t, out.data(), static_cast<uint32_t>(out.size()),
                            &n),
              ECUIO_OK)
        << ecuio_last_error_detail();
    out.resize(n);
    return out;
}

Bytes reply_bytes(const ecuio_sim_reply_t& r)
{
    return Bytes(r.data, r.data + r.length);
}

/* The engine speed in rpm from a 41 0C reply */
double rpm_of(const ecuio_sim_reply_t& r)
{
    EXPECT_EQ(r.length, 4u);
    return (r.data[2] * 256 + r.data[3]) / 4.0;
}

} // namespace

// The unbound sim is the same car as the bus's: its engine ECU answers
// its own request ID and the functional one as sim_ecu.h does, and an ID
// no ECU hears gets nothing and isn't counted
TEST(SimAskTest, AnUnboundSimAnswersAsTheEngineEcu)
{
    Unbound u("mock://ask?values=canned");
    ASSERT_NE(u.sim, nullptr);
    ecuio_sim_ecu canned;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://ask?values=canned", &canned.config,
                                  why, sizeof why),
              ecuio_sim_uri_result::ok);
    for (const Bytes& q :
         {Bytes{0x01, 0x00}, Bytes{0x01, 0x0C}, Bytes{0x09, 0x02}, Bytes{0x03},
          Bytes{0x22, 0xF1, 0x90}}) {
        uint8_t want[ECUIO_SIM_REPLY_MAX];
        const uint32_t want_len = ecuio_sim_answer(
            canned, q.data(), static_cast<uint32_t>(q.size()), false, want);
        std::vector<ecuio_sim_reply_t> got(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        ASSERT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q.data(),
                                static_cast<uint32_t>(q.size()), 0, got.data(),
                                ECUIO_SIM_MAX_REPLIES, &n),
                  ECUIO_OK);
        ASSERT_EQ(n, 1u) << std::hex << int{q[0]};
        EXPECT_EQ(got[0].reply_id, 0x7E8u);
        EXPECT_EQ(got[0].flags, 0u);
        EXPECT_EQ(got[0].delay_us, 0);
        EXPECT_EQ(reply_bytes(got[0]), Bytes(want, want + want_len));
        EXPECT_EQ(got[0].fc_us, 0u);
        EXPECT_EQ(got[0].cf_us, 0u);
        for (const uint32_t word : got[0].reserved) {
            EXPECT_EQ(word, 0u);
        }
    }
    const std::vector<ecuio_sim_reply_t> functional =
        sim_ask(u.sim, 0x7DF, {0x01, 0x00});
    ASSERT_EQ(functional.size(), 1u);
    EXPECT_EQ(functional[0].reply_id, 0x7E8u);
    EXPECT_TRUE(sim_ask(u.sim, 0x7DF, {0x22, 0xF1, 0x90}).empty())
        << "a functional request's refusal is suppressed";

    const uint64_t before = u.stats().requests;
    EXPECT_TRUE(sim_ask(u.sim, 0x7E5, {0x01, 0x00}).empty());
    EXPECT_EQ(u.stats().requests, before) << "no ECU heard it";

    ASSERT_EQ(reply_bytes(sim_ask(u.sim, 0x7E0, {0x04})[0]), (Bytes{0x44}));
    const ecuio_sim_stats_t s = u.stats();
    EXPECT_EQ(s.clears, 1u);
    EXPECT_EQ(s.requests, 8u);
    EXPECT_EQ(s.replies, 7u);
    EXPECT_EQ(s.refused, 1u) << "22 F190 on the engine ECU";
    EXPECT_EQ(s.flags, 0u);
}

TEST(SimAskTest, TheContracts)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound(nullptr, 0, &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_unbound("mock://a", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_unbound("mock://a", 1, &sim),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sim, nullptr);
    EXPECT_EQ(ecuio_sim_create_unbound("can://a", 0, &sim), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(
        ecuio_sim_create_unbound("mock://a?profile=sim-ask-none", 0, &sim),
        ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(ecuio_sim_create_unbound("replay://relative/frames.csv", 0, &sim),
              ECUIO_ERR_BAD_URI);
    const std::string missing =
        std::string(ECUIO_REPLAY_FIXTURES) + "/no-such-recording/frames.csv";
    EXPECT_EQ(
        ecuio_sim_create_unbound(("replay://" + missing).c_str(), 0, &sim),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()).find(missing),
              std::string::npos)
        << "never the path";

    Unbound u("mock://ask");
    ASSERT_NE(u.sim, nullptr);
    const uint8_t q[2] = {0x01, 0x00};
    ecuio_sim_reply_t r[2];
    uint32_t n = 99;
    EXPECT_EQ(ecuio_sim_ask(nullptr, 0x7E0, q, 2, 0, r, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, nullptr, 2, 0, r, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, r, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, nullptr, 1, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 0, 0, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 4096, 0, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, -2, r, 2, &n),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(n, 99u) << "a refusal leaves the count";
    EXPECT_EQ(u.stats().requests, 0u);

    /* capacity 0 counts the replies */
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7DF, q, 2, 0, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7DF, q, 2, ECUIO_SIM_NOW, r, 2, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 1u);
}

// The ECU's time is the caller's: the drive cycle's key-on holds the
// engine stopped for its first 15 s, it runs by 20 s, and a time before
// the last is taken as the last
TEST(SimAskTest, TheClockIsTheCallersAndNeverGoesBack)
{
    Unbound u("mock://ask");
    ASSERT_NE(u.sim, nullptr);
    std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 0);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(rpm_of(r[0]), 0.0) << "key on, the engine stopped";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 10 * kSecond);
    EXPECT_EQ(rpm_of(r[0]), 0.0);
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 20 * kSecond);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_GT(rpm_of(r[0]), 500.0) << "the engine started at 15 s";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 5 * kSecond);
    EXPECT_GT(rpm_of(r[0]), 500.0) << "the clock never goes back";

    /* A step past an hour moves the time and steps an hour of the
     * cycle at most: it answers at once, as the drive cycle reads an
     * hour of ticks on from 20 s */
    const auto before = std::chrono::steady_clock::now();
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 1000LL * 3600 * kSecond);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - before)
            .count();
    EXPECT_LT(took, 2.0) << "the step ran the whole thousand hours";
    ASSERT_EQ(r.size(), 1u);
    ecuio_mock::DriveCycle car;
    for (int i = 0; i < 20 * 20 + 3600 * 20; ++i) {
        car.step();
    }
    EXPECT_NEAR(rpm_of(r[0]), car.rpm, 0.25);
}

// A request is answered once, whatever the capacity: a capacity of 0
// counts the replies and is still a request, so the next one meets the
// second request's fault
TEST(SimAskTest, ACountOnlyCallIsARequestOfItsOwn)
{
    Unbound u("mock://ask?faults=silent:2");
    ASSERT_NE(u.sim, nullptr);
    const uint8_t q[] = {0x01, 0x0C};
    uint32_t n = 99;
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, nullptr, 0, &n), ECUIO_OK);
    EXPECT_EQ(n, 1u);
    std::vector<ecuio_sim_reply_t> r(10);
    n = 99;
    EXPECT_EQ(ecuio_sim_ask(u.sim, 0x7E0, q, 2, 0, r.data(), 10, &n), ECUIO_OK);
    EXPECT_EQ(n, 0u) << "the second request is the silent one";
    const ecuio_sim_stats_t s = u.stats();
    EXPECT_EQ(s.requests, 2u);
    EXPECT_EQ(s.silent, 1u);
    EXPECT_EQ(s.replies, 1u);
}

// Each fault as the reply's start and a flag: pending's 7F <service> 78
// at once and the reply 300 ms on, late's 130 ms, the two segmented
// breaks flagged, silent none; each counted as on a bus
TEST(SimAskTest, FaultsAreTheRepliesTimesAndFlags)
{
    {
        Unbound u("mock://ask?faults=pending:1");
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 2u);
        EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_PENDING);
        EXPECT_EQ(r[0].delay_us, 0);
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x7F, 0x01, 0x78}));
        EXPECT_EQ(r[1].flags, 0u);
        EXPECT_EQ(r[1].delay_us, 300000);
        EXPECT_EQ(r[1].data[0], 0x41);
        EXPECT_EQ(u.stats().pending, 1u);
        EXPECT_EQ(u.stats().replies, 1u) << "the 78 isn't the reply";
        EXPECT_EQ(sim_ask(u.sim, 0x7E0, {0x01, 0x00}).size(), 1u)
            << "the bitmaps are never broken";
    }
    {
        Unbound u("mock://ask?faults=late:1");
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].flags, ECUIO_SIM_REPLY_LATE);
        EXPECT_EQ(r[0].delay_us, 130000);
        EXPECT_EQ(u.stats().late, 1u);
    }
    {
        Unbound u("mock://ask?faults=silent:1");
        EXPECT_TRUE(sim_ask(u.sim, 0x7E0, {0x01, 0x0C}).empty());
        EXPECT_EQ(u.stats().silent, 1u);
        EXPECT_EQ(u.stats().replies, 0u);
    }
    for (const char* uri :
         {"mock://ask?faults=bad_sequence:1", "mock://ask?faults=stalled:1"}) {
        Unbound u(uri);
        const bool bad = std::strstr(uri, "bad_sequence") != nullptr;
        std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].flags, 0u) << uri << ": a Single Frame can't break";
        r = sim_ask(u.sim, 0x7E0, {0x09, 0x02});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_GT(r[0].length, 7u);
        EXPECT_EQ(r[0].flags,
                  bad ? ECUIO_SIM_REPLY_BAD_SEQUENCE : ECUIO_SIM_REPLY_STALLED)
            << uri;
        const ecuio_sim_stats_t s = u.stats();
        EXPECT_EQ(bad ? s.bad_sequence : s.stalled, 1u) << uri;
        EXPECT_EQ(s.replies, 1u) << uri << ": a broken reply isn't whole";
    }
}

// modules=2's module answers its own IDs and, functionally, only what it
// has; a profile's DIDs answer the values they were given
TEST(SimAskTest, ModulesAndAProfilesDids)
{
    {
        Unbound u("mock://ask?modules=2");
        std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E1, {0x22, 0xD1, 0x01});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].reply_id, 0x7E9u);
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x62, 0xD1, 0x01, 0x03}));
        r = sim_ask(u.sim, 0x7DF, {0x22, 0xD1, 0x01});
        ASSERT_EQ(r.size(), 1u) << "the engine ECU's refusal is suppressed";
        EXPECT_EQ(r[0].reply_id, 0x7E9u);
        r = sim_ask(u.sim, 0x7DF, {0x01, 0x00});
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].reply_id, 0x7E8u);
    }
    /* The registry keeps the pointers: static storage */
    static const ecuio_signal_def_t def =
        did_def(7, 0x7E8, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                16, 0.5, -10.0, "Ask field");
    static ecuio_signal_set_t set = set_of({});
    set.count = 1;
    set.defs = &def;
    ASSERT_EQ(ecuio_profile_register("sim-ask-dids", &set, nullptr), ECUIO_OK);
    {
        Unbound u("mock://ask?profile=sim-ask-dids");
        ASSERT_NE(u.sim, nullptr);
        ASSERT_EQ(ecuio_sim_set_value(u.sim, 7, 90.5), ECUIO_OK);
        const std::vector<ecuio_sim_reply_t> r =
            sim_ask(u.sim, 0x7E0, {0x22, 0x12, 0x34});
        ASSERT_EQ(r.size(), 1u);
        /* (90.5 + 10) / 0.5 = 201 */
        EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x62, 0x12, 0x34, 0x00, 0xC9}));
    }
    EXPECT_EQ(ecuio_profile_unregister("sim-ask-dids"), ECUIO_OK);
}

// ADR-0065: a set of as many modules as a set holds is as many nodes, each
// answering its own request ID with its own value, and a functional
// request every one of them answers fits the replies a caller sizes for
TEST(SimAskTest, ASimAnswersAsEachOfASetsModules)
{
    /* The registry keeps the pointers: static storage */
    static std::vector<ecuio_signal_def_t> defs;
    static std::vector<ecuio_channel_decl_t> channels;
    defs.clear();
    channels.clear();
    for (uint32_t m = 0; m < ECUIO_MAX_CHANNELS; ++m) {
        ecuio_channel_decl_t c{};
        c.rx_id = 0x740u + m;
        c.tx_id = 0x700u + m;
        channels.push_back(c);
        char own[32];
        char shared[32];
        std::snprintf(own, sizeof own, "Own %u", m);
        std::snprintf(shared, sizeof shared, "Shared %u", m);
        defs.push_back(did_def(static_cast<uint16_t>(100 + m), c.rx_id,
                               static_cast<uint16_t>(0x2000 + m),
                               ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                               1.0, 0.0, own));
        defs.push_back(did_def(static_cast<uint16_t>(200 + m), c.rx_id, 0x1000,
                               ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                               1.0, 0.0, shared));
    }
    static ecuio_signal_set_t set;
    set = set_of(defs, channels);
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("sim-ask-modules", &set, &why), ECUIO_OK)
        << why.code << " at " << why.index;
    {
        Unbound u("mock://ask?profile=sim-ask-modules");
        ASSERT_NE(u.sim, nullptr);
        for (uint32_t m = 0; m < ECUIO_MAX_CHANNELS; ++m) {
            ASSERT_EQ(ecuio_sim_set_value(u.sim, static_cast<uint16_t>(100 + m),
                                          m + 1.0),
                      ECUIO_OK)
                << m;
        }
        for (uint32_t m = 0; m < ECUIO_MAX_CHANNELS; ++m) {
            SCOPED_TRACE("module " + std::to_string(m));
            const uint8_t lo = static_cast<uint8_t>(m);
            std::vector<ecuio_sim_reply_t> r =
                sim_ask(u.sim, 0x700u + m, {0x22, 0x20, lo});
            ASSERT_EQ(r.size(), 1u);
            EXPECT_EQ(r[0].reply_id, 0x740u + m);
            EXPECT_EQ(reply_bytes(r[0]),
                      (Bytes{0x62, 0x20, lo, static_cast<uint8_t>(m + 1)}));
            /* Another module's DID is refused by the one asked */
            const uint8_t other =
                static_cast<uint8_t>((m + 1) % ECUIO_MAX_CHANNELS);
            r = sim_ask(u.sim, 0x700u + m, {0x22, 0x20, other});
            ASSERT_EQ(r.size(), 1u);
            EXPECT_EQ(r[0].reply_id, 0x740u + m);
            EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x7F, 0x22, 0x31}));
        }
        const std::vector<ecuio_sim_reply_t> all =
            sim_ask(u.sim, 0x7DF, {0x22, 0x10, 0x00});
        ASSERT_EQ(all.size(), size_t{ECUIO_MAX_CHANNELS})
            << "the engine ECU's refusal is suppressed";
        std::set<uint32_t> ids;
        for (const ecuio_sim_reply_t& r : all) {
            ids.insert(r.reply_id);
            EXPECT_EQ(reply_bytes(r), (Bytes{0x62, 0x10, 0x00, 0x00}));
        }
        EXPECT_EQ(ids.size(), size_t{ECUIO_MAX_CHANNELS});
        EXPECT_EQ(*ids.begin(), 0x740u);
        EXPECT_EQ(*ids.rbegin(), 0x740u + ECUIO_MAX_CHANNELS - 1);
    }
    {
        /* Every reply put off behind a 7F 22 78: two entries a module */
        Unbound u("mock://ask?profile=sim-ask-modules&faults=pending:1");
        ASSERT_NE(u.sim, nullptr);
        const std::vector<ecuio_sim_reply_t> all =
            sim_ask(u.sim, 0x7DF, {0x22, 0x10, 0x00});
        EXPECT_EQ(all.size(), size_t{2 * ECUIO_MAX_CHANNELS});
        EXPECT_LE(all.size(), size_t{ECUIO_SIM_MAX_REPLIES});
        size_t pending = 0;
        for (const ecuio_sim_reply_t& r : all) {
            pending += (r.flags & ECUIO_SIM_REPLY_PENDING) != 0;
        }
        EXPECT_EQ(pending, size_t{ECUIO_MAX_CHANNELS});
    }
    EXPECT_EQ(ecuio_profile_unregister("sim-ask-modules"), ECUIO_OK);
}

// A recording answers at the caller's time: the fixture's engine start
// moves the engine speed from its first row to its last
TEST(SimAskTest, ARecordingAnswersAtTheCallersTime)
{
    const std::string uri = std::string("replay://") + ECUIO_REPLAY_FIXTURES +
                            "/2026-10-25-engine-start";
    Unbound u(uri.c_str());
    ASSERT_NE(u.sim, nullptr);
    std::vector<ecuio_sim_reply_t> r = sim_ask(u.sim, 0x7E0, {0x01, 0x00}, 0);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x41, 0x00, 0xFE, 0x7F, 0xA8, 0x13}))
        << "the car's own bitmap";
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 0);
    ASSERT_EQ(r.size(), 1u);
    const Bytes first = reply_bytes(r[0]);
    r = sim_ask(u.sim, 0x7E0, {0x01, 0x0C}, 600 * kSecond);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(reply_bytes(r[0]), first) << "its last replies hold";
}

// Through the unbound sim's nodes: the engine node enters, answers D200
// and leaves by S3 on the caller's clock; module 2 has no session
TEST(SimAskTest, TheEngineNodeHasAnExtendedSessionAndTheModuleHasNone)
{
    Unbound u("mock://ask?values=canned&modules=2");
    ASSERT_NE(u.sim, nullptr);
    const Bytes refused{0x7F, 0x22, 0x31};
    const Bytes entered{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    int64_t t = 0;
    const auto only = [&](uint32_t id, std::initializer_list<uint8_t> req) {
        const auto r = sim_ask(u.sim, id, req, t);
        return r.empty() ? Bytes{} : reply_bytes(r[0]);
    };
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x00}), refused);
    EXPECT_EQ(only(0x7E0, {0x10, 0x03}), entered);
    t += 100000;
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x00}),
              (Bytes{0x62, 0xD2, 0x00, 0x1A, 0x2C}));
    EXPECT_EQ(only(0x7E0, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_TRUE(only(0x7E0, {0x3E, 0x80}).empty());
    t += ECUIO_SIM_S3_US - ECUIO_SIM_TICK_US;
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x01}),
              (Bytes{0x62, 0xD2, 0x01, 0x00, 0x64}))
        << "heard 3E 80 just inside S3";
    t += ECUIO_SIM_S3_US + ECUIO_SIM_TICK_US;
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x01}), refused)
        << "S3 of silence returns it to the default session";
    EXPECT_EQ(only(0x7E0, {0x10, 0x03}), entered);
    EXPECT_EQ(only(0x7E0, {0x10, 0x01}),
              (Bytes{0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}));
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x00}), refused);
    EXPECT_EQ(only(0x7E0, {0x10, 0x05}), (Bytes{0x7F, 0x10, 0x12}));

    EXPECT_EQ(only(0x7E1, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x12}));
    EXPECT_EQ(only(0x7E1, {0x22, 0xD1, 0x00}), (Bytes{0x62, 0xD1, 0x00, 0x82}));
    EXPECT_EQ(only(0x7E1, {0x22, 0xD2, 0x00}), refused)
        << "D200 is the engine ECU's, and only in its session";
    EXPECT_EQ(only(0x7E1, {0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_TRUE(sim_ask(u.sim, 0x7DF, {0x10, 0x03}, t).empty());
    EXPECT_EQ(only(0x7E0, {0x22, 0xD2, 0x00}), refused)
        << "the functional ID entered nothing";
}

// S3's value, written as a number (the other cases add ECUIO_SIM_S3_US, which
// a changed constant moves with them): 5 s of silence lapses the session
TEST(SimAskTest, TheExtendedSessionHoldsJustUnderFiveSecondsAndLapsesAtFive)
{
    const Bytes refused{0x7F, 0x22, 0x31};
    const Bytes d200{0x62, 0xD2, 0x00, 0x1A, 0x2C};
    const auto probe = [&](int64_t silence_us) {
        Unbound u("mock://ask?values=canned&modules=2");
        EXPECT_NE(u.sim, nullptr);
        const auto e = sim_ask(u.sim, 0x7E0, {0x10, 0x03}, 0);
        EXPECT_EQ(e.size(), 1u);
        const auto r = sim_ask(u.sim, 0x7E0, {0x22, 0xD2, 0x00}, silence_us);
        return r.empty() ? Bytes{} : reply_bytes(r[0]);
    };
    EXPECT_EQ(probe(4999000), d200) << "just under 5 s";
    EXPECT_EQ(probe(5000000), refused) << "5 s";
}

// A profile's nodes have no extended session
TEST(SimAskTest, AProfilesNodesHaveNoExtendedSession)
{
    Unbound u("mock://ask?values=canned&profile=obd2");
    ASSERT_NE(u.sim, nullptr);
    const auto r = sim_ask(u.sim, 0x7E0, {0x10, 0x03});
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(reply_bytes(r[0]), (Bytes{0x7F, 0x10, 0x12}));
}
