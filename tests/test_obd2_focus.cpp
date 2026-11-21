// File/layer: tests/test_obd2_focus.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

/* Starts channel 0's clock over, for a helper that counts time from 0
 * (rotation_of, packing_turns) run again on an engine that has run:
 * nothing is out, so only the floor stands in the way */
void clock_over(ecuio_request_engine* e)
{
    ASSERT_EQ(e->chans[0].outstanding, ECUIO_ENGINE_NONE);
    e->chans[0].next_send_us = 0;
    e->chans[0].deadline_us = 0;
}

} // namespace

/* ADR-0056 section 2: a focus parks every request outside it and runs
 * the picks in the fast class, so a slow PID picked is asked each turn,
 * the two picks taking turns in table order; a count of 0 unparks them
 * all and gives each its class back */
TEST(RequestEngineTest, AFocusParksTheRestAndRunsThePicksFast)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ASSERT_TRUE(
        ecuio_engine_derive(&e, ecuio_obd2_signal_set(), &rx, 1, nullptr));
    ecuio_engine_classify(&e, ecuio_obd2_pid_fast);
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    uint32_t slow_row = UINT32_MAX;
    uint32_t fast_row = UINT32_MAX;
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        if (pid == 0x0C || pid == 0x05) {
            keep[i] = 1;
        }
        if (pid == 0x05) {
            slow_row = i;
        }
        if (pid == 0x0C) {
            fast_row = i;
        }
    }
    ASSERT_NE(slow_row, UINT32_MAX);
    ASSERT_NE(fast_row, UINT32_MAX);
    ASSERT_EQ(e.requests[slow_row].slow, 1u) << "05 is slow by class";
    const uint32_t before = e.chans[0].requests;
    const uint32_t generation = e.chans[0].generation;

    ecuio_engine_focus(&e, keep);
    EXPECT_TRUE(e.focused);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_GT(e.chans[0].generation, generation);
    EXPECT_EQ(e.requests[slow_row].slow, 0u) << "a pick runs fast";
    EXPECT_EQ(e.requests[slow_row].slow_default, 1u);
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].parked != 0, keep[i] == 0) << "row " << i;
    }
    const uint8_t first = slow_row < fast_row ? 0x05 : 0x0C;
    const uint8_t second = first == 0x05 ? 0x0C : 0x05;
    EXPECT_EQ(
        rotation_of(&e, 6),
        (std::vector<uint8_t>{first, second, first, second, first, second}));

    ecuio_engine_unfocus(&e);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    EXPECT_FALSE(e.focused);
    EXPECT_EQ(e.chans[0].requests, before);
    EXPECT_EQ(e.requests[slow_row].slow, 1u) << "its class is back";
    for (uint32_t i = 0; i < e.count; ++i) {
        EXPECT_EQ(e.requests[i].parked, 0u) << "row " << i;
        EXPECT_EQ(e.requests[i].slow, e.requests[i].slow_default);
    }
    /* Two fast to one slow again: three slow turns in nine, whatever the
     * phase the focused turns left the counter in */
    const std::vector<uint8_t> sent = rotation_of(&e, 9);
    ASSERT_EQ(sent.size(), 9u);
    size_t slow_turns = 0;
    for (uint8_t pid : sent) {
        slow_turns += ecuio_obd2_pid_fast(pid) ? 0 : 1;
    }
    EXPECT_EQ(slow_turns, 3u);
}

/* A focus that leaves a channel nothing to ask: the rotation sends
 * nothing and the engine has no next event, and the focus's end brings
 * the rotation back */
TEST(RequestEngineTest, AFocusWithNoPickOnAChannelLeavesItNothingToAsk)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    const uint8_t none[ECUIO_PROFILE_MAX_REQUESTS] = {};
    ecuio_engine_focus(&e, none);
    EXPECT_EQ(e.chans[0].requests, 0u);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu);
    EXPECT_LT(ecuio_engine_next_event_us(&e, 0), INT64_MAX);
}

/* The probe's walk runs on under a focus and what it drops stays
 * dropped after the focus ends; a request both parked and dropped is
 * counted out of the rotation once, and a rescan unparks nothing.
 * Bitmap 00's bits: PID p is bit 32 - p, so 0x00180000 lists 0x0C and
 * 0x0D, leaves 0x0B clear, and ends the chain. */
TEST(RequestEngineTest, TheWalkDropsUnderAFocusAndADropOutlivesIt)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C, 0x0D}));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[1] = 1; /* 0x0C */
    ecuio_engine_focus(&e, keep);
    ecuio_engine_probe(&e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, 0));
    ASSERT_EQ(answer_bitmap(&e, 0, 0x00, 0x00180000u, 4 * kMs),
              ecuio_reply::positive);
    EXPECT_FALSE(e.chans[0].walking);
    EXPECT_NE(e.requests[0].dropped, 0u) << "0x0B is unsupported";
    EXPECT_EQ(e.requests[0].parked, 1u);
    EXPECT_EQ(e.chans[0].requests, 1u) << "0x0C alone; 0x0B counted out once";
    EXPECT_EQ(send_next(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US), 0x0Cu);
    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.requests[0].parked, 1u) << "a rescan unparks nothing";
    EXPECT_EQ(e.requests[2].parked, 1u);
    ecuio_engine_unfocus(&e);
    EXPECT_NE(e.requests[0].dropped, 0u) << "the drop outlives the focus";
    EXPECT_EQ(e.requests[0].parked, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u) << "0x0C and 0x0D";
}

/* A pack gathers kept requests alone: under a focus on three fast PIDs
 * every request out names picks alone, and the three pack together,
 * the parked rows between them passed over (0x11, a fast row between
 * 0x0D and 0x10 in the table, is parked) */
TEST(RequestEngineTest, APackGathersKeptRequestsAlone)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_from(fake_reply));
    const std::vector<uint8_t> picks = {0x0C, 0x0D, 0x10};
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e.count; ++i) {
        if (std::find(picks.begin(), picks.end(), e.requests[i].bytes[1]) !=
            picks.end()) {
            keep[i] = 1;
        }
    }
    ecuio_engine_focus(&e, keep);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    const std::vector<Bytes> sent =
        packing_turns(&e, 40, packed_from(fake_reply));
    ASSERT_FALSE(sent.empty());
    bool all_three = false;
    for (const Bytes& req : sent) {
        ASSERT_GE(req.size(), 2u);
        EXPECT_EQ(req[0], 0x01u);
        for (size_t k = 1; k < req.size(); ++k) {
            EXPECT_NE(std::find(picks.begin(), picks.end(), req[k]),
                      picks.end())
                << "PID " << int(req[k]) << " isn't a pick";
        }
        all_three = all_three || req.size() == 1 + picks.size();
    }
    EXPECT_TRUE(all_three) << "the three picks pack as one request";
}

namespace {

/* One request out on channel 0: when, and what it asked */
struct Asked {
    int64_t at;
    Bytes request;
};

/* Channel 0's rotation up to until_us on the engine's own clock, each
 * request answered 4 ms later through respond, the clock stepped by
 * ecuio_engine_next_event_us alone: a wait it doesn't report fails here,
 * where a worker would spin */
std::vector<Asked> paced_run(ecuio_request_engine* e, int64_t until_us,
                             const Responder& respond)
{
    std::vector<Asked> out;
    int64_t now = 0;
    while (now < until_us) {
        (void)ecuio_engine_expire(e, 0, now);
        const int32_t r = ecuio_engine_due(e, 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            const int64_t next = ecuio_engine_next_event_us(e, now);
            if (next <= 0 || next == INT64_MAX) {
                ADD_FAILURE() << "nothing due, and next event " << next
                              << " us, at " << now;
                break;
            }
            now += next;
            continue;
        }
        const ecuio_request& q = ecuio_engine_request_of(e, 0, r);
        const Bytes req(q.bytes, q.bytes + q.len);
        out.push_back({now, req});
        ecuio_engine_sent(e, 0, r, true, now);
        now += 4 * kMs;
        const Bytes reply = respond(req);
        if (!reply.empty()) {
            (void)ecuio_engine_on_frame(e, 0, reply.data(),
                                        static_cast<uint16_t>(reply.size()),
                                        now, nullptr);
        }
    }
    return out;
}

Bytes single_pid_reply(const Bytes& req)
{
    return Bytes{0x41, req[1], 0x12, 0x34};
}

/* Keeps every row of a table one_channel_of made */
void focus_all(ecuio_request_engine* e, int64_t min_sweep_us)
{
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e->count; ++i) {
        keep[i] = 1;
    }
    ecuio_engine_focus(e, keep, min_sweep_us);
}

} // namespace

/* ADR-0056 section 4: a sweep starts no sooner than the floor after the
 * last one started, the requests inside it at the engine's own 20 ms
 * floor; the picks keep their order, and the wait is reported to the
 * driver (paced_run steps by it) */
TEST(RequestEngineTest, AMinimumTimeSpacesTheSweepsFromStartToStart)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 200 * kMs);
    const std::vector<Asked> sent = paced_run(&e, 1000 * kMs, single_pid_reply);
    ASSERT_EQ(sent.size(), 15u) << "five sweeps of three in 1 s";
    const uint8_t order[3] = {0x0C, 0x0D, 0x0F};
    for (size_t k = 0; k < sent.size(); ++k) {
        EXPECT_EQ(sent[k].request[1], order[k % 3]) << "request " << k;
        EXPECT_EQ(sent[k].at, int64_t(k / 3) * 200 * kMs +
                                  int64_t(k % 3) * ECUIO_ENGINE_GAP_US)
            << "request " << k;
    }
}

/* A sweep that takes longer than the floor waits for nothing: one
 * after another at the engine's own pace, as with no floor at all */
TEST(RequestEngineTest, ASweepLongerThanItsMinimumIsNotHeldBack)
{
    ecuio_request_engine floored;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&floored, {0x0C, 0x0D, 0x0F, 0x10}));
    focus_all(&floored, 50 * kMs); /* a sweep takes 80 ms */
    ecuio_request_engine plain;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&plain, {0x0C, 0x0D, 0x0F, 0x10}));
    focus_all(&plain, 0);
    const std::vector<Asked> a =
        paced_run(&floored, 500 * kMs, single_pid_reply);
    const std::vector<Asked> b = paced_run(&plain, 500 * kMs, single_pid_reply);
    ASSERT_EQ(a.size(), b.size());
    for (size_t k = 0; k < a.size(); ++k) {
        EXPECT_EQ(a[k].at, b[k].at) << "request " << k;
        EXPECT_EQ(a[k].request, b[k].request) << "request " << k;
    }
}

/* Eight picks at six PIDs a pack: a sweep is two requests, whose rows
 * run past the eight, and the count starts over, so every sweep asks
 * every pick, sweeps start a floor apart and the wait falls only
 * between requests */
TEST(RequestEngineTest, AMinimumTimeCountsAPacksRowsAndEveryPickIsAsked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(packing_engine(&e));
    (void)packing_turns(&e, 200, packed_from(fake_reply));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    std::set<uint8_t> picks;
    for (uint32_t i = 0; i < e.count && picks.size() < 8; ++i) {
        if (ecuio_engine_packable(&e, 0, i)) {
            keep[i] = 1;
            picks.insert(e.requests[i].bytes[1]);
        }
    }
    ASSERT_EQ(picks.size(), 8u);
    ecuio_engine_focus(&e, keep, 300 * kMs);
    ASSERT_NO_FATAL_FAILURE(clock_over(&e));
    const std::vector<Asked> sent =
        paced_run(&e, 3000 * kMs, packed_from(fake_reply));
    ASSERT_EQ(sent.size(), 20u) << "ten sweeps of two requests";
    for (size_t k = 0; k < sent.size(); k += 2) {
        EXPECT_EQ(sent[k].at, int64_t(k / 2) * 300 * kMs) << "sweep " << k / 2;
        EXPECT_EQ(sent[k + 1].at - sent[k].at, ECUIO_ENGINE_GAP_US);
        EXPECT_EQ(sent[k].request.size(), 7u) << "six PIDs in the pack";
        std::set<uint8_t> asked;
        for (size_t r = 0; r < 2; ++r) {
            asked.insert(sent[k + r].request.begin() + 1,
                         sent[k + r].request.end());
        }
        EXPECT_EQ(asked, picks) << "sweep " << k / 2;
    }
}

/* Only the rotation waits: a scanned DID goes out in the gap, the next
 * event says so, and the sweep after it starts on its own time */
TEST(RequestEngineTest, AOneShotGoesOutInAMinimumTimesGap)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D}));
    focus_all(&e, 1000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    const uint8_t a[4] = {0x41, 0x0C, 0x12, 0x34};
    (void)ecuio_engine_on_frame(&e, 0, a, sizeof a, 4 * kMs, nullptr);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 20 * kMs), 1);
    ecuio_engine_sent(&e, 0, 1, true, 20 * kMs);
    const uint8_t b[3] = {0x41, 0x0D, 0x20};
    (void)ecuio_engine_on_frame(&e, 0, b, sizeof b, 24 * kMs, nullptr);

    /* The gap, with only the rotation left to send */
    EXPECT_EQ(ecuio_engine_due(&e, 0, 40 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 40 * kMs), 960 * kMs);

    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 40 * kMs), 0)
        << "the scanned DID is due now";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 40 * kMs), ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, ECUIO_ENGINE_ONCE, true, 40 * kMs);
    const uint8_t refused[3] = {0x7F, 0x22, 0x31};
    (void)ecuio_engine_on_frame(&e, 0, refused, sizeof refused, 44 * kMs,
                                nullptr);

    EXPECT_EQ(ecuio_engine_due(&e, 0, 60 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 60 * kMs), 940 * kMs)
        << "the rotation waits on";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 999 * kMs), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 1000 * kMs), 0);
}

/* The wait lasts as long as its focus: the same time again (a scanned
 * DID joined) keeps a wait under way, another ends it, and the focus's
 * end frees the rotation and sets the minimum back to none */
TEST(RequestEngineTest, AMinimumTimeLastsAsLongAsItsFocus)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C}));
    focus_all(&e, 5000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), ECUIO_ENGINE_NONE);

    focus_all(&e, 5000 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), ECUIO_ENGINE_NONE)
        << "the same time keeps the wait";

    focus_all(&e, 100 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 0)
        << "a new time ends it";
    ecuio_engine_sent(&e, 0, 0, true, ECUIO_ENGINE_WAIT_US);
    (void)ecuio_engine_expire(&e, 0, 2 * ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US - 20 * kMs),
              ECUIO_ENGINE_NONE)
        << "the new time, from that sweep's start";

    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.min_sweep_us, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US), 0)
        << "no focus, no minimum";
}

/* A focus taken in the middle of a sweep starts a new one: every row
 * of it is asked before the next wait */
TEST(RequestEngineTest, AFocusInTheMiddleOfASweepStartsANewOne)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    focus_all(&e, 500 * kMs);
    int64_t now = 100 * kMs;
    for (int k = 0; k < 3; ++k) {
        const int32_t r = ecuio_engine_due(&e, 0, now);
        ASSERT_NE(r, ECUIO_ENGINE_NONE) << "row " << k << " of the new sweep";
        ecuio_engine_sent(&e, 0, r, true, now);
        now += ECUIO_ENGINE_WAIT_US;
        (void)ecuio_engine_expire(&e, 0, now);
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE)
        << "its three rows asked, the wait begins";
}

/* A pick refused and dropped after it was asked still leaves the sweep
 * to ask every pick left, and the next sweep counts from the first
 * request's time */
TEST(RequestEngineTest, APickDroppedInASweepStillLeavesTheRestAsked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    e.probe = true; /* an explicit refusal drops its request */
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    const uint8_t refused[3] = {0x7F, 0x01, 0x12};
    (void)ecuio_engine_on_frame(&e, 0, refused, sizeof refused, 4 * kMs,
                                nullptr);
    ASSERT_NE(e.requests[0].dropped, 0) << "0C dropped";

    int64_t now = 20 * kMs;
    for (const int32_t row : {1, 2}) {
        ASSERT_EQ(ecuio_engine_due(&e, 0, now), row)
            << "row " << row << " in the sweep the drop happened in";
        ecuio_engine_sent(&e, 0, row, true, now);
        const uint8_t ok[4] = {0x41, e.requests[row].bytes[1], 0x12, 0x34};
        (void)ecuio_engine_on_frame(&e, 0, ok, sizeof ok, now + 4 * kMs,
                                    nullptr);
        now += ECUIO_ENGINE_GAP_US;
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 500 * kMs - now)
        << "the next sweep is a minimum after the first request";
    EXPECT_EQ(ecuio_engine_due(&e, 0, 500 * kMs), 1);
}

/* A row dropped before it was asked in this sweep isn't waited for:
 * the sweep ends once the rows left have been asked */
TEST(RequestEngineTest, ARowDroppedBeforeItsTurnShortensTheSweep)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x0D, 0x0F}));
    focus_all(&e, 500 * kMs);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 0), 0);
    ecuio_engine_sent(&e, 0, 0, true, 0);
    (void)ecuio_engine_expire(&e, 0, ECUIO_ENGINE_WAIT_US);
    ecuio_engine_drop(&e, 2, ECUIO_DROP_BITMAP, 0);
    EXPECT_EQ(ecuio_engine_due(&e, 0, ECUIO_ENGINE_WAIT_US), 1);
    ecuio_engine_sent(&e, 0, 1, true, ECUIO_ENGINE_WAIT_US);
    (void)ecuio_engine_expire(&e, 0, 2 * ECUIO_ENGINE_WAIT_US);
    EXPECT_EQ(ecuio_engine_due(&e, 0, 2 * ECUIO_ENGINE_WAIT_US),
              ECUIO_ENGINE_NONE)
        << "0C and 0D asked, 0F gone: the wait begins";
}

/* No focus, no minimum: the rotation runs at the engine's own pace */
TEST(RequestEngineTest, ASessionWithNoFocusHasNoMinimumTime)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0C, 0x05}));
    EXPECT_EQ(e.min_sweep_us, 0);
    const std::vector<Asked> sent = paced_run(&e, 400 * kMs, single_pid_reply);
    ASSERT_EQ(sent.size(), 20u) << "a request every 20 ms";
    for (size_t k = 0; k < sent.size(); ++k) {
        EXPECT_EQ(sent[k].at, int64_t(k) * ECUIO_ENGINE_GAP_US);
    }
}

/* A request out when a focus parks it is still answered: its reply
 * correlates by the table, and the next request is a pick */
TEST(RequestEngineTest, ARequestOutWhenParkedIsStillAnswered)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B, 0x0C}));
    EXPECT_EQ(send_next(&e, 0, 0), 0x0Bu);
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[1] = 1; /* 0x0C */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.chans[0].outstanding, 0) << "0x0B stays out";
    const uint8_t reply[3] = {0x41, 0x0B, 0x64};
    EXPECT_EQ(
        ecuio_engine_on_frame(&e, 0, reply, sizeof(reply), 4 * kMs, nullptr),
        ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(send_next(&e, 0, 4 * kMs + ECUIO_ENGINE_GAP_US), 0x0Cu);
}

/* A duplicate pair judged while a focus is on (a re-walk after a
 * recovery): the demoted PID's class is remembered, not applied, so a
 * pick keeps running fast until the focus ends. PID p's bit in the
 * answered bitmaps is 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8. */
TEST(RequestEngineTest, ADuplicateJudgedUnderAFocusStaysFastUntilItEnds)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(obd2_walked(&e, kTestCar, false));
    uint32_t demoted = UINT32_MAX;
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    for (uint32_t i = 0; i < e.count; ++i) {
        const uint8_t pid = e.requests[i].bytes[1];
        if (pid == 0x23 || pid == 0x59) {
            keep[i] = 1;
        }
        if (pid == 0x59) {
            demoted = i;
        }
    }
    ASSERT_NE(demoted, UINT32_MAX);
    ecuio_engine_focus(&e, keep);
    for (const uint8_t pid : {uint8_t{0x23}, uint8_t{0x59}}) {
        e.chans[0].supported[(pid - 1u) / 8u] |= 0x80u >> ((pid - 1u) % 8u);
    }
    ecuio_engine_judge_duplicates(&e, 0);
    EXPECT_EQ(e.requests[demoted].slow_default, 1u) << "judged slow";
    EXPECT_EQ(e.requests[demoted].slow, 0u) << "but a pick runs fast";
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.requests[demoted].slow, 1u) << "its class once the focus ends";
}

/* A request added under a focus (a scanned DID found) starts parked and
 * marks the focus for resolving again (focus_joined), which the next
 * focus call clears; it joins the rotation when the focus ends */
TEST(RequestEngineTest, ARequestAddedUnderAFocusStartsParked)
{
    ecuio_request_engine e;
    ASSERT_NO_FATAL_FAILURE(one_channel_of(&e, {0x0B}));
    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[0] = 1;
    ecuio_engine_focus(&e, keep);
    EXPECT_FALSE(e.focus_joined);
    const uint8_t req[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, req, 3), ecuio_engine_add_result::added);
    EXPECT_EQ(e.requests[1].parked, 1u);
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_TRUE(e.focus_joined);
    ecuio_engine_focus(&e, keep);
    EXPECT_FALSE(e.focus_joined) << "the focus resolved again";
    EXPECT_EQ(e.requests[1].parked, 1u) << "no pick names it";
    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.requests[1].parked, 0u);
    EXPECT_EQ(e.chans[0].requests, 2u);
}

/* A focus moves only the generations of the channels whose count it
 * changed: a channel that keeps every request keeps its generation, so
 * its support report isn't published again, and a swap of picks inside
 * a channel that keeps its count moves nothing */
TEST(RequestEngineTest, AFocusMovesOnlyTheChangedChannelsGenerations)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    for (const uint8_t pid : {uint8_t{0x0B}, uint8_t{0x0C}}) {
        const uint8_t req[2] = {0x01, pid};
        ASSERT_EQ(ecuio_engine_add(&e, 0, req, 2),
                  ecuio_engine_add_result::added);
    }
    const uint8_t other[2] = {0x01, 0x0D};
    ASSERT_EQ(ecuio_engine_add(&e, 1, other, 2),
              ecuio_engine_add_result::added);
    const uint32_t g0 = e.chans[0].generation;
    const uint32_t g1 = e.chans[1].generation;

    uint8_t keep[ECUIO_PROFILE_MAX_REQUESTS] = {};
    keep[0] = 1; /* channel 0's 0x0B */
    keep[2] = 1; /* channel 1's 0x0D */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_GT(e.chans[0].generation, g0) << "channel 0 lost a request";
    EXPECT_EQ(e.chans[1].generation, g1) << "channel 1 kept its one";

    const uint32_t g0_focused = e.chans[0].generation;
    keep[0] = 0;
    keep[1] = 1; /* 0x0C in 0x0B's place */
    ecuio_engine_focus(&e, keep);
    EXPECT_EQ(e.requests[0].parked, 1u);
    EXPECT_EQ(e.requests[1].parked, 0u);
    EXPECT_EQ(e.chans[0].generation, g0_focused) << "the count stood";
    EXPECT_EQ(e.chans[1].generation, g1);

    ecuio_engine_unfocus(&e);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_GT(e.chans[0].generation, g0_focused);
    EXPECT_EQ(e.chans[1].generation, g1);
}
