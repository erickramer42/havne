// File/layer: tests/test_signal_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals)

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

/* Whether text holds words */
bool has(const std::string& text, const char* words)
{
    return text.find(words) != std::string::npos;
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

/* An 8-bit signal in deg C reading -40 to 215, with the JSON after "gauge" */
std::string gauged(const std::string& gauge)
{
    return one(
        R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0},"offset":-40,"unit":"degC","gauge":)" +
        gauge + "}");
}

const ecuio_signal_gauge_t* gauge_of(const File& f, const char* key)
{
    return ecuio_signal_file_gauge(f.get(),
                                   ecuio_signal_file_find(f.get(), key));
}

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
    ASSERT_EQ(set->count, obd2->count + 3)
        << "the base's, then the file's, its Boost standing for obd2's";
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
    ASSERT_EQ(set->computed_count, 1u) << "the file's boost replaced obd2's";
    int boosts = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        boosts += std::strcmp(set->defs[i].name, "Boost") == 0 ? 1 : 0;
    }
    EXPECT_EQ(boosts, 1) << "one Boost, the file's";
    EXPECT_EQ(boost->id, ECUIO_OBD2_SIG_BOOST_ID)
        << "the file's Boost has the base's ID: what names 7011 reads it";
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
        /* the base's boost comes first, the file's program after it */
        const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
        return set->computed[set->computed_count - 1].input_ids[0];
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

/* A signal named as a base def replaces it, whatever it is; the rest of the
 * base stays and the set's names are one each */
TEST(SignalFileTest, ASignalNamedAsABaseDefReplacesIt)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    File f = load(
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)"
        R"({"key":"temp","name":"Coolant Temp","unit":"kPa",)"
        R"("read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"rpm","name":"RPM","read":{"service":"22","number":"D101"},"field":{"byte":0}}]})");
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    EXPECT_EQ(set->count, obd2->count) << "two out, two in";
    for (uint32_t i = 0; i < set->count; ++i) {
        for (uint32_t j = i + 1; j < set->count; ++j) {
            EXPECT_STRNE(set->defs[i].name, set->defs[j].name);
        }
    }
    const ecuio_signal_def_t* temp = ecuio_signal_file_find(f.get(), "temp");
    ASSERT_NE(temp, nullptr);
    EXPECT_EQ(temp->id, ECUIO_OBD2_SIG_COOLANT_TEMP_ID)
        << "it takes the base def's ID";
    EXPECT_EQ(temp->mux_key, 0x62D100u) << "the file's def, not PID 05's";
    /* Each replaced ID is in the set once, on the file's def */
    for (const auto& [id, key] :
         {std::pair<uint16_t, const char*>{ECUIO_OBD2_SIG_COOLANT_TEMP_ID, "temp"},
          std::pair<uint16_t, const char*>{ECUIO_OBD2_SIG_RPM_ID, "rpm"}}) {
        int with = 0;
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                ++with;
                EXPECT_STREQ(ecuio_signal_file_key(f.get(), &set->defs[i]), key)
                    << "the def with ID " << id << " is the file's";
            }
        }
        EXPECT_EQ(with, 1) << id;
    }
    /* A def the file left alone has no key: it is the base's */
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == ECUIO_OBD2_SIG_SPEED_ID) {
            EXPECT_STREQ(ecuio_signal_file_key(f.get(), &set->defs[i]), "");
        }
    }
}

TEST(SignalFileTest, TheBaseKeepsItsBoostWhenTheFileLeavesItAlone)
{
    File f = load(
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)" +
        std::string(kNumber) + "]}");
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    EXPECT_EQ(set->count, ecuio_obd2_signal_set()->count + 1);
    EXPECT_EQ(set->computed_count, 1u);
    EXPECT_NE(set->defs[ecuio_obd2_signal_set()->count - 1].flags &
                  ECUIO_DEF_FLAG_COMPUTED,
              0u);
}

/* The file's def has the base's ID, so what reads that ID reads the file's:
 * obd2's own Boost reads a replaced MAP or baro, and a computed signal's
 * obd2.<key> reads a replaced def */
TEST(SignalFileTest, WhatReadsAReplacedIdReadsTheFilesDef)
{
    const std::string head =
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)";
    File f = load(head +
        R"({"key":"baro","name":"Barometric Pressure","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0}}]})");
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    EXPECT_EQ(set->computed_count, 1u) << "obd2's Boost stays";
    const ecuio_signal_def_t* boost = nullptr;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == ECUIO_OBD2_SIG_BOOST_ID) {
            boost = &set->defs[i];
        }
    }
    ASSERT_NE(boost, nullptr);
    EXPECT_EQ(set->computed[boost->table_first].input_ids[1],
              ECUIO_OBD2_PID_SIG_ID(0x33, 0));
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "baro")->id,
              ECUIO_OBD2_PID_SIG_ID(0x33, 0))
        << "that input is now the file's baro";

    File g = load(head +
        R"({"key":"s","name":"Vehicle Speed","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0}},)"
        R"({"key":"c","name":"C","kind":"computed","expr":"obd2.speed"}]})");
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* gs = ecuio_signal_file_set(g.get());
    const ecuio_signal_def_t* c = ecuio_signal_file_find(g.get(), "c");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(gs->computed[c->table_first].input_ids[0], ECUIO_OBD2_SIG_SPEED_ID)
        << "obd2.speed is the file's Vehicle Speed";
    EXPECT_STREQ(ecuio_signal_file_key(g.get(), ecuio_signal_file_find(g.get(), "s")), "s");
    /* A replacement that can't be an input (a text) is refused at its
     * signal, naming the computed signal that reads it */
    const std::string text_baro = refusal(
        head +
        R"({"key":"b","name":"Barometric Pressure","kind":"text","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0}}]})");
    EXPECT_TRUE(has(text_baro, "line 1, column 81")) << text_baro;
    EXPECT_TRUE(
        has(text_baro,
            "signal 'b' replaces 'Barometric Pressure', which 'Boost' "
            "reads: a signal a computed one reads is a number or an enum"))
        << text_baro;
    const std::string text_speed = refusal(
        head +
        R"({"key":"s","name":"Vehicle Speed","kind":"hex","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0}},)"
        R"({"key":"c","name":"C","kind":"computed","expr":"obd2.speed"}]})");
    EXPECT_TRUE(
        has(text_speed, "signal 's' replaces 'Vehicle Speed', which 'C' reads"))
        << text_speed;
}

/* Two signals of a file with one name are refused, naming both: a recording
 * keys its values by name */
TEST(SignalFileTest, ANameTwiceInOneFileIsRefusedNamingBoth)
{
    const std::string two =
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)"
        R"({"key":"a","name":"Same","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"b","name":"Same","read":{"service":"22","number":"D101"},"field":{"byte":0}}]})";
    const std::string why = refusal(two);
    EXPECT_TRUE(has(why, "signal 'b' has the name 'Same'"));
    EXPECT_TRUE(has(why, "signal 'a' has too"));
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

// An expression reads a signal listed after it: its inputs are the ids
// the later signals get
TEST(SignalFileTest, AnExpressionReadsASignalListedAfterIt)
{
    File f = load(one(
        std::string(
            R"({"key":"sum","name":"Sum","kind":"computed","expr":"a - b"},)") +
        kNumber +
        R"(,{"key":"b","name":"B","read":{"service":"22","number":"D101"},"field":{"byte":0}})"));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    ASSERT_EQ(set->computed_count, 1u);
    EXPECT_EQ(set->computed[0].input_count, 2u);
    EXPECT_EQ(set->computed[0].input_ids[0], ECUIO_USER_SIG_ID_BASE + 1);
    EXPECT_EQ(set->computed[0].input_ids[1], ECUIO_USER_SIG_ID_BASE + 2);
}

// A field must end within the 4096 bytes a reply holds; one ending at the
// last byte loads
TEST(SignalFileTest, AFieldEndingPastTheReplyIsRefused)
{
    const auto signal = [](const std::string& kind, const std::string& field) {
        return one(R"({"key":"a","name":"A",)" + kind +
                   R"("read":{"service":"22","number":"D100"},"field":)" +
                   field + "}");
    };
    const std::string enumeration = R"("kind":"enum","values":{"1":"One"},)";
    const std::string text = R"("kind":"text",)";
    for (const std::string& fits :
         {signal("", R"({"byte":4092})"),
          signal("", R"({"byte":4091,"bits":16})"),
          signal("", R"({"byte":4092,"bits":4,"bit":4,"order":"le"})"),
          signal(enumeration, R"({"byte":4092})"),
          signal(text, R"({"byte":4090,"bytes":3})")}) {
        EXPECT_NE(load(fits), nullptr) << ecuio_last_error_detail() << "\n"
                                       << fits;
    }
    for (const std::string& past :
         {signal("", R"({"byte":4092,"bits":16})"),
          signal("", R"({"byte":4092,"bits":4,"bit":6,"order":"le"})"),
          signal(enumeration, R"({"byte":4092,"bits":16})"),
          signal(text, R"({"byte":4090,"bytes":4})")}) {
        const std::string why = refusal(past);
        EXPECT_NE(
            why.find(
                "signal 'a''s field ends past the 4096 bytes a reply holds"),
            std::string::npos)
            << why;
    }
}

// A def's key is the file's own name for it, the inverse of find: each
// own def names its key, and a base set's def (extends), another set's
// def with the same ID, a copy, and NULLs give ""
TEST(SignalFileTest, EachOwnDefNamesItsKeyAndNoOtherDefDoes)
{
    File f = load(kExample);
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    ASSERT_EQ(set->count, obd2->count + 3) << "the file's Boost stands for obd2's";
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t* def = &set->defs[i];
        const char* key = ecuio_signal_file_key(f.get(), def);
        ASSERT_NE(key, nullptr) << def->name;
        if (i < obd2->count - 1) {
            EXPECT_STREQ(key, "") << "the base's " << def->name;
            EXPECT_STREQ(ecuio_signal_file_key(f.get(), &obd2->defs[i]), "")
                << "obd2's own " << def->name;
        } else {
            EXPECT_NE(std::string(key), "") << def->name;
            EXPECT_EQ(ecuio_signal_file_find(f.get(), key), def) << key;
        }
    }
    EXPECT_STREQ(
        ecuio_signal_file_key(f.get(), ecuio_signal_file_find(f.get(), "tft")),
        "tft");
    EXPECT_STREQ(ecuio_signal_file_key(
                     f.get(), ecuio_signal_file_find(f.get(), "boost")),
                 "boost")
        << "a computed def has its key too";

    /* By address, never by ID: a copy and another file's def aren't it */
    const ecuio_signal_def_t copy = *ecuio_signal_file_find(f.get(), "tft");
    EXPECT_STREQ(ecuio_signal_file_key(f.get(), &copy), "");
    File other = load(kExample);
    ASSERT_NE(other, nullptr);
    EXPECT_STREQ(ecuio_signal_file_key(
                     f.get(), ecuio_signal_file_find(other.get(), "tft")),
                 "");

    EXPECT_STREQ(ecuio_signal_file_key(nullptr, &copy), "");
    EXPECT_STREQ(ecuio_signal_file_key(f.get(), nullptr), "");

    /* A file that extends nothing: every def is its own, each keyed */
    File alone = load(one(kNumber));
    ASSERT_NE(alone, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* s = ecuio_signal_file_set(alone.get());
    ASSERT_EQ(s->count, 1u);
    EXPECT_STREQ(ecuio_signal_file_key(alone.get(), &s->defs[0]), "a");
}

TEST(SignalFileTest, AGaugeIsKeptAsTheFileWroteIt)
{
    File f = load(
        gauged(R"({"min":40.5,"max":130,"warn_low":60,"warn_high":110.25})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    const ecuio_signal_gauge_t* g = gauge_of(f, "a");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->min, 40.5);
    EXPECT_EQ(g->max, 130.0);
    EXPECT_EQ(g->warn_low, 60.0);
    EXPECT_EQ(g->warn_high, 110.25);
    EXPECT_EQ(g->flags, ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH);
    EXPECT_EQ(g->reserved[0] | g->reserved[1] | g->reserved[2], 0u);
}

TEST(SignalFileTest, AGaugesLevelsAreOptionalAndEachHasItsFlag)
{
    File none = load(gauged(R"({"min":0,"max":100})"));
    ASSERT_TRUE(none) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(none, "a"), nullptr);
    EXPECT_EQ(gauge_of(none, "a")->flags, 0u);
    EXPECT_EQ(gauge_of(none, "a")->warn_low, 0.0);
    EXPECT_EQ(gauge_of(none, "a")->warn_high, 0.0);

    File low = load(gauged(R"({"min":0,"max":100,"warn_low":10})"));
    ASSERT_TRUE(low) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(low, "a"), nullptr);
    EXPECT_EQ(gauge_of(low, "a")->flags, ECUIO_GAUGE_WARN_LOW);
    EXPECT_EQ(gauge_of(low, "a")->warn_low, 10.0);
    EXPECT_EQ(gauge_of(low, "a")->warn_high, 0.0);

    File high = load(gauged(R"({"min":0,"max":100,"warn_high":90})"));
    ASSERT_TRUE(high) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(high, "a"), nullptr);
    EXPECT_EQ(gauge_of(high, "a")->flags, ECUIO_GAUGE_WARN_HIGH);
    EXPECT_EQ(gauge_of(high, "a")->warn_high, 90.0);
    EXPECT_EQ(gauge_of(high, "a")->warn_low, 0.0);
}

TEST(SignalFileTest, AGaugeTakesWhatADialCanShow)
{
    const char* const kept[] = {
        R"({"min":0,"max":100,"warn_low":0,"warn_high":100})", /* the ends */
        R"({"min":-100,"max":500})", /* wider than the field */
        R"({"min":-100,"max":-40})", /* meets the field's lowest */
        R"({"min":215,"max":300})",  /* meets its highest */
        R"({"min":200,"max":300})",  /* overlaps it */
        R"({"min":0,"max":100,"notes":"from the gauge cluster","x-tool":[1]})",
    };
    for (const char* gauge : kept) {
        File f = load(gauged(gauge));
        EXPECT_TRUE(f) << gauge << ": " << ecuio_last_error_detail();
    }
}

/* The field's reach follows its scale's sign and its own: a negative
 * scale reads its span backwards (8 bits at -0.5 from 100: -27.5 to 100)
 * and a signed field reads below zero (8 signed bits: -128 to 127) */
TEST(SignalFileTest, AFieldsReachFollowsItsScaleAndItsSign)
{
    struct Case {
        const char* field;
        const char* gauge;
        bool kept;
    };
    const char* const backwards =
        R"("field":{"byte":0},"scale":-0.5,"offset":100)";
    const char* const signed8 = R"("field":{"byte":0,"signed":true})";
    const Case cases[] = {
        {backwards, R"({"min":-27.5,"max":-10})", true},  /* its lowest */
        {backwards, R"({"min":100,"max":200})", true},    /* its highest */
        {backwards, R"({"min":-40,"max":0})", true},      /* overlapping */
        {backwards, R"({"min":-60,"max":-28})", false},   /* under it */
        {backwards, R"({"min":100.5,"max":200})", false}, /* over it */
        {signed8, R"({"min":-128,"max":-100})", true},    /* its lowest */
        {signed8, R"({"min":120,"max":200})", true},      /* overlapping */
        {signed8, R"({"min":-200,"max":-129})", false},   /* under it */
        {signed8, R"({"min":128,"max":200})", false},     /* over it */
    };
    for (const Case& k : cases) {
        File f = load(one(std::string(R"({"key":"a","name":"A","read":)"
                                      R"({"service":"22","number":"D100"},)") +
                          k.field + R"(,"gauge":)" + k.gauge + "}"));
        EXPECT_EQ(static_cast<bool>(f), k.kept)
            << k.field << " " << k.gauge << ": " << ecuio_last_error_detail();
        if (!k.kept) {
            EXPECT_NE(std::string(ecuio_last_error_detail())
                          .find("but its field reads only"),
                      std::string::npos)
                << ecuio_last_error_detail();
        }
    }
}

TEST(SignalFileTest, EveryGaugeRefusalSaysWhy)
{
    struct Case {
        const char* gauge;
        const char* words;
    };
    const Case cases[] = {
        {"[]", "signal 'a''s gauge is a list, not an object"},
        {R"({"max":1})", "gauge needs \"min\" and \"max\""},
        {R"({"min":0})", "gauge needs \"min\" and \"max\""},
        {R"({"min":"0","max":1})", "gauge min is a string, not a number"},
        {R"({"min":0,"max":null})", "gauge max is"},
        {R"({"min":0,"max":1,"warn_low":true})", "gauge warn_low is"},
        {R"({"min":0,"max":1,"warn_high":[]})", "gauge warn_high is a list"},
        {R"({"min":0,"max":1,"high":1})",
         "unknown key 'high' in signal 'a''s gauge"},
        {R"({"min":5,"max":5})", "gauge max 5 isn't above its min 5"},
        {R"({"min":9,"max":5})", "gauge max 5 isn't above its min 9"},
        {R"({"min":0,"max":10,"warn_low":-0.5})",
         "gauge warn_low -0.5 is outside its range, 0 to 10"},
        {R"({"min":0,"max":10,"warn_low":10.5})",
         "gauge warn_low 10.5 is outside"},
        {R"({"min":0,"max":10,"warn_high":-0.5})",
         "gauge warn_high -0.5 is outside"},
        {R"({"min":0,"max":10,"warn_high":10.5})",
         "gauge warn_high 10.5 is outside"},
        {R"({"min":0,"max":10,"warn_low":5,"warn_high":5})",
         "gauge warn_high 5 isn't above its warn_low 5"},
        {R"({"min":0,"max":10,"warn_low":6,"warn_high":5})",
         "gauge warn_high 5 isn't above its warn_low 6"},
        {R"({"min":250,"max":400})",
         "gauge runs 250 to 400, but its field reads only -40 to 215: "
         "is the gauge in another unit than 'degC'?"},
        {R"({"min":-100,"max":-41})", "but its field reads only -40 to 215"},
    };
    for (const Case& c : cases) {
        const std::string words = refusal(gauged(c.gauge));
        EXPECT_NE(words.find("line 1, column"), std::string::npos) << words;
        EXPECT_NE(words.find(c.words), std::string::npos)
            << c.gauge << ": " << words;
    }
}

TEST(SignalFileTest, ARangeTheFieldCantReachSaysSoWithNoUnitToName)
{
    const std::string words = refusal(
        one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
            R"("field":{"byte":0},"gauge":{"min":300,"max":400}})"));
    EXPECT_NE(words.find("in another unit than the signal's?"),
              std::string::npos)
        << words;
}

TEST(SignalFileTest, ANumberPastTheDoubleRangeIsNoGauge)
{
    EXPECT_NE(refusal(gauged(R"({"min":0,"max":1e999})")).find("line 1"),
              std::string::npos);
    EXPECT_NE(refusal(gauged(R"({"min":-1e999,"max":0})")).find("line 1"),
              std::string::npos);
    EXPECT_NE(refusal(gauged(R"({"min":0,"max":NaN})")).find("line 1"),
              std::string::npos);
}

TEST(SignalFileTest, OnlyNumbersAndComputedSignalsTakeAGauge)
{
    const char* const kGauge = R"("gauge":{"min":0,"max":1})";
    const std::string read =
        R"("read":{"service":"22","number":"D100"},"field":{"byte":0},)";
    for (const std::string& s :
         {R"({"key":"a","name":"A",)" + read + R"("kind":"text",)" + kGauge +
              "}",
          R"({"key":"a","name":"A",)" + read + R"("kind":"hex",)" + kGauge +
              "}",
          R"({"key":"a","name":"A",)" + read +
              R"("kind":"enum","values":{"1":"On"},)" + kGauge + "}"}) {
        const std::string words = refusal(one(s));
        EXPECT_NE(words.find("unknown key 'gauge' in signal 'a'"),
                  std::string::npos)
            << words;
    }

    File f =
        load(one(std::string(kNumber) + "," +
                 R"({"key":"b","name":"B","kind":"computed","expr":"a * 2",)" +
                 R"("gauge":{"min":-5,"max":5000,"warn_high":4000}})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(f, "b"), nullptr);
    EXPECT_EQ(gauge_of(f, "b")->min, -5.0)
        << "a computed signal has no field to judge";
    EXPECT_EQ(gauge_of(f, "b")->warn_high, 4000.0);
    EXPECT_EQ(gauge_of(f, "a"), nullptr);
}

TEST(SignalFileTest, AGaugeIsTheDefsOwnAndNoOtherDefs)
{
    File f = load(
        R"json({"havne_signals":1,"name":"T","source":{"name":"t"},"extends":"obd2",
      "signals":[
        {"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},
         "gauge":{"min":0,"max":1}},
        {"key":"b","name":"B","read":{"service":"22","number":"D101"},"field":{"byte":0},
         "gauge":{"min":2,"max":3}},
        {"key":"c","name":"C","read":{"service":"22","number":"D102"},"field":{"byte":0}}
      ]})json");
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    ASSERT_NE(gauge_of(f, "a"), nullptr);
    ASSERT_NE(gauge_of(f, "b"), nullptr);
    EXPECT_EQ(gauge_of(f, "a")->min, 0.0);
    EXPECT_EQ(gauge_of(f, "b")->min, 2.0) << "each def's own";
    EXPECT_EQ(gauge_of(f, "c"), nullptr);

    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < obd2->count; ++i) {
        EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &set->defs[i]), nullptr)
            << "the base set's " << set->defs[i].name;
        EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &obd2->defs[i]), nullptr);
    }
    ecuio_signal_def_t copy = *ecuio_signal_file_find(f.get(), "a");
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(), &copy), nullptr)
        << "a copy isn't the file's";
    File same = load(
        R"({"havne_signals":1,"name":"T","source":{"name":"t"},"ecus":[{"id":"p","rx":"7E8"}],
      "signals":[{"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},
      "gauge":{"min":0,"max":1}}]})");
    ASSERT_TRUE(same) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(),
                                      ecuio_signal_file_find(same.get(), "a")),
              nullptr)
        << "another file's def";
    EXPECT_EQ(ecuio_signal_file_gauge(nullptr, &copy), nullptr);
    EXPECT_EQ(ecuio_signal_file_gauge(f.get(), nullptr), nullptr);
}

TEST(SignalFileTest, AGaugeLeavesTheDefAndTheRestOfTheSetAlone)
{
    File with = load(gauged(R"({"min":0,"max":100,"warn_high":90})"));
    File without = load(
        one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
            R"("field":{"byte":0},"offset":-40,"unit":"degC"})"));
    ASSERT_TRUE(with) << ecuio_last_error_detail();
    ASSERT_TRUE(without) << ecuio_last_error_detail();
    const ecuio_signal_set_t* a = ecuio_signal_file_set(with.get());
    const ecuio_signal_set_t* b = ecuio_signal_file_set(without.get());
    ASSERT_EQ(a->count, b->count);
    EXPECT_EQ(std::memcmp(a->defs, b->defs, sizeof *a->defs * a->count), 0)
        << "a recording's decode never depends on a gauge";
}

TEST(SignalFileTest, AToolsOwnXGaugeStaysIgnored)
{
    File f = load(one(
        R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0},"x-gauge":{"min":9,"max":1,"warn_low":"no"}})"));
    ASSERT_TRUE(f) << ecuio_last_error_detail();
    EXPECT_EQ(gauge_of(f, "a"), nullptr);
}
