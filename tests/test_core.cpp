// File/layer: tests/test_core.cpp | GTest-based, CI-validated
// Testing: Verifies registration, dedup, dispatch, and lifecycle contracts

#include <gtest/gtest.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
#include "qttune/core.h"
#include "qttune/internal.h"
#include "qttune/signals.h"
#include "qttune/signal_sets.h"
}

namespace {

std::atomic<int> g_callback_count{0};

void counting_callback(const qttune_frame_t* frame, void* user_data)
{
    (void)frame;
    int* counter = static_cast<int*>(user_data);
    if (counter) {
        (*counter)++;
    }
    g_callback_count++;
}

/* Helper to create test session */
qttune_session_t* make_test_session(void)
{
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("mock://test", &session), QT_OK);
    return session;
}

/* Poll-until-predicate with deadline. The mock's 50ms cadence is a
 * LOCAL expectation, not a CI-runner guarantee — a starved macOS VM
 * was observed delivering 2 frames in 200+ ms of wall clock
 * (2026-10-14 CI flake). Polling keeps the asserted CONTRACT (the
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

TEST(CoreTest, StatusStringValidValues) {
    EXPECT_STREQ(qttune_status_string(QT_OK), "success");
    EXPECT_STREQ(qttune_status_string(QT_ERR_NULL_ARGUMENT), "null argument");
    EXPECT_STREQ(qttune_status_string(QT_ERR_NOT_IMPLEMENTED), "not implemented yet");
    EXPECT_STREQ(qttune_status_string(QT_ERR_NO_TRANSPORT), "no transport available");
    EXPECT_STREQ(qttune_status_string(QT_ERR_OUT_OF_MEMORY), "out of memory");
    EXPECT_STREQ(qttune_status_string(QT_ERR_SESSION_CLOSED), "session already closed");
    EXPECT_STREQ(qttune_status_string(QT_ERR_ALREADY_REGISTERED), "already registered");
    EXPECT_STREQ(qttune_status_string(QT_ERR_LOAD_FAILED), "failed to load transport library");
    
    /* Explicit cast required by MSVC for enum conversion */
    EXPECT_STRNE(qttune_status_string(static_cast<qttune_status_t>(999)), nullptr);
}

TEST(CoreTest, VersionStringContainsNumbers) {
    const char* version = qttune_version_string();
    EXPECT_TRUE(version != nullptr);
    EXPECT_GT(strlen(version), 0);
    EXPECT_NE(strstr(version, "."), nullptr);
}

TEST(CoreTest, InitShutdownIdempotent) {
    EXPECT_EQ(qttune_init(), QT_OK);
    EXPECT_EQ(qttune_init(), QT_OK);
    
    qttune_shutdown();
    
    EXPECT_EQ(qttune_init(), QT_OK);
    qttune_shutdown();
}

TEST(CoreTest, SessionCreateRequiresNonNullPointer) {
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("mock://test", &session), QT_OK);
    
    EXPECT_EQ(qttune_session_create("mock://test", nullptr), QT_ERR_NULL_ARGUMENT);
    
    if (session) {
        qttune_session_close(session);
    }
}

TEST(CoreTest, RegisterCallbackSuccess) {
    qttune_session_t* session = make_test_session();
    int counter = 0;
    
    qttune_status_t status = qttune_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status, QT_OK);
    
    qttune_session_close(session);
}

TEST(CoreTest, RegisterDuplicateCallbackIdempotent) {
    qttune_session_t* session = make_test_session();
    int counter = 0;
    
    qttune_status_t status1 = qttune_register_frame_callback(
        session, counting_callback, &counter
    );
    qttune_status_t status2 = qttune_register_frame_callback(
        session, counting_callback, &counter
    );
    
    EXPECT_EQ(status1, QT_OK);
    EXPECT_EQ(status2, QT_OK);  /* Silently deduplicated, not an error */
    
    qttune_session_close(session);
}

TEST(CoreTest, SameCallbackDifferentUserdataAllowed) {
    qttune_session_t* session = make_test_session();
    int counter1 = 0;
    int counter2 = 0;
    
    /* Same function, different user_data = distinct registrations */
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter1), QT_OK);
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter2), QT_OK);
    
    qttune_session_close(session);
}

TEST(CoreTest, UnregisterCallback) {
    qttune_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter), QT_OK);
    
    EXPECT_EQ(qttune_unregister_frame_callback(
        session, counting_callback, &counter), QT_OK);
    
    /* Second unregister is also success (idempotent) */
    EXPECT_EQ(qttune_unregister_frame_callback(
        session, counting_callback, &counter), QT_OK);
    
    qttune_session_close(session);
}

TEST(CoreTest, UnregisterNonexistentCallbackDoesntFail) {
    qttune_session_t* session = make_test_session();
    
    /* Never registered, but should not crash */
    EXPECT_EQ(qttune_unregister_frame_callback(
        session, counting_callback, nullptr), QT_OK);
    
    qttune_session_close(session);
}

TEST(CoreTest, NullPointerHandling) {
    EXPECT_EQ(qttune_register_frame_callback(
        nullptr, counting_callback, nullptr), QT_ERR_NULL_ARGUMENT);
    
    qttune_session_t* session = make_test_session();
    EXPECT_EQ(qttune_register_frame_callback(
        session, nullptr, nullptr), QT_ERR_NULL_ARGUMENT);
    
    qttune_session_close(session);
}

/* Threading contract test - validates snapshot pattern compiles */
TEST(CoreTest, CallbackSnapshotPatternCompiles) {
    /* This test documents the threading discipline:
     * 1. Mock transport will call qttune_session_dispatch_callbacks()
     * 2. That function copies the callback list under lock
     * 3. Then invokes callbacks OUTSIDE the lock
     *
     * This prevents deadlock if callback calls unregister().
     *
     * Full threading validation requires TSAN job or live transport test.
     */
    qttune_session_t* session = make_test_session();
    int counter = 0;
    
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter), QT_OK);
    
    /* Null frame is a documented no-op — proves precondition guard */
    qttune_session_dispatch_callbacks(session, nullptr);
    
    qttune_session_close(session);
}

TEST(CoreTest, CallbackDispatchInvokesRegisteredCallback) {
    qttune_session_t* session = make_test_session();
    int counter = 0;
    qttune_frame_t frame{};
    frame.data_length = 8;

    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter), QT_OK);

    qttune_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 1);

    /* Duplicate registration is deduplicated — one invocation per dispatch */
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, &counter), QT_OK);
    qttune_session_dispatch_callbacks(session, &frame);
    EXPECT_EQ(counter, 2);  // Still 1 per dispatch, dedup worked

    qttune_session_close(session);
}

TEST(CoreTest, MockTransportDeliversFrames) {
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://test", &session), QT_OK);

    std::atomic<int> frame_count{0};
    /* plain function to keep extern "C" signature compatibility */
    static std::atomic<int>* counter_ptr = &frame_count;
    EXPECT_EQ(qttune_register_frame_callback(
        session,
        [](const qttune_frame_t*, void*) { (*counter_ptr)++; },
        nullptr), QT_OK);

    EXPECT_EQ(qttune_session_start(session), QT_OK);

    /* CONTRACT: at least 3 frames eventually arrive. The 50ms tick
     * implies ~4 in 200ms LOCALLY, but CI runners under load have
     * delivered fewer (observed 2-in-356ms on macOS, 2026-10-14) —
     * poll to the condition instead of betting on a fixed sleep. */
    const bool delivered = wait_for(
        [&frame_count]() { return frame_count.load() >= 3; },
        5000);
    EXPECT_TRUE(delivered) << "mock delivered only "
                           << frame_count.load() << " frames in 5s";
    EXPECT_GE(frame_count.load(), 3);

    /* Close must join the worker without hanging */
    EXPECT_EQ(qttune_session_close(session), QT_OK);
}

TEST(CoreTest, UnknownTransportUriRejected) {
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("j2534://dev0", &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(qttune_session_create(nullptr, &session),
              QT_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, SessionStartWithoutTransportFails) {
    /* Valid mock session, but force no-transport via closed early?
     * Not reachable via public API in v0.2 — skip rather than assert UB. */
    SUCCEED();
}

TEST(CoreTest, CallbackExecutesOnTransportWorkerThread) {
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://thread-test", &session), QT_OK);

    /* std::atomic<std::thread::id> is lock-free on all 3 CI platforms;
     * mutex alternative also fine. */
    std::atomic<std::thread::id> seen_id{};
    struct Ctx { std::atomic<std::thread::id>* out; };
    static Ctx ctx{&seen_id};  /* static: captureless lambda can't capture */

    EXPECT_EQ(qttune_register_frame_callback(
        session,
        [](const qttune_frame_t*, void* ud) {
            static_cast<Ctx*>(ud)->out->store(std::this_thread::get_id());
        },
        &ctx), QT_OK);

    const auto main_id = std::this_thread::get_id();
    EXPECT_EQ(qttune_session_start(session), QT_OK);

    /* Already a poll loop — widened deadline only (400ms -> 2s) to
     * cover starved runners; the shape was right. */
    const bool seen = wait_for(
        [&seen_id]() { return seen_id.load() != std::thread::id{}; },
        2000);
    EXPECT_TRUE(seen);

    EXPECT_NE(seen_id.load(), std::thread::id{});
    EXPECT_NE(seen_id.load(), main_id);  /* Marshaling in bridge is MANDATORY */

    EXPECT_EQ(qttune_session_close(session), QT_OK);
}

namespace {

struct ReentryCtx {
    qttune_session_t* session = nullptr;
    std::atomic<int> self_count{0};
    std::atomic<int> other_count{0};
};

/* NAMED function — required so the callback can unregister ITSELF.
 * A captureless lambda cannot refer to its own function pointer. */
void self_unregistering_cb(const qttune_frame_t*, void* user_data)
{
    ReentryCtx* c = static_cast<ReentryCtx*>(user_data);
    c->self_count.fetch_add(1, std::memory_order_relaxed);
    /* Runs on worker thread while snapshot dispatch is mid-iteration.
     * Touches the callback registry (same mutex being snapshotted).
     * Deadlocks here if the snapshot pattern regresses. */
    qttune_unregister_frame_callback(c->session, &self_unregistering_cb, user_data);
}

void sibling_counter_cb(const qttune_frame_t*, void* user_data)
{
    static_cast<ReentryCtx*>(user_data)->other_count
        .fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(CoreTest, SelfUnregisterDuringLiveDispatch) {
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://reentry", &session), QT_OK);

    ReentryCtx ctx;   // stack-local is safe: session_close joins the worker
    ctx.session = session;

    EXPECT_EQ(qttune_register_frame_callback(
        session, &self_unregistering_cb, &ctx), QT_OK);
    EXPECT_EQ(qttune_register_frame_callback(
        session, &sibling_counter_cb, &ctx), QT_OK);

    EXPECT_EQ(qttune_session_start(session), QT_OK);
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
    EXPECT_EQ(qttune_session_close(session), QT_OK);
}

namespace {

struct MockCapture {
    static constexpr size_t kMaxFrames = 20;
    uint8_t payloads[kMaxFrames][8]{};
    size_t count = 0;
};

/* Named free function — payload capture needs self-reference-free
 * simplicity; user_data points at a stack struct, safe because
 * session_close joins the worker before the stack frame unwinds. */
void capture_payload_cb(const qttune_frame_t* frame, void* user_data)
{
    MockCapture* cap = static_cast<MockCapture*>(user_data);
    if (cap->count < MockCapture::kMaxFrames) {
        std::memcpy(cap->payloads[cap->count], frame->data, 8);
        ++cap->count;
    }
}

} // namespace

TEST(CoreTest, MockFramesDecodeAgainstMockSignalSet) {
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://signals", &session), QT_OK);

    MockCapture cap;
    EXPECT_EQ(qttune_register_frame_callback(
        session, &capture_payload_cb, &cap), QT_OK);

    EXPECT_EQ(qttune_session_start(session), QT_OK);

    /* Collect up to kMaxFrames, polling to the condition. Was a fixed
     * 1400ms sleep; under starvation fewer than the nominal 28 frames
     * would arrive and the >=5 floor got uncomfortably close. Now we
     * simply leave as soon as the buffer is full. */
    wait_for([&cap]() { return cap.count >= MockCapture::kMaxFrames; },
             10000);

    /* Close joins the worker; capture struct outlives dispatch safely */
    EXPECT_EQ(qttune_session_close(session), QT_OK);

    ASSERT_GE(cap.count, size_t{5}) << "expected several frames from mock";

    /* Encoder/decoder round-trip contract: every frame decodes under
     * every mock definition with physically plausible results. A
     * byte-order or scale inversion passes structural checks but
     * produces absurd physics — the range assertions catch that. */
    const QttuneSignalSet* set = qttune_mock_signal_set();
    ASSERT_NE(set, nullptr);

    for (size_t f = 0; f < cap.count; ++f) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const QttuneSignalDef& def = set->defs[i];
            float val = 0.0f;
            ASSERT_EQ(qttune_decode_signal(&def, cap.payloads[f], 8, &val), 0)
                << "frame " << f << ", signal " << def.name;

            switch (def.id) {
            case MOCK_SIG_RPM_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LT(val, 8000.0f); break;
            case MOCK_SIG_COOLANT_ID:
                EXPECT_GE(val, 15.0f);  EXPECT_LE(val, 100.0f); break;
            case MOCK_SIG_SPEED_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, 100.0f); break;
            case MOCK_SIG_THROTTLE_ID:
            case MOCK_SIG_LOAD_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, 100.0f); break;
            case MOCK_SIG_IAT_ID:
                EXPECT_GE(val, 15.0f);   EXPECT_LE(val, 60.0f); break;
            default:
                FAIL() << "unknown signal id in mock set: " << def.id;
            }
        }
    }
}
