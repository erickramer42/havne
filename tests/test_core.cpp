// File/layer: tests/test_core.cpp | GTest, UNIT tier, headless CI job

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

extern "C" {
#include "qttune/core.h"
#include "qttune/internal.h"
#include "qttune/signals.h"
#include "qttune/signal_sets.h"
#include "qttune/profile.h"
}

/* C++ header (namespaced classes and inline functions): must stay
 * outside the extern "C" block above. */
#include "mock_drive_cycle.h"

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
        qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
}

TEST(CoreTest, UnregisterNonexistentCallbackDoesntFail) {
    qttune_session_t* session = make_test_session();
    
    /* Never registered, but should not crash */
    EXPECT_EQ(qttune_unregister_frame_callback(
        session, counting_callback, nullptr), QT_OK);
    
    qttune_session_close(session);
    qttune_session_destroy(session);
}

TEST(CoreTest, NullPointerHandling) {
    EXPECT_EQ(qttune_register_frame_callback(
        nullptr, counting_callback, nullptr), QT_ERR_NULL_ARGUMENT);
    
    qttune_session_t* session = make_test_session();
    EXPECT_EQ(qttune_register_frame_callback(
        session, nullptr, nullptr), QT_ERR_NULL_ARGUMENT);
    
    qttune_session_close(session);
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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
    qttune_session_destroy(session);
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

    /* Close joins the worker; capture struct outlives dispatch safely.
     * Destroy here, before the ASSERTs below can return early. */
    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);

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
                EXPECT_GE(val, kMockRpmMin);  EXPECT_LE(val, kMockRpmMax); break;
            case MOCK_SIG_COOLANT_ID:
                EXPECT_GE(val, kMockCoolantMin);
                EXPECT_LE(val, kMockCoolantMax); break;
            case MOCK_SIG_SPEED_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, kMockSpeedMax); break;
            case MOCK_SIG_THROTTLE_ID:
            case MOCK_SIG_LOAD_ID:
                EXPECT_GE(val, 0.0f);   EXPECT_LE(val, 100.0f); break;
            case MOCK_SIG_IAT_ID:
                EXPECT_GE(val, kMockIatMin);  EXPECT_LE(val, kMockIatMax); break;
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

using qttune_mock::DriveCycle;
using qttune_mock::Phase;

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
    for (uint32_t i = 0; i < qttune_mock::ticks_for(600.0f); ++i) {
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
    EXPECT_LE(pull.peak_rpm, qttune_mock::kRevLimitRpm);

    /* Exactly 5 s on the limiter, bouncing just under it */
    EXPECT_EQ(lim.ticks, qttune_mock::ticks_for(5.0f));
    EXPECT_GE(lim.min_rpm, qttune_mock::kRevLimitRpm - 200.0f);
    EXPECT_LE(lim.peak_rpm, qttune_mock::kRevLimitRpm);
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
    EXPECT_LE(pull3.peak_rpm, qttune_mock::kRevLimitRpm);
    EXPECT_LT(pull3.entry_rpm, 2200.0f);
    EXPECT_GT(pull3.entry_kmh, pull2.entry_kmh + 10.0f);
    EXPECT_GT(pull3.ticks, pull2.ticks * 3 / 2) << "3rd should pull longer";

    EXPECT_EQ(lim3.ticks, qttune_mock::ticks_for(5.0f));
    EXPECT_GE(lim3.peak_kmh, lim2.peak_kmh + 40.0f);

    const float coolant2 = std::fmax(pull2.peak_coolant, lim2.peak_coolant);
    const float coolant3 = std::fmax(pull3.peak_coolant, lim3.peak_coolant);
    const float iat2 = std::fmax(pull2.peak_iat, lim2.peak_iat);
    const float iat3 = std::fmax(pull3.peak_iat, lim3.peak_iat);
    /* "Obviously hotter": the longer pull heat-soaks the engine */
    EXPECT_GT(coolant3, coolant2 + 4.0f);
    EXPECT_GT(iat3, iat2 + 6.0f);
}

TEST(CoreTest, MockDriveCycleDragRunCoversAQuarterMile)
{
    const LoopRun r = run_one_loop();
    const PhaseStats& launch = stats(r, Phase::Launch);
    const PhaseStats& run = stats(r, Phase::DragRun);

    /* Launch control from a standstill */
    EXPECT_EQ(launch.peak_kmh, 0.0f);
    EXPECT_LE(launch.peak_rpm, qttune_mock::kLaunchRpm);
    EXPECT_GE(launch.peak_rpm, qttune_mock::kLaunchRpm - 150.0f);

    /* Through the gears, shifting before the limiter */
    EXPECT_EQ(run.max_gear, qttune_mock::kTopGear);
    EXPECT_LT(run.peak_rpm, qttune_mock::kRevLimitRpm);

    /* The run ends on the tick that crosses the line */
    EXPECT_GE(r.end.distance_m, qttune_mock::kQuarterMileM);
    EXPECT_LT(r.end.distance_m, qttune_mock::kQuarterMileM + 5.0f);
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
    const QttuneSignalSet* set = qttune_mock_signal_set();
    ASSERT_NE(set, nullptr);

    float worst_err[8] = {};
    float lo[8];
    float hi[8];
    for (int i = 0; i < 8; ++i) {
        lo[i] = 1e9f;
        hi[i] = -1e9f;
    }
    ASSERT_LE(set->count, 8u);

    DriveCycle dc;
    uint32_t steps = 0;
    while (dc.loops < 2 && steps < qttune_mock::ticks_for(1200.0f)) {
        dc.step();
        ++steps;
        uint8_t payload[8];
        qttune_mock::encode_payload(dc, payload);

        for (uint32_t i = 0; i < set->count; ++i) {
            const QttuneSignalDef& def = set->defs[i];
            float val = 0.0f;
            ASSERT_EQ(qttune_decode_signal(&def, payload, 8, &val), 0)
                << def.name << " at tick " << steps;
            float phys = 0.0f;
            switch (def.id) {
            case MOCK_SIG_RPM_ID:      phys = dc.rpm; break;
            case MOCK_SIG_COOLANT_ID:  phys = dc.coolant_c; break;
            case MOCK_SIG_SPEED_ID:    phys = dc.speed_kmh; break;
            case MOCK_SIG_THROTTLE_ID: phys = dc.throttle_pct; break;
            case MOCK_SIG_IAT_ID:      phys = dc.iat_c; break;
            case MOCK_SIG_LOAD_ID:     phys = dc.load_pct; break;
            default: FAIL() << "unknown signal id in mock set: " << def.id;
            }
            worst_err[i] = std::fmax(worst_err[i], std::fabs(val - phys));
            lo[i] = std::fmin(lo[i], val);
            hi[i] = std::fmax(hi[i], val);
        }
    }
    ASSERT_EQ(dc.loops, 2u) << "two loops never completed";

    for (uint32_t i = 0; i < set->count; ++i) {
        const QttuneSignalDef& def = set->defs[i];
        /* Half an LSB of each signal's scale, plus float slack */
        const float tol = 0.5f * std::fabs(def.scale) + 0.01f;
        EXPECT_LE(worst_err[i], tol) << def.name << " lost precision or clamped";

        switch (def.id) {
        case MOCK_SIG_RPM_ID:
            EXPECT_GE(lo[i], kMockRpmMin);     EXPECT_LE(hi[i], kMockRpmMax); break;
        case MOCK_SIG_COOLANT_ID:
            EXPECT_GE(lo[i], kMockCoolantMin); EXPECT_LE(hi[i], kMockCoolantMax); break;
        case MOCK_SIG_SPEED_ID:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], kMockSpeedMax); break;
        case MOCK_SIG_IAT_ID:
            EXPECT_GE(lo[i], kMockIatMin);     EXPECT_LE(hi[i], kMockIatMax); break;
        default:
            EXPECT_GE(lo[i], 0.0f);            EXPECT_LE(hi[i], 100.0f); break;
        }
    }
}

namespace {

struct ArbIdCapture {
    std::atomic<int> telemetry_count{0};
    std::atomic<int> diag_count{0};
    std::atomic<int> other_count{0};
    std::atomic<int> extended_mismatch{0};
    std::atomic<uint64_t> last_ts{0};
    std::atomic<int> ts_regression{0};
};

void arb_capture_cb(const qttune_frame_t* frame, void* user_data)
{
    ArbIdCapture* c = static_cast<ArbIdCapture*>(user_data);
    const uint64_t prev = c->last_ts.exchange(frame->timestamp_us);
    if (frame->timestamp_us < prev) {
        c->ts_regression.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame->arbitration_id == 0x0C0) {
        c->telemetry_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & QTTUNE_FRAME_FLAG_EXTENDED) != 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (frame->arbitration_id == 0x18FF0C00) {
        c->diag_count.fetch_add(1, std::memory_order_relaxed);
        if ((frame->flags & QTTUNE_FRAME_FLAG_EXTENDED) == 0) {
            c->extended_mismatch.fetch_add(1, std::memory_order_relaxed);
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
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://arb", &session), QT_OK);

    ArbIdCapture cap;
    ASSERT_EQ(qttune_register_frame_callback(session, &arb_capture_cb, &cap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

    /* Enough ticks for several 16-frame cycles incl. diag frames */
    wait_for([&cap]() {
        return cap.diag_count.load() >= 2 && cap.telemetry_count.load() >= 30;
    }, 10000);

    qttune_session_close(session);
    qttune_session_destroy(session);

    EXPECT_EQ(cap.other_count.load(), 0);
    EXPECT_EQ(cap.extended_mismatch.load(), 0);
    EXPECT_GE(cap.diag_count.load(), 2);
    EXPECT_GT(cap.telemetry_count.load(), cap.diag_count.load() * 4);
    /* Session-relative timestamps must never regress (header contract) */
    EXPECT_EQ(cap.ts_regression.load(), 0);
    /* First sample lands within the first tick, not microseconds since
     * boot — session-relative, not absolute. */
    EXPECT_LT(cap.last_ts.load(), uint64_t{600} * 1000 * 1000 * 1000);
}

TEST(CoreTest, CloseIsIdempotentAndDestroyIsSafe)
{
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://lifecycle", &session), QT_OK);

    /* Double close: second returns the error code WITHOUT UB
     * (regression guard — old close() deleted, making this a
     * use-after-free that only survived allocator luck) */
    EXPECT_EQ(qttune_session_close(session), QT_OK);
    EXPECT_EQ(qttune_session_close(session), QT_ERR_SESSION_CLOSED);

    /* Post-close API calls fail cleanly, not UB */
    EXPECT_EQ(qttune_session_start(session), QT_ERR_SESSION_CLOSED);
    EXPECT_EQ(qttune_register_frame_callback(
        session, counting_callback, nullptr), QT_ERR_SESSION_CLOSED);

    /* destroy() is the freeing call; handle invalid after (free()). */
    EXPECT_EQ(qttune_session_destroy(session), QT_OK);

    /* free(nullptr) semantics */
    EXPECT_EQ(qttune_session_destroy(nullptr), QT_OK);
}

TEST(CoreTest, DestroyAutoClosesActiveSession)
{
    /* The fclose/free split's convenience path: destroy on an open,
     * RUNNING session must join the worker and free — no hang, no leak. */
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://autodestroy", &session), QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);
    std::atomic<int> frames{0};
    static std::atomic<int>* p = &frames;
    EXPECT_EQ(qttune_register_frame_callback(
        session, [](const qttune_frame_t*, void*) { (*p)++; }, nullptr), QT_OK);
    wait_for([&frames]() { return frames.load() >= 1; }, 5000);
    EXPECT_EQ(qttune_session_destroy(session), QT_OK);
}

// ---------------------------------------------------------------------------
// ADR-0013 slice 1: profile registry and session-level selection
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileLookupRawReturnsOkWithNullSet) {
    /* THE contract the null-profile design lives or dies on: "raw" is
     * REGISTERED with a NULL set. Lookup success + null is distinct
     * from lookup failure — presence and content are separate axes,
     * and the first sketch of this feature collapsed them. */
    const QttuneSignalSet* set = nullptr;
    EXPECT_EQ(qttune_profile_lookup("raw", &set), QT_OK);
    EXPECT_EQ(set, nullptr);
}

TEST(CoreTest, ProfileLookupMockReturnsMockSet) {
    const QttuneSignalSet* set = nullptr;
    EXPECT_EQ(qttune_profile_lookup("mock", &set), QT_OK);
    EXPECT_EQ(set, qttune_mock_signal_set());
}

TEST(CoreTest, ProfileLookupUnknownNameFailsWithoutTouchingOut) {
    QttuneSignalSet sentinel = {};
    const QttuneSignalSet* set = &sentinel;
    EXPECT_EQ(qttune_profile_lookup("no-such-profile", &set),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(set, &sentinel);   /* untouched on failure — caller may
                                    rely on its own initialization */
}

TEST(CoreTest, ProfileLookupRejectsNullArguments) {
    const QttuneSignalSet* set = nullptr;
    EXPECT_EQ(qttune_profile_lookup(nullptr, &set), QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_profile_lookup("raw", nullptr), QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_profile_register(nullptr, nullptr), QT_ERR_NULL_ARGUMENT);
}

TEST(CoreTest, ProfileRegisterOverwritesLastWins) {
    /* Duplicate-registration semantics: last wins, matching the URI
     * grammar's duplicate-key rule. Registered under a throwaway name
     * so no built-in is disturbed. */
    QttuneSignalSet mine = {};
    EXPECT_EQ(qttune_profile_register("test-profile-tmp", &mine), QT_OK);
    const QttuneSignalSet* got = nullptr;
    EXPECT_EQ(qttune_profile_lookup("test-profile-tmp", &got), QT_OK);
    EXPECT_EQ(got, &mine);

    EXPECT_EQ(qttune_profile_register("test-profile-tmp", nullptr), QT_OK);
    got = &mine;
    EXPECT_EQ(qttune_profile_lookup("test-profile-tmp", &got), QT_OK);
    EXPECT_EQ(got, nullptr);
}

TEST(CoreTest, MockSessionSelectsMockSet) {
    /* Scheme default: mock:// → the mock profile's set. */
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://profile-default", &session),
              QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), qttune_mock_signal_set());
    qttune_session_destroy(session);
}

TEST(CoreTest, ProfileOverrideRawYieldsNullSet) {
    /* ?profile=raw on a mock session: override to the null profile.
     * ALSO the probe for mock-transport query tolerance (ASSUMED —
     * mock_transport.cpp not reviewed): if this goes red, the mock is
     * choking on the query, and the fix belongs THERE, not here. */
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create("mock://x?profile=raw", &session),
              QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), nullptr);
    qttune_session_destroy(session);
}

TEST(CoreTest, UnknownProfileRejectedAtCreate) {
    /* Strict grammar: an unknown name is QT_ERR_NO_TRANSPORT at
     * session_create — cross-platform, scheme-independent. */
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("mock://x?profile=bogus", &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

#ifdef _WIN32
TEST(CoreTest, UnknownProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, Windows-only: bogus profile on a
     * NONEXISTENT DLL path. Profile validation must run BEFORE
     * transport attach — so this yields QT_ERR_NO_TRANSPORT (3), not
     * QT_ERR_LOAD_FAILED (7). If it ever returns 7, the profile check
     * moved after the attach and the fail-fast contract broke. */
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create(
        "j2534://C:/qttune-definitely-not-a-real-dll.dll?profile=bogus",
        &session), QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// ADR-0013 slice 2: profile query tri-state — a present-but-malformed
// ?profile= value must fail session_create, never silently downgrade
// to the scheme default. Closes the core-vs-parse_query strictness
// asymmetry from the 2026-10-18 session-open review.
// ---------------------------------------------------------------------------

TEST(CoreTest, ProfileQueryEmptyValueRejected) {
    /* ?profile= with nothing after '=': PRESENT but malformed — the
     * old tolerant scanner silently fell back to the scheme default,
     * which could hand a typo'd ?profile=raw a decoding session. */
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("mock://x?profile=", &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryTooLongValueRejected) {
    /* 64+ chars can't be a registry name (64-byte scan buffer, room
     * for the terminator). Fail fast, not truncate-and-confuse. */
    std::string uri = "mock://x?profile=";
    uri.append(80, 'a');
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create(uri.c_str(), &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

TEST(CoreTest, ProfileQueryBoundaryLengthAcceptedAt63) {
    /* 63 chars is the last length the scanner accepts. Registered
     * under exactly that name so this proves the ACCEPTING side of
     * the boundary — the 64 case above pins the rejecting side. */
    const std::string name63(63, 'q');
    const QttuneSignalSet* mock_set = qttune_mock_signal_set();
    ASSERT_EQ(qttune_profile_register(name63.c_str(), mock_set), QT_OK);

    std::string uri = "mock://boundary?profile=" + name63;
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), mock_set);
    qttune_session_destroy(session);
}

TEST(CoreTest, ProfileQueryMalformedFailsEvenWhenLaterValidExists) {
    /* Scanner semantics: ANY malformed occurrence fails, last-wins
     * applies only across syntactically valid values. The empty
     * first value must not be healed by the valid second one. */
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("mock://x?profile=&profile=mock",
                                    &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);

    /* Reverse order: valid first, malformed LAST — a later malformed
     * value still poisons the URI (it is not a "drop the bad one,
     * keep scanning" grammar — it is strict). */
    EXPECT_EQ(qttune_session_create("mock://x?profile=mock&profile=",
                                    &session),
              QT_ERR_NO_TRANSPORT);
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
    ASSERT_EQ(qttune_profile_register(name63.c_str(),
                                      qttune_mock_signal_set()), QT_OK);

    std::string uri1 = "mock://x?profile=raw&profile=" + name63;
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri1.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session),
              qttune_mock_signal_set());   /* last (name63) won */
    qttune_session_destroy(session);

    /* And the inverse order: raw last → null set */
    std::string uri2 = "mock://x?profile=" + name63 + "&profile=raw";
    ASSERT_EQ(qttune_session_create(uri2.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), nullptr);
    qttune_session_destroy(session);
}

#ifdef _WIN32
TEST(CoreTest, MalformedProfileRejectedBeforeDllLoad) {
    /* Ordering discriminator, matching UnknownProfileRejectedBeforeDllLoad:
     * a MALFORMED profile value on a nonexistent DLL path must yield
     * NO_TRANSPORT (profile-parse failure), not QT_ERR_LOAD_FAILED.
     * If it returns 7, the tri-state check moved after attach. */
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create(
        "j2534://C:/qttune-definitely-not-a-real-dll.dll?profile=",
        &session), QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}
#endif
