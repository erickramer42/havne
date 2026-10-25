// File/layer: tests/test_core.cpp | GTest, UNIT tier, headless CI job

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "ecuio/profile.h"
}

/* C++ headers (namespaced classes and inline functions): must stay
 * outside the extern "C" block above. */
#include "mock_drive_cycle.h"
#include "uds_generic.h"   /* core/src: the template set's identity */

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
                 "unsupported URI scheme: expected mock:// or j2534://");
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
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_LOAD_FAILED), "failed to load transport library");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_INVALID_ARGUMENT), "invalid argument");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_BAD_URI), "malformed transport URI");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_UNKNOWN_PROFILE), "unknown profile");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_VENDOR_REFUSED), "the adapter's driver refused");
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_SURROGATE_FAILED), "surrogate process failed");

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

TEST(CoreTest, MockDriveCycleEveryTickRoundTripsThroughTheMockSet)
{
    /* Two loops: the cold first one and a warm one. Every tick goes
     * through the real encoder and decoder. A value the wire can't
     * carry would clamp in the encoder and show up as a large error. */
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
    while (dc.loops < 2 && steps < ecuio_mock::ticks_for(1200.0f)) {
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
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";

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
    /* Two loops, every tick, against the rules a real ECU's log keeps:
     * rpm locked to wheel speed in gear, no speed at idle rpm with the
     * clutch in, airflow consistent with rpm and load, lambda lean
     * exactly when fuel is cut, and temperatures that move like masses
     * of metal and coolant. */
    DriveCycle dc;
    DriveCycle prev = dc;
    uint32_t cut_run = 0;
    uint32_t fueled_run = 0;
    int checked_locked = 0;
    while (dc.loops < 2 && dc.tick < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        const float k = dc.gear > 0 ? ecuio_mock::rpm_per_mps(dc.gear) : 0.0f;
        const bool limiter = dc.phase == Phase::Limiter2nd || dc.phase == Phase::Limiter3rd;
        const bool locked = dc.gear > 0 && !dc.clutch_slip && !dc.shifting() && !dc.spinning;

        if (locked && !limiter) {
            EXPECT_NEAR(dc.rpm, dc.wheel_mps * k, 1.0f) << "tick " << dc.tick;
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
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";
    EXPECT_GT(checked_locked, 1000) << "too few in-gear ticks checked";
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

TEST(CoreTest, MockDriveCycleLaunchSpinsTheTyresAndHooksUp)
{
    /* Off the line the clutch slips while the tyres sit near peak slip;
     * once it locks in 1st the driver feathers the throttle with the
     * tyres spinning a few hundred rpm above road speed. From 2nd the
     * drive is below the grip and the tyres hook up. */
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

    /* destroy() is the freeing call; handle invalid after (free()). */
    EXPECT_EQ(ecuio_session_destroy(session), ECUIO_OK);

    /* free(nullptr) semantics */
    EXPECT_EQ(ecuio_session_destroy(nullptr), ECUIO_OK);
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
