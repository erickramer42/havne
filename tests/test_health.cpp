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

#include <gtest/gtest.h>

#include <algorithm>
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
#include "recording_rows.h" /* kRowMax */
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
    void write(const std::string& name, const std::string& bytes) const
    {
        const fs::path p = path / name;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << bytes;
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
    EXPECT_FALSE(fs::exists(f.path / "health.json.partial"));
    EXPECT_EQ(ecuio_health_export(&o, &r), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the file exists already");
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

// ---- The report from a recording ----

/* A frames.csv row */
std::string row(uint64_t t, uint32_t id, uint32_t flags,
                const std::vector<uint8_t>& data)
{
    char head[96];
    std::snprintf(head, sizeof head, "%llu,%llu,0x%X,0x%X,",
                  static_cast<unsigned long long>(t),
                  static_cast<unsigned long long>(t), id, flags);
    std::string out = head;
    for (size_t i = 0; i < data.size(); ++i) {
        char b[4];
        std::snprintf(b, sizeof b, i == 0 ? "%02X" : " %02X", data[i]);
        out += b;
    }
    return out + "\n";
}

std::vector<uint8_t> with_text(std::vector<uint8_t> head, const char* text,
                               size_t pad = 0)
{
    head.insert(head.end(), text, text + std::strlen(text));
    head.insert(head.end(), pad, 0x00);
    return head;
}

constexpr uint32_t kWhole = ECUIO_FRAME_FLAG_REASSEMBLED;

/* An ELM327 session's recording of the engine ECU's replies: the MIL on,
 * two stored codes (then a 7F 03 78, still working, which answers
 * nothing), the pending list refused, no permanent code, a freeze frame
 * stored by P0301 at 3000 rpm, Service 09's PID bitmap (no item), the VIN
 * across three frames as they crossed the bus (its Flow Control between)
 * and a calibration ID whole; another module's refusal and an
 * unattributable one; at 9 s readiness again, the MIL off. The values:
 * the first ranked signal and the distance since the codes were
 * cleared. */
void car_recording(const Folder& f, const std::string& name = "rec")
{
    const std::string rank1 = ranked_def(1)->name;
    const std::string since = pid_def(0x31)->name;
    f.write(name + "/events.log",
            "ecuio session recording, format 2 (ADR-0049)\n"
            "started  2026-11-06 14:00:00.000 (UTC)\n"
            "utc  2026-11-06T05:30:00.000Z\n"
            "core  0.5.1\n"
            "uri  elm327://COM3?profile=obd2&serial=115200\n"
            "profile  obd2\n"
            "frame flags  complete\n"
            "\n");
    const std::string vin = kVin;
    f.write(
        name + "/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(1000, 0x7E8, kWhole, {0x41, 0x01, 0x82, 0x07, 0xE5, 0x00}) +
            row(1100, 0x7E8, kWhole, {0x41, 0x41, 0x00, 0x07, 0xE5, 0x04}) +
            row(1200, 0x7E8, kWhole, {0x43, 0x02, 0x03, 0x01, 0x04, 0x20}) +
            row(1250, 0x7E8, kWhole, {0x7F, 0x03, 0x78}) +
            row(1300, 0x7E8, kWhole, {0x7F, 0x07, 0x11}) +
            row(1400, 0x7E8, kWhole, {0x4A, 0x00}) +
            row(1500, 0x7E8, kWhole,
                {0x42, 0x00, 0x00, 0x58, 0x18, 0x00, 0x00}) +
            row(1600, 0x7E8, kWhole, {0x42, 0x02, 0x00, 0x03, 0x01}) +
            row(1700, 0x7E8, kWhole, {0x42, 0x0C, 0x00, 0x2E, 0xE0}) +
            row(1750, 0x7E8, kWhole, {0x49, 0x00, 0x55, 0x40, 0x00, 0x00}) +
            row(1800, 0x7E8, 0,
                with_text({0x10, 0x14, 0x49, 0x02, 0x01},
                          vin.substr(0, 3).c_str())) +
            row(1805, 0x7E0, 0,
                {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) +
            row(1810, 0x7E8, 0, with_text({0x21}, vin.substr(3, 7).c_str())) +
            row(1820, 0x7E8, 0, with_text({0x22}, vin.substr(10, 7).c_str())) +
            row(1900, 0x7E8, kWhole,
                with_text({0x49, 0x04, 0x01}, "FAKECAL-01", 6)) +
            row(2000, 0x7E9, kWhole, {0x7F, 0x03, 0x11}) +
            row(2100, 0x7E8, kWhole, {0x7F, 0x01, 0x12}) +
            row(9000000, 0x7E8, kWhole, {0x41, 0x01, 0x00, 0x07, 0xE5, 0x00}));
    f.write(name + "/signals.csv", "host_us,signal,value,unit\n"
                                   "1000000," +
                                       rank1 + ",800," + ranked_def(1)->unit +
                                       "\n"
                                       "3000000," +
                                       since + ",1234," + pid_def(0x31)->unit +
                                       "\n"
                                       "4000000," +
                                       rank1 + ",850," + ranked_def(1)->unit +
                                       "\n"
                                       "8000000," +
                                       rank1 + ",900," + ranked_def(1)->unit +
                                       "\n");
}

/* ecuio_health_export over a folder, the file read back */
struct Reported {
    ecuio_status_t status = ECUIO_OK;
    ecuio_health_report_t r{};
    std::string bytes;
    std::string detail;
    std::string out;

    Reported(const Folder& f, const std::string& folder, uint32_t format,
             uint64_t from_us = 0, uint64_t to_us = 0,
             const char* baseline = nullptr)
    {
        static int n = 0;
        out = f.utf8("report-" + std::to_string(++n) +
                     (format == ECUIO_HEALTH_TEXT ? ".txt" : ".json"));
        ecuio_health_options_t o{};
        o.format = format;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        o.latest_model_year = 2027;
        o.utc_us = kUtc;
        o.from_us = from_us;
        o.to_us = to_us;
        o.baseline_folder_utf8 = baseline;
        status = ecuio_health_export(&o, &r);
        detail = ecuio_last_error_detail();
        bytes = read_file(fs::u8path(out));
    }
};

/* The replies rebuild the ECU's reports as the session kept them: the
 * last of each kind wins, a refusal of a list is that list's, another
 * module's refusal makes no ECU; both formats hold the same facts and
 * neither the VIN nor the calibration ID */
TEST(HealthTest, ARecordingsRepliesRebuildTheEcusReports)
{
    Folder f;
    car_recording(f);
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    const Reported text(f, f.utf8("rec"), ECUIO_HEALTH_TEXT);
    ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
    const ecuio_json::Value v = parsed(json.bytes);
    std::vector<std::string> facts;
    strings_of(v, facts);
    for (const std::string& fact : facts) {
        EXPECT_NE(text.bytes.find(fact), std::string::npos)
            << "the text lacks '" << fact << "'\n"
            << text.bytes;
    }
    for (const std::string* out : {&json.bytes, &text.bytes}) {
        EXPECT_EQ(out->find(kVin), std::string::npos);
        EXPECT_EQ(out->find("FAKECAL"), std::string::npos);
        EXPECT_EQ(out->find("COM3"), std::string::npos);
    }
    const ecuio_json::Value& src = *v.find("source");
    EXPECT_EQ(src.find("kind")->text, "recording");
    EXPECT_EQ(src.find("uri")->text,
              "elm327://<address>?profile=obd2&serial=115200");
    EXPECT_EQ(src.find("backend")->text, "elm327");
    EXPECT_EQ(src.find("started")->text, "2026-11-06T05:30:00Z");
    EXPECT_EQ(src.find("to_s")->number, 9.0);
    EXPECT_TRUE(v.find("vehicle")->find("vin_read")->boolean);
    ASSERT_EQ(v.find("ecus")->items.size(), 1u);
    const ecuio_json::Value& ecu = v.find("ecus")->items[0];
    EXPECT_EQ(ecu.find("ecu")->text, "0x7E8");
    EXPECT_EQ(ecu.find("state")->text, "recorded");
    /* The later readiness reply: the MIL off; the drive cycle's reply
     * laid on its monitors, though it came first */
    EXPECT_FALSE(ecu.find("readiness")->find("mil")->boolean);
    const std::vector<ecuio_json::Value>& monitors =
        ecu.find("readiness")->find("monitors")->items;
    ASSERT_FALSE(monitors.empty());
    size_t cycle_incomplete = 0;
    for (const ecuio_json::Value& m : monitors) {
        EXPECT_TRUE(m.find("cycle_enabled")->boolean) << m.find("name")->text;
        cycle_incomplete += m.find("cycle_complete")->boolean ? 0 : 1;
    }
    EXPECT_EQ(cycle_incomplete, 1u);
    EXPECT_EQ(ecu.find("stored")->find("codes")->items.size(), 2u);
    EXPECT_EQ(ecu.find("pending")->find("reply")->text, "7F 07 11");
    EXPECT_EQ(ecu.find("permanent")->find("outcome")->text, "answered");
    EXPECT_EQ(ecu.find("freeze")->find("stored_by")->find("code")->text,
              "P0301");
    ASSERT_EQ(ecu.find("freeze")->find("values")->items.size(), 1u);
    EXPECT_EQ(ecu.find("freeze")->find("values")->items[0].find("value")->whole,
              3000);
    ASSERT_EQ(ecu.find("identification")->items.size(), 2u);
    EXPECT_EQ(ecu.find("identification")->items[0].find("name")->text, "VIN");
    EXPECT_EQ(ecu.find("identification")->items[1].find("name")->text, "CALID");
    ASSERT_EQ(ecu.find("since_cleared")->items.size(), 1u);
    EXPECT_EQ(ecu.find("since_cleared")->items[0].find("age_ms")->whole, 6000);
    ASSERT_EQ(v.find("key_values")->items.size(), 1u);
    const ecuio_json::Value& key = v.find("key_values")->items[0];
    EXPECT_EQ(key.find("value")->whole, 900);
    EXPECT_EQ(key.find("age_ms")->whole, 1000);
    EXPECT_EQ(v.find("ecus_note"), nullptr);
    EXPECT_EQ(json.r.ecus, 1u);
    EXPECT_EQ(json.r.codes, 2u);
    EXPECT_EQ(json.r.values, 1u);
    EXPECT_EQ(json.r.since_cleared, 1u);
    EXPECT_EQ(json.r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 2u);
    EXPECT_EQ(json.r.flags, ECUIO_HEALTH_VIN_DECODED);
}

/* The range: up to 5 s the first readiness reply (the MIL on); from 8.5 s
 * only the later one, every other kind not recorded, and no value in
 * force from the range's start */
TEST(HealthTest, ARangeTakesTheRepliesThatEndInIt)
{
    Folder f;
    car_recording(f);
    const Reported early(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 0, 5000000);
    ASSERT_EQ(early.status, ECUIO_OK) << early.detail;
    const ecuio_json::Value e = parsed(early.bytes);
    EXPECT_EQ(e.find("source")->find("to_s")->number, 5.0);
    const ecuio_json::Value& ecu = e.find("ecus")->items.at(0);
    EXPECT_TRUE(ecu.find("readiness")->find("mil")->boolean);
    EXPECT_EQ(e.find("inspection")->find("verdict")->text, "mil_on");
    EXPECT_EQ(e.find("key_values")->items.at(0).find("value")->whole, 850);
    EXPECT_EQ(e.find("key_values")->items.at(0).find("age_ms")->whole, 1000);

    const Reported late(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 8500000, 0);
    ASSERT_EQ(late.status, ECUIO_OK) << late.detail;
    EXPECT_NE(late.bytes.find("Range: 8.500 s to 9.000 s"), std::string::npos)
        << late.bytes;
    EXPECT_NE(late.bytes.find("  Readiness: answered, MIL off, 0 confirmed "
                              "codes\n"),
              std::string::npos)
        << late.bytes;
    EXPECT_NE(late.bytes.find("  Stored codes: not recorded\n"),
              std::string::npos);
    EXPECT_NE(late.bytes.find("  Freeze frame: not recorded\n"),
              std::string::npos);
    EXPECT_NE(late.bytes.find("None of the ranked signals has a value."),
              std::string::npos);
    EXPECT_NE(late.bytes.find("No VIN was read."), std::string::npos);

    /* A range that starts after the recording ends is refused; one that
     * starts at its end is its last moment */
    const Reported past(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 500000000, 0);
    EXPECT_EQ(past.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(past.detail,
              "the range starts at 500 s, after the recording ends at 9 s");
    EXPECT_TRUE(past.bytes.empty());
    const Reported at_end(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 9000000, 0);
    EXPECT_EQ(at_end.status, ECUIO_OK) << at_end.detail;
}

/* The replay fixture: no events.log of its own, so one naming obd2 beside
 * its frames; its values decoded from the frames, and no ECU, since its
 * replies are Service 01's values alone */
TEST(HealthTest, TheReplayFixturesFramesGiveItsValuesAndNoEcu)
{
    Folder f;
    std::error_code ec;
    fs::create_directories(f.path / "fixture", ec);
    fs::copy_file(fs::u8path(ECUIO_REPLAY_FIXTURES) /
                      "2026-10-25-engine-start" / "frames.csv",
                  f.path / "fixture" / "frames.csv", ec);
    ASSERT_FALSE(ec) << ec.message();
    f.write("fixture/events.log",
            "ecuio session recording, format 2 (ADR-0049)\n"
            "profile  obd2\n"
            "\n");
    const Reported rep(f, f.utf8("fixture"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(rep.status, ECUIO_OK) << rep.detail;
    const ecuio_json::Value v = parsed(rep.bytes);
    EXPECT_TRUE(v.find("ecus")->items.empty());
    EXPECT_EQ(v.find("ecus_note")->text,
              "no ECU answered a diagnostic or identification request in "
              "the range");
    EXPECT_EQ(v.find("source")->find("uri")->type, ecuio_json::Type::null);
    EXPECT_EQ(v.find("source")->find("backend")->text, "unknown");
    EXPECT_GT(rep.r.values, 3u);
    EXPECT_EQ(rep.r.ecus, 0u);
    EXPECT_EQ(v.find("key_values")->items.at(0).find("name")->text,
              ranked_def(1)->name);
}

/* A recording of values alone (no frames.csv): the key values, no ECU,
 * and why */
TEST(HealthTest, ARecordingOfValuesAloneSaysItHasNoFrames)
{
    Folder f;
    const Reported json(f, ECUIO_GOLDEN_RECORDING, ECUIO_HEALTH_JSON);
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    const ecuio_json::Value v = parsed(json.bytes);
    EXPECT_FALSE(v.find("source")->find("frames")->boolean);
    EXPECT_EQ(v.find("source")->find("uri")->text,
              "replay://<path>?profile=obd2&speed=max");
    EXPECT_TRUE(v.find("ecus")->items.empty());
    EXPECT_EQ(v.find("ecus_note")->text, "the recording has no frames");
    EXPECT_GT(json.r.values, 3u);
    const Reported text(f, ECUIO_GOLDEN_RECORDING, ECUIO_HEALTH_TEXT);
    EXPECT_NE(text.bytes.find("\nECUs: the recording has no frames\n"),
              std::string::npos)
        << text.bytes;

    /* Neither frames nor values: refused, as the folder export refuses */
    f.write("empty/events.log", "ecuio session recording, format 2 "
                                "(ADR-0049)\n\n");
    const Reported none(f, f.utf8("empty"), ECUIO_HEALTH_JSON);
    EXPECT_EQ(none.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(none.detail, "the folder has no frames.csv or signals.csv");
    EXPECT_FALSE(fs::exists(fs::u8path(none.out)));
    EXPECT_FALSE(fs::exists(fs::u8path(none.out + ".partial")));

    /* A row the reader refuses, by its line */
    f.write("bad/frames.csv", "host_us,timestamp_us,id,flags,data\n" +
                                  row(2000, 0x7E8, kWhole, {0x43, 0x00}) +
                                  row(1000, 0x7E8, kWhole, {0x43, 0x00}));
    const Reported bad(f, f.utf8("bad"), ECUIO_HEALTH_JSON);
    EXPECT_EQ(bad.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(bad.detail,
              "frames.csv line 3: host_us is earlier than the row before");
    EXPECT_TRUE(bad.bytes.empty());
}

/* A folder export's scrubbed copy has no identification reply: its report
 * reads no VIN and lists no item, and keeps the codes */
TEST(HealthTest, AScrubbedCopysReportHasNoIdentification)
{
    Folder f;
    car_recording(f);
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_FOLDER;
    const std::string folder = f.utf8("rec");
    const std::string out = f.utf8("copy");
    o.folder_utf8 = folder.c_str();
    o.out_path_utf8 = out.c_str();
    ecuio_export_report_t er{};
    ASSERT_EQ(ecuio_export(&o, &er), ECUIO_OK) << ecuio_last_error_detail();
    const Reported rep(f, out, ECUIO_HEALTH_JSON);
    ASSERT_EQ(rep.status, ECUIO_OK) << rep.detail;
    const ecuio_json::Value v = parsed(rep.bytes);
    EXPECT_FALSE(v.find("vehicle")->find("vin_read")->boolean);
    const ecuio_json::Value& ecu = v.find("ecus")->items.at(0);
    EXPECT_TRUE(ecu.find("identification")->items.empty());
    EXPECT_EQ(ecu.find("stored")->find("codes")->items.size(), 2u);
    EXPECT_EQ(rep.r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 0u);
    EXPECT_EQ(rep.r.flags, 0u);
}

/* An identification reply before the range still has its value
 * searched for: a number signal in the range that reads it (an ECU's
 * serial as a number) fails the report */
TEST(HealthTest, AnIdentityReplyOutsideTheRangeIsStillChecked)
{
    Folder f;
    const std::string rank1 = ranked_def(1)->name;
    f.write("rec/events.log", "ecuio session recording, format 2 "
                              "(ADR-0049)\nprofile  obd2\n\n");
    f.write(
        "rec/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(1000, 0x7E8, kWhole,
                with_text({0x62, 0xF1, 0x8C}, "12345678")) +
            row(3000000, 0x7E8, kWhole, {0x41, 0x01, 0x00, 0x07, 0xE5, 0x00}));
    f.write("rec/signals.csv", "host_us,signal,value,unit\n"
                               "2500000," +
                                   rank1 + ",12345678," + ranked_def(1)->unit +
                                   "\n");
    const Reported whole(f, f.utf8("rec"), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(whole.status, ECUIO_ERR_LOAD_FAILED);
    const Reported later(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 2000000, 0);
    EXPECT_EQ(later.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(later.r.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(later.detail, "the check found an identification reply's value "
                            "in the report: nothing was written");
    EXPECT_TRUE(later.bytes.empty());
}

/* The text of a small recording, byte for byte */
TEST(HealthTest, TheTextOfARecordingIsByteForByte)
{
    Folder f;
    f.write("small/events.log", "ecuio session recording, format 2 (ADR-0049)\n"
                                "uri  mock://Weekend car\n"
                                "profile  mock\n"
                                "\n");
    f.write(
        "small/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(500000, 0x7E8, kWhole, {0x41, 0x01, 0x00, 0x01, 0x00, 0x00}) +
            row(600000, 0x7E8, kWhole, {0x43, 0x00}) +
            row(2500000, 0x7E8, kWhole, {0x7F, 0x0A, 0x11}));
    const Reported rep(f, f.utf8("small"), ECUIO_HEALTH_TEXT);
    ASSERT_EQ(rep.status, ECUIO_OK) << rep.detail;
    const std::string misfire = ecuio_monitor_name(0, 0);
    EXPECT_EQ(rep.bytes,
              std::string("Havne ") + ecuio_version_string() +
                  ": a scrubbed health report (ADR-0054)\n"
                  "Source: a recording, mock://<label> (mock), profile "
                  "mock\n"
                  "Range: 0.000 s to 2.500 s\n"
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
                  "ECU 0x7E8 (recorded)\n"
                  "  Readiness: answered, MIL off, 0 confirmed codes\n"
                  "    " +
                  misfire +
                  ": complete\n"
                  "  Stored codes: answered, none\n"
                  "  Pending codes: not recorded\n"
                  "  Permanent codes: refused (7F 0A 11)\n"
                  "  Freeze frame: not recorded\n"
                  "\n"
                  "Key values (the profile's ranked signals)\n"
                  "  The profile ranks no signals.\n"
                  "\n"
                  "Left out: 0 identification texts (the VIN, calibration "
                  "IDs, serials and the like)\n");
}

/* A since-cleared value on a reply ID no listed ECU takes goes among the
 * key values, after the ranked ones, in both formats */
TEST(HealthTest, ASinceClearedValueNoEcuTakesIsAKeyValue)
{
    Car car;
    car.diag.rx_id = 0x7E9; /* the value's 0x7E8 is no listed ECU's */
    ecuio_health_report_t r{};
    const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON, &r));
    EXPECT_TRUE(
        v.find("ecus")->items.at(0).find("since_cleared")->items.empty());
    const std::vector<ecuio_json::Value>& keys = v.find("key_values")->items;
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0].find("name")->text, ranked_def(1)->name);
    EXPECT_EQ(keys[1].find("name")->text, ranked_def(2)->name);
    EXPECT_EQ(keys[2].find("name")->text, pid_def(0x31)->name);
    EXPECT_EQ(keys[2].find("value")->whole, 1234);
    EXPECT_EQ(keys[2].find("age_ms")->whole, 4000);
    EXPECT_EQ(r.values, 3u);
    EXPECT_EQ(r.since_cleared, 1u);
    const std::string text = car.write(ECUIO_HEALTH_TEXT);
    const size_t head = text.find("  Since the codes were cleared, on a "
                                  "reply ID no ECU is listed for:\n");
    ASSERT_NE(head, std::string::npos) << text;
    EXPECT_LT(text.find(std::string(ranked_def(2)->name) + ": "), head);
    EXPECT_GT(text.find(std::string(pid_def(0x31)->name) + ": "), head);

    /* No ranked value: the text says so, and still shows it */
    car.values.erase(car.values.begin());
    car.values.erase(car.values.begin() + 1, car.values.end());
    car.in.values = car.values.data();
    car.in.value_count = static_cast<uint32_t>(car.values.size());
    const std::string alone = car.write(ECUIO_HEALTH_TEXT);
    EXPECT_NE(alone.find("  None of the ranked signals has a value.\n"),
              std::string::npos)
        << alone;
    EXPECT_NE(alone.find(std::string(pid_def(0x31)->name) + ": "),
              std::string::npos);
}

/* From a recording with none of an ECU's replies, the since-cleared value
 * signals.csv holds is still in the report */
TEST(HealthTest, ARecordingsSinceClearedValueWithNoEcuIsAKeyValue)
{
    Folder f;
    f.write("rec/events.log", "ecuio session recording, format 2 (ADR-0049)\n"
                              "profile  obd2\n"
                              "frame flags  complete\n"
                              "\n");
    f.write("rec/frames.csv",
            "host_us,timestamp_us,id,flags,data\n" +
                row(1000, 0x7E8, kWhole, {0x41, 0x31, 0x04, 0xD2}) +
                row(2000, 0x7E8, kWhole, {0x41, 0x0C, 0x1A, 0xF8}));
    f.write("rec/signals.csv", std::string("host_us,signal,value,unit\n") +
                                   "1000," + pid_def(0x31)->name + ",1234," +
                                   pid_def(0x31)->unit + "\n");
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    EXPECT_EQ(json.r.ecus, 0u);
    EXPECT_EQ(json.r.since_cleared, 1u);
    const ecuio_json::Value v = parsed(json.bytes);
    ASSERT_EQ(v.find("key_values")->items.size(), 1u);
    EXPECT_EQ(v.find("key_values")->items[0].find("value")->whole, 1234);
}

/* A recording older than the REASSEMBLED flag holds whole replies with
 * flags 0; they're taken whole, as the reader decodes them, but only on a
 * reply ID: another ID's frame that opens like a reply makes no ECU */
TEST(HealthTest, AnOlderRecordingsWholeRowsGiveItsEcu)
{
    Folder f;
    f.write("rec/events.log", "ecuio session recording, format 2 (ADR-0049)\n"
                              "profile  obd2\n"
                              "\n");
    f.write(
        "rec/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(1000, 0x7E8, 0, {0x41, 0x01, 0x82, 0x07, 0xE5, 0x00}) +
            row(1100, 0x7E8, 0, {0x43, 0x02, 0x03, 0x01, 0x04, 0x20}) +
            row(1200, 0x3E8, 0, {0x41, 0x01, 0x82, 0x07, 0xE5, 0x00}) +
            row(1300, 0x3E8, 0, {0x43, 0x01, 0x01, 0x71}) +
            row(1400, 0x7E8, ECUIO_FRAME_FLAG_ERROR, {0x43, 0x01, 0x01, 0x71}));
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    EXPECT_EQ(json.r.ecus, 1u);
    EXPECT_EQ(json.r.codes, 2u);
    const ecuio_json::Value v = parsed(json.bytes);
    ASSERT_EQ(v.find("ecus")->items.size(), 1u);
    const ecuio_json::Value& ecu = v.find("ecus")->items[0];
    EXPECT_EQ(ecu.find("ecu")->text, "0x7E8");
    EXPECT_TRUE(ecu.find("readiness")->find("mil")->boolean);
    EXPECT_EQ(ecu.find("stored")->find("codes")->items.size(), 2u);
}

/* The JSON is UTF-8 whatever bytes its source held: valid sequences as
 * they are, a byte no sequence holds as U+FFFD */
TEST(HealthTest, TheJsonIsUtf8WhateverBytesItsSourceHeld)
{
    Car car;
    std::snprintf(car.info.profile, sizeof car.info.profile, "%s",
                  "ob\xFF"
                  "d\xC3\xA9\xE2\x82");
    const std::string json = car.write(ECUIO_HEALTH_JSON);
    const ecuio_json::Value v = parsed(json);
    std::vector<std::string> facts;
    strings_of(v, facts);
    EXPECT_NE(std::find(facts.begin(), facts.end(),
                        "ob\xEF\xBF\xBD"
                        "d\xC3\xA9\xEF\xBF\xBD\xEF\xBF\xBD"),
              facts.end())
        << json;
}

/* An age past what milliseconds can round to saturates, never wraps */
TEST(HealthTest, AnAgeNearItsLimitSaturates)
{
    Car car;
    car.values[0].age_us = UINT64_MAX;
    const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON));
    const ecuio_json::Value& k = v.find("key_values")->items.at(0);
    EXPECT_EQ(k.find("age_ms")->whole, static_cast<int64_t>(UINT64_MAX / 1000));
    EXPECT_TRUE(k.find("stale")->boolean);
}

/* An enumeration's value that isn't a raw value a table holds (negative,
 * a fraction, past UINT32_MAX) is written as its number, with no label */
TEST(HealthTest, AnEnumerationValueNoTableHoldsHasNoLabel)
{
    Car car;
    ecuio_enum_entry_t entries[3] = {
        {0, "Off"}, {1, "Open loop"}, {2, "Closed loop"}};
    ecuio_signal_def_t d{};
    d.id = 900;
    d.message_id = 0x7E8;
    d.kind = ECUIO_VALUE_ENUM;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "Fuel System 1");
    d.plot_rank = 1;
    d.table_count = 3;
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &d;
    set.enum_count = 3;
    set.enums = entries;
    car.in.set = &set;
    for (double raw : {-1.0, 2.5, 1e20, 4294967298.0}) {
        SCOPED_TRACE(raw);
        const ecuio_health_value_t value{900, 0, raw, 0, 0};
        car.in.values = &value;
        car.in.value_count = 1;
        const ecuio_json::Value v = parsed(car.write(ECUIO_HEALTH_JSON));
        ASSERT_EQ(v.find("key_values")->items.size(), 1u);
        EXPECT_EQ(v.find("key_values")->items[0].find("value")->type,
                  ecuio_json::Type::number);
    }
    const ecuio_health_value_t two{900, 0, 2.0, 0, 0};
    car.in.values = &two;
    EXPECT_EQ(parsed(car.write(ECUIO_HEALTH_JSON))
                  .find("key_values")
                  ->items.at(0)
                  .find("value")
                  ->text,
              "Closed loop");
}

/* A report that isn't written counts nothing written; a failed check
 * keeps its class */
TEST(HealthTest, AReportNotWrittenCountsNothing)
{
    Folder f;
    car_recording(f);
    const Reported first(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(first.status, ECUIO_OK) << first.detail;
    ASSERT_EQ(first.r.ecus, 1u);
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    const std::string folder = f.utf8("rec");
    o.folder_utf8 = folder.c_str();
    o.out_path_utf8 = first.out.c_str(); /* exists */
    ecuio_health_report_t r{};
    EXPECT_EQ(ecuio_health_export(&o, &r), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "the file exists already");
    EXPECT_EQ(r.ecus, 0u);
    EXPECT_EQ(r.codes, 0u);
    EXPECT_EQ(r.values, 0u);
    EXPECT_EQ(r.since_cleared, 0u);
    EXPECT_EQ(r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 0u);
    EXPECT_EQ(r.flags, 0u);

    /* An ECU serial read as a number signal fails the check */
    Folder g;
    car_recording(g);
    std::string frames = read_file(g.path / "rec" / "frames.csv");
    frames +=
        row(9100000, 0x7E8, kWhole, with_text({0x62, 0xF1, 0x8C}, "12345678"));
    g.write("rec/frames.csv", frames);
    g.write("rec/signals.csv", std::string("host_us,signal,value,unit\n") +
                                   "8000000," + ranked_def(1)->name +
                                   ",12345678," + ranked_def(1)->unit + "\n");
    const Reported failed(g, g.utf8("rec"), ECUIO_HEALTH_JSON);
    EXPECT_EQ(failed.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(failed.r.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1)
        << failed.detail;
    EXPECT_EQ(failed.r.ecus, 0u);
    EXPECT_EQ(failed.r.values, 0u);
    EXPECT_EQ(failed.r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 0u);
}

/* A number value longer than any buffer but the row's is read whole, never
 * cut into another number */
TEST(HealthTest, ALongNumberIsReadWhole)
{
    Folder f;
    car_recording(f);
    f.write("rec/signals.csv", std::string("host_us,signal,value,unit\n") +
                                   "8000000," + ranked_def(1)->name + "," +
                                   std::string(67, '0') + "850," +
                                   ranked_def(1)->unit + "\n");
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    const ecuio_json::Value v = parsed(json.bytes);
    ASSERT_EQ(v.find("key_values")->items.size(), 1u);
    EXPECT_EQ(v.find("key_values")->items[0].find("value")->whole, 850);
}

// ---- The vehicle's baseline (ADR-0049's appendix "a vehicle's
// baseline") ----

/* A baseline's folder, named as the app names a recording's: its stamp
 * and the vehicle's label, which no report may hold */
const char* const kBaseline = "20261101-090000 Daily driver";

/* A baseline's events.log, all the report reads of it: started 2026-11-01
 * at 09:00 UTC, 312.4 s long, an ELM327 session's under obd2 */
std::string baseline_log(const std::string& before = std::string(),
                         const std::string& after = std::string())
{
    return before +
           "ecuio session recording, format 3 (ADR-0049)\n"
           "started  2026-11-01 09:00:00.000 (UTC)\n"
           "utc  2026-11-01T09:00:00.000Z\n"
           "core  0.6.0\n"
           "uri  elm327://COM4?profile=obd2\n"
           "profile  obd2\n"
           "frame flags  complete\n"
           "\n"
           "09:00:01.250  +1.250  link  ok (state 0, 0 episodes)\n" +
           after +
           "09:05:12.400  +312.400  end  0 records dropped, 0 notes "
           "dropped\n";
}

const char* const kBaselineLine =
    "\nBaseline: the vehicle's baseline recording, started "
    "2026-11-01T09:00:00Z, 312.400 s (elm327), profile obd2\n";

/* The report names the baseline by its start, length, backend and
 * profile, in both formats; nothing else in the report changes, and
 * neither its folder's name, its label nor its URI's port is written. A
 * baseline of "" is none, as NULL is. */
TEST(HealthTest, AReportNamesTheBaselineByItsStartNeverItsFolder)
{
    Folder f;
    car_recording(f);
    f.write(std::string(kBaseline) + "/events.log", baseline_log());
    const std::string base = f.utf8(kBaseline);
    const std::string rec = f.utf8("rec");
    const Reported json(f, rec, ECUIO_HEALTH_JSON, 0, 0, base.c_str());
    const Reported text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, base.c_str());
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
    const ecuio_json::Value v = parsed(json.bytes);
    const ecuio_json::Value* b = v.find("baseline");
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(b->type, ecuio_json::Type::object) << json.bytes;
    EXPECT_TRUE(b->find("read")->boolean);
    EXPECT_EQ(b->find("started")->text, "2026-11-01T09:00:00Z");
    EXPECT_EQ(b->find("length_s")->number, 312.4);
    EXPECT_EQ(b->find("profile")->text, "obd2");
    EXPECT_EQ(b->find("backend")->text, "elm327");
    EXPECT_FALSE(b->find("is_source")->boolean);
    EXPECT_EQ(b->find("why"), nullptr);
    EXPECT_EQ(json.r.flags & ECUIO_HEALTH_BASELINE_READ,
              ECUIO_HEALTH_BASELINE_READ);
    EXPECT_NE(text.bytes.find(kBaselineLine), std::string::npos) << text.bytes;
    std::vector<std::string> facts;
    strings_of(*b, facts);
    for (const std::string& s : facts) {
        EXPECT_NE(text.bytes.find(s), std::string::npos)
            << "the text lacks '" << s << "'";
    }
    for (const std::string& out : {json.bytes, text.bytes}) {
        EXPECT_EQ(out.find("Daily"), std::string::npos);
        EXPECT_EQ(out.find("20261101"), std::string::npos);
        EXPECT_EQ(out.find("COM4"), std::string::npos);
    }

    /* Without one, null and no line; the rest the same bytes */
    const Reported plain_json(f, rec, ECUIO_HEALTH_JSON);
    const Reported plain_text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, "");
    ASSERT_EQ(plain_text.status, ECUIO_OK) << plain_text.detail;
    EXPECT_EQ(parsed(plain_json.bytes).find("baseline")->type,
              ecuio_json::Type::null);
    EXPECT_EQ(plain_json.r.flags & ECUIO_HEALTH_BASELINE_READ, 0u);
    EXPECT_EQ(plain_text.bytes.find("Baseline"), std::string::npos);
    std::string without = text.bytes;
    without.replace(without.find(kBaselineLine), std::strlen(kBaselineLine),
                    "\n");
    EXPECT_EQ(without, plain_text.bytes);
}

/* The report's own recording as its baseline is said to be it, and so is
 * a copy of it (the same start); a session's report never is */
TEST(HealthTest, TheReportsOwnRecordingAsItsBaselineIsSaidToBeIt)
{
    Folder f;
    car_recording(f);
    f.write("copy/events.log", read_file(f.path / "rec" / "events.log"));
    const std::string rec = f.utf8("rec");
    for (const std::string& base : {rec, f.utf8("copy")}) {
        const Reported json(f, rec, ECUIO_HEALTH_JSON, 0, 0, base.c_str());
        const Reported text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, base.c_str());
        ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
        EXPECT_TRUE(
            parsed(json.bytes).find("baseline")->find("is_source")->boolean);
        EXPECT_NE(text.bytes.find("\nBaseline: the vehicle's baseline "
                                  "recording, started 2026-11-06T05:30:00Z ("
                                  "elm327), profile obd2: this recording\n"),
                  std::string::npos)
            << text.bytes;
    }

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://Daily driver", &s), ECUIO_OK);
    const std::string out = f.utf8("session.json");
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    o.utc_us = kUtc;
    o.baseline_folder_utf8 = rec.c_str();
    ecuio_health_report_t r{};
    ASSERT_EQ(ecuio_health_export(&o, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_json::Value v = parsed(read_file(f.path / "session.json"));
    EXPECT_TRUE(v.find("baseline")->find("read")->boolean);
    EXPECT_FALSE(v.find("baseline")->find("is_source")->boolean);
    EXPECT_EQ(r.flags & ECUIO_HEALTH_BASELINE_READ, ECUIO_HEALTH_BASELINE_READ);
    ecuio_session_destroy(s);
}

/* A baseline that can't be read says why in both formats, never its
 * path, and the report is still written */
TEST(HealthTest, ABaselineThatCantBeReadSaysWhyAndTheReportIsWritten)
{
    Folder f;
    car_recording(f);
    f.write("no-log/frames.csv", "host_us,timestamp_us,id,flags,data\n");
    f.write("not-a-recording/events.log", "a shopping list\n");
    const struct {
        const char* folder;
        const char* why;
    } cases[] = {
        {"gone", "its folder is gone: it was moved or deleted"},
        {"no-log", "its folder holds no events.log"},
        {"not-a-recording", "its events.log isn't a recording's"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder);
        const std::string base = f.utf8(c.folder);
        const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 0, 0,
                            base.c_str());
        const Reported text(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 0, 0,
                            base.c_str());
        ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
        ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
        const ecuio_json::Value v = parsed(json.bytes);
        const ecuio_json::Value* b = v.find("baseline");
        EXPECT_FALSE(b->find("read")->boolean);
        EXPECT_EQ(b->find("why")->text, c.why);
        EXPECT_EQ(b->find("started"), nullptr);
        EXPECT_EQ(json.r.flags & ECUIO_HEALTH_BASELINE_READ, 0u);
        EXPECT_NE(text.bytes.find(std::string("\nBaseline: the vehicle's "
                                              "baseline recording can't be "
                                              "read: ") +
                                  c.why + "\n"),
                  std::string::npos)
            << text.bytes;
        for (const std::string& out : {json.bytes, text.bytes}) {
            EXPECT_EQ(out.find(base), std::string::npos);
        }
    }
}

/* events.log is taken as the reader takes it: a byte order mark skipped,
 * format 1 by its first line's ending; a baseline with no utc line has
 * no start, one with no timed line no length; and a line past the
 * reader's room is one line, its rest never read as a timed line of its
 * own */
TEST(HealthTest, ABaselinesEventsLogIsReadAsTheReadersIs)
{
    Folder f;
    car_recording(f);
    const std::string old_header = "Havne 0.3.2 session recording (ADR-0017)\n"
                                   "profile  obd2\n"
                                   "\n";
    /* the reader's room is kRowMax + 1 bytes a read: the line's rest
     * starts with a timed line's shape */
    const std::string note = "09:00:02.000  +2.000  note  ";
    const std::string long_line =
        note +
        std::string(ecuio_recording_rows::kRowMax + 1 - note.size(), 'x') +
        "09:59:59.000  +9999.000  end  0 records dropped, 0 notes dropped\n";
    const struct {
        const char* folder;
        std::string log;
        const char* line;
    } cases[] = {
        {"bom", baseline_log("\xEF\xBB\xBF"), kBaselineLine},
        {"long", baseline_log(std::string(), long_line), kBaselineLine},
        {"older", old_header,
         "\nBaseline: the vehicle's baseline recording, with no start time "
         "(unknown), profile obd2\n"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder);
        f.write(std::string(c.folder) + "/events.log", c.log);
        const std::string base = f.utf8(c.folder);
        const Reported text(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 0, 0,
                            base.c_str());
        ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
        EXPECT_NE(text.bytes.find(c.line), std::string::npos) << text.bytes;
    }
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 0, 0,
                        f.utf8("older").c_str());
    const ecuio_json::Value v = parsed(json.bytes);
    const ecuio_json::Value* b = v.find("baseline");
    EXPECT_TRUE(b->find("read")->boolean);
    EXPECT_EQ(b->find("started")->type, ecuio_json::Type::null);
    EXPECT_EQ(b->find("length_s")->type, ecuio_json::Type::null);
    EXPECT_EQ(b->find("backend")->text, "unknown");
}

} // namespace
