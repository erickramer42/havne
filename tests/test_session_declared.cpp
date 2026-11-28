#include <gtest/gtest.h>

#include "../core/src/request_engine.h"
#include "../core/src/request_setup.h"

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;
constexpr uint8_t kRange = ECUIO_ENGINE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_ENGINE_REASON_READ;
constexpr uint8_t kVisit = ECUIO_ENGINE_REASON_VISIT;

/* A module with an extended session: 10 xx and 3E 00 answered, its DID */
struct Ecu {
    std::vector<Bytes> heard;

    Bytes operator()(const Bytes& q)
    {
        heard.push_back(q);
        if (q.size() == 2 && q[0] == 0x10) {
            return {0x50, q[1], 0x00, 0x32, 0x01, 0xF4};
        }
        if (q == Bytes{0x3E, 0x00}) {
            return {0x7E, 0x00};
        }
        if (q.size() == 3 && q[0] == 0x22) {
            return {0x62, q[1], q[2], 0x12};
        }
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }

    size_t count(const Bytes& want) const
    {
        size_t n = 0;
        for (const Bytes& b : heard) {
            n += b == want ? 1 : 0;
        }
        return n;
    }
};

void step(ecuio_request_engine* e, uint32_t c, int64_t* now, Ecu* ecu)
{
    const int32_t r = ecuio_engine_due(e, c, *now);
    if (r == ECUIO_ENGINE_NONE) {
        *now += ECUIO_ENGINE_GAP_US;
        return;
    }
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    const Bytes sent(q.bytes, q.bytes + q.len);
    ecuio_engine_sent(e, c, r, true, *now);
    const Bytes reply = (*ecu)(sent);
    *now += 4 * kMs;
    (void)ecuio_engine_on_frame(
        e, c, reply.data(), static_cast<uint16_t>(reply.size()), *now, nullptr);
    *now += ECUIO_ENGINE_GAP_US;
}

/* Two modules with a DID each; channel 1 declares the extended session */
void engine(ecuio_request_engine* e, bool allowed)
{
    ASSERT_TRUE(ecuio_engine_init(e, 2));
    for (uint32_t c = 0; c < 2; ++c) {
        const uint8_t did[3] = {0x22, 0x1E, static_cast<uint8_t>(0x1C + c)};
        ASSERT_EQ(ecuio_engine_add(e, c, did, 3),
                  ecuio_engine_add_result::added);
    }
    e->chans[1].session_may_declare = true;
    ecuio_engine_session_allow(e, 1, allowed);
}

void want(ecuio_request_engine* e, uint8_t r0, uint8_t r1)
{
    const uint8_t reasons[2] = {r0, r1};
    ecuio_engine_modules_apply(e, reasons, nullptr, 0);
}

void run(ecuio_request_engine* e, int64_t* now, Ecu* a, Ecu* b, int steps)
{
    for (int i = 0; i < steps; ++i) {
        step(e, 0, now, a);
        step(e, 1, now, b);
    }
}

} // namespace

TEST(DeclaredSessionTest, ADeclaredModuleIsInItsSessionWhileASignalOnItIsRead)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, true));
    want(&e, kRange | kRead, kRange | kRead);
    int64_t now = 0;
    Ecu other;
    Ecu declared;
    run(&e, &now, &other, &declared, 20);
    ASSERT_FALSE(declared.heard.empty());
    EXPECT_EQ(declared.heard.front(), (Bytes{0x10, 0x03}));
    EXPECT_EQ(declared.count({0x10, 0x03}), 1u);
    EXPECT_EQ(other.count({0x10, 0x03}), 0u);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_source, ECUIO_SESSION_SOURCE_SET);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(s.session_idle, ECUIO_SESSION_IDLE_NONE);

    /* The last signal on it stops being read: one 10 01, the module open */
    want(&e, kRange | kRead, kRange);
    declared.heard.clear();
    run(&e, &now, &other, &declared, 20);
    EXPECT_EQ(declared.count({0x10, 0x01}), 1u);
    EXPECT_EQ(declared.count({0x10, 0x03}), 0u);
    const int64_t quiet = now + ECUIO_ENGINE_S3_US;
    while (now < quiet) {
        step(&e, 1, &now, &declared);
    }
    EXPECT_EQ(declared.count({0x3E, 0x00}), 0u) << "no keep-alive once left";
    EXPECT_EQ(e.chans[1].module_state, ECUIO_MODULE_STATE_OPEN);
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_idle, ECUIO_SESSION_IDLE_NOT_READ);
    EXPECT_EQ(s.session_source, ECUIO_SESSION_SOURCE_NONE);

    /* Read again: entered again */
    want(&e, kRange | kRead, kRange | kRead);
    declared.heard.clear();
    run(&e, &now, &other, &declared, 4);
    EXPECT_EQ(declared.count({0x10, 0x03}), 1u);
}

TEST(DeclaredSessionTest, WithNoConsentNothingIsEntered)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, false));
    want(&e, kRange | kRead, kRange | kRead);
    int64_t now = 0;
    Ecu other;
    Ecu declared;
    run(&e, &now, &other, &declared, 20);
    EXPECT_EQ(declared.count({0x10, 0x03}), 0u);
    EXPECT_GE(declared.count({0x22, 0x1E, 0x1D}), 1u) << "read in default";
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_idle, ECUIO_SESSION_IDLE_NOT_ALLOWED);
    EXPECT_EQ(s.session_asked, 0);

    /* Allowed later, before anything was sent of it: entered */
    static ecuio_request_engine late;
    ASSERT_NO_FATAL_FAILURE(engine(&late, false));
    want(&late, kRange | kRead, kRange | kRead);
    ecuio_engine_session_allow(&late, 1, true);
    ecuio_engine_sessions_follow(&late);
    Ecu again;
    run(&late, &now, &other, &again, 4);
    EXPECT_EQ(again.count({0x10, 0x03}), 1u);
}

TEST(DeclaredSessionTest, AModuleOpenOnlyForItsRangeOrAVisitEntersNothing)
{
    for (const uint8_t reason : {kRange, kVisit}) {
        SCOPED_TRACE(static_cast<int>(reason));
        static ecuio_request_engine e;
        ASSERT_NO_FATAL_FAILURE(engine(&e, true));
        want(&e, kRange | kRead, reason);
        int64_t now = 0;
        Ecu other;
        Ecu declared;
        run(&e, &now, &other, &declared, 20);
        EXPECT_EQ(declared.count({0x10, 0x03}), 0u);
    }
}

TEST(DeclaredSessionTest, AConversationsSessionIsItsOwn)
{
    static ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(engine(&e, true));
    ASSERT_TRUE(ecuio_engine_session_enter(&e, 1));
    want(&e, kRange | kRead, kRange);
    int64_t now = 0;
    Ecu other;
    Ecu declared;
    run(&e, &now, &other, &declared, 20);
    EXPECT_EQ(declared.count({0x10, 0x03}), 1u);
    EXPECT_EQ(declared.count({0x10, 0x01}), 0u)
        << "nothing read there, but the conversation holds it";
    EXPECT_EQ(e.chans[1].session_source, ECUIO_SESSION_SOURCE_CONVERSATION);
}

namespace {

/* A set of two modules, 7E9 declaring session, one DID on each; obd2's
 * engine PID too on 7E9 when standard */
struct Declaring {
    ecuio_channel_decl_t channels[2] = {};
    ecuio_signal_set_t set{};

    explicit Declaring(uint8_t session)
    {
        channels[0].rx_id = 0x7E8;
        channels[0].tx_id = 0x7E0;
        channels[1].rx_id = 0x7E9;
        channels[1].tx_id = 0x7E1;
        channels[1].session = session;
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.channel_count = 2;
        set.channels = channels;
    }
};

const uint32_t kRx[2] = {0x7E8, 0x7E9};

void declare(ecuio_request_engine* e, const Declaring& d, bool standard,
             const request_params& p, bool* ok)
{
    ASSERT_TRUE(ecuio_engine_init(e, 2));
    const uint8_t did[3] = {0x22, 0x1E, 0x1D};
    ASSERT_EQ(ecuio_engine_add(e, 1, did, 3), ecuio_engine_add_result::added);
    if (standard) {
        const uint8_t pid[2] = {0x01, 0x0C};
        ASSERT_EQ(ecuio_engine_add(e, 1, pid, 2),
                  ecuio_engine_add_result::added);
    }
    *ok = ecuio_declare_sessions(e, &d.set, kRx, 2, p);
}

} // namespace

TEST(DeclaredSessionTest, TheAttachJudgesEachModulesDeclaration)
{
    static ecuio_request_engine e;
    bool ok = false;
    request_params p;
    ASSERT_NO_FATAL_FAILURE(
        declare(&e, Declaring(ECUIO_CHANNEL_SESSION_EXTENDED), false, p, &ok));
    EXPECT_TRUE(ok);
    EXPECT_FALSE(e.chans[0].session_may_declare);
    EXPECT_TRUE(e.chans[1].session_may_declare);

    ASSERT_NO_FATAL_FAILURE(
        declare(&e, Declaring(ECUIO_CHANNEL_SESSION_EXTENDED), true, p, &ok));
    EXPECT_FALSE(e.chans[1].session_may_declare)
        << "a module the standard signals read stays in default";
    EXPECT_EQ(e.chans[1].session_idle, ECUIO_SESSION_IDLE_STANDARD);

    ASSERT_NO_FATAL_FAILURE(
        declare(&e, Declaring(ECUIO_CHANNEL_SESSION_DEFAULT), false, p, &ok));
    EXPECT_FALSE(e.chans[1].session_may_declare);
    EXPECT_EQ(e.chans[1].session_idle, ECUIO_SESSION_IDLE_DEFAULT);

    request_params off;
    off.session_given = true;
    ASSERT_NO_FATAL_FAILURE(declare(
        &e, Declaring(ECUIO_CHANNEL_SESSION_EXTENDED), false, off, &ok));
    EXPECT_FALSE(e.chans[1].session_may_declare) << "?session=01";
    EXPECT_EQ(e.chans[1].session_idle, ECUIO_SESSION_IDLE_OFF);

    request_params keep;
    keep.keepalive_ms = 1000;
    ASSERT_NO_FATAL_FAILURE(declare(
        &e, Declaring(ECUIO_CHANNEL_SESSION_EXTENDED), false, keep, &ok));
    EXPECT_TRUE(ok) << "?keepalive_ms= keeps a declared session";
    EXPECT_EQ(e.keepalive_us, 1000 * kMs);
    ASSERT_NO_FATAL_FAILURE(
        declare(&e, Declaring(ECUIO_CHANNEL_SESSION_NONE), false, keep, &ok));
    EXPECT_FALSE(ok) << "?keepalive_ms= with no session to keep";
}
