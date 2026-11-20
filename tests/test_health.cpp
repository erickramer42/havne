// File/layer: tests/test_health.cpp | test_core, Qt-free, C++17
// Testing: UNIT: HealthTest (test_core)
//
// The health report (ADR-0054's appendix "the health report as built"):
// reports built by hand through health.h, both formats holding the same
// facts (every string the JSON holds is in the text), the VIN and the
// identification texts never in either, the inspection with and without
// a model year, the key values' rule and the since-cleared PIDs, the
// check failing a report that would carry an identity text, and
// ecuio_health_export over a mock:// session and its argument checks.
// Then the report from a recording: a hand-written folder of replies
// (reassembled and as they crossed the bus) rebuilt into the ECU's
// reports, a range, the replay fixture's frames decoded for its values, a
// recording of values alone, and a folder export's scrubbed copy. Then
// the vehicle's baseline: named by its events.log's start, length,
// profile and backend, never its folder; the report's own recording;
// one that can't be read; the reader's forms of its first line. Files
// go to a folder of their own under TEMP/TMPDIR, removed at the end.

#include "test_health_support.h"

namespace {

TEST(HealthTest, BothFormatsHoldTheSameFactsAndNeitherTheVin)
{
    Car car;
    ecuio_health_report_t r{};
    const std::string json = car.write(ECUIO_HEALTH_JSON, &r);
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    const ecuio_json::Value v = parsed(json);
    std::vector<std::string> facts;
    strings_of(v, facts);
    EXPECT_GT(facts.size(), 25u);
    for (const std::string& f : facts) {
        EXPECT_NE(text.find(f), std::string::npos)
            << "the text lacks '" << f << "'\n"
            << text;
    }
    for (const std::string& out : {json, text}) {
        EXPECT_EQ(out.find(kVin), std::string::npos);
        EXPECT_EQ(out.find("FAKECAL"), std::string::npos);
        EXPECT_EQ(out.find("COM3"), std::string::npos);
        EXPECT_NE(out.find("elm327://<address>?profile=obd2&serial=115200"),
                  std::string::npos);
        EXPECT_NE(out.find("2026-11-06T14:22:33Z"), std::string::npos);
        EXPECT_NE(out.find("P0301"), std::string::npos);
        EXPECT_NE(out.find("7F 0A 11"), std::string::npos);
    }
    EXPECT_EQ(v.find("havne_health")->whole, 1);
    EXPECT_TRUE(v.find("vehicle")->find("vin_read")->boolean);
    ecuio_vin_info_t vi{};
    ASSERT_EQ(ecuio_vin_decode(kVin, 2027, &vi), ECUIO_OK);
    EXPECT_EQ(v.find("vehicle")->find("model_year")->whole, vi.model_year);
    EXPECT_EQ(v.find("vehicle")->find("check_digit_matches")->boolean,
              (vi.flags & ECUIO_VIN_CHECK_MATCHES) != 0);
    EXPECT_EQ(std::string(v.find("inspection")->find("verdict")->text),
              "mil_on");
    EXPECT_EQ(v.find("inspection")->find("allowed")->whole, 1);
    EXPECT_EQ(r.ecus, 1u);
    EXPECT_EQ(r.codes, 2u);
    EXPECT_EQ(r.values, 2u);
    EXPECT_EQ(r.since_cleared, 1u);
    EXPECT_EQ(r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 2u);
    EXPECT_EQ(r.flags, ECUIO_HEALTH_VIN_DECODED);

    /* A report still being read says so */
    car.diag.flags &= ~ECUIO_DIAG_COMPLETE;
    EXPECT_EQ(parsed(car.write(ECUIO_HEALTH_JSON))
                  .find("ecus")
                  ->items.at(0)
                  .find("state")
                  ->text,
              "reading");
}

/* The session keys are core's own: the URI keeps them, and a value that
 * isn't plain is dropped with its key */
TEST(HealthTest, TheSessionKeysAreKeptInTheUri)
{
    Car car;
    car.in.uri =
        "elm327://"
        "COM3?profile=uds-generic&did=F40C&session=03&keepalive_ms=2000";
    const std::string kept =
        "elm327://<address>?profile=uds-generic&did=F40C&session=03&"
        "keepalive_ms=2000";
    EXPECT_NE(car.write(ECUIO_HEALTH_JSON).find(kept), std::string::npos);
    EXPECT_NE(car.write(ECUIO_HEALTH_TEXT).find(kept), std::string::npos);
    car.in.uri = "elm327://COM3?profile=uds-generic&session=0 3";
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    EXPECT_NE(text.find("elm327://<address>?profile=uds-generic "),
              std::string::npos)
        << text;
}

/* The since-cleared PIDs sit with their ECU, ranked or not, and aren't
 * repeated among the key values; the key values come in rank order,
 * each with its age, stale past ECUIO_VALUE_STALE_US */
TEST(HealthTest, KeyValuesAreTheRankedSignalsAndTheSinceClearedPidsTheEcus)
{
    Car car;
    const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON));
    const ecuio_json::Value& ecu = v.find("ecus")->items.at(0);
    ASSERT_EQ(ecu.find("since_cleared")->items.size(), 1u);
    const ecuio_json::Value& since = ecu.find("since_cleared")->items[0];
    EXPECT_EQ(since.find("name")->text, pid_def(0x31)->name);
    EXPECT_EQ(since.find("value")->whole, 1234);
    EXPECT_EQ(since.find("age_ms")->whole, 4000);
    EXPECT_TRUE(since.find("stale")->boolean);
    const std::vector<ecuio_json::Value>& keys = v.find("key_values")->items;
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].find("name")->text, ranked_def(1)->name);
    EXPECT_EQ(keys[0].find("age_ms")->whole, 120);
    EXPECT_FALSE(keys[0].find("stale")->boolean);
    EXPECT_EQ(keys[1].find("name")->text, ranked_def(2)->name);
    for (const ecuio_json::Value& k : keys) {
        EXPECT_NE(k.find("name")->text, pid_def(0x31)->name);
    }

    /* Ranked, a since-cleared PID still stays with its ECU */
    std::vector<ecuio_signal_def_t> defs(obd2()->defs,
                                         obd2()->defs + obd2()->count);
    for (ecuio_signal_def_t& d : defs) {
        if (d.id == pid_def(0x31)->id) {
            d.plot_rank = 3;
        }
    }
    ecuio_signal_set_t ranked = *obd2();
    ranked.defs = defs.data();
    car.in.set = &ranked;
    const ecuio_json::Value rv = parsed(car.write(ECUIO_HEALTH_JSON));
    EXPECT_EQ(rv.find("key_values")->items.size(), 2u);
    EXPECT_EQ(rv.find("ecus")->items.at(0).find("since_cleared")->items.size(),
              1u);
    /* Another ECU's reply ID: not this ECU's value */
    car.diag.rx_id = 0x7E9;
    EXPECT_TRUE(parsed(car.write(ECUIO_HEALTH_JSON))
                    .find("ecus")
                    ->items.at(0)
                    .find("since_cleared")
                    ->items.empty());
    car.diag.rx_id = 0x7E8;

    /* A set that ranks nothing says so */
    ecuio_signal_set_t none{};
    none.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    car.in.set = &none;
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    EXPECT_NE(text.find("The profile ranks no signals."), std::string::npos);
    EXPECT_FALSE(parsed(car.write(ECUIO_HEALTH_JSON))
                     .find("profile_ranks_signals")
                     ->boolean);
}

/* Imperial units convert each value as the display does */
TEST(HealthTest, TheValuesAreInTheUnitsAsked)
{
    Car car;
    car.in.units = ECUIO_UNITS_IMPERIAL;
    const ecuio_signal_def_t* distance = pid_def(0x31);
    ASSERT_NE(distance, nullptr);
    ecuio_display_value_t dv{};
    ASSERT_EQ(ecuio_display_convert(distance->unit, distance->decimal_places,
                                    1234.0, ECUIO_UNITS_IMPERIAL, &dv),
              ECUIO_DISPLAY_OK);
    ASSERT_EQ(dv.converted, 1u);
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    EXPECT_NE(text.find("Units: imperial"), std::string::npos);
    EXPECT_NE(text.find(std::string(distance->name) + ": " +
                        ecuio_health::fixed(dv.value, dv.decimal_places) + " " +
                        dv.unit),
              std::string::npos)
        << text;
}

/* An enumeration's value is its label, from the set */
TEST(HealthTest, AnEnumerationIsWrittenAsItsLabel)
{
    Car car;
    ecuio_enum_entry_t entries[2] = {{1, "Open loop"}, {2, "Closed loop"}};
    ecuio_signal_def_t d{};
    d.id = 900;
    d.message_id = 0x7E8;
    d.kind = ECUIO_VALUE_ENUM;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "Fuel System 1");
    d.plot_rank = 1;
    d.table_first = 0;
    d.table_count = 2;
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &d;
    set.enum_count = 2;
    set.enums = entries;
    car.in.set = &set;
    const ecuio_health_value_t value{900, 0, 2.0, 0, 0};
    car.in.values = &value;
    car.in.value_count = 1;
    const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON));
    ASSERT_EQ(v.find("key_values")->items.size(), 1u);
    const ecuio_json::Value& k = v.find("key_values")->items[0];
    EXPECT_EQ(k.find("value")->text, "Closed loop");
    EXPECT_EQ(k.find("unit"), nullptr);
    EXPECT_NE(car.write(ECUIO_HEALTH_TEXT).find("Fuel System 1: Closed loop"),
              std::string::npos);
}

/* No VIN read (or a scrubbed recording): the inspection takes no model
 * year and says which rule that is; two incomplete monitors depend on
 * the year */
TEST(HealthTest, WithNoModelYearTheInspectionSaysWhichRule)
{
    Car car;
    car.id.items = 0;
    car.diag.flags &= ~ECUIO_DIAG_MIL_ON;
    car.diag.monitors[0].flags = 0;
    const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON));
    EXPECT_FALSE(v.find("vehicle")->find("vin_read")->boolean);
    EXPECT_EQ(v.find("vehicle")->find("model_year")->type,
              ecuio_json::Type::null);
    const ecuio_json::Value& insp = *v.find("inspection");
    EXPECT_EQ(insp.find("verdict")->text, "year_needed");
    EXPECT_EQ(insp.find("model_year")->type, ecuio_json::Type::null);
    EXPECT_EQ(insp.find("allowed")->type, ecuio_json::Type::null);
    EXPECT_EQ(insp.find("incomplete")->items.size(), 2u);
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    EXPECT_NE(text.find("No VIN was read."), std::string::npos);
    EXPECT_NE(text.find("No model year: one incomplete monitor passes either "
                        "model year's rule, two pass only 1996-2000's, three "
                        "or more pass neither"),
              std::string::npos)
        << text;
}

/* A value equal to an identification text (an ECU serial read as a
 * number signal) fails the report: nothing is written */
TEST(HealthTest, AnIdentityTextInTheReportFailsIt)
{
    Folder f;
    Car car;
    identity_item(car.id, 0x22, 0xF18C, 2, ECUIO_IDENTITY_ANSWERED, "12345678");
    car.values[0].value = 12345678.0;
    ecuio_health_report_t r{};
    ecuio_scrub::values identity;
    const std::string bytes = car.write(ECUIO_HEALTH_TEXT, &r, &identity);
    ASSERT_NE(bytes.find("12345678"), std::string::npos);
    EXPECT_FALSE(ecuio_health::put(f.utf8("health.txt"), bytes, identity, r));
    EXPECT_EQ(r.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the check found an identification reply's value in the "
              "report: nothing was written");
    EXPECT_FALSE(fs::exists(f.path / "health.txt"));
    EXPECT_FALSE(partial_left(f.path, "health.txt"));

    /* Without the echo it's written, and a second time refused */
    Car plain;
    ecuio_health_report_t ok{};
    ecuio_scrub::values ids;
    const std::string good = plain.write(ECUIO_HEALTH_TEXT, &ok, &ids);
    ASSERT_TRUE(ecuio_health::put(f.utf8("health.txt"), good, ids, ok))
        << ecuio_last_error_detail();
    EXPECT_EQ(read_file(f.path / "health.txt"), good);
    EXPECT_FALSE(ecuio_health::put(f.utf8("health.txt"), good, ids, ok));
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the file exists already");
}

/* A name taken after the exists check (another process wrote it) is
 * never replaced, and the report's own file is removed, not left beside
 * it */
TEST(HealthTest, ANameTakenBeforeTheRenameLeavesTheOthersFileAndNoPartial)
{
    static fs::path taken;
    Folder f;
    taken = f.path / "health.txt";
    Car car;
    ecuio_health_report_t r{};
    ecuio_scrub::values ids;
    const std::string bytes = car.write(ECUIO_HEALTH_TEXT, &r, &ids);
    ecuio_health::put_test_before_rename = [](const std::string&) {
        std::ofstream(taken, std::ios::binary) << "another's";
    };
    const bool put = ecuio_health::put(f.utf8("health.txt"), bytes, ids, r);
    ecuio_health::put_test_before_rename = nullptr;
    EXPECT_FALSE(put);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("the report can't be put in place"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_EQ(read_file(taken), "another's");
    EXPECT_FALSE(partial_left(f.path, "health.txt"));
}

/* The text, byte for byte, for an ECU with nothing stored */
TEST(HealthTest, TheTextOfAQuietCarIsByteForByte)
{
    ecuio_session_info_t info{};
    info.backend = ECUIO_BACKEND_MOCK;
    info.channel_count = 1;
    std::snprintf(info.profile, sizeof info.profile, "mock");
    ecuio_channel_diagnostics_t diag{};
    diag.rx_id = 0x7E8;
    diag.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS;
    diag.readiness = ECUIO_DIAG_ANSWERED;
    diag.monitor_count = 1;
    diag.monitors[0] = {0, ECUIO_MONITOR_COMPLETE, {}};
    for (ecuio_dtc_list_t& l : diag.lists) {
        l.outcome = ECUIO_DIAG_ANSWERED;
    }
    diag.freeze = ECUIO_DIAG_ANSWERED;
    ecuio_channel_identity_t id{};
    ecuio_health::inputs in;
    in.info = &info;
    in.uri = "mock://Daily driver";
    in.diagnostics = &diag;
    in.identity = &id;
    in.channels = 1;
    in.utc_us = kUtc;
    const std::string misfire = ecuio_monitor_name(0, 0);
    EXPECT_EQ(ecuio_health::write(in, ECUIO_HEALTH_TEXT, nullptr, nullptr),
              std::string("Havne ") + ecuio_version_string() +
                  ": a scrubbed health report (ADR-0054)\n"
                  "Source: a live session, mock://<label> (mock), profile "
                  "mock\n"
                  "Time: 2026-11-06T14:22:33Z\n"
                  "Units: metric\n"
                  "\n"
                  "Vehicle (from its VIN, which is left out)\n"
                  "  No VIN was read.\n"
                  "\n"
                  "Inspection (40 CFR 85.2222)\n"
                  "  Verdict: ready, ready: the incomplete monitors are "
                  "within what the rule allows\n"
                  "  No model year: one incomplete monitor passes either "
                  "model year's rule, two pass only 1996-2000's, three or "
                  "more pass neither\n"
                  "  Incomplete: none\n"
                  "  Reports counted: 1\n"
                  "\n"
                  "ECU 0x7E8 (complete)\n"
                  "  Readiness: answered, MIL off, 0 confirmed codes\n"
                  "    " +
                  misfire +
                  ": complete\n"
                  "  Stored codes: answered, none\n"
                  "  Pending codes: answered, none\n"
                  "  Permanent codes: answered, none\n"
                  "  Freeze frame: answered\n"
                  "\n"
                  "Key values (the profile's ranked signals)\n"
                  "  The profile ranks no signals.\n"
                  "\n"
                  "Left out: 0 identification texts (the VIN, calibration "
                  "IDs, serials and the like)\n");
}

/* The whole car's text: its MIL and codes, the freeze frame, a
 * since-cleared value past ECUIO_VALUE_STALE_US said to be stale, the
 * identification by outcome alone, the key values with their ages */
TEST(HealthTest, TheTextOfACarWithCodesIsByteForByte)
{
    Car car;
    EXPECT_EQ(car.write(ECUIO_HEALTH_TEXT),
              std::string("Havne ") + ecuio_version_string() +
                  ": a scrubbed health report (ADR-0054)\n"
                  "Source: a live session, "
                  "elm327://<address>?profile=obd2&serial=115200 (elm327, "
                  "500000 bit/s), profile obd2\n"
                  "Time: 2026-11-06T14:22:33Z\n"
                  "Units: metric\n"
                  "\n"
                  "Vehicle (from its VIN, which is left out)\n"
                  "  Model year: 2021\n"
                  "  Make: not known\n"
                  "  Check digit: matches\n"
                  "\n"
                  "Inspection (40 CFR 85.2222)\n"
                  "  Verdict: mil_on, fails: an ECU commands the MIL\n"
                  "  Model year 2021: 1 incomplete monitor allowed\n"
                  "  Incomplete: Catalyst\n"
                  "  Reports counted: 1\n"
                  "\n"
                  "ECU 0x7E8 (complete)\n"
                  "  Readiness: answered, MIL on, 2 confirmed codes\n"
                  "    Misfire: complete, this drive cycle complete\n"
                  "    Catalyst: incomplete, this drive cycle incomplete\n"
                  "  Stored codes: answered\n"
                  "    P0301  Cylinder 1 Misfire Detected\n"
                  "    P0420  Catalyst System Efficiency Below Threshold "
                  "Bank 1\n"
                  "  Pending codes: answered, none\n"
                  "  Permanent codes: refused (7F 0A 11)\n"
                  "  Freeze frame: answered, stored by P0301  Cylinder 1 "
                  "Misfire Detected\n"
                  "    RPM: 3000 RPM\n"
                  "  Since the codes were cleared:\n"
                  "    Distance Since Clear: 1234 km (4.0 s old, stale)\n"
                  "  Identification (each item's text left out): VIN "
                  "answered, CALID answered, CVN refused\n"
                  "\n"
                  "Key values (the profile's ranked signals)\n"
                  "  RPM: 1726 RPM (0.1 s old)\n"
                  "  Coolant Temp: 42 " +
                  ranked_def(2)->unit +
                  " (0.2 s old)\n"
                  "\n"
                  "Left out: 2 identification texts (the VIN, calibration "
                  "IDs, serials and the like)\n");
}

TEST(HealthTest, NumbersAndTimesAreWrittenWithoutTheLocale)
{
    EXPECT_EQ(ecuio_health::fixed(3.0, 0), "3");
    EXPECT_EQ(ecuio_health::fixed(1.25, 1), "1.3");
    EXPECT_EQ(ecuio_health::fixed(-12.5, 1), "-12.5");
    EXPECT_EQ(ecuio_health::fixed(-0.04, 1), "0.0");
    EXPECT_EQ(ecuio_health::fixed(0.005, 3), "0.005");
    EXPECT_EQ(ecuio_health::utc_text(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(ecuio_health::utc_text(951782400000000u), "2000-02-29T00:00:00Z");
    EXPECT_EQ(ecuio_health::utc_text(253402300799999999u),
              "9999-12-31T23:59:59Z");
    EXPECT_EQ(ecuio_health::utc_text(UINT64_MAX), "9999-12-31T23:59:59Z");
    EXPECT_EQ(ecuio_health::utc_text(kUtc), "2026-11-06T14:22:33Z");
}

/* The public call over a mock:// session: the mock's report (published
 * at create), its URI's label out, the file written once */
TEST(HealthTest, TheExportWritesAMockSessionsReport)
{
    Folder f;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://Daily driver", &s), ECUIO_OK);
    const std::string out = f.utf8("health.json");
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    o.utc_us = kUtc;
    ecuio_health_report_t r{};
    ASSERT_EQ(ecuio_health_export(&o, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string json = read_file(f.path / "health.json");
    const ecuio_json::Value v = parsed(json);
    EXPECT_EQ(v.find("source")->find("uri")->text, "mock://<label>");
    EXPECT_EQ(json.find("Daily"), std::string::npos);
    EXPECT_EQ(r.ecus, 1u);
    EXPECT_GT(r.codes, 0u);
    EXPECT_FALSE(partial_left(f.path, "health.json"));
    EXPECT_EQ(ecuio_health_export(&o, &r), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the file exists already");
    ecuio_session_destroy(s);
}

/* ADR-0065: a session's report lists every module of the session, those
 * past the four the session info names itself too */
TEST(HealthTest, ASessionsReportListsEveryModule)
{
    Folder f;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  "mock://x?profile=uds-generic&did=7E8:D100:u8,7E9:D101:u8,"
                  "7EA:D102:u8,7EB:D103:u8,7EC:D104:u8,7ED:D105:u8",
                  &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string out = f.utf8("health.json");
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    o.utc_us = kUtc;
    ecuio_health_report_t r{};
    ASSERT_EQ(ecuio_health_export(&o, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(r.ecus, 6u);
    const ecuio_json::Value v = parsed(read_file(f.path / "health.json"));
    const char* const ids[] = {"0x7E8", "0x7E9", "0x7EA",
                               "0x7EB", "0x7EC", "0x7ED"};
    ASSERT_EQ(v.find("ecus")->items.size(), 6u);
    for (size_t c = 0; c < 6; ++c) {
        EXPECT_EQ(v.find("ecus")->items[c].find("ecu")->text, ids[c]) << c;
    }
    ecuio_session_destroy(s);
}

TEST(HealthTest, ArgumentsAreCheckedBeforeAnythingIsRead)
{
    /* a folder of its own: an argument let through writes nothing a
     * later run would meet */
    Folder f;
    const std::string out = f.utf8("health.txt");
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://args", &s), ECUIO_OK);
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_TEXT;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    ecuio_health_report_t r{};
    r.ecus = 7;
    EXPECT_EQ(ecuio_health_export(nullptr, &r), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.ecus, 0u);
    ecuio_health_options_t a = o;
    a.out_path_utf8 = nullptr;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    for (uint32_t format : {0u, 3u}) {
        a = o;
        a.format = format;
        EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    }
    a = o;
    a.flags = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    for (size_t i = 0; i < sizeof a.reserved / sizeof a.reserved[0]; ++i) {
        a = o;
        a.reserved[i] = 1;
        EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    }
    a = o;
    a.reserved0 = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.units = 2;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.session = nullptr;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.folder_utf8 = "rec";
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.value_count = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    /* a session's values are its newest: a range is a recording's */
    a = o;
    a.from_us = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.to_us = 5;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_health_value_t bad{1, 1, 0.0, 0, 0};
    a = o;
    a.values = &bad;
    a.value_count = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    /* A recording takes no values of the caller's */
    const ecuio_health_value_t given{1, 0, 0.0, 0, 0};
    a = o;
    a.session = nullptr;
    a.folder_utf8 = "rec";
    a.values = &given;
    a.value_count = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(fs::is_empty(f.path));
    ecuio_session_destroy(s);
}

} // namespace
