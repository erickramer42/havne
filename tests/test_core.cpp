// File/layer: tests/test_core.cpp | GTest, UNIT tier, headless CI job

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h> /* _mkdir, _rmdir */
#else
#include <sys/stat.h>
#include <time.h> /* clock_gettime: the platform's clock, for ecuio_clock_us */
#include <unistd.h>
#endif

extern "C" {
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "ecuio/profile.h"
}

/* C++ headers (namespaced classes and inline functions): must stay
 * outside the extern "C" block above. */
#include "error_detail.h" /* core/src: a refused vendor call's words */
#include "link_health.h"  /* core/src: the workers' link-health rules */
#include "mock_drive_cycle.h"
#include "uds_generic.h"   /* core/src: the template set's identity */
#include "new_counter.h"   /* tests: operator new counted per thread */

namespace {

std::atomic<int> g_callback_count{0};

void counting_callback(const ecuio_frame_t* frame, void* user_data)
{
    (void)frame;
    int* counter = static_cast<int*>(user_data);
    if (counter) {
        (*counter)++;
    }
    g_callback_count++;
}

/* Helper to create test session */
ecuio_session_t* make_test_session(void)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);
    return session;
}

/* Poll-until-predicate with deadline. The mock's 50ms cadence is a
 * LOCAL expectation, not a CI-runner guarantee — a starved macOS VM
 * can deliver 2 frames in 200+ ms of wall clock. Polling keeps the
 * asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). Deadline is long
 * enough to absorb pathological runner load, short enough that a
 * genuinely dead transport still fails the test promptly. */
template <typename Predicate>
bool wait_for(Predicate pred, int timeout_ms, int poll_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return pred();   // final check past the deadline, then give up
}

} // anonymous namespace

/* Each create failure says why: a status, and words in
 * ecuio_last_error_detail. A success, or a null argument, leaves "". */
TEST(CoreTest, CreateFailuresNameTheirCause) {
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create("can://bus0", &s), ECUIO_ERR_NO_TRANSPORT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "unsupported URI scheme: expected mock://, replay://, "
                 "j2534://, elm327:// or socketcan://");
    EXPECT_EQ(ecuio_session_create("mock://x?interval=10", &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "mock:// takes no query key 'interval', only profile and did");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=mock&&profile=raw", &s),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "malformed query: expected key=value pairs joined by '&'");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=", &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?profile= needs a profile name of 1 to 63 characters");
    std::string long_query = "mock://x?profile=mock";
    for (int i = 0; i < 16; ++i) {
        long_query += "&profile=mock";
    }
    EXPECT_EQ(ecuio_session_create(long_query.c_str(), &s), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the query has more than 16 key=value pairs");
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_STREQ(ecuio_last_error_detail(), "no profile is named 'bogus'");

    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_session_destroy(s);

    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(ecuio_session_create(nullptr, &s), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
}

/* A refused vendor call's detail is one line: the vendor's text loses
 * its line breaks and trailing blanks, and text that's only blanks is
 * left out, as is no text at all. */
TEST(CoreTest, VendorDetailIsOneLine)
{
    ecuio_detail_set_vendor("PassThruOpen", 8, "no adapter\r\nattached  \n");
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED): "
        "no adapter  attached");
    ecuio_detail_set_vendor("PassThruOpen", 8, " \r\n");
    EXPECT_STREQ(
        ecuio_last_error_detail(),
        "PassThruOpen returned status 8 (0x8, ERR_DEVICE_NOT_CONNECTED)");
    ecuio_detail_set_vendor("PassThruConnect", 34, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "PassThruConnect returned status 34 (0x22, vendor-specific)");
    ecuio_detail_clear();
}

/* Per thread, like dlerror(): a failure on one thread leaves another
 * thread's detail as it was. */
TEST(CoreTest, LastErrorDetailIsPerThread) {
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=bogus", &s),
              ECUIO_ERR_UNKNOWN_PROFILE);
    std::string other;
    std::thread t([&other]() {
        ecuio_session_t* s2 = nullptr;
        (void)ecuio_session_create("mock://x?interval=1", &s2);
        other = ecuio_last_error_detail();
    });
    t.join();
    EXPECT_STREQ(ecuio_last_error_detail(), "no profile is named 'bogus'");
    EXPECT_EQ(other, "mock:// takes no query key 'interval', only profile and did");
}

/* A mock session says it's one, and names its profile, and reports
 * nothing else; the info outlives close. */
TEST(CoreTest, MockSessionInfoNamesTheBackend) {
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_session_info_t info;
    memset(&info, 0xEE, sizeof(info));
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_MOCK);
    EXPECT_EQ(info.dll_arch, ECUIO_DLL_ARCH_UNKNOWN);
    EXPECT_EQ(info.flags, 0u);
    EXPECT_EQ(info.protocol_id, 0u);
    EXPECT_EQ(info.baud, 0u);
    EXPECT_EQ(info.channel_count, 0u);
    EXPECT_STREQ(info.firmware_version, "");
    EXPECT_STREQ(info.profile, "mock");
    for (uint32_t r : info.reserved) {
        EXPECT_EQ(r, 0u);
    }

    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_MOCK);

    EXPECT_EQ(ecuio_session_get_info(s, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_info(nullptr, &info), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(s);

    ASSERT_EQ(ecuio_session_create("mock://demo?profile=raw", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_STREQ(info.profile, "raw");
    ecuio_session_destroy(s);
}

/* A mock session has no channels, so no ECU to report on: every index
 * is out of range, and the report is left as it was. */
TEST(CoreTest, MockSessionHasNoChannelSupport) {
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_support_t support;
    memset(&support, 0xEE, sizeof(support));
    EXPECT_EQ(ecuio_session_get_channel_support(s, 0, &support),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(support.flags, 0xEEEEEEEEu);
    EXPECT_EQ(ecuio_session_get_channel_support(s, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_channel_support(nullptr, 0, &support),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(s);
}

TEST(CoreTest, StatusStringValidValues) {
    EXPECT_STREQ(ecuio_status_string(ECUIO_OK), "success");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NULL_ARGUMENT), "null argument");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NOT_IMPLEMENTED), "not implemented yet");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_NO_TRANSPORT), "no transport available");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_OUT_OF_MEMORY), "out of memory");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_SESSION_CLOSED), "session already closed");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_ALREADY_REGISTERED), "already registered");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_LOAD_FAILED), "couldn't load what the URI names");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_INVALID_ARGUMENT), "invalid argument");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_BAD_URI), "malformed transport URI");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_UNKNOWN_PROFILE), "unknown profile");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_VENDOR_REFUSED), "the adapter's driver refused");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_SURROGATE_FAILED), "surrogate process failed");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_RECORDING_CHANGED),
                 "the recording changed after it was opened");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_VEHICLE_TIMEOUT),
                 "vehicle not responding (timeout)");

    /* Explicit cast required by MSVC for enum conversion */
    EXPECT_STRNE(ecuio_status_string(static_cast<ecuio_status_t>(999)), nullptr);
}

TEST(CoreTest, VersionStringContainsNumbers) {
    const char* version = ecuio_version_string();
    EXPECT_TRUE(version != nullptr);
    EXPECT_GT(strlen(version), 0);
    EXPECT_NE(strstr(version, "."), nullptr);
}

TEST(CoreTest, InitShutdownIdempotent) {
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    
    ecuio_shutdown();
    
    EXPECT_EQ(ecuio_init(), ECUIO_OK);
    ecuio_shutdown();
}

/* core.h's promise: init and shutdown are optional. A session needs
 * neither, and one that is open when shutdown runs keeps delivering. */
TEST(CoreTest, ShutdownLeavesAnOpenSessionWorking) {
    ecuio_shutdown();   /* with no init before it */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://no-init", &session), ECUIO_OK);
    std::atomic<int> frames{0};
    ASSERT_EQ(ecuio_register_frame_callback(
                  session,
                  [](const ecuio_frame_t*, void* user_data) {
                      static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
                  },
                  &frames),
              ECUIO_OK);
    ecuio_shutdown();
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_TRUE(wait_for([&frames]() { return frames.load() >= 2; }, 5000))
        << "only " << frames.load() << " frames after shutdown";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST(CoreTest, SessionCreateRequiresNonNullPointer) {
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);
    
    EXPECT_EQ(ecuio_session_create("mock://test", nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    if (session) {
        ecuio_session_close(session);
        ecuio_session_destroy(session);
    }
}

TEST(CoreTest, RegisterCallbackSuccess) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    ecuio_status_t status = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status, ECUIO_OK);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, RegisterDuplicateCallbackIdempotent) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    ecuio_status_t status1 = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    ecuio_status_t status2 = ecuio_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status1, ECUIO_OK);
    EXPECT_EQ(status2, ECUIO_OK);   /* Silently deduplicated, not an error */
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, SameCallbackDifferentUserdataAllowed) {
    ecuio_session_t* session = make_test_session();
    int counter1 = 0;
    int counter2 = 0;
    
    /* Same function, different user_data = distinct registrations */
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter1), ECUIO_OK);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter2), ECUIO_OK);

    /* ...so unregistering one pair leaves the other */
    EXPECT_EQ(
        ecuio_unregister_frame_callback(session, counting_callback, &counter2),
        ECUIO_OK);
    ecuio_frame_t frame{};
    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter1, 1);
    EXPECT_EQ(counter2, 0);

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnregisterCallback) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    /* Second unregister is also success (idempotent) */
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnregisterNonexistentCallbackDoesntFail) {
    ecuio_session_t* session = make_test_session();
    
    /* Never registered, but should not crash */
    EXPECT_EQ(ecuio_unregister_frame_callback(
        session, counting_callback, nullptr), ECUIO_OK);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, NullPointerHandling) {
    EXPECT_EQ(ecuio_register_frame_callback(
        nullptr, counting_callback, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    ecuio_session_t* session = make_test_session();
    EXPECT_EQ(ecuio_register_frame_callback(
        session, nullptr, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

/* Threading contract test - validates snapshot pattern compiles */
TEST(CoreTest, CallbackSnapshotPatternCompiles) {
    /* This test documents the threading discipline:
     * 1. Mock transport will call ecuio_session_dispatch_callbacks()
     * 2. That function copies the callback list under lock
     * 3. Then invokes callbacks OUTSIDE the lock
     *
     * This prevents deadlock if callback calls unregister().
     *
     * Full threading validation requires TSAN job or live transport test.
     */
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    
    /* Null frame is a documented no-op — proves precondition guard */
    ecuio_session_dispatch_callbacks(session, nullptr);
    
    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, CallbackDispatchInvokesRegisteredCallback) {
    ecuio_session_t* session = make_test_session();
    int counter = 0;
    ecuio_frame_t frame{};
    frame.data_length = 8;

    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);

    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 1);

    /* Duplicate registration is deduplicated — one invocation per dispatch */
    EXPECT_EQ(ecuio_register_frame_callback(
        session, counting_callback, &counter), ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 2);  // Still 1 per dispatch, dedup worked

    ecuio_session_close(session);
    ecuio_session_destroy(session);
}

TEST(CoreTest, MockTransportDeliversFrames) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);

    std::atomic<int> frame_count{0};
    /* plain function to keep extern "C" signature compatibility */
    static std::atomic<int>* counter_ptr = &frame_count;
    EXPECT_EQ(ecuio_register_frame_callback(
        session,
        [](const ecuio_frame_t*, void*) { (*counter_ptr)++; },
        nullptr), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* CONTRACT: at least 3 frames eventually arrive. The 50ms tick
     * implies ~4 in 200ms LOCALLY, but CI runners under load deliver
     * fewer (2 in 356 ms has been seen on macOS) — poll to the
     * condition instead of betting on a fixed sleep. */
    const bool delivered = wait_for(
        [&frame_count]() { return frame_count.load() >= 3; },
        5000);
    EXPECT_TRUE(delivered) << "mock delivered only "
                           << frame_count.load() << " frames in 5s";
    EXPECT_GE(frame_count.load(), 3);

    /* Close must join the worker without hanging */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Send isn't built yet. A frame whose data_length claims more than
 * data[] holds is the caller's error, INVALID_ARGUMENT; any frame that
 * fits says the call isn't implemented. */
TEST(CoreTest, SessionSendRefusesALengthPastTheFramesData) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &session), ECUIO_OK);
    ecuio_frame_t frame = {};
    frame.data_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX + 1);
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_INVALID_ARGUMENT);
    frame.data_length = static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX);
    EXPECT_EQ(ecuio_session_send(session, &frame), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(ecuio_session_send(session, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_destroy(session);
}

TEST(CoreTest, UnknownTransportUriRejected) {
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("j2534://dev0", &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(ecuio_session_create(nullptr, &session),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, CallbackExecutesOnTransportWorkerThread) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://thread-test", &session), ECUIO_OK);

    /* std::atomic<std::thread::id> is lock-free on all 3 CI platforms;
     * mutex alternative also fine. */
    std::atomic<std::thread::id> seen_id{};
    struct Ctx { std::atomic<std::thread::id>* out; };
    static Ctx ctx{&seen_id};  /* static: captureless lambda can't capture */

    EXPECT_EQ(ecuio_register_frame_callback(
        session,
        [](const ecuio_frame_t*, void* ud) {
            static_cast<Ctx*>(ud)->out->store(std::this_thread::get_id());
        },
        &ctx), ECUIO_OK);

    const auto main_id = std::this_thread::get_id();
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Already a poll loop — widened deadline only (400ms -> 2s) to
     * cover starved runners; the shape was right. */
    const bool seen = wait_for(
        [&seen_id]() { return seen_id.load() != std::thread::id{}; },
        2000);
    EXPECT_TRUE(seen);

    EXPECT_NE(seen_id.load(), std::thread::id{});
    EXPECT_NE(seen_id.load(), main_id);  /* Marshaling in bridge is MANDATORY */

    /* A second start leaves the one worker running. */
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

struct ReentryCtx {
    ecuio_session_t* session = nullptr;
    std::atomic<int> self_count{0};
    std::atomic<int> other_count{0};
};

/* NAMED function — required so the callback can unregister ITSELF.
 * A captureless lambda cannot refer to its own function pointer. */
void self_unregistering_cb(const ecuio_frame_t*, void* user_data)
{
    ReentryCtx* c = static_cast<ReentryCtx*>(user_data);
    c->self_count.fetch_add(1, std::memory_order_relaxed);
    /* Runs on worker thread while snapshot dispatch is mid-iteration.
     * Touches the callback registry (same mutex being snapshotted).
     * Deadlocks here if the snapshot pattern regresses. */
    ecuio_unregister_frame_callback(c->session, &self_unregistering_cb, user_data);
}

void sibling_counter_cb(const ecuio_frame_t*, void* user_data)
{
    static_cast<ReentryCtx*>(user_data)->other_count
        .fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(CoreTest, SelfUnregisterDuringLiveDispatch) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://reentry", &session), ECUIO_OK);

    ReentryCtx ctx;   // stack-local is safe: session_close joins the worker
    ctx.session = session;

    EXPECT_EQ(ecuio_register_frame_callback(
        session, &self_unregistering_cb, &ctx), ECUIO_OK);
    EXPECT_EQ(ecuio_register_frame_callback(
        session, &sibling_counter_cb, &ctx), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    /* Poll until the sibling has seen >= 3 frames — that implies the
     * self-callback fired (it dispatched first) and the stream is
     * flowing. Polls to the CONDITION instead of a fixed 250ms. */
    wait_for([&ctx]() { return ctx.other_count.load() >= 3; }, 5000);

    /* Exactly one self-invocation: snapshot allowed THIS dispatch to
     * complete, and the removal took effect for every frame after. */
    EXPECT_EQ(ctx.self_count.load(), 1);
    /* Sibling survived the concurrent removal untouched. */
    EXPECT_GE(ctx.other_count.load(), 3);

    /* Quiesce: close joins worker; must not hang under reentry. */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

/* One counter per callback, so a dispatch shows which it reached */
struct Tally {
    int hits[8] = {};
};

void tally_cb(const ecuio_frame_t*, void* user_data)
{
    ++*static_cast<int*>(user_data);
}

/* The first callback unregisters the second and registers a third, on
 * the first frame only */
struct SwapCtx {
    ecuio_session_t* session = nullptr;
    int removed = 0;
    int added = 0;
    bool swapped = false;
};

void removed_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<SwapCtx*>(user_data)->removed;
}

void added_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<SwapCtx*>(user_data)->added;
}

void swapping_cb(const ecuio_frame_t*, void* user_data)
{
    auto* c = static_cast<SwapCtx*>(user_data);
    if (!c->swapped) {
        c->swapped = true;
        ecuio_unregister_frame_callback(c->session, removed_cb, c);
        ecuio_register_frame_callback(c->session, added_cb, c);
    }
}

} // namespace

/* Every callback runs once a frame at any list length, either side of
 * the snapshot's room on the stack */
TEST(CoreTest, DispatchReachesEveryCallbackOnceAtAnyListLength)
{
    for (int n = 1; n <= 8; ++n) {
        SCOPED_TRACE(n);
        ecuio_session_t* session = make_test_session();
        Tally t;
        for (int i = 0; i < n; ++i) {
            EXPECT_EQ(
                ecuio_register_frame_callback(session, tally_cb, &t.hits[i]),
                ECUIO_OK);
        }
        ecuio_frame_t frame{};
        ecuio_session_dispatch_callbacks(session, &frame);
        ecuio_session_dispatch_callbacks(session, &frame);
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(t.hits[i], i < n ? 2 : 0) << "callback " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* A callback that unregisters one and registers another mid-dispatch:
 * that frame still reaches the one removed and not the one added, the
 * next frame the reverse (ADR-0002), on a list that fits the stack and
 * one that doesn't */
TEST(CoreTest, DispatchReachesTheListAsItStoodWhenTheFrameArrived)
{
    for (int fillers : {0, 4}) {
        SCOPED_TRACE(fillers);
        ecuio_session_t* session = make_test_session();
        SwapCtx c;
        c.session = session;
        Tally t;
        EXPECT_EQ(ecuio_register_frame_callback(session, swapping_cb, &c),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(session, removed_cb, &c),
                  ECUIO_OK);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(
                ecuio_register_frame_callback(session, tally_cb, &t.hits[i]),
                ECUIO_OK);
        }
        ecuio_frame_t frame{};
        ecuio_session_dispatch_callbacks(session, &frame);
        EXPECT_EQ(c.removed, 1) << "in the snapshot";
        EXPECT_EQ(c.added, 0) << "not in it";
        ecuio_session_dispatch_callbacks(session, &frame);
        EXPECT_EQ(c.removed, 1);
        EXPECT_EQ(c.added, 1);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(t.hits[i], 2) << "filler " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* A dispatch to up to four callbacks (the app registers one) allocates
 * nothing; a fifth puts the snapshot on the heap, which also shows the
 * counter sees core's allocations */
TEST(CoreTest, DispatchToAShortListAllocatesNothing)
{
    ecuio_session_t* session = make_test_session();
    Tally t;
    ecuio_frame_t frame{};
    for (int n = 1; n <= 5; ++n) {
        EXPECT_EQ(
            ecuio_register_frame_callback(session, tally_cb, &t.hits[n - 1]),
            ECUIO_OK);
        const unsigned long before = new_counter::on_this_thread();
        ecuio_session_dispatch_callbacks(session, &frame);
        const unsigned long news = new_counter::on_this_thread() - before;
        if (n <= 4) {
            EXPECT_EQ(news, 0u) << n << " callbacks";
        } else {
            EXPECT_GT(news, 0u) << "the heap copy past four";
        }
    }
    ecuio_session_destroy(session);
}

namespace {

/* The mock's physical envelope across a whole drive cycle. Shared by
 * the worker test below, which sees only the first second of the
 * cycle, and the deterministic cycle tests, which see all of it. */
constexpr float kMockRpmMin = 600.0f;
constexpr float kMockRpmMax = 7100.0f;
constexpr float kMockCoolantMin = 15.0f;
constexpr float kMockCoolantMax = 110.0f;
constexpr float kMockSpeedMax = 200.0f;
constexpr float kMockIatMin = 15.0f;
constexpr float kMockIatMax = 80.0f;
constexpr float kMockMafMax = 300.0f;   /* WOT near the limiter peaks ~255 */
constexpr float kMockOilMin = 15.0f;    /* cold start reads 20 */
constexpr float kMockOilMax = 140.0f;
constexpr float kMockLambdaMin = 0.8f;  /* WOT enrichment reads ~0.86 */
constexpr float kMockLambdaMax = 2.0f;  /* fuel cut pegs the sensor lean */

struct MockCapture {
    static constexpr size_t kMaxFrames = 20;
    uint8_t payloads[kMaxFrames][ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN]{};
    size_t count = 0;
};

/* Named free function — payload capture needs self-reference-free
 * simplicity; user_data points at a stack struct, safe because
 * session_close joins the worker before the stack frame unwinds. */
void capture_payload_cb(const ecuio_frame_t* frame, void* user_data)
{
    MockCapture* cap = static_cast<MockCapture*>(user_data);
    if (cap->count < MockCapture::kMaxFrames) {
        std::memcpy(cap->payloads[cap->count], frame->data, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN);
        ++cap->count;
    }
}

} // namespace

TEST(CoreTest, MockFramesDecodeAgainstMockSignalSet) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://signals", &session), ECUIO_OK);

    MockCapture cap;
    EXPECT_EQ(ecuio_register_frame_callback(
        session, &capture_payload_cb, &cap), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Collect up to kMaxFrames, polling to the condition. Was a fixed
     * 1400ms sleep; under starvation fewer than the nominal 28 frames
     * would arrive and the >=5 floor got uncomfortably close. Now we
     * simply leave as soon as the buffer is full. */
    wait_for([&cap]() { return cap.count >= MockCapture::kMaxFrames; },
             10000);

    /* Close joins the worker; capture struct outlives dispatch safely.
     * Destroy here, before the ASSERTs below can return early. */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count, size_t{5}) << "expected several frames from mock";

    /* Encoder/decoder round-trip contract: every frame decodes under
     * every mock definition with physically plausible results. A
     * byte-order or scale inversion passes structural checks but
     * produces absurd physics — the range assertions catch that. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    for (size_t f = 0; f < cap.count; ++f) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            double val = 0.0;
            ASSERT_EQ(ecuio_decode_signal(&def, cap.payloads[f], ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val), 0)
                << "frame " << f << ", signal " << def.name;

            switch (def.id) {
            case ECUIO_MOCK_SIG_RPM_ID:
                EXPECT_GE(val, kMockRpmMin);  EXPECT_LE(val, kMockRpmMax); break;
            case ECUIO_MOCK_SIG_COOLANT_ID:
                EXPECT_GE(val, kMockCoolantMin);
                EXPECT_LE(val, kMockCoolantMax); break;
            case ECUIO_MOCK_SIG_SPEED_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, kMockSpeedMax); break;
            case ECUIO_MOCK_SIG_THROTTLE_ID:
            case ECUIO_MOCK_SIG_LOAD_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, 100.0f); break;
            case ECUIO_MOCK_SIG_IAT_ID:
                EXPECT_GE(val, kMockIatMin);  EXPECT_LE(val, kMockIatMax); break;
            case ECUIO_MOCK_SIG_MAF_ID:
                EXPECT_GE(val, 0.0f);          EXPECT_LE(val, kMockMafMax); break;
            case ECUIO_MOCK_SIG_OIL_ID:
                EXPECT_GE(val, kMockOilMin);    EXPECT_LE(val, kMockOilMax); break;
            case ECUIO_MOCK_SIG_LAMBDA_ID:
                EXPECT_GE(val, kMockLambdaMin); EXPECT_LE(val, kMockLambdaMax); break;
            default:
                FAIL() << "unknown signal id in mock set: " << def.id;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Mock drive cycle: stepped deterministically, no worker, no wall clock
// ---------------------------------------------------------------------------

namespace {

using ecuio_mock::DriveCycle;
using ecuio_mock::Phase;

constexpr int kPhaseCount = static_cast<int>(Phase::Cooldown) + 1;

struct PhaseStats {
    uint32_t ticks = 0;
    float entry_rpm = 0.0f;    /* after the phase's first tick */
    float entry_kmh = 0.0f;
    float min_rpm = 1e9f;
    float peak_rpm = 0.0f;
    float peak_kmh = 0.0f;
    float peak_coolant = -100.0f;
    float peak_iat = -100.0f;
    float peak_oil = -100.0f;
    int max_gear = 0;
};

struct LoopRun {
    PhaseStats phase[kPhaseCount];
    std::vector<Phase> order;  /* phases in visit order */
    bool closed = false;       /* looped back to Roll2nd */
    DriveCycle end;
};

const PhaseStats& stats(const LoopRun& r, Phase p)
{
    return r.phase[static_cast<int>(p)];
}

/* Step one loop, WarmUp through Cooldown, stopping at the tick that
 * returns to Roll2nd. Bounded at 10 simulated minutes. */
LoopRun run_one_loop()
{
    LoopRun r;
    DriveCycle dc;
    PhaseStats* s = nullptr;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f); ++i) {
        dc.step();
        if (dc.loops > 0) {
            r.closed = true;
            break;
        }
        if (r.order.empty() || r.order.back() != dc.phase) {
            r.order.push_back(dc.phase);
            s = &r.phase[static_cast<int>(dc.phase)];
            s->entry_rpm = dc.rpm;
            s->entry_kmh = dc.speed_kmh;
        }
        ++s->ticks;
        s->min_rpm = std::fmin(s->min_rpm, dc.rpm);
        s->peak_rpm = std::fmax(s->peak_rpm, dc.rpm);
        s->peak_kmh = std::fmax(s->peak_kmh, dc.speed_kmh);
        s->peak_coolant = std::fmax(s->peak_coolant, dc.coolant_c);
        s->peak_iat = std::fmax(s->peak_iat, dc.iat_c);
        s->peak_oil = std::fmax(s->peak_oil, dc.oil_c);
        if (dc.gear > s->max_gear) {
            s->max_gear = dc.gear;
        }
    }
    r.end = dc;
    return r;
}

} // namespace

TEST(CoreTest, MockDriveCycleVisitsEveryPhaseInOrder)
{
    const LoopRun r = run_one_loop();
    ASSERT_TRUE(r.closed) << "the cycle never looped back to Roll2nd";
    ASSERT_EQ(r.order.size(), static_cast<size_t>(kPhaseCount));
    for (int i = 0; i < kPhaseCount; ++i) {
        EXPECT_EQ(static_cast<int>(r.order[static_cast<size_t>(i)]), i)
            << "visit " << i;
    }
    EXPECT_EQ(r.end.phase, Phase::Roll2nd);
}

TEST(CoreTest, MockDriveCycleSecondGearPullHoldsTheLimiter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull = stats(r, Phase::Pull2nd);
    const PhaseStats& lim = stats(r, Phase::Limiter2nd);

    /* From low speed and rpm, in 2nd, to the limiter */
    EXPECT_EQ(pull.max_gear, 2);
    EXPECT_LT(pull.entry_rpm, 2000.0f);
    EXPECT_LT(pull.entry_kmh, 35.0f);
    EXPECT_LE(pull.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* Exactly 5 s on the limiter: past it on first contact, before the
     * first cut lands, then sawing just under it */
    EXPECT_EQ(lim.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim.min_rpm, ecuio_mock::kRevLimitRpm - 200.0f);
    EXPECT_GT(lim.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LE(lim.peak_rpm, ecuio_mock::kRevLimitRpm + 100.0f);
    EXPECT_NEAR(lim.peak_kmh, 113.0f, 5.0f);

    /* Past either end of its table the torque curve holds that end */
    EXPECT_EQ(ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm + 500.0f),
              ecuio_mock::torque_nm(ecuio_mock::kRevLimitRpm));
    EXPECT_EQ(ecuio_mock::torque_nm(-100.0f), 0.0f);
}

TEST(CoreTest, MockDriveCycleThirdGearPullIsLongerFasterAndHotter)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& pull2 = stats(r, Phase::Pull2nd);
    const PhaseStats& lim2 = stats(r, Phase::Limiter2nd);
    const PhaseStats& pull3 = stats(r, Phase::Pull3rd);
    const PhaseStats& lim3 = stats(r, Phase::Limiter3rd);

    EXPECT_EQ(pull3.max_gear, 3);
    EXPECT_LE(pull3.peak_rpm, ecuio_mock::kRevLimitRpm);
    EXPECT_LT(pull3.entry_rpm, 2200.0f);
    EXPECT_GT(pull3.entry_kmh, pull2.entry_kmh + 10.0f);
    EXPECT_GT(pull3.ticks, pull2.ticks * 3 / 2) << "3rd should pull longer";

    EXPECT_EQ(lim3.ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(lim3.peak_kmh, lim2.peak_kmh + 40.0f);

    const float coolant2 = std::fmax(pull2.peak_coolant, lim2.peak_coolant);
    const float coolant3 = std::fmax(pull3.peak_coolant, lim3.peak_coolant);
    const float oil2 = std::fmax(pull2.peak_oil, lim2.peak_oil);
    const float oil3 = std::fmax(pull3.peak_oil, lim3.peak_oil);
    const float iat3 = std::fmax(pull3.peak_iat, lim3.peak_iat);
    /* The longer pull puts more heat into the cold engine. The intake
     * air doesn't follow: with no turbo, and ram air at speed, it stays
     * near ambient. */
    EXPECT_GT(coolant3, coolant2 + 4.0f);
    EXPECT_GT(oil3, oil2 + 4.0f);
    EXPECT_LE(iat3, ecuio_mock::kAmbientC + 10.0f);
}

TEST(CoreTest, MockDriveCycleDragRunCoversAQuarterMile)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& launch = stats(r, Phase::Launch);
    const PhaseStats& run = stats(r, Phase::DragRun);

    /* Launch control from a standstill */
    EXPECT_EQ(launch.peak_kmh, 0.0f);
    EXPECT_LE(launch.peak_rpm, ecuio_mock::kLaunchRpm);
    EXPECT_GE(launch.peak_rpm, ecuio_mock::kLaunchRpm - 150.0f);

    /* Through the gears, shifting before the limiter */
    EXPECT_EQ(run.max_gear, ecuio_mock::kTopGear);
    EXPECT_LT(run.peak_rpm, ecuio_mock::kRevLimitRpm);

    /* The run ends on the tick that crosses the line */
    EXPECT_GE(r.end.distance_m, ecuio_mock::kQuarterMileM);
    EXPECT_LT(r.end.distance_m, ecuio_mock::kQuarterMileM + 5.0f);
    EXPECT_GE(r.end.drag_et_s, 11.0f);
    EXPECT_LE(r.end.drag_et_s, 15.0f);
    EXPECT_GE(r.end.drag_trap_kmh, 150.0f);
    EXPECT_LE(r.end.drag_trap_kmh, 200.0f);
}

TEST(CoreTest, MockDriveCycleLaterLoopsRunAtOperatingTemperature)
{
    /* Only the first loop starts cold; each later one picks up where the
     * last ended. The first loop's runs stay on a cold engine. From the
     * third the coolant holds the thermostat's range, and by the last
     * loop here it climbs past 210 F at WOT until the fan cuts in, with
     * the oil built up to 226-243 F. */
    constexpr uint32_t kLoops = 6;
    struct Range {
        float lo = 1e9f, hi = -1e9f;
        void add(float v)
        {
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
    };
    Range coolant[kLoops];
    Range oil[kLoops];
    bool restarted_cold = false;

    /* The cold first loop */
    float first_pull2_coolant = -1e9f;
    float first_end_coolant = 0.0f, first_end_oil = 0.0f;

    /* The hot last loop: coolant at cruise and at WOT (the pulls, their
     * limiters and the quarter mile), the fan, and the oil across the
     * quarter mile and the stop after it */
    Range hot_cruise_coolant, hot_wot_coolant, hot_drag_oil;
    float hot_drag_oil_entry = 0.0f;
    bool hot_fan = false;
    float hot_end_coolant = 0.0f, hot_end_oil = 0.0f;

    DriveCycle dc;
    while (dc.loops < kLoops && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        if (dc.loops >= kLoops) {
            break;
        }
        const uint32_t i = dc.loops;
        coolant[i].add(dc.coolant_c);
        oil[i].add(dc.oil_c);
        restarted_cold = restarted_cold || (i > 0 && dc.phase == Phase::WarmUp);
        if (i == 0) {
            if (dc.phase == Phase::Pull2nd) {
                first_pull2_coolant =
                    std::fmax(first_pull2_coolant, dc.coolant_c);
            }
            first_end_coolant = dc.coolant_c;
            first_end_oil = dc.oil_c;
        } else if (i == kLoops - 1) {
            const Phase p = dc.phase;
            if (p == Phase::Cruise4th) {
                hot_cruise_coolant.add(dc.coolant_c);
            }
            if (p == Phase::Pull2nd || p == Phase::Limiter2nd ||
                p == Phase::Pull3rd || p == Phase::Limiter3rd ||
                p == Phase::DragRun) {
                hot_wot_coolant.add(dc.coolant_c);
            }
            if (p == Phase::DragRun || p == Phase::Shutdown) {
                if (hot_drag_oil.hi < -1e8f) {
                    hot_drag_oil_entry = dc.oil_c;
                }
                hot_drag_oil.add(dc.oil_c);
            }
            hot_fan = hot_fan || dc.fan_on;
            hot_end_coolant = dc.coolant_c;
            hot_end_oil = dc.oil_c;
        }
    }
    ASSERT_EQ(dc.loops, kLoops) << "six loops never completed";
    EXPECT_FALSE(restarted_cold) << "a later loop ran the cold start";

    /* Cold first loop: the thermostat never opens, and the oil trails
     * the coolant as the sump's mass warms */
    EXPECT_LT(first_pull2_coolant, 40.0f);
    EXPECT_LT(coolant[0].hi, ecuio_mock::kThermostatOpenC);
    EXPECT_LT(first_end_oil, first_end_coolant);

    /* From the third loop on, the coolant never falls below the
     * thermostat's range */
    for (uint32_t i = 2; i < kLoops; ++i) {
        EXPECT_GE(coolant[i].lo, 85.0f) << "loop " << i + 1;
    }

    /* Hot: the thermostat holds the cruise, the radiator can't keep up
     * at WOT, and the fan cuts in */
    EXPECT_GE(hot_cruise_coolant.lo, 85.0f);
    EXPECT_LE(hot_cruise_coolant.hi, 97.0f);
    EXPECT_GE(hot_wot_coolant.hi, 100.0f) << "the radiator kept up at WOT";
    EXPECT_LE(hot_wot_coolant.hi, 105.0f);
    EXPECT_TRUE(hot_fan) << "the fan never cut in";
    EXPECT_GE(oil[kLoops - 1].lo, 100.0f);
    EXPECT_GE(oil[kLoops - 1].hi, 112.0f);
    EXPECT_LE(oil[kLoops - 1].hi, 125.0f);
    /* The sump's mass: a quarter mile moves the oil several degrees, not
     * the coolant's swing */
    EXPECT_GE(hot_drag_oil.hi - hot_drag_oil_entry, 2.0f);
    EXPECT_LE(hot_drag_oil.hi - hot_drag_oil_entry, 9.0f);
    EXPECT_GT(hot_end_oil, hot_end_coolant)
        << "the oil cooled below the coolant";
}

TEST(CoreTest, MockDriveCycleEveryTickRoundTripsThroughTheMockSet)
{
    /* Three loops: the cold first one, the one that warms through the
     * thermostat, and a hot one. Every tick goes through the real
     * encoder and decoder. A value the wire can't carry would clamp in
     * the encoder and show up as a large error. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    /* Per-signal accumulators, indexed like set->defs. */
    constexpr uint32_t kMaxDefs = 16;
    ASSERT_LE(set->count, kMaxDefs);
    double worst_err[kMaxDefs] = {};
    double lo[kMaxDefs];
    double hi[kMaxDefs];
    for (uint32_t i = 0; i < kMaxDefs; ++i) {
        lo[i] = 1e9;
        hi[i] = -1e9;
    }

    DriveCycle dc;
    uint32_t steps = 0;
    while (dc.loops < 3 && steps < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ++steps;
        uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN];
        ecuio_mock::encode_payload(dc, payload);

        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            double val = 0.0;
            ASSERT_EQ(ecuio_decode_signal(&def, payload, ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN, &val), 0)
                << def.name << " at tick " << steps;
            double phys = 0.0;
            switch (def.id) {
            case ECUIO_MOCK_SIG_RPM_ID:       phys = dc.rpm; break;
            case ECUIO_MOCK_SIG_COOLANT_ID:   phys = dc.coolant_c; break;
            case ECUIO_MOCK_SIG_SPEED_ID:     phys = dc.speed_kmh; break;
            case ECUIO_MOCK_SIG_THROTTLE_ID: phys = dc.throttle_pct; break;
            case ECUIO_MOCK_SIG_IAT_ID:       phys = dc.iat_c; break;
            case ECUIO_MOCK_SIG_LOAD_ID:      phys = dc.load_pct; break;
            case ECUIO_MOCK_SIG_MAF_ID:       phys = dc.maf_gs; break;
            case ECUIO_MOCK_SIG_OIL_ID:       phys = dc.oil_c; break;
            case ECUIO_MOCK_SIG_LAMBDA_ID:    phys = dc.lambda; break;
            default: FAIL() << "unknown signal id in mock set: " << def.id;
            }
            worst_err[i] = std::fmax(worst_err[i], std::fabs(val - phys));
            lo[i] = std::fmin(lo[i], val);
            hi[i] = std::fmax(hi[i], val);
        }
    }
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";

    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        /* Half an LSB of each signal's scale, plus float slack */
        const double tol = 0.5 * std::fabs(def.scale) + 0.01;
        EXPECT_LE(worst_err[i], tol) << def.name << " lost precision or clamped";

        switch (def.id) {
        case ECUIO_MOCK_SIG_RPM_ID:
            EXPECT_GE(lo[i], kMockRpmMin);     EXPECT_LE(hi[i], kMockRpmMax); break;
        case ECUIO_MOCK_SIG_COOLANT_ID:
            EXPECT_GE(lo[i], kMockCoolantMin); EXPECT_LE(hi[i], kMockCoolantMax); break;
        case ECUIO_MOCK_SIG_SPEED_ID:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], kMockSpeedMax); break;
        case ECUIO_MOCK_SIG_IAT_ID:
            EXPECT_GE(lo[i], kMockIatMin);     EXPECT_LE(hi[i], kMockIatMax); break;
        case ECUIO_MOCK_SIG_MAF_ID:
            EXPECT_GE(lo[i], 0.0f);    EXPECT_LE(hi[i], kMockMafMax); break;
        case ECUIO_MOCK_SIG_OIL_ID:
            EXPECT_GE(lo[i], kMockOilMin); EXPECT_LE(hi[i], kMockOilMax); break;
        case ECUIO_MOCK_SIG_LAMBDA_ID:
            EXPECT_GE(lo[i], kMockLambdaMin); EXPECT_LE(hi[i], kMockLambdaMax); break;
        default:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], 100.0f); break;
        }
    }
}

namespace {

/* Steps a fresh cycle until `phase` starts (in the first loop), then
 * calls `visit` on each of its ticks. */
template <typename F>
void for_each_tick_of(Phase phase, F visit)
{
    DriveCycle dc;
    bool in = false;
    for (uint32_t i = 0; i < ecuio_mock::ticks_for(600.0f) && dc.loops == 0; ++i) {
        dc.step();
        if (dc.phase == phase) {
            in = true;
            visit(dc);
        } else if (in) {
            return;
        }
    }
}

} // namespace

TEST(CoreTest, MockDriveCycleSignalsStayPhysical)
{
    /* Three loops (cold, warming, hot), every tick, against the rules a
     * real ECU's log keeps:
     * rpm locked to wheel speed in gear, no speed at idle rpm with the
     * clutch in, airflow consistent with rpm and load, lambda lean
     * exactly when fuel is cut, and temperatures that move like masses
     * of metal and coolant. */
    DriveCycle dc;
    DriveCycle prev = dc;
    uint32_t cut_run = 0;
    uint32_t fueled_run = 0;
    uint32_t fueled_at_limit = 0;
    int checked_locked = 0;
    while (dc.loops < 3 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        const float k = dc.gear > 0 ? ecuio_mock::rpm_per_mps(dc.gear) : 0.0f;
        const bool limiter = dc.phase == Phase::Limiter2nd || dc.phase == Phase::Limiter3rd;
        const bool locked = dc.gear > 0 && !dc.clutch_slip && !dc.shifting() && !dc.spinning;

        if (locked && !limiter) {
            /* Within the crank reading's firing-pulse jitter */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 10.0f) << "tick " << dc.tick;
            ++checked_locked;
        } else if (locked) {
            /* The saw is crank speed against the driveline's compliance */
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 200.0f) << "tick " << dc.tick;
        }
        if (dc.gear == 0 && !dc.shifting()) {
            EXPECT_LE(dc.speed_kmh, 50.0f) << "clutch in at speed, tick " << dc.tick;
        }
        if (dc.gear > 0 && !dc.clutch_slip && !dc.shifting()) {
            EXPECT_GE(dc.rpm, 1000.0f) << "lugging in gear " << dc.gear
                                       << ", tick " << dc.tick;
        }
        if (dc.spinning) {
            EXPECT_GE(dc.wheel_mps, dc.v_mps) << "tick " << dc.tick;
            EXPECT_LE(dc.slip(), 0.5f) << "tick " << dc.tick;
        }

        const float estimate = ecuio_mock::maf_wot_gs(dc.rpm) * dc.load_pct / 100.0f;
        EXPECT_NEAR(dc.maf_gs, estimate, 0.1f * estimate + 0.5f) << "tick " << dc.tick;
        EXPECT_GT(dc.maf_gs, 5.0f) << "engine running, tick " << dc.tick;
        EXPECT_GE(dc.throttle_pct, ecuio_mock::kTpClosedPct - 0.01f);
        EXPECT_LE(dc.throttle_pct, ecuio_mock::kTpWotPct + 0.01f);

        cut_run = dc.fuel_cut ? cut_run + 1 : 0;
        fueled_run = dc.fuel_cut ? 0 : fueled_run + 1;
        /* The rev limit cuts fuel the tick after rpm meets it, whatever
         * the phase: a lift off the limiter doesn't sit there fueled */
        fueled_at_limit = (!limiter && !dc.fuel_cut && dc.rpm >= ecuio_mock::kLimiterOnRpm)
            ? fueled_at_limit + 1 : 0;
        EXPECT_LE(fueled_at_limit, 1u) << "fueled at the rev limit, tick " << dc.tick;
        if (cut_run >= 2) {
            EXPECT_GT(dc.lambda, 1.3f) << "fuel cut, tick " << dc.tick;
        }
        if (fueled_run >= 3) {
            EXPECT_GE(dc.lambda, 0.8f) << "tick " << dc.tick;
            EXPECT_LE(dc.lambda, 1.1f) << "tick " << dc.tick;
        }

        EXPECT_LE(std::fabs(dc.coolant_c - prev.coolant_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.oil_c - prev.oil_c), 0.5f) << "tick " << dc.tick;
        EXPECT_LE(std::fabs(dc.iat_c - prev.iat_c), 0.5f) << "tick " << dc.tick;
        prev = dc;
    }
    ASSERT_EQ(dc.loops, 3u) << "three loops never completed";
    EXPECT_GT(checked_locked, 1000) << "too few in-gear ticks checked";
}

TEST(CoreTest, MockDriveCycleReadingsNeverHoldDeadStill)
{
    /* A running engine's rpm, as the wire carries it, never holds one
     * value for long: the idle hunts, the driver's speed wanders, the
     * crank reading jitters. And away from the moments that are quick
     * on a real car (a shift, the limiter, the launch and the drag run),
     * rpm never steps between ticks by more than an engine can change,
     * phase changes included. */
    DriveCycle dc;
    uint8_t payload[ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN] = {};
    uint16_t last_raw = 0;
    uint32_t same = 0, longest = 0;
    float prev_rpm = dc.rpm;
    float worst_step = 0.0f;
    uint64_t worst_tick = 0;
    while (dc.loops < 2 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ecuio_mock::encode_payload(dc, payload);
        const uint16_t raw = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
        same = (dc.tick > 1 && raw == last_raw) ? same + 1 : 0;
        longest = std::max(longest, same);
        last_raw = raw;

        const bool quick = dc.shifting() || dc.phase == Phase::Limiter2nd ||
                           dc.phase == Phase::Limiter3rd || dc.phase == Phase::Launch ||
                           dc.phase == Phase::DragRun;
        if (!quick && std::fabs(dc.rpm - prev_rpm) > worst_step) {
            worst_step = std::fabs(dc.rpm - prev_rpm);
            worst_tick = dc.tick;
        }
        prev_rpm = dc.rpm;
    }
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";
    EXPECT_LE(longest, 10u) << "rpm read the same for over half a second";
    EXPECT_LE(worst_step, 150.0f) << "rpm stepped at tick " << worst_tick;
}

TEST(CoreTest, MockDriveCycleLimiterCutsFuelNotAir)
{
    /* On the limiter the blade stays open, airflow stays near WOT and the
     * fuel is cut most of each cycle: lambda reads lean, load pegged.
     * Road speed holds while rpm saws. */
    float pull_maf = 0.0f;
    for_each_tick_of(Phase::Pull2nd, [&](const DriveCycle& dc) {
        pull_maf = std::fmax(pull_maf, dc.maf_gs);
    });
    float min_load = 100.0f, min_maf = 1e9f, max_lambda = 0.0f;
    float min_kmh = 1e9f, max_kmh = 0.0f;
    uint32_t cut = 0, ticks = 0;
    for_each_tick_of(Phase::Limiter2nd, [&](const DriveCycle& dc) {
        EXPECT_FLOAT_EQ(dc.throttle_pct, ecuio_mock::kTpWotPct);
        min_load = std::fmin(min_load, dc.load_pct);
        min_maf = std::fmin(min_maf, dc.maf_gs);
        max_lambda = std::fmax(max_lambda, dc.lambda);
        if (dc.phase_ticks >= ecuio_mock::ticks_for(1.0f)) {
            min_kmh = std::fmin(min_kmh, dc.speed_kmh);
            max_kmh = std::fmax(max_kmh, dc.speed_kmh);
        }
        cut += dc.fuel_cut ? 1u : 0u;
        ++ticks;
    });
    ASSERT_EQ(ticks, ecuio_mock::ticks_for(5.0f));
    EXPECT_GE(min_load, 95.0f);
    EXPECT_GE(min_maf, 0.9f * pull_maf);
    EXPECT_GT(max_lambda, 1.6f);
    EXPECT_GT(cut, ticks / 2) << "the limiter should spend most of its time in cut";
    EXPECT_LE(max_kmh - min_kmh, 1.0f) << "speed should hold on the limiter";
}

TEST(CoreTest, MockDriveCycleHighwayCruiseHoldsSpeed)
{
    /* A light, steady cruise in top gear: closed-loop fueling, the low
     * load and airflow of road-load power. */
    float lo_kmh = 1e9f, hi_kmh = 0.0f, lo_rpm = 1e9f, hi_rpm = 0.0f;
    float hi_load = 0.0f, hi_maf = 0.0f, lo_lambda = 9.0f, hi_lambda = 0.0f;
    uint32_t ticks = 0;
    for_each_tick_of(Phase::Cruise4th, [&](const DriveCycle& dc) {
        EXPECT_EQ(dc.gear, ecuio_mock::kTopGear);
        if (dc.phase_ticks < ecuio_mock::ticks_for(5.0f)) {
            return;   /* settling from the lift */
        }
        lo_kmh = std::fmin(lo_kmh, dc.speed_kmh);
        hi_kmh = std::fmax(hi_kmh, dc.speed_kmh);
        lo_rpm = std::fmin(lo_rpm, dc.rpm);
        hi_rpm = std::fmax(hi_rpm, dc.rpm);
        hi_load = std::fmax(hi_load, dc.load_pct);
        hi_maf = std::fmax(hi_maf, dc.maf_gs);
        lo_lambda = std::fmin(lo_lambda, dc.lambda);
        hi_lambda = std::fmax(hi_lambda, dc.lambda);
        ++ticks;
    });
    ASSERT_GE(ticks, ecuio_mock::ticks_for(9.0f));
    EXPECT_GE(lo_kmh, 98.0f);
    EXPECT_LE(hi_kmh, 102.0f);
    EXPECT_GE(lo_rpm, 2850.0f);
    EXPECT_LE(hi_rpm, 3100.0f);
    EXPECT_LE(hi_load, 35.0f);
    EXPECT_LE(hi_maf, 35.0f);
    EXPECT_GE(lo_lambda, 0.97f);
    EXPECT_LE(hi_lambda, 1.03f);
}

TEST(CoreTest, MockDriveCycleColdStartIdlesFastThenSettles)
{
    /* The cold-start strategy as the first vehicle showed it: a fast
     * idle near 1430 rpm on extra air for ~20 s, open-loop fueling, then
     * an 850-900 rpm warm-up idle. */
    float early_lo = 1e9f, early_hi = 0.0f, late_rpm = 0.0f, fast_load = 0.0f;
    float fast_tp = 0.0f, lambda_lo = 9.0f, lambda_hi = 0.0f;
    for_each_tick_of(Phase::WarmUp, [&](const DriveCycle& dc) {
        if (dc.phase_ticks < ecuio_mock::ticks_for(15.0f)) {
            early_lo = std::fmin(early_lo, dc.rpm);
            early_hi = std::fmax(early_hi, dc.rpm);
            fast_load = std::fmax(fast_load, dc.load_pct);
            fast_tp = std::fmax(fast_tp, dc.throttle_pct);
        }
        late_rpm = dc.rpm;
        lambda_lo = std::fmin(lambda_lo, dc.lambda);
        lambda_hi = std::fmax(lambda_hi, dc.lambda);
    });
    EXPECT_GE(early_lo, 1400.0f);
    EXPECT_LE(early_hi, 1460.0f);
    EXPECT_GE(fast_load, 40.0f) << "catalyst heating runs on extra air";
    EXPECT_GT(fast_tp, 30.0f);
    EXPECT_GE(late_rpm, 850.0f);
    EXPECT_LE(late_rpm, 900.0f);
    EXPECT_NEAR(lambda_lo, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
    EXPECT_NEAR(lambda_hi, ecuio_mock::kLambdaColdOpenLoop, 0.01f);
}

TEST(CoreTest, MockDriveCycleLaunchSpinsTheTiresAndHooksUp)
{
    /* Off the line the clutch slips while the tires sit near peak slip;
     * once it locks in 1st the driver feathers the throttle with the
     * tires spinning a few hundred rpm above road speed. From 2nd the
     * drive is below the grip and the tires hook up. */
    float flare_hi = 0.0f;
    float slip_hi = 0.0f;
    float clutch_rpm_lo = 1e9f;
    bool spun_in_second = false;
    for_each_tick_of(Phase::DragRun, [&](const DriveCycle& dc) {
        if (dc.clutch_slip) {
            clutch_rpm_lo = std::fmin(clutch_rpm_lo, dc.rpm);
        }
        if (dc.gear == 1 && !dc.clutch_slip && !dc.shifting()) {
            const float ground_rpm = dc.v_mps * ecuio_mock::rpm_per_mps(1);
            flare_hi = std::fmax(flare_hi, dc.rpm - ground_rpm);
            slip_hi = std::fmax(slip_hi, dc.slip());
        }
        if (dc.gear >= 2 && dc.spinning) {
            spun_in_second = true;
        }
    });
    EXPECT_GE(clutch_rpm_lo, 3000.0f) << "the engine bogged off the line";
    EXPECT_GE(flare_hi, 300.0f);
    EXPECT_LE(flare_hi, 800.0f);
    EXPECT_GT(slip_hi, 0.05f);
    EXPECT_LT(slip_hi, 0.3f);
    EXPECT_FALSE(spun_in_second);
}

namespace {

struct ArbIdCapture {
    std::atomic<int> telemetry_count{0};
    std::atomic<int> diag_count{0};
    std::atomic<int> other_count{0};
    std::atomic<int> extended_mismatch{0};
    std::atomic<int> fd_mismatch{0};
    std::atomic<uint64_t> last_ts{0};
    std::atomic<int> ts_regression{0};
};

void arb_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    ArbIdCapture* c = static_cast<ArbIdCapture*>(user_data);
    const uint64_t prev = c->last_ts.exchange(frame->timestamp_us);
    if (frame->timestamp_us < prev) {
        c->ts_regression.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame->arbitration_id == 0x0C0) {
        c->telemetry_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & ECUIO_FRAME_FLAG_EXTENDED) != 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
        if ((frame->flags & ECUIO_FRAME_FLAG_FD) == 0) {
            c->fd_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (frame->arbitration_id == 0x18FF0C00) {
        c->diag_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & ECUIO_FRAME_FLAG_EXTENDED) == 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
        if ((frame->flags & ECUIO_FRAME_FLAG_FD) == 0) {
            c->fd_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        c->other_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

/* Contract: the mock emits exactly two arbitration IDs — the
 * telemetry ID the signal set declares (0x0C0) and a 29-bit diagnostic
 * ID every 16th frame — with FLAG_EXTENDED set iff the ID is 29-bit.
 * Combined with MockSignalsDeclareTelemetryMessageId (test_signals),
 * this pins the two halves of the bridge's decode gate; the gate line
 * itself is exercised end-to-end by MockFramesDecodeAgainstMockSignalSet
 * continuing to produce sane values from 0x0C0 traffic only. */
TEST(CoreTest, MockEmitsKnownArbitrationIds)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://arb", &session), ECUIO_OK);

    ArbIdCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &arb_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Enough ticks for several 16-frame cycles incl. diag frames */
    wait_for([&cap]() {
        return cap.diag_count.load() >= 2 && cap.telemetry_count.load() >= 30;
    }, 10000);

    ecuio_session_close(session);
    ecuio_session_destroy(session);

    EXPECT_EQ(cap.other_count.load(), 0);
    EXPECT_EQ(cap.extended_mismatch.load(), 0);
    EXPECT_GE(cap.diag_count.load(), 2);
    EXPECT_GT(cap.telemetry_count.load(), cap.diag_count.load() * 4);
    /* Session-relative timestamps must never regress (header contract) */
    EXPECT_EQ(cap.ts_regression.load(), 0);
    /* First sample lands within the first tick, not microseconds since
     * boot — session-relative, not absolute. */
    EXPECT_LT(cap.last_ts.load(), uint64_t{600} * 1000 * 1000 * 1000);
    EXPECT_EQ(cap.fd_mismatch.load(), 0) << "FD flag missing on one or more frames";
}

namespace {

struct TickCapture {
    std::atomic<int> count{0};
    uint64_t ts[96] = {}; /* read after close, which joins the worker */
};

void tick_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    TickCapture* c = static_cast<TickCapture*>(user_data);
    const int i = c->count.fetch_add(1);
    if (i < 96) {
        c->ts[i] = frame->timestamp_us;
    }
}

} // namespace

/* Contract: one frame per kTickS of wall time. The drive cycle advances
 * kTickS per frame, so a slower worker plays the car in slow motion
 * against the frames' timestamps (a 0-100 time, the limiter's period).
 *
 * The worker sleeps to a deadline (due += interval) and restarts the
 * schedule only when a wake comes more than an interval late. So between
 * restarts every frame is sent between its due time and one interval
 * past it, and a run of k gaps sums to 50 k ms within one interval,
 * whatever each gap is: on Windows they land on the 15.6 ms sleep step
 * (about 47 or 62 ms). A stall, a gap over two intervals, is left out
 * of the mean. Every restart sends its next frame at once, a gap under
 * half an interval, and each stall or restart moves the sum by less than
 * an interval. Counting stalls and short gaps as e events, the mean of n
 * gaps is 50 ms within 50 (e + 1) / n, plus a millisecond for the
 * clock.
 *
 * Sleeping a fixed 50 ms after each tick makes every gap the long one:
 * 62 ms on Windows, which fails this at once. On Linux a fixed sleep
 * overshoots by a fraction of a millisecond, which no 4 s run can tell
 * from a deadline. A host that stalls so often that the bound can't see
 * a worker 10 % slow skips, and ctest reports the skip as one: the
 * macos-build runner wakes 50 ms sleeps 100-200 ms late, a third of the
 * gaps at a time. */
TEST(CoreTest, MockFramesKeepTheTickInterval)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://cadence", &session), ECUIO_OK);

    TickCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, &tick_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    constexpr int kFrames = 81; /* 80 gaps, 4 s */
    wait_for([&]() { return cap.count.load() >= kFrames; }, 20000);

    ecuio_session_close(session);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count.load(), kFrames);
    double sum_ms = 0.0;
    int counted = 0;
    int stalls = 0;
    int events = 0; /* stalls, and gaps under half an interval */
    std::string gaps; /* every gap, for a failure on a host we can't run */
    for (int i = 1; i < kFrames; ++i) {
        const double gap_ms =
            static_cast<double>(cap.ts[i] - cap.ts[i - 1]) / 1000.0;
        gaps += std::to_string(static_cast<int>(gap_ms)) + " ";
        if (gap_ms <= 100.0) {
            sum_ms += gap_ms;
            ++counted;
            if (gap_ms < 25.0)
                ++events;
        } else {
            ++stalls;
            ++events;
        }
    }
    ASSERT_GT(counted, 0) << "gaps (ms): " << gaps;
    const double bound_ms = 50.0 * (events + 1) / counted + 1.0;
    if (bound_ms >= 5.0) {
        GTEST_SKIP() << "this host stalled the worker " << stalls
                     << " times (" << events << " stalls and restarts), "
                        "so the bound (+/- "
                     << bound_ms << " ms) can't see a worker 10 % slow; "
                        "gaps (ms): " << gaps;
    }
    const double mean_ms = sum_ms / counted;
    EXPECT_GE(mean_ms, 50.0 - bound_ms) << "gaps (ms): " << gaps;
    EXPECT_LE(mean_ms, 50.0 + bound_ms)
        << "the drive cycle would play slower than real time; gaps (ms): "
        << gaps;
}

/* Session time's clock (ADR-0033 §1) never goes back, nor does its slept
 * count, and while the machine is awake it keeps the waits' pace */
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
     * RUNNING session must join the worker and free — no hang, no leak. */
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
     * from lookup failure — presence and content are separate axes,
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
    EXPECT_EQ(set, &sentinel);   /* untouched on failure — caller may
                                    rely on its own initialization */
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

    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-why", mock, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(v.index, UINT32_MAX);
    EXPECT_EQ(ecuio_profile_unregister("test-why"), ECUIO_OK);
}

TEST(CoreTest, ProfileRegisterRefusesMoreRequestsThanTheEngineHolds) {
    /* A set may derive at most ECUIO_PROFILE_MAX_REQUESTS requests
     * (ADR-0015 §2), so a session never leaves one unasked with nothing
     * to say why. Defs read from one reply count once: def 1 reads PID
     * 0x00's reply, as def 0 does. One past the limit is refused, naming
     * the first def that didn't fit, and leaves nothing registered. */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    static ecuio_signal_def_t defs[ECUIO_PROFILE_MAX_REQUESTS + 2];
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS + 2; ++i) {
        defs[i] = obd2->defs[0];
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].mux_key = 0x4100u | ((i == 1) ? 0u : i);
    }
    static ecuio_signal_set_t fits = {};
    fits.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    fits.count = ECUIO_PROFILE_MAX_REQUESTS + 1;
    fits.defs = defs;
    fits.channel_count = obd2->channel_count;
    fits.channels = obd2->channels;
    ecuio_signal_set_t over = fits;
    over.count = ECUIO_PROFILE_MAX_REQUESTS + 2;

    ecuio_validation_t v;
    EXPECT_EQ(ecuio_profile_register("test-requests", &fits, &v), ECUIO_OK);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_OK);
    EXPECT_EQ(ecuio_profile_unregister("test-requests"), ECUIO_OK);

    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(ecuio_profile_register("test-requests", &over, &v),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_REQUEST_COUNT);
    EXPECT_EQ(v.index, ECUIO_PROFILE_MAX_REQUESTS + 1);
    EXPECT_EQ(v.reserved[0], 0u);
    EXPECT_EQ(v.reserved[1], 0u);
    const ecuio_signal_set_t* got = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("test-requests", &got), ECUIO_ERR_UNKNOWN_PROFILE);
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
    /* Scheme default: mock:// → the mock profile's set. */
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://profile-default", &session),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_mock_signal_set());
    ecuio_session_destroy(session);
}

TEST(CoreTest, ProfileOverrideRawYieldsNullSet) {
    /* ?profile=raw on a mock session: override to the null profile.
     * ALSO the probe for mock-transport query tolerance (ASSUMED —
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
     * session_create — cross-platform, scheme-independent. */
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?profile=bogus", &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

#ifdef _WIN32
TEST(CoreTest, UnknownProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, Windows-only: bogus profile on a
     * NONEXISTENT DLL path. Profile validation must run BEFORE
     * transport attach — so this yields ECUIO_ERR_UNKNOWN_PROFILE, not
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
// ADR-0013: profile query tri-state — a present-but-malformed
// ?profile= value must fail session_create, never silently downgrade
// to the scheme default. Core is as strict about ?profile= as
// parse_query is about every other key.
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileQueryEmptyValueRejected) {
    /* ?profile= with nothing after '=': PRESENT but malformed — the
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
     * the boundary — the 64 case above pins the rejecting side. */
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

    /* Reverse order: valid first, malformed LAST — a later malformed
     * value still poisons the URI (it is not a "drop the bad one,
     * keep scanning" grammar — it is strict). */
    EXPECT_EQ(ecuio_session_create("mock://x?profile=mock&profile=",
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryLastValidWinsAcrossDuplicates) {
    /* Across WELL-FORMED values, the last wins — matching the
     * documented duplicate-key rule for every other URI parameter.
     * Distinguished from the malformed cases above by using a real,
     * registered name that differs from the scheme default. */
    const std::string name63(63, 'q');   /* registered by the boundary
                                            test above — but tests
                                            shouldn't depend on order. */
    ASSERT_EQ(ecuio_profile_register(name63.c_str(),
                                     ecuio_mock_signal_set(), nullptr), ECUIO_OK);

    std::string uri1 = "mock://x?profile=raw&profile=" + name63;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri1.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session),
              ecuio_mock_signal_set());    /* last (name63) won */
    ecuio_session_destroy(session);

    /* And the inverse order: raw last → null set */
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

namespace {

/* The DID replies the mock answers with, kept whole: at most kMax, each
 * one a frame (4 KB) on the heap. */
struct DidCapture {
    static constexpr size_t kMax = 24;
    std::vector<ecuio_frame_t> frames;
    std::atomic<size_t> count{0};
};

void capture_did_cb(const ecuio_frame_t* frame, void* user_data)
{
    DidCapture* cap = static_cast<DidCapture*>(user_data);
    if (frame->data_length >= 3 && frame->data[0] == 0x62 &&
        cap->count.load() < DidCapture::kMax) {
        cap->frames[cap->count.load()] = *frame;
        cap->count.fetch_add(1);
    }
}

} // namespace

/* Under uds-generic the mock answers each of the session's DIDs every
 * tick, from the ECU the def names, so the app shows the profile with no
 * hardware: a number that counts the ticks and a text naming the DID. */
TEST(CoreTest, MockAnswersTheSessionsDids) {
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(
        "mock://dids?profile=uds-generic&did=1E1C:u16,F190:text,7E9:1E1C:u8",
        &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_EQ(set->count, 3u);
    DidCapture cap;
    cap.frames.resize(DidCapture::kMax);
    ASSERT_EQ(ecuio_register_frame_callback(session, &capture_did_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&cap]() { return cap.count.load() >= 9; }, 5000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);   /* joins the worker */

    ASSERT_GE(cap.count.load(), size_t{9}) << "three DIDs over three ticks";
    size_t seen[3] = {0, 0, 0};
    double last_word = -1.0;
    for (size_t f = 0; f < cap.count.load(); ++f) {
        const ecuio_frame_t& frame = cap.frames[f];
        EXPECT_EQ(frame.flags, 0u) << "a classic-CAN reply, as J2534 delivers";
        bool applied = false;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (!ecuio_def_applies_to_frame(&d, &frame)) {
                continue;
            }
            applied = true;
            ++seen[i];
            if (d.kind == ECUIO_VALUE_TEXT) {
                char text[64] = "";
                ASSERT_EQ(ecuio_decode_text(&d, frame.data, frame.data_length,
                                            text, sizeof(text)), ECUIO_DECODE_OK);
                EXPECT_EQ(std::string(text).rfind("MOCK F190 #", 0), size_t{0}) << text;
            } else {
                double v = 0.0;
                ASSERT_EQ(ecuio_decode_signal(&d, frame.data, frame.data_length, &v),
                          ECUIO_DECODE_OK);
                if (i == 0) {
                    EXPECT_GT(v, last_word) << "the tick count grows";
                    last_word = v;
                    EXPECT_EQ(frame.arbitration_id, 0x7E8u);
                } else {
                    EXPECT_EQ(frame.arbitration_id, 0x7E9u) << "the ECU the prefix named";
                    EXPECT_EQ(frame.data_length, 4u) << "u8: one value byte";
                }
            }
        }
        EXPECT_TRUE(applied) << "frame " << f << " answers no def";
    }
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_GE(seen[i], size_t{3}) << set->defs[i].name;
    }
    ecuio_session_destroy(session);
}

// ---- replay:// (ADR-0023) ----

namespace {

/* A scratch recording in the temp folder, named for the running test and
 * removed at the end. path is absolute and uses forward slashes, which
 * the replay path accepts on every platform. */
struct TempRecording {
    std::string dir;
    std::string csv;
    bool made_dir = false;

    explicit TempRecording(const std::string& text, bool as_folder = false)
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr)
            tmp = std::getenv("TMPDIR");
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\')
                c = '/';
        }
        while (base.size() > 1 && base.back() == '/')
            base.pop_back();
        const std::string name =
            std::string("ecuio-replay-") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        if (as_folder) {
            dir = base + "/" + name;
#ifdef _WIN32
            made_dir = _mkdir(dir.c_str()) == 0 || errno == EEXIST;
#else
            made_dir = mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
#endif
            csv = dir + "/frames.csv";
        } else {
            csv = base + "/" + name + ".csv";
        }
        FILE* f = std::fopen(csv.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ~TempRecording()
    {
        std::remove(csv.c_str());
        if (made_dir) {
#ifdef _WIN32
            _rmdir(dir.c_str());
#else
            rmdir(dir.c_str());
#endif
        }
    }
    std::string uri(const char* query = "") const
    {
        return "replay://" + (dir.empty() ? csv : dir) + query;
    }
};

const char* const kFramesHeader = "host_us,timestamp_us,id,flags,data\n";

/* Every frame a replay delivers, and when it arrived */
struct ReplayCapture {
    std::mutex mutex;
    std::vector<ecuio_frame_t> frames;
    std::vector<std::chrono::steady_clock::time_point> arrived;
    /* The callback holds the worker this long at this frame (a late
     * frame); 0 never */
    size_t stall_at = 0;
    int stall_ms = 0;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }
};

void replay_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<ReplayCapture*>(user_data);
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(c->mutex);
        c->frames.push_back(*frame);
        c->arrived.push_back(std::chrono::steady_clock::now());
        n = c->frames.size();
    }
    if (c->stall_at != 0 && n == c->stall_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds(c->stall_ms));
    }
}

/* Creates, registers and starts a replay session, or fails the test */
ecuio_session_t* start_replay(const std::string& uri, ReplayCapture* capture)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    if (session == nullptr)
        return nullptr;
    EXPECT_EQ(
        ecuio_register_frame_callback(session, replay_capture_cb, capture),
        ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    return session;
}

/* Rows at step_us steps of session time, 0x7E8 and 41 0C 1F 40 each */
std::string paced_rows(int n, int step_us = 100000)
{
    std::string text = kFramesHeader;
    for (int i = 0; i < n; ++i) {
        const std::string t = std::to_string(5000000 + int64_t{step_us} * i);
        text += t + "," + t + ",0x7E8,0x0,41 0C 1F 40\n";
    }
    return text;
}

/* Reads the worker's own operator new count at two frames past its
 * start-up; the callback itself allocates nothing */
struct WorkerNews {
    static constexpr int kFrom = 3;
    int to = 0;
    std::atomic<int> frames{0};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
    int batches = 0; /* values batches, counted on the worker */
};

void worker_news_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<WorkerNews*>(user_data)->batches;
}

void worker_news_cb(const ecuio_frame_t*, void* user_data)
{
    auto* w = static_cast<WorkerNews*>(user_data);
    const int n = w->frames.fetch_add(1) + 1;
    if (n == WorkerNews::kFrom)
        w->at_from.store(new_counter::on_this_thread());
    if (n == w->to)
        w->at_to.store(new_counter::on_this_thread());
}

} // namespace

/* Past start-up a worker's frames cost it no allocation, so none can
 * throw: the mock's ticks, and replay's rows and passes (the file
 * reopened each time round), each decoded for a values callback too */
TEST(CoreTest, WorkersAllocateNothingPerFrame)
{
    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?speed=max&loop=1&profile=obd2");
    struct Case {
        const char* uri;
        int to;
    };
    for (const Case& c : {Case{"mock://demo", 12}, Case{replay.c_str(), 200}}) {
        SCOPED_TRACE(c.uri);
        WorkerNews w;
        w.to = c.to;
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(c.uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_register_frame_callback(session, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, worker_news_values_cb, &w),
            ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= c.to; }, 5000);
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        ASSERT_TRUE(reached) << w.frames.load() << " frames";
        EXPECT_GT(w.batches, 0) << "the values path ran";
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << c.to;
    }
}

/* Each row goes out as recorded (ID, flags, bytes, an empty payload
 * included), stamped with the recording's session time rebased to the
 * first row; the session info says what's playing */
TEST(CoreTest, ReplaySendsTheRecordedFramesAtTheirRebasedTimes)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "10,1000000,0x7E8,0x0,41 0C 1F 40\n"
                      "600,1000500,0x18DAF110,0x1,\n"
                      "2100,1002000,0x7E8,0x40,62 F1 90 41 42\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));

    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_REPLAY);
    EXPECT_EQ(info.replay_frames, 3u);
    EXPECT_EQ(info.replay_span_ms, 2u);
    EXPECT_EQ(info.replay_speed_milli, 0u) << "max";
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    EXPECT_EQ(info.channel_count, 0u) << "passive";
    EXPECT_STREQ(info.profile, "raw") << "replay's default";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int32_t last = -1;
    uint32_t episodes = 99;
    ASSERT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
              ECUIO_OK);
    EXPECT_EQ(last, ECUIO_OK) << "the end of the file isn't an error";
    EXPECT_EQ(episodes, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);

    ASSERT_EQ(cap.frames.size(), 3u)
        << "the end of the file sends nothing more";
    const ecuio_frame_t& a = cap.frames[0];
    EXPECT_EQ(a.arbitration_id, 0x7E8u);
    EXPECT_EQ(a.flags, 0u);
    EXPECT_EQ(a.timestamp_us, 0u);
    ASSERT_EQ(a.data_length, 4u);
    EXPECT_EQ(a.data[0], 0x41);
    EXPECT_EQ(a.data[3], 0x40);
    const ecuio_frame_t& b = cap.frames[1];
    EXPECT_EQ(b.arbitration_id, 0x18DAF110u);
    EXPECT_EQ(b.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(b.data_length, 0u);
    EXPECT_EQ(b.timestamp_us, 500u);
    const ecuio_frame_t& c = cap.frames[2];
    EXPECT_EQ(c.flags, 0x40u) << "flags as recorded, TRUNCATED among them";
    EXPECT_EQ(c.data_length, 5u);
    EXPECT_EQ(c.timestamp_us, 2000u);
    ecuio_session_destroy(session);
}

/* A raw adapter clock that wraps steps timestamp_us backwards: that row
 * is timed by its host_us step instead, so session time never decreases */
TEST(CoreTest, ReplayTimesABackwardStampByTheHostClock)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,4294967000,0x7E8,0x0,41 0D 00\n"
                      "200,4294967200,0x7E8,0x0,41 0D 01\n"
                      "600,100,0x7E8,0x0,41 0D 02\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].timestamp_us, 0u);
    EXPECT_EQ(cap.frames[1].timestamp_us, 200u);
    EXPECT_EQ(cap.frames[2].timestamp_us, 600u) << "the host's 400 us step";
    ecuio_session_destroy(session);
}

/* loop=1 plays the file again, its stamps carried on from the last pass */
TEST(CoreTest, ReplayLoopsWithStampsThatNeverDecrease)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "1000,1000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri("?speed=max&loop=1"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 6; }, 2000));
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP, 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    for (size_t i = 1; i < cap.frames.size(); ++i) {
        ASSERT_GE(cap.frames[i].timestamp_us, cap.frames[i - 1].timestamp_us)
            << "frame " << i;
    }
    EXPECT_EQ(cap.frames[2].timestamp_us, 1000u)
        << "the second pass starts where the first ended";
    EXPECT_EQ(cap.frames[3].timestamp_us, 2000u);
    EXPECT_EQ(cap.frames[2].data[2], 0x00) << "from the top";
    ecuio_session_destroy(session);
}

/* A recording whose frames all share one time (one frame, or several
 * stamped alike) has no span to pace a pass by: ?loop=1 is refused at
 * create, where the worker would spin; played once, it plays */
TEST(CoreTest, ReplayRefusesToLoopARecordingWithNoSpan)
{
    struct Case {
        const char* rows;
        size_t frames;
    };
    for (const Case& c : {Case{"5,7,0x7E8,0x0,41 0D 00\n", 1},
                          Case{"5,7,0x7E8,0x0,41 0D 00\n"
                               "6,7,0x7E8,0x0,41 0D 01\n", 2}}) {
        TempRecording rec(std::string(kFramesHeader) + c.rows);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri("?loop=1").c_str(), &session),
                  ECUIO_ERR_BAD_URI);
        EXPECT_EQ(session, nullptr);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "?loop=1 needs a recording that spans time; this one's "
                     "frames all share one time");

        ReplayCapture cap;
        session = start_replay(rec.uri("?speed=max"), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= c.frames; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        EXPECT_EQ(cap.frames.size(), c.frames) << "played once";
        ecuio_session_destroy(session);
    }
}

/* One second of recording takes about a second at speed 1 and a tenth of
 * that at 10; the frames keep the recording's times either way */
TEST(CoreTest, ReplayPacesByTheRecordingsTimeOverTheSpeed)
{
    TempRecording rec(paced_rows(11));
    for (const char* q : {"", "?speed=10"}) {
        SCOPED_TRACE(q);
        ReplayCapture cap;
        ecuio_session_t* session = start_replay(rec.uri(q), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 11; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        if (q[0] == '\0') {
            EXPECT_GE(ms, 900) << "real time";
            EXPECT_LE(ms, 1600);
        } else {
            EXPECT_GE(ms, 80) << "ten times faster";
            EXPECT_LE(ms, 400);
        }
        EXPECT_EQ(cap.frames.back().timestamp_us, 1000000u)
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Each frame is due at its time from the start of playback, not a step
 * after the last send: a frame held up 400 ms is followed at once by the
 * ones it delayed, and a second of recording still takes about a second.
 * Waiting each step from the last send would add the stall, and on
 * Windows every short wait's rounding, so 200 steps of 5 ms (speed 1) or
 * 50 ms at 10x (5 ms waits) show it either way. */
TEST(CoreTest, ReplayCatchesUpAfterALateFrameAndNeverDrifts)
{
    struct Case {
        const char* query;
        int step_us;
    };
    for (const Case& c : {Case{"", 5000}, Case{"?speed=10", 50000}}) {
        SCOPED_TRACE(c.query);
        TempRecording rec(paced_rows(201, c.step_us));
        ReplayCapture cap;
        cap.stall_at = 20;
        cap.stall_ms = 400;
        ecuio_session_t* session = start_replay(rec.uri(c.query), &cap);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= 201; }, 5000));
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            cap.arrived.back() - cap.arrived.front())
                            .count();
        EXPECT_GE(ms, 950) << "the last frame is due a second in";
        EXPECT_LE(ms, 1250) << "late frames catch up; the stall isn't added";
        EXPECT_EQ(cap.frames.back().timestamp_us,
                  200u * static_cast<uint64_t>(c.step_us))
            << "the recording's time";
        ecuio_session_destroy(session);
    }
}

/* Attach checked the file, but it can change before the worker reads it
 * again: playback sends the rows up to the first bad one, then stops, and
 * the session still closes cleanly */
TEST(CoreTest, ReplayStopsAtARowThatTurnedBadAfterAttach)
{
    TempRecording rec(paced_rows(5));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    {
        const std::string rows = paced_rows(3);
        const std::string cut =
            rows + "5300000,5300000,0x7E8,0x0,41 0"; // cut mid-row
        FILE* f = std::fopen(rec.csv.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(cut.data(), 1, cut.size(), f);
        std::fclose(f);
    }
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 3; }, 2000));
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED) << "says why it stopped";
    EXPECT_EQ(episodes, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cap.count(), 3u) << "nothing past the bad row";
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A recording cut short after attach plays what's left, then says why it
 * stopped: a reader counting to attach's rows would wait forever. One
 * that grew plays attach's rows and ends as at the end of its file */
TEST(CoreTest, ReplayOfARecordingWithOtherRowsPlaysAttachsAtMost)
{
    struct Case {
        int rows_now;
        ecuio_status_t state;
    };
    for (const Case& c :
         {Case{3, ECUIO_ERR_RECORDING_CHANGED}, Case{7, ECUIO_OK}}) {
        SCOPED_TRACE(c.rows_now);
        TempRecording rec(paced_rows(5));
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        {
            const std::string rows = paced_rows(c.rows_now);
            FILE* f = std::fopen(rec.csv.c_str(), "wb");
            ASSERT_NE(f, nullptr);
            std::fwrite(rows.data(), 1, rows.size(), f);
            std::fclose(f);
        }
        ReplayCapture cap;
        ASSERT_EQ(
            ecuio_register_frame_callback(session, replay_capture_cb, &cap),
            ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
        const size_t played =
            static_cast<size_t>(c.rows_now < 5 ? c.rows_now : 5);
        ASSERT_TRUE(wait_for([&] { return cap.count() >= played; }, 2000));
        int32_t last = ECUIO_OK;
        uint32_t episodes = 0;
        if (c.state != ECUIO_OK) {
            ASSERT_TRUE(wait_for(
                [&] {
                    ecuio_session_get_error_state(session, &last, &episodes);
                    return last != ECUIO_OK;
                },
                2000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ecuio_session_get_error_state(session, &last, &episodes);
        EXPECT_EQ(last, c.state);
        EXPECT_EQ(episodes, c.state == ECUIO_OK ? 0u : 1u);
        EXPECT_EQ(cap.count(), played) << "attach's rows at most";
        ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
    }
}

/* A recording deleted between attach and start: nothing to play, and the
 * error state says so */
TEST(CoreTest, ReplayOfARecordingThatWentAwaySaysSo)
{
    TempRecording rec(paced_rows(3));
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(rec.uri("?speed=max").c_str(), &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(std::remove(rec.csv.c_str()), 0);
    ReplayCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, replay_capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_session_get_error_state(session, &last, &episodes);
            return last != ECUIO_OK;
        },
        2000));
    EXPECT_EQ(last, ECUIO_ERR_RECORDING_CHANGED);
    EXPECT_EQ(episodes, 1u);
    EXPECT_EQ(cap.count(), 0u);
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A long gap in the recording doesn't hold up a close */
TEST(CoreTest, ReplayClosesAtOnceDuringAGap)
{
    TempRecording rec(std::string(kFramesHeader) +
                      "0,0,0x7E8,0x0,41 0D 00\n"
                      "3600000000,3600000000,0x7E8,0x0,41 0D 01\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(rec.uri(), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 1; }, 2000));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    EXPECT_EQ(cap.count(), 1u) << "the hour-later frame never went out";
    ecuio_session_destroy(session);
}

/* Attach reads the whole file: anything but a frames.csv, or a bad row,
 * fails create with its line number. Either line ending reads, and a
 * whole-message payload arrives whole. */
TEST(CoreTest, ReplayRefusesABadRecordingWithItsLineNumber)
{
    const std::string ok_row = "1,1,0x7E8,0x0,41 0C 1F 40\n";
    struct Case {
        std::string text;
        const char* detail;
    };
    const std::vector<Case> cases = {
        {"", "the recording's frames.csv is empty"},
        {"host_us,signal,value,unit\n1,RPM,800,RPM\n",
         "not a Havne frames.csv, or a newer format"},
        {kFramesHeader, "the recording's frames.csv holds no frames"},
        {std::string(kFramesHeader) + ok_row + "2,2,7E8,0x0,41\n",
         "frames.csv line 3: id isn't 0x-prefixed hex"},
        {std::string(kFramesHeader) + "x,1,0x7E8,0x0,41\n",
         "frames.csv line 2: host_us isn't"},
        {std::string(kFramesHeader) + "1,-1,0x7E8,0x0,41\n",
         "frames.csv line 2: timestamp_us isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,1,41\n",
         "frames.csv line 2: flags isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 0\n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x7E8,0x0,41 \n",
         "frames.csv line 2: data isn't"},
        {std::string(kFramesHeader) + "1,1,0x123456789,0x0,41\n",
         "frames.csv line 2: id isn't"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        TempRecording rec(c.text);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(session, nullptr);
        EXPECT_EQ(std::string(ecuio_last_error_detail()).rfind(c.detail, 0),
                  size_t{0})
            << ecuio_last_error_detail();
    }

    std::string long_data, too_long;
    for (int i = 0; i < ECUIO_FRAME_DATA_MAX; ++i)
        long_data += (i ? " " : "") + std::string("A5");
    too_long = long_data + " A5";
    {
        TempRecording rec(std::string(kFramesHeader) + "1,1,0x7E8,0x0," +
                          too_long + "\n");
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(rec.uri().c_str(), &session),
                  ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "frames.csv line 2: data is longer than a frame holds");
    }

    TempRecording crlf(std::string("host_us,timestamp_us,id,flags,data\r\n") +
                       "1,1,0x7E8,0x0," + long_data +
                       "\r\n2,2,0x7E8,0x0,41 0C 1F 40\r\n");
    ReplayCapture cap;
    ecuio_session_t* session = start_replay(crlf.uri("?speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(cap.frames[0].data_length,
              static_cast<uint16_t>(ECUIO_FRAME_DATA_MAX));
    EXPECT_EQ(cap.frames[0].data[ECUIO_FRAME_DATA_MAX - 1], 0xA5);
    EXPECT_EQ(cap.frames[1].data_length, 4u)
        << "CRLF read as an ending, not data";
    ecuio_session_destroy(session);

    ecuio_session_t* missing = nullptr;
#ifdef _WIN32
    const char* gone = "replay://C:/ecuio-definitely-missing/frames.csv";
#else
    const char* gone = "replay:///ecuio-definitely-missing/frames.csv";
#endif
    EXPECT_EQ(ecuio_session_create(gone, &missing), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "couldn't open the recording's frames.csv");
}

/* The path must be absolute; a folder means its frames.csv; speed, loop
 * and unknown keys are judged before the file is read */
TEST(CoreTest, ReplayJudgesItsPathAndQuery)
{
    TempRecording folder(paced_rows(2), /*as_folder=*/true);
    ASSERT_TRUE(folder.made_dir);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(folder.uri().c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_destroy(session);
    session = nullptr;
    ASSERT_EQ(
        ecuio_session_create((folder.uri() + "/?speed=0.1").c_str(), &session),
        ECUIO_OK)
        << "a trailing separator";
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.replay_speed_milli, 100u);
    ecuio_session_destroy(session);

    for (const char* bad : {"replay://", "replay://?speed=1",
                            "replay://recordings/x", "replay://frames.csv"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(bad, &session), ECUIO_ERR_BAD_URI)
            << bad;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q :
         {"?speed=0", "?speed=0.05", "?speed=101", "?speed=1.2345", "?speed=1.",
          "?speed=.5", "?speed=fast", "?speed=", "?loop=2", "?loop=yes"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_ERR_BAD_URI)
            << q;
        EXPECT_EQ(session, nullptr);
    }
    for (const char* q : {"?speed=100", "?speed=2.5", "?speed=max", "?loop=0",
                          "?loop=1", "?speed=1&speed=3"}) {
        session = nullptr;
        EXPECT_EQ(ecuio_session_create(folder.uri(q).c_str(), &session),
                  ECUIO_OK)
            << q;
        ecuio_session_destroy(session);
    }
    session = nullptr;
    EXPECT_EQ(
        ecuio_session_create(folder.uri("?baud=500000").c_str(), &session),
        ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "replay:// takes no query key 'baud', only profile, did, "
                 "speed and loop");

    TempRecording empty_dir_marker("", /*as_folder=*/true);
    std::remove(empty_dir_marker.csv.c_str());
    session = nullptr;
    EXPECT_EQ(ecuio_session_create(empty_dir_marker.uri().c_str(), &session),
              ECUIO_ERR_LOAD_FAILED)
        << "a folder with no frames.csv";
}

/* Recorded replies decode under the profile the URI names: the defs
 * match by ID and mux key, with no request behind them */
TEST(CoreTest, ReplayDecodesUnderTheProfileItNames)
{
    TempRecording rec(paced_rows(2));
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay(rec.uri("?profile=obd2&speed=max"), &cap);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= 2; }, 2000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    bool decoded = false;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!ecuio_def_applies_to_frame(&d, &cap.frames[0]))
            continue;
        double v = 0.0;
        ASSERT_EQ(ecuio_decode_signal(&d, cap.frames[0].data,
                                      cap.frames[0].data_length, &v),
                  ECUIO_DECODE_OK);
        EXPECT_DOUBLE_EQ(v, 2000.0) << d.name << ": 0x1F40 / 4";
        decoded = true;
    }
    EXPECT_TRUE(decoded) << "41 0C is obd2's RPM";
    ecuio_session_destroy(session);
}

/* The second vehicle session's engine start (docs/bench-sessions/
 * 2026-10-25-vehicle.md, Phase 4), replayed under obd2: the values the
 * sheet reports come back out of the recorded replies. The fixture is
 * the recording scrubbed of its identification replies (ADR-0023), and
 * this test holds it to that. */
TEST(CoreTest, ReplayOfTheVehicleFixtureDecodesTheSheetsValues)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    ReplayCapture cap;
    ecuio_session_t* session =
        start_replay("replay://" + dir + "?profile=obd2&speed=max", &cap);
    ASSERT_NE(session, nullptr);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    const size_t n = info.replay_frames;
    EXPECT_EQ(n, 7074u);
    EXPECT_EQ(info.replay_span_ms, 297446u);
    ASSERT_TRUE(wait_for([&] { return cap.count() >= n; }, 10000));
    ASSERT_EQ(ecuio_session_close(session), ECUIO_OK);
    ASSERT_EQ(cap.frames.size(), n);

    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    /* The obd2 def reading a Service 01 PID */
    const auto def_for = [set](uint8_t pid) -> const ecuio_signal_def_t* {
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].mux_length == 2 &&
                set->defs[i].mux_key == (0x4100u | pid)) {
                return &set->defs[i];
            }
        }
        return nullptr;
    };
    const ecuio_signal_def_t* rpm = def_for(0x0C);
    const ecuio_signal_def_t* coolant = def_for(0x05);
    const ecuio_signal_def_t* volts = def_for(0x42);
    ASSERT_TRUE(rpm && coolant && volts);

    double rpm_max = 0, volts_min = 1e9, coolant_min = 1e9, coolant_max = -1e9;
    double first_rpm_s = -1;
    size_t identification = 0;
    for (const ecuio_frame_t& f : cap.frames) {
        identification +=
            (f.data_length >= 1 && f.data[0] == 0x49) ||
            (f.data_length >= 2 && f.data[0] == 0x62 && f.data[1] == 0xF1);
        double v = 0;
        if (ecuio_def_applies_to_frame(rpm, &f) &&
            ecuio_decode_signal(rpm, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            rpm_max = std::max(rpm_max, v);
            if (v > 0 && first_rpm_s < 0)
                first_rpm_s = f.timestamp_us / 1e6;
        }
        if (ecuio_def_applies_to_frame(volts, &f) &&
            ecuio_decode_signal(volts, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            volts_min = std::min(volts_min, v);
        }
        if (ecuio_def_applies_to_frame(coolant, &f) &&
            ecuio_decode_signal(coolant, f.data, f.data_length, &v) ==
                ECUIO_DECODE_OK) {
            coolant_min = std::min(coolant_min, v);
            coolant_max = std::max(coolant_max, v);
        }
    }
    EXPECT_EQ(identification, 0u)
        << "the fixture carries no identification reply";
    EXPECT_NEAR(rpm_max, 1261.25, 0.01) << "the sheet's 1,261 peak";
    EXPECT_NEAR(volts_min, 9.564, 0.0005)
        << "the sheet's 9.56 V dip at the starter";
    EXPECT_EQ(coolant_min, 60.0) << "the sheet's 60 to 86 degrees C";
    EXPECT_EQ(coolant_max, 86.0);
    EXPECT_NEAR(first_rpm_s, 3.45, 0.01)
        << "the start, 3.45 s into the recording";
    EXPECT_EQ(cap.frames.back().timestamp_us, 297446075u)
        << "the recording's span, rebased";
    ecuio_session_destroy(session);
}

/* ---- Decoded values (ADR-0024) ------------------------------------- */

namespace {

/* One delivered value, copied out of its batch */
struct SeenValue {
    uint16_t id = 0;
    uint8_t kind = 0;
    double value = 0.0;
    std::string text;
    const char* text_at = nullptr; /* where the batch held it */
    uint32_t text_len = 0;
};

struct SeenBatch {
    uint64_t timestamp_us = 0;
    uint32_t arbitration_id = 0;
    std::vector<SeenValue> values;
};

/* Every batch a values callback received */
struct ValuesCapture {
    std::mutex mutex;
    std::vector<SeenBatch> batches;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return batches.size();
    }
};

void values_capture_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    SeenBatch b;
    b.timestamp_us = batch->timestamp_us;
    b.arbitration_id = batch->arbitration_id;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        SeenValue s;
        s.id = v.signal_id;
        s.kind = v.kind;
        s.value = v.value;
        s.text_at = v.text;
        s.text_len = v.text_len;
        if (v.text != nullptr)
            s.text = v.text;
        b.values.push_back(s);
    }
    auto* c = static_cast<ValuesCapture*>(user_data);
    std::lock_guard<std::mutex> lock(c->mutex);
    c->batches.push_back(std::move(b));
}

void values_counting_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++*static_cast<int*>(user_data);
}

ecuio_signal_def_t value_def(uint16_t id, uint8_t kind, uint32_t message_id)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.kind = kind;
    d.message_id = message_id;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof(d.name), "V%u", static_cast<unsigned>(id));
    return d;
}

/* A text def reading byte_length bytes from start, keyed when key_len */
ecuio_signal_def_t text_value_def(uint16_t id, uint8_t kind, uint8_t key_len,
                                  uint32_t key, uint16_t start,
                                  uint16_t byte_length)
{
    ecuio_signal_def_t d = value_def(id, kind, 0x123);
    d.mux_length = key_len;
    d.mux_key = key;
    d.start_byte = start;
    d.byte_length = byte_length;
    return d;
}

ecuio_frame_t value_frame(uint32_t id, std::initializer_list<uint8_t> bytes,
                          uint64_t timestamp_us = 0)
{
    ecuio_frame_t f{};
    f.arbitration_id = id;
    f.timestamp_us = timestamp_us;
    for (uint8_t b : bytes)
        f.data[f.data_length++] = b;
    return f;
}

} // namespace

/* A frame's batch holds every def it decodes, in the set's order, with
 * the values ecuio_decode_signal gives and the frame's time and ID; a
 * def that matches but can't decode (past the payload) is left out; a
 * frame that decodes nothing, and a raw session, deliver no batch */
TEST(CoreTest, ValuesCallbackDeliversEachFramesDecodeWithItsTime)
{
    static ecuio_signal_def_t defs[4];
    defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123);
    defs[0].bit_length = 8;
    defs[1] = value_def(2, ECUIO_VALUE_UINT, 0x456); /* another ID */
    defs[1].bit_length = 8;
    defs[2] = value_def(3, ECUIO_VALUE_UINT, 0x123);
    defs[2].byte_order = ECUIO_BYTE_ORDER_BE;
    defs[2].start_byte = 1;
    defs[2].bit_length = 16;
    defs[2].scale = 0.5;
    defs[3] = value_def(4, ECUIO_VALUE_UINT, 0x123); /* past 3 bytes */
    defs[3].start_byte = 5;
    defs[3].bit_length = 8;
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 4;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-values", &set, nullptr), ECUIO_OK);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://v?profile=test-values", &session),
              ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t f = value_frame(0x123, {10, 0x01, 0x00}, 777);
    ecuio_session_dispatch_callbacks(session, &f);
    const ecuio_frame_t other = value_frame(0x999, {1, 2, 3});
    ecuio_session_dispatch_callbacks(session, &other);

    ASSERT_EQ(cap.batches.size(), 1u)
        << "no batch for a frame that decodes nothing";
    const SeenBatch& b = cap.batches[0];
    EXPECT_EQ(b.timestamp_us, 777u);
    EXPECT_EQ(b.arbitration_id, 0x123u);
    ASSERT_EQ(b.values.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        const ecuio_signal_def_t& d = defs[i == 0 ? 0 : 2];
        double expected = 0.0;
        ASSERT_EQ(ecuio_decode_signal(&d, f.data, f.data_length, &expected),
                  ECUIO_DECODE_OK);
        EXPECT_EQ(b.values[i].id, d.id);
        EXPECT_EQ(b.values[i].kind, ECUIO_VALUE_UINT);
        EXPECT_EQ(b.values[i].value, expected);
        EXPECT_EQ(b.values[i].text_at, nullptr);
        EXPECT_EQ(b.values[i].text_len, 0u);
    }
    EXPECT_EQ(b.values[1].value, 128.0);
    ecuio_session_destroy(session);

    ASSERT_EQ(ecuio_session_create("mock://v?profile=raw", &session), ECUIO_OK);
    ValuesCapture none;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &none),
              ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(none.batches.size(), 0u) << "raw decodes nothing";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-values"), ECUIO_OK);
}

/* The frame callback's registration rules: a NULL argument refused, a
 * pair registered twice called once, unregister idempotent, no
 * registering on a closed session */
TEST(CoreTest, ValuesCallbackRegistersAsTheFrameCallbackDoes)
{
    ecuio_session_t* session = make_test_session();
    int batches = 0;
    EXPECT_EQ(
        ecuio_register_values_callback(nullptr, values_counting_cb, &batches),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_register_values_callback(session, nullptr, &batches),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_unregister_values_callback(nullptr, values_counting_cb, &batches),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);

    /* The mock set's telemetry frame */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    ecuio_frame_t f{};
    f.arbitration_id = set->defs[0].message_id;
    f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(batches, 1) << "deduplicated";

    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(batches, 1) << "unregistered";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(
        ecuio_register_values_callback(session, values_counting_cb, &batches),
        ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(
        ecuio_unregister_values_callback(session, values_counting_cb, &batches),
        ECUIO_OK);
    ecuio_session_destroy(session);
}

namespace {

/* The frame callbacks' count when each values callback ran, and a
 * values callback the first frame callback registers */
struct OrderCtx {
    ecuio_session_t* session = nullptr;
    int frames = 0;
    std::vector<int> frames_at_values;
    int late_batches = 0;
};

void late_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<OrderCtx*>(user_data)->late_batches;
}

void order_frame_cb(const ecuio_frame_t*, void* user_data)
{
    auto* c = static_cast<OrderCtx*>(user_data);
    if (++c->frames == 1)
        ecuio_register_values_callback(c->session, late_values_cb, c);
}

void order_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    auto* c = static_cast<OrderCtx*>(user_data);
    c->frames_at_values.push_back(c->frames);
}

} // namespace

/* Each frame reaches the frame callbacks before its values reach the
 * values callbacks, and a values callback registered by a frame
 * callback starts with the next frame: the lists are taken as they
 * stand when the frame arrives */
TEST(CoreTest, ValuesFollowTheFrameCallbacks)
{
    ecuio_session_t* session = make_test_session();
    OrderCtx c;
    c.session = session;
    EXPECT_EQ(ecuio_register_frame_callback(session, order_frame_cb, &c),
              ECUIO_OK);
    EXPECT_EQ(ecuio_register_values_callback(session, order_values_cb, &c),
              ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    ecuio_frame_t f{};
    f.arbitration_id = set->defs[0].message_id;
    f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_session_dispatch_callbacks(session, &f);
    ecuio_session_dispatch_callbacks(session, &f);
    EXPECT_EQ(c.frames_at_values, (std::vector<int>{1, 2}))
        << "the frame callback ran first both times";
    EXPECT_EQ(c.late_batches, 1) << "from the second frame only";
    ecuio_session_destroy(session);
}

namespace {

/* The first values callback unregisters the second and registers a
 * third, on the first batch only */
struct ValuesSwap {
    ecuio_session_t* session = nullptr;
    int removed = 0;
    int added = 0;
    bool swapped = false;
};

void values_removed_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<ValuesSwap*>(user_data)->removed;
}

void values_added_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<ValuesSwap*>(user_data)->added;
}

void values_swapping_cb(const ecuio_value_batch_t*, void* user_data)
{
    auto* c = static_cast<ValuesSwap*>(user_data);
    if (!c->swapped) {
        c->swapped = true;
        ecuio_unregister_values_callback(c->session, values_removed_cb, c);
        ecuio_register_values_callback(c->session, values_added_cb, c);
    }
}

} // namespace

/* As for the frame callbacks: a values callback removed mid-dispatch
 * still gets that frame's batch, one added doesn't, the next frame the
 * reverse, on a list that fits the stack and one that doesn't */
TEST(CoreTest, ValuesReachTheListAsItStoodWhenTheFrameArrived)
{
    for (int fillers : {0, 4}) {
        SCOPED_TRACE(fillers);
        ecuio_session_t* session = make_test_session();
        ValuesSwap c;
        c.session = session;
        int filler_batches[4] = {};
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_swapping_cb, &c),
            ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_removed_cb, &c),
            ECUIO_OK);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(ecuio_register_values_callback(
                          session, values_counting_cb, &filler_batches[i]),
                      ECUIO_OK);
        }
        const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
        ASSERT_NE(set, nullptr);
        ecuio_frame_t f{};
        f.arbitration_id = set->defs[0].message_id;
        f.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
        ecuio_session_dispatch_callbacks(session, &f);
        EXPECT_EQ(c.removed, 1) << "in the snapshot";
        EXPECT_EQ(c.added, 0) << "not in it";
        ecuio_session_dispatch_callbacks(session, &f);
        EXPECT_EQ(c.removed, 1);
        EXPECT_EQ(c.added, 1);
        for (int i = 0; i < fillers; ++i) {
            EXPECT_EQ(filler_batches[i], 2) << "filler " << i;
        }
        ecuio_session_destroy(session);
    }
}

/* Every text value of a batch has bytes of its own (ADR-0024 Â§4): an
 * unkeyed def, two defs sharing a one-byte key and one with a two-byte
 * key all match "ABCD", four text values at once; a def keyed on
 * another byte doesn't match. The next frame starts from the first slot
 * again */
TEST(CoreTest, ValuesGiveEachTextValueASlotOfItsOwn)
{
    static ecuio_signal_def_t defs[5];
    defs[0] = text_value_def(1, ECUIO_VALUE_HEX, 0, 0, 0, 2);
    defs[1] = text_value_def(2, ECUIO_VALUE_TEXT, 1, 0x41, 2, 1);
    defs[2] = text_value_def(3, ECUIO_VALUE_TEXT, 1, 0x41, 3, 1);
    defs[3] = text_value_def(4, ECUIO_VALUE_TEXT, 2, 0x4142, 2, 2);
    defs[4] = text_value_def(5, ECUIO_VALUE_TEXT, 1, 0x5A, 1, 1);
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 5;
    set.defs = defs;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-text-slots", &set, &why), ECUIO_OK)
        << "code " << why.code << " at def " << why.index;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-text-slots", &session),
        ECUIO_OK);
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t abcd = value_frame(0x123, {'A', 'B', 'C', 'D'});
    const ecuio_frame_t abef = value_frame(0x123, {'A', 'B', 'E', 'F'});
    ecuio_session_dispatch_callbacks(session, &abcd);
    ecuio_session_dispatch_callbacks(session, &abef);
    ASSERT_EQ(cap.batches.size(), 2u);

    const std::vector<SeenValue>& v = cap.batches[0].values;
    ASSERT_EQ(v.size(), 4u) << "a slot for every text value the frame decodes";
    const char* const expected[] = {"41 42", "C", "D", "CD"};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(v[i].id, i + 1);
        EXPECT_EQ(v[i].text, expected[i]) << "value " << i;
        EXPECT_EQ(v[i].text_len, std::strlen(expected[i]));
        EXPECT_EQ(v[i].value, 0.0);
        for (size_t j = 0; j < i; ++j) {
            const std::ptrdiff_t apart = v[i].text_at - v[j].text_at;
            EXPECT_GE(std::abs(apart),
                      static_cast<std::ptrdiff_t>(ECUIO_DECODE_TEXT_MAX))
                << "values " << j << " and " << i << " share bytes";
        }
    }
    const std::vector<SeenValue>& next = cap.batches[1].values;
    ASSERT_EQ(next.size(), 4u);
    EXPECT_EQ(next[1].text, "E");
    EXPECT_EQ(next[3].text, "EF");
    EXPECT_EQ(next[0].text_at, v[0].text_at) << "the slots start again";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-text-slots"), ECUIO_OK);
}

/* uds-generic's set holds a scanned range's DIDs from create, so a
 * reply for one decodes before the scan has found it (ADR-0024 Â§1) */
TEST(CoreTest, ValuesOfAScannedDidComeFromTheSetBuiltAtCreate)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://u?profile=uds-generic&did=F180-F182",
                                   &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    uint16_t f181 = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == 0x62F181u)
            f181 = set->defs[i].id;
    }
    ASSERT_NE(f181, 0) << "F181 is in the set";
    ValuesCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t reply =
        value_frame(0x7E8, {0x62, 0xF1, 0x81, 0x41, 0x42});
    ecuio_session_dispatch_callbacks(session, &reply);
    ASSERT_EQ(cap.batches.size(), 1u);
    ASSERT_EQ(cap.batches[0].values.size(), 1u);
    EXPECT_EQ(cap.batches[0].values[0].id, f181);
    EXPECT_EQ(cap.batches[0].values[0].kind, ECUIO_VALUE_HEX);
    EXPECT_EQ(cap.batches[0].values[0].text, "41 42");
    ecuio_session_destroy(session);
}

namespace {

/* What a frame decodes to, by direct calls, set by the frame callback
 * and checked by the values callback that follows it on the worker */
struct DirectCheck {
    const ecuio_signal_set_t* set = nullptr;
    std::vector<SeenValue> expected;
    std::atomic<int> frames{0};
    std::atomic<int> batches{0};
    std::atomic<int> mismatches{0};
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
    bool same = c->pending && batch->count == c->expected.size();
    for (uint32_t i = 0; same && i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        const SeenValue& e = c->expected[i];
        same = v.signal_id == e.id && v.value == e.value &&
               (v.text == nullptr ? e.text.empty() : e.text == v.text);
    }
    if (!same)
        ++c->mismatches;
    c->pending = false;
}

} // namespace

/* Every batch equals the direct decode of its frame, over the vehicle
 * fixture under obd2 and the mock's drive cycle under its own set */
TEST(CoreTest, ValuesMatchDirectDecodesOfEveryFrame)
{
    const std::string fixture =
        "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
        "/2026-10-25-engine-start?profile=obd2&speed=max";
    struct Case {
        const char* uri;
        int frames;
    };
    for (const Case& k :
         {Case{fixture.c_str(), 7074}, Case{"mock://direct", 20}}) {
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
