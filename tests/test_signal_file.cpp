// File/layer: tests/test_signal_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, the signal-file loader (ADR-0051,
//          core/src/signal_file.cpp): the ADR's example into its set, each
//          field's default, the expression compiler and obd2's keys, every
//          refusal with its line, column and words

#include <gtest/gtest.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include "computed.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

File load(const std::string& json, ecuio_status_t* status = nullptr)
{
    ecuio_signal_file_t* f = reinterpret_cast<ecuio_signal_file_t*>(1);
    const ecuio_status_t s = ecuio_signal_file_load(json.data(), json.size(), &f);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(f, nullptr) << "out is NULL on a failure";
    }
    return File(f);
}

/* The detail a refused file leaves */
std::string refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    File f = load(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT) << json;
    return ecuio_last_error_detail();
}

/* ADR-0051 section 2's example */
const char* const kExample = R"json({
  "havne_signals": 1,
  "name": "Daily driver extras",
  "notes": "TFT checked against the gauge",
  "source": { "name": "Written by hand for the daily driver" },
  "extends": "obd2",
  "ecus": [
    { "id": "pcm", "rx": "7E8" },
    { "id": "tcm", "rx": "7E9" }
  ],
  "signals": [
    {
      "key": "tft",
      "name": "Trans Fluid Temp",
      "ecu": "tcm",
      "read": { "service": "22", "number": "1E1C" },
      "field": { "byte": 0, "bits": 16, "order": "be", "signed": false },
      "scale": 1, "offset": -40,
      "unit": "°C", "decimals": 0, "group": "engine"
    },
    {
      "key": "fuel_status",
      "name": "Fuel System 1",
      "ecu": "pcm",
      "read": { "service": "01", "number": "03" },
      "field": { "byte": 0, "bits": 8 },
      "kind": "enum",
      "values": { "1": "Open loop, cold", "2": "Closed loop", "4": "Open loop, load" },
      "group": "fuel"
    },
    {
      "key": "cal_id",
      "name": "TCM Calibration",
      "ecu": "tcm",
      "read": { "service": "22", "number": "F188" },
      "field": { "byte": 0 },
      "kind": "text",
      "group": "diagnostics"
    },
    {
      "key": "boost",
      "name": "Boost",
      "kind": "computed",
      "expr": "obd2.map - obd2.baro",
      "max_age": { "obd2.baro": 30 },
      "unit": "kPa", "decimals": 0, "group": "air", "plot": 1
    }
  ]
})json";

/* A minimal file around one signal's JSON, on one ECU */
std::string one(const std::string& signal, const std::string& ecus =
                                               R"([{"id":"pcm","rx":"7E8"}])")
{
    return std::string(R"({"havne_signals":1,"name":"T","source":{"name":"t"},)") +
           R"("ecus":)" + ecus + R"(,"signals":[)" + signal + "]}";
}

const char* const kNumber =
    R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0}})";

} // namespace

TEST(SignalFileTest, TheAdrsExampleLoadsIntoItsSet)
{
    File f = load(kExample);
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_STREQ(ecuio_signal_file_name(f.get()), "Daily driver extras");
    EXPECT_STREQ(ecuio_signal_file_profile(f.get()), "user.daily-driver-extras");
    EXPECT_STREQ(ecuio_signal_file_extends(f.get()), "obd2");
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    ASSERT_EQ(set->count, obd2->count + 4) << "the base's, then the file's";
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(set->defs[0].id, obd2->defs[0].id);

    /* pcm is the base's 7E8; tcm is a channel of its own, tx by the rule */
    ASSERT_EQ(set->channel_count, 2u);
    EXPECT_EQ(set->channels[0].rx_id, 0x7E8u);
    EXPECT_EQ(set->channels[1].rx_id, 0x7E9u);
    EXPECT_EQ(set->channels[1].tx_id, 0x7E1u);

    const ecuio_signal_def_t* tft = ecuio_signal_file_find(f.get(), "tft");
    ASSERT_NE(tft, nullptr);
    EXPECT_EQ(tft->id, ECUIO_USER_SIG_ID_BASE + 0);
    EXPECT_EQ(tft->message_id, 0x7E9u);
    EXPECT_EQ(tft->mux_length, 3);
    EXPECT_EQ(tft->mux_key, 0x621E1Cu);
    EXPECT_EQ(tft->start_byte, 3) << "byte 0 after 62 1E 1C";
    EXPECT_EQ(tft->bit_length, 16);
    EXPECT_EQ(tft->byte_order, ECUIO_BYTE_ORDER_BE);
    EXPECT_EQ(tft->kind, ECUIO_VALUE_UINT);
    EXPECT_EQ(tft->offset, -40.0);
    EXPECT_STREQ(tft->unit, "\xC2\xB0" "C");
    EXPECT_EQ(tft->group, ECUIO_SIGNAL_GROUP_ENGINE);
    const uint8_t reply[] = {0x62, 0x1E, 0x1C, 0x00, 0x5A};
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(tft, reply, sizeof reply, &v), ECUIO_DECODE_OK);
    EXPECT_EQ(v, 50.0);

    const ecuio_signal_def_t* fuel = ecuio_signal_file_find(f.get(), "fuel_status");
    ASSERT_NE(fuel, nullptr);
    EXPECT_EQ(fuel->kind, ECUIO_VALUE_ENUM);
    EXPECT_EQ(fuel->message_id, 0x7E8u);
    EXPECT_EQ(fuel->mux_key, 0x4103u);
    EXPECT_EQ(fuel->start_byte, 2);
    EXPECT_EQ(fuel->table_count, 3);
    EXPECT_STREQ(ecuio_enum_label(set, fuel, 2), "Closed loop");
    EXPECT_STREQ(ecuio_enum_label(set, fuel, 4), "Open loop, load");
    EXPECT_EQ(ecuio_enum_label(set, fuel, 3), nullptr);

    const ecuio_signal_def_t* cal = ecuio_signal_file_find(f.get(), "cal_id");
    ASSERT_NE(cal, nullptr);
    EXPECT_EQ(cal->kind, ECUIO_VALUE_TEXT);
    EXPECT_EQ(cal->byte_length, 0) << "to the end";
    EXPECT_EQ(cal->bit_length, 0);

    const ecuio_signal_def_t* boost = ecuio_signal_file_find(f.get(), "boost");
    ASSERT_NE(boost, nullptr);
    EXPECT_EQ(boost->flags, ECUIO_DEF_FLAG_COMPUTED);
    EXPECT_EQ(boost->plot_rank, 1);
    ASSERT_EQ(set->computed_count, 1u);
    const ecuio_computed_t& p = set->computed[boost->table_first];
    ASSERT_EQ(p.input_count, 2);
    EXPECT_EQ(p.input_ids[0], ECUIO_OBD2_PID_SIG_ID(0x0B, 0)) << "obd2.map";
    EXPECT_EQ(p.input_ids[1], ECUIO_OBD2_PID_SIG_ID(0x33, 0)) << "obd2.baro";
    EXPECT_EQ(p.max_age_us[0], 0u) << "the stale age";
    EXPECT_EQ(p.max_age_us[1], 30000000u);
    const double in[4] = {140.0, 101.0, 0.0, 0.0};
    ASSERT_TRUE(ecuio_computed_run(p, in, &v));
    EXPECT_EQ(v, 39.0);

    EXPECT_EQ(ecuio_signal_file_find(f.get(), "missing"), nullptr);
    EXPECT_EQ(ecuio_signal_file_find(f.get(), nullptr), nullptr);

    /* It registers as its profile, and sessions find it there */
    ASSERT_EQ(ecuio_profile_register(ecuio_signal_file_profile(f.get()), set, nullptr),
              ECUIO_OK);
    const ecuio_signal_set_t* found = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("user.daily-driver-extras", &found), ECUIO_OK);
    EXPECT_EQ(found, set);
    EXPECT_EQ(ecuio_profile_unregister("user.daily-driver-extras"), ECUIO_OK);
}

TEST(SignalFileTest, AFieldTakesItsDefaults)
{
    File f = load(one(kNumber));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_def_t* d = ecuio_signal_file_find(f.get(), "a");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, ECUIO_VALUE_UINT) << "number, unsigned";
    EXPECT_EQ(d->bit_length, 8);
    EXPECT_EQ(d->byte_order, ECUIO_BYTE_ORDER_BE);
    EXPECT_EQ(d->scale, 1.0);
    EXPECT_EQ(d->offset, 0.0);
    EXPECT_EQ(d->decimal_places, 0);
    EXPECT_EQ(d->group, ECUIO_SIGNAL_GROUP_OTHER);
    EXPECT_EQ(d->plot_rank, 0);
    EXPECT_STREQ(d->unit, "");
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    ASSERT_EQ(set->count, 1u) << "no base without extends";
    ASSERT_EQ(set->channel_count, 1u);
    EXPECT_EQ(set->channels[0].tx_id, 0x7E0u);
    EXPECT_STREQ(ecuio_signal_file_profile(f.get()), "user.t");
    EXPECT_STREQ(ecuio_signal_file_extends(f.get()), "") << "extends none";
    EXPECT_STREQ(ecuio_signal_file_extends(nullptr), "");

    /* A signed little-endian bit field, scaled */
    File g = load(one(R"({"key":"b","name":"B","read":{"service":"01","number":"5C"},)"
                      R"("field":{"byte":1,"bits":4,"bit":2,"order":"le","signed":true},)"
                      R"("scale":0.5,"offset":1,"decimals":1,"group":"oxygen sensors","plot":3})"));
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_def_t* b = ecuio_signal_file_find(g.get(), "b");
    EXPECT_EQ(b->kind, ECUIO_VALUE_INT);
    EXPECT_EQ(b->start_byte, 3) << "byte 1 after 41 5C";
    EXPECT_EQ(b->bit_offset, 2);
    EXPECT_EQ(b->bit_length, 4);
    EXPECT_EQ(b->byte_order, ECUIO_BYTE_ORDER_LE);
    EXPECT_EQ(b->group, ECUIO_SIGNAL_GROUP_OXYGEN);
    EXPECT_EQ(b->plot_rank, 3);
    const uint8_t reply[] = {0x41, 0x5C, 0x00, 0x3C};   /* bits 2-5: 1111 = -1 */
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(b, reply, sizeof reply, &v), ECUIO_DECODE_OK);
    EXPECT_EQ(v, 0.5) << "-1 x 0.5 + 1";
}

TEST(SignalFileTest, AnEnumsValuesMayComeInAnyOrder)
{
    /* The file's order is the writer's; the table is ascending */
    File f = load(one(R"({"key":"m","name":"Mode","kind":"enum","read":{"service":"01","number":"03"},)"
                      R"("field":{"byte":0},"values":{"8":"Eight","2":"Two","16":"Sixteen"}})"));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_def_t* m = ecuio_signal_file_find(f.get(), "m");
    ASSERT_EQ(m->table_count, 3);
    EXPECT_EQ(set->enums[m->table_first].raw, 2u);
    EXPECT_STREQ(ecuio_enum_label(set, m, 16), "Sixteen");
    EXPECT_STREQ(ecuio_enum_label(set, m, 2), "Two");
    EXPECT_STREQ(ecuio_enum_label(set, m, 8), "Eight");
}

TEST(SignalFileTest, AProfileNameComesFromTheName)
{
    const auto profile = [](const std::string& name) {
        File f = load(R"({"havne_signals":1,"name":")" + name +
                      R"(","source":{"name":"t"},"ecus":[{"id":"e","rx":"7E8"}],"signals":[)" +
                      kNumber + "]}");
        return std::string(f ? ecuio_signal_file_profile(f.get()) : "refused");
    };
    EXPECT_EQ(profile("Daily driver extras"), "user.daily-driver-extras");
    EXPECT_EQ(profile("  TFT & boost!! "), "user.tft-boost");
    EXPECT_EQ(profile("v2.1"), "user.v2-1");
    EXPECT_EQ(profile("--"), "refused") << "no letter or digit";
    EXPECT_EQ(profile(std::string(58, 'a')), "user." + std::string(58, 'a'));
    EXPECT_EQ(profile(std::string(59, 'a')), "refused") << "past 63";
}

TEST(SignalFileTest, AnExpressionCompilesWithPrecedence)
{
    const auto run = [](const std::string& expr, double a, double b, double* out) {
        File f = load(one(std::string(kNumber) +
                          R"(,{"key":"b","name":"B","read":{"service":"22","number":"D101"},"field":{"byte":0}})"
                          R"(,{"key":"c","name":"C","kind":"computed","expr":")" +
                          expr + "\"}"));
        if (!f) {
            return std::string(ecuio_last_error_detail());
        }
        const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
        const ecuio_computed_t& p = set->computed[0];
        double in[4] = {};
        for (uint32_t k = 0; k < p.input_count; ++k) {
            in[k] = p.input_ids[k] == ECUIO_USER_SIG_ID_BASE ? a : b;
        }
        return ecuio_computed_run(p, in, out) ? std::string("ok") : std::string("none");
    };
    double v = 0.0;
    ASSERT_EQ(run("a + b * 2", 1, 3, &v), "ok");
    EXPECT_EQ(v, 7.0);
    ASSERT_EQ(run("(a + b) * 2", 1, 3, &v), "ok");
    EXPECT_EQ(v, 8.0);
    ASSERT_EQ(run("a - b - 1", 10, 3, &v), "ok");
    EXPECT_EQ(v, 6.0) << "left to right";
    ASSERT_EQ(run("a / b / 2", 12, 3, &v), "ok");
    EXPECT_EQ(v, 2.0);
    ASSERT_EQ(run("-a + 0.25", 1, 0, &v), "ok");
    EXPECT_EQ(v, -0.75);
    ASSERT_EQ(run("-(a - b)", 1, 3, &v), "ok");
    EXPECT_EQ(v, 2.0);
    ASSERT_EQ(run("a * a * a", 2, 0, &v), "ok");
    EXPECT_EQ(v, 8.0) << "one input read three times";
    EXPECT_EQ(run("a / b", 1, 0, &v), "none") << "a division by zero";

    const auto refused = [&](const std::string& expr, const char* words) {
        double unused = 0.0;
        const std::string got = run(expr, 0, 0, &unused);
        EXPECT_NE(got.find(words), std::string::npos) << expr << ": " << got;
    };
    refused("a +", "ends where a value should be");
    refused("(a + b", "has no ')'");
    refused("a b", "where an operator or the end should be");
    refused("a % b", "where an operator or the end should be");
    refused("1..2", "two points");
    refused("z", "which no signal is");
    refused("c", "a computed signal");
    refused("obd2.map", "needs \"extends\": \"obd2\"");
    refused("a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a+a", "more than 16 operations");
    refused("1.5 * 1.5", "reads no signal");
    ASSERT_EQ(run("a * 1.5 * 1.5", 2, 0, &v), "ok");
    EXPECT_EQ(v, 4.5) << "a constant used twice is one";
    refused("((((((((((((((((((a))))))))))))))))))", "nests deeper than 16");
}

TEST(SignalFileTest, Obd2PublishesItsKeys)
{
    const auto input = [](const std::string& key) -> long {
        File f = load(R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2",)"
                      R"("signals":[{"key":"c","name":"C","kind":"computed","expr":"obd2.)" +
                      key + "\"}]}");
        if (!f) {
            return -1;
        }
        return ecuio_signal_file_set(f.get())->computed[0].input_ids[0];
    };
    EXPECT_EQ(input("map"), long{ECUIO_OBD2_PID_SIG_ID(0x0B, 0)});
    EXPECT_EQ(input("baro"), long{ECUIO_OBD2_PID_SIG_ID(0x33, 0)});
    EXPECT_EQ(input("rpm"), long{ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_EQ(input("pid_0c"), long{ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_EQ(input("coolant"), long{ECUIO_OBD2_SIG_COOLANT_TEMP_ID});
    EXPECT_EQ(input("pid_14"), long{ECUIO_OBD2_PID_SIG_ID(0x14, 0)});
    EXPECT_EQ(input("pid_14_1"), long{ECUIO_OBD2_PID_SIG_ID(0x14, 1)});
    EXPECT_EQ(input("pid_14_7"), -1) << "a field the PID doesn't have";
    EXPECT_EQ(input("pid_zz"), -1);
    EXPECT_EQ(input("boost"), -1);

    /* With no ecus, a signal reads the base's first ECU */
    File f = load(R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)" +
                  std::string(kNumber) + "]}");
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "a")->message_id, 0x7E8u);
}

TEST(SignalFileTest, EveryRefusalSaysWhereAndWhy)
{
    struct Case {
        std::string json;
        const char* words;
    };
    const std::string base = R"({"havne_signals":1,"name":"T","source":{"name":"t"},)";
    const Case cases[] = {
        {"", "line"},
        {"[]", "the file is a list, not an object"},
        {R"({"name":"T"})", "starts with \"havne_signals\""},
        {R"({"havne_signals":2,"name":"T"})", "format 2, and this Havne reads format 1"},
        {base + R"("signals":[]})", "lists none"},
        {base + R"("signalz":[]})", "unknown key 'signalz' in the file"},
        {R"({"havne_signals":1,"name":"T","signals":[]})", "needs a \"source\""},
        {R"({"havne_signals":1,"name":"T","source":{},"signals":[]})", "\"source\" needs a \"name\""},
        {R"({"havne_signals":1,"source":{"name":"t"},"signals":[]})", "needs a \"name\""},
        {base + R"("extends":"mock","signals":[]})", "only \"obd2\""},
        {one(kNumber, R"([{"id":"bcm","rx":"7B8"}])"), "ecu 'bcm': rx 7B8 needs a tx"},
        {one(kNumber, R"([{"id":"bcm","rx":"7B8","tx":"18DA40F1"}])"), "another width than its rx"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"b","rx":"18DAF118"}])"),
         "29-bit beside 11-bit IDs"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"b","rx":"7E9"},{"id":"c","rx":"7EA"},)"
                      R"({"id":"d","rx":"7EB"},{"id":"e","rx":"7EC"}])"),
         "past the 4 ECUs"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"a","rx":"7E9"}])"), "is given twice"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"b","rx":"7E8"}])"), "shares its rx with ecu 'a'"},
        {one(kNumber, R"([{"id":"a","rx":"7G8"}])"), "isn't hex"},
        {one(kNumber, R"([])"), "lists no ECU"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},"scael":2})"),
         "unknown key 'scael' in signal 'a'"},
        {one(std::string(kNumber) + "," + kNumber), "key 'a' is given twice"},
        {one(R"({"key":"A","name":"A"})"), "takes 1 to 31 of a-z"},
        {one(R"({"key":"a","name":"A","kind":"float"})"), "isn't number, text, hex, enum or computed"},
        {one(R"({"key":"a","name":"A","field":{"byte":0}})"), "needs \"read\""},
        {one(R"({"key":"a","name":"A","read":{"service":"09","number":"02"},"field":{"byte":0}})"),
         "isn't \"01\" or \"22\""},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D10"},"field":{"byte":0}})"),
         "takes 4 hex digits"},
        {one(R"({"key":"a","name":"A","ecu":"tcm","read":{"service":"22","number":"D100"},"field":{"byte":0}})"),
         "which \"ecus\" doesn't list"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0,"bits":4}})"),
         "big-endian, so whole bytes"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0,"bits":33,"order":"le"}})"),
         "takes a whole number from 1 to 32"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},"scale":0})"),
         "scale is 0"},
        {one(R"({"key":"a","name":"=A","read":{"service":"22","number":"D100"},"field":{"byte":0}})"),
         "starts as a spreadsheet formula"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},"group":"brakes"})"),
         "isn't a group"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},"decimals":4})"),
         "from 0 to 3"},
        {one(R"({"key":"a","name":"","read":{"service":"22","number":"D100"},"field":{"byte":0}})"),
         "takes 1 to 31"},
        {one(R"({"key":"a","name":"A\u0000B","read":{"service":"22","number":"D100"},"field":{"byte":0}})"),
         "holds a NUL"},
        {one(R"({"key":"a","name":"A","kind":"text","read":{"service":"22","number":"F188"},"field":{"byte":0,"bits":8}})"),
         "unknown key 'bits'"},
        {one(R"({"key":"a","name":"A","kind":"text","read":{"service":"22","number":"F188"},"field":{"byte":0},"scale":2})"),
         "unknown key 'scale'"},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0}})"),
         "needs \"values\""},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0},"values":{"256":"X"}})"),
         "can't be in its 8-bit field"},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0},"values":{"01":"X"}})"),
         "isn't a decimal number"},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0},"values":{"1":"-X"}})"),
         "starts as a spreadsheet formula"},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0},"values":{"1":"0123456789012345678901234567"}})"),
         "takes 1 to 27"},
        {one(R"({"key":"a","name":"A","kind":"enum","read":{"service":"01","number":"03"},"field":{"byte":0,"signed":true},"values":{"1":"X"}})"),
         "unknown key 'signed'"},
        {one(std::string(kNumber) + R"(,{"key":"c","name":"C","kind":"computed","expr":"a","max_age":{"b":1}})"),
         "which its expr doesn't read"},
        {one(std::string(kNumber) + R"(,{"key":"c","name":"C","kind":"computed","expr":"a","max_age":{"a":61}})"),
         "takes 0.1 to 60 seconds"},
        {one(std::string(kNumber) + R"(,{"key":"c","name":"C","kind":"computed","expr":"a","ecu":"pcm"})"),
         "unknown key 'ecu'"},
        {one(R"({"key":"c","name":"C","kind":"computed"})"), "needs an \"expr\""},
    };
    for (const Case& c : cases) {
        const std::string got = refusal(c.json);
        EXPECT_NE(got.find(c.words), std::string::npos)
            << "file: " << c.json << "\n  got: " << got << "\n  want: " << c.words;
        EXPECT_EQ(got.compare(0, 5, "line "), 0) << got;
    }

    /* The line and column are the offending value's */
    const std::string got = refusal("{\n  \"havne_signals\": 1,\n  \"name\": \"T\",\n"
                                    "  \"source\": {\"name\": \"t\"},\n  \"extends\": 7\n}");
    EXPECT_EQ(got, "line 5, column 14: \"extends\" is a number, not a string");

    /* JSON's own refusals come through with their place too */
    EXPECT_NE(refusal("{\"havne_signals\":1,}").find("line 1"), std::string::npos);
    EXPECT_NE(refusal("\xEF\xBB\xBF{}").find("line 1"), std::string::npos) << "a BOM";
}

TEST(SignalFileTest, TheRequestsMustFitTheEngine)
{
    /* One DID a signal: 256 fit, 257 don't */
    const auto file = [](unsigned n) {
        std::string s;
        for (unsigned i = 0; i < n; ++i) {
            char sig[160];
            std::snprintf(sig, sizeof sig,
                          R"(%s{"key":"s%u","name":"S%u","read":{"service":"22","number":"%04X"},"field":{"byte":0}})",
                          i ? "," : "", i, i, 0x1000 + i);
            s += sig;
        }
        return one(s);
    };
    File fits = load(file(ECUIO_PROFILE_MAX_REQUESTS));
    EXPECT_NE(fits, nullptr) << ecuio_last_error_detail();
    EXPECT_NE(refusal(file(ECUIO_PROFILE_MAX_REQUESTS + 1)).find("more than 256 requests"),
              std::string::npos);
}

TEST(SignalFileTest, NullsAndTheLoadersBounds)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load("{}", 2, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_load(nullptr, 2, &f), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(f, nullptr);
    EXPECT_EQ(ecuio_signal_file_load(nullptr, 0, &f), ECUIO_ERR_INVALID_ARGUMENT)
        << "no bytes is an empty file";
    EXPECT_EQ(ecuio_signal_file_set(nullptr), nullptr);
    EXPECT_STREQ(ecuio_signal_file_name(nullptr), "");
    EXPECT_STREQ(ecuio_signal_file_profile(nullptr), "");
    ecuio_signal_file_free(nullptr);

    /* 1 MiB at most */
    std::string big = one(kNumber);
    big.insert(big.size() - 1, ",\"x-pad\":\"" +
                                   std::string(ECUIO_SIGNAL_FILE_MAX_BYTES, ' ') + "\"");
    EXPECT_FALSE(refusal(big).empty()) << "past 1 MiB";
    /* x- keys and notes are carried past */
    File g = load(one(R"({"key":"a","name":"A","notes":"n","x-tool":{"any":[1,2]},)"
                      R"("read":{"service":"22","number":"D100"},"field":{"byte":0}})"));
    EXPECT_NE(g, nullptr) << ecuio_last_error_detail();
}
