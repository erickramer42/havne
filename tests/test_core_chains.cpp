// File/layer: tests/test_core_chains.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

/* The validator's depths are a heap byte per program, taken only for a
 * set with a chain; no room is a code of its own, which registration
 * gives as out of memory, the registry unchanged */
TEST(CoreTest, NoRoomToCheckAChainIsOutOfMemory)
{
    static ChainSet chain;
    static ComputedSet flat;
    unsigned long before = new_counter::on_this_thread();
    EXPECT_EQ(ecuio_validate_signal_set(&flat.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(new_counter::on_this_thread() - before, 0u)
        << "a set with no chain allocates nothing";
    before = new_counter::on_this_thread();
    EXPECT_EQ(ecuio_validate_signal_set(&chain.set, nullptr),
              ECUIO_VALIDATE_OK);
    EXPECT_EQ(new_counter::on_this_thread() - before, 1u) << "the depths";

    fail_new_on_this_thread(1);
    ecuio_validation_t v{};
    v.index = 7;
    EXPECT_EQ(ecuio_validate_signal_set(&chain.set, &v),
              ECUIO_VALIDATE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(v.index, UINT32_MAX);
    fail_new_on_this_thread(1);
    EXPECT_EQ(ecuio_profile_register("test-chain-oom", &chain.set, &v),
              ECUIO_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(v.code, ECUIO_VALIDATE_ERR_OUT_OF_MEMORY);
    const ecuio_signal_set_t* found = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("test-chain-oom", &found),
              ECUIO_ERR_UNKNOWN_PROFILE);
}

/* A computed signal carries no request: a focus on one keeps the number
 * defs under it asked, down its chain (ADR-0062), and parks every other
 * request */
TEST(CoreTest, AFocusOnAComputedSignalKeepsItsLeavesAsked)
{
    const std::string file = R"({"havne_signals": 2, "name": "Focus chain",
 "source": {"name": "Havne's tests"}, "extends": "obd2", "signals": [
 {"key": "per_rev", "name": "Per Rev", "kind": "computed", "expr": "obd2.maf / obd2.rpm"},
 {"key": "scaled", "name": "Scaled", "kind": "computed", "expr": "per_rev * 1000"}]})";
    ecuio_signal_file_t* sf = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &sf), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_profile_register(ecuio_signal_file_profile(sf),
                                     ecuio_signal_file_set(sf), nullptr),
              ECUIO_OK);
    const std::string uri =
        std::string("mock://focus?profile=") + ecuio_signal_file_profile(sf);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ASSERT_GT(e->count, 2u);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);

    const uint16_t scaled = ecuio_signal_file_find(sf, "scaled")->id;
    ASSERT_EQ(ecuio_session_set_focus(s, &scaled, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, std::vector<uint16_t>{scaled})
            << "the picks as given";
    }
    ecuio_take_focus(s, e.get(), seen);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < e->count; ++k) {
        const ecuio_request& r = e->requests[k];
        if (r.len != 2 || r.bytes[0] != 0x01 || r.bytes[1] % 0x20 == 0) {
            continue; /* a data PID's, not a bitmap's */
        }
        const bool leaf = r.bytes[1] == 0x0C || r.bytes[1] == 0x10;
        EXPECT_EQ(r.parked, leaf ? 0u : 1u)
            << "PID " << static_cast<unsigned>(r.bytes[1]);
        kept += leaf ? 1u : 0u;
    }
    EXPECT_EQ(kept, 2u) << "RPM and MAF";
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_profile_unregister(ecuio_signal_file_profile(sf)),
              ECUIO_OK);
    ecuio_signal_file_free(sf);
}

/* A packed Service 01 reply (ADR-0039 Appendix A) decodes record by
 * record, and its computed values follow once, after every record: MAP
 * (0x0B) and baro (0x33) in one reply give one boost, from that reply's
 * baro, not one per record from the baro before it */
TEST(CoreTest, AComputedValueFollowsAPackedReplyOnce)
{
    static ecuio_signal_def_t defs[3];
    const uint8_t pids[2] = {0x0B, 0x33};
    for (uint16_t i = 0; i < 2; ++i) {
        defs[i] = value_def(static_cast<uint16_t>(i + 1), ECUIO_VALUE_UINT,
                            ECUIO_OBD2_ECU_ARB_ID);
        defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
        defs[i].start_byte = 2;
        defs[i].bit_length = 8;
        defs[i].mux_length = 2;
        defs[i].mux_key = 0x4100u | pids[i];
    }
    defs[2] = value_def(3, ECUIO_VALUE_UINT, 0);
    defs[2].flags = ECUIO_DEF_FLAG_COMPUTED;
    static ecuio_computed_t boost{};
    boost.input_ids[0] = 1;
    boost.input_ids[1] = 2;
    boost.input_count = 2;
    boost.step_count = 3;
    boost.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    boost.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
    boost.steps[2] = ECUIO_COMPUTED_SUBTRACT;
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 3;
    set.defs = defs;
    set.computed_count = 1;
    set.computed = &boost;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-computed-packed", &set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://t?profile=test-computed-packed",
                                   &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t baro =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x33, 90}, 1000000);
    const ecuio_frame_t packed = value_frame(
        ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0B, 100, 0x33, 95}, 2000000);
    ecuio_session_dispatch_callbacks(session, &baro);
    ecuio_session_dispatch_callbacks(session, &packed);
    ASSERT_EQ(cap.batches.size(), 2u);
    const std::vector<SeenValue>& v = cap.batches[1].values;
    ASSERT_EQ(v.size(), 3u) << "MAP, baro, then one boost";
    EXPECT_EQ(v[0].id, 1);
    EXPECT_EQ(v[0].value, 100.0);
    EXPECT_EQ(v[1].id, 2);
    EXPECT_EQ(v[1].value, 95.0);
    EXPECT_EQ(v[2].id, 3);
    EXPECT_EQ(v[2].value, 5.0) << "from this reply's baro";
    EXPECT_EQ(cap.batches[1].timestamp_us, 2000000u);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-packed"), ECUIO_OK);
}

/* Reply timing (ADR-0042): a session with no rotation has no rows and
 * no clock, the count asked with capacity 0, and the NULLs refused */
TEST(CoreTest, ASessionWithNoRotationHasNoTimingRows)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://a?profile=obd2", &session),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ecuio_session_timing_t t{};
    t.rows = 99;
    ecuio_request_timing_t row{};
    uint32_t count = 99;
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(t.rows, 0u);
    EXPECT_EQ(t.interval_clock, ECUIO_TIMING_CLOCK_NONE);
    EXPECT_EQ(t.flags, 0u);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, &row, 1, &count), ECUIO_OK);
    EXPECT_EQ(ecuio_session_get_timing(nullptr, &t, &row, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, nullptr, &row, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, &row, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_close(session);
    EXPECT_EQ(ecuio_session_get_timing(session, &t, nullptr, 0, &count),
              ECUIO_OK)
        << "readable after close";
    ecuio_session_destroy(session);
}

/* ADR-0039 section 6: the test car answers PID 55 with bank 1's byte alone
 * (docs/bench-sessions/2026-10-31-elm327-vehicle.md), so the bank 3
 * field is absent once ECUIO_ABSENT_AFTER_FRAMES replies have ended
 * before it, and leaves the list when a reply carries it. Bank 1 is
 * never absent; a session with no values callback judges nothing; the
 * list stays readable after close. */
TEST(CoreTest, AFieldPastEveryReplyIsAbsent)
{
    const uint16_t b3 = ECUIO_OBD2_PID_SIG_ID(0x55, 1);
    const ecuio_frame_t one =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80});
    const ecuio_frame_t both =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x55, 0x80, 0x7F});
    uint16_t ids[4] = {};
    uint32_t count = 99;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://a?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    for (uint32_t k = 1; k <= ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
        ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
                  ECUIO_OK);
        EXPECT_EQ(count, k < ECUIO_ABSENT_AFTER_FRAMES ? 0u : 1u)
            << "after " << k << " short replies";
    }
    EXPECT_EQ(ids[0], b3);
    EXPECT_EQ(ecuio_session_get_absent_signals(session, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u) << "capacity 0 asks the count";
    EXPECT_EQ(ecuio_session_get_absent_signals(session, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_session_close(session);
    EXPECT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u) << "readable after close";
    ecuio_session_destroy(session);

    ASSERT_EQ(
        ecuio_session_create("mock://b?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
    }
    ecuio_session_dispatch_callbacks(session, &both);
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "a reply that carries it clears it";
    ecuio_session_destroy(session);

    ASSERT_EQ(
        ecuio_session_create("mock://c?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(session, &one);
    }
    ASSERT_EQ(ecuio_session_get_absent_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "no values callback, nothing decoded or judged";
    ecuio_session_destroy(session);
}

/* ADR-0027 section 3: the VIN a vehicle is known by: 09 02's text, else
 * F190's; letters and digits only, upper case, at 17 characters */
TEST(CoreTest, TheVinIsService09sThenF190s)
{
    const auto item = [](std::initializer_list<uint8_t> request,
                         uint8_t outcome, const std::string& text) {
        ecuio_identity_item_t it{};
        uint8_t k = 0;
        for (uint8_t b : request) {
            it.request[k++] = b;
        }
        it.length = k;
        it.outcome = outcome;
        it.text_length = static_cast<uint8_t>(text.size());
        memcpy(it.text, text.data(), text.size());
        return it;
    };
    const auto vin = [](const ecuio_channel_identity_t& id) {
        char out[ECUIO_VIN_TEXT_LEN] = "x";
        EXPECT_EQ(ecuio_identity_vin(&id, out, sizeof out), ECUIO_OK);
        return std::string(out);
    };
    ecuio_channel_identity_t id{};
    id.flags = ECUIO_IDENTITY_COMPLETE;
    id.items = 2;
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "f190vin0000000002");
    id.list[1] = item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED,
                      std::string(2, '\0') + " FAKEJ2534VIN00001");
    EXPECT_EQ(vin(id), "FAKEJ2534VIN00001")
        << "09 02 first, its padding dropped";

    id.list[1].outcome = ECUIO_IDENTITY_REFUSED;
    EXPECT_EQ(vin(id), "F190VIN0000000002")
        << "F190 when 09 02 wasn't answered, in upper case";

    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN0001");
    EXPECT_EQ(vin(id), "") << "16 characters";
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN000012");
    EXPECT_EQ(vin(id), "") << "18 characters";

    id.list[0] =
        item({0x09, 0x04}, ECUIO_IDENTITY_ANSWERED, "CALIBRATION00001X");
    EXPECT_EQ(vin(id), "") << "a CALID of 17 isn't a VIN";

    id.items = 1;
    id.list[1] =
        item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN00001");
    EXPECT_EQ(vin(id), "") << "past the count isn't read";

    char small[ECUIO_VIN_TEXT_LEN - 1] = "x";
    EXPECT_EQ(ecuio_identity_vin(&id, small, sizeof small),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(small[0], 'x') << "untouched";
    EXPECT_EQ(ecuio_identity_vin(nullptr, small, sizeof small),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_identity_vin(&id, nullptr, ECUIO_VIN_TEXT_LEN),
              ECUIO_ERR_NULL_ARGUMENT);
}

/* ecuio_session_get_unsupported_signals' contract on a channel that has
 * learned nothing (the mock under its own profile, generation 0): none,
 * before start and after close; NULLs refused as the absent signals'
 * are. The judgment itself is PidSupportTest's (test_obd2), and a
 * session's through a fake ECU
 * Elm327SessionTest.TheSignalsOfAWithheldPidAreUnsupported */
TEST(CoreTest, AChannelThatLearnedNothingHasNoUnsupportedSignals)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://a?diagnose=0", &session), ECUIO_OK);
    uint16_t ids[4] = {};
    uint32_t count = 99;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(
        ecuio_session_get_unsupported_signals(session, nullptr, 0, &count),
        ECUIO_OK);
    EXPECT_EQ(
        ecuio_session_get_unsupported_signals(session, nullptr, 1, &count),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unsupported_signals(nullptr, ids, 4, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_session_close(session);
    count = 99;
    EXPECT_EQ(ecuio_session_get_unsupported_signals(session, ids, 4, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "readable after close";
    ecuio_session_destroy(session);
}



/* The mock answers Service 01 from its drive cycle under a profile that
 * reads PIDs: its ECU's supported-PID walk lists what it answers before
 * the session is handed out, and core lists the set's other Service 01
 * signals as its ECU not sending them (ADR-0039 section 6); Boost, whose
 * MAP and baro it sends, isn't among them. With diagnostics on or off,
 * before start and after close. */
TEST(CoreTest, TheMockSendsTheDemoCarsPidsAndDisclaimsTheRest)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> others;
    uint32_t requests = 0;
    uint32_t answered = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue;
        }
        const uint8_t pid = static_cast<uint8_t>(d.mux_key);
        const bool sent = std::find(kDemoPids.begin(), kDemoPids.end(), pid) !=
                          kDemoPids.end();
        if (!sent) {
            others.push_back(d.id);
        }
        /* a request per PID: its first field's def */
        bool first = true;
        for (uint32_t k = 0; k < i && first; ++k) {
            first = set->defs[k].mux_key != d.mux_key ||
                    (set->defs[k].flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
        }
        requests += first ? 1u : 0u;
        answered += (first && sent) ? 1u : 0u;
    }
    ASSERT_GE(answered, 15u) << "the set holds the drive cycle's PIDs";

    for (const char* uri :
         {"mock://a?profile=obd2&diagnose=0", "mock://a?profile=obd2"}) {
        SCOPED_TRACE(uri);
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
        ecuio_channel_support_t s{};
        ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
        EXPECT_EQ(s.rx_id, 0x7E8u);
        EXPECT_NE(s.generation, 0u) << "learned before start";
        EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
        EXPECT_EQ(s.ranges, 0x07u) << "00, 20 and 40: the last PID is 5C";
        EXPECT_EQ(s.pids_supported, kDemoPids.size());
        EXPECT_EQ(s.pids_through, 0x60u);
        EXPECT_EQ(s.requests, requests);
        EXPECT_EQ(s.dropped, requests - answered) << "the rest, by the bitmaps";
        EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_BITMAP);
        EXPECT_EQ(s.scan_total, 0u);
        EXPECT_EQ(unsupported_ids(session), others);

        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        ecuio_channel_support_t later{};
        ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &later),
                  ECUIO_OK);
        EXPECT_EQ(later.generation, s.generation) << "nothing asked again";
        ecuio_session_close(session);
        EXPECT_EQ(unsupported_ids(session), others) << "readable after close";
        ecuio_session_destroy(session);
    }
}

/* The ids a values callback saw */
struct SeenValues {
    std::mutex m;
    std::vector<uint16_t> ids;
    std::vector<uint32_t> frame_flags;
};

void see_values(const ecuio_value_batch_t* batch, void* user)
{
    auto* seen = static_cast<SeenValues*>(user);
    std::lock_guard<std::mutex> lock(seen->m);
    for (uint32_t i = 0; i < batch->count; ++i) {
        seen->ids.push_back(batch->values[i].signal_id);
    }
}

void see_frames(const ecuio_frame_t* frame, void* user)
{
    auto* seen = static_cast<SeenValues*>(user);
    std::lock_guard<std::mutex> lock(seen->m);
    if (frame->arbitration_id == 0x7E8u) {
        seen->frame_flags.push_back(frame->flags);
    }
}

/* The mock reads a signal of no declared ECU as its engine ECU's, where
 * registration counted it for the set's first ECU. A set that registers
 * can so ask the mock's engine ECU more than a module's most: create
 * refuses it as the address's, in words, never as out of memory. */
TEST(CoreTest, TheMockRefusesInWordsASetThatOverfillsItsEngineEcu)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    constexpr uint32_t kOwn = 1000;  /* the engine ECU's DIDs */
    constexpr uint32_t kStray = 100; /* DIDs on an ID no ECU declares */
    static_assert(kOwn + 1 <= ECUIO_MODULE_MAX_REQUESTS &&
                      kOwn + 1 + kStray > ECUIO_MODULE_MAX_REQUESTS,
                  "the engine ECU's own fit, and not with the stray ones");
    static ecuio_channel_decl_t other_first[2] = {{0x7E9, 0x7E1, 0, 0, {}},
                                                  {0x7E8, 0x7E0, 0, 0, {}}};
    static ecuio_signal_def_t defs[kOwn + kStray + 1];
    for (uint32_t i = 0; i < kOwn + kStray; ++i) {
        defs[i] = obd2->defs[0];
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].message_id = i < kOwn ? 0x7E8u : 0x7EAu;
        defs[i].byte_order = ECUIO_BYTE_ORDER_BE;
        defs[i].start_byte = 3;
        defs[i].bit_length = 8;
        defs[i].mux_length = 3;
        defs[i].mux_key = 0x620000u | (0x1000u + i);
        std::snprintf(defs[i].name, sizeof defs[i].name, "Signal %u", i);
    }
    /* One Service 01 signal, so the mock walks the PIDs */
    defs[kOwn + kStray] = obd2->defs[0];
    ASSERT_EQ(defs[kOwn + kStray].mux_length, 2u);
    ASSERT_EQ(defs[kOwn + kStray].mux_key >> 8, 0x41u);
    ASSERT_EQ(defs[kOwn + kStray].message_id, 0x7E8u);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = kOwn + kStray + 1;
    set.defs = defs;
    set.channel_count = 2;
    set.channels = other_first;

    ecuio_validation_t v;
    ASSERT_EQ(ecuio_profile_register("test-stray", &set, &v), ECUIO_OK)
        << v.code << " at " << v.index;
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://a?profile=test-stray", &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the profile asks the mock's engine ECU (0x7E8) more than 1024 "
              "requests: the mock reads the signals of no declared ECU as "
              "the engine ECU's");
    ecuio_session_destroy(session);

    /* Without the stray ones it connects */
    set.count = kOwn;
    defs[kOwn - 1] = defs[kOwn + kStray];
    session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://a?profile=test-stray", &session),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-stray"), ECUIO_OK);
}

/* A set that declares another ECU: the mock declares its channel after
 * the engine ECU's, and that ECU answers the supported-PID walk with
 * bitmaps listing no data PID, so a Service 01 signal read there is listed
 * as not sent while the same PID on the engine ECU is sent. A set whose
 * ECUs, with the engine ECU's first, pass ECUIO_MAX_CHANNELS is refused. */
TEST(CoreTest, TheMockDeclaresTheSetsOtherEcusWhichSupportNoPid)
{
    static const char kTwoEcus[] =
        R"({"havne_signals":1,"name":"Two ECUs","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"},{"id":"tcm","rx":"7E9"}],)"
        R"("signals":[)"
        R"({"key":"rpm","name":"Engine RPM","ecu":"pcm",)"
        R"("read":{"service":"01","number":"0C"},)"
        R"("field":{"byte":0,"bits":16,"order":"be"}},)"
        R"({"key":"tcm_rpm","name":"TCM RPM","ecu":"tcm",)"
        R"("read":{"service":"01","number":"0C"},)"
        R"("field":{"byte":0,"bits":16,"order":"be"}}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(kTwoEcus, sizeof kTwoEcus - 1, &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const char* profile = ecuio_signal_file_profile(file);
    ASSERT_EQ(
        ecuio_profile_register(profile, ecuio_signal_file_set(file), nullptr),
        ECUIO_OK);
    const uint16_t engine_rpm = ecuio_signal_file_find(file, "rpm")->id;
    const uint16_t tcm_rpm = ecuio_signal_file_find(file, "tcm_rpm")->id;

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create(
            (std::string("mock://two?diagnose=0&profile=") + profile).c_str(),
            &session),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    ASSERT_EQ(info.channel_count, 2u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    EXPECT_EQ(info.rx_ids[1], 0x7E9u);
    EXPECT_EQ(info.tx_ids[1], 0x7E1u);

    ecuio_channel_support_t engine{};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &engine), ECUIO_OK);
    EXPECT_EQ(engine.rx_id, 0x7E8u);
    EXPECT_EQ(engine.pids_supported, kDemoPids.size());
    ecuio_channel_support_t tcm{};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 1, &tcm), ECUIO_OK);
    EXPECT_EQ(tcm.rx_id, 0x7E9u);
    EXPECT_NE(tcm.generation, 0u) << "walked before start";
    EXPECT_EQ(tcm.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(tcm.ranges, 0xFFu) << "every bitmap answered, each linking on";
    EXPECT_EQ(tcm.pids_supported, 0u);
    EXPECT_EQ(tcm.requests, 1u);
    EXPECT_EQ(tcm.dropped, 1u);
    EXPECT_EQ(unsupported_ids(session), std::vector<uint16_t>{tcm_rpm});

    /* The engine ECU's 0C is sent; nothing on 0x7E9 */
    SeenValues seen;
    ASSERT_EQ(ecuio_register_values_callback(session, see_values, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ecuio_session_close(session);
    {
        std::lock_guard<std::mutex> lock(seen.m);
        EXPECT_NE(std::find(seen.ids.begin(), seen.ids.end(), engine_rpm),
                  seen.ids.end());
        EXPECT_EQ(std::find(seen.ids.begin(), seen.ids.end(), tcm_rpm),
                  seen.ids.end());
    }
    ecuio_session_destroy(session);
    ASSERT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    ecuio_signal_file_free(file);

    /* A profile with no channel of its own, or with only the engine ECU's,
     * has the one */
    for (const char* uri : {"mock://a?diagnose=0", "mock://a?profile=obd2",
                            "mock://a?profile=raw"}) {
        SCOPED_TRACE(uri);
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
        ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
        EXPECT_EQ(info.channel_count, 1u);
        EXPECT_EQ(info.rx_ids[0], 0x7E8u);
        ecuio_session_destroy(session);
    }

    /* As many other ECUs as a set holds: with the engine ECU's channel
     * they are one too many, said in words; one fewer and each has its
     * channel after the engine ECU's */
    const auto others = [](uint32_t n) {
        std::string uri = "mock://a?diagnose=0&profile=uds-generic&did=";
        for (uint32_t i = 0; i < n; ++i) {
            char item[16];
            std::snprintf(item, sizeof item, "%s%03X:D100", i > 0 ? "," : "",
                          0x700u + i);
            uri += item;
        }
        return uri;
    };
    session = nullptr;
    EXPECT_EQ(ecuio_session_create(others(ECUIO_MAX_CHANNELS).c_str(),
                                   &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the profile declares 32 ECUs and none is the mock's engine "
              "ECU (0x7E8): with it they are one more than the 32 a session "
              "reads");
    const std::string full = others(ECUIO_MAX_CHANNELS - 1);
    ASSERT_EQ(ecuio_session_create(full.c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    ASSERT_EQ(info.module_count, uint32_t{ECUIO_MAX_CHANNELS});
    ASSERT_EQ(info.channel_count, uint32_t{ECUIO_SESSION_INFO_MAX_CHANNELS});
    for (uint32_t c = 0; c < ECUIO_MAX_CHANNELS; ++c) {
        const uint32_t want = c == 0 ? 0x7E8u : 0x700u + c - 1;
        ecuio_channel_decl_t m{};
        ASSERT_EQ(ecuio_session_get_module(session, c, &m), ECUIO_OK) << c;
        EXPECT_EQ(m.rx_id, want) << c;
        if (c < ECUIO_SESSION_INFO_MAX_CHANNELS) {
            EXPECT_EQ(info.rx_ids[c], want) << c;
        }
        ecuio_channel_support_t s{};
        ASSERT_EQ(ecuio_session_get_channel_support(session, c, &s), ECUIO_OK);
        EXPECT_EQ(s.rx_id, want);
        EXPECT_EQ(s.generation, 0u) << "no PID to walk for";
    }
    ecuio_session_destroy(session);
}

/* A running demo car under obd2 sends each PID it answers as a whole
 * reply on 0x7E8, and Boost is worked out from its MAP and baro */
TEST(CoreTest, TheDemoCarUnderObd2SendsItsValues)
{
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://demo?profile=obd2&diagnose=0", &session),
        ECUIO_OK);
    SeenValues seen;
    ASSERT_EQ(ecuio_register_values_callback(session, see_values, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_register_frame_callback(session, see_frames, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ecuio_session_close(session);
    std::lock_guard<std::mutex> lock(seen.m);
    for (const uint16_t id :
         {uint16_t{ECUIO_OBD2_SIG_RPM_ID}, uint16_t{ECUIO_OBD2_SIG_SPEED_ID},
          uint16_t{ECUIO_OBD2_SIG_BATTERY_V_ID},
          static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(0x0B, 0)),
          static_cast<uint16_t>(ECUIO_OBD2_PID_SIG_ID(0x33, 0)),
          uint16_t{ECUIO_OBD2_SIG_BOOST_ID}}) {
        EXPECT_NE(std::find(seen.ids.begin(), seen.ids.end(), id),
                  seen.ids.end())
            << id;
    }
    ASSERT_FALSE(seen.frame_flags.empty());
    for (const uint32_t flags : seen.frame_flags) {
        EXPECT_EQ(flags, ECUIO_FRAME_FLAG_REASSEMBLED);
    }
    ecuio_session_destroy(session);
}

