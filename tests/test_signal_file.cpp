// File/layer: tests/test_signal_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals)

#include "test_signal_file_support.h"

namespace {

/* n ECUs as an "ecus" list, each on IDs of its own */
std::string ecus_of(unsigned n)
{
    std::string s = "[";
    for (unsigned i = 0; i < n; ++i) {
        char e[64];
        std::snprintf(e, sizeof e,
                      "%s{\"id\":\"e%u\",\"rx\":\"%03X\",\"tx\":\"%03X\"}",
                      i > 0 ? "," : "", i, 0x700u + i, 0x600u + i);
        s += e;
    }
    return s + "]";
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
            "reads: a computed signal reads a number, an enum or another "
            "computed signal"))
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

/* ADR-0062: in format 2 a computed replacement is read as any computed
 * signal is, by the file's own (the boost-ratio example's shape) and by
 * obd2's Boost; format 1 refuses it, naming the reader */
TEST(SignalFileTest, AComputedReplacementIsReadInFormat2)
{
    const auto head = [](int format) {
        return R"({"havne_signals":)" + std::to_string(format) +
               R"(,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)";
    };
    File f = load(
        head(2) +
        R"({"key":"boost","name":"Boost","kind":"computed","expr":"obd2.map - obd2.baro"},)"
        R"({"key":"pr","name":"Pressure Ratio","kind":"computed",)"
        R"("expr":"(boost + obd2.baro) / obd2.baro"}]})");
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    EXPECT_EQ(set->computed_count, 2u)
        << "obd2's Boost left out, two of the file's";
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "boost")->id,
              ECUIO_OBD2_SIG_BOOST_ID);
    const ecuio_signal_def_t* pr = ecuio_signal_file_find(f.get(), "pr");
    EXPECT_EQ(set->computed[pr->table_first].input_ids[0],
              ECUIO_OBD2_SIG_BOOST_ID)
        << "Pressure Ratio reads the file's Boost";

    const std::string map =
        R"({"key":"map","name":"Intake Manifold Pressure","kind":"computed",)"
        R"("expr":"obd2.baro * 1","unit":"kPa"}]})";
    File g = load(head(2) + map);
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* gs = ecuio_signal_file_set(g.get());
    const ecuio_signal_def_t* boost = nullptr;
    for (uint32_t i = 0; i < gs->count; ++i) {
        if (gs->defs[i].id == ECUIO_OBD2_SIG_BOOST_ID) {
            boost = &gs->defs[i];
        }
    }
    ASSERT_NE(boost, nullptr);
    const ecuio_signal_def_t* own = ecuio_signal_file_find(g.get(), "map");
    EXPECT_EQ(gs->computed[boost->table_first].input_ids[0], own->id)
        << "obd2's Boost reads the computed MAP";
    EXPECT_NE(own->flags & ECUIO_DEF_FLAG_COMPUTED, 0u);

    const std::string one = refusal(head(1) + map);
    EXPECT_TRUE(has(one, "signal 'map' replaces 'Intake Manifold Pressure', "
                         "which 'Boost' reads: a computed signal read by "
                         "another needs \"havne_signals\": 2"))
        << one;
    /* obd2's Boost keeps an age bound on baro, which a computed input can't
     * carry */
    const std::string aged = refusal(
        head(2) +
        R"({"key":"baro","name":"Barometric Pressure","kind":"computed",)"
        R"("expr":"obd2.load * 0 + 101","unit":"kPa"}]})");
    EXPECT_TRUE(has(aged, "signal 'baro' replaces 'Barometric Pressure', which "
                          "'Boost' reads with an age bound: a computed input's "
                          "ages are its own inputs'"))
        << aged;
    const std::string aged_by_key = refusal(
        head(2) +
        R"({"key":"s","name":"Vehicle Speed","kind":"computed","expr":"obd2.rpm * 0"},)"
        R"({"key":"c","name":"C","kind":"computed","expr":"obd2.speed + 1",)"
        R"("max_age":{"obd2.speed":5}}]})");
    EXPECT_TRUE(has(aged_by_key, "signal 's' replaces 'Vehicle Speed', which "
                                 "'C' reads with an age bound"))
        << aged_by_key;
    /* A file's obd2.<key> reads a replacement by its ID: the reader's
     * format decides */
    const std::string speed =
        R"({"key":"s","name":"Vehicle Speed","kind":"computed","expr":"obd2.rpm * 0"},)"
        R"({"key":"c","name":"C","kind":"computed","expr":"obd2.speed + 1"}]})";
    EXPECT_TRUE(load(head(2) + speed)) << ecuio_last_error_detail();
    const std::string by_key = refusal(head(1) + speed);
    EXPECT_TRUE(has(by_key, "signal 's' replaces 'Vehicle Speed', which 'C' "
                            "reads: a computed signal read by another needs "
                            "\"havne_signals\": 2"))
        << by_key;
}

/* A chain through a replacement is the file's chain: a circle and a depth
 * past 8 are refused in the file's words, and a replacement that puts
 * obd2's Boost past 8 deep is refused at it; exactly 8 deep loads */
TEST(SignalFileTest, AChainThroughAReplacementIsChecked)
{
    const std::string head =
        R"({"havne_signals":2,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)";
    const std::string circle = refusal(
        head +
        R"({"key":"boost","name":"Boost","kind":"computed","expr":"pr - 1"},)"
        R"({"key":"pr","name":"Pressure Ratio","kind":"computed","expr":"boost + 1"}]})");
    EXPECT_TRUE(has(circle, "signal 'boost' reads 'pr', which reads 'boost': "
                            "computed signals can't read in a circle"))
        << circle;

    /* e1 up a chain from engine speed, then a last level named as a base
     * def, levels deep in all */
    const auto chain = [&](const std::string& last, const std::string& more,
                           int levels = 8) {
        std::string text =
            head +
            R"({"key":"e1","name":"E1","kind":"computed","expr":"obd2.rpm + 1"},)";
        for (int k = 2; k < levels; ++k) {
            text += R"({"key":"e)" + std::to_string(k) + R"(","name":"E)" +
                    std::to_string(k) + R"(","kind":"computed","expr":"e)" +
                    std::to_string(k - 1) + R"( + 1"},)";
        }
        return text + R"({"key":"top","name":")" + last +
               R"(","kind":"computed","expr":"e)" + std::to_string(levels - 1) +
               R"( + 1","unit":"kPa"})" + more + "]}";
    };
    EXPECT_TRUE(load(chain("Boost", ""))) << ecuio_last_error_detail();
    const std::string nine = refusal(
        chain("Boost",
              R"(,{"key":"n","name":"N","kind":"computed","expr":"top + 1"})"));
    EXPECT_TRUE(has(nine, "signal 'n' sits 9 computed signals deep: a chain "
                          "goes 8 deep at most"))
        << nine;
    EXPECT_TRUE(load(chain("Intake Manifold Pressure", "", 7)))
        << "obd2's Boost 8 deep: " << ecuio_last_error_detail();
    const std::string under_boost =
        refusal(chain("Intake Manifold Pressure", ""));
    EXPECT_TRUE(
        has(under_boost,
            "signal 'top' replaces 'Intake Manifold Pressure', which 'Boost' "
            "reads: that puts 'Boost' 9 computed signals deep: a chain "
            "goes 8 deep at most"))
        << under_boost;
}

/* Two signals of a file with one name are refused in format 1 and 2,
 * naming both (ADR-0065 section 9), though a set the validator takes may
 * repeat a name */
TEST(SignalFileTest, ANameTwiceInOneFileIsRefusedNamingBoth)
{
    for (const char* format : {"1", "2"}) {
        const std::string two =
            std::string(R"({"havne_signals":)") + format +
            R"(,"name":"T","source":{"name":"t"},"extends":"obd2","signals":[)"
            R"({"key":"a","name":"Same","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
            R"({"key":"b","name":"Same","read":{"service":"22","number":"D101"},"field":{"byte":0}}]})";
        const std::string why = refusal(two);
        EXPECT_TRUE(has(why, "signal 'b' has the name 'Same'"))
            << "format " << format << ": " << why;
        EXPECT_TRUE(has(why, "signal 'a' has too")) << why;
    }
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
        {R"({"havne_signals":4,"name":"T"})",
         "format 4, and this Havne reads formats 1 to 3"},
        {R"({"havne_signals":0,"name":"T"})",
         "format 0, and this Havne reads formats 1 to 3"},
        {R"({"havne_signals":1.5,"name":"T"})",
         "format ?, and this Havne reads formats 1 to 3"},
        {base + R"("signals":[]})", "lists none"},
        {base + R"("signalz":[]})", "unknown key 'signalz' in the file"},
        {R"({"havne_signals":1,"name":"T","signals":[]})",
         "needs a \"source\""},
        {R"({"havne_signals":1,"name":"T","source":{},"signals":[]})",
         "\"source\" needs a \"name\""},
        {R"({"havne_signals":1,"source":{"name":"t"},"signals":[]})",
         "needs a \"name\""},
        {base + R"("extends":"mock","signals":[]})", "only \"obd2\""},
        {one(kNumber, R"([{"id":"bcm","rx":"7B8"}])"),
         "ecu 'bcm': rx 7B8 needs a tx"},
        {one(kNumber, R"([{"id":"bcm","rx":"7B8","tx":"18DA40F1"}])"),
         "another width than its rx"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"b","rx":"18DAF118"}])"),
         "29-bit beside 11-bit IDs"},
        {one(kNumber, ecus_of(ECUIO_MAX_CHANNELS + 1)),
         "ecu 'e32' is past the 32 ECUs one session reads"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"a","rx":"7E9"}])"),
         "is given twice"},
        {one(kNumber, R"([{"id":"a","rx":"7E8"},{"id":"b","rx":"7E8"}])"),
         "shares its rx with ecu 'a'"},
        {one(kNumber, R"([{"id":"a","rx":"7G8"}])"), "isn't hex"},
        {one(kNumber, R"([])"), "lists no ECU"},
        {one(R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},"field":{"byte":0},"scael":2})"),
         "unknown key 'scael' in signal 'a'"},
        {one(std::string(kNumber) + "," + kNumber), "key 'a' is given twice"},
        {one(R"({"key":"A","name":"A"})"), "takes 1 to 31 of a-z"},
        {one(R"({"key":"a","name":"A","kind":"float"})"),
         "isn't number, text, hex, enum or computed"},
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
        {one(std::string(kNumber) +
             R"(,{"key":"c","name":"C","kind":"computed","expr":"a","max_age":{"b":1}})"),
         "which its expr doesn't read"},
        {one(std::string(kNumber) +
             R"(,{"key":"c","name":"C","kind":"computed","expr":"a","max_age":{"a":61}})"),
         "takes 0.1 to 60 seconds"},
        {one(std::string(kNumber) +
             R"(,{"key":"c","name":"C","kind":"computed","expr":"a","ecu":"pcm"})"),
         "unknown key 'ecu'"},
        {one(R"({"key":"c","name":"C","kind":"computed"})"),
         "needs an \"expr\""},
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

TEST(SignalFileTest, EachEcusRequestsMustFitAModule)
{
    /* One DID for each signal: a file alone fits a module's most with
     * every signal it may hold; on obd2, whose requests count on its ECU,
     * it passes it */
    static_assert(ECUIO_SIGNAL_FILE_MAX_SIGNALS >= ECUIO_MODULE_MAX_REQUESTS,
                  "one file can ask a module its most");
    const auto file = [](unsigned n, bool on_obd2) {
        std::string s;
        for (unsigned i = 0; i < n; ++i) {
            char sig[160];
            std::snprintf(sig, sizeof sig,
                          R"(%s{"key":"s%u","name":"S%u","read":{"service":"22","number":"%04X"},"field":{"byte":0}})",
                          i ? "," : "", i, i, 0x1000 + i);
            s += sig;
        }
        std::string json = one(s);
        if (on_obd2) {
            json.insert(json.find(',') + 1, R"("extends":"obd2",)");
        }
        return json;
    };
    File fits = load(file(ECUIO_MODULE_MAX_REQUESTS, false));
    ASSERT_NE(fits, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(fits.get())->count,
              ECUIO_MODULE_MAX_REQUESTS);
    const std::string past = refusal(file(ECUIO_MODULE_MAX_REQUESTS, true));
    EXPECT_NE(past.find("the signals ask ecu 'pcm' more than " +
                        std::to_string(ECUIO_MODULE_MAX_REQUESTS) +
                        " requests, the base's counted"),
              std::string::npos)
        << past;

    /* The first signal past the module's most is the one named: obd2's
     * requests and the file's first ones fill it */
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    ecuio_derived base;
    const uint32_t rx = ECUIO_OBD2_ECU_ARB_ID;
    ASSERT_EQ(ecuio_engine_plan(obd2, &rx, 1, nullptr, &base),
              ecuio_plan_result::ok);
    ASSERT_LT(base.total, ECUIO_MODULE_MAX_REQUESTS);
    const unsigned room = ECUIO_MODULE_MAX_REQUESTS - base.total;
    File full = load(file(room, true));
    EXPECT_NE(full, nullptr) << ecuio_last_error_detail();
    const std::string one_more = refusal(file(room + 1, true));
    EXPECT_NE(one_more.find("the signals ask ecu 'pcm' more than"),
              std::string::npos)
        << one_more;
    const std::string json = file(room + 1, true);
    const size_t at = json.find("\"key\":\"s" + std::to_string(room) + "\"");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(one_more.find("column " + std::to_string(at)), std::string::npos)
        << "the signal past the module's most, at its opening brace: "
        << one_more;
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

/* A file of n signals, a module's most for each ECU in turn, each a DID of
 * its own on its ECU */
std::string car_file(unsigned n)
{
    std::string s;
    s.reserve(n * size_t{100});
    for (unsigned i = 0; i < n; ++i) {
        char sig[160];
        std::snprintf(
            sig, sizeof sig,
            R"(%s{"key":"s%u","name":"S%u","ecu":"e%u","read":{"service":"22","number":"%04X"},"field":{"byte":0}})",
            i ? "," : "", i, i, i / ECUIO_MODULE_MAX_REQUESTS,
            0x1000 + i % ECUIO_MODULE_MAX_REQUESTS);
        s += sig;
    }
    return one(s, ecus_of((n + ECUIO_MODULE_MAX_REQUESTS - 1) /
                          ECUIO_MODULE_MAX_REQUESTS));
}

/* The file with spaces before its last brace, size bytes in all */
std::string padded_to(std::string json, size_t size)
{
    EXPECT_LE(json.size(), size);
    json.insert(json.size() - 1, std::string(size - json.size(), ' '));
    return json;
}

File load_set(const std::string& json, ecuio_status_t* status = nullptr)
{
    ecuio_signal_file_t* f = reinterpret_cast<ecuio_signal_file_t*>(1);
    const ecuio_status_t s = ecuio_signal_file_load_with(
        json.data(), json.size(), ECUIO_SIGNAL_FILE_LOAD_SET, &f);
    if (status != nullptr) {
        *status = s;
    }
    if (s != ECUIO_OK) {
        EXPECT_EQ(f, nullptr) << "out is NULL on a failure";
    }
    return File(f);
}

std::string set_refusal(const std::string& json)
{
    ecuio_status_t s = ECUIO_OK;
    File f = load_set(json, &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    return ecuio_last_error_detail();
}

/* A file is held to a data file's bounds unless its reader says it is a
 * set's written copy (ADR-0065 section 6): then a set's bounds hold, and
 * nothing else about the load differs */
TEST(SignalFileTest, ASetsWrittenCopyLoadsOnlyWhenTheCallerSaysItIsOne)
{
    /* The call's own refusals */
    ecuio_signal_file_t* f = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_load_with("{}", 2, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_load_with(nullptr, 2, 0, &f),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(f, nullptr);
    const std::string small = one(kNumber);
    f = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_load_with(small.data(), small.size(), 0x2, &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(f, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a signal file's load takes no flag 0x2");
    f = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_load_with(
                  small.data(), small.size(),
                  ECUIO_SIGNAL_FILE_LOAD_SET | 0x80000000u, &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(f, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a signal file's load takes no flag 0x80000000");
    f = nullptr;
    ASSERT_EQ(ecuio_signal_file_load_with(small.data(), small.size(), 0, &f),
              ECUIO_OK);
    ecuio_signal_file_free(f);
    EXPECT_NE(load_set(small), nullptr) << "a small file is a set's too";

    /* Signals: one past a data file's most */
    const unsigned file_most = ECUIO_SIGNAL_FILE_MAX_SIGNALS;
    const unsigned set_most = ECUIO_SIGNAL_SET_MAX_SIGNALS;
    EXPECT_NE(load(car_file(file_most)), nullptr) << ecuio_last_error_detail();
    const std::string past_file = car_file(file_most + 1);
    EXPECT_TRUE(
        has(refusal(past_file), R"("signals" lists 1025, more than 1024)"))
        << ecuio_last_error_detail();
    File copy = load_set(past_file);
    ASSERT_NE(copy, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(copy.get())->count, file_most + 1);

    /* A set's most, and one past it */
    const std::string whole = car_file(set_most);
    ASSERT_GT(whole.size(), size_t{ECUIO_SIGNAL_FILE_MAX_BYTES});
    ASSERT_LT(whole.size(), size_t{ECUIO_SIGNAL_SET_MAX_BYTES});
    File car = load_set(whole);
    ASSERT_NE(car, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(car.get());
    EXPECT_EQ(set->count, set_most);
    EXPECT_EQ(set->channel_count, set_most / ECUIO_MODULE_MAX_REQUESTS);
    EXPECT_EQ(ecuio_validate_signal_set(set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_STREQ(set->defs[set_most - 1].name,
                 ("S" + std::to_string(set_most - 1)).c_str());
    EXPECT_EQ(set->defs[set_most - 1].message_id,
              0x700u + set_most / ECUIO_MODULE_MAX_REQUESTS - 1);
    EXPECT_TRUE(has(set_refusal(car_file(set_most + 1)),
                    R"("signals" lists 16385, more than 16384)"))
        << ecuio_last_error_detail();

    /* Bytes: a data file's most and a byte more, a set's most and a byte
     * more */
    EXPECT_NE(load(padded_to(small, ECUIO_SIGNAL_FILE_MAX_BYTES)), nullptr)
        << ecuio_last_error_detail();
    const std::string a_byte_more =
        padded_to(small, ECUIO_SIGNAL_FILE_MAX_BYTES + size_t{1});
    EXPECT_FALSE(refusal(a_byte_more).empty());
    EXPECT_NE(load_set(a_byte_more), nullptr) << ecuio_last_error_detail();
    EXPECT_NE(load_set(padded_to(whole, ECUIO_SIGNAL_SET_MAX_BYTES)), nullptr)
        << ecuio_last_error_detail();
    EXPECT_FALSE(
        set_refusal(padded_to(whole, ECUIO_SIGNAL_SET_MAX_BYTES + size_t{1}))
            .empty());
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
