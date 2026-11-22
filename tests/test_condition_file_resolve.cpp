// File/layer: tests/test_condition_file_resolve.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ConditionFileTest (test_signals)

#include "test_condition_file_support.h"

namespace {

/* A fixture's bytes, by its path under tests/fixtures */
std::string fixture(const std::filesystem::path& relative)
{
    std::ifstream in(std::filesystem::path(ECUIO_TEST_FIXTURES) / relative,
                     std::ios::binary);
    EXPECT_TRUE(in.good()) << relative;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

ecuio_condition_binding_t resolve(const Set& s, uint32_t i,
                                  const ecuio_signal_set_t* signals,
                                  const ecuio_signal_file_t* file = nullptr)
{
    ecuio_condition_binding_t b{};
    EXPECT_EQ(ecuio_condition_set_resolve(s.get(), i, signals, file, &b),
              ECUIO_OK);
    return b;
}

/* obd2's def for a PID's first field */
const ecuio_signal_def_t& obd2_pid(uint32_t pid)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_length == 2 && d.mux_offset == 0 &&
            d.mux_key == (0x4100u | pid)) {
            return d;
        }
    }
    ADD_FAILURE() << "obd2 has no PID " << pid;
    return set->defs[0];
}

} // namespace

/* The fixtures are the fuzz target's seeds (fuzz/seeds.py) and the
 * CLI's goldens' inputs: each loads, but the one written to be refused */
TEST(ConditionFileTest, EveryFixtureLoadsButTheOneWrittenToBeRefused)
{
    namespace fs = std::filesystem;
    int loaded = 0;
    int refused = 0;
    for (const fs::directory_entry& e :
         fs::directory_iterator(fs::path(ECUIO_TEST_FIXTURES) / "conditions")) {
        const std::string name = e.path().filename().string();
        const std::string json = fixture(fs::path("conditions") / name);
        ecuio_status_t st = ECUIO_OK;
        Set s = load(json, &st);
        if (name == "refused.json") {
            EXPECT_EQ(st, ECUIO_ERR_INVALID_ARGUMENT) << name;
            ++refused;
        } else {
            EXPECT_EQ(st, ECUIO_OK)
                << name << ": " << ecuio_last_error_detail();
            ++loaded;
        }
    }
    EXPECT_GE(loaded, 3);
    EXPECT_EQ(refused, 1);
}

/* The seed of every key reaches every part of the loader and the resolve:
 * both joins, the three term kinds, the four comparisons, every use, and
 * against its signal file a reference of each form */
TEST(ConditionFileTest, TheFixtureOfEveryKeyHoldsEachOne)
{
    Set s = load(fixture("conditions/every-key.json"));
    ASSERT_NE(s, nullptr);
    File f = signals(fixture("signals/extras.json"));
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(ecuio_condition_set_count(s.get()), 2u);
    ecuio_check_options_t checks{};
    ASSERT_EQ(ecuio_condition_set_checks(s.get(), &checks), ECUIO_OK);
    EXPECT_EQ(checks.gap_us, 500000u);
    EXPECT_EQ(checks.rate_drop_x100, 250u);
    EXPECT_EQ(checks.settle_us, 3000000u);

    const ecuio_condition_info_t a = info_of(s, 0);
    EXPECT_EQ(a.flags,
              ECUIO_CONDITION_INFO_DISABLED | ECUIO_CONDITION_INFO_ANY);
    EXPECT_EQ(a.uses, ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_RECORD |
                          ECUIO_CONDITION_USE_MARK |
                          ECUIO_CONDITION_ALERT_SOUND);
    EXPECT_EQ(a.for_us, ECUIO_CONDITION_MAX_FOR_US);
    EXPECT_EQ(a.clear_for_us, 1u);
    EXPECT_EQ(a.before_us, ECUIO_CONDITION_MAX_BEFORE_US);
    EXPECT_EQ(a.after_us, ECUIO_CONDITION_MAX_AFTER_US);
    ASSERT_EQ(a.term_count, 4u);
    const uint32_t tests[] = {ECUIO_TERM_BELOW, ECUIO_TERM_EQUALS,
                              ECUIO_TERM_DIFFERS, ECUIO_TERM_ABOVE};
    for (uint32_t k = 0; k < 4; ++k) {
        EXPECT_EQ(term_of(s, 0, k).test, tests[k]) << k;
    }
    const ecuio_condition_term_t first = term_of(s, 0, 0);
    ASSERT_EQ(first.reference_count, 3u);
    EXPECT_EQ(first.references[0].kind, ECUIO_REFERENCE_KEY);
    EXPECT_EQ(first.references[1].id, uint32_t{ECUIO_MOCK_SIG_SPEED_ID});
    EXPECT_EQ(first.references[2].id, uint32_t{ECUIO_OBD2_SIG_SPEED_ID});

    /* Against the file each term names a signal, and the text one is why
     * the condition can't be judged once it is switched on */
    const ecuio_condition_binding_t b =
        resolve(s, 0, ecuio_signal_file_set(f.get()), f.get());
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_DISABLED);
    for (uint32_t k = 0; k < 4; ++k) {
        EXPECT_NE(b.signal_id[k], UINT32_MAX) << k;
        EXPECT_EQ(b.reference[k], 0u) << k;
    }
    /* With no file the keys name nothing: the obd2 references are taken */
    const ecuio_condition_binding_t base = resolve(s, 0, profile("obd2"));
    EXPECT_EQ(base.reference[0], 2u);
    EXPECT_EQ(base.signal_id[0], uint32_t{ECUIO_OBD2_SIG_SPEED_ID});
    EXPECT_EQ(base.reference[2], 1u);
    EXPECT_EQ(base.signal_id[1], UINT32_MAX);

    const ecuio_condition_info_t other = info_of(s, 1);
    EXPECT_EQ(other.flags, 0u);
    EXPECT_EQ(term_of(s, 1, 0).kind, ECUIO_TERM_LINK);
    EXPECT_EQ(term_of(s, 1, 1).kind, ECUIO_TERM_CHECK);
}

TEST(ConditionFileTest, ARefusalOnALaterLineNamesIt)
{
    EXPECT_EQ(
        refusal("{\n  \"havne_conditions\": 1,\n  \"conditions\": [\n"
                "    { \"key\": \"pull\", \"name\": \"Pull\",\n"
                "      \"when\": { \"signal\": [\"obd2.rpm\"],\n"
                "                \"abov\": 2500 } }\n  ]\n}\n"),
        "line 6, column 17: unknown key 'abov' in condition 'pull''s when");
    EXPECT_EQ(refusal("{\"havne_conditions\":1,\n\"conditions\":[],}"),
              "line 2, column 17: a trailing comma before '}'");
}

TEST(ConditionFileTest, NullsAndTheLoadersBounds)
{
    ecuio_condition_set_t* out = reinterpret_cast<ecuio_condition_set_t*>(1);
    EXPECT_EQ(ecuio_condition_set_load("{}", 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_load(nullptr, 2, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    out = reinterpret_cast<ecuio_condition_set_t*>(1);
    EXPECT_EQ(ecuio_condition_set_load(nullptr, 0, &out),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "no bytes at all is a file refused, not a NULL";
    EXPECT_EQ(out, nullptr);
    ecuio_condition_set_free(nullptr);
    EXPECT_EQ(ecuio_condition_set_count(nullptr), 0u);
    EXPECT_STREQ(ecuio_condition_set_name(nullptr), "");

    /* The byte bound: a file of exactly the bound loads, one more doesn't */
    const std::string head = R"({"havne_conditions":1,"notes":")";
    const std::string tail = R"(","conditions":[]})";
    const std::string most =
        head +
        std::string(ECUIO_CONDITION_FILE_MAX_BYTES - head.size() - tail.size(),
                    'n') +
        tail;
    ASSERT_EQ(most.size(), size_t{ECUIO_CONDITION_FILE_MAX_BYTES});
    EXPECT_NE(load(most), nullptr);
    ecuio_status_t st = ECUIO_OK;
    load(head + "n" + most.substr(head.size()), &st);
    EXPECT_EQ(st, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("262144"),
              std::string::npos)
        << ecuio_last_error_detail();

    /* The most conditions, and one more */
    std::string list;
    for (uint32_t i = 0; i < ECUIO_CONDITION_MAX_CONDITIONS; ++i) {
        const std::string n = std::to_string(i);
        list += std::string(i == 0 ? "" : ",") + R"({"key":"c)" + n +
                R"(","name":"C)" + n + R"(","when":{"link":"not_ok"}})";
    }
    Set full = load(file_of(list));
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(ecuio_condition_set_count(full.get()),
              uint32_t{ECUIO_CONDITION_MAX_CONDITIONS});
    EXPECT_STREQ(info_of(full, 63).key, "c63");
    EXPECT_EQ(
        refusal(file_of(list +
                        R"(,{"key":"x","name":"X","when":{"link":"not_ok"}})")),
        "line 1, column 36: \"conditions\" lists 65 conditions; a file "
        "takes at most 64");

    /* The most references, the longest key, name and unit */
    const std::string key31(31, 'k');
    const std::string name40(40, 'n');
    Set wide =
        load(file_of(R"({"key":")" + key31 + R"(","name":")" + name40 +
                     R"(","when":{"signal":["id:0","id:65535","key:)" + key31 +
                     R"(","id:4","id:5","id:6","id:7","obd2.module_voltage"],)"
                     R"("unit":"0123456789abcde","above":1}})"));
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(info_of(wide, 0).key, key31);
    EXPECT_EQ(info_of(wide, 0).name, name40);
    const ecuio_condition_term_t t = term_of(wide, 0, 0);
    ASSERT_EQ(t.reference_count, uint32_t{ECUIO_CONDITION_MAX_REFERENCES});
    EXPECT_EQ(t.references[0].id, 0u);
    EXPECT_EQ(t.references[1].id, 65535u);
    EXPECT_EQ(t.references[2].text, "key:" + key31);
    EXPECT_EQ(t.references[2].text[sizeof t.references[2].text - 1], '\0');
    EXPECT_STREQ(t.references[7].text, "obd2.module_voltage");
    EXPECT_STREQ(t.unit, "0123456789abcde");
}

TEST(ConditionFileTest, TheGettersRefuseWhatTheyMustAndLeaveOutAlone)
{
    Set s = load(kExample);
    ASSERT_NE(s, nullptr);
    ecuio_condition_info_t info;
    std::memset(&info, 0x5A, sizeof info);
    const ecuio_condition_info_t info_before = info;
    EXPECT_EQ(ecuio_condition_set_get(nullptr, 0, &info),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_get(s.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_get(s.get(), 4, &info),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&info, &info_before, sizeof info), 0);

    ecuio_condition_term_t term;
    std::memset(&term, 0x5A, sizeof term);
    const ecuio_condition_term_t term_before = term;
    EXPECT_EQ(ecuio_condition_set_get_term(nullptr, 0, 0, &term),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), 4, 0, &term),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), 0, 1, &term),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a term past the condition's own count";
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), 1, 2, &term),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&term, &term_before, sizeof term), 0);
    EXPECT_EQ(ecuio_condition_set_get_term(s.get(), 1, 1, &term), ECUIO_OK);

    ecuio_check_options_t checks;
    EXPECT_EQ(ecuio_condition_set_checks(nullptr, &checks),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_checks(s.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_condition_binding_t b;
    std::memset(&b, 0x5A, sizeof b);
    const ecuio_condition_binding_t b_before = b;
    const ecuio_signal_set_t* obd2 = profile("obd2");
    EXPECT_EQ(ecuio_condition_set_resolve(nullptr, 0, obd2, nullptr, &b),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_resolve(s.get(), 0, obd2, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_condition_set_resolve(s.get(), 4, obd2, nullptr, &b),
              ECUIO_ERR_INVALID_ARGUMENT);
    File f = signals();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(ecuio_condition_set_resolve(s.get(), 0, obd2, f.get(), &b),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a file whose set isn't the one given";
    EXPECT_EQ(ecuio_condition_set_resolve(s.get(), 0, nullptr, f.get(), &b),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&b, &b_before, sizeof b), 0);
}

TEST(ConditionFileTest, TheSetKeepsTheBytesItRead)
{
    const std::string json = std::string(kExample) + "\n \t";
    Set s = load(json);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->bytes, json);
}

TEST(ConditionFileTest, TheExampleResolvesAgainstObd2AndTheMock)
{
    Set s = load(kExample);
    ASSERT_NE(s, nullptr);
    const ecuio_signal_set_t* obd2 = profile("obd2");
    const ecuio_signal_set_t* mock = profile("mock");

    ecuio_condition_binding_t b = resolve(s, 0, obd2);
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(b.why_term, UINT32_MAX);
    EXPECT_EQ(b.term_count, 1u);
    EXPECT_EQ(b.reserved0, 0u);
    EXPECT_EQ(b.reference[0], 0u);
    EXPECT_EQ(b.signal_id[0], uint32_t{ECUIO_OBD2_SIG_COOLANT_TEMP_ID});
    for (int i = 1; i < 4; ++i) {
        EXPECT_EQ(b.reference[i], UINT32_MAX) << i;
        EXPECT_EQ(b.signal_id[i], UINT32_MAX) << i;
    }
    EXPECT_EQ(b.reserved[0], 0u);
    EXPECT_EQ(b.reserved[1], 0u);

    /* The second of two references is the one obd2 provides, the first
     * the mock's */
    b = resolve(s, 1, obd2);
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(b.term_count, 2u);
    EXPECT_EQ(b.signal_id[0], uint32_t{ECUIO_OBD2_SIG_THROTTLE_ID});
    EXPECT_EQ(b.reference[1], 1u);
    EXPECT_EQ(b.signal_id[1], uint32_t{ECUIO_OBD2_SIG_RPM_ID});

    b = resolve(s, 1, mock);
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NO_SIGNAL)
        << "obd2's throttle isn't the mock's";
    EXPECT_EQ(b.why_term, 0u);
    EXPECT_EQ(b.reference[0], UINT32_MAX);
    EXPECT_EQ(b.signal_id[0], UINT32_MAX);
    EXPECT_EQ(b.reference[1], 0u) << "a later term still says what it took";
    EXPECT_EQ(b.signal_id[1], uint32_t{ECUIO_MOCK_SIG_RPM_ID});

    /* A link and a check term bind to any set, and to none */
    for (const ecuio_signal_set_t* set : {obd2, mock, profile("raw")}) {
        for (uint32_t i : {2u, 3u}) {
            b = resolve(s, i, set);
            EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NONE);
            EXPECT_EQ(b.why_term, UINT32_MAX);
            EXPECT_EQ(b.reference[0], UINT32_MAX);
            EXPECT_EQ(b.signal_id[0], UINT32_MAX);
        }
    }
    b = resolve(s, 0, nullptr);
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NO_SIGNAL)
        << "no set provides no signal";
    EXPECT_EQ(b.why_term, 0u);
}

TEST(ConditionFileTest, ATermBindsToANumberInItsOwnUnit)
{
    File f = signals();
    ASSERT_NE(f, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const auto why = [&](const std::string& term, uint32_t* id = nullptr) {
        Set s = load(when(term));
        EXPECT_NE(s, nullptr);
        const ecuio_condition_binding_t b = resolve(s, 0, set, f.get());
        if (id != nullptr) {
            *id = b.signal_id[0];
        }
        EXPECT_EQ(b.why_term,
                  b.why == ECUIO_CONDITION_WHY_NONE ? UINT32_MAX : 0u);
        return b.why;
    };
    const auto id_of = [&](const char* key) {
        return uint32_t{ecuio_signal_file_find(f.get(), key)->id};
    };

    uint32_t id = 0;
    EXPECT_EQ(
        why(R"({"signal":["key:oil_pressure"],"unit":"psi","below":10})", &id),
        ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(id, id_of("oil_pressure"));

    /* The unit, byte for byte; the def is still named */
    EXPECT_EQ(
        why(R"({"signal":["key:oil_pressure"],"unit":"kPa","below":10})", &id),
        ECUIO_CONDITION_WHY_UNIT);
    EXPECT_EQ(id, id_of("oil_pressure"));
    EXPECT_EQ(why(R"({"signal":["key:oil_pressure"],"unit":"PSI","below":10})"),
              ECUIO_CONDITION_WHY_UNIT);
    EXPECT_EQ(
        why(R"({"signal":["key:oil_pressure"],"unit":"psi ","below":10})"),
        ECUIO_CONDITION_WHY_UNIT);
    EXPECT_EQ(why(R"({"signal":["key:oil_pressure"],"unit":"","below":10})"),
              ECUIO_CONDITION_WHY_UNIT);
    EXPECT_EQ(why(R"({"signal":["key:oil_pressure"],"equals":10})"),
              ECUIO_CONDITION_WHY_UNIT)
        << "equals with no unit reads as the unit \"\"";
    EXPECT_EQ(
        why(R"({"signal":["key:oil_pressure"],"unit":"psi","equals":10})"),
        ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(why(R"({"signal":["obd2.coolant"],"unit":"C","above":100})"),
              ECUIO_CONDITION_WHY_UNIT);

    /* A number with no unit takes a level under the unit "" */
    EXPECT_EQ(why(R"({"signal":["key:count"],"unit":"","above":3})"),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(why(R"({"signal":["key:count"],"differs":3})"),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(why(R"({"signal":["key:count"],"unit":"psi","above":3})"),
              ECUIO_CONDITION_WHY_UNIT);

    /* An enumeration has no order; text has nothing to compare */
    EXPECT_EQ(why(R"({"signal":["key:gear"],"equals":1})"),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(why(R"({"signal":["key:gear"],"differs":0})"),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(why(R"({"signal":["key:gear"],"unit":"","above":1})", &id),
              ECUIO_CONDITION_WHY_ENUM_ORDER);
    EXPECT_EQ(id, id_of("gear"));
    EXPECT_EQ(why(R"({"signal":["key:gear"],"unit":"","below":1})"),
              ECUIO_CONDITION_WHY_ENUM_ORDER);
    EXPECT_EQ(why(R"({"signal":["key:gear"],"unit":"psi","above":1})"),
              ECUIO_CONDITION_WHY_ENUM_ORDER)
        << "the kind is judged before the unit";
    EXPECT_EQ(why(R"({"signal":["key:part"],"equals":1})"),
              ECUIO_CONDITION_WHY_NOT_A_NUMBER);
    EXPECT_EQ(why(R"({"signal":["key:part"],"unit":"psi","above":1})"),
              ECUIO_CONDITION_WHY_NOT_A_NUMBER);

    /* A computed signal is a number */
    EXPECT_EQ(why(R"({"signal":["key:ratio"],"unit":"","above":1})", &id),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(id, id_of("ratio"));

    /* The first reference the set provides is taken, right or not */
    EXPECT_EQ(
        why(R"({"signal":["key:none","key:part","key:count"],"equals":1})",
            &id),
        ECUIO_CONDITION_WHY_NOT_A_NUMBER);
    EXPECT_EQ(id, id_of("part"));
    EXPECT_EQ(why(R"({"signal":["key:none","id:9"],"equals":1})", &id),
              ECUIO_CONDITION_WHY_NO_SIGNAL);
    EXPECT_EQ(id, UINT32_MAX);

    /* The base's defs by id, through the file's set */
    EXPECT_EQ(why(R"({"signal":["obd2.rpm"],"unit":"RPM","above":6500})", &id),
              ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(id, uint32_t{ECUIO_OBD2_SIG_RPM_ID});

    /* A key names nothing without the file */
    Set keyed = load(when(R"({"signal":["key:count"],"differs":3})"));
    ASSERT_NE(keyed, nullptr);
    EXPECT_EQ(resolve(keyed, 0, set).why, ECUIO_CONDITION_WHY_NO_SIGNAL);
    EXPECT_EQ(resolve(keyed, 0, set, f.get()).why, ECUIO_CONDITION_WHY_NONE);
}

TEST(ConditionFileTest, AnIdNumberedByOrderNamesNothing)
{
    File f = signals();
    ASSERT_NE(f, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_def_t* count = ecuio_signal_file_find(f.get(), "count");
    ASSERT_NE(count, nullptr);
    ASSERT_GE(count->id, ECUIO_USER_SIG_ID_BASE);
    Set s = load(when(R"({"signal":["id:)" + std::to_string(count->id) +
                      R"("],"differs":3})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_binding_t b = resolve(s, 0, set, f.get());
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_NO_SIGNAL)
        << "a file's own signal goes by its key";
    EXPECT_EQ(b.signal_id[0], UINT32_MAX);

    /* The bound is uds-generic's band: its ids are the URI's order */
    ecuio_signal_def_t defs[2] = {};
    for (int i = 0; i < 2; ++i) {
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].message_id = 0x7E8;
    }
    defs[0].id = ECUIO_UDS_GENERIC_SIG_ID_BASE - 1;
    defs[1].id = ECUIO_UDS_GENERIC_SIG_ID_BASE;
    ecuio_signal_set_t two{};
    two.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    two.defs = defs;
    two.count = 2;
    Set below = load(when(R"({"signal":["id:19999"],"differs":3})"));
    Set at = load(when(R"({"signal":["id:20000"],"differs":3})"));
    ASSERT_NE(below, nullptr);
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(resolve(below, 0, &two).why, ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(resolve(below, 0, &two).signal_id[0], 19999u);
    EXPECT_EQ(resolve(at, 0, &two).why, ECUIO_CONDITION_WHY_NO_SIGNAL);
}

/* A signal named as a base def: where the loader lets it replace the def
 * it keeps the base's id and gains a key, and both references reach it */
TEST(ConditionFileTest, ADefNamedAsABaseDefIsFoundByTheBaseIdAndByItsKey)
{
    const ecuio_signal_def_t& map = obd2_pid(0x0B);
    File f = signals(std::string(R"({"havne_signals":2,"name":"Replaces MAP",)"
                                 R"("source":{"name":"t"},"extends":"obd2",)"
                                 R"("ecus":[{"id":"pcm","rx":"7E8"}],)"
                                 R"("signals":[{"key":"map_wide","name":")") +
                     map.name +
                     R"(","read":{"service":"22","number":"D100"},)"
                     R"("field":{"byte":0,"bits":16},"unit":"kPa"}]})");
    ASSERT_NE(f, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const ecuio_signal_def_t* own = ecuio_signal_file_find(f.get(), "map_wide");
    ASSERT_NE(own, nullptr);

    Set s = load(file_of(
        R"({"key":"by_id","name":"By id","when":{"signal":["obd2.map"],)"
        R"("unit":"kPa","above":150}},)"
        R"({"key":"by_key","name":"By key","when":{"signal":["key:map_wide"],)"
        R"("unit":"kPa","above":150}})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_binding_t by_id = resolve(s, 0, set, f.get());
    const ecuio_condition_binding_t by_key = resolve(s, 1, set, f.get());
    EXPECT_EQ(by_id.why, ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(by_id.signal_id[0], uint32_t{map.id});
    EXPECT_EQ(by_key.why, ECUIO_CONDITION_WHY_NONE);
    EXPECT_EQ(by_key.signal_id[0], uint32_t{own->id});
    ASSERT_LT(own->id, ECUIO_USER_SIG_ID_BASE)
        << "a def named as a base one replaces it";
    EXPECT_EQ(own->id, map.id);
    EXPECT_EQ(by_id.signal_id[0], by_key.signal_id[0])
        << "the replacement answers to the base's id and to its key";
    uint32_t with_id = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        with_id += set->defs[i].id == map.id ? 1u : 0u;
    }
    EXPECT_EQ(with_id, 1u);
}

TEST(ConditionFileTest, ADisabledConditionSaysSoAndStillResolves)
{
    Set s = load(file_of(
        R"({"key":"off","name":"Off","enabled":false,"when":{"any":[)"
        R"({"signal":["id:9"],"equals":1},)"
        R"({"signal":["obd2.rpm"],"unit":"RPM","above":1}]}},)"
        R"({"key":"on","name":"On","enabled":true,"when":{"link":"timeout"}})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_binding_t b = resolve(s, 0, profile("obd2"));
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_DISABLED);
    EXPECT_EQ(b.why_term, UINT32_MAX);
    EXPECT_EQ(b.signal_id[0], UINT32_MAX);
    EXPECT_EQ(b.signal_id[1], uint32_t{ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_EQ(info_of(s, 1).flags, 0u);
    EXPECT_EQ(resolve(s, 1, profile("obd2")).why, ECUIO_CONDITION_WHY_NONE);
}

TEST(ConditionFileTest, AnAnyWithOneTermUnboundIsStillUnavailable)
{
    Set s =
        load(when(R"({"any":[{"signal":["obd2.rpm"],"unit":"RPM","above":1},)"
                  R"({"link":"timeout"},)"
                  R"({"signal":["obd2.coolant"],"unit":"F","above":1},)"
                  R"({"signal":["id:9"],"equals":1}]})"));
    ASSERT_NE(s, nullptr);
    const ecuio_condition_binding_t b = resolve(s, 0, profile("obd2"));
    EXPECT_EQ(b.why, ECUIO_CONDITION_WHY_UNIT)
        << "the first term that doesn't bind";
    EXPECT_EQ(b.why_term, 2u);
    EXPECT_EQ(b.term_count, 4u);
    EXPECT_EQ(b.signal_id[2], uint32_t{ECUIO_OBD2_SIG_COOLANT_TEMP_ID});
    EXPECT_EQ(b.reference[3], UINT32_MAX);
}
