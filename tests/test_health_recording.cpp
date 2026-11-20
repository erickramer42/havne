// File/layer: tests/test_health_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: HealthTest (test_core)

#include "test_health_support.h"

namespace {

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
    EXPECT_FALSE(
        partial_left(f.path, fs::u8path(none.out).filename().u8string()));

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
 * a fraction, past UINT32_MAX) is written as its number, with no label.
 * UINT32_MAX itself is the last that is, and 2^32 the first past it */
TEST(HealthTest, AnEnumerationValueNoTableHoldsHasNoLabel)
{
    Car car;
    ecuio_enum_entry_t entries[4] = {{0, "Off"},
                                     {1, "Open loop"},
                                     {2, "Closed loop"},
                                     {UINT32_MAX, "Top of the table"}};
    ecuio_signal_def_t d{};
    d.id = 900;
    d.message_id = 0x7E8;
    d.kind = ECUIO_VALUE_ENUM;
    d.bit_length = 8;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof d.name, "Fuel System 1");
    d.plot_rank = 1;
    d.table_count = 4;
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &d;
    set.enum_count = 4;
    set.enums = entries;
    car.in.set = &set;
    for (double raw : {-1.0, 2.5, 1e20, 4294967296.0, 4294967298.0}) {
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
    const ecuio_health_value_t top{900, 0, 4294967295.0, 0, 0};
    car.in.values = &top;
    EXPECT_EQ(parsed(car.write(ECUIO_HEALTH_JSON))
                  .find("key_values")
                  ->items.at(0)
                  .find("value")
                  ->text,
              "Top of the table");
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

} // namespace
