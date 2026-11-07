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
// Files go to a folder of their own under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "ecuio/vin.h"
#include "health.h"
#include "json.h"
#include "scrub.h"
#include "temp_name.h"

namespace fs = std::filesystem;

namespace {

/* The simulated ECU's VIN, the fixtures' made-up form */
const char* const kVin = "ZZZFAKE11M0000001";
/* 2026-11-06T14:22:33Z */
constexpr uint64_t kUtc = 1793974953000000u;

struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name("ecuio-health");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
    std::string utf8(const std::string& name) const
    {
        return (path / name).generic_u8string();
    }
};

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

const ecuio_signal_set_t* obd2()
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    return set;
}

/* The obd2 def keyed 41 <pid> */
const ecuio_signal_def_t* pid_def(uint8_t pid)
{
    const ecuio_signal_set_t* set = obd2();
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_length == 2 && d.mux_key == (0x4100u | pid)) {
            return &d;
        }
    }
    return nullptr;
}

const ecuio_signal_def_t* ranked_def(uint8_t rank)
{
    const ecuio_signal_set_t* set = obd2();
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].plot_rank == rank) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

/* A numeric obd2 def the set doesn't rank, and not a since-cleared PID */
const ecuio_signal_def_t* unranked_def()
{
    const ecuio_signal_set_t* set = obd2();
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const uint32_t pid = d.mux_key & 0xFFu;
        if (d.plot_rank == 0 && d.kind == ECUIO_VALUE_UINT &&
            d.mux_length == 2 && pid != 0x21 && pid != 0x30 && pid != 0x31 &&
            pid != 0x4D && pid != 0x4E) {
            return &d;
        }
    }
    return nullptr;
}

void identity_item(ecuio_channel_identity_t& id, uint8_t service, uint16_t key,
                   uint8_t key_len, uint8_t outcome, const char* text)
{
    ecuio_identity_item_t& it = id.list[id.items++];
    it.request[0] = service;
    if (key_len == 1) {
        it.request[1] = static_cast<uint8_t>(key);
    } else {
        it.request[1] = static_cast<uint8_t>(key >> 8);
        it.request[2] = static_cast<uint8_t>(key);
    }
    it.length = static_cast<uint8_t>(1 + key_len);
    it.outcome = outcome;
    std::snprintf(it.text, sizeof it.text, "%s", text);
    it.text_length = static_cast<uint8_t>(std::strlen(it.text));
}

/* An ELM327 session's engine ECU: the MIL on, two stored codes, none
 * pending, the permanent list refused, a freeze frame, the VIN and a
 * calibration ID answered, a CVN refused; the values its rotation held */
struct Car {
    ecuio_session_info_t info{};
    ecuio_channel_diagnostics_t diag{};
    ecuio_channel_identity_t id{};
    std::vector<ecuio_health_value_t> values;
    ecuio_health::inputs in;

    Car()
    {
        info.backend = ECUIO_BACKEND_ELM327;
        info.baud = 500000;
        info.channel_count = 1;
        std::snprintf(info.profile, sizeof info.profile, "obd2");
        diag.rx_id = 0x7E8;
        diag.generation = 3;
        diag.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE |
                     ECUIO_DIAG_READINESS | ECUIO_DIAG_MIL_ON |
                     ECUIO_DIAG_FREEZE_FRAME | ECUIO_DIAG_CYCLE;
        diag.readiness = ECUIO_DIAG_ANSWERED;
        diag.dtc_count = 2;
        diag.monitor_count = 2;
        diag.monitors[0] = {0,
                            ECUIO_MONITOR_COMPLETE |
                                ECUIO_MONITOR_CYCLE_ENABLED |
                                ECUIO_MONITOR_CYCLE_COMPLETE,
                            {}};
        diag.monitors[1] = {8, ECUIO_MONITOR_CYCLE_ENABLED, {}};
        diag.lists[ECUIO_DIAG_STORED] = {ECUIO_DIAG_ANSWERED, 0, 2, 2, {}};
        diag.lists[ECUIO_DIAG_STORED].codes[0] = 0x0301;
        diag.lists[ECUIO_DIAG_STORED].codes[1] = 0x0420;
        diag.lists[ECUIO_DIAG_PENDING] = {ECUIO_DIAG_ANSWERED, 0, 0, 0, {}};
        diag.lists[ECUIO_DIAG_PERMANENT] = {ECUIO_DIAG_REFUSED, 0x11, 0, 0, {}};
        diag.freeze = ECUIO_DIAG_ANSWERED;
        diag.freeze_dtc = 0x0301;
        diag.freeze_count = 1;
        diag.freeze_values[0] = {pid_def(0x0C)->id, 0, 3000.0};
        diag.cycle = ECUIO_DIAG_ANSWERED;
        id.rx_id = 0x7E8;
        id.flags = ECUIO_IDENTITY_COMPLETE;
        identity_item(id, 0x09, 0x02, 1, ECUIO_IDENTITY_ANSWERED, kVin);
        identity_item(id, 0x09, 0x04, 1, ECUIO_IDENTITY_ANSWERED, "FAKECAL-01");
        identity_item(id, 0x09, 0x06, 1, ECUIO_IDENTITY_REFUSED, "");
        values.push_back({ranked_def(1)->id, 0, 1726.0, 120000, 0});
        values.push_back({pid_def(0x31)->id, 0, 1234.0, 4000000, 0});
        values.push_back({ranked_def(2)->id, 0, 42.4, 250000, 0});
        values.push_back({unranked_def()->id, 0, 7.0, 100000, 0});
        in.info = &info;
        in.uri = "elm327://COM3?profile=obd2&serial=115200";
        in.set = obd2();
        in.diagnostics = &diag;
        in.identity = &id;
        in.channels = 1;
        in.values = values.data();
        in.value_count = static_cast<uint32_t>(values.size());
        in.latest_model_year = 2027;
        in.utc_us = kUtc;
    }
    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;

    std::string write(uint32_t format, ecuio_health_report_t* r = nullptr,
                      ecuio_scrub::values* identity = nullptr) const
    {
        return ecuio_health::write(in, format, r, identity);
    }
};

/* Every string the JSON holds, member values and array items alike */
void strings_of(const ecuio_json::Value& v, std::vector<std::string>& out)
{
    if (v.type == ecuio_json::Type::string) {
        out.push_back(v.text);
    }
    for (const ecuio_json::Value& i : v.items) {
        strings_of(i, out);
    }
    for (const ecuio_json::Member& m : v.members) {
        strings_of(m.value, out);
    }
}

ecuio_json::Value parsed(const std::string& json)
{
    ecuio_json::Value v;
    ecuio_json::Error e;
    EXPECT_TRUE(ecuio_json::parse(json.data(), json.size(), &v, &e))
        << e.to_string() << "\n"
        << json;
    return v;
}

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
    EXPECT_FALSE(fs::exists(f.path / "health.txt.partial"));

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

TEST(HealthTest, NumbersAndTimesAreWrittenWithoutTheLocale)
{
    EXPECT_EQ(ecuio_health::fixed(3.0, 0), "3");
    EXPECT_EQ(ecuio_health::fixed(1.25, 1), "1.3");
    EXPECT_EQ(ecuio_health::fixed(-12.5, 1), "-12.5");
    EXPECT_EQ(ecuio_health::fixed(-0.04, 1), "0.0");
    EXPECT_EQ(ecuio_health::fixed(0.005, 3), "0.005");
    EXPECT_EQ(ecuio_health::utc_text(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(ecuio_health::utc_text(951782400000000u), "2000-02-29T00:00:00Z");
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
    EXPECT_FALSE(fs::exists(f.path / "health.json.partial"));
    EXPECT_EQ(ecuio_health_export(&o, &r), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the file exists already");
    ecuio_session_destroy(s);
}

TEST(HealthTest, ArgumentsAreCheckedBeforeAnythingIsRead)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://args", &s), ECUIO_OK);
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_TEXT;
    o.session = s;
    o.out_path_utf8 = "health.txt";
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
    a = o;
    a.reserved[3] = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
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
    ecuio_health_value_t bad{1, 1, 0.0, 0, 0};
    a = o;
    a.values = &bad;
    a.value_count = 1;
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.session = nullptr;
    a.folder_utf8 = "rec"; /* a recording: step 2 */
    EXPECT_EQ(ecuio_health_export(&a, nullptr), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_FALSE(fs::exists("health.txt"));
    ecuio_session_destroy(s);
}

} // namespace
