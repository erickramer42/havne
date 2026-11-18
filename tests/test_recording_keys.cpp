// File/layer: tests/test_recording_keys.cpp | C++17, Qt-free, every platform
// Testing: UNIT: RecordingKeysTest (test_core): a recording that keeps
//          each signal under its key (format 4, ADR-0065 section 9): the
//          reader's signals and find, the frames open with a signal
//          file, the CSV exports' key column and head cells, the folder
//          copy, and the health report's values by key

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
#include "recording_rows.h" /* core/src */
#include "temp_name.h"

namespace fs = std::filesystem;
namespace rows = ecuio_recording_rows;

namespace {

const char kBom[] = "\xEF\xBB\xBF";
const char kRpm1[] = "7E8:410C:2.0:16";
const char kRpm2[] = "7E9:410C:2.0:16";
const char kFuel[] = "7E8:4103:2.0:8";

/* A scratch folder under TEMP or TMPDIR, removed at the end: one for
 * each test (the name is the test's), a second recording in a subfolder */
struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-recording-keys");
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

    std::string utf8(const char* name = "") const
    {
        return (name[0] != '\0' ? path / name : path).generic_u8string();
    }
    void write(const char* name, const std::string& text) const
    {
        std::error_code ec;
        fs::create_directories((path / name).parent_path(), ec);
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
    std::string read(const char* name) const
    {
        std::ifstream f(path / name, std::ios::binary);
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    }
};

/* events.log with this first line and these header lines, the first of
 * them the file's line 8, and a summary */
std::string events_of(const char* first_line,
                      const std::vector<std::string>& header_lines,
                      const char* profile = "test")
{
    std::string s = std::string(first_line) +
                    "\nstarted  2026-11-05 12:00:00.000 (local time)\n"
                    "utc  2026-11-05T18:00:00.000Z\ncore  0.9.0\n"
                    "uri  mock://x\nprofile  " +
                    profile + "\nframe flags  complete\n";
    for (const std::string& l : header_lines) {
        s += l + "\n";
    }
    return s + "columns  local time, seconds since the recording started\n\n"
               "\n12:00:10.000  +10.000  end  the test ended: 0 frames, 0 "
               "values, 0 records dropped, 0 notes dropped\n";
}

std::string signals_csv(const std::string& rows_text)
{
    return std::string(kBom) + rows::kSignalsHeader + "\n" + rows_text;
}

struct Reader {
    ecuio_recording_t* r = nullptr;
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    explicit Reader(const std::string& folder)
    {
        rc = ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r);
    }
    ~Reader() { ecuio_recording_close(r); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    ecuio_recording_info_t info() const
    {
        ecuio_recording_info_t i{};
        EXPECT_EQ(ecuio_recording_get_info(r, &i), ECUIO_OK);
        return i;
    }
    ecuio_recording_signal_t signal(uint32_t k) const
    {
        ecuio_recording_signal_t s{};
        EXPECT_EQ(ecuio_recording_get_signal(r, k, &s), ECUIO_OK);
        /* The key terminated, and zero from its NUL to the end */
        const size_t n = strnlen(s.key, sizeof s.key);
        EXPECT_LT(n, sizeof s.key);
        for (size_t i = n; i < sizeof s.key; ++i) {
            EXPECT_EQ(s.key[i], '\0') << s.key << ", byte " << i;
        }
        return s;
    }
    /* The find's answer: the signal's place, or UINT32_MAX for a refusal,
     * which leaves the index alone */
    uint32_t find(const char* text) const
    {
        uint32_t index = 77;
        const ecuio_status_t st = ecuio_recording_find_signal(r, text, &index);
        if (st != ECUIO_OK) {
            EXPECT_EQ(st, ECUIO_ERR_INVALID_ARGUMENT) << text;
            EXPECT_EQ(index, 77u) << "untouched on a refusal";
            return UINT32_MAX;
        }
        return index;
    }
    std::string text_at(uint32_t signal, uint64_t at_us) const
    {
        char text[64] = "";
        uint64_t at = 0;
        EXPECT_EQ(
            ecuio_recording_text_at(r, signal, at_us, text, sizeof text, &at),
            ECUIO_OK);
        return text;
    }
};

std::string open_detail(const std::string& folder)
{
    ecuio_recording_t* r = nullptr;
    EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r, nullptr);
    ecuio_recording_close(r);
    return ecuio_last_error_detail();
}

/* Two signals named RPM on two modules, an enum and a signal under its
 * author's key, and a row whose key no signal line declares */
void write_keyed(const Folder& f)
{
    f.write(
        "events.log",
        events_of(
            rows::kFormat4,
            {std::string("signal  ") + kRpm1 + ",RPM,number,rpm,0,2,1",
             std::string("signal  ") + kRpm2 + ",RPM,number,rpm,0,2,1",
             std::string("signal  ") + kFuel + ",\"Fuel, System\",enum,,0,1,3",
             std::string("label  ") + kFuel + ",2,Closed loop",
             "signal  boost,Boost,number,kPa,0,0,2"}));
    f.write("signals.csv",
            signals_csv(std::string("1000000,") + kRpm1 + ",800,rpm\n" +
                        "1000000," + kRpm2 + ",900,rpm\n" +
                        "1000000,boost,12,kPa\n" + "1500000," + kFuel +
                        ",2,\n" + "2000000," + kRpm1 + ",810,rpm\n" +
                        "2000000," + kRpm2 + ",910,rpm\n" +
                        "2000000,boost,13,kPa\n" + "2500000,id:7,1.5,V\n"));
}

/* A CSV export of folder f's recording, its outcome and bytes */
struct Exported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::vector<std::string> lines; /* line ends dropped */

    Exported(const Folder& f, uint32_t layout,
             const std::vector<const char*>& columns = {},
             const char* recording = "")
    {
        const std::string folder = f.utf8(recording);
        const std::string out = f.utf8("out.csv");
        std::error_code ec;
        fs::remove(f.path / "out.csv", ec);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.layout = layout;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        o.column_count = static_cast<uint32_t>(columns.size());
        o.columns_utf8 = columns.empty() ? nullptr : columns.data();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        std::istringstream in(f.read("out.csv"));
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
    }
};

/* A folder copy of the recording in f's rec, its outcome and two files */
struct Copied {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    std::string detail;
    std::string events;
    std::string signals;

    Copied(const Folder& f, uint64_t from_us, uint64_t to_us)
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8("copy");
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_FOLDER;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        ecuio_export_report_t report{};
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        events = f.read("copy/events.log");
        signals = f.read("copy/signals.csv");
    }
};

/* A health report of one of f's folders as text, its outcome and counts */
struct Reported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_health_report_t report{};
    std::string detail;
    std::string text;

    Reported(const Folder& f, const char* recording,
             const char* baseline = nullptr)
    {
        const std::string folder = f.utf8(recording);
        const std::string base = baseline != nullptr ? f.utf8(baseline) : "";
        const std::string out = f.utf8("report.txt");
        std::error_code ec;
        fs::remove(f.path / "report.txt", ec);
        ecuio_health_options_t o{};
        o.format = ECUIO_HEALTH_TEXT;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        o.baseline_folder_utf8 = baseline != nullptr ? base.c_str() : nullptr;
        rc = ecuio_health_export(&o, &report);
        detail = ecuio_last_error_detail();
        text = f.read("report.txt");
    }
};

/* A signal file's text loaded, and the key each of its signals is kept
 * under by the key its author gave it */
struct Loaded {
    ecuio_signal_file_t* file = nullptr;
    std::vector<ecuio_signal_key_t> keys;

    explicit Loaded(const std::string& json)
    {
        EXPECT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
        if (set != nullptr) {
            keys.resize(set->count);
            uint32_t count = 0;
            EXPECT_EQ(ecuio_signal_set_keys(set, file, keys.data(), set->count,
                                            &count),
                      ECUIO_OK);
        }
    }
    ~Loaded() { ecuio_signal_file_free(file); }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;

    std::string key(const char* authors) const
    {
        const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
        const ecuio_signal_def_t* d = ecuio_signal_file_find(file, authors);
        EXPECT_NE(d, nullptr) << authors;
        return d != nullptr ? keys[static_cast<size_t>(d - set->defs)].text
                            : std::string();
    }
};

} // namespace

// A format 4 recording's signals each carry the key their rows are under
// and the name their signal line gives, so two signals of one name stay
// two; a row with no signal line shows its key as its name
TEST(RecordingKeysTest, AFormat4RecordingKeepsEachSignalUnderItsKey)
{
    Folder f;
    write_keyed(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().format, ECUIO_RECORDING_FORMAT_KEYS);
    ASSERT_EQ(r.info().signal_count, 5u);

    const ecuio_recording_signal_t one = r.signal(0);
    EXPECT_STREQ(one.key, kRpm1);
    EXPECT_STREQ(one.name, "RPM");
    EXPECT_STREQ(one.unit, "rpm");
    EXPECT_EQ(one.samples, 2u);
    EXPECT_EQ(one.lowest, 800.0);
    EXPECT_EQ(one.highest, 810.0);
    EXPECT_EQ(one.plot_rank, 2);
    const ecuio_recording_signal_t two = r.signal(1);
    EXPECT_STREQ(two.key, kRpm2);
    EXPECT_STREQ(two.name, "RPM");
    EXPECT_EQ(two.lowest, 900.0);
    EXPECT_EQ(two.highest, 910.0);
    EXPECT_EQ(r.text_at(0, 2500000), "810");
    EXPECT_EQ(r.text_at(1, 2500000), "910");

    const ecuio_recording_signal_t fuel = r.signal(2);
    EXPECT_STREQ(fuel.key, kFuel);
    EXPECT_STREQ(fuel.name, "Fuel, System");
    EXPECT_EQ(fuel.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    ecuio_enum_entry_t label{};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_recording_get_labels(r.r, 2, &label, 1, &count), ECUIO_OK);
    ASSERT_EQ(count, 1u) << "the label line names its signal by key";
    EXPECT_EQ(label.raw, 2u);
    EXPECT_STREQ(label.label, "Closed loop");

    EXPECT_STREQ(r.signal(3).key, "boost");
    EXPECT_STREQ(r.signal(3).name, "Boost");
    const ecuio_recording_signal_t bare = r.signal(4);
    EXPECT_STREQ(bare.key, "id:7");
    EXPECT_STREQ(bare.name, "id:7") << "no signal line: its key shown";
    EXPECT_STREQ(bare.unit, "V") << "the row's";
}

// The find takes a key first, then a name one signal has; a name two
// signals have is refused with their keys
TEST(RecordingKeysTest, FindTakesAKeyThenANameOneSignalHas)
{
    Folder f;
    write_keyed(f);
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.find(kRpm1), 0u);
    EXPECT_EQ(r.find(kRpm2), 1u);
    EXPECT_EQ(r.find("Fuel, System"), 2u);
    EXPECT_EQ(r.find("Boost"), 3u);
    EXPECT_EQ(r.find("boost"), 3u);
    EXPECT_EQ(r.find("id:7"), 4u);

    EXPECT_EQ(r.find("RPM"), UINT32_MAX);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "2 signals are named 'RPM': name one by its key "
                 "(7E8:410C:2.0:16, 7E9:410C:2.0:16)");
    EXPECT_EQ(r.find("rpm"), UINT32_MAX) << "a name as it is written";
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no signal with the key or name 'rpm'");
    EXPECT_EQ(r.find(""), UINT32_MAX);

    uint32_t index = 77;
    EXPECT_EQ(ecuio_recording_find_signal(nullptr, "RPM", &index),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_find_signal(r.r, nullptr, &index),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recording_find_signal(r.r, "RPM", nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(index, 77u);
}

// An older recording's signals have no key and are found by name
TEST(RecordingKeysTest, AnOlderRecordingsSignalsGoByName)
{
    Folder f;
    f.write("events.log",
            events_of(rows::kFormat3, {"signal  RPM,number,rpm,0,2,1",
                                       "signal  Fuel,enum,,0,1,3",
                                       "label  Fuel,2,Closed loop"}));
    f.write("signals.csv",
            signals_csv("1000000,RPM,800,rpm\n1500000,Fuel,2,\n"));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().format, ECUIO_RECORDING_FORMAT_ENUMS);
    ASSERT_EQ(r.info().signal_count, 2u);
    EXPECT_STREQ(r.signal(0).key, "");
    EXPECT_STREQ(r.signal(1).key, "");
    EXPECT_STREQ(r.signal(1).name, "Fuel");
    EXPECT_EQ(r.find("Fuel"), 1u);
    EXPECT_EQ(r.find("RPM"), 0u);
    EXPECT_EQ(r.find(kRpm1), UINT32_MAX);
}

// A format 4 signal line gives the key before the name, and a label line
// names its signal by key: each refusal names the line
TEST(RecordingKeysTest, AFormat4HeaderLineNeedsItsKey)
{
    const auto refused = [](const std::vector<std::string>& lines) {
        Folder f;
        f.write("events.log", events_of(rows::kFormat4, lines));
        f.write("signals.csv", signals_csv(""));
        return open_detail(f.utf8());
    };
    EXPECT_EQ(refused({"signal  ,RPM,number,rpm,0,2,1"}),
              "events.log line 8: the signal's key is empty or longer than "
              "31 bytes");
    EXPECT_EQ(
        refused({"signal  " + std::string(32, 'k') + ",RPM,number,rpm,0,2,1"}),
        "events.log line 8: the signal's key is empty or longer than "
        "31 bytes");
    EXPECT_EQ(refused({"signal  RPM"}),
              "events.log line 8: the signal line isn't "
              "key,name,kind,unit,decimals,rank,group");
    EXPECT_EQ(refused({"signal  RPM,number,rpm,0,2,1"}),
              "events.log line 8: the signal's kind isn't number, text or "
              "enum")
        << "format 3's line: its name read as the key";
    EXPECT_EQ(refused({std::string("signal  ") + kFuel + ",Fuel,enum,,0,1,3",
                       "label  Fuel,2,Closed loop"}),
              "events.log line 9: the label names no enum signal above it")
        << "a label by its signal's name";

    /* The longest key reads */
    Folder f;
    const std::string longest(31, 'k');
    f.write("events.log", events_of(rows::kFormat4, {"signal  " + longest +
                                                     ",RPM,number,rpm,0,2,1"}));
    f.write("signals.csv", signals_csv("1000000," + longest + ",800,rpm\n"));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.signal(0).key, longest);
}

// Opened through its frames, each signal has its set's key: with the
// set's signal file a computed signal goes by its author's key, with
// the set alone by its id; a file of another set is refused
TEST(RecordingKeysTest, OpenedThroughItsFramesASignalHasItsSetsKey)
{
    const auto load = [](const char* name) {
        const std::string json =
            std::string(R"({"havne_signals":1,"name":")") + name +
            R"(","source":{"name":"t"},)"
            R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
            R"({"key":"gear","name":"Gear","ecu":"pcm","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
            R"({"key":"twice","name":"Twice","kind":"computed","expr":"gear * 2"}]})";
        ecuio_signal_file_t* file = nullptr;
        EXPECT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        return file;
    };
    ecuio_signal_file_t* file = load("Keys");
    ecuio_signal_file_t* other = load("Other");
    ASSERT_NE(file, nullptr);
    ASSERT_NE(other, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);

    Folder f;
    f.write("events.log", events_of(rows::kFormat4, {}));
    f.write("frames.csv", "host_us,timestamp_us,id,flags,data\n"
                          "1000000,1000000,0x7E8,0x100,62 D1 00 05\n");
    const auto keys_with = [&](const ecuio_signal_file_t* given) {
        std::vector<std::string> keys;
        ecuio_recording_t* r = nullptr;
        EXPECT_EQ(ecuio_recording_open_frames_with(f.utf8().c_str(), set, given,
                                                   nullptr, nullptr, &r),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_recording_info_t info{};
        EXPECT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
        EXPECT_NE(info.flags & ECUIO_RECORDING_INFO_FRAMES, 0u);
        for (uint32_t k = 0; k < info.signal_count; ++k) {
            ecuio_recording_signal_t s{};
            EXPECT_EQ(ecuio_recording_get_signal(r, k, &s), ECUIO_OK);
            keys.emplace_back(s.key);
            uint32_t found = 77;
            EXPECT_EQ(ecuio_recording_find_signal(r, s.key, &found), ECUIO_OK);
            EXPECT_EQ(found, k);
        }
        ecuio_recording_close(r);
        return keys;
    };
    EXPECT_EQ(keys_with(file),
              (std::vector<std::string>{"7E8:62D100:3.0:8", "twice"}));
    const std::vector<std::string> alone = keys_with(nullptr);
    ASSERT_EQ(alone.size(), 2u);
    EXPECT_EQ(alone[0], "7E8:62D100:3.0:8");
    EXPECT_EQ(alone[1].rfind("id:", 0), 0u) << alone[1];

    ecuio_recording_t* plain = nullptr;
    ASSERT_EQ(ecuio_recording_open_frames(f.utf8().c_str(), set, nullptr,
                                          nullptr, &plain),
              ECUIO_OK);
    ecuio_recording_signal_t s{};
    ASSERT_EQ(ecuio_recording_get_signal(plain, 1, &s), ECUIO_OK);
    EXPECT_EQ(std::string(s.key), alone[1]) << "the plain open: no file";
    ecuio_recording_close(plain);

    ecuio_recording_t* r = reinterpret_cast<ecuio_recording_t*>(&f);
    EXPECT_EQ(ecuio_recording_open_frames_with(f.utf8().c_str(), set, other,
                                               nullptr, nullptr, &r),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a file of another set";
    EXPECT_EQ(r, nullptr);
    ecuio_signal_file_free(file);
    ecuio_signal_file_free(other);
}

// The long CSV of a format 4 recording names each row's signal and gives
// its key last; an older recording's has no key column
TEST(RecordingKeysTest, TheLongCsvGivesEachRowsKey)
{
    Folder f;
    write_keyed(f);
    const Exported e(f, ECUIO_CSV_LONG);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_GE(e.lines.size(), 10u);
    EXPECT_EQ(e.lines[1], "time_s,signal,unit,value,key");
    EXPECT_EQ(e.lines[2], "1.000000,RPM,rpm,800,7E8:410C:2.0:16");
    EXPECT_EQ(e.lines[3], "1.000000,RPM,rpm,900,7E9:410C:2.0:16");
    EXPECT_EQ(e.lines[4], "1.000000,Boost,kPa,12,boost");
    EXPECT_EQ(e.lines[5], "1.500000,\"Fuel, System\",,2,7E8:4103:2.0:8");
    EXPECT_EQ(e.lines[9], "2.500000,id:7,V,1.5,id:7");

    f.write("old/events.log",
            events_of(rows::kFormat3, {"signal  RPM,number,rpm,0,2,1"}));
    f.write("old/signals.csv", signals_csv("1000000,RPM,800,rpm\n"));
    const Exported o(f, ECUIO_CSV_LONG, {}, "old");
    ASSERT_EQ(o.rc, ECUIO_OK) << o.detail;
    ASSERT_GE(o.lines.size(), 3u);
    EXPECT_EQ(o.lines[1], "time_s,signal,unit,value");
    EXPECT_EQ(o.lines[2], "1.000000,RPM,rpm,800");
}

// The wide CSV heads a column "Name (unit)", with its key after only
// where two columns share that head; a column is named by its key or by
// a name one signal has
TEST(RecordingKeysTest, TheWideCsvTellsColumnsOfOneNameApartByKey)
{
    Folder f;
    write_keyed(f);
    const Exported all(f, ECUIO_CSV_WIDE);
    ASSERT_EQ(all.rc, ECUIO_OK) << all.detail;
    ASSERT_GE(all.lines.size(), 2u);
    EXPECT_EQ(all.lines[0],
              "Time (ms),RPM (rpm) [7E8:410C:2.0:16],"
              "RPM (rpm) [7E9:410C:2.0:16],\"Fuel, System\",Boost (kPa),"
              "id:7 (V)");

    const Exported picked(f, ECUIO_CSV_WIDE, {kRpm2, "Boost"});
    ASSERT_EQ(picked.rc, ECUIO_OK) << picked.detail;
    ASSERT_GE(picked.lines.size(), 3u);
    EXPECT_EQ(picked.lines[0], "Time (ms),RPM (rpm),Boost (kPa)")
        << "a head cell of its own stays bare";
    EXPECT_EQ(picked.lines[1], "1000,900,12");
    EXPECT_EQ(picked.lines[2], "2000,910,13");

    const Exported both(f, ECUIO_CSV_WIDE, {kRpm2, kRpm1});
    ASSERT_EQ(both.rc, ECUIO_OK) << both.detail;
    ASSERT_GE(both.lines.size(), 2u);
    EXPECT_EQ(both.lines[0], "Time (ms),RPM (rpm) [7E9:410C:2.0:16],"
                             "RPM (rpm) [7E8:410C:2.0:16]");
    EXPECT_EQ(both.lines[1], "1000,900,800");

    const Exported named(f, ECUIO_CSV_WIDE, {"RPM"});
    EXPECT_EQ(named.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(named.detail, "2 signals are named 'RPM': name one by its key "
                            "(7E8:410C:2.0:16, 7E9:410C:2.0:16)");
    EXPECT_TRUE(named.lines.empty()) << "nothing written";

    const Exported twice(f, ECUIO_CSV_WIDE, {"Boost", "boost"});
    EXPECT_EQ(twice.rc, ECUIO_ERR_INVALID_ARGUMENT)
        << "one signal by its name and by its key";
    EXPECT_EQ(twice.detail, "the column 'Boost' is named twice");

    /* A text signal named as a column is refused by its name */
    f.write(
        "text/events.log",
        events_of(rows::kFormat4, {"signal  7E8:62D101:3:t4,Mode,text,,0,0,2",
                                   "signal  boost,Boost,number,kPa,0,0,2"}));
    f.write("text/signals.csv", signals_csv("1000000,7E8:62D101:3:t4,Sport,\n"
                                            "1000000,boost,12,kPa\n"));
    for (const char* column : {"Mode", "7E8:62D101:3:t4"}) {
        const Exported mode(f, ECUIO_CSV_WIDE, {column}, "text");
        EXPECT_EQ(mode.rc, ECUIO_ERR_INVALID_ARGUMENT) << column;
        EXPECT_EQ(mode.detail,
                  "the signal 'Mode' is a text signal, never a column")
            << column;
    }
    const Exported rest(f, ECUIO_CSV_WIDE, {}, "text");
    ASSERT_EQ(rest.rc, ECUIO_OK) << rest.detail;
    ASSERT_GE(rest.lines.size(), 1u);
    EXPECT_EQ(rest.lines[0], "Time (ms),Boost (kPa)")
        << "a text signal left out";

    const Exported lacking(f, ECUIO_CSV_WIDE, {"Boost", "Nothing"});
    ASSERT_EQ(lacking.rc, ECUIO_OK) << lacking.detail;
    ASSERT_GE(lacking.lines.size(), 1u);
    EXPECT_EQ(lacking.lines[0], "Time (ms),Boost (kPa),Nothing")
        << "a column the recording lacks, headed as given";
    EXPECT_EQ(lacking.detail, "") << "the lookup's words don't outlive it";
}

// A name or a key that starts as a spreadsheet formula does stops a CSV,
// said of the signal's name, with nothing written
TEST(RecordingKeysTest, AFormulaInANameOrAKeyStopsTheCsv)
{
    Folder f;
    f.write("named/events.log",
            events_of(rows::kFormat4, {"signal  boost,=1+1,number,kPa,0,0,2"}));
    f.write("named/signals.csv", signals_csv("1000000,boost,12,kPa\n"));
    f.write("keyed/events.log",
            events_of(rows::kFormat4, {"signal  =k,Boost,number,kPa,0,0,2"}));
    f.write("keyed/signals.csv", signals_csv("1000000,=k,12,kPa\n"));
    for (const uint32_t layout : {ECUIO_CSV_LONG, ECUIO_CSV_WIDE}) {
        const Exported name(f, layout, {}, "named");
        EXPECT_EQ(name.rc, ECUIO_ERR_LOAD_FAILED) << layout;
        EXPECT_EQ(name.detail.rfind("the signal '=1+1''s name starts as a "
                                    "spreadsheet formula does",
                                    0),
                  0u)
            << name.detail;
        EXPECT_TRUE(name.lines.empty()) << layout;
        const Exported key(f, layout, {}, "keyed");
        EXPECT_EQ(key.rc, ECUIO_ERR_LOAD_FAILED) << layout;
        EXPECT_EQ(key.detail.rfind("the signal 'Boost''s key starts as a "
                                   "spreadsheet formula does",
                                   0),
                  0u)
            << key.detail;
        EXPECT_TRUE(key.lines.empty()) << layout;
    }
}

// A recording's own signals.json says which signals are text or
// identification, by the keys its rows are under: one at an address a
// signal before it has goes by its author's key. Neither is a column.
TEST(RecordingKeysTest, ASetsTextAndIdentificationSignalsAreLeftOutByKey)
{
    const std::string json =
        R"({"havne_signals":2,"name":"Scrub","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"part","name":"Part","kind":"text","ecu":"pcm","read":{"service":"22","number":"D802"},"field":{"byte":0}},)"
        R"({"key":"part_again","name":"Part again","kind":"text","ecu":"pcm","read":{"service":"22","number":"D802"},"field":{"byte":0}},)"
        R"({"key":"cal","name":"Cal","ecu":"pcm","read":{"service":"22","number":"F18C"},"field":{"byte":0,"bits":16}},)"
        R"({"key":"speed","name":"Speed","ecu":"pcm","read":{"service":"22","number":"D803"},"field":{"byte":0,"bits":16}}]})";
    const Loaded file(json);
    ASSERT_NE(file.file, nullptr);
    const std::string part = file.key("part");
    const std::string again = file.key("part_again");
    const std::string cal = file.key("cal");
    const std::string speed = file.key("speed");
    EXPECT_EQ(again, "part_again") << "the later of two at one address";
    ASSERT_NE(part, again);

    /* No signal line: only the set says what each row's signal is, and a
     * text value that reads as a number doesn't give itself away */
    Folder f;
    f.write("rec/signals.json", json);
    f.write("rec/events.log", events_of(rows::kFormat4, {}));
    f.write("rec/signals.csv",
            signals_csv("1000000," + part + ",123456,\n" + "2000000," + again +
                        ",654321,\n" + "3000000," + cal + ",4242,\n" +
                        "4000000," + speed + ",10,\n" + "5000000," + speed +
                        ",11,\n"));
    const Exported all(f, ECUIO_CSV_WIDE, {}, "rec");
    ASSERT_EQ(all.rc, ECUIO_OK) << all.detail;
    ASSERT_GE(all.lines.size(), 1u);
    EXPECT_EQ(all.lines[0], "Time (ms)," + speed);
    EXPECT_EQ(all.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 2u);
    EXPECT_EQ(all.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);

    for (const std::string& text : {part, again}) {
        const Exported column(f, ECUIO_CSV_WIDE, {text.c_str()}, "rec");
        EXPECT_EQ(column.rc, ECUIO_ERR_INVALID_ARGUMENT) << text;
        EXPECT_NE(column.detail.find("is a text signal, never a column"),
                  std::string::npos)
            << column.detail;
    }
    const Exported ident(f, ECUIO_CSV_WIDE, {cal.c_str()}, "rec");
    EXPECT_EQ(ident.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(ident.detail.find("is an identification signal, never a column"),
              std::string::npos)
        << ident.detail;
}

// A folder copy keeps a format 4 recording's events.log, its first line
// and signal lines as they are, and a range opens with a held line for a
// condition ON at its start, as format 3's does
TEST(RecordingKeysTest, AFolderCopyKeepsAFormat4RecordingsLines)
{
    Folder f;
    f.write("rec/events.log",
            std::string(rows::kFormat4) +
                "\nstarted  2023-11-14 22:13:20.250 (UTC)\n"
                "utc  2023-11-14T22:13:20.250Z\n"
                "profile  raw\n"
                "frame flags  complete\n"
                "signal  boost,Boost,number,kPa,0,0,2\n"
                "columns  local time, seconds since the recording started "
                "(host_us / 1e6 in the CSV files), kind, text\n"
                "\n"
                "22:13:21.250  +1.000  cond  "
                "boost_low,on,fired,-3.5,1,1000000,950000\n"
                "22:13:23.250  +3.000  cond  "
                "boost_low,off,,1,1,3000000,2950000\n"
                "22:13:25.250  +5.000  end  Disconnected by the user: 0 "
                "frames, 3 values, 0 records dropped, 0 notes dropped\n");
    f.write("rec/signals.csv",
            signals_csv("1000000,boost,12,kPa\n2000000,boost,13,kPa\n"
                        "3000000,boost,14,kPa\n"));
    const Copied c(f, 2000000, 2500000);
    ASSERT_EQ(c.rc, ECUIO_OK) << c.detail;
    EXPECT_EQ(c.events.rfind(std::string(rows::kFormat4) + "\n", 0), 0u)
        << c.events;
    EXPECT_NE(c.events.find("\nsignal  boost,Boost,number,kPa,0,0,2\n"),
              std::string::npos)
        << c.events;
    EXPECT_NE(c.events.find("  cond  boost_low,on,held,-3.5,1,2000000,"),
              std::string::npos)
        << c.events;
    EXPECT_EQ(c.events.find("boost_low,off"), std::string::npos)
        << "past the range";
    EXPECT_NE(c.signals.find("2000000,boost,13,kPa"), std::string::npos)
        << c.signals;
    EXPECT_EQ(c.signals.find("3000000,boost"), std::string::npos);
}

// The health report finds each of the set's values by the key the
// recording keeps it under: whatever name its signal line gives, a
// computed signal by its author's key, read from the rows or decoded
// from the frames. A format 4 baseline is read as any other.
TEST(RecordingKeysTest, TheHealthReportFindsAValueByItsKey)
{
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const ecuio_signal_def_t* rpm = nullptr;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        if (std::strcmp(obd2->defs[i].name, "RPM") == 0) {
            rpm = &obd2->defs[i];
        }
    }
    ASSERT_NE(rpm, nullptr);
    ASSERT_GT(rpm->plot_rank, 0) << "a key value";
    ecuio_signal_key_t rpm_key{};
    ASSERT_EQ(ecuio_signal_address_key(rpm, &rpm_key), ECUIO_OK);

    Folder f;
    f.write("renamed/events.log",
            events_of(rows::kFormat4,
                      {std::string("signal  ") + rpm_key.text +
                       ",Engine Speed,number,RPM,0,1,1"},
                      "obd2"));
    f.write("renamed/signals.csv", signals_csv(std::string("1000000,") +
                                               rpm_key.text + ",3000,RPM\n"));
    f.write("base/events.log",
            std::string(rows::kFormat4) +
                "\nstarted  2026-11-01 09:00:00.000 (UTC)\n"
                "utc  2026-11-01T09:00:00.000Z\n"
                "core  0.9.0\n"
                "uri  elm327://COM4?profile=obd2\n"
                "profile  obd2\n"
                "frame flags  complete\n"
                "\n"
                "09:00:01.250  +1.250  link  ok (state 0, 0 episodes)\n"
                "09:05:12.400  +312.400  end  0 records dropped, 0 notes "
                "dropped\n");
    const Reported renamed(f, "renamed", "base");
    ASSERT_EQ(renamed.rc, ECUIO_OK) << renamed.detail;
    EXPECT_EQ(renamed.report.values, 1u) << renamed.text;
    EXPECT_NE(renamed.text.find("3000"), std::string::npos) << renamed.text;
    EXPECT_NE(renamed.text.find(
                  "\nBaseline: the vehicle's baseline recording, started "
                  "2026-11-01T09:00:00Z, 312.400 s (elm327), profile obd2\n"),
              std::string::npos)
        << renamed.text;

    const std::string json =
        R"({"havne_signals":1,"name":"Keys","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"gear","name":"Gear","plot":1,"ecu":"pcm","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"twice","name":"Twice","plot":2,"kind":"computed","expr":"gear * 2"}]})";
    const Loaded file(json);
    ASSERT_NE(file.file, nullptr);
    const std::string gear = file.key("gear");
    f.write("rows/signals.json", json);
    f.write("rows/events.log", events_of(rows::kFormat4, {}));
    f.write("rows/signals.csv",
            signals_csv("1000000," + gear + ",5,\n1000000,twice,10,\n"));
    const Reported by_rows(f, "rows");
    ASSERT_EQ(by_rows.rc, ECUIO_OK) << by_rows.detail;
    EXPECT_EQ(by_rows.report.values, 2u) << by_rows.text;

    f.write("frames/signals.json", json);
    f.write("frames/events.log", events_of(rows::kFormat4, {}));
    f.write("frames/frames.csv", "host_us,timestamp_us,id,flags,data\n"
                                 "1000000,1000000,0x7E8,0x100,62 D1 00 05\n");
    const Reported by_frames(f, "frames");
    ASSERT_EQ(by_frames.rc, ECUIO_OK) << by_frames.detail;
    EXPECT_EQ(by_frames.report.values, 2u) << by_frames.text;
}

// A text that is one signal's key and another signal's name is the
// signal of that key, for the reader's find and for a named column
TEST(RecordingKeysTest, AKeyComesBeforeAnotherSignalsName)
{
    Folder f;
    f.write("events.log",
            events_of(rows::kFormat4,
                      {"signal  boost,Boost A,number,kPa,0,0,2",
                       "signal  7E8:62D100:3.0:8,boost,number,psi,0,0,2"}));
    f.write("signals.csv", signals_csv("1000000,boost,12,kPa\n"
                                       "1000000,7E8:62D100:3.0:8,3,psi\n"));
    Reader r(f.utf8());
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.find("boost"), 0u) << "the signal keyed boost";
    EXPECT_EQ(r.find("Boost A"), 0u);
    EXPECT_EQ(r.find("7E8:62D100:3.0:8"), 1u) << "its own key still finds it";

    const Exported keyed(f, ECUIO_CSV_WIDE, {"boost"});
    ASSERT_EQ(keyed.rc, ECUIO_OK) << keyed.detail;
    ASSERT_GE(keyed.lines.size(), 2u);
    EXPECT_EQ(keyed.lines[0], "Time (ms),Boost A (kPa)");
    EXPECT_EQ(keyed.lines[1], "1000,12");
    const Exported named(f, ECUIO_CSV_WIDE, {"7E8:62D100:3.0:8"});
    ASSERT_EQ(named.rc, ECUIO_OK) << named.detail;
    ASSERT_GE(named.lines.size(), 2u);
    EXPECT_EQ(named.lines[0], "Time (ms),boost (psi)");
    EXPECT_EQ(named.lines[1], "1000,3");
}
