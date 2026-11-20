// File/layer: tests/test_core_values.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;


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

/* Every text value of a batch has bytes of its own (ADR-0024 section 4): an
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


/* A computed value follows a frame that decodes one of its inputs,
 * from each input's newest value no older than its bound, after the
 * frame's own values (ADR-0051 section 5); an enum's value is its raw
 * number with its label, or none for a raw value with no entry */
TEST(CoreTest, ComputedValuesFollowTheirInputsWithinTheirAges)
{
    static ComputedSet s;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-computed", &s.set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://t?profile=test-computed", &session),
              ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const auto send = [&](uint32_t id, std::initializer_list<uint8_t> bytes,
                          double seconds) {
        const ecuio_frame_t f =
            value_frame(id, bytes, static_cast<uint64_t>(seconds * 1e6));
        ecuio_session_dispatch_callbacks(session, &f);
    };
    send(0x123, {100, 1}, 0.0);    /* 0: MAP, no baro yet */
    send(0x124, {95}, 1.0);        /* 1: baro, MAP 1 s old */
    send(0x123, {110, 7}, 2.0);    /* 2: an unlabeled mode */
    send(0x123, {120, 2}, 20.0);   /* 3: baro 19 s old: boost's 30 s, not the ratio's 3 s */
    send(0x123, {130, 2}, 40.0);   /* 4: baro 39 s old: neither */
    send(0x124, {0}, 41.0);        /* 5: a zero baro: no ratio */
    send(0x125, {1}, 42.0);        /* 6: decodes nothing, feeds nothing */
    ASSERT_EQ(cap.batches.size(), 6u);
    const std::vector<SeenBatch>& b = cap.batches;

    ASSERT_EQ(b[0].values.size(), 2u) << "MAP and the mode alone";
    EXPECT_EQ(b[0].values[1].text, "Open loop");
    EXPECT_EQ(b[0].values[1].value, 1.0);
    EXPECT_EQ(b[0].values[1].kind, ECUIO_VALUE_ENUM);
    EXPECT_EQ(b[0].values[1].text_len, 9u);

    ASSERT_EQ(b[1].values.size(), 3u);
    EXPECT_EQ(b[1].values[0].id, 3) << "the frame's own value first";
    EXPECT_EQ(b[1].values[1].id, 4);
    EXPECT_EQ(b[1].values[1].value, 5.0);
    EXPECT_EQ(b[1].values[1].kind, ECUIO_VALUE_UINT);
    EXPECT_EQ(b[1].values[1].text_at, nullptr);
    EXPECT_EQ(b[1].values[2].id, 5);
    EXPECT_DOUBLE_EQ(b[1].values[2].value, 100.0 / 95.0);
    EXPECT_EQ(b[1].timestamp_us, 1000000u);

    ASSERT_EQ(b[2].values.size(), 4u);
    EXPECT_EQ(b[2].values[1].value, 7.0);
    EXPECT_EQ(b[2].values[1].text_at, nullptr) << "no label: the number shows";
    EXPECT_EQ(b[2].values[1].text_len, 0u);
    EXPECT_EQ(value_of(b[2], 4)->value, 15.0);

    ASSERT_NE(value_of(b[3], 4), nullptr) << "baro's own bound, 30 s";
    EXPECT_EQ(value_of(b[3], 4)->value, 25.0);
    EXPECT_EQ(value_of(b[3], 5), nullptr) << "the stale age, 3 s";
    EXPECT_EQ(value_of(b[3], 2)->text, "Closed loop");

    EXPECT_EQ(value_of(b[4], 4), nullptr) << "39 s is past 30";
    EXPECT_EQ(b[4].values.size(), 2u);

    ASSERT_NE(value_of(b[5], 4), nullptr) << "MAP 1 s old";
    EXPECT_EQ(value_of(b[5], 4)->value, 130.0);
    EXPECT_EQ(value_of(b[5], 5), nullptr) << "a division by zero";

    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed"), ECUIO_OK);
}

/* A computed value's age bound is inclusive, and an input decoded
 * later than the frame being computed (a clock that stepped back)
 * gives no value */
TEST(CoreTest, AComputedInputAtItsBoundStillCounts)
{
    static ComputedSet s;
    ASSERT_EQ(ecuio_profile_register("test-computed-age", &s.set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-computed-age", &session),
        ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t baro = value_frame(0x124, {100}, 10000000);
    const ecuio_frame_t at = value_frame(0x123, {110, 1},
                                         10000000 + ECUIO_VALUE_STALE_US);
    const ecuio_frame_t past = value_frame(0x123, {111, 1},
                                           10000001 + ECUIO_VALUE_STALE_US);
    const ecuio_frame_t back = value_frame(0x124, {100}, 20000000);
    const ecuio_frame_t earlier = value_frame(0x123, {112, 1}, 15000000);
    for (const ecuio_frame_t* f : {&baro, &at, &past, &back, &earlier}) {
        ecuio_session_dispatch_callbacks(session, f);
    }
    ASSERT_EQ(cap.batches.size(), 5u);
    ASSERT_NE(value_of(cap.batches[1], 5), nullptr) << "exactly 3 s";
    EXPECT_DOUBLE_EQ(value_of(cap.batches[1], 5)->value, 1.1);
    EXPECT_EQ(value_of(cap.batches[2], 5), nullptr) << "1 us past it";
    EXPECT_NE(value_of(cap.batches[2], 4), nullptr) << "boost's 30 s";
    EXPECT_EQ(value_of(cap.batches[4], 4), nullptr) << "baro newer than the frame";
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-age"), ECUIO_OK);
}

/* A signal file's computed signal reads each input no older than
 * ECUIO_VALUE_STALE_US (3 s) unless its max_age gives that input more:
 * inputs 4 s apart give no value, and the same expression with max_age
 * 10 on the older input gives one */
TEST(CoreTest, AComputedSignalsInputsFourSecondsApartNeedItsMaxAge)
{
    const std::string file = R"({"havne_signals": 1, "name": "Inputs apart",
 "source": {"name": "Havne's tests"}, "extends": "obd2", "signals": [
 {"key": "plain", "name": "Plain", "kind": "computed", "expr": "obd2.rpm - obd2.speed"},
 {"key": "aged", "name": "Aged", "kind": "computed", "expr": "obd2.rpm - obd2.speed",
  "max_age": {"obd2.speed": 10}}]})";
    ecuio_signal_file_t* sf = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &sf), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_profile_register(ecuio_signal_file_profile(sf),
                                     ecuio_signal_file_set(sf), nullptr),
              ECUIO_OK);
    const std::string uri =
        std::string("mock://apart?profile=") + ecuio_signal_file_profile(sf);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    /* Speed 50 km/h at 10 s; 1000 rpm (4000 quarter turns) at 13 s and 14 s */
    const ecuio_frame_t speed =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0D, 50}, 10000000);
    const ecuio_frame_t at3 =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x0F, 0xA0}, 13000000);
    const ecuio_frame_t at4 =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x0F, 0xA0}, 14000000);
    for (const ecuio_frame_t* f : {&speed, &at3, &at4}) {
        ecuio_session_dispatch_callbacks(session, f);
    }
    const uint16_t plain = ecuio_signal_file_find(sf, "plain")->id;
    const uint16_t aged = ecuio_signal_file_find(sf, "aged")->id;
    ASSERT_EQ(cap.batches.size(), 3u);
    ASSERT_NE(value_of(cap.batches[1], plain), nullptr) << "3 s apart";
    EXPECT_EQ(value_of(cap.batches[1], plain)->value, 950.0);
    EXPECT_EQ(value_of(cap.batches[2], plain), nullptr)
        << "4 s apart: past the 3 s bound";
    ASSERT_NE(value_of(cap.batches[2], aged), nullptr) << "max_age 10";
    EXPECT_EQ(value_of(cap.batches[2], aged)->value, 950.0);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister(ecuio_signal_file_profile(sf)),
              ECUIO_OK);
    ecuio_signal_file_free(sf);
}

/* Each input past its bound or never seen reaches the program as no
 * value, so an if's branch not taken can hold one (ADR-0051 section 5's
 * format 2 rule), and a format 1 program over a stale input still gives
 * none, as it did when any stale input blanked it. A registered set: no
 * file writes these steps yet. */
TEST(CoreTest, AStaleInputIsNoValueOnlyWhereTheProgramReadsIt)
{
    static ecuio_signal_def_t defs[4];
    static ecuio_computed_t computed[2];
    static ecuio_signal_set_t set{};
    defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123); /* a gear */
    defs[0].bit_length = 8;
    defs[1] = value_def(3, ECUIO_VALUE_UINT, 0x124); /* a speed */
    defs[1].bit_length = 8;
    for (uint16_t i = 2; i < 4; ++i) {
        defs[i] = value_def(static_cast<uint16_t>(i + 2), ECUIO_VALUE_UINT, 0);
        defs[i].flags = ECUIO_DEF_FLAG_COMPUTED;
        defs[i].table_first = static_cast<uint16_t>(i - 2);
    }
    for (ecuio_computed_t& c : computed) {
        c = ecuio_computed_t{};
        c.input_ids[0] = 1;
        c.input_ids[1] = 3;
        c.input_count = 2;
    }
    /* if(gear == 0, 0, speed) */
    const uint8_t pick[] = {
        ECUIO_COMPUTED_PUSH_INPUT | 0u, ECUIO_COMPUTED_PUSH_CONSTANT | 0u,
        ECUIO_COMPUTED_EQUAL,           ECUIO_COMPUTED_PUSH_CONSTANT | 0u,
        ECUIO_COMPUTED_PUSH_INPUT | 1u, ECUIO_COMPUTED_IF};
    std::memcpy(computed[0].steps, pick, sizeof pick);
    computed[0].step_count = sizeof pick;
    computed[0].constant_count = 1;
    /* gear - speed, format 1 */
    computed[1].steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
    computed[1].steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
    computed[1].steps[2] = ECUIO_COMPUTED_SUBTRACT;
    computed[1].step_count = 3;
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 4;
    set.defs = defs;
    set.computed_count = 2;
    set.computed = computed;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-computed-stale", &set, &why),
              ECUIO_OK)
        << "code " << why.code << " at " << why.index;

    const auto run = [](bool speed_seen) {
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create("mock://t?profile=test-computed-stale",
                                       &session),
                  ECUIO_OK);
        ValuesCapture cap;
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_capture_cb, &cap),
            ECUIO_OK);
        const auto send = [&](uint32_t id, uint8_t byte, double seconds) {
            const ecuio_frame_t f =
                value_frame(id, {byte}, static_cast<uint64_t>(seconds * 1e6));
            ecuio_session_dispatch_callbacks(session, &f);
        };
        if (speed_seen) {
            send(0x124, 50, 0.0);
        }
        send(0x123, 1, 1.0); /* the speed 1 s old */
        send(0x123, 1, 5.0); /* 5 s old: past the stale age */
        send(0x123, 0, 6.0); /* in neutral, the speed not read */
        ecuio_session_destroy(session);
        return cap.batches;
    };

    const std::vector<SeenBatch> seen = run(true);
    ASSERT_EQ(seen.size(), 4u);
    ASSERT_NE(value_of(seen[1], 4), nullptr);
    EXPECT_EQ(value_of(seen[1], 4)->value, 50.0) << "in gear, the speed";
    ASSERT_NE(value_of(seen[1], 5), nullptr);
    EXPECT_EQ(value_of(seen[1], 5)->value, -49.0);
    EXPECT_EQ(value_of(seen[2], 4), nullptr) << "the branch taken is stale";
    EXPECT_EQ(value_of(seen[2], 5), nullptr) << "format 1, as before";
    ASSERT_NE(value_of(seen[3], 4), nullptr) << "the stale branch not taken";
    EXPECT_EQ(value_of(seen[3], 4)->value, 0.0);
    EXPECT_EQ(value_of(seen[3], 5), nullptr) << "format 1, as before";
    EXPECT_EQ(seen[3].values.size(), 2u) << "the gear and the pick";

    const std::vector<SeenBatch> never = run(false);
    ASSERT_EQ(never.size(), 3u);
    EXPECT_EQ(value_of(never[0], 4), nullptr) << "never seen, taken";
    EXPECT_EQ(value_of(never[0], 5), nullptr);
    ASSERT_NE(value_of(never[2], 4), nullptr) << "never seen, not taken";
    EXPECT_EQ(value_of(never[2], 4)->value, 0.0);
    EXPECT_EQ(value_of(never[2], 5), nullptr);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-stale"), ECUIO_OK);
}

/* Computing allocates nothing on the worker, as decoding doesn't
 * (CoreTest.WorkersAllocateNothingPerFrame's rule) */
TEST(CoreTest, ComputingValuesAllocatesNothing)
{
    static ComputedSet s;
    ASSERT_EQ(ecuio_profile_register("test-computed-new", &s.set, nullptr),
              ECUIO_OK);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(
        ecuio_session_create("mock://t?profile=test-computed-new", &session),
        ECUIO_OK);
    int batches = 0;
    ASSERT_EQ(ecuio_register_values_callback(session, values_counting_cb, &batches),
              ECUIO_OK);
    const ecuio_frame_t map = value_frame(0x123, {120, 2}, 1000000);
    const ecuio_frame_t baro = value_frame(0x124, {100}, 1100000);
    ecuio_session_dispatch_callbacks(session, &map);
    const unsigned long before = new_counter::on_this_thread();
    ecuio_session_dispatch_callbacks(session, &baro);
    ecuio_session_dispatch_callbacks(session, &map);
    EXPECT_EQ(new_counter::on_this_thread() - before, 0u);
    EXPECT_EQ(batches, 3);
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister("test-computed-new"), ECUIO_OK);
}


/* Fed through a leaf, a chain works out from its leaves and joins the
 * batch after the frame's values in the set's order (C3, C2, C1), each
 * def once, though worked out C1 first */
TEST(CoreTest, AChainJoinsTheBatchInTheSetsOrder)
{
    static ChainSet s;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-chain-order", &s.set, &why),
              ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    {
        ChainRun r("test-chain-order");
        EXPECT_EQ(r.send(2, 100, 0.0), std::vector<uint16_t>{2})
            << "L1 never seen: C1 and all above it have no value";
        EXPECT_EQ(r.send(3, 5, 0.0), std::vector<uint16_t>{3});
        EXPECT_EQ(r.send(1, 120, 0.1), (std::vector<uint16_t>{1, 13, 12, 11}));
        EXPECT_EQ(r.value(11), 20.0);
        EXPECT_EQ(r.value(12), 40.0);
        EXPECT_EQ(r.value(13), 45.0);
    }
    EXPECT_EQ(ecuio_profile_unregister("test-chain-order"), ECUIO_OK);
}

/* C3 fed by L3 alone works C2 and C1 out from the leaves at its frame:
 * C3 joins the batch, the two it read don't, since nothing fed them */
TEST(CoreTest, AChainFedThroughOneInputWorksOutTheOtherAtItsFrame)
{
    static ChainSet s;
    ASSERT_EQ(ecuio_profile_register("test-chain-fed", &s.set, nullptr),
              ECUIO_OK);
    {
        ChainRun r("test-chain-fed");
        r.send(2, 100, 0.0);
        r.send(1, 120, 0.1);
        EXPECT_EQ(r.send(3, 7, 0.5), (std::vector<uint16_t>{3, 13}));
        EXPECT_EQ(r.value(13), 47.0);
        r.send(1, 130, 0.6);
        EXPECT_EQ(r.send(3, 9, 0.7), (std::vector<uint16_t>{3, 13}));
        EXPECT_EQ(r.value(13), 69.0) << "(130 - 100) * 2 + 9, worked out at this frame";
    }
    EXPECT_EQ(ecuio_profile_unregister("test-chain-fed"), ECUIO_OK);
}

/* A leaf past the bound written beside it (C1's 1 s on L2) leaves C1 and
 * all above it without a value from that frame on, not later; a fresh
 * L2 brings them back */
TEST(CoreTest, AChainsLeafPastItsBoundLeavesNoValueAndNoLater)
{
    static ChainSet s;
    ASSERT_EQ(ecuio_profile_register("test-chain-age", &s.set, nullptr),
              ECUIO_OK);
    {
        ChainRun r("test-chain-age");
        r.send(2, 100, 0.0);
        r.send(1, 120, 0.0);
        EXPECT_EQ(r.send(3, 5, 1.0), (std::vector<uint16_t>{3, 13}))
            << "L2 exactly 1 s old: within its bound";
        EXPECT_EQ(r.value(13), 45.0);
        EXPECT_EQ(r.send(3, 5, 1.000001), std::vector<uint16_t>{3})
            << "L2 1 us past its bound";
        EXPECT_EQ(r.send(1, 120, 1.5), std::vector<uint16_t>{1});
        EXPECT_EQ(r.send(2, 90, 2.0), (std::vector<uint16_t>{2, 13, 12, 11}));
        EXPECT_EQ(r.value(11), 30.0);
        EXPECT_EQ(r.value(13), 65.0);
    }
    EXPECT_EQ(ecuio_profile_unregister("test-chain-age"), ECUIO_OK);
}

namespace {

/* A chain ECUIO_COMPUTED_MAX_DEPTH deep over one number (id 1, 0x123):
 * level k (id 100 + k) is the level below plus 1, listed top first */
struct DeepChain {
    ecuio_signal_def_t defs[1 + ECUIO_COMPUTED_MAX_DEPTH];
    ecuio_computed_t computed[ECUIO_COMPUTED_MAX_DEPTH];
    ecuio_signal_set_t set{};

    DeepChain()
    {
        defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123);
        defs[0].bit_length = 8;
        const uint32_t depth = ECUIO_COMPUTED_MAX_DEPTH;
        for (uint32_t k = 1; k <= depth; ++k) {
            const uint32_t at = depth - k; /* top first */
            ecuio_signal_def_t& d = defs[1 + at];
            d = value_def(static_cast<uint16_t>(100 + k), ECUIO_VALUE_UINT, 0);
            d.flags = ECUIO_DEF_FLAG_COMPUTED;
            d.table_first = static_cast<uint16_t>(at);
            ecuio_computed_t& c = computed[at];
            c = ecuio_computed_t{};
            c.input_ids[0] = k == 1 ? 1 : static_cast<uint16_t>(100 + k - 1);
            c.input_count = 1;
            c.constants[0] = 1.0;
            c.constant_count = 1;
            c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
            c.steps[1] = ECUIO_COMPUTED_PUSH_CONSTANT | 0u;
            c.steps[2] = ECUIO_COMPUTED_ADD;
            c.step_count = 3;
        }
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 1 + depth;
        set.defs = defs;
        set.computed_count = depth;
        set.computed = computed;
    }
};

} // namespace

/* The deepest chain: every level once, the top the leaf plus 8, and the
 * worker allocating nothing for it */
TEST(CoreTest, TheDeepestChainComputesAndAllocatesNothing)
{
    static DeepChain s;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("test-chain-deep", &s.set, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    {
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(
            ecuio_session_create("mock://t?profile=test-chain-deep", &session),
            ECUIO_OK);
        int batches = 0;
        ASSERT_EQ(ecuio_register_values_callback(session, values_counting_cb,
                                                 &batches),
                  ECUIO_OK);
        const ecuio_frame_t first = value_frame(0x123, {10}, 0);
        ecuio_session_dispatch_callbacks(session, &first);
        const unsigned long before = new_counter::on_this_thread();
        for (uint8_t k = 1; k <= 20; ++k) {
            const ecuio_frame_t f = value_frame(0x123, {k}, 50000u * k);
            ecuio_session_dispatch_callbacks(session, &f);
        }
        EXPECT_EQ(new_counter::on_this_thread() - before, 0u);
        EXPECT_EQ(batches, 21);
        ecuio_session_destroy(session);
    }
    {
        ChainRun r("test-chain-deep");
        r.send(1, 10, 0.0);
        const std::vector<uint16_t> ids = r.send(1, 50, 1.1);
        const std::vector<uint16_t> top_first = {1,   108, 107, 106, 105,
                                                 104, 103, 102, 101};
        EXPECT_EQ(ids, top_first);
        EXPECT_EQ(r.value(108), 58.0);
        EXPECT_EQ(r.value(101), 51.0);
    }
    EXPECT_EQ(ecuio_profile_unregister("test-chain-deep"), ECUIO_OK);
}

