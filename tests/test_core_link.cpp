// File/layer: tests/test_core_link.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;


namespace {

/* What a frame decodes to, by direct calls, set by the frame callback
 * and checked by the values callback that follows it on the worker */
struct DirectCheck {
    const ecuio_signal_set_t* set = nullptr;
    std::vector<SeenValue> expected;
    std::atomic<int> frames{0};
    std::atomic<int> batches{0};
    std::atomic<int> mismatches{0};
    std::atomic<int> computed{0}; /* computed values after a frame's */
    std::atomic<int> missing{0}; /* a frame that decoded, no batch */
    bool pending = false;
};

void direct_frame_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<DirectCheck*>(user_data);
    if (c->pending)
        ++c->missing;
    c->expected.clear();
    const uint16_t len = frame->data_length;
    char text[ECUIO_DECODE_TEXT_MAX];
    for (uint32_t i = 0; i < c->set->count; ++i) {
        const ecuio_signal_def_t& d = c->set->defs[i];
        if (!ecuio_def_applies_to_frame(&d, frame))
            continue;
        SeenValue s;
        s.id = d.id;
        if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            if (ecuio_decode_text(&d, frame->data, len, text, sizeof(text)) !=
                ECUIO_DECODE_OK)
                continue;
            s.text = text;
        } else if (ecuio_decode_signal(&d, frame->data, len, &s.value) !=
                   ECUIO_DECODE_OK) {
            continue;
        }
        c->expected.push_back(s);
    }
    c->pending = !c->expected.empty();
    ++c->frames;
}

void direct_values_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* c = static_cast<DirectCheck*>(user_data);
    ++c->batches;
    /* The frame's own values, then any computed one its frame fed */
    bool same = c->pending && batch->count >= c->expected.size();
    for (uint32_t i = 0; same && i < c->expected.size(); ++i) {
        const ecuio_value_t& v = batch->values[i];
        const SeenValue& e = c->expected[i];
        same = v.signal_id == e.id && v.value == e.value &&
               (v.text == nullptr ? e.text.empty() : e.text == v.text);
    }
    for (uint32_t i = static_cast<uint32_t>(c->expected.size());
         same && i < batch->count; ++i) {
        same = false;
        for (uint32_t d = 0; d < c->set->count; ++d) {
            const ecuio_signal_def_t& def = c->set->defs[d];
            if (def.id == batch->values[i].signal_id &&
                (def.flags & ECUIO_DEF_FLAG_COMPUTED)) {
                same = true;
                ++c->computed;
            }
        }
    }
    if (!same)
        ++c->mismatches;
    c->pending = false;
}

} // namespace

/* Every batch equals the direct decode of its frame, over the vehicle
 * fixture under obd2 and the mock's drive cycle under its own set, then
 * the computed values it fed: none under obd2, the mock's boost after
 * every frame */
TEST(CoreTest, ValuesMatchDirectDecodesOfEveryFrame)
{
    const std::string fixture =
        "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
        "/2026-10-25-engine-start?profile=obd2&speed=max";
    struct Case {
        const char* uri;
        int frames;
        bool computed; /* one computed value a batch */
    };
    for (const Case& k : {Case{fixture.c_str(), 7074, false},
                          Case{"mock://direct", 20, true}}) {
        SCOPED_TRACE(k.uri);
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(k.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        DirectCheck c;
        c.set = ecuio_session_signal_set(session);
        ASSERT_NE(c.set, nullptr);
        EXPECT_EQ(ecuio_register_frame_callback(session, direct_frame_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_values_callback(session, direct_values_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return c.frames.load() >= k.frames; }, 10000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << c.frames.load() << " frames";
        EXPECT_GT(c.batches.load(), 0);
        EXPECT_EQ(c.mismatches.load(), 0);
        EXPECT_EQ(c.computed.load(), k.computed ? c.batches.load() : 0);
        EXPECT_EQ(c.missing.load(), 0) << "a frame that decoded with no batch";
    }
}

/* ---- Link health (link_health.h), session-free: the rules every worker
 * that judges a link shares. The J2534 worker's use of them is HARNESS
 * (J2534HealthTest). */

/* The backoff step: from the base every wait grows and none exceeds the
 * cap, including the step that lands on it, and at the cap it stays. A
 * worker test can't see this; reaching the cap takes over 6 s of
 * sustained failure. */
TEST(CoreTest, LinkBackoffStepDoublesToCapWithoutOvershoot)
{
    uint32_t ms = LINK_BACKOFF_BASE_MS;
    uint32_t steps = 0;
    while (ms < LINK_BACKOFF_CAP_MS) {
        const uint32_t next = link_backoff_next(ms);
        EXPECT_GT(next, ms);
        EXPECT_LE(next, LINK_BACKOFF_CAP_MS) << "overshoot after " << ms;
        ms = next;
        ASSERT_LT(++steps, 32u) << "never reached the cap";
    }
    EXPECT_EQ(ms, LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(LINK_BACKOFF_CAP_MS), LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(LINK_BACKOFF_CAP_MS - 1), LINK_BACKOFF_CAP_MS);
    EXPECT_EQ(link_backoff_next(UINT32_MAX), LINK_BACKOFF_CAP_MS)
        << "doubling must not wrap";
}

/* A hard read failure is one episode however long it lasts. A clean read
 * ends the backoff without ending the failure, and the next hard read
 * backs off from the base again, still in the same episode. A matched
 * reply ends it. */
TEST(CoreTest, LinkHardReadIsOneEpisodeAndBacksOffFromTheBase)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 1u);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS);

    h.backoff_ms = link_backoff_next(h.backoff_ms); /* the worker's sleep */
    link_on_read(h, true, false, false);
    EXPECT_EQ(episodes.load(), 1u) << "the same failure, no new episode";
    EXPECT_EQ(h.backoff_ms, 2 * LINK_BACKOFF_BASE_MS) << "left to grow";

    link_on_read(h, false, false, false);
    EXPECT_EQ(h.backoff_ms, 0u) << "a clean read ends the backoff";
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "but not the failure";

    link_on_read(h, true, false, false);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS) << "restarted from the base";
    EXPECT_EQ(episodes.load(), 1u);

    h.recovered = false;
    link_on_read(h, false, true, false);
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_EQ(h.backoff_ms, 0u);
    EXPECT_TRUE(h.recovered) << "the probe resumes on recovery";
    EXPECT_EQ(episodes.load(), 1u);
}

/* Quiet is a run of LINK_HEALTH_QUIET_RUN, with no episode: reads count
 * only when told to (passive, or nothing left to ask), request timeouts
 * always, and a matched reply starts the run over. */
TEST(CoreTest, LinkQuietRunGoesQuietWithoutAnEpisode)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, false);
    }
    EXPECT_EQ(published.load(), ECUIO_OK)
        << "empty reads between requests mean nothing";

    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    EXPECT_EQ(published.load(), ECUIO_OK) << "one short of the run";
    link_on_read(h, false, true, true);
    EXPECT_EQ(h.quiet_runs, 0u) << "a reply starts the run over";

    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(h);
    }
    EXPECT_EQ(published.load(), ECUIO_OK);
    link_on_request_timeout(h);
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(episodes.load(), 0u) << "quiet isn't a failure";
}

/* An idle pass of a session that asks only its picks, with nothing
 * picked, is no quiet read: the run a quiet read was counting starts
 * over, and a timeout quiet reads alone made ends. One a request's wait
 * ran out for stays, pass after pass, until a reply. A hard failure or
 * failing writes are the adapter's and stay. */
TEST(CoreTest, LinkAnUnpickedPassCountsNoQuietAndEndsOnlyAQuietReadsTimeout)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
    }
    EXPECT_EQ(published.load(), ECUIO_OK);
    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    link_on_unpicked(h);
    EXPECT_EQ(h.quiet_runs, 0u) << "the run starts over";
    link_on_read(h, false, false, true);
    EXPECT_EQ(published.load(), ECUIO_OK)
        << "one quiet read after it is no run";

    /* Picks all dropped: nothing left to ask, and the reads count */
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    h.recovered = false;
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_OK)
        << "nothing asked, nothing unanswered";
    EXPECT_TRUE(h.recovered);
    EXPECT_EQ(episodes.load(), 0u);

    /* Requests nothing answered: the walk's, identification's or
     * diagnostics', or picks since taken away */
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(h);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    h.recovered = false;
    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
        ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "pass " << i << ": something asked went unanswered";
    }
    EXPECT_FALSE(h.recovered) << "or the worker would ask it all again";

    /* One unanswered request is enough to keep a quiet read's timeout */
    link_on_read(h, false, true, false);
    ASSERT_EQ(published.load(), ECUIO_OK);
    link_on_request_timeout(h);
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);

    /* A reply ends it, and what timed out before it no longer counts */
    link_on_read(h, false, true, false);
    ASSERT_EQ(published.load(), ECUIO_OK);
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_EQ(episodes.load(), 0u);

    std::atomic<int32_t> published2{ECUIO_OK};
    std::atomic<uint32_t> episodes2{0};
    link_health w(published2, episodes2);
    link_on_write(w, false);
    ASSERT_EQ(published2.load(), ECUIO_ERR_REQUEST_FAILED);
    for (uint32_t i = 0; i < 2 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(w);
    }
    EXPECT_FALSE(w.unanswered) << "the state is the writes'";
    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(w);
    }
    EXPECT_EQ(published2.load(), ECUIO_ERR_REQUEST_FAILED)
        << "only a sent write ends it";
    EXPECT_EQ(w.backoff_ms, LINK_BACKOFF_BASE_MS)
        << "the backoff is the writes'";
}

/* An unpicked pass is a clean read. A hard failure ends once a quiet
 * run's worth of them came in a row, as any session's does after its
 * adapter is back: to OK when nothing went unanswered since the last
 * reply, to a timeout when something did. A hard read between them
 * starts the count over in the same episode, and under failing writes
 * the first steps down to the writes' state. */
TEST(CoreTest, LinkAnUnpickedPassEndsAHardFailureOnceTheAdapterReads)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_read(h, true, false, false);
    ASSERT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS);
    link_on_unpicked(h);
    EXPECT_EQ(h.backoff_ms, 0u) << "the adapter read";
    for (uint32_t i = 1; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
        ASSERT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE)
            << "pass " << i + 1;
    }
    /* A hard read: the count starts over, the episode stands */
    link_on_read(h, true, false, false);
    for (uint32_t i = 0; i + 1 < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
        ASSERT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE)
            << "pass " << i + 1 << " after the hard read";
    }
    h.recovered = false;
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_OK) << "nothing had gone unanswered";
    EXPECT_TRUE(h.recovered) << "the walk and the rescan follow";
    EXPECT_EQ(episodes.load(), 1u);

    /* The same with a request unanswered before the adapter failed */
    link_on_request_timeout(h);
    link_on_read(h, true, false, false);
    ASSERT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    h.recovered = false;
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
    }
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_FALSE(h.recovered);
    EXPECT_EQ(episodes.load(), 2u);
    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_unpicked(h);
    }
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT) << "until a reply";
    link_on_read(h, false, true, false);
    EXPECT_EQ(published.load(), ECUIO_OK);

    /* Writes failing under the hard failure: the adapter reads, so the
     * state is the writes', at the first clean pass */
    link_on_read(h, true, false, false);
    link_on_write(h, false);
    ASSERT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes.load(), 3u) << "no new episode for the step down";
    link_on_write(h, true);
    EXPECT_EQ(published.load(), ECUIO_OK);
}

/* A reply while the write latch is set changes no state, but it is a
 * reply: what timed out before it no longer stands behind a timeout */
TEST(CoreTest, LinkAReplyUnderTheWriteLatchStillAnswers)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_request_timeout(h);
    ASSERT_TRUE(h.unanswered);
    link_on_write(h, false);
    ASSERT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    link_on_read(h, false, true, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED)
        << "the latch is the writes'";
    EXPECT_FALSE(h.unanswered);
    link_on_write(h, true);
    ASSERT_EQ(published.load(), ECUIO_OK);

    /* So a timeout quiet reads alone then make ends with the picks */
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_read(h, false, false, true);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    link_on_unpicked(h);
    EXPECT_EQ(published.load(), ECUIO_OK);
}

/* A failed write latches REQUESTS_FAILED as one episode with a backoff.
 * While it's latched, timeouts don't count toward quiet and replies don't
 * clear it; only a sent write does. */
TEST(CoreTest, LinkFailedWriteLatchesUntilAWriteGoesThrough)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_write(h, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes.load(), 1u);
    EXPECT_EQ(h.backoff_ms, LINK_BACKOFF_BASE_MS);

    link_on_write(h, false);
    EXPECT_EQ(episodes.load(), 1u);
    for (uint32_t i = 0; i < 2 * LINK_HEALTH_QUIET_RUN; ++i) {
        link_on_request_timeout(h);
    }
    EXPECT_EQ(h.quiet_runs, 0u) << "the state is the writes'";
    link_on_read(h, false, true, true);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED)
        << "a reply doesn't clear the latch";

    h.recovered = false;
    link_on_write(h, true);
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_EQ(h.backoff_ms, 0u);
    EXPECT_TRUE(h.recovered);
    EXPECT_EQ(episodes.load(), 1u);
}

/* A hard read outranks a failing write and counts its own episode; a
 * write failing under it changes nothing; a clean read under the latch
 * steps down to REQUESTS_FAILED without a new episode. */
TEST(CoreTest, LinkHardReadOutranksTheLatchAndStepsDownToIt)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    link_on_write(h, false);
    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 2u);

    link_on_write(h, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_EQ(episodes.load(), 2u);

    link_on_read(h, false, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes.load(), 2u) << "a step down, not a new failure";
}

/* The session's atomics are written only on a transition, so a reader
 * polling them never sees churn from a steady state. */
TEST(CoreTest, LinkPublishesOnlyTransitions)
{
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);

    published.store(-99); /* a marker: a store would overwrite it */
    link_on_read(h, false, true, false);
    link_on_write(h, true);
    EXPECT_EQ(published.load(), -99) << "OK to OK is no transition";
    link_on_read(h, true, false, false);
    EXPECT_EQ(published.load(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
}

/* The decode's index (def_index.h) finds, for any frame, exactly the
 * defs ecuio_def_applies_to_frame takes, in the set's order: sets that
 * mix unkeyed defs and every key shape on a few IDs, keys shared
 * between defs, against frames too short for a key, error reports and
 * IDs no def listens on. The bytes come from a small alphabet so keys
 * match often. */
TEST(DefIndexTest, FindsExactlyTheDefsThatApplyInTheSetsOrder)
{
    std::mt19937 rng(20261031u);
    const auto pick = [&rng](uint32_t n) {
        return static_cast<uint32_t>(rng() % n);
    };
    const uint32_t ids[] = {0x100u, 0x7E8u, 0x7E9u};
    const uint8_t alphabet[] = {0x00u, 0x01u, 0x41u, 0x0Cu};
    uint64_t found_total = 0;
    for (int round = 0; round < 200; ++round) {
        std::vector<ecuio_signal_def_t> defs(1 + pick(40));
        for (ecuio_signal_def_t& d : defs) {
            d = ecuio_signal_def_t{};
            d.message_id = ids[pick(3)];
            d.mux_length = static_cast<uint8_t>(pick(4));
            if (d.mux_length > 0) {
                d.mux_offset = static_cast<uint8_t>(pick(3));
                for (uint8_t b = 0; b < d.mux_length; ++b) {
                    d.mux_key = (d.mux_key << 8) | alphabet[pick(4)];
                }
            }
        }
        ecuio_def_index index;
        ecuio_def_index_build(index, defs.data(),
                              static_cast<uint32_t>(defs.size()));
        std::vector<uint32_t> found(defs.size());
        for (int f = 0; f < 50; ++f) {
            ecuio_frame_t frame{};
            frame.arbitration_id = pick(8) == 0 ? 0x123u : ids[pick(3)];
            frame.flags = pick(10) == 0 ? ECUIO_FRAME_FLAG_ERROR : 0u;
            frame.data_length = static_cast<uint16_t>(pick(7));
            for (uint16_t b = 0; b < frame.data_length; ++b) {
                frame.data[b] = alphabet[pick(4)];
            }
            std::vector<uint32_t> want;
            for (uint32_t i = 0; i < defs.size(); ++i) {
                if (ecuio_def_applies_to_frame(&defs[i], &frame)) {
                    want.push_back(i);
                }
            }
            const uint32_t n = ecuio_def_index_find(index, frame, found.data());
            ASSERT_EQ(std::vector<uint32_t>(found.begin(), found.begin() + n),
                      want)
                << "round " << round << ", frame " << f;
            found_total += n;
        }
    }
    EXPECT_GT(found_total, 2000u) << "the frames must match defs often";
}

/* The surrogate's session logs (surrogate_log_names.h): each kind's name
 * matched exactly, so pruning TEMP deletes nothing else */
TEST(CoreTest, SurrogateLogNamesAreMatchedExactly)
{
    EXPECT_TRUE(
        sur_log_is_kept_name(L"ecuio-surrogate.20261101-001100-842.log"));
    EXPECT_FALSE(sur_log_is_kept_name(L"ecuio-surrogate.log"));
    EXPECT_FALSE(
        sur_log_is_kept_name(L"ecuio-surrogate.20261101-001100-842.log.bak"));
    EXPECT_FALSE(
        sur_log_is_kept_name(L"ecuio-surrogate.2026110-001100-842.log"));

    EXPECT_TRUE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1.log"));
    EXPECT_TRUE(sur_log_is_per_spawn_name(
        L"ecuio-surrogate-4294967295-2147483647.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(
        L"ecuio-surrogate-42949672951-1.log")); /* 11 digits */
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate--1.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1.log.txt"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-30584-1-2.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate-x-1.log"));
    EXPECT_FALSE(sur_log_is_per_spawn_name(L"ecuio-surrogate.log"));
    EXPECT_FALSE(
        sur_log_is_per_spawn_name(L"ecuio-surrogate.20261101-001100-842.log"));
    EXPECT_FALSE(
        sur_log_is_per_spawn_name(L"ecuio-elm327.20261101-001100-842.log"));
}

/* Past the newest SUR_LOG_KEEP, the oldest go first, by last write, not
 * by name: a per-spawn name's PID sorts in no time order */
TEST(CoreTest, SurrogateLogExcessIsTheOldestByLastWrite)
{
    std::vector<SurLogFile> files;
    for (uint64_t i = 0; i < SUR_LOG_KEEP + 3; ++i) {
        /* names counting down while the writes count up */
        files.push_back(SurLogFile{L"ecuio-surrogate-" +
                                       std::to_wstring(900 - i) + L"-1.log",
                                   1000 + i});
    }
    const std::vector<std::wstring> gone = sur_log_excess(files, SUR_LOG_KEEP);
    ASSERT_EQ(gone.size(), 3u);
    EXPECT_EQ(gone[0], L"ecuio-surrogate-900-1.log");
    EXPECT_EQ(gone[1], L"ecuio-surrogate-899-1.log");
    EXPECT_EQ(gone[2], L"ecuio-surrogate-898-1.log");

    EXPECT_TRUE(sur_log_excess(std::vector<SurLogFile>(
                                   files.begin(), files.begin() + SUR_LOG_KEEP),
                               SUR_LOG_KEEP)
                    .empty());
    /* the same write time: the name decides, so a run is the same each time */
    const std::vector<std::wstring> tie =
        sur_log_excess({{L"b", 5}, {L"a", 5}, {L"c", 9}}, 2);
    ASSERT_EQ(tie.size(), 1u);
    EXPECT_EQ(tie[0], L"a");
}

// ---- VIN decode (ADR-0043): 49 CFR 565.15 and NHTSA vPIC's codes. The
// VINs are made up around "FAKE", their check digits worked out by the
// regulation's Tables III and IV, but for Table VI's own sample.

namespace {

ecuio_vin_info_t vin_decoded(const char* vin, uint32_t latest = 0)
{
    ecuio_vin_info_t info;
    std::memset(&info, 0xA5, sizeof info);
    EXPECT_EQ(ecuio_vin_decode(vin, latest, &info), ECUIO_OK) << vin;
    return info;
}

} // namespace

TEST(VinTest, TheRegulationsSampleDecodes)
{
    // 565.15(c)(5), Table VI: check digit 4; position 7 is a 9, so its
    // year code 5 is 2005
    const ecuio_vin_info_t v = vin_decoded("1G4AH59H45G118341");
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES);
    EXPECT_EQ(v.check_digit, '4');
    EXPECT_EQ(v.model_year, 2005);
    EXPECT_STREQ(v.wmi, "1G4");
    EXPECT_EQ(v.wmi_length, 3);
    EXPECT_STREQ(ecuio_vin_make(v.wmi), "Buick");
    for (uint32_t r : v.reserved) {
        EXPECT_EQ(r, 0u);
    }
}

TEST(VinTest, PositionSevenPlacesTheYearInItsCycle)
{
    // Table VII's note: a letter in 7 is 2010-2039, a digit 1980-2009
    EXPECT_EQ(vin_decoded("1FAFAKE10M0000001").model_year, 2021);
    EXPECT_EQ(vin_decoded("1FAAB12C5FAKE0001").model_year, 1985);
    // Table VII's own rows, 2005 to 2039, against the decode
    const char* codes = "56789ABCDEFGHJKLMNPRSTVWXY123456789";
    for (int i = 0; codes[i] != '\0'; ++i) {
        char vin[] = "1FAFAKE1?M0000001";
        vin[9] = codes[i];
        const int year = 2005 + i;
        // The check digit doesn't matter to the year; position 7 picks
        // the cycle's half, a digit for the years before 2010
        vin[6] = year < 2010 ? '1' : 'E';
        vin[8] = '0';
        EXPECT_EQ(vin_decoded(vin).model_year, year) << vin;
    }
}

TEST(VinTest, ACodeThatIsNoYearHasNone)
{
    for (char c : {'0', 'U', 'Z'}) {
        char vin[] = "1FAFAKE10M0000001";
        vin[9] = c;
        EXPECT_EQ(vin_decoded(vin).model_year, 0) << c;
    }
}

TEST(VinTest, AYearPastTheLatestIsTheCyclesEarlierOne)
{
    // Y is 2030 by a letter in 7; past 2027, it is 2000
    ecuio_vin_info_t v = vin_decoded("1FAFAKE13Y0000001", 2027);
    EXPECT_EQ(v.model_year, 2000);
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES | ECUIO_VIN_YEAR_EARLIER);
    // Within the bound, or with none, it stays
    v = vin_decoded("1FAFAKE13Y0000001", 2030);
    EXPECT_EQ(v.model_year, 2030);
    EXPECT_EQ(v.flags, ECUIO_VIN_CHECK_MATCHES);
    EXPECT_EQ(vin_decoded("1FAFAKE13Y0000001").model_year, 2030);
    // A digit in 7 is already the earlier half
    EXPECT_EQ(vin_decoded("1FAAB12C5FAKE0001", 1980).model_year, 1985);
}

TEST(VinTest, TheCheckDigitIsComputedAndCompared)
{
    // Remainder 10 is an X
    ecuio_vin_info_t v = vin_decoded("1FAFAKE1XM0000006");
    EXPECT_EQ(v.check_digit, 'X');
    EXPECT_EQ(v.flags & ECUIO_VIN_CHECK_MATCHES, ECUIO_VIN_CHECK_MATCHES);
    // A wrong digit decodes the same, flagged: many markets need none
    v = vin_decoded("1FAFAKE18M0000001");
    EXPECT_EQ(v.check_digit, '0');
    EXPECT_EQ(v.flags & ECUIO_VIN_CHECK_MATCHES, 0u);
    EXPECT_EQ(v.model_year, 2021);
    EXPECT_STREQ(v.wmi, "1FA");
}

TEST(VinTest, ANineInPositionThreeIsASixCharacterCode)
{
    // 565.15(a): positions 1-3 and 12-14
    const ecuio_vin_info_t v = vin_decoded("1A9FAKE18MA596001");
    EXPECT_STREQ(v.wmi, "1A9596");
    EXPECT_EQ(v.wmi_length, 6);
    EXPECT_STREQ(ecuio_vin_make(v.wmi), "ACG");
    EXPECT_STREQ(ecuio_vin_make("1A9"), "");
}

TEST(VinTest, TheMakeIsVpicsForTheCode)
{
    EXPECT_STREQ(ecuio_vin_make("1FA"), "Ford");
    EXPECT_STREQ(ecuio_vin_make("JN1"), "Datsun, Infiniti or Nissan");
    EXPECT_STREQ(ecuio_vin_make("ZZZ"), "");
    EXPECT_STREQ(ecuio_vin_make("1F"), "");
    EXPECT_STREQ(ecuio_vin_make("1FAX"), "");
    EXPECT_STREQ(ecuio_vin_make("1fa"), "");
    EXPECT_STREQ(ecuio_vin_make(""), "");
    EXPECT_STREQ(ecuio_vin_make(nullptr), "");
    EXPECT_EQ(std::strncmp(ecuio_vin_source(), "vPICList_lite_", 14), 0);
}

TEST(VinTest, OnlyAVinIsDecoded)
{
    ecuio_vin_info_t v;
    for (const char* bad :
         {"1FAFAKE10M000000", "1FAFAKE10M00000011", "1faFAKE10M0000001",
          "1FAFAKE10M000000I", "1FAFAKE10M000000O", "1FAFAKE10M000000Q",
          "1FAFAKE10M00000-1", "", "FAKEJ2534VIN00001"}) {
        std::memset(&v, 0xA5, sizeof v);
        EXPECT_EQ(ecuio_vin_decode(bad, 0, &v), ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
        EXPECT_EQ(v.flags, 0u) << bad;
        EXPECT_EQ(v.wmi[0], '\0') << bad;
    }
    EXPECT_EQ(ecuio_vin_decode(nullptr, 0, &v), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_vin_decode("1FAFAKE10M0000001", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

// A packed reply decodes each def once: the defs keyed by a record's PID
// from their records, every other def on its ID (unkeyed, or keyed
// otherwise) from the reply whole, so a batch never holds more values
// than the set has defs
TEST(CoreTest, APackedReplyDecodesEachDefOnce)
{
    static ecuio_signal_def_t defs[3];
    std::memset(defs, 0, sizeof defs);
    defs[0].id = 1;
    defs[0].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[0].mux_length = 2;
    defs[0].mux_key = 0x410C;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].byte_order = ECUIO_BYTE_ORDER_BE;
    defs[0].start_byte = 2;
    defs[0].bit_length = 16;
    defs[0].scale = 0.25;
    std::strcpy(defs[0].name, "Rpm");
    defs[1].id = 2;
    defs[1].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[1].kind = ECUIO_VALUE_UINT;
    defs[1].bit_length = 8;
    defs[1].scale = 1;
    std::strcpy(defs[1].name, "First byte");
    defs[2].id = 3;
    defs[2].message_id = ECUIO_OBD2_ECU_ARB_ID;
    defs[2].mux_length = 1;
    defs[2].mux_key = 0x41;
    defs[2].kind = ECUIO_VALUE_UINT;
    defs[2].start_byte = 1;
    defs[2].bit_length = 8;
    defs[2].scale = 1;
    std::strcpy(defs[2].name, "Second byte");
    static ecuio_signal_set_t set;
    set = ecuio_signal_set_t{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("packed-each-once", &set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://p?profile=packed-each-once", &session),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);

    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x1A, 0xF8, 0x0D, 0x20});
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.count(), 1u);
    EXPECT_EQ(id_values(cap.batches[0]),
              (std::vector<std::pair<uint16_t, double>>{
                  {1, 1726.0}, {2, 65.0}, {3, 12.0}}))
        << "the record's def, then the others from the reply whole";
    EXPECT_EQ(unsplit(session), 0u);

    ecuio_session_destroy(session);
    (void)ecuio_profile_unregister("packed-each-once");
}

/* ADR-0056 section 2: the focus call's contract on a session that asks
 * nothing (the mock): taken before and after start, an id the set lacks
 * refused by name, a count of 0 taken, NULLs and a closed session
 * refused; a session with no set (raw) takes only a count of 0 */
TEST(CoreTest, SetFocusTakesTheSetsIdsAndRefusesTheRest)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->count, 2u);
    const uint16_t ids[2] = {set->defs[0].id, set->defs[1].id};
    EXPECT_EQ(ecuio_session_set_focus(nullptr, ids, 2),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 1), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 2), ECUIO_OK);
    const uint16_t stranger[2] = {ids[0], 65000};
    EXPECT_EQ(ecuio_session_set_focus(s, stranger, 2),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no signal with id 65000 in profile mock");
    EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 1), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, ids, 1), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);

    ecuio_session_t* raw = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=raw", &raw), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(raw, ids, 1), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus(raw, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(raw);
}

/* A pick given twice is picked once, at its first place, so a
 * recording's focus names each signal once */
TEST(CoreTest, APickGivenTwiceIsPickedOnceAtItsFirstPlace)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus-twice", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->count, 2u);
    const uint16_t a = set->defs[0].id;
    const uint16_t b = set->defs[1].id;
    const uint16_t ids[5] = {b, a, b, b, a};
    ASSERT_EQ(ecuio_session_set_focus(s, ids, 5), ECUIO_OK);
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, (std::vector<uint16_t>{b, a}));
    }
    ecuio_session_destroy(s);
}

