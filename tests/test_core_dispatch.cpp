// File/layer: tests/test_core_dispatch.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

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

    /* Already a poll loop: widened deadline only (400ms -> 2s) to
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

/* NAMED function: required so the callback can unregister ITSELF.
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
    /* Poll until the sibling has seen >= 3 frames: that implies the
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


struct MockCapture {
    static constexpr size_t kMaxFrames = 20;
    uint8_t payloads[kMaxFrames][ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN]{};
    size_t count = 0;
};

/* Named free function: payload capture needs self-reference-free
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
     * produces absurd physics: the range assertions catch that. */
    const ecuio_signal_set_t* set = ecuio_mock_signal_set();
    ASSERT_NE(set, nullptr);

    for (size_t f = 0; f < cap.count; ++f) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            if (def.flags & ECUIO_DEF_FLAG_COMPUTED) {
                continue; /* no field: MockBoostIsMapMinusBaro */
            }
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
            case ECUIO_MOCK_SIG_MAP_ID:
                EXPECT_GE(val, kMockMapMin);
                EXPECT_LE(val, kMockBaroMax);
                break;
            case ECUIO_MOCK_SIG_BARO_ID:
                EXPECT_GE(val, kMockBaroMin);
                EXPECT_LE(val, kMockBaroMax);
                break;
            case ECUIO_MOCK_SIG_BATTERY_V_ID:
                EXPECT_GE(val, kMockBatteryMin);
                EXPECT_LE(val, kMockBatteryMax);
                break;
            default:
                FAIL() << "unknown signal id in mock set: " << def.id;
            }
        }
    }
}

namespace {

/* Each batch's MAP, baro and boost, for the first kMax batches that
 * carry boost */
struct BoostCapture {
    static constexpr size_t kMax = 20;
    double map[kMax]{};
    double baro[kMax]{};
    double boost[kMax]{};
    size_t count = 0;
};

void capture_boost_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* cap = static_cast<BoostCapture*>(user_data);
    if (cap->count >= BoostCapture::kMax) {
        return;
    }
    double map = -1.0, baro = -1.0, boost = 0.0;
    bool has_boost = false;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.signal_id == ECUIO_MOCK_SIG_MAP_ID)
            map = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BARO_ID)
            baro = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BOOST_ID) {
            boost = v.value;
            has_boost = true;
        }
    }
    if (has_boost) {
        cap->map[cap->count] = map;
        cap->baro[cap->count] = baro;
        cap->boost[cap->count] = boost;
        ++cap->count;
    }
}

/* Every batch's boost against its MAP and baro, counted */
struct BoostTally {
    size_t batches = 0;
    size_t wrong = 0; /* boost isn't MAP minus baro */
    double lowest = 1e9;
    double highest = -1e9;
};

void tally_boost_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* t = static_cast<BoostTally*>(user_data);
    double map = -1.0, baro = -1.0, boost = 0.0;
    bool has_boost = false;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.signal_id == ECUIO_MOCK_SIG_MAP_ID)
            map = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BARO_ID)
            baro = v.value;
        if (v.signal_id == ECUIO_MOCK_SIG_BOOST_ID) {
            boost = v.value;
            has_boost = true;
        }
    }
    if (!has_boost) {
        return;
    }
    ++t->batches;
    if (map < 0.0 || baro < 0.0 || boost != map - baro) {
        ++t->wrong;
    }
    t->lowest = std::fmin(t->lowest, boost);
    t->highest = std::fmax(t->highest, boost);
}

} // namespace

/* The mock's computed signal (ADR-0051 section 5) through the worker:
 * every frame carries MAP and baro, so every batch carries boost, the
 * one minus the other, within the model's bounds */
TEST(CoreTest, MockBoostIsMapMinusBaro)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://boost", &session), ECUIO_OK);
    BoostCapture cap;
    EXPECT_EQ(ecuio_register_values_callback(session, &capture_boost_cb, &cap),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for([&cap]() { return cap.count >= BoostCapture::kMax; }, 10000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    ASSERT_GE(cap.count, size_t{5}) << "expected boost in several batches";
    for (size_t i = 0; i < cap.count; ++i) {
        ASSERT_GE(cap.map[i], 0.0) << "a batch with boost but no MAP, " << i;
        ASSERT_GE(cap.baro[i], 0.0) << "a batch with boost but no baro, " << i;
        EXPECT_DOUBLE_EQ(cap.boost[i], cap.map[i] - cap.baro[i])
            << "batch " << i;
        EXPECT_GE(cap.boost[i], kMockBoostMin) << "batch " << i;
        EXPECT_LE(cap.boost[i], kMockBoostMax) << "batch " << i;
    }

    /* The worker's first frames are all key on, MAP and baro one value,
     * so a whole loop, the engine running, goes through a session's own
     * dispatch, every tick a frame */
    ecuio_session_t* fed = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://boost-fed", &fed), ECUIO_OK);
    BoostTally tally;
    EXPECT_EQ(ecuio_register_values_callback(fed, &tally_boost_cb, &tally),
              ECUIO_OK);
    auto frame = std::make_unique<ecuio_frame_t>(); /* 4 KB: not on the stack */
    frame->arbitration_id = ECUIO_MOCK_TELEMETRY_ARB_ID;
    frame->flags = ECUIO_FRAME_FLAG_FD;
    frame->data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;
    ecuio_mock::DriveCycle dc;
    size_t ticks = 0;
    while (dc.loops < 1 && ticks < ecuio_mock::ticks_for(1200.0f)) {
        dc.step();
        ++ticks;
        frame->timestamp_us = ticks * 50000u;
        ecuio_mock::encode_payload(dc, frame->data);
        ecuio_session_dispatch_callbacks(fed, frame.get());
    }
    ecuio_session_destroy(fed);
    ASSERT_EQ(dc.loops, 1u) << "the loop never completed";
    EXPECT_EQ(tally.batches, ticks) << "a frame without boost";
    EXPECT_EQ(tally.wrong, 0u) << "boost that isn't MAP minus baro";
    EXPECT_LT(tally.lowest, -50.0) << "no running engine's vacuum";
    EXPECT_GE(tally.lowest, kMockBoostMin);
    EXPECT_LE(tally.highest, kMockBoostMax);
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

/* Contract: the mock emits exactly two arbitration IDs, the
 * telemetry ID the signal set declares (0x0C0) and a 29-bit diagnostic
 * ID every 16th frame: with FLAG_EXTENDED set iff the ID is 29-bit.
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
     * boot: session-relative, not absolute. */
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

/* Session time's clock (ADR-0033 section 1) never goes back, nor does its slept
 * count, and while the machine is awake it keeps the waits' pace */
// ---------------------------------------------------------------------------
// A requesting worker's waits (core/src/precise_wait.h)
