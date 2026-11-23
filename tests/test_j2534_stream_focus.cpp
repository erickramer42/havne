// File/layer: tests/test_j2534_stream_focus.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* ADR-0056 section 2: a focus set before start narrows the rotation to
 * the picked PIDs from the worker's first request, and a count of 0
 * brings the whole set back. The page keeps 16 writes: the second run's
 * are read after a reset of the page, where the first few may still be
 * the picks'. */
TEST_F(J2534AdoptionTest, AFocusAsksOnlyThePickedPids)
{
    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=obd2&filter=0x7E8&probe=0&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            (d.mux_key == 0x410Cu || d.mux_key == 0x410Du)) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    ASSERT_EQ(ecuio_session_set_focus(session, ids.data(), 2), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 16, 250);
    uint8_t last = 0;
    for (unsigned long i = 0; i < 16; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_GE(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        EXPECT_TRUE(wbuf[5] == 0x0C || wbuf[5] == 0x0D)
            << "write " << i << " asks PID " << int(wbuf[5]);
        EXPECT_NE(wbuf[5], last) << "write " << i << " repeats a pick";
        last = wbuf[5];
    }

    ASSERT_EQ(ecuio_session_set_focus(session, nullptr, 0), ECUIO_OK);
    fake_control::reset(fake_page());
    wait_for_writes(get_write_count, 16, 250);
    std::set<uint8_t> pids;
    for (unsigned long i = 0; i < 16; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_GE(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        pids.insert(wbuf[5]);
    }
    EXPECT_GT(pids.size(), 2u) << "the whole set is asked again";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ADR-0065 section 3: a session on a set that derives more requests than
 * a rotation holds says so, writes nothing until a focus picks and stays
 * out of "not responding" meanwhile, then asks the picks alone; a pick
 * the ECU refuses still times the link out, and the timeout ends when
 * the pick is taken away */
TEST_F(J2534AdoptionTest, ASetPastARotationIsAskedOnlyItsPicks)
{
    using car_sized::id_of;
    using car_sized::kMost;
    KnobGuard module("ECUIO_FAKE_MODULE_DIDS", "7E8:1007=2A;7E9:1003=11");
    ASSERT_EQ(ecuio_profile_register("j2534-car", car_sized::set(), nullptr),
              ECUIO_OK);
    char uri[700];
    /* The probe stays on: with it a refused request leaves the rotation */
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=j2534-car&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    const auto state = [&] {
        int32_t last = -1;
        uint32_t episodes = 0;
        EXPECT_EQ(ecuio_session_get_error_state(session, &last, &episodes),
                  ECUIO_OK);
        return last;
    };
    const auto reaches = [&](int32_t want) {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (state() != want && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return state() == want;
    };
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 2u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Twice the quiet run a link takes to say an ECU isn't answering */
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    unsigned long written = 77;
    get_write_count(&written);
    EXPECT_EQ(written, 0u) << "nothing is asked before a pick";
    EXPECT_EQ(state(), ECUIO_OK)
        << "a session asking nothing isn't one an ECU doesn't answer";

    /* One signal of each module */
    const uint16_t picks[] = {id_of(kMost + 3), id_of(7)};
    ASSERT_EQ(ecuio_session_set_focus(session, picks, 2), ECUIO_OK)
        << ecuio_last_error_detail();
    wait_for_writes(get_write_count, 8, 250);
    bool engine = false;
    bool other = false;
    for (unsigned long i = 0; i < 8; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 7u) << "write " << i;
        const unsigned char to_engine[7] = {0, 0, 0x07, 0xE0, 0x22, 0x10, 0x07};
        const unsigned char to_other[7] = {0, 0, 0x07, 0xE1, 0x22, 0x10, 0x03};
        const bool a = memcmp(wbuf, to_engine, 7) == 0;
        const bool b = memcmp(wbuf, to_other, 7) == 0;
        EXPECT_TRUE(a || b) << "write " << i << " asks " << int(wbuf[5])
                            << int(wbuf[6]) << " of " << int(wbuf[3]);
        engine = engine || a;
        other = other || b;
    }
    EXPECT_TRUE(engine && other) << "each module's pick";
    EXPECT_EQ(state(), ECUIO_OK);

    /* A pick the ECU refuses is dropped, but it was asked: the silence
     * after it counts as any table's does */
    const uint16_t refused = id_of(9);
    ASSERT_EQ(ecuio_session_set_focus(session, &refused, 1), ECUIO_OK);
    ASSERT_TRUE(reaches(ECUIO_ERR_VEHICLE_TIMEOUT))
        << "picks that were all dropped still time out";
    /* The pick taken away: nothing is asked, and its timeout ends */
    ASSERT_EQ(ecuio_session_set_focus(session, nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(reaches(ECUIO_OK)) << "nothing asked: nothing fails to answer";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("j2534-car"), ECUIO_OK);
}

/* A session on the car-sized set, which asks only its picks, registered
 * under a name of the case's own; closed and freed with the case, the
 * profile after it */
struct CarSession {
    const char* profile;
    ecuio_session_t* s = nullptr;
    explicit CarSession(const char* name) : profile(name)
    {
        EXPECT_EQ(ecuio_profile_register(name, car_sized::set(), nullptr),
                  ECUIO_OK);
        char uri[700];
        snprintf(uri, sizeof(uri), "j2534://%s?profile=%s", FAKE_J2534_DLL_PATH,
                 name);
        EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~CarSession()
    {
        if (s != nullptr) {
            EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
            ecuio_session_destroy(s);
        }
        EXPECT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    }
    CarSession(const CarSession&) = delete;
    CarSession& operator=(const CarSession&) = delete;

    int32_t state(uint32_t* episodes = nullptr) const
    {
        int32_t last = -1;
        uint32_t count = 0;
        EXPECT_EQ(ecuio_session_get_error_state(s, &last, &count), ECUIO_OK);
        if (episodes != nullptr) {
            *episodes = count;
        }
        return last;
    }
    /* Whether each open module's identification and diagnostics have an
     * outcome for every item (a closed one is never asked) */
    bool settled() const
    {
        for (uint32_t c = 0; c < 2; ++c) {
            ecuio_channel_identity_t id{};
            ecuio_channel_diagnostics_t diag{};
            ecuio_channel_support_t sup{};
            if (ecuio_session_get_channel_support(s, c, &sup) == ECUIO_OK &&
                (sup.flags & ECUIO_SUPPORT_CLOSED) != 0) {
                continue; /* no answer to the start's probe, nothing picked */
            }
            if (ecuio_session_get_channel_identity(s, c, &id) != ECUIO_OK ||
                ecuio_session_get_channel_diagnostics(s, c, &diag) !=
                    ECUIO_OK ||
                (id.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
                (diag.flags & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
        }
        return true;
    }
};

/* An unpicked session's checks at an interval of the case's own, the
 * shipped one given back after (PickOnlyTest pins that one) */
struct CheckEvery {
    explicit CheckEvery(int64_t ms)
    {
        ecuio_set_unpicked_check_ms(static_cast<uint32_t>(ms));
    }
    ~CheckEvery() { ecuio_set_unpicked_check_ms(0); }
    CheckEvery(const CheckEvery&) = delete;
    CheckEvery& operator=(const CheckEvery&) = delete;
};
constexpr int64_t kCheckMs = 2000;

bool within(int ms, const std::function<bool()>& done)
{
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

/* Whether count() stops changing for quiet_ms, within limit_ms. A
 * recovery's reports read complete before its last request is out (a
 * report keeps its earlier outcomes until an item asked again settles),
 * so "nothing more is asked" is judged by the requests themselves. */
template <typename Count>
bool goes_quiet(Count count, int64_t quiet_ms, int64_t limit_ms)
{
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
    auto last = count();
    auto since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto seen = count();
        const auto now = std::chrono::steady_clock::now();
        if (seen != last) {
            last = seen;
            since = now;
        } else if (now - since >= std::chrono::milliseconds(quiet_ms)) {
            return true;
        }
    }
    return false;
}

unsigned long writes_now()
{
    unsigned long n = 0;
    page_write_count(&n);
    return n;
}

/* Such a session with nothing picked still asks identification and
 * diagnostics, each module's. One module of two answers nothing at all,
 * and the other nothing to what it doesn't serve (the fake's refusals
 * as silence): the silence is one module's and an item's, never the
 * vehicle's. The link is OK through the unanswered items, and after the
 * adapter failed and read again, when they are all asked again with no
 * reply among them. */
TEST_F(J2534AdoptionTest, AnUnpickedSessionWithOneSilentModuleStaysOk)
{
    KnobGuard absent("ECUIO_FAKE_ABSENT_ECU", "7E9");
    KnobGuard silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    CarSession car("j2534-one-module-car");
    ASSERT_NE(car.s, nullptr);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(car.s, &info), ECUIO_OK);
    ASSERT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
    ASSERT_EQ(ecuio_session_start(car.s), ECUIO_OK);
    /* The state is read all the while, so a timeout fails in its words */
    const auto settle_by =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (!car.settled()) {
        ASSERT_EQ(car.state(), ECUIO_OK)
            << "one module's silence was taken for the vehicle's";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ecuio_channel_identity_t engine_ecu{};
    ASSERT_EQ(ecuio_session_get_channel_identity(car.s, 0, &engine_ecu),
              ECUIO_OK);
    EXPECT_NE(engine_ecu.flags & ECUIO_IDENTITY_VIN, 0u);
    uint32_t silent_items = 0;
    for (uint32_t i = 0; i < engine_ecu.items; ++i) {
        silent_items +=
            engine_ecu.list[i].outcome == ECUIO_IDENTITY_UNANSWERED ? 1u : 0u;
    }
    EXPECT_GE(silent_items, 1u)
        << "the case needs an item the live ECU leaves unanswered";
    /* The start's probe heard nothing (off in this binary): the engine
     * ECU stands in, and the other module is closed with nothing picked,
     * so its silence is never asked */
    ecuio_channel_support_t module{};
    ASSERT_EQ(ecuio_session_get_channel_support(car.s, 1, &module), ECUIO_OK);
    EXPECT_NE(module.flags & ECUIO_SUPPORT_CLOSED, 0u);
    uint32_t episodes = 0;
    EXPECT_EQ(car.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u);

    /* The adapter fails and reads again: the recovery asks every
     * unanswered item again, the live ECU's too, and none is answered */
    ASSERT_TRUE(goes_quiet(writes_now, 200, 5000));
    const unsigned long before = writes_now();
    {
        KnobGuard dead("ECUIO_FAKE_READ_STATUS", "7");
        ASSERT_TRUE(within(5000, [&] {
            return car.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        }));
    }
    ASSERT_TRUE(within(10000, [&] { return car.state() == ECUIO_OK; }));
    /* OK until the writes have stopped for 2 s. The check keeps its 5 s
     * here: one falling due before the failure ends would ask the engine
     * ECU a request it is silent on */
    unsigned long last = writes_now();
    auto since = std::chrono::steady_clock::now();
    const auto give_up = since + std::chrono::milliseconds(20000);
    while (std::chrono::steady_clock::now() - since <
           std::chrono::milliseconds(2000)) {
        ASSERT_EQ(car.state(), ECUIO_OK)
            << "the unanswered items asked again took the link down";
        ASSERT_LT(std::chrono::steady_clock::now(), give_up)
            << "the writes never stopped";
        const unsigned long seen = writes_now();
        if (seen != last) {
            last = seen;
            since = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(last, before) << "the recovery asked the unanswered items again";
    EXPECT_EQ(car.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u) << "the adapter's failure, and nothing since";
}

/* A vehicle that answers none of it (its ignition off) isn't
 * responding, and the link says so for as long as nothing answers: an
 * idle pass never turns that into OK. While it is down the worker
 * writes one request an interval (here kCheckMs) to the module standing
 * in for the silent start's probe (the first: the car asks no PID 0C),
 * the first a whole interval after the items settled. The first
 * answered (a refusal is an answer) ends the timeout, the identification
 * is read after all, and the checks end. */
TEST_F(J2534AdoptionTest,
       AnUnpickedSessionOnASilentVehicleChecksItAndFindsItAwake)
{
    const CheckEvery checks(kCheckMs);
    const char* const kEveryEcu = "FFFFFFFF";
    KnobGuard absent("ECUIO_FAKE_ABSENT_ECU", kEveryEcu);
    CarSession car("j2534-quiet-car");
    ASSERT_NE(car.s, nullptr);
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(car.s, capture_cb, &cap), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(car.s), ECUIO_OK);
    ASSERT_TRUE(within(10000, [&] {
        return car.state() == ECUIO_ERR_VEHICLE_TIMEOUT;
    })) << "what it asked went unanswered";
    const auto settle_by =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (!car.settled()) {
        ASSERT_EQ(car.state(), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "the timeout ended with nothing answered";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto settled_at = std::chrono::steady_clock::now();
    EXPECT_GT(writes_now(), 0u);
    /* The page keeps its first writes: count the checks from here. Its
     * reset clears the knob with them. */
    fake_control::reset(fake_page());
    ASSERT_TRUE(
        fake_control::set(fake_page(), "ECUIO_FAKE_ABSENT_ECU", kEveryEcu));

    /* The first check, an interval on, and the timeout through it */
    const auto first_by = settled_at + std::chrono::milliseconds(8000);
    while (writes_now() == 0) {
        ASSERT_EQ(car.state(), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "an idle pass with nothing picked ended the timeout";
        ASSERT_LT(std::chrono::steady_clock::now(), first_by)
            << "no check within 8 s";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - settled_at);
    /* Half an interval is the bound: this thread may read the clock
     * late; PickOnlyTest pins the interval to the microsecond */
    EXPECT_GE(waited.count(), kCheckMs / 2)
        << "nothing is asked again before an interval has passed";
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    const unsigned char engine_first[7] = {0, 0, 0x07, 0xE0, 0x22, 0x10, 0x00};
    ASSERT_EQ(wlen, 7u);
    EXPECT_EQ(memcmp(wbuf, engine_first, 7), 0)
        << "the engine ECU's first request";
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckMs / 4));
    EXPECT_EQ(writes_now(), 1u) << "one request, once";
    EXPECT_EQ(car.state(), ECUIO_ERR_VEHICLE_TIMEOUT)
        << "its silence changes nothing";
    EXPECT_EQ(cap.count.load(), 0);

    /* The ignition on: the next check, the engine ECU's next row, is
     * answered, refused; 0x7E9 answers the car's first DID but stays
     * closed with nothing picked */
    KnobGuard module("ECUIO_FAKE_MODULE_DIDS", "7E9:1000=11");
    ASSERT_TRUE(
        fake_control::set(fake_page(), "ECUIO_FAKE_ABSENT_ECU", nullptr));
    ASSERT_TRUE(within(8000, [&] { return car.state() == ECUIO_OK; }))
        << "no check found the vehicle awake";
    get_recorded_write(1, wbuf, &wlen, &wflags);
    const unsigned char engine_next[7] = {0, 0, 0x07, 0xE0, 0x22, 0x10, 0x01};
    ASSERT_EQ(wlen, 7u);
    EXPECT_EQ(memcmp(wbuf, engine_next, 7), 0) << "the engine ECU's again";
    /* Its reply is a frame like any, the first this session got */
    ASSERT_GE(cap.count.load(), 1);
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    const unsigned char answer[3] = {0x7F, 0x22, 0x31};
    ASSERT_EQ(cap.payload_lens[0], 3u);
    EXPECT_EQ(memcmp(cap.payloads[0], answer, 3), 0);
    /* The recovery: what went unanswered is asked again, and the engine
     * ECU's VIN is in */
    ASSERT_TRUE(within(10000, [&] {
        ecuio_channel_identity_t id{};
        return ecuio_session_get_channel_identity(car.s, 0, &id) == ECUIO_OK &&
               (id.flags & ECUIO_IDENTITY_VIN) != 0 && car.settled();
    })) << "the identification was never read";
    /* Longer than an interval with no write: the checks ended */
    EXPECT_TRUE(goes_quiet(writes_now, kCheckMs * 3 / 2, 10000))
        << "with the link OK nothing is asked";
    uint32_t episodes = 0;
    EXPECT_EQ(car.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u) << "a vehicle not responding is no episode";
}

/* An adapter whose reads fail and then read again while nothing is
 * picked: the hard failure ends on the adapter's clean reads, with
 * nothing asked that a reply could answer. */
TEST_F(J2534AdoptionTest,
       AnUnpickedSessionLeavesAHardFailureOnceTheAdapterReads)
{
    CarSession car("j2534-failing-car");
    ASSERT_NE(car.s, nullptr);
    ASSERT_EQ(ecuio_session_start(car.s), ECUIO_OK);
    ASSERT_TRUE(within(12000, [&] { return car.settled(); }));
    ASSERT_EQ(car.state(), ECUIO_OK);
    ASSERT_TRUE(goes_quiet(writes_now, 200, 5000));
    const unsigned long before = writes_now();
    {
        /* STATUS_FAILED on every read */
        KnobGuard dead("ECUIO_FAKE_READ_STATUS", "7");
        ASSERT_TRUE(within(5000, [&] {
            return car.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        }));
        /* Past a quiet run's passes at the backoff's base: a failing
         * read is no clean pass, so the failure stays and the backoff
         * grows (at its base the adapter is read 20 times in 2 s) */
        const uint32_t reads = fake_control::reads(fake_page());
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < end) {
            ASSERT_EQ(car.state(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_LE(fake_control::reads(fake_page()) - reads, 10u)
            << "a dead adapter was read at the backoff's base throughout";
    }
    ASSERT_TRUE(within(10000, [&] { return car.state() == ECUIO_OK; }))
        << "the adapter reads again and the state never left the failure";
    uint32_t episodes = 0;
    EXPECT_EQ(car.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u);
    EXPECT_EQ(writes_now(), before) << "nothing was asked to end it";
}

/* ADR-0051's appendix of 2026-11-10: a focus on obd2's computed Boost alone
 * asks its inputs, MAP and baro, and nothing else, and Boost computes from
 * the fake ECU's 35 kPa and 101 kPa */
TEST_F(J2534AdoptionTest, AFocusOnBoostAsksItsInputsAndComputesIt)
{
    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=obd2&filter=0x7E8&probe=0&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const uint16_t boost = ECUIO_OBD2_SIG_BOOST_ID;
    ASSERT_EQ(ecuio_session_set_focus(session, &boost, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    struct Seen {
        std::atomic<int> boosts{0};
        std::atomic<double> last{0.0};
    } seen;
    ASSERT_EQ(ecuio_register_values_callback(
                  session,
                  [](const ecuio_value_batch_t* b, void* user_data) {
                      auto* n = static_cast<Seen*>(user_data);
                      for (uint32_t i = 0; i < b->count; ++i) {
                          if (b->values[i].signal_id == ECUIO_OBD2_SIG_BOOST_ID) {
                              n->last.store(b->values[i].value);
                              ++n->boosts;
                          }
                      }
                  },
                  &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 16, 250);
    for (unsigned long i = 1; i < 16; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_GE(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        EXPECT_TRUE(wbuf[5] == 0x0B || wbuf[5] == 0x33)
            << "write " << i << " asks PID " << int(wbuf[5]);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (seen.boosts.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_GT(seen.boosts.load(), 0) << "Boost computed";
    EXPECT_EQ(seen.last.load(), 35.0 - 101.0);
}

/* ADR-0056 section 4 through the fake's write stamps: two picks with a
 * minimum time of 200 ms, the sweeps at least that far apart at their
 * median and each sweep's two requests together */
TEST_F(J2534AdoptionTest, AMinimumTimeSpacesTheFocusSweeps)
{
    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=obd2&filter=0x7E8&probe=0&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            (d.mux_key == 0x410Cu || d.mux_key == 0x410Du)) {
            ids.push_back(d.id);
        }
    }
    ASSERT_EQ(ids.size(), 2u);
    ASSERT_EQ(ecuio_session_set_focus_with(session, ids.data(), 2, 200, 0),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    const unsigned long want = 12;
    wait_for_writes(get_write_count, want, 500);
    unsigned long written = 0;
    get_write_count(&written);
    ASSERT_GE(written, want);
    std::vector<int64_t> sweeps;
    std::vector<int64_t> within;
    for (uint32_t i = 0; i + 2 < want; i += 2) {
        fake_control::Write a{};
        fake_control::Write b{};
        fake_control::Write c{};
        fake_control::recorded_write(fake_page(), i, &a);
        fake_control::recorded_write(fake_page(), i + 1, &b);
        fake_control::recorded_write(fake_page(), i + 2, &c);
        sweeps.push_back(static_cast<int64_t>(c.us - a.us));
        within.push_back(static_cast<int64_t>(b.us - a.us));
    }
    std::sort(sweeps.begin(), sweeps.end());
    std::sort(within.begin(), within.end());
    EXPECT_GE(sweeps[sweeps.size() / 2], 190000)
        << "sweeps came faster than the minimum";
    EXPECT_LT(within[within.size() / 2], 100000)
        << "a sweep's requests were spread over the gap";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* The obd2 set's id for Service 01 PID pid */
uint16_t obd2_id_of(const ecuio_signal_set_t* set, uint8_t pid)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            d.mux_key == (0x4100u | pid)) {
            return d.id;
        }
    }
    ADD_FAILURE() << "no def reads PID " << int(pid);
    return 0;
}

/* ?poll=0x0C asks that one request, so a focus on 0x0D alone would
 * leave the session nothing to ask: it's refused, before start and
 * while running, and the session goes on asking 0x0C; a focus on 0x0C
 * is taken. A closed session refuses any focus. */
TEST_F(J2534AdoptionTest, AFocusUnderPollIsJudgedByThePolledPid)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&poll=0x0C",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    const uint16_t rpm = obd2_id_of(set, 0x0C);
    const uint16_t speed = obd2_id_of(set, 0x0D);
    EXPECT_EQ(ecuio_session_set_focus(session, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(ecuio_session_set_focus(session, &rpm, 1), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(session, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);

    wait_for_writes(get_write_count, 5, 200);
    for (unsigned long i = 0; i < 5; ++i) {
        expect_write(i, 0x01, 0x0C);
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(session, &rpm, 1),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(session);
}

/* A focus outlives the link's loss and return: the probe's walk and the
 * rescan the recovery runs ask what they must, and once the walk is
 * done every request out names picks alone (a pack of them too). */
TEST_F(J2534AdoptionTest, AFocusOutlivesTheLinksRecovery)
{
    std::unique_ptr<KnobGuard> quiet =
        std::make_unique<KnobGuard>("ECUIO_FAKE_READ_STATUS", "9");
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    const uint16_t ids[2] = {obd2_id_of(set, 0x0C), obd2_id_of(set, 0x0D)};
    ASSERT_EQ(ecuio_session_set_focus(session, ids, 2), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    int32_t le = 0;
    uint32_t ec = 0;
    for (int i = 0; i < 500 && le != ECUIO_ERR_VEHICLE_TIMEOUT; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)ecuio_session_get_error_state(session, &le, &ec);
    }
    ASSERT_EQ(le, ECUIO_ERR_VEHICLE_TIMEOUT) << "the bus never read as quiet";

    quiet.reset();
    ecuio_channel_support_t s = {};
    ASSERT_TRUE(wait_for_support(
        session, s,
        [](const ecuio_channel_support_t& r) {
            return (r.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        3000))
        << "the walk didn't resume (flags " << s.flags << ")";
    fake_control::reset(fake_page());
    wait_for_writes(get_write_count, 16, 250);
    for (unsigned long i = 0; i < 16; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_GE(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        for (unsigned long k = 5; k < wlen; ++k) {
            EXPECT_TRUE(wbuf[k] == 0x0C || wbuf[k] == 0x0D)
                << "write " << i << " asks PID " << int(wbuf[k]);
        }
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

} // namespace

#endif // _WIN32
