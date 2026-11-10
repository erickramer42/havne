// File/layer: tests/test_session_control.cpp | test_obd2, Qt-free
// Testing: UNIT: SessionControlTest (the request engine's session step,
//          ADR-0060, with the time passed in)

#include <gtest/gtest.h>

#include "../core/src/request_engine.h"
#include "../core/src/request_setup.h"
#include "../core/src/uri_query.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
constexpr int64_t kMs = 1000;

constexpr uint16_t kDid = 0x1E1C;    /* answered in either session */
constexpr uint16_t kExtDid = 0xD100; /* answered in the extended one only */

/* An ECU with an extended session, or none: 10 03 entered or refused with
 * refuse_nrc (silent: no answer at all), 10 01 left, 3E 00 answered;
 * kDid always, kExtDid only in the extended session (7F 22 31 in the
 * default one, as an ECU that keeps a DID to a session answers) */
struct Ecu {
    bool has_extended = true;
    uint8_t refuse_nrc = 0x12;
    bool silent_session = false;
    bool pending_first = false; /* 10 03 answered 7F 10 78 once first */
    bool extended = false;
    int entries = 0;

    Bytes operator()(const Bytes& q)
    {
        if (q.size() == 2 && q[0] == 0x10) {
            if (q[1] == 0x01) {
                extended = false;
                return {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4};
            }
            if (silent_session) {
                return {};
            }
            if (!has_extended || q[1] != 0x03) {
                return {0x7F, 0x10, refuse_nrc};
            }
            extended = true;
            ++entries;
            return {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
        }
        if (q.size() == 2 && q[0] == 0x3E && q[1] == 0x00) {
            return {0x7E, 0x00};
        }
        if (q.size() == 3 && q[0] == 0x22) {
            const uint16_t did = static_cast<uint16_t>((q[1] << 8) | q[2]);
            if (did == kDid) {
                return {0x62, q[1], q[2], 0x12, 0x34};
            }
            if (did == kExtDid && extended) {
                return {0x62, q[1], q[2], 0xAA};
            }
            return {0x7F, 0x22, 0x31};
        }
        return {0x7F, q.empty() ? uint8_t{0} : q[0], 0x11};
    }
};

using Responder = std::function<Bytes(const Bytes&)>;

/* Sends channel c's next request at *now and answers it through respond
 * (silence waits the wait out); the bytes sent, or none when nothing was
 * due (the time then moves on a floor's gap) */
Bytes step(ecuio_request_engine* e, uint32_t c, int64_t* now,
           const Responder& respond)
{
    const int32_t r = ecuio_engine_due(e, c, *now);
    if (r == ECUIO_ENGINE_NONE) {
        *now += ECUIO_ENGINE_GAP_US;
        return {};
    }
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    const Bytes sent(q.bytes, q.bytes + q.len);
    ecuio_engine_sent(e, c, r, true, *now);
    const Bytes reply = respond(sent);
    if (reply.empty()) {
        *now += ECUIO_ENGINE_WAIT_US;
        EXPECT_TRUE(ecuio_engine_expire(e, c, *now));
    } else {
        *now += 4 * kMs;
        (void)ecuio_engine_on_frame(e, c, reply.data(),
                                    static_cast<uint16_t>(reply.size()), *now,
                                    nullptr);
    }
    *now += ECUIO_ENGINE_GAP_US;
    return sent;
}

/* n requests (or idle steps) of channel c, what went out in order */
std::vector<Bytes> steps(ecuio_request_engine* e, uint32_t c, int64_t* now,
                         const Responder& respond, int n)
{
    std::vector<Bytes> out;
    for (int i = 0; i < n; ++i) {
        Bytes b = step(e, c, now, respond);
        if (!b.empty()) {
            out.push_back(b);
        }
    }
    return out;
}

bool is(const Bytes& b, std::initializer_list<uint8_t> want)
{
    return b == Bytes(want);
}

size_t count_of(const std::vector<Bytes>& v,
                std::initializer_list<uint8_t> want)
{
    size_t n = 0;
    for (const Bytes& b : v) {
        n += is(b, want) ? 1 : 0;
    }
    return n;
}

/* One channel: kDid in the rotation, kExtDid scanned */
void uds_engine(ecuio_request_engine* e, uint32_t channels = 1)
{
    ecuio_engine_init(e, channels);
    for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t did[3] = {0x22, kDid >> 8, kDid & 0xFF};
        ASSERT_EQ(ecuio_engine_add(e, c, did, 3),
                  ecuio_engine_add_result::added);
        ASSERT_TRUE(ecuio_engine_scan_add(e, c, kExtDid));
    }
}

/* Every row of the rotation parked, as a focus parks them: the channel
 * asks nothing more of its own */
void park_all(ecuio_request_engine* e)
{
    for (uint32_t i = 0; i < e->count; ++i) {
        e->requests[i].parked = 1;
    }
    ecuio_engine_recount(e);
}

} // namespace

TEST(SessionControlTest, OnlyTheExtendedSessionIsEntered)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    EXPECT_FALSE(ecuio_engine_session(&e, 0x02, ECUIO_ENGINE_KEEPALIVE_US));
    EXPECT_FALSE(ecuio_engine_session(&e, 0x01, ECUIO_ENGINE_KEEPALIVE_US));
    EXPECT_FALSE(ecuio_engine_session(&e, 0x04, ECUIO_ENGINE_KEEPALIVE_US));
    EXPECT_EQ(e.session, 0);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_NOT_ASKED);
    EXPECT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    EXPECT_EQ(e.session, 0x03);
}

/* The engine's own check, whatever reached its state: a session the
 * allow-list refuses is never sent, and the channel reads in default */
TEST(SessionControlTest, TheEngineNeverSendsASessionItsListRefuses)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    e.session = 0x02; /* past ecuio_engine_session, as a bad caller might */
    e.chans[0].session_step = ECUIO_SESSION_STATE_ENTERING;
    Ecu ecu;
    int64_t now = 0;
    const std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 20);
    for (const Bytes& b : sent) {
        EXPECT_FALSE(b.size() >= 1 && (b[0] == 0x10 || b[0] == 0x3E))
            << "sent " << static_cast<int>(b[0]) << " "
            << static_cast<int>(b.size() > 1 ? b[1] : 0);
    }
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_REFUSED);
    ecuio_request q;
    EXPECT_FALSE(ecuio_engine_session_leave(&e, 0, &q));
}

TEST(SessionControlTest, TheSessionIsEnteredBeforeAnythingElseGoesOut)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    int64_t now = 0;
    const std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 6);
    ASSERT_GE(sent.size(), 3u);
    EXPECT_TRUE(is(sent[0], {0x10, 0x03}));
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 1u);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[0].session_entries, 1u);
    /* the extended-only DID is found in it */
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
}

TEST(SessionControlTest, NothingElseGoesOutWhileTheEntryIsPending)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    int64_t now = 0;
    ASSERT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, now);
    const uint8_t pending[3] = {0x7F, 0x10, 0x78};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, pending, 3, now + 50 * kMs, nullptr),
              ecuio_reply::pending);
    /* the pending wait, past the plain one */
    now += 2 * ECUIO_ENGINE_WAIT_US;
    EXPECT_FALSE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    const uint8_t entered[6] = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, entered, 6, now, nullptr),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
}

TEST(SessionControlTest, ARefusalReadsOnInTheDefaultSessionWithItsCode)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    ecu.has_extended = false;
    ecu.refuse_nrc = 0x22;
    int64_t now = 0;
    std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 10);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_REFUSED);
    EXPECT_EQ(e.chans[0].session_nrc, 0x22);
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 1u);
    EXPECT_GE(count_of(sent, {0x22, kDid >> 8, kDid & 0xFF}), 2u);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_REFUSED);
    /* a refusal stands: a recovery doesn't ask again */
    ecuio_engine_rescan(&e);
    sent = steps(&e, 0, &now, std::ref(ecu), 10);
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 0u);
    ecuio_request q;
    EXPECT_FALSE(ecuio_engine_session_leave(&e, 0, &q));
}

TEST(SessionControlTest, AnUnansweredEntryIsAskedAgainOnARecovery)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    ecu.silent_session = true;
    int64_t now = 0;
    std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 10);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_UNANSWERED);
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 1u);
    EXPECT_GE(count_of(sent, {0x22, kDid >> 8, kDid & 0xFF}), 2u);
    ecu.silent_session = false;
    ecuio_engine_rescan(&e);
    sent = steps(&e, 0, &now, std::ref(ecu), 4);
    ASSERT_FALSE(sent.empty());
    EXPECT_TRUE(is(sent[0], {0x10, 0x03}));
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
}

/* One module refusing doesn't hold back another: each channel's session
 * is its own */
TEST(SessionControlTest, EachChannelEntersOrIsRefusedOnItsOwn)
{
    static ecuio_request_engine e;
    uds_engine(&e, 2);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu engine_ecu;
    Ecu body;
    body.has_extended = false;
    int64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        (void)step(&e, 0, &now, std::ref(engine_ecu));
        (void)step(&e, 1, &now, std::ref(body));
    }
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[1].session_step, ECUIO_SESSION_STATE_REFUSED);
    EXPECT_EQ(e.chans[1].session_nrc, 0x12);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);   /* channel 0's */
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_REFUSED); /* channel 1's */
}

TEST(SessionControlTest, TesterPresentFillsOnlyAQuietGap)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, 1000 * kMs));
    Ecu ecu;
    int64_t now = 0;
    /* A busy rotation keeps the session: no tester present in 5 s */
    std::vector<Bytes> sent;
    while (now < 5000 * kMs) {
        Bytes b = step(&e, 0, &now, std::ref(ecu));
        if (!b.empty()) {
            sent.push_back(b);
        }
    }
    EXPECT_EQ(count_of(sent, {0x3E, 0x00}), 0u);
    /* Parked, the rotation asks nothing: the gap gets 3E 00 once its
     * time has passed since the last request, not before */
    park_all(&e);
    const int64_t last = e.chans[0].last_sent_us;
    const int64_t in = ecuio_engine_next_event_us(&e, now);
    EXPECT_EQ(now + in, last + 1000 * kMs);
    now = last + 1000 * kMs - kMs;
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    now = last + 1000 * kMs;
    sent = steps(&e, 0, &now, std::ref(ecu), 1);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is(sent[0], {0x3E, 0x00}));
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
}

TEST(SessionControlTest, KeepaliveIsHeldToItsBounds)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, 1));
    EXPECT_EQ(e.keepalive_us, ECUIO_ENGINE_KEEPALIVE_MIN_US);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_S3_US));
    EXPECT_EQ(e.keepalive_us, ECUIO_ENGINE_KEEPALIVE_MAX_US);
    EXPECT_LT(ECUIO_ENGINE_KEEPALIVE_MAX_US, ECUIO_ENGINE_S3_US);
}

/* 7F (or 7E) to a request in the session: the ECU left it. The session
 * is entered again and the request asked again, not dropped; refused
 * again straight after an entry, it's the request's own answer. */
TEST(SessionControlTest, ALostSessionIsEnteredAgainAndTheRequestAskedAgain)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    ecuio_engine_probe(&e, 0); /* drops on explicit answers */
    Ecu ecu;
    int64_t now = 0;
    (void)steps(&e, 0, &now, std::ref(ecu), 6);
    ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    ASSERT_FALSE(e.chans[0].session_fresh);
    /* the ECU falls back to default and refuses 0x22 there */
    bool lost = true;
    const Responder fell_back = [&](const Bytes& q) -> Bytes {
        if (lost && q.size() == 3 && q[0] == 0x22) {
            return {0x7F, 0x22, 0x7F};
        }
        if (q.size() == 2 && q[0] == 0x10) {
            lost = false;
        }
        return ecu(q);
    };
    std::vector<Bytes> sent = steps(&e, 0, &now, fell_back, 4);
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent[0][0], 0x22);
    EXPECT_TRUE(is(sent[1], {0x10, 0x03}));
    EXPECT_EQ(sent[2][0], 0x22);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].dropped, 0) << i;
    }
    EXPECT_EQ(e.chans[0].session_lost, 1u);
    EXPECT_EQ(e.chans[0].session_entries, 2u);
    /* Refused at once after an entry: no loop, the request is dropped */
    const Responder always = [&](const Bytes& q) -> Bytes {
        if (q.size() == 3 && q[0] == 0x22) {
            return {0x7F, 0x22, 0x7F};
        }
        return ecu(q);
    };
    e.chans[0].session_fresh = false;
    sent = steps(&e, 0, &now, always, 10);
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 1u);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_ACCESS);
    EXPECT_EQ(e.chans[0].session_entries, 3u);
}

TEST(SessionControlTest, AScannedDidRefusedForALostSessionIsAskedAgain)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    e.scan_first = true;
    Ecu ecu;
    int64_t now = 0;
    /* entered, then the rotation's DID answered */
    ASSERT_TRUE(is(step(&e, 0, &now, std::ref(ecu)), {0x10, 0x03}));
    e.chans[0].session_fresh = false; /* as after another positive reply */
    bool lost = true;
    const Responder fell_back = [&](const Bytes& q) -> Bytes {
        if (lost && q.size() == 3 && q[0] == 0x22) {
            return {0x7F, 0x22, 0x7E};
        }
        if (q.size() == 2 && q[0] == 0x10) {
            lost = false;
        }
        return ecu(q);
    };
    const std::vector<Bytes> sent = steps(&e, 0, &now, fell_back, 4);
    ASSERT_GE(sent.size(), 3u);
    EXPECT_TRUE(is(sent[0], {0x22, kExtDid >> 8, kExtDid & 0xFF}));
    EXPECT_TRUE(is(sent[1], {0x10, 0x03}));
    EXPECT_TRUE(is(sent[2], {0x22, kExtDid >> 8, kExtDid & 0xFF}));
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
}

TEST(SessionControlTest, TesterPresentUnansweredEntersTheSessionAgain)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, 1000 * kMs));
    Ecu ecu;
    int64_t now = 0;
    (void)steps(&e, 0, &now, std::ref(ecu), 4);
    park_all(&e);
    now = e.chans[0].last_sent_us + 1000 * kMs;
    const Responder silent_tp = [&](const Bytes& q) -> Bytes {
        return (q.size() == 2 && q[0] == 0x3E) ? Bytes{} : ecu(q);
    };
    const std::vector<Bytes> sent = steps(&e, 0, &now, silent_tp, 3);
    ASSERT_GE(sent.size(), 2u);
    EXPECT_TRUE(is(sent[0], {0x3E, 0x00}));
    EXPECT_TRUE(is(sent[1], {0x10, 0x03}));
    EXPECT_EQ(e.chans[0].session_lost, 1u);
}

/* A worker held up past S3 asks nothing for 5 s: the ECU has left the
 * session, so it's entered again rather than kept */
TEST(SessionControlTest, ASessionQuietForS3IsEnteredAgain)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    int64_t now = 0;
    (void)steps(&e, 0, &now, std::ref(ecu), 4);
    now = e.chans[0].last_sent_us + ECUIO_ENGINE_S3_US;
    const std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 1);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is(sent[0], {0x10, 0x03}));
    EXPECT_EQ(e.chans[0].session_lost, 1u);
}

TEST(SessionControlTest, ARecoveryEntersAnEnteredSessionAgain)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    int64_t now = 0;
    (void)steps(&e, 0, &now, std::ref(ecu), 4);
    ecuio_engine_rescan(&e);
    const std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 1);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is(sent[0], {0x10, 0x03}));
}

TEST(SessionControlTest, TheCloseLeavesOnlyASessionEntered)
{
    static ecuio_request_engine e;
    uds_engine(&e, 2);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu entered;
    Ecu refused;
    refused.has_extended = false;
    int64_t now = 0;
    for (int i = 0; i < 4; ++i) {
        (void)step(&e, 0, &now, std::ref(entered));
        (void)step(&e, 1, &now, std::ref(refused));
    }
    ecuio_request q;
    ASSERT_TRUE(ecuio_engine_session_leave(&e, 0, &q));
    EXPECT_EQ(q.len, 2);
    EXPECT_EQ(q.bytes[0], 0x10);
    EXPECT_EQ(q.bytes[1], 0x01);
    EXPECT_FALSE(ecuio_engine_session_leave(&e, 1, &q));
}

/* With no ?session=, nothing changes: no session request, ever */
TEST(SessionControlTest, TheDefaultSendsNoSessionRequest)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    Ecu ecu;
    int64_t now = 0;
    park_all(&e);
    std::vector<Bytes> sent;
    while (now < 12000 * kMs) {
        Bytes b = step(&e, 0, &now, std::ref(ecu));
        if (!b.empty()) {
            sent.push_back(b);
        }
    }
    for (const Bytes& b : sent) {
        EXPECT_NE(b[0], 0x10);
        EXPECT_NE(b[0], 0x3E);
    }
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), INT64_MAX);
    ecuio_request q;
    EXPECT_FALSE(ecuio_engine_session_leave(&e, 0, &q));
}

namespace {

/* ecuio_request_param over one key=value */
bool param(const char* text, request_params* p, const char** wanted)
{
    ecuio_uri::param q{};
    const char* eq = strchr(text, '=');
    q.key = text;
    q.key_len = static_cast<size_t>(eq - text);
    q.val = eq + 1;
    q.val_len = strlen(eq + 1);
    *wanted = nullptr;
    return ecuio_request_param(q, p, wanted);
}

} // namespace

TEST(SessionControlTest, TheUriTakesTheDefaultOrTheExtendedSessionOnly)
{
    const char* wanted = nullptr;
    request_params p;
    EXPECT_EQ(p.session, ECUIO_SESSION_DEFAULT);
    ASSERT_TRUE(param("session=03", &p, &wanted));
    EXPECT_EQ(wanted, nullptr);
    EXPECT_EQ(p.session, 0x03);
    ASSERT_TRUE(param("session=01", &p, &wanted));
    EXPECT_EQ(p.session, 0x01);
    for (const char* bad : {"session=02", "session=04", "session=", "session=x",
                            "session=0x40", "session=-3"}) {
        request_params q;
        ASSERT_TRUE(param(bad, &q, &wanted)) << bad;
        EXPECT_STREQ(wanted,
                     "01 (the default session) or 03 (the extended one)")
            << bad;
        EXPECT_EQ(q.session, ECUIO_SESSION_DEFAULT) << bad;
    }
    ASSERT_TRUE(param("keepalive_ms=500", &p, &wanted));
    EXPECT_EQ(p.keepalive_ms, 500u);
    ASSERT_TRUE(param("keepalive_ms=4500", &p, &wanted));
    EXPECT_EQ(p.keepalive_ms, 4500u);
    for (const char* bad : {"keepalive_ms=499", "keepalive_ms=4501",
                            "keepalive_ms=5000", "keepalive_ms=0"}) {
        request_params q;
        ASSERT_TRUE(param(bad, &q, &wanted)) << bad;
        EXPECT_STREQ(wanted, "a time from 500 to 4500 ms") << bad;
        EXPECT_EQ(q.keepalive_ms, 0u) << bad;
    }
}

namespace {

/* An ECU that enters the session but drops out of it after every n-th
 * read it answers: the read after is refused 7F 22 7F until 10 03 again */
struct Flaky {
    Ecu ecu;
    int every = 2;
    int answered = 0;
    bool out = false;

    Bytes operator()(const Bytes& q)
    {
        if (q.size() == 2 && q[0] == 0x10) {
            out = false;
            answered = 0;
            return ecu(q);
        }
        if (q.size() == 3 && q[0] == 0x22) {
            if (out) {
                return {0x7F, 0x22, 0x7F};
            }
            if (++answered == every) {
                out = true;
            }
        }
        return ecu(q);
    }
};

} // namespace

/* A module that keeps leaving the session isn't asked to enter it every
 * few reads: the bound's losses within its window abandon it, and it
 * reads in default with the code named */
TEST(SessionControlTest, AModuleThatKeepsLeavingTheSessionIsReadInDefault)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Flaky ecu;
    int64_t now = 0;
    const std::vector<Bytes> sent = steps(&e, 0, &now, std::ref(ecu), 60);
    ASSERT_LT(now, ECUIO_ENGINE_LOSS_WINDOW_US);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ABANDONED);
    EXPECT_EQ(e.chans[0].session_nrc, 0x7F);
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), ECUIO_SESSION_LOSSES_MAX);
    EXPECT_EQ(e.chans[0].session_lost, ECUIO_SESSION_LOSSES_MAX - 1);
    /* abandoned: no 10 03 and no tester present from then on */
    const std::vector<Bytes> later = steps(&e, 0, &now, std::ref(ecu), 60);
    EXPECT_EQ(count_of(later, {0x10, 0x03}), 0u);
    EXPECT_EQ(count_of(later, {0x3E, 0x00}), 0u);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_ABANDONED);
    EXPECT_EQ(s.session, ECUIO_SESSION_DEFAULT);
    EXPECT_EQ(s.flags & ECUIO_SUPPORT_EXTENDED, 0u);
    /* it entered once, so the close still returns it to default */
    ecuio_request q;
    EXPECT_TRUE(ecuio_engine_session_leave(&e, 0, &q));
}

TEST(SessionControlTest, LossesFurtherApartThanTheWindowNeverAbandonIt)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    int64_t now = 0;
    (void)steps(&e, 0, &now, std::ref(ecu), 4);
    const uint8_t lost[3] = {0x7F, 0x22, 0x7F};
    for (uint32_t i = 0; i < 2 * ECUIO_SESSION_LOSSES_MAX; ++i) {
        /* a read answered, then one refused for a lost session, then the
         * session entered again: one loss a window */
        now += ECUIO_ENGINE_LOSS_WINDOW_US / 2 + kMs;
        e.chans[0].last_sent_us = now; /* a keepalive kept it meanwhile */
        (void)steps(&e, 0, &now, std::ref(ecu), 3);
        int32_t r = ECUIO_ENGINE_NONE;
        for (int k = 0; k < 10 && r == ECUIO_ENGINE_NONE; ++k) {
            r = ecuio_engine_due(&e, 0, now);
            now += (r == ECUIO_ENGINE_NONE) ? ECUIO_ENGINE_GAP_US : 0;
        }
        ASSERT_GE(r, 0) << i; /* a read of the rotation */
        ecuio_engine_sent(&e, 0, r, true, now);
        (void)ecuio_engine_on_frame(&e, 0, lost, 3, now + kMs, nullptr);
        now += ECUIO_ENGINE_GAP_US;
        (void)steps(&e, 0, &now, std::ref(ecu), 1); /* 10 03 */
        ASSERT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED) << i;
    }
    EXPECT_EQ(e.chans[0].session_lost, 2 * ECUIO_SESSION_LOSSES_MAX);
}

TEST(SessionControlTest, TheCountsStopAt65535)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu ecu;
    int64_t now = 0;
    e.chans[0].session_entries = 65534;
    e.chans[0].session_lost = 65535;
    (void)steps(&e, 0, &now, std::ref(ecu), 1);
    EXPECT_EQ(e.chans[0].session_entries, 65535);
    for (int i = 0; i < 2; ++i) {
        ecuio_engine_rescan(&e); /* a recovery: entered again */
        (void)steps(&e, 0, &now, std::ref(ecu), 1);
    }
    EXPECT_EQ(e.chans[0].session_entries, 65535);
    EXPECT_EQ(e.chans[0].session_lost, 65535);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.session_entries, 65535);
    EXPECT_EQ(s.session_lost, 65535);
}

TEST(SessionControlTest, TheReportNamesEachChannelsSession)
{
    static ecuio_request_engine e;
    uds_engine(&e, 2);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.session_asked, 0);
    EXPECT_EQ(s.session, ECUIO_SESSION_DEFAULT);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_NOT_ASKED);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    Ecu entered;
    Ecu refused;
    refused.has_extended = false;
    refused.refuse_nrc = 0x22;
    int64_t now = 0;
    const uint32_t before = e.chans[0].generation;
    for (int i = 0; i < 4; ++i) {
        (void)step(&e, 0, &now, std::ref(entered));
        (void)step(&e, 1, &now, std::ref(refused));
    }
    EXPECT_NE(e.chans[0].generation, before);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.session_asked, 0x03);
    EXPECT_EQ(s.session, 0x03);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(s.session_nrc, 0);
    EXPECT_EQ(s.session_entries, 1);
    EXPECT_EQ(s.session_lost, 0);
    EXPECT_NE(s.flags & ECUIO_SUPPORT_EXTENDED, 0u);
    ecuio_engine_support(&e, 1, &s);
    EXPECT_EQ(s.session_asked, 0x03);
    EXPECT_EQ(s.session, ECUIO_SESSION_DEFAULT);
    EXPECT_EQ(s.session_state, ECUIO_SESSION_STATE_REFUSED);
    EXPECT_EQ(s.session_nrc, 0x22);
    EXPECT_EQ(s.session_entries, 0);
    EXPECT_EQ(s.flags & ECUIO_SUPPORT_EXTENDED, 0u);
}

/* A 7F straight after the entry is the read's own answer, not a lost
 * session: the read drops and the session stands, entered once */
TEST(SessionControlTest, ARefusalStraightAfterTheEntryIsTheReadsOwn)
{
    static ecuio_request_engine e;
    uds_engine(&e);
    ASSERT_TRUE(ecuio_engine_session(&e, 0x03, ECUIO_ENGINE_KEEPALIVE_US));
    ecuio_engine_probe(&e, 0);
    Ecu ecu;
    const Responder refuses = [&](const Bytes& q) -> Bytes {
        if (q.size() == 3 && q[0] == 0x22 && q[1] == (kDid >> 8)) {
            return {0x7F, 0x22, 0x7F};
        }
        return ecu(q);
    };
    int64_t now = 0;
    const std::vector<Bytes> sent = steps(&e, 0, &now, refuses, 6);
    ASSERT_GE(sent.size(), 3u);
    EXPECT_TRUE(is(sent[0], {0x10, 0x03}));
    EXPECT_TRUE(is(sent[1], {0x22, kDid >> 8, kDid & 0xFF}));
    EXPECT_EQ(count_of(sent, {0x10, 0x03}), 1u);
    EXPECT_EQ(e.requests[0].dropped, ECUIO_DROP_ACCESS);
    EXPECT_EQ(e.chans[0].session_step, ECUIO_SESSION_STATE_ENTERED);
    EXPECT_EQ(e.chans[0].session_lost, 0);
}
