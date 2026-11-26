// File/layer: tests/test_core_clock_profile.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

// ---------------------------------------------------------------------------

namespace {

/* The shortest of n waits of d on w, each timed on the steady clock, and
 * their mean, in microseconds; each must be d or more */
struct Waited {
    int64_t shortest_us = INT64_MAX;
    int64_t mean_us = 0;
    int64_t median_us = 0; /* what a few preempted waits don't move */
};

Waited wait_n(ecuio_precise_wait& w, std::chrono::microseconds d, int n)
{
    Waited out;
    int64_t total = 0;
    std::vector<int64_t> each;
    for (int i = 0; i < n; ++i) {
        const auto from = std::chrono::steady_clock::now();
        w.sleep_for(d);
        const int64_t us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - from)
                .count();
        out.shortest_us = std::min(out.shortest_us, us);
        total += us;
        each.push_back(us);
    }
    out.mean_us = total / n;
    std::sort(each.begin(), each.end());
    out.median_us = each[each.size() / 2];
    return out;
}

} // namespace

/* Core's promise is that a wait never ends early, and on Windows (10
 * 1803 and later) that the high-resolution timer ends a 2 ms wait about
 * on time where a plain sleep ends it at the scheduler's 15.6 ms step.
 * Twenty precise waits and twenty plain ones are timed in the same
 * round, and the precise waits' median must be under 4 ms (a quarter of
 * the step, which an idle machine meets) or under half the plain
 * sleeps' (which a loaded machine meets, since load delays both alike);
 * a machine whose scheduler already steps at about 1 ms meets the first
 * with a plain median the second can't halve. Up to three rounds, the
 * best kept, since one preempted round proves nothing. Where there is
 * no such timer (Linux, macOS: a plain sleep, which CI's macOS runners
 * end 10 to 18 ms late and core can't shorten) the two are one path and
 * only the floor is judged. With the timer ignored in sleep_for (both
 * medians the step's) both bounds fail. */
TEST(CoreTest, APreciseWaitEndsOnTime)
{
    ecuio_precise_wait w;
    ecuio_precise_wait plain(false);
#ifdef _WIN32
    ASSERT_TRUE(w.high_resolution())
        << "CreateWaitableTimerExW refused the flag";
#endif
    Waited got;
    Waited got_plain;
    const auto on_time = [](const Waited& precise, const Waited& plain_sleep) {
        return precise.median_us < 4000 ||
               precise.median_us * 2 < plain_sleep.median_us;
    };
    for (int round = 0; round < 3; ++round) {
        got = wait_n(w, std::chrono::milliseconds(2), 20);
        got_plain = wait_n(plain, std::chrono::milliseconds(2), 20);
        ASSERT_GE(got.shortest_us, 2000) << "a wait ended early";
        if (!w.high_resolution() || on_time(got, got_plain)) {
            break;
        }
    }
    if (w.high_resolution()) {
        EXPECT_TRUE(on_time(got, got_plain))
            << "the precise waits ended on the plain sleep's step: "
            << got.median_us << " us against " << got_plain.median_us;
    }
    RecordProperty("mean_us", static_cast<int>(got.mean_us));
    RecordProperty("median_us", static_cast<int>(got.median_us));
    RecordProperty("plain_median_us", static_cast<int>(got_plain.median_us));
}

/* A wait until a time ends at a stop within a piece: a second's wait
 * stopped 20 ms in returns long before the second (judged against half
 * of it, which load stretches only toward the stop), and one never
 * stopped waits it out, never ending early */
TEST(CoreTest, APreciseWaitUntilEndsAtAStop)
{
    ecuio_precise_wait w;
    std::atomic<bool> stop{false};
    const auto from = std::chrono::steady_clock::now();
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
    });
    const bool stopped = w.sleep_until(from + std::chrono::seconds(1), stop);
    const auto took = std::chrono::steady_clock::now() - from;
    stopper.join();
    EXPECT_TRUE(stopped);
    EXPECT_LT(took, std::chrono::milliseconds(500))
        << "the stop waited the second out";

    std::atomic<bool> never{false};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(w.sleep_until(start + std::chrono::milliseconds(50), never));
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(50))
        << "a wait ended early";
    EXPECT_TRUE(w.sleep_until(start, stop))
        << "a stop already set ends it at once";
}

/* Where the flag is refused (built here by asking for none), the wait is
 * a plain sleep: at the system's step, never shorter than asked, and
 * nothing for a wait of nothing */
TEST(CoreTest, APreciseWaitsFallbackNeverEndsEarly)
{
    ecuio_precise_wait w(false);
    EXPECT_FALSE(w.high_resolution());
    const Waited got = wait_n(w, std::chrono::milliseconds(2), 5);
    EXPECT_GE(got.shortest_us, 2000) << "a wait ended early";
    RecordProperty("mean_us", static_cast<int>(got.mean_us));
    const auto from = std::chrono::steady_clock::now();
    w.sleep_for(std::chrono::microseconds(0));
    w.sleep_for(std::chrono::microseconds(-5));
    EXPECT_LT(std::chrono::steady_clock::now() - from,
              std::chrono::milliseconds(1));
}

TEST(CoreTest, ClockNeverGoesBackAndKeepsPace)
{
    uint64_t last = ecuio_clock_us();
    uint64_t slept = ecuio_clock_slept_us();
    for (int i = 0; i < 10000; ++i) {
        const uint64_t now = ecuio_clock_us();
        ASSERT_GE(now, last);
        last = now;
        const uint64_t s = ecuio_clock_slept_us();
        ASSERT_GE(s, slept);
        slept = s;
    }
    const uint64_t c0 = ecuio_clock_us();
    const auto s0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t elapsed = ecuio_clock_us() - c0;
    const auto steady = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - s0)
                            .count();
    EXPECT_GE(elapsed, 50000u);
    EXPECT_NEAR(static_cast<double>(elapsed), static_cast<double>(steady),
                1000.0);
}

/* It is the platform's clock that counts sleep, read between two reads of
 * that clock. Where that clock and the one that stops in sleep agree (a
 * machine that hasn't slept since it started, as a CI runner), this can't
 * tell them apart; a real sleep is the bench's (BENCH prep #27) */
TEST(CoreTest, ClockIsThePlatformsSleepCountingClock)
{
#if defined(__linux__)
    const auto platform = []() {
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
               static_cast<uint64_t>(ts.tv_nsec) / 1000u;
    };
    const uint64_t slack = 0;
#elif defined(__APPLE__)
    const auto platform = []() {
        return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000u;
    };
    const uint64_t slack = 0;
#else
    /* MSVC's steady_clock is QueryPerformanceCounter, to nanoseconds by
     * its own rounding: a microsecond either way */
    const auto platform = []() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    const uint64_t slack = 1;
#endif
    const uint64_t before = platform();
    const uint64_t clock = ecuio_clock_us();
    const uint64_t after = platform();
    EXPECT_LE(before, clock + slack);
    EXPECT_LE(clock, after + slack);
}

namespace {

struct ClockCapture {
    std::atomic<int> count{0};
    uint64_t stamp[8] = {}; /* read after close, which joins the worker */
    uint64_t clock[8] = {};
};

void clock_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    ClockCapture* c = static_cast<ClockCapture*>(user_data);
    const int i = c->count.fetch_add(1);
    if (i < 8) {
        c->clock[i] = ecuio_clock_us();
        c->stamp[i] = frame->timestamp_us;
    }
}

} // namespace

/* A session's time is ecuio_clock_us since its attach, the clock the app
 * stamps a recording's host_us with: each frame's clock less its stamp
 * lands between the reads around create, give or take its dispatch */
TEST(CoreTest, SessionTimeRunsOnTheClock)
{
    ecuio_session_t* session = nullptr;
    const uint64_t before = ecuio_clock_us();
    ASSERT_EQ(ecuio_session_create("mock://clock", &session), ECUIO_OK);
    const uint64_t after = ecuio_clock_us();
    ClockCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &clock_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&]() { return cap.count.load() >= 8; }, 10000);
    ecuio_session_close(session);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count.load(), 8);
    for (int i = 0; i < 8; ++i) {
        const uint64_t epoch = cap.clock[i] - cap.stamp[i];
        EXPECT_GE(epoch, before) << "frame " << i;
        EXPECT_LE(epoch, after + 20000) << "frame " << i;
    }
}

TEST(CoreTest, CloseIsIdempotentAndDestroyIsSafe)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://lifecycle", &session), ECUIO_OK);

    /* Double close: second returns the error code WITHOUT UB. close()
     * must not free the session (destroy does), or this second call is
     * a use-after-free that only survives on allocator luck */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_ERR_SESSION_CLOSED);

    /* Post-close API calls fail cleanly, not UB */
    EXPECT_EQ(ecuio_session_start(session), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, nullptr), ECUIO_ERR_SESSION_CLOSED);
    ecuio_frame_t frame = {};
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_session_receive(session, &frame, 0),
              ECUIO_ERR_NOT_IMPLEMENTED);

    /* destroy() is the freeing call; handle invalid after (free()). */
    EXPECT_EQ(ecuio_session_destroy(session), ECUIO_OK);

    /* free(nullptr) semantics */
    EXPECT_EQ(ecuio_session_destroy(nullptr), ECUIO_OK);

    /* Every other call refuses a NULL session. */
    int32_t last_error = 0;
    uint32_t error_count = 0;
    EXPECT_EQ(ecuio_session_start(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_close(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_unregister_frame_callback(nullptr, counting_callback, nullptr),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_error_state(nullptr, &last_error, &error_count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_signal_set(nullptr), nullptr);
}

TEST(CoreTest, DestroyAutoClosesActiveSession)
{
    /* The fclose/free split's convenience path: destroy on an open,
     * RUNNING session must join the worker and free: no hang, no leak. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://autodestroy", &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::atomic<int> frames{0};
    static std::atomic<int>* p = &frames;
    EXPECT_EQ(ecuio_register_frame_callback(
        session, [](const ecuio_frame_t*, void*) { (*p)++; }, nullptr), ECUIO_OK);
    wait_for([&frames]() { return frames.load() >= 1; }, 5000);
    EXPECT_EQ(ecuio_session_destroy(session), ECUIO_OK);
}

// ---------------------------------------------------------------------------
// ADR-0013: profile registry and session-level selection
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileLookupRawReturnsOkWithNullSet) {
    /* THE contract the null-profile design lives or dies on: "raw" is
     * REGISTERED with a NULL set. Lookup success + null is distinct
     * from lookup failure: presence and content are separate axes,
     * and the first sketch of this feature collapsed them. */
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("raw", &set), ECUIO_OK);
    EXPECT_EQ(set, nullptr);
}

TEST(CoreTest, ProfileLookupMockReturnsMockSet) {
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("mock", &set), ECUIO_OK);
    EXPECT_EQ(set, ecuio_mock_signal_set());
}

TEST(CoreTest, ProfileLookupUnknownNameFailsWithoutTouchingOut) {
    ecuio_signal_set_t sentinel = {};
    const ecuio_signal_set_t* set = &sentinel;
    EXPECT_EQ(ecuio_profile_lookup("no-such-profile", &set),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(set, &sentinel);   /* untouched on failure: caller may
                                    rely on its own initialization */
}

// A merged name nothing registered is said to be a merged set's, by the
// create and the lookup alike; any other name keeps the plain words
TEST(CoreTest, ProfileAnUnknownMergedNameSaysWhatItIs)
{
    const char* const words =
        "no profile is named 'merged.0123456789abcdef': a merged set's name, "
        "made from parts this program hasn't merged: merge the parts again";
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(
        ecuio_session_create("mock://x?profile=merged.0123456789abcdef", &s),
        ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(s, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(), words);
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("merged.0123456789abcdef", &set),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_STREQ(ecuio_last_error_detail(), words);
    EXPECT_EQ(ecuio_profile_lookup("mergedx", &set), ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_STREQ(ecuio_last_error_detail(), "no profile is named 'mergedx'");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=user.merged.x", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no profile is named 'user.merged.x'");
}

TEST(CoreTest, ProfileLookupRejectsNullArguments) {
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(nullptr, &set), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("raw", nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_profile_register(nullptr, nullptr, nullptr), ECUIO_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, ProfileRegisterOverwritesLastWins) {
    /* Duplicate-registration semantics: last wins, matching the URI
     * grammar's duplicate-key rule. Registered under a throwaway name
     * so no built-in is disturbed. */
    ecuio_signal_set_t mine = {};
    mine.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    EXPECT_EQ(ecuio_profile_register("test-profile-tmp", &mine, nullptr), ECUIO_OK);
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("test-profile-tmp", &got), ECUIO_OK);
    EXPECT_EQ(got, &mine);

    EXPECT_EQ(ecuio_profile_register("test-profile-tmp", nullptr, nullptr), ECUIO_OK);
    got = &mine;
    EXPECT_EQ(ecuio_profile_lookup("test-profile-tmp", &got), ECUIO_OK);
    EXPECT_EQ(got, nullptr);
    EXPECT_EQ(ecuio_profile_unregister("test-profile-tmp"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterValidatesNameAndSet) {
    /* The registry is the plugin surface (ADR-0013), so it checks what
     * it's handed: a set that fails validation, or a name no URI could
     * select, is refused and leaves nothing registered. */
    ecuio_signal_def_t bad_def = {}; /* bit_length 0 */
    ecuio_signal_set_t bad = {};
    bad.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    bad.count = 1;
    bad.defs = &bad_def;
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_register("test-bad-set", &bad, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("test-bad-set", &got), ECUIO_ERR_UNKNOWN_PROFILE);

    /* A set written for an earlier layout carries 1 or 2 where
     * layout_version sits, so it's refused instead of decoding garbage. */
    ecuio_signal_set_t old_layout = *ecuio_mock_signal_set();
    old_layout.layout_version = 2;
    EXPECT_EQ(ecuio_profile_register("test-old-layout", &old_layout, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_lookup("test-old-layout", &got), ECUIO_ERR_UNKNOWN_PROFILE);

    for (const char* name : {"", "Upper", "has space", "a&b", "a=b", "q?x"}) {
        EXPECT_EQ(ecuio_profile_register(name, nullptr, nullptr),
                  ECUIO_ERR_INVALID_ARGUMENT) << "'" << name << "'";
    }
    const std::string name64(ECUIO_PROFILE_NAME_MAX + 1, 'q');
    EXPECT_EQ(ecuio_profile_register(name64.c_str(), nullptr, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_profile_register("ok-name_1.2", nullptr, nullptr), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister("ok-name_1.2"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterReportsTheValidatorResult) {
    /* A refused set says which check failed and on which def, so a
     * plugin author doesn't have to search for it. */
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    ecuio_signal_def_t defs[2] = {mock->defs[0], mock->defs[1]};
    defs[1].bit_length = 0;
    ecuio_signal_set_t bad = {};
    bad.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    bad.count = 2;
    bad.defs = defs;

    ecuio_validation_t v;
    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", &bad, &v), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_BIT_LENGTH);
    EXPECT_EQ(v.index, 1u);
    EXPECT_EQ(v.reserved[0], 0u);
    EXPECT_EQ(v.reserved[1], 0u);

    /* A name refused before the set is checked, or a NULL set, which
     * isn't checked at all: code 0 and no index. */
    const struct {
        const char* name;
        const ecuio_signal_set_t* set;
        ecuio_status_t status;
    } kUnchecked[] = {
        {nullptr, &bad, ECUIO_ERR_NULL_ARGUMENT},
        {"Upper", &bad, ECUIO_ERR_INVALID_ARGUMENT},
        {"obd2", &bad, ECUIO_ERR_INVALID_ARGUMENT},
        {"test-why", nullptr, ECUIO_OK},
    };
    for (const auto& c : kUnchecked) {
        const char* label = c.name != nullptr ? c.name : "NULL name";
        std::memset(&v, 0xAB, sizeof v);
        EXPECT_EQ(ecuio_profile_register(c.name, c.set, &v), c.status) << label;
        EXPECT_EQ(v.code, 0) << label;
        EXPECT_EQ(v.index, UINT32_MAX) << label;
        EXPECT_EQ(v.reserved[0], 0u) << label;
        EXPECT_EQ(v.reserved[1], 0u) << label;
    }

    /* A registered set may repeat a name (ADR-0065 section 9) */
    defs[1].bit_length = mock->defs[1].bit_length;
    std::memcpy(defs[1].name, defs[0].name, sizeof defs[1].name);
    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", &bad, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(v.index, UINT32_MAX);

    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", mock, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(v.index, UINT32_MAX);
    EXPECT_EQ(ecuio_profile_unregister("test-why"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterRefusesMoreRequestsThanAModuleIsAsked)
{
    /* A set may derive at most ECUIO_MODULE_MAX_REQUESTS requests of one
     * module (ADR-0065 section 3), each module counted by itself: two
     * modules asked their most register, twice what a rotation holds. Defs
     * read from one reply count once: def 1 reads DID 0x1000's reply, as
     * def 0 does. One past a module's most is refused, naming the first
     * def that didn't fit, and leaves nothing registered. Service 22 DIDs,
     * since the limit is more than Service 01's PIDs. */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    constexpr uint32_t kMost = ECUIO_MODULE_MAX_REQUESTS;
    static ecuio_channel_decl_t two[2] = {{0x7E8, 0x7E0, 0, 0},
                                          {0x7E9, 0x7E1, 0, 0}};
    /* Module A's kMost + 1 defs (kMost requests), then B's kMost, then
     * one more of B's */
    static ecuio_signal_def_t defs[2 * kMost + 2];
    for (uint32_t i = 0; i < 2 * kMost + 2; ++i) {
        const bool a = i <= kMost;
        const uint32_t n = a ? i : i - (kMost + 1);
        defs[i] = obd2->defs[0];
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].message_id = two[a ? 0 : 1].rx_id;
        defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
        defs[i].start_byte = 3;
        defs[i].bit_length = 8;
        defs[i].mux_length = 3;
        defs[i].mux_key = 0x620000u | (0x1000u + ((a && n == 1) ? 0u : n));
        std::snprintf(defs[i].name, sizeof defs[i].name, "Signal %u", i);
    }
    static ecuio_signal_set_t fits = {};
    fits.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    fits.count = 2 * kMost + 1;
    fits.defs = defs;
    fits.channel_count = 2;
    fits.channels = two;
    ecuio_signal_set_t over = fits;
    over.count = 2 * kMost + 2;
    /* Module A alone, one request past its most */
    static ecuio_signal_def_t one_module[kMost + 2];
    for (uint32_t i = 0; i < kMost + 2; ++i) {
        one_module[i] = defs[i];
        one_module[i].message_id = two[0].rx_id;
        one_module[i].mux_key = 0x620000u | (0x1000u + ((i == 1) ? 0u : i));
    }
    ecuio_signal_set_t over_one = fits;
    over_one.count = kMost + 2;
    over_one.defs = one_module;
    over_one.channel_count = 1;

    ecuio_validation_t v;
    EXPECT_EQ(ecuio_profile_register("test-requests", &fits, &v), ECUIO_OK)
        << v.code << " at " << v.index;
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(ecuio_profile_unregister("test-requests"), ECUIO_OK);

    const ecuio_signal_set_t* got = nullptr;
    for (const ecuio_signal_set_t* set : {&over, &over_one}) {
        std::memset(&v, 0xAB, sizeof v);
        EXPECT_EQ(ecuio_profile_register("test-requests", set, &v),
                  ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_REQUEST_COUNT);
        EXPECT_EQ(v.index, set->count - 1) << "the first def past the most";
        EXPECT_EQ(v.reserved[0], 0u);
        EXPECT_EQ(v.reserved[1], 0u);
        EXPECT_EQ(ecuio_profile_lookup("test-requests", &got),
                  ECUIO_ERR_UNKNOWN_PROFILE);
    }

    /* Out of memory while its requests are counted never lets it in */
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_profile_register("test-requests", &over, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    const unsigned long news = news_on_this_thread() - before;
    ASSERT_GT(news, 0u) << "the table its requests are counted in";
    for (unsigned long n = 1; n <= news; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t rc =
            ecuio_profile_register("test-requests", &over, nullptr);
        fail_new_on_this_thread(0);
        EXPECT_TRUE(rc == ECUIO_ERR_OUT_OF_MEMORY ||
                    rc == ECUIO_ERR_INVALID_ARGUMENT)
            << "at " << n << ": " << rc;
        EXPECT_EQ(ecuio_profile_lookup("test-requests", &got),
                  ECUIO_ERR_UNKNOWN_PROFILE)
            << "at " << n;
    }
}

TEST(CoreTest, ProfileBuiltInsCannotBeReplacedOrRemoved) {
    /* A URI naming a built-in always means the same set. */
    for (const char* name : {"mock", "raw", "obd2"}) {
        EXPECT_EQ(ecuio_profile_register(name, ecuio_mock_signal_set(), nullptr),
                  ECUIO_ERR_INVALID_ARGUMENT) << name;
        EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_ERR_INVALID_ARGUMENT) << name;
    }
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    EXPECT_EQ(set, ecuio_obd2_signal_set());
    ASSERT_EQ(ecuio_profile_lookup("raw", &set), ECUIO_OK);
    EXPECT_EQ(set, nullptr);
}

TEST(CoreTest, ProfileUnregisterKeepsExistingSessionsOnTheirSet) {
    /* Unregister removes the name for new sessions only: a session
     * keeps the set it was created with (the profile.h lifetime rule). */
    static const ecuio_signal_set_t mine = *ecuio_mock_signal_set();
    ASSERT_EQ(ecuio_profile_register("test-unregister", &mine, nullptr), ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=test-unregister", &session),
              ECUIO_OK);

    EXPECT_EQ(ecuio_profile_unregister("test-unregister"), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister("test-unregister"), ECUIO_OK);     /* idempotent */
    EXPECT_EQ(ecuio_profile_unregister(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_signal_set(session), &mine);

    ecuio_session_t* second = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=test-unregister", &second),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(second, nullptr);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockSessionSelectsMockSet) {
    /* Scheme default: mock:// -> the mock profile's set. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://profile-default", &session),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_mock_signal_set());
    ecuio_session_destroy(session);
}

TEST(CoreTest, ProfileOverrideRawYieldsNullSet) {
    /* ?profile=raw on a mock session: override to the null profile.
     * ALSO the probe for mock-transport query tolerance (ASSUMED:
     * mock_transport.cpp not reviewed): if this goes red, the mock is
     * choking on the query, and the fix belongs THERE, not here. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=raw", &session),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), nullptr);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockUriQueryIsStrict) {
    /* mock:// shares j2534://'s query grammar (uri_query.h): profile is
     * its only key, and malformed queries fail at create. The text
     * before the '?' stays a free label. */
    for (const char* uri : {"mock://x?interval=10", "mock://x?",
                            "mock://x?profile=mock&", "mock://x?&profile=mock",
                            "mock://x?profile", "mock://x?PROFILE=mock",
                            "mock://x?profile=mock&&profile=raw"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI) << uri;
        EXPECT_EQ(session, nullptr) << uri;
    }
    for (const char* uri : {"mock://", "mock://any-label", "mock://x?profile=raw",
                            "mock://x?profile=raw&profile=mock"}) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK) << uri;
        ecuio_session_destroy(session);
    }
}

TEST(CoreTest, UnknownProfileRejectedAtCreate) {
    /* Strict grammar: an unknown name is ECUIO_ERR_UNKNOWN_PROFILE at
     * session_create: cross-platform, scheme-independent. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

#ifdef _WIN32
TEST(CoreTest, UnknownProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, Windows-only: bogus profile on a
     * NONEXISTENT DLL path. Profile validation must run BEFORE
     * transport attach, so this yields ECUIO_ERR_UNKNOWN_PROFILE, not
     * ECUIO_ERR_LOAD_FAILED. If it ever returns LOAD_FAILED, the profile check
     * moved after the attach and the fail-fast contract broke. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(
        "j2534://C:/ecuio-definitely-not-a-real-dll.dll?profile=bogus",
        &session), ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// ADR-0013: profile query tri-state, a present-but-malformed
// ?profile= value must fail session_create, never silently downgrade
// to the scheme default. Core is as strict about ?profile= as
// parse_query is about every other key.
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileQueryEmptyValueRejected) {
    /* ?profile= with nothing after '=': PRESENT but malformed, the
     * old tolerant scanner silently fell back to the scheme default,
     * which could hand a typo'd ?profile=raw a decoding session. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=", &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryTooLongValueRejected) {
    /* 64+ chars can't be a registry name (64-byte scan buffer, room
     * for the terminator). Fail fast, not truncate-and-confuse. */
    std::string uri = "mock://x?profile=";
    uri.append(80, 'a');
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryBoundaryLengthAcceptedAt63) {
    /* 63 chars is the last length the scanner accepts. Registered
     * under exactly that name so this proves the ACCEPTING side of
     * the boundary: the 64 case above pins the rejecting side. */
    const std::string name63(63, 'q');
    const ecuio_signal_set_t* mock_set = ecuio_mock_signal_set();
    ASSERT_EQ(ecuio_profile_register(name63.c_str(), mock_set, nullptr), ECUIO_OK);

    std::string uri = "mock://boundary?profile=" + name63;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), mock_set);
    ecuio_session_destroy(session);
}

TEST(CoreTest, ProfileQueryMalformedFailsEvenWhenLaterValidExists) {
    /* Scanner semantics: ANY malformed occurrence fails, last-wins
     * applies only across syntactically valid values. The empty
     * first value must not be healed by the valid second one. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=&profile=mock",
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);

    /* Reverse order: valid first, malformed LAST, a later malformed
     * value still poisons the URI (it is not a "drop the bad one,
     * keep scanning" grammar: it is strict). */
    EXPECT_EQ(ecuio_session_create("mock://x?profile=mock&profile=",
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryLastValidWinsAcrossDuplicates) {
    /* Across WELL-FORMED values, the last wins: matching the
     * documented duplicate-key rule for every other URI parameter.
     * Distinguished from the malformed cases above by using a real,
     * registered name that differs from the scheme default. */
    const std::string name63(63, 'q');   /* registered by the boundary
                                            test above, but tests
                                            shouldn't depend on order. */
    ASSERT_EQ(ecuio_profile_register(name63.c_str(),
                                     ecuio_mock_signal_set(), nullptr), ECUIO_OK);

    std::string uri1 = "mock://x?profile=raw&profile=" + name63;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri1.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session),
              ecuio_mock_signal_set());    /* last (name63) won */
    ecuio_session_destroy(session);

    /* And the inverse order: raw last -> null set */
    std::string uri2 = "mock://x?profile=" + name63 + "&profile=raw";
    ASSERT_EQ(ecuio_session_create(uri2.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), nullptr);
    ecuio_session_destroy(session);
}

#ifdef _WIN32
TEST(CoreTest, MalformedProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, matching UnknownProfileRejectedBeforeDllLoad:
     * a MALFORMED profile value on a nonexistent DLL path must yield
     * BAD_URI (profile-parse failure), not ECUIO_ERR_LOAD_FAILED.
     * If it returns 7, the tri-state check moved after attach. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(
        "j2534://C:/ecuio-definitely-not-a-real-dll.dll?profile=",
        &session), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// ADR-0020: uds-generic builds its set from the URI's ?did= entries, a set
// of the session's own; no other profile takes the key.
// ---------------------------------------------------------------------------

TEST(CoreTest, UdsGenericBuildsItsSetFromTheUri) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(
        "mock://x?profile=uds-generic&did=1E1C:u16,F190:text", &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    EXPECT_NE(set, ecuio_uds_generic_template_set()) << "the session's own";
    ASSERT_EQ(set->count, 2u);
    EXPECT_STREQ(set->defs[0].name, "DID 1E1C");
    EXPECT_EQ(set->defs[0].kind, ECUIO_VALUE_UINT);
    EXPECT_STREQ(set->defs[1].name, "DID F190");
    EXPECT_EQ(set->defs[1].kind, ECUIO_VALUE_TEXT);
    ASSERT_EQ(set->channel_count, 1u);
    EXPECT_EQ(set->channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_STREQ(info.profile, "uds-generic");

    /* A second session gets a set of its own */
    ecuio_session_t* other = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://y?profile=uds-generic&did=1E1C",
                                   &other), ECUIO_OK);
    EXPECT_NE(ecuio_session_signal_set(other), set);
    EXPECT_EQ(ecuio_session_signal_set(other)->count, 1u);
    ecuio_session_destroy(other);
    EXPECT_EQ(set->count, 2u) << "destroying one leaves the other's set";
    ecuio_session_destroy(session);
}

TEST(CoreTest, DidKeyBelongsToUdsGeneric) {
    const struct {
        const char* uri;
        const char* detail;
    } kCases[] = {
        {"mock://x?did=1E1C",
         "?did= belongs to the uds-generic profile: add ?profile=uds-generic"},
        {"mock://x?profile=raw&did=1E1C",
         "?did= belongs to the uds-generic profile: add ?profile=uds-generic"},
        {"mock://x?profile=uds-generic",
         "?profile=uds-generic needs ?did= to say which DIDs to read"},
        {"mock://x?profile=uds-generic&did=1EFF-1E00",
         "?did= needs a range whose end isn't before its start, not '1EFF-1E00'"},
        {"mock://x?profile=uds-generic&did=",
         "?did= needs at least one DID"},
    };
    for (const auto& c : kCases) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(c.uri, &session), ECUIO_ERR_BAD_URI) << c.uri;
        EXPECT_EQ(session, nullptr) << c.uri;
        EXPECT_STREQ(ecuio_last_error_detail(), c.detail) << c.uri;
    }
    /* The profile's name is judged first */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus&did=1E1C", &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

