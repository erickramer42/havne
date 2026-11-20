// File/layer: tests/test_condition_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ConditionFileTest (test_signals)

#include "test_condition_file_support.h"

TEST(ConditionFileTest, TheAdrsExampleLoads)
{
    Set s = load(kExample);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(ecuio_condition_set_name(s.get()), "Daily driver");
    ASSERT_EQ(ecuio_condition_set_count(s.get()), 4u);

    ecuio_check_options_t checks;
    std::memset(&checks, 0xFF, sizeof checks);
    ASSERT_EQ(ecuio_condition_set_checks(s.get(), &checks), ECUIO_OK);
    EXPECT_EQ(checks.gap_us, 1000000u);
    EXPECT_EQ(checks.rate_drop_x100, 1000u);
    EXPECT_EQ(checks.settle_us, 2000000u);
    EXPECT_EQ(checks.flags, 0u);
    for (uint64_t r : checks.reserved) {
        EXPECT_EQ(r, 0u);
    }

    const ecuio_condition_info_t coolant_high = info_of(s, 0);
    EXPECT_STREQ(coolant_high.key, "coolant_high");
    EXPECT_STREQ(coolant_high.name, "Coolant high");
    EXPECT_EQ(coolant_high.flags, 0u);
    EXPECT_EQ(coolant_high.uses,
              ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_MARK |
                  ECUIO_CONDITION_ALERT_SOUND | ECUIO_CONDITION_ALERT_BANNER);
    EXPECT_EQ(coolant_high.alert_level, ECUIO_ALERT_DANGER);
    EXPECT_EQ(coolant_high.term_count, 1u);
    EXPECT_EQ(coolant_high.for_us, 2000000u);
    EXPECT_EQ(coolant_high.clear_for_us, 0u);
    EXPECT_EQ(coolant_high.before_us, 0u);
    EXPECT_EQ(coolant_high.after_us, 0u);
    EXPECT_EQ(coolant_high.reserved[0], 0u);
    EXPECT_EQ(coolant_high.reserved[1], 0u);

    const ecuio_condition_term_t hot = term_of(s, 0, 0);
    EXPECT_EQ(hot.kind, ECUIO_TERM_SIGNAL);
    EXPECT_EQ(hot.test, ECUIO_TERM_ABOVE);
    EXPECT_EQ(hot.level, 110.0);
    EXPECT_EQ(hot.clear, 105.0);
    EXPECT_EQ(hot.max_age_us, 10000000u);
    EXPECT_EQ(hot.flags, ECUIO_TERM_HAS_CLEAR | ECUIO_TERM_HAS_UNIT);
    EXPECT_STREQ(hot.unit, "\xC2\xB0"
                           "C");
    ASSERT_EQ(hot.reference_count, 1u);
    EXPECT_EQ(hot.references[0].kind, ECUIO_REFERENCE_ID);
    EXPECT_EQ(hot.references[0].id, ECUIO_OBD2_SIG_COOLANT_TEMP_ID);
    EXPECT_STREQ(hot.references[0].text, "obd2.coolant");
    EXPECT_EQ(hot.reserved0, 0u);
    EXPECT_EQ(hot.reserved[0], 0u);
    EXPECT_EQ(hot.reserved[1], 0u);

    const ecuio_condition_info_t pull = info_of(s, 1);
    EXPECT_STREQ(pull.key, "pull");
    EXPECT_EQ(pull.flags, 0u) << "all";
    EXPECT_EQ(pull.uses, ECUIO_CONDITION_USE_RECORD | ECUIO_CONDITION_USE_MARK);
    EXPECT_EQ(pull.alert_level, ECUIO_ALERT_NONE);
    EXPECT_EQ(pull.term_count, 2u);
    EXPECT_EQ(pull.for_us, 500000u);
    EXPECT_EQ(pull.before_us, 5000000u);
    EXPECT_EQ(pull.after_us, 5000000u);
    const ecuio_condition_term_t throttle = term_of(s, 1, 0);
    EXPECT_EQ(throttle.level, 80.0);
    EXPECT_EQ(throttle.clear, 70.0);
    EXPECT_EQ(throttle.max_age_us, 0u) << "the default bound";
    EXPECT_STREQ(throttle.unit, "%");
    const ecuio_condition_term_t rpm = term_of(s, 1, 1);
    EXPECT_EQ(rpm.flags, ECUIO_TERM_HAS_UNIT);
    EXPECT_EQ(rpm.clear, 0.0);
    ASSERT_EQ(rpm.reference_count, 2u);
    EXPECT_EQ(rpm.references[0].id, 1001u);
    EXPECT_STREQ(rpm.references[0].text, "id:1001");
    EXPECT_EQ(rpm.references[1].id, 7003u);
    EXPECT_EQ(rpm.references[2].text[0], '\0');

    const ecuio_condition_info_t link = info_of(s, 2);
    EXPECT_EQ(link.uses, ECUIO_CONDITION_USE_RECORD);
    EXPECT_EQ(link.before_us, 10000000u);
    const ecuio_condition_term_t not_ok = term_of(s, 2, 0);
    EXPECT_EQ(not_ok.kind, ECUIO_TERM_LINK);
    EXPECT_EQ(not_ok.test, ECUIO_TERM_LINK_NOT_OK);
    EXPECT_EQ(not_ok.reference_count, 0u);
    EXPECT_EQ(not_ok.flags, 0u);

    const ecuio_condition_info_t slow = info_of(s, 3);
    EXPECT_EQ(slow.uses, ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_MARK |
                             ECUIO_CONDITION_ALERT_BANNER);
    EXPECT_EQ(slow.alert_level, ECUIO_ALERT_WARNING);
    const ecuio_condition_term_t drop = term_of(s, 3, 0);
    EXPECT_EQ(drop.kind, ECUIO_TERM_CHECK);
    EXPECT_EQ(drop.test, ECUIO_CHECK_RATE_DROP);
}

TEST(ConditionFileTest, WhatAFileLeavesOutTakesItsDefault)
{
    Set s = load(when(R"({"signal":["key:gear"],"equals":1})"));
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(ecuio_condition_set_name(s.get()), "");
    ecuio_check_options_t checks;
    std::memset(&checks, 0xFF, sizeof checks);
    ASSERT_EQ(ecuio_condition_set_checks(s.get(), &checks), ECUIO_OK);
    EXPECT_EQ(checks.gap_us, 0u);
    EXPECT_EQ(checks.rate_drop_x100, 0u);
    EXPECT_EQ(checks.settle_us, 0u);

    const ecuio_condition_info_t info = info_of(s, 0);
    EXPECT_EQ(info.flags, 0u) << "enabled, and one term joins as all";
    EXPECT_EQ(info.uses, 0u) << "a condition with no use loads";
    EXPECT_EQ(info.alert_level, ECUIO_ALERT_NONE);
    EXPECT_EQ(info.for_us, 0u);
    EXPECT_EQ(info.clear_for_us, 0u);
    const ecuio_condition_term_t t = term_of(s, 0, 0);
    EXPECT_EQ(t.test, ECUIO_TERM_EQUALS);
    EXPECT_EQ(t.level, 1.0);
    EXPECT_EQ(t.flags, 0u) << "no unit written, no clear";
    EXPECT_STREQ(t.unit, "");
    EXPECT_EQ(t.max_age_us, 0u);
    EXPECT_EQ(t.references[0].kind, ECUIO_REFERENCE_KEY);
    EXPECT_EQ(t.references[0].id, 0u);
    EXPECT_STREQ(t.references[0].text, "key:gear");
    EXPECT_STREQ(t.references[0].text + 4, "gear");
}

TEST(ConditionFileTest, EachKeyReadsAsWritten)
{
    Set s = load(file_of(
        R"({"key":"a","name":"A","enabled":false,"when":{"any":[)" + coolant() +
        R"(,{"signal":["id:7003"],"unit":"RPM","below":500,"clear":600},)"
        R"({"signal":["key:gear"],"differs":0,"unit":"","max_age":0.1},)"
        R"({"link":"hard_failure"}]},"for":600,"clear_for":0.25,)"
        R"("alert":{"level":"warning","sound":false,"banner":false},)"
        R"("record":{},"mark":false},)"
        R"({"key":"b","name":"B","when":{"all":[{"check":"isotp"}]},)"
        R"("record":{"before":60,"after":600}})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_info_t a = info_of(s, 0);
    EXPECT_EQ(a.flags,
              ECUIO_CONDITION_INFO_DISABLED | ECUIO_CONDITION_INFO_ANY);
    EXPECT_EQ(a.uses, ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_RECORD)
        << "an alert may neither sound nor show a banner; mark false is no use";
    EXPECT_EQ(a.term_count, 4u);
    EXPECT_EQ(a.for_us, ECUIO_CONDITION_MAX_FOR_US);
    EXPECT_EQ(a.clear_for_us, 250000u);
    EXPECT_EQ(a.before_us, 0u) << "an empty record records while ON";
    EXPECT_EQ(a.after_us, 0u);

    const ecuio_condition_term_t below = term_of(s, 0, 1);
    EXPECT_EQ(below.test, ECUIO_TERM_BELOW);
    EXPECT_EQ(below.level, 500.0);
    EXPECT_EQ(below.clear, 600.0);
    const ecuio_condition_term_t differs = term_of(s, 0, 2);
    EXPECT_EQ(differs.test, ECUIO_TERM_DIFFERS);
    EXPECT_EQ(differs.flags, ECUIO_TERM_HAS_UNIT) << "an empty unit is written";
    EXPECT_EQ(differs.max_age_us, ECUIO_COMPUTED_MIN_AGE_US);
    EXPECT_EQ(term_of(s, 0, 3).test, ECUIO_TERM_LINK_HARD_FAILURE);

    const ecuio_condition_info_t b = info_of(s, 1);
    EXPECT_EQ(b.flags, 0u);
    EXPECT_EQ(b.before_us, ECUIO_CONDITION_MAX_BEFORE_US);
    EXPECT_EQ(b.after_us, ECUIO_CONDITION_MAX_AFTER_US);
    EXPECT_EQ(term_of(s, 1, 0).test, ECUIO_CHECK_ISOTP);

    /* Each word of a link and a check term */
    const char* const links[] = {"not_ok", "timeout", "request_failed",
                                 "hard_failure"};
    const uint32_t link_tests[] = {
        ECUIO_TERM_LINK_NOT_OK, ECUIO_TERM_LINK_TIMEOUT,
        ECUIO_TERM_LINK_REQUEST_FAILED, ECUIO_TERM_LINK_HARD_FAILURE};
    for (int i = 0; i < 4; ++i) {
        Set l = load(when(std::string(R"({"link":")") + links[i] + "\"}"));
        ASSERT_NE(l, nullptr);
        EXPECT_EQ(term_of(l, 0, 0).test, link_tests[i]) << links[i];
    }
    const char* const checks[] = {"gap", "rate_drop", "late_id", "overflow",
                                  "isotp"};
    const uint32_t check_tests[] = {ECUIO_CHECK_GAP, ECUIO_CHECK_RATE_DROP,
                                    ECUIO_CHECK_LATE_ID, ECUIO_CHECK_OVERFLOW,
                                    ECUIO_CHECK_ISOTP};
    for (int i = 0; i < 5; ++i) {
        Set c = load(when(std::string(R"({"check":")") + checks[i] + "\"}"));
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(term_of(c, 0, 0).test, check_tests[i]) << checks[i];
    }
}

TEST(ConditionFileTest, NotesAndAToolsOwnKeysAreIgnoredEverywhere)
{
    Set s = load(
        R"({"havne_conditions":1,"notes":"n","x-tool":{"a":1},)"
        R"("checks":{"notes":"n","x-a":1,"gap":2},"conditions":[)"
        R"({"key":"c","name":"C","notes":"n","x-a":[],)"
        R"("when":{"notes":"n","any":[{"notes":"n","x-a":1,"link":"timeout"},)"
        R"({"notes":"n","check":"gap"},)"
        R"({"notes":"n","x-a":1,"signal":["id:1"],"equals":0}]},)"
        R"("alert":{"notes":"n","x-a":1,"level":"danger"},)"
        R"("record":{"notes":"n","x-a":1}}]})");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(info_of(s, 0).term_count, 3u);
    EXPECT_EQ(refusal(when(R"({"notes":1,"link":"timeout"})")),
              "line 1, column 75: condition 'c''s when's notes is a number, "
              "not a string");
}

TEST(ConditionFileTest, AnEmptyListIsAFile)
{
    Set s = load(R"({"havne_conditions":1,"conditions":[]})");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(ecuio_condition_set_count(s.get()), 0u);
}

TEST(ConditionFileTest, ADurationRoundsToTheMicrosecond)
{
    Set s = load(when(coolant(R"(,"max_age":0.1000004)"),
                      R"(,"for":0.0000004,"clear_for":1.0000006,)"
                      R"("record":{"before":59.9999999,"after":1e-6})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_info_t info = info_of(s, 0);
    EXPECT_EQ(info.for_us, 0u);
    EXPECT_EQ(info.clear_for_us, 1000001u);
    EXPECT_EQ(info.before_us, 60000000u);
    EXPECT_EQ(info.after_us, 1u);
    EXPECT_EQ(term_of(s, 0, 0).max_age_us, 100000u);

    Set c = load(R"({"havne_conditions":1,"checks":{"gap":0.0000005,)"
                 R"("rate_drop":1.006,"settle":3600},"conditions":[]})");
    ASSERT_NE(c, nullptr);
    ecuio_check_options_t checks{};
    ASSERT_EQ(ecuio_condition_set_checks(c.get(), &checks), ECUIO_OK);
    EXPECT_EQ(checks.gap_us, 1u);
    EXPECT_EQ(checks.rate_drop_x100, 101u);
    EXPECT_EQ(checks.settle_us, uint64_t{ECUIO_CONDITION_MAX_CHECK_US});
}

TEST(ConditionFileTest, AClearAtItsLevelIsTaken)
{
    Set s = load(file_of(
        R"({"key":"a","name":"A","when":)" + coolant(R"(,"clear":100)") + "}," +
        R"({"key":"b","name":"B","when":{"signal":["id:1"],"unit":"V",)"
        R"("below":11.5,"clear":11.5}})"));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(term_of(s, 0, 0).clear, 100.0);
    EXPECT_EQ(term_of(s, 1, 0).clear, 11.5);
    EXPECT_EQ(term_of(s, 1, 0).flags,
              ECUIO_TERM_HAS_CLEAR | ECUIO_TERM_HAS_UNIT);
}

TEST(ConditionFileTest, EveryRefusalSaysWhereAndWhy)
{
    struct Case {
        std::string json;
        const char* detail;
    };
    const std::string c = R"({"key":"c","name":"C","when":{"link":"not_ok"})";
    const std::string eight =
        R"("id:1","id:2","id:3","id:4","id:5","id:6","id:7","id:8")";
    const Case cases[] = {
        /* The file */
        {"", "line 1, column 1: expected a value, found the end of the file"},
        {"[]", "line 1, column 1: the file is a list, not an object"},
        {R"({"conditions":[]})",
         "line 1, column 1: a condition file starts with "
         "\"havne_conditions\""},
        {R"({"name":"T","havne_conditions":1,"conditions":[]})",
         "line 1, column 1: a condition file starts with "
         "\"havne_conditions\""},
        {R"({"havne_conditions":2,"conditions":[]})",
         "line 1, column 21: \"havne_conditions\" is format 2, and this "
         "Havne reads format 1"},
        {R"({"havne_conditions":0,"conditions":[]})",
         "line 1, column 21: \"havne_conditions\" is format 0, and this "
         "Havne reads format 1"},
        {R"({"havne_conditions":1.0,"conditions":[]})",
         "line 1, column 21: \"havne_conditions\" is format ?, and this "
         "Havne reads format 1"},
        {R"({"havne_conditions":"1","conditions":[]})",
         "line 1, column 21: \"havne_conditions\" is a string, not a number"},
        {R"({"havne_conditions":1})",
         "line 1, column 1: the file needs \"conditions\", a list"},
        {R"({"havne_conditions":1,"conditions":{}})",
         "line 1, column 36: \"conditions\" is an object, not a list"},
        {R"({"havne_conditions":1,"conditionz":[]})",
         "line 1, column 23: unknown key 'conditionz' in the file"},
        {R"({"havne_conditions":1,"name":"","conditions":[]})",
         "line 1, column 30: \"name\" is 0 bytes; it takes 1 to 63"},
        {R"({"havne_conditions":1,"name":")" + std::string(64, 'n') +
             R"(","conditions":[]})",

         "line 1, column 30: \"name\" is 64 bytes; it takes 1 to 63"},
        {R"({"havne_conditions":1,"name":7,"conditions":[]})",
         "line 1, column 30: \"name\" is a number, not a string"},
        {R"({"havne_conditions":1,"name":"a\u0000b","conditions":[]})",
         "line 1, column 30: \"name\" holds a NUL character"},
        {R"({"havne_conditions":1,"conditions":[],"conditions":[]})",
         "line 1, column 39: duplicate key 'conditions'"},
        /* checks */
        {R"({"havne_conditions":1,"checks":[],"conditions":[]})",
         "line 1, column 32: \"checks\" is a list, not an object"},
        {R"({"havne_conditions":1,"checks":{"gaps":1},"conditions":[]})",
         "line 1, column 33: unknown key 'gaps' in \"checks\""},
        {R"({"havne_conditions":1,"checks":{"gap":0},"conditions":[]})",
         "line 1, column 39: \"checks\"'s gap takes more than 0, up to 3600 "
         "seconds"},
        {R"({"havne_conditions":1,"checks":{"gap":1e-7},"conditions":[]})",
         "line 1, column 39: \"checks\"'s gap takes more than 0, up to 3600 "
         "seconds"},
        {R"({"havne_conditions":1,"checks":{"gap":3600.1},"conditions":[]})",
         "line 1, column 39: \"checks\"'s gap takes more than 0, up to 3600 "
         "seconds"},
        {R"({"havne_conditions":1,"checks":{"gap":1e300},"conditions":[]})",
         "line 1, column 39: \"checks\"'s gap takes more than 0, up to 3600 "
         "seconds"},
        {R"({"havne_conditions":1,"checks":{"rate_drop":1e300},"conditions":[]})",
         "line 1, column 45: \"checks\"'s rate_drop takes a multiple above 1, "
         "up to 1000, read to hundredths"},
        {R"({"havne_conditions":1,"checks":{"gap":"1"},"conditions":[]})",
         "line 1, column 39: \"checks\"'s gap is a string, not a number"},
        {R"({"havne_conditions":1,"checks":{"settle":-1},"conditions":[]})",
         "line 1, column 42: \"checks\"'s settle takes more than 0, up to "
         "3600 seconds"},
        {R"({"havne_conditions":1,"checks":{"settle":3601},"conditions":[]})",
         "line 1, column 42: \"checks\"'s settle takes more than 0, up to "
         "3600 seconds"},
        {R"({"havne_conditions":1,"checks":{"rate_drop":1},"conditions":[]})",
         "line 1, column 45: \"checks\"'s rate_drop takes a multiple above "
         "1, up to 1000, read to hundredths"},
        {R"({"havne_conditions":1,"checks":{"rate_drop":1.004},"conditions":[]})",
         "line 1, column 45: \"checks\"'s rate_drop takes a multiple above "
         "1, up to 1000, read to hundredths"},
        {R"({"havne_conditions":1,"checks":{"rate_drop":1000.01},"conditions":[]})",
         "line 1, column 45: \"checks\"'s rate_drop takes a multiple above "
         "1, up to 1000, read to hundredths"},
        {R"({"havne_conditions":1,"checks":{"rate_drop":true},"conditions":[]})",
         "line 1, column 45: \"checks\"'s rate_drop is true or false, not a "
         "number"},
        /* A condition */
        {file_of("7"),
         "line 1, column 37: a condition is a number, not an object"},
        {file_of(R"({"name":"C","when":{"link":"not_ok"}})"),
         "line 1, column 37: a condition needs a \"key\""},
        {file_of(R"({"key":7})"),
         "line 1, column 44: a condition's key is a number, not a string"},
        {file_of(R"({"key":""})"),
         "line 1, column 44: a condition's key is 0 bytes; it takes 1 to 31"},
        {file_of(R"({"key":")" + std::string(32, 'k') + R"("})"),
         "line 1, column 44: a condition's key is 32 bytes; it takes 1 to 31"},
        {file_of(R"({"key":"Pull"})"),
         "line 1, column 44: key 'Pull' takes 1 to 31 of a-z, 0-9 and _"},
        {file_of(R"({"key":"a-b"})"),
         "line 1, column 44: key 'a-b' takes 1 to 31 of a-z, 0-9 and _"},
        {file_of(c + "}," + c + "}"),
         "line 1, column 92: key 'c' is given twice"},
        {file_of(R"({"key":"c","when":{"link":"not_ok"}})"),
         "line 1, column 37: condition 'c' needs a \"name\""},
        {file_of(R"({"key":"c","name":"","when":{"link":"not_ok"}})"),
         "line 1, column 55: condition 'c''s name is 0 bytes; it takes 1 to "
         "40"},
        {file_of(R"({"key":"c","name":")" + std::string(41, 'n') +
                 R"(","when":{"link":"not_ok"}})"),

         "line 1, column 55: condition 'c''s name is 41 bytes; it takes 1 "
         "to 40"},
        {file_of(R"({"key":"c","name":"=C","when":{"link":"not_ok"}})"),
         "line 1, column 55: condition 'c''s name '=C' starts as a "
         "spreadsheet formula would (= + - @, a tab or a CR), which a "
         "recording's CSV can't hold"},
        {file_of(R"({"key":"c","name":"@C","when":{"link":"not_ok"}})"),
         "line 1, column 55: condition 'c''s name '@C' starts as a "
         "spreadsheet formula would (= + - @, a tab or a CR), which a "
         "recording's CSV can't hold"},
        {file_of(c + R"(},{"key":"d","name":"C","when":{"link":"not_ok"}})"),

         "line 1, column 103: condition 'd' has the name 'C', which "
         "condition 'c' has too: a name names one condition"},
        {file_of(R"({"key":"c","name":"C"})"),
         "line 1, column 37: condition 'c' needs a \"when\""},
        {file_of(c + R"(,"whn":1})"),
         "line 1, column 84: unknown key 'whn' in condition 'c'"},
        {file_of(c + R"(,"enabled":1})"),
         "line 1, column 94: condition 'c''s enabled is a number, not true "
         "or false"},
        {file_of(c + R"(,"for":-0.1})"),
         "line 1, column 90: condition 'c''s for takes 0 to 600 seconds"},
        {file_of(c + R"(,"for":600.1})"),
         "line 1, column 90: condition 'c''s for takes 0 to 600 seconds"},
        {file_of(c + R"(,"for":"2"})"),
         "line 1, column 90: condition 'c''s for is a string, not a number"},
        {file_of(c + R"(,"clear_for":601})"),
         "line 1, column 96: condition 'c''s clear_for takes 0 to 600 "
         "seconds"},
        {file_of(c + R"(,"clear_for":-1})"),
         "line 1, column 96: condition 'c''s clear_for takes 0 to 600 "
         "seconds"},
        /* The uses */
        {file_of(c + R"(,"alert":true})"),
         "line 1, column 92: condition 'c''s alert is true or false, not an "
         "object"},
        {file_of(c + R"(,"alert":{}})"),
         "line 1, column 92: condition 'c''s alert needs a \"level\": "
         "warning or danger"},
        {file_of(c + R"(,"alert":{"sound":true}})"),
         "line 1, column 92: condition 'c''s alert needs a \"level\": "
         "warning or danger"},
        {file_of(c + R"(,"alert":{"banner":true}})"),
         "line 1, column 92: condition 'c''s alert needs a \"level\": "
         "warning or danger"},
        {file_of(c + R"(,"alert":{"level":"high"}})"),
         "line 1, column 101: condition 'c''s alert's level 'high' isn't "
         "warning or danger"},
        {file_of(c + R"(,"alert":{"level":2}})"),
         "line 1, column 101: condition 'c''s alert's level is a number, "
         "not a string"},
        {file_of(c + R"(,"alert":{"level":"danger","sound":"yes"}})"),
         "line 1, column 118: condition 'c''s alert's sound is a string, "
         "not true or false"},
        {file_of(c + R"(,"alert":{"level":"danger","banner":1}})"),
         "line 1, column 119: condition 'c''s alert's banner is a number, "
         "not true or false"},
        {file_of(c + R"(,"alert":{"level":"danger","color":"red"}})"),
         "line 1, column 110: unknown key 'color' in condition 'c''s alert"},
        {file_of(c + R"(,"record":true})"),
         "line 1, column 93: condition 'c''s record is true or false, not "
         "an object"},
        {file_of(c + R"(,"record":{"before":60.1}})"),
         "line 1, column 103: condition 'c''s record's before takes 0 to 60 "
         "seconds"},
        {file_of(c + R"(,"record":{"before":-1}})"),
         "line 1, column 103: condition 'c''s record's before takes 0 to 60 "
         "seconds"},
        {file_of(c + R"(,"record":{"after":600.5}})"),
         "line 1, column 102: condition 'c''s record's after takes 0 to 600 "
         "seconds"},
        {file_of(c + R"(,"record":{"after":"5"}})"),
         "line 1, column 102: condition 'c''s record's after is a string, "
         "not a number"},
        {file_of(c + R"(,"record":{"during":1}})"),
         "line 1, column 94: unknown key 'during' in condition 'c''s record"},
        {file_of(c + R"(,"mark":"yes"})"),
         "line 1, column 91: condition 'c''s mark is a string, not true or "
         "false"},
        /* when and its join */
        {when("[]"),
         "line 1, column 66: condition 'c''s when is a list, not an object"},
        {when("{}"),
         "line 1, column 66: condition 'c''s when needs one of \"signal\", "
         "\"link\" and \"check\""},
        {when(R"({"all":[{"link":"not_ok"}],"any":[{"link":"not_ok"}]})"),
         "line 1, column 99: condition 'c''s when takes \"all\" or \"any\", "
         "not both"},
        {when(R"({"all":{}})"),
         "line 1, column 73: condition 'c''s when's all is an object, not a "
         "list"},
        {when(R"({"all":[]})"),
         "line 1, column 73: condition 'c''s when's all lists 0 terms; it "
         "takes 1 to 4"},
        {when(R"({"any":[]})"),
         "line 1, column 73: condition 'c''s when's any lists 0 terms; it "
         "takes 1 to 4"},
        {when(R"({"any":[{"link":"not_ok"},{"link":"timeout"},{"check":"gap"},)"
              R"({"check":"isotp"},{"check":"overflow"}]})"),

         "line 1, column 73: condition 'c''s when's any lists 5 terms; it "
         "takes 1 to 4"},
        {when(R"({"all":[{"link":"not_ok"}],"link":"not_ok"})"),
         "line 1, column 93: unknown key 'link' in condition 'c''s when"},
        {when(R"({"all":[{"any":[{"link":"not_ok"}]}]})"),
         "line 1, column 81: condition 'c''s term 1 holds \"any\": terms "
         "join one level deep"},
        {when(R"({"any":[{"link":"not_ok"},{"all":[]}]})"),
         "line 1, column 99: condition 'c''s term 2 holds \"all\": terms "
         "join one level deep"},
        {when(R"({"all":[7]})"),
         "line 1, column 74: condition 'c''s term 1 is a number, not an "
         "object"},
        /* A term's kind */
        {when(R"({"link":"not_ok","check":"gap"})"),
         "line 1, column 66: condition 'c''s when takes one of \"signal\", "
         "\"link\" and \"check\", not two"},
        {when(R"({"signal":["id:1"],"equals":1,"link":"not_ok"})"),
         "line 1, column 66: condition 'c''s when takes one of \"signal\", "
         "\"link\" and \"check\", not two"},
        {when(R"({"above":1,"unit":"V"})"),
         "line 1, column 66: condition 'c''s when needs one of \"signal\", "
         "\"link\" and \"check\""},
        {when(R"({"link":"down"})"),
         "line 1, column 74: condition 'c''s when's link 'down' isn't "
         "not_ok, timeout, request_failed or hard_failure"},
        {when(R"({"link":1})"),
         "line 1, column 74: condition 'c''s when's link is a number, not a "
         "string"},
        {when(R"({"link":"not_ok","unit":"V"})"),
         "line 1, column 83: unknown key 'unit' in condition 'c''s when"},
        {when(R"({"link":"not_ok","max_age":1})"),
         "line 1, column 83: unknown key 'max_age' in condition 'c''s when"},
        {when(R"({"check":"link"})"),
         "line 1, column 75: condition 'c''s when's check 'link' isn't gap, "
         "rate_drop, late_id, overflow or isotp"},
        {when(R"({"check":["gap"]})"),
         "line 1, column 75: condition 'c''s when's check is a list, not a "
         "string"},
        {when(R"({"check":"gap","clear":1})"),
         "line 1, column 81: unknown key 'clear' in condition 'c''s when"},
        {when(R"({"any":[{"link":"not_ok"},{"check":"asleep"}]})"),
         "line 1, column 101: condition 'c''s term 2's check 'asleep' isn't "
         "gap, rate_drop, late_id, overflow or isotp"},
        /* A signal term's references */
        {when(R"({"signal":"id:1","equals":1})"),
         "line 1, column 76: condition 'c''s when's signal is a string, not "
         "a list"},
        {when(R"({"signal":[],"equals":1})"),
         "line 1, column 76: condition 'c''s when's signal lists 0 "
         "references; it takes 1 to 8"},
        {when(R"({"signal":[)" + eight + R"(,"id:9"],"equals":1})"),
         "line 1, column 76: condition 'c''s when's signal lists 9 "
         "references; it takes 1 to 8"},
        {when(R"({"signal":[7],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference is a number, "
         "not a string"},
        {when(R"({"signal":[""],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference is 0 bytes; "
         "it takes 1 to 39"},
        {when(R"({"signal":["key:)" + std::string(36, 'k') +
              R"("],"equals":1})"),

         "line 1, column 77: condition 'c''s when's reference is 40 bytes; "
         "it takes 1 to 39"},
        {when(R"({"signal":["rpm"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'rpm' isn't "
         "id:<number>, key:<key> or obd2.<name>"},
        {when(R"({"signal":["did:7E8:1E1C"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference "
         "'did:7E8:1E1C' isn't id:<number>, key:<key> or obd2.<name>"},
        {when(R"({"signal":["ID:1"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'ID:1' isn't "
         "id:<number>, key:<key> or obd2.<name>"},
        {when(R"({"signal":["id:"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:' takes a "
         "whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:65536"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:65536' "
         "takes a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:01"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:01' takes "
         "a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:-1"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:-1' takes "
         "a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:+1"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:+1' takes "
         "a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:1.0"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:1.0' "
         "takes a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:0x10"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:0x10' "
         "takes a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["id:123456"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'id:123456' "
         "takes a whole number from 0 to 65535 after id:"},
        {when(R"({"signal":["key:"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'key:' takes "
         "1 to 31 of a-z, 0-9 and _ after key:"},
        {when(R"({"signal":["key:Boost"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'key:Boost' "
         "takes 1 to 31 of a-z, 0-9 and _ after key:"},
        {when(R"({"signal":["key:)" + std::string(32, 'k') +
              R"("],"equals":1})"),

         "line 1, column 77: condition 'c''s when's reference "
         "'key:kkkkkkkkkkkkkkkkkkkkkkkkkkkk...' takes 1 to 31 of a-z, 0-9 "
         "and _ after key:"},
        {when(R"({"signal":["obd2.coolnt"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'obd2.coolnt' "
         "isn't one of obd2's keys"},
        {when(R"({"signal":["obd2.pid_zz"],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'obd2.pid_zz' "
         "isn't one of obd2's keys"},
        {when(R"({"signal":["obd2."],"equals":1})"),
         "line 1, column 77: condition 'c''s when's reference 'obd2.' isn't "
         "one of obd2's keys"},
        {when(R"({"signal":["id:1","id:1"],"equals":1})"),
         "line 1, column 84: condition 'c''s when's reference 'id:1' names "
         "the signal 'id:1' named before it"},
        {when(R"({"signal":["key:a","id:2","key:a"],"equals":1})"),
         "line 1, column 92: condition 'c''s when's reference 'key:a' names "
         "the signal 'key:a' named before it"},
        {when(R"({"signal":["obd2.coolant","id:7002"],"equals":1})"),
         "line 1, column 92: condition 'c''s when's reference 'id:7002' "
         "names the signal 'obd2.coolant' named before it"},
        {when(R"({"signal":["id:7003","obd2.rpm"],"equals":1})"),
         "line 1, column 87: condition 'c''s when's reference 'obd2.rpm' "
         "names the signal 'id:7003' named before it"},
        /* Its comparison */
        {when(R"({"signal":["id:1"]})"),
         "line 1, column 66: condition 'c''s when needs one of \"above\", "
         "\"below\", \"equals\" and \"differs\""},
        {when(R"({"signal":["id:1"],"unit":"V","above":1,"below":2})"),
         "line 1, column 114: condition 'c''s when takes one of \"above\", "
         "\"below\", \"equals\" and \"differs\", not two"},
        {when(R"({"signal":["id:1"],"equals":1,"differs":2})"),
         "line 1, column 106: condition 'c''s when takes one of \"above\", "
         "\"below\", \"equals\" and \"differs\", not two"},
        {when(R"({"signal":["id:1"],"unit":"V","above":"1"})"),
         "line 1, column 104: condition 'c''s when's above is a string, not "
         "a number"},
        {when(R"({"signal":["id:1"],"equals":null})"),
         "line 1, column 94: condition 'c''s when's equals is null, not a "
         "number"},
        {when(R"({"signal":["id:1"],"above":1})"),
         "line 1, column 66: condition 'c''s when compares with a level, so "
         "it needs the level's \"unit\" (\"\" for a number that has none)"},
        {when(R"({"signal":["id:1"],"below":1})"),
         "line 1, column 66: condition 'c''s when compares with a level, so "
         "it needs the level's \"unit\" (\"\" for a number that has none)"},
        {when(R"({"signal":["id:1"],"unit":7,"above":1})"),
         "line 1, column 92: condition 'c''s when's unit is a number, not a "
         "string"},
        {when(R"({"signal":["id:1"],"unit":"0123456789abcdef","above":1})"),

         "line 1, column 92: condition 'c''s when's unit is 16 bytes; it "
         "takes 0 to 15"},
        {when(R"({"signal":["id:1"],"unit":"a\u0000","above":1})"),
         "line 1, column 92: condition 'c''s when's unit holds a NUL "
         "character"},
        {when(R"({"signal":["id:1"],"unit":"V","abov":1})"),
         "line 1, column 96: unknown key 'abov' in condition 'c''s when"},
        {when(R"({"signal":["id:1"],"equals":1,"clear":0})"),
         "line 1, column 104: condition 'c''s when's clear goes with above "
         "or below"},
        {when(R"({"signal":["id:1"],"differs":1,"clear":0})"),
         "line 1, column 105: condition 'c''s when's clear goes with above "
         "or below"},
        {when(R"({"signal":["id:1"],"unit":"V","above":10,"clear":10.5})"),

         "line 1, column 115: condition 'c''s when's clear 10.5 is above "
         "its level 10: it lies on the other side"},
        {when(R"({"signal":["id:1"],"unit":"V","below":10,"clear":9.5})"),

         "line 1, column 115: condition 'c''s when's clear 9.5 is below its "
         "level 10: it lies on the other side"},
        {when(R"({"signal":["id:1"],"unit":"V","above":10,"clear":"9"})"),

         "line 1, column 115: condition 'c''s when's clear is a string, not "
         "a number"},
        {when(R"({"signal":["id:1"],"equals":1,"max_age":0.09})"),
         "line 1, column 106: condition 'c''s when's max_age takes 0.1 to "
         "60 seconds"},
        {when(R"({"signal":["id:1"],"equals":1,"max_age":60.5})"),
         "line 1, column 106: condition 'c''s when's max_age takes 0.1 to "
         "60 seconds"},
        {when(R"({"signal":["id:1"],"equals":1,"max_age":"1"})"),
         "line 1, column 106: condition 'c''s when's max_age is a string, "
         "not a number"},
        {when(R"({"all":[{"link":"timeout"},{"signal":["id:1"],"equals":1,)"
              R"("for":1}]})"),

         "line 1, column 123: unknown key 'for' in condition 'c''s term 2"},
    };
    for (const Case& k : cases) {
        EXPECT_EQ(refusal(k.json), k.detail) << k.json;
    }
}
