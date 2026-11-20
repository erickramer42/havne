// File/layer: tests/test_health_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_health*.cpp (test_core)

#pragma once

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
#include "partial_left.h"
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

inline std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

inline const ecuio_signal_set_t* obd2()
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &set), ECUIO_OK);
    return set;
}

/* The obd2 def keyed 41 <pid> */
inline const ecuio_signal_def_t* pid_def(uint8_t pid)
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

inline const ecuio_signal_def_t* ranked_def(uint8_t rank)
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
inline const ecuio_signal_def_t* unranked_def()
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

inline void identity_item(ecuio_channel_identity_t& id, uint8_t service, uint16_t key,
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
inline void strings_of(const ecuio_json::Value& v, std::vector<std::string>& out)
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

inline ecuio_json::Value parsed(const std::string& json)
{
    ecuio_json::Value v;
    ecuio_json::Error e;
    EXPECT_TRUE(ecuio_json::parse(json.data(), json.size(), &v, &e))
        << e.to_string() << "\n"
        << json;
    return v;
}

// ---- The report from a recording ----

/* A frames.csv row */
inline std::string row(uint64_t t, uint32_t id, uint32_t flags,
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

inline std::vector<uint8_t> with_text(std::vector<uint8_t> head, const char* text,
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
inline void car_recording(const Folder& f, const std::string& name = "rec")
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

} // namespace
