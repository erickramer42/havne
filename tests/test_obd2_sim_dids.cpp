// File/layer: tests/test_obd2_sim_dids.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

std::vector<uint8_t> did_ask(const ecuio_signal_set_t& set, uint32_t rx,
                             std::initializer_list<uint8_t> req,
                             const std::vector<double>& values,
                             bool functional = false)
{
    const std::vector<uint8_t> q(req);
    uint8_t out[ECUIO_SIM_REPLY_MAX];
    const uint32_t n = ecuio_sim_did_answer(
        set, rx, q.data(), static_cast<uint32_t>(q.size()), functional,
        [&values](uint32_t i) { return values[i]; }, out, sizeof out);
    return std::vector<uint8_t>(out, out + n);
}

} // namespace

// Every field of one DID's reply, each kind and byte order, decodes
// through its own def to the value it was given: a big-endian word with
// an offset, a signed little-endian field at a bit offset, a binary32,
// a one-bit flag in a byte another field shares, and a text field its
// name
TEST(SimDidsTest, EachFieldDecodesToTheValueItWasGiven)
{
    const std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                16, 1.0, -40.0, "Fluid"),
        did_def(2, 0x7E9, 0x1234, ECUIO_VALUE_INT, ECUIO_BYTE_ORDER_LE, 5, 4,
                12, 0.5, 0.0, "Torque"),
        did_def(3, 0x7E9, 0x1234, ECUIO_VALUE_FLOAT, ECUIO_BYTE_ORDER_BE, 7, 0,
                32, 1.0, 0.0, "Ratio"),
        did_def(4, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_LE, 5, 0,
                1, 1.0, 0.0, "Lockup"),
        did_def(5, 0x7E9, 0x1235, ECUIO_VALUE_TEXT, ECUIO_BYTE_ORDER_LE, 3, 0,
                0, 1.0, 0.0, "TCM-Sim"),
    };
    const std::vector<ecuio_channel_decl_t> channels = {
        {0x7E9, 0x7E1, 0, 0, {}}};
    const ecuio_signal_set_t set = set_of(defs, channels);
    ASSERT_EQ(ecuio_validate_signal_set(&set, nullptr), 0)
        << "defs a session would take";
    const std::vector<double> values = {90.0, -100.5, 1.25, 1.0, 0.0};
    const std::vector<uint8_t> r =
        did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values);
    ASSERT_EQ(r.size(), 11u)
        << "62 12 34, then the fields to the binary32's end";
    EXPECT_EQ(r[0], 0x62);
    EXPECT_EQ(r[3], 0x00);
    EXPECT_EQ(r[4], 0x82) << "90 + 40 = 130, big-endian";
    for (size_t i = 0; i < 4; ++i) {
        double v = -1.0;
        ASSERT_EQ(ecuio_decode_signal(&defs[i], r.data(),
                                      static_cast<uint16_t>(r.size()), &v),
                  ECUIO_DECODE_OK)
            << defs[i].name;
        EXPECT_EQ(v, values[i]) << defs[i].name;
    }
    const std::vector<uint8_t> t =
        did_ask(set, 0x7E9, {0x22, 0x12, 0x35}, values);
    char text[ECUIO_DECODE_TEXT_MAX] = {};
    ASSERT_EQ(ecuio_decode_text(&defs[4], t.data(),
                                static_cast<uint16_t>(t.size()), text,
                                sizeof text),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(text, "TCM-Sim");
}

// The values a field holds and the ones it can't: a byte with an offset
// runs offset to offset + 255, a signed byte -128 to 127, a value rounds
// to the field's step, and NaN, a text def, or a zero scale off its
// offset hold none
TEST(SimDidsTest, AFieldsRangeIsWhatItAnswers)
{
    uint32_t raw = 0;
    const ecuio_signal_def_t u8 =
        did_def(1, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                1.0, -40.0, "u8");
    EXPECT_TRUE(ecuio_sim_raw_for(u8, -40.0, &raw));
    EXPECT_EQ(raw, 0u);
    EXPECT_TRUE(ecuio_sim_raw_for(u8, 215.0, &raw));
    EXPECT_EQ(raw, 255u);
    EXPECT_FALSE(ecuio_sim_raw_for(u8, 216.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(u8, -41.0, &raw));
    const ecuio_signal_def_t s8 =
        did_def(2, 0x7E8, 1, ECUIO_VALUE_INT, ECUIO_BYTE_ORDER_BE, 3, 0, 8, 1.0,
                0.0, "s8");
    EXPECT_TRUE(ecuio_sim_raw_for(s8, -128.0, &raw));
    EXPECT_EQ(raw, 0x80u);
    EXPECT_TRUE(ecuio_sim_raw_for(s8, 127.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(s8, 128.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(s8, -129.0, &raw));
    const ecuio_signal_def_t tenth =
        did_def(3, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 16,
                0.1, 0.0, "tenth");
    EXPECT_TRUE(ecuio_sim_raw_for(tenth, 1.04, &raw));
    EXPECT_EQ(raw, 10u) << "the nearest step";
    EXPECT_FALSE(ecuio_sim_raw_for(tenth, std::nan(""), &raw));
    const ecuio_signal_def_t text =
        did_def(4, 0x7E8, 1, ECUIO_VALUE_TEXT, ECUIO_BYTE_ORDER_LE, 3, 0, 0,
                1.0, 0.0, "text");
    EXPECT_FALSE(ecuio_sim_raw_for(text, 0.0, &raw));
    const ecuio_signal_def_t flat =
        did_def(5, 0x7E8, 1, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0, 8,
                0.0, 7.0, "flat");
    EXPECT_TRUE(ecuio_sim_raw_for(flat, 7.0, &raw));
    EXPECT_FALSE(ecuio_sim_raw_for(flat, 8.0, &raw));
}

// What isn't the set's: a DID none of its defs reads, one on another
// module, a list of DIDs, another service; a functional request's
// refusals unsaid, its answer said
TEST(SimDidsTest, WhatTheSetDoesntReadIsRefused)
{
    const std::vector<ecuio_signal_def_t> defs = {
        did_def(1, 0x7E9, 0x1234, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                8, 1.0, 0.0, "a"),
        did_def(2, 0x7EA, 0x5678, ECUIO_VALUE_UINT, ECUIO_BYTE_ORDER_BE, 3, 0,
                8, 1.0, 0.0, "b"),
    };
    const ecuio_signal_set_t set = set_of(defs);
    const std::vector<double> values = {7.0, 9.0};
    const std::vector<uint8_t> no = {0x7F, 0x22, 0x31};
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values),
              (std::vector<uint8_t>{0x62, 0x12, 0x34, 0x07}));
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0xF1, 0x90}, values), no);
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x56, 0x78}, values), no)
        << "0x7EA's DID";
    EXPECT_EQ(did_ask(set, 0x7EA, {0x22, 0x56, 0x78}, values),
              (std::vector<uint8_t>{0x62, 0x56, 0x78, 0x09}));
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34, 0x56, 0x78}, values), no);
    EXPECT_EQ(did_ask(set, 0x7E9, {0x01, 0x0C}, values),
              (std::vector<uint8_t>{0x7F, 0x01, 0x11}));
    EXPECT_TRUE(did_ask(set, 0x7E9, {0x22, 0xF1, 0x90}, values, true).empty());
    EXPECT_TRUE(did_ask(set, 0x7E9, {0x01, 0x0C}, values, true).empty());
    EXPECT_EQ(did_ask(set, 0x7E9, {0x22, 0x12, 0x34}, values, true).size(), 4u);
    EXPECT_TRUE(did_ask(set, 0x7E9, {}, values).empty());
}

// A set's channels are the simulated car's modules: the engine ECU's
// pair and none at all both put the DIDs on the engine ECU, every other
// channel is a module; a 29-bit ID, the functional ID, or half of the
// engine ECU's pair is refused, why naming the channel
TEST(SimDidsTest, TheModulesAreTheSetsChannels)
{
    ecuio_sim_module_plan plan;
    char why[256] = {};
    const ecuio_signal_set_t none = set_of({});
    ASSERT_TRUE(ecuio_sim_plan_modules(none, &plan, why, sizeof why));
    EXPECT_TRUE(plan.engine);
    EXPECT_EQ(plan.count, 0u);
    const std::vector<ecuio_channel_decl_t> two = {{0x7E8, 0x7E0, 0, 0, {}},
                                                   {0x7EA, 0x7E2, 0, 0, {}}};
    ASSERT_TRUE(
        ecuio_sim_plan_modules(set_of({}, two), &plan, why, sizeof why));
    EXPECT_TRUE(plan.engine);
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.tx[0], 0x7E2u);
    EXPECT_EQ(plan.rx[0], 0x7EAu);
    const std::vector<ecuio_channel_decl_t> module = {{0x7E9, 0x7E1, 0, 0, {}}};
    ASSERT_TRUE(
        ecuio_sim_plan_modules(set_of({}, module), &plan, why, sizeof why));
    EXPECT_FALSE(plan.engine) << "the engine ECU answers no DID of the set";
    EXPECT_EQ(plan.count, 1u);
    for (const std::vector<ecuio_channel_decl_t>& bad :
         {std::vector<ecuio_channel_decl_t>{{0x18DAF110, 0x18DA10F1, 0, 0, {}}},
          std::vector<ecuio_channel_decl_t>{{0x7E9, 0x7DF, 0, 0, {}}},
          std::vector<ecuio_channel_decl_t>{{0x7EA, 0x7E0, 0, 0, {}}},
          std::vector<ecuio_channel_decl_t>{{0x7E8, 0x7E2, 0, 0, {}}}}) {
        why[0] = '\0';
        EXPECT_FALSE(
            ecuio_sim_plan_modules(set_of({}, bad), &plan, why, sizeof why))
            << std::hex << bad[0].rx_id;
        EXPECT_NE(std::strstr(why, "channel 0"), nullptr) << why;
    }
}

// profile= names a registered profile by the registry's grammar, on
// mock:// alone, and never beside modules=2
TEST(SimDidsTest, TheUrisProfile)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?profile=user.daily-extras_2", &c,
                                  why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(std::string(c.profile, c.profile_len), "user.daily-extras_2");
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_EQ(c.profile_len, 0u);
#ifdef _WIN32
    const std::string replay = "replay://C:/recordings/run?profile=obd2";
#else
    const std::string replay = "replay:///recordings/run?profile=obd2";
#endif
    for (const std::string& bad :
         {std::string("mock://sim?profile="),
          std::string("mock://sim?profile=Obd2"),
          std::string("mock://sim?profile=a/b"),
          std::string("mock://sim?profile=") + std::string(64, 'a'),
          std::string("mock://sim?profile=obd2&modules=2"), replay}) {
        why[0] = '\0';
        EXPECT_EQ(ecuio_sim_parse_uri(bad.c_str(), &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_NE(why[0], '\0') << bad;
    }
}

namespace {

/* One recorded frame: the engine ECU's unless id says otherwise */
void take(ecuio_sim_recording& r, int64_t at_us,
          std::initializer_list<uint8_t> data, uint32_t id = 0x7E8,
          uint32_t flags = 0)
{
    std::unique_ptr<ecuio_frame_t> f(new ecuio_frame_t());
    f->arbitration_id = id;
    f->flags = flags;
    f->data_length = static_cast<uint16_t>(data.size());
    std::copy(data.begin(), data.end(), f->data);
    ecuio_sim_recording_take(r, at_us, *f);
}

std::vector<uint8_t> rask(const ecuio_sim_recording& r, int64_t now_us,
                          std::initializer_list<uint8_t> req,
                          const char* vin = nullptr)
{
    const std::vector<uint8_t> q(req);
    std::vector<uint8_t> out(ECUIO_SIM_RECORDING_REPLY_MAX);
    const uint32_t n = ecuio_sim_recording_answer(
        r, now_us, q.data(), static_cast<uint32_t>(q.size()), vin, out.data());
    out.resize(n);
    return out;
}

/* A scratch frames.csv in TEMP/TMPDIR, removed when it goes */
struct ScratchCsv {
    std::string path;
    explicit ScratchCsv(const char* name, const std::string& text)
    {
        const char* dir = std::getenv("TEMP");
        if (dir == nullptr) {
            dir = std::getenv("TMPDIR");
        }
        path = std::string(dir != nullptr ? dir : "/tmp") + "/" +
               temp_name(std::string("ecuio-") + name) + ".csv";
        FILE* f = std::fopen(path.c_str(), "wb");
        EXPECT_NE(f, nullptr) << path;
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ~ScratchCsv() { std::remove(path.c_str()); }
};

} // namespace

// Each kept reply answers the request it came for; a refusal, a clear's
// 44, another ECU's reply, an error, a 29-bit or a cut-short frame isn't
// kept
TEST(SimRecordingTest, KeepsEachReplyUnderTheRequestItAnswers)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8});
    take(r, 0, {0x42, 0x0C, 0x00, 0x2E, 0xE0});
    take(r, 0, {0x43, 0x01, 0x03, 0x01});
    take(r, 0, {0x47, 0x00});
    take(r, 0, {0x4A, 0x00});
    take(r, 0, {0x49, 0x0A, 0x01, 'E', 'C', 'M'});
    take(r, 0, {0x62, 0x1E, 0x05, 0x55});
    take(r, 0, {0x7F, 0x22, 0x31});
    take(r, 0, {0x44});
    take(r, 0, {0x41, 0x0D, 0x32}, 0x7E9);
    take(r, 0, {0x41, 0x05, 0x7B}, 0x7E8, ECUIO_FRAME_FLAG_ERROR);
    take(r, 0, {0x41, 0x0F, 0x50}, 0x7E8, ECUIO_FRAME_FLAG_EXTENDED);
    take(r, 0, {0x41, 0x10, 0x01}, 0x7E8, ECUIO_FRAME_FLAG_TRUNCATED);
    EXPECT_EQ(r.replies.size(), 7u);
    EXPECT_EQ(rask(r, 0, {0x01, 0x0C}), (Bytes{0x41, 0x0C, 0x1A, 0xF8}));
    EXPECT_EQ(rask(r, 0, {0x02, 0x0C, 0x00}),
              (Bytes{0x42, 0x0C, 0x00, 0x2E, 0xE0}));
    EXPECT_EQ(rask(r, 0, {0x03}), (Bytes{0x43, 0x01, 0x03, 0x01}));
    EXPECT_EQ(rask(r, 0, {0x07}), (Bytes{0x47, 0x00}));
    EXPECT_EQ(rask(r, 0, {0x0A}), (Bytes{0x4A, 0x00}));
    EXPECT_EQ(rask(r, 0, {0x09, 0x0A}),
              (Bytes{0x49, 0x0A, 0x01, 'E', 'C', 'M'}));
    EXPECT_EQ(rask(r, 0, {0x22, 0x1E, 0x05}), (Bytes{0x62, 0x1E, 0x05, 0x55}));
    for (uint8_t pid : std::initializer_list<uint8_t>{0x0D, 0x05, 0x0F, 0x10}) {
        EXPECT_EQ(rask(r, 0, {0x01, pid}), (Bytes{0x7F, 0x01, 0x12}))
            << "not kept: " << int(pid);
    }
}

// The reply at the ECU's time: the first before it, the last at or
// before it between, the last after the end (held), or round the span
TEST(SimRecordingTest, AnswersAtTheEcusTime)
{
    ecuio_sim_recording r;
    take(r, 1 * kSecond, {0x41, 0x0D, 10});
    take(r, 3 * kSecond, {0x41, 0x0D, 30});
    take(r, 5 * kSecond, {0x41, 0x0D, 50});
    EXPECT_EQ(r.span_us, 5 * kSecond);
    const auto speed = [&](int64_t at) { return rask(r, at, {0x01, 0x0D})[2]; };
    EXPECT_EQ(speed(0), 10);
    EXPECT_EQ(speed(2 * kSecond), 10);
    EXPECT_EQ(speed(3 * kSecond), 30);
    EXPECT_EQ(speed(5 * kSecond - 1), 30);
    EXPECT_EQ(speed(5 * kSecond), 50);
    EXPECT_EQ(speed(60 * kSecond), 50) << "held after the end";
    r.loop = true;
    EXPECT_EQ(speed(6 * kSecond), 10) << "6 s is 1 s into the second lap";
    EXPECT_EQ(speed(8 * kSecond), 30);
    EXPECT_EQ(speed(14 * kSecond), 30);
}

// A packed reply (ADR-0039 Appendix A) is split into its PIDs, and a pack
// asked is put together from each PID's reply now
TEST(SimRecordingTest, PacksAreSplitAndPutTogether)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8, 0x0D, 0x32, 0x05, 0x7B});
    take(r, 2 * kSecond, {0x41, 0x0D, 0x40});
    EXPECT_EQ(rask(r, 0, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x32}));
    EXPECT_EQ(rask(r, 0, {0x01, 0x05}), (Bytes{0x41, 0x05, 0x7B}));
    EXPECT_EQ(rask(r, 3 * kSecond, {0x01, 0x05, 0x0D, 0x0C}),
              (Bytes{0x41, 0x05, 0x7B, 0x0D, 0x40, 0x0C, 0x1A, 0xF8}))
        << "in the order asked, each PID's newest";
    EXPECT_EQ(rask(r, 0, {0x01, 0x0C, 0x11}), (Bytes{0x41, 0x0C, 0x1A, 0xF8}))
        << "a PID it doesn't hold left out";
    EXPECT_TRUE(rask(r, 0, {0x01, 0x11, 0x0F}).empty()) << "none: silence";
}

// The car's own bitmaps when the recording holds any (the rest refused,
// as that car's chain is what it is); worked out from the PIDs it holds
// when it holds none
TEST(SimRecordingTest, TheBitmapsAreTheCarsOrWorkedOut)
{
    ecuio_sim_recording car;
    take(car, 0, {0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11});
    take(car, 0, {0x41, 0x0C, 0x1A, 0xF8});
    take(car, 0, {0x41, 0x2F, 0x80}); /* a PID past 0x20, its bitmap unsent */
    EXPECT_TRUE(car.bitmaps);
    EXPECT_EQ(rask(car, 0, {0x01, 0x00}),
              (Bytes{0x41, 0x00, 0xBE, 0x3E, 0xB8, 0x11}));
    EXPECT_EQ(rask(car, 0, {0x01, 0x20}), (Bytes{0x7F, 0x01, 0x12}))
        << "the car's chain, not one worked out";

    ecuio_sim_recording none;
    for (uint8_t pid :
         std::initializer_list<uint8_t>{0x05, 0x0C, 0x0D, 0x2F, 0x42}) {
        take(none, 0, {0x41, pid, 0x00, 0x00});
    }
    EXPECT_FALSE(none.bitmaps);
    // 05 is bit 4, 0C and 0D bits 11 and 12, 20 bit 31 (data past it)
    EXPECT_EQ(rask(none, 0, {0x01, 0x00}),
              (Bytes{0x41, 0x00, 0x08, 0x18, 0x00, 0x01}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x20}),
              (Bytes{0x41, 0x20, 0x00, 0x02, 0x00, 0x01}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x40}),
              (Bytes{0x41, 0x40, 0x40, 0x00, 0x00, 0x00}));
    EXPECT_EQ(rask(none, 0, {0x01, 0x60}), (Bytes{0x7F, 0x01, 0x12}))
        << "nothing past 0x40";
}

// A VIN given answers 09 02 instead of the recording's; what it doesn't
// hold is refused, a clear always
TEST(SimRecordingTest, TheVinGivenAndTheRefusals)
{
    ecuio_sim_recording r;
    take(r, 0, {0x49, 0x02, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'});
    take(r, 0, {0x41, 0x0C, 0x1A, 0xF8});
    EXPECT_EQ(rask(r, 0, {0x09, 0x02})[3], 'Z') << "the recording's";
    const Bytes given = rask(r, 0, {0x09, 0x02}, "1FAFAKE10M0000001");
    ASSERT_EQ(given.size(), 20u);
    EXPECT_EQ(std::string(given.begin() + 3, given.end()), "1FAFAKE10M0000001");
    EXPECT_EQ(rask(r, 0, {0x09, 0x04}), (Bytes{0x7F, 0x09, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x01, 0x05}), (Bytes{0x7F, 0x01, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x03}), (Bytes{0x7F, 0x03, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x02, 0x0C, 0x00}), (Bytes{0x7F, 0x02, 0x12}));
    EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x31}));
    EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90, 0xF1, 0x8C}),
              (Bytes{0x7F, 0x22, 0x13}));
    EXPECT_EQ(rask(r, 0, {0x04}), (Bytes{0x7F, 0x04, 0x22}));
    EXPECT_EQ(rask(r, 0, {0x10, 0x03}), (Bytes{0x7F, 0x10, 0x11}));
}

// Through the simulated ECU: its ticks are the recording's time, its
// functional refusals kept to itself, its VIN given or not
TEST(SimRecordingTest, TheEcuAnswersFromItsRecording)
{
    ecuio_sim_recording r;
    take(r, 0, {0x41, 0x0D, 10});
    take(r, 3 * kSecond, {0x41, 0x0D, 30});
    ecuio_sim_ecu e;
    e.recording = &r;
    EXPECT_EQ(ask(e, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 10}));
    for (int i = 0; i < 60; ++i) { /* 3 s of 50 ms ticks */
        ecuio_sim_tick(e);
    }
    EXPECT_EQ(e.time_us, 3 * kSecond);
    EXPECT_EQ(ask(e, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 30}));
    EXPECT_EQ(ask(e, {0x01, 0x05}), (Bytes{0x7F, 0x01, 0x12}));
    EXPECT_TRUE(ask(e, {0x01, 0x05}, true).empty()) << "functional: silence";
    EXPECT_EQ(ask(e, {0x09, 0x02}), (Bytes{0x7F, 0x09, 0x12}));
    std::memcpy(e.config.vin, "1FAFAKE10M0000001", 18);
    e.config.vin_given = true;
    EXPECT_EQ(ask(e, {0x09, 0x02}).size(), 20u);
    EXPECT_EQ(ask(e, {0x04}), (Bytes{0x7F, 0x04, 0x22})) << "never clears";
}

// The committed fixture (the test car's engine start): the car's
// bitmaps, its PIDs over the recording, its refusals not kept; the
// folder or the file
TEST(SimRecordingTest, TheLoaderReadsTheFixture)
{
    const std::string dir =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    for (const std::string& path : {dir, dir + "/frames.csv"}) {
        ecuio_sim_recording r;
        ASSERT_EQ(ecuio_sim_load_recording(path, &r), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_TRUE(r.bitmaps);
        EXPECT_EQ(rask(r, 0, {0x01, 0x00}),
                  (Bytes{0x41, 0x00, 0xFE, 0x7F, 0xA8, 0x13}))
            << "the fixture's first row";
        EXPECT_GT(r.span_us, 10 * kSecond);
        const Bytes first = rask(r, 0, {0x01, 0x0C});
        const Bytes last = rask(r, r.span_us, {0x01, 0x0C});
        ASSERT_EQ(first.size(), 4u);
        ASSERT_EQ(last.size(), 4u);
        EXPECT_NE(first, last) << "the engine speed moves over the start";
        EXPECT_EQ(rask(r, 0, {0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x31}))
            << "its refusals aren't replies";
        EXPECT_EQ(r.replies.size(), 15u) << "9 data PIDs, 6 bitmaps";
    }
}

// What the loader refuses, by the line, never naming the path
// A last row with no newline is a write cut short (ADR-0049 section 7):
// left out, whether or not it would parse
TEST(SimRecordingTest, ATornLastRowIsLeftOut)
{
    ScratchCsv csv("sim-torn", "host_us,timestamp_us,id,flags,data\n"
                               "1,1,0x7E8,0x0,41 0C 1A F8\n"
                               "2,2,0x7E8,0x0,41 0D 20");
    ecuio_sim_recording r;
    ASSERT_EQ(ecuio_sim_load_recording(csv.path, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(r.replies.size(), 1u) << "only 41 0C's reply";
}

// A stretch (from and to, by host_us) answers from its own rows, its
// first at the ECU's start: before it, after it, or a stretch of a reply
// the engine ECU never sent, nothing of it is kept
TEST(SimRecordingTest, AStretchAnswersFromItsOwnRows)
{
    ScratchCsv csv("sim-stretch", "host_us,timestamp_us,id,flags,data\n"
                                  "0,500,0x7E8,0x0,41 0D 0A\n"
                                  "1000000,1000500,0x7E8,0x0,41 0D 14\n"
                                  "2000000,1500500,0x7E8,0x0,41 0D 1E\n"
                                  "3000000,2000500,0x7E9,0x0,41 0D 28\n"
                                  "4000000,2500500,0x7E8,0x0,41 0D 32\n");
    ecuio_stretch::stretch s;
    s.from_us = 900000;
    s.to_us = 2000000;
    ecuio_sim_recording r;
    ASSERT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(rask(r, 0, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x14}))
        << "the stretch's first row at 0";
    EXPECT_EQ(rask(r, 499999, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x14}));
    EXPECT_EQ(rask(r, 500000, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x1E}))
        << "its next by the session time between them";
    EXPECT_EQ(r.span_us, 500000);
    EXPECT_EQ(rask(r, 10 * kSecond, {0x01, 0x0D}), (Bytes{0x41, 0x0D, 0x1E}))
        << "held past its end: row 5 isn't in it";

    s.from_us = 2500000;
    s.to_us = 3500000;
    EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_ERR_LOAD_FAILED)
        << "0x7E9's row alone";
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the stretch holds no reply from the engine ECU (0x7E8) to "
                 "answer with");
    s.from_us = 5000000;
    s.to_us = ecuio_stretch::kOpen;
    EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r, s), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the recording holds no frames from 5 s on: its frames run "
                 "from 0 s to 4 s");
}

TEST(SimRecordingTest, TheLoadersRefusals)
{
    const std::string header = "host_us,timestamp_us,id,flags,data\n";
    struct Case {
        const char* name;
        std::string text;
        const char* says;
    };
    const Case cases[] = {
        {"sim-not-frames", "time,value\n1,2\n", "header row"},
        {"sim-bad-row", header + "1,1,0x7E8,0x0,41 0C 1A F8\n2,2,7E8,0x0,41\n",
         "frames.csv line 3"},
        {"sim-no-ecu", header + "1,1,0x7E9,0x0,41 0C 1A F8\n",
         "no reply from the engine ECU"},
        {"sim-empty", "", "empty"},
    };
    for (const Case& c : cases) {
        ScratchCsv csv(c.name, c.text);
        ecuio_sim_recording r;
        EXPECT_EQ(ecuio_sim_load_recording(csv.path, &r), ECUIO_ERR_LOAD_FAILED)
            << c.name;
        const std::string detail = ecuio_last_error_detail();
        EXPECT_NE(detail.find(c.says), std::string::npos) << detail;
        EXPECT_EQ(detail.find("ecuio-"), std::string::npos) << detail;
    }
    ecuio_sim_recording r;
    EXPECT_EQ(ecuio_sim_load_recording(std::string(ECUIO_REPLAY_FIXTURES) +
                                           "/no-such-recording",
                                       &r),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("couldn't open"),
              std::string::npos);
}
