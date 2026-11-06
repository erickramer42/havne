// File/layer: tests/test_recorder.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)
//
// Each case records into a folder of its own under TEMP/TMPDIR, removed
// at the end.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "link_health.h" /* core/src: a worker's link transitions */
#include "recorder.h"    /* core/src: the signal file's bytes */
#include "session.h"     /* core/src: the session's record link */
#include "sha256.h"      /* core/src: the file's hash */
#include "temp_name.h"

namespace fs = std::filesystem;

namespace {

template <typename Predicate> bool eventually(Predicate pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/* A recordings root of the case's own, removed at the end */
struct Root {
    fs::path path;
    Root()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr)
            tmp = std::getenv("TMPDIR");
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-recorder");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~Root()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_u8string(); }
};

/* A file's text, carriage returns dropped (the files are text mode) */
std::string read_text(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::string out;
    for (char c : s) {
        if (c != '\r')
            out += c;
    }
    return out;
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        out.push_back(line);
    return out;
}

/* The case's profile: a number at half a count a bit, and a text value */
ecuio_signal_def_t kDefs[2];
ecuio_signal_set_t kSet;
constexpr uint32_t kNumberId = 0x100;
constexpr uint32_t kTextId = 0x101;

void register_profile()
{
    static bool done = false;
    if (done)
        return;
    std::memset(kDefs, 0, sizeof kDefs);
    kDefs[0].id = 1;
    kDefs[0].message_id = kNumberId;
    kDefs[0].kind = ECUIO_VALUE_UINT;
    kDefs[0].bit_length = 8;
    kDefs[0].scale = 0.5;
    std::strcpy(kDefs[0].name, "Half");
    std::strcpy(kDefs[0].unit, "\xC2\xB0"
                               "C");
    kDefs[0].decimal_places = 1;
    kDefs[0].plot_rank = 2;
    kDefs[0].group = ECUIO_SIGNAL_GROUP_FUEL;
    kDefs[1].id = 2;
    kDefs[1].message_id = kTextId;
    kDefs[1].kind = ECUIO_VALUE_TEXT;
    std::strcpy(kDefs[1].name, "Words");
    kSet = ecuio_signal_set_t{};
    kSet.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    kSet.count = 2;
    kSet.defs = kDefs;
    ASSERT_EQ(ecuio_profile_register("rec-test", &kSet, nullptr), ECUIO_OK);
    done = true;
}

void dispatch(ecuio_session_t* s, uint32_t id, const std::string& bytes,
              uint32_t flags = 0)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = id;
    f.flags = flags;
    f.data_length = static_cast<uint16_t>(bytes.size());
    std::memcpy(f.data, bytes.data(), bytes.size());
    ecuio_session_dispatch_callbacks(s, &f);
}

struct Recording {
    ecuio_session_t* session = nullptr;
    ecuio_recorder_t* recorder = nullptr;
    std::string folder;

    Recording(const Root& root, const char* uri, uint32_t ring_bytes = 0,
              const char* label = nullptr, std::vector<const char*> header = {})
    {
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        o.ring_bytes = ring_bytes;
        const std::string keep = root.utf8();
        o.root_utf8 = keep.c_str();
        o.label_utf8 = label;
        o.header_lines = header.empty() ? nullptr : header.data();
        o.header_line_count = static_cast<uint32_t>(header.size());
        EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
            << ecuio_last_error_detail();
        if (recorder != nullptr) {
            char buf[1024];
            uint32_t len = 0;
            EXPECT_EQ(
                ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len),
                ECUIO_OK);
            folder = buf;
        }
        if (session != nullptr && recorder != nullptr) {
            EXPECT_EQ(ecuio_session_record(session, recorder), ECUIO_OK);
        }
    }

    /* The session destroyed, then the recorder closed, as the app does */
    ecuio_status_t finish(const char* why = "the test ended")
    {
        ecuio_session_destroy(session);
        session = nullptr;
        const ecuio_status_t st = ecuio_recorder_close(recorder, why);
        recorder = nullptr;
        return st;
    }

    ~Recording()
    {
        ecuio_session_destroy(session);
        ecuio_recorder_close(recorder, nullptr);
    }

    std::string file(const char* name) const
    {
        return read_text(fs::u8path(folder) / name);
    }
};

} // namespace

/* Open makes <root>/<stamp>[ <label>] with the three files; the label is
 * reduced to what a folder's name may hold, and the folder is absolute */
TEST(RecorderTest, OpenMakesTheFolderAndItsFiles)
{
    Root root;
    ecuio_record_options_t o{};
    const std::string r = root.utf8() + "/deeper/still";
    o.root_utf8 = r.c_str();
    o.label_utf8 = "  Daily  driver: <\xC3\xB8l>/? ..  ";
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    EXPECT_EQ(len, std::strlen(buf));
    const fs::path folder = fs::u8path(buf);
    EXPECT_TRUE(folder.is_absolute());
    const std::string name = folder.filename().u8string();
    ASSERT_GE(name.size(), 15u) << name;
    EXPECT_EQ(name[8], '-');
    EXPECT_EQ(name.substr(15), " Daily driver \xC3\xB8l");
    for (const char* f : {"frames.csv", "signals.csv", "events.log"}) {
        EXPECT_TRUE(fs::exists(folder / f)) << f;
    }
    EXPECT_FALSE(fs::exists(folder / "markers.csv"));
    EXPECT_FALSE(fs::exists(folder / "latency.csv"));

    /* A cut folder name says how long it is */
    char small[8];
    ASSERT_EQ(ecuio_recorder_get_folder(rec, small, sizeof small, &len),
              ECUIO_OK);
    EXPECT_EQ(std::strlen(small), 7u);
    EXPECT_EQ(len, std::strlen(buf));
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
}

/* The folder is the root as the caller spelled it, made absolute: a
 * relative root joins the working directory, "." and ".." go, and a
 * link in it stays (macOS's temporary directory is one, /var to
 * /private/var, and the app shows the folder relative to the root it
 * named) */
TEST(RecorderTest, TheFolderKeepsTheRootAsTheCallerSpelledIt)
{
    Root root;
    std::error_code ec;
    fs::create_directories(root.path / "real", ec);
    ASSERT_FALSE(ec) << ec.message();
    auto parent_of_folder = [](const std::string& r) {
        ecuio_record_options_t o{};
        o.root_utf8 = r.c_str();
        ecuio_recorder_t* rec = nullptr;
        EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
            << ecuio_last_error_detail();
        char buf[2048] = "";
        uint32_t len = 0;
        if (rec != nullptr) {
            EXPECT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len),
                      ECUIO_OK);
            EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
        }
        return fs::u8path(buf).parent_path();
    };
    EXPECT_EQ(parent_of_folder(root.utf8() + "/real/./x/../y"),
              root.path / "real" / "y");

    const std::string relative = temp_name("ecuio-recorder-relative");
    const fs::path cwd = fs::current_path();
    EXPECT_EQ(parent_of_folder(relative), cwd / relative);
    fs::remove_all(cwd / relative, ec);

#ifndef _WIN32
    /* Making a link on Windows needs a privilege; GetFullPathNameW never
     * follows one */
    fs::create_directory_symlink(root.path / "real", root.path / "link", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(parent_of_folder(root.utf8() + "/link"), root.path / "link");
#endif
}

/* Two recorders opened in one second never share a folder */
TEST(RecorderTest, TwoRecordersInOneSecondGetTwoFolders)
{
    Root root;
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    std::vector<ecuio_recorder_t*> recs(3, nullptr);
    std::vector<std::string> folders;
    for (ecuio_recorder_t*& rec : recs) {
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
        char buf[1024];
        uint32_t len = 0;
        ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
        folders.push_back(buf);
    }
    EXPECT_NE(folders[0], folders[1]);
    EXPECT_NE(folders[1], folders[2]);
    EXPECT_NE(folders[0], folders[2]);
    for (ecuio_recorder_t* rec : recs)
        ecuio_recorder_close(rec, nullptr);
}

/* What open and attach refuse */
TEST(RecorderTest, OpenAndAttachRefuseWhatTheyMust)
{
    Root root;
    const std::string r = root.utf8();
    ecuio_recorder_t* rec = nullptr;
    ecuio_record_options_t o{};
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_NULL_ARGUMENT);
    o.root_utf8 = r.c_str();
    o.flags = 1;
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_INVALID_ARGUMENT);
    o.flags = 0;
    o.reserved[2] = 1;
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_INVALID_ARGUMENT);
    o.reserved[2] = 0;
    EXPECT_EQ(rec, nullptr);

    /* A root that can't be a folder: a file stands where it would go */
    fs::create_directories(root.path);
    {
        std::ofstream(root.path / "a-file") << "x";
    }
    const std::string under_file = r + "/a-file/inside";
    o.root_utf8 = under_file.c_str();
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_RECORDING_FAILED);
    EXPECT_EQ(rec, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("can't create"),
              std::string::npos);
    EXPECT_EQ(std::string(ecuio_last_error_detail()).find("a-file"),
              std::string::npos)
        << "the detail never names the path";

    o.root_utf8 = r.c_str();
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    /* Notes and marks before a session attaches have no zero */
    EXPECT_EQ(ecuio_recorder_note(rec, "early"), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_mark(rec, "early"), ECUIO_ERR_INVALID_ARGUMENT);

    ecuio_session_t* started = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://started", &started), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(started), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(started, rec), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(started);

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://first", &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, rec), ECUIO_ERR_INVALID_ARGUMENT);

    /* One recorder holds one session; a session holds one recorder */
    ecuio_session_t* other = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://other", &other), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(other, rec), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_recorder_t* second = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &second), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, second), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_close(other);
    EXPECT_EQ(ecuio_session_record(other, second), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(other);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(second, nullptr), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
}

/* Every frame and every value, ADR-0017's rows; values are decoded for
 * the recording with no values callback registered; the header is
 * core's, the caller's lines under one key, every line with its kind */
TEST(RecorderTest, ARecordingHoldsFramesValuesAndItsHeader)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://rec?profile=rec-test", 0, nullptr,
                  {"surrogate frame logging  off", "two\nlines"});
    ASSERT_NE(rec.recorder, nullptr);
    dispatch(rec.session, kNumberId, std::string("\x03", 1));
    dispatch(rec.session, kTextId, "A,B \"q\"");
    dispatch(rec.session, 0x7E8, std::string("\x41\x0C\x1A\xF8", 4), 0x80);
    EXPECT_EQ(ecuio_recorder_note(rec.recorder, "Connected: mock://rec"),
              ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_mark(rec.recorder, "launch, hard"), ECUIO_OK);
    ASSERT_EQ(rec.finish("disconnected"), ECUIO_OK);

    const std::vector<std::string> frames = lines_of(rec.file("frames.csv"));
    ASSERT_EQ(frames.size(), 4u);
    EXPECT_EQ(frames[0], "host_us,timestamp_us,id,flags,data");
    EXPECT_NE(frames[1].find(",0,0x100,0x0,03"), std::string::npos)
        << frames[1];
    EXPECT_NE(frames[2].find(",0x101,0x0,41 2C 42 20 22 71 22"),
              std::string::npos)
        << frames[2];
    EXPECT_NE(frames[3].find(",0x7E8,0x80,41 0C 1A F8"), std::string::npos)
        << frames[3];

    const std::vector<std::string> values = lines_of(rec.file("signals.csv"));
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0], "\xEF\xBB\xBFhost_us,signal,value,unit");
    EXPECT_NE(values[1].find(",Half,1.5,\xC2\xB0"
                             "C"),
              std::string::npos)
        << values[1];
    EXPECT_NE(values[2].find(",Words,\"A,B \"\"q\"\"\","), std::string::npos)
        << values[2];

    const std::string events = rec.file("events.log");
    const std::vector<std::string> ev = lines_of(events);
    ASSERT_GE(ev.size(), 10u) << events;
    EXPECT_EQ(ev[0], "ecuio session recording, format 3 (ADR-0049)");
    EXPECT_EQ(ev[1].rfind("started  ", 0), 0u);
    EXPECT_EQ(ev[2].rfind("utc  ", 0), 0u);
    EXPECT_EQ(ev[2].back(), 'Z');
    EXPECT_NE(events.find("\nuri  mock://rec?profile=rec-test\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nprofile  rec-test\n"), std::string::npos);
    EXPECT_NE(events.find("\nframe flags  complete\n"), std::string::npos);
    EXPECT_NE(events.find("\nsignal  Half,number,\xC2\xB0"
                          "C,1,2,3\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nsignal  Words,text,,0,0,0\n"), std::string::npos);
    EXPECT_NE(events.find("\ncaller  surrogate frame logging  off\n"),
              std::string::npos);
    EXPECT_NE(events.find("\ncaller  two lines\n"), std::string::npos);
    EXPECT_NE(events.find("  note  Connected: mock://rec\n"), std::string::npos)
        << events;
    EXPECT_NE(events.find(
                  "  end  disconnected: 3 frames, 2 values, 0 records dropped, "
                  "0 notes dropped\n"),
              std::string::npos)
        << events;

    const std::vector<std::string> marks = lines_of(rec.file("markers.csv"));
    ASSERT_EQ(marks.size(), 2u);
    EXPECT_EQ(marks[0], "host_us,label");
    EXPECT_NE(marks[1].find(",\"launch, hard\""), std::string::npos)
        << marks[1];
}

/* Format 3 (ADR-0049's appendix of 2026-11-05): an enum's raw number in
 * signals.csv and its labels on label lines after its signal line, read
 * back through the reader. A label's comma and quotes are CSV-quoted, a
 * line break in one is a space; only the first def of a name writes
 * labels (the reader keeps that one's signal line), and an enum with no
 * entries or a number writes none. */
constexpr uint32_t kEnumId = 0x110;
ecuio_signal_def_t kEnumDefs[4];
ecuio_enum_entry_t kEnumEntries[4];
ecuio_signal_set_t kEnumSet;

ecuio_enum_entry_t entry(uint32_t raw, const char* label)
{
    ecuio_enum_entry_t e{};
    e.raw = raw;
    std::strcpy(e.label, label);
    return e;
}

void register_enum_profile()
{
    static bool done = false;
    if (done)
        return;
    std::memset(kEnumDefs, 0, sizeof kEnumDefs);
    for (int i = 0; i < 4; ++i) {
        ecuio_signal_def_t& d = kEnumDefs[i];
        d.id = static_cast<uint16_t>(i + 1);
        d.message_id = kEnumId + static_cast<uint32_t>(i);
        d.kind = ECUIO_VALUE_ENUM;
        d.bit_length = 8;
        d.scale = 1.0;
    }
    std::strcpy(kEnumDefs[0].name, "Fuel, System");
    kEnumDefs[0].table_first = 0;
    kEnumDefs[0].table_count = 3;
    kEnumDefs[0].plot_rank = 1;
    kEnumDefs[0].group = ECUIO_SIGNAL_GROUP_FUEL;
    std::strcpy(kEnumDefs[1].name, "Fuel, System"); /* its labels unwritten */
    kEnumDefs[1].table_first = 3;
    kEnumDefs[1].table_count = 1;
    std::strcpy(kEnumDefs[2].name, "Mode"); /* no entries */
    kEnumDefs[3].kind = ECUIO_VALUE_UINT;
    std::strcpy(kEnumDefs[3].name, "Count");
    kEnumEntries[0] = entry(1, "Open loop, cold");
    kEnumEntries[1] = entry(2, "Closed \"loop\"");
    kEnumEntries[2] = entry(4294967295u, "Two\nlines");
    kEnumEntries[3] = entry(9, "Other");
    kEnumSet = ecuio_signal_set_t{};
    kEnumSet.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    kEnumSet.count = 4;
    kEnumSet.defs = kEnumDefs;
    kEnumSet.enum_count = 4;
    kEnumSet.enums = kEnumEntries;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("rec-enum", &kEnumSet, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    done = true;
}

TEST(RecorderTest, AnEnumsNumberIsInTheRowsAndItsLabelsInTheHeader)
{
    register_enum_profile();
    Root root;
    Recording rec(root, "mock://enum?profile=rec-enum");
    ASSERT_NE(rec.recorder, nullptr);
    dispatch(rec.session, kEnumId, std::string("\x02", 1));
    dispatch(rec.session, kEnumId, std::string("\x03", 1)); /* no label */
    dispatch(rec.session, kEnumId + 3, std::string("\x07", 1));
    const std::string folder = rec.folder;
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> values = lines_of(rec.file("signals.csv"));
    ASSERT_EQ(values.size(), 4u);
    EXPECT_NE(values[1].find(",\"Fuel, System\",2,"), std::string::npos)
        << values[1];
    EXPECT_NE(values[2].find(",\"Fuel, System\",3,"), std::string::npos)
        << values[2];
    EXPECT_NE(values[3].find(",Count,7,"), std::string::npos) << values[3];

    const std::string events = rec.file("events.log");
    EXPECT_EQ(events.rfind("ecuio session recording, format 3 (ADR-0049)\n", 0),
              0u);
    EXPECT_NE(events.find("\nsignal  \"Fuel, System\",enum,,0,1,3\n"
                          "label  \"Fuel, System\",1,\"Open loop, cold\"\n"
                          "label  \"Fuel, System\",2,\"Closed \"\"loop\"\"\"\n"
                          "label  \"Fuel, System\",4294967295,Two lines\n"
                          "signal  \"Fuel, System\",enum,,0,0,0\n"
                          "signal  Mode,enum,,0,0,0\n"
                          "signal  Count,number,,0,0,0\n"),
              std::string::npos)
        << events;
    EXPECT_EQ(events.find(",9,Other"), std::string::npos) << events;

    /* Read back: the number as a number, the word beside it */
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_ENUMS);
    ASSERT_EQ(info.signal_count, 3u) << "one signal a name";
    ecuio_recording_signal_t fuel{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 0, &fuel), ECUIO_OK);
    EXPECT_STREQ(fuel.name, "Fuel, System");
    EXPECT_EQ(fuel.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(fuel.samples, 2u);
    EXPECT_EQ(fuel.lowest, 2.0);
    EXPECT_EQ(fuel.highest, 3.0);
    ecuio_enum_entry_t got[4] = {};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_recording_get_labels(r, 0, got, 4, &count), ECUIO_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(got[0].raw, 1u);
    EXPECT_STREQ(got[0].label, "Open loop, cold");
    EXPECT_STREQ(got[1].label, "Closed \"loop\"");
    EXPECT_EQ(got[2].raw, 4294967295u);
    EXPECT_STREQ(got[2].label, "Two lines");
    EXPECT_STREQ(ecuio_recording_enum_label(r, 0, 2), "Closed \"loop\"");
    ecuio_recording_signal_t mode{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 1, &mode), ECUIO_OK);
    EXPECT_EQ(mode.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(ecuio_recording_get_labels(r, 1, nullptr, 0, &count), ECUIO_OK);
    EXPECT_EQ(count, 0u);
    ecuio_recording_signal_t plain{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 2, &plain), ECUIO_OK);
    EXPECT_EQ(plain.flags, 0u);
    ecuio_recording_close(r);
}

/* A value is written with a '.' whatever LC_NUMERIC says, as a Qt
 * program's locale may say a comma */
TEST(RecorderTest, ValuesKeepTheirDecimalPointUnderAnyLocale)
{
    register_profile();
    const char* before = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = before != nullptr ? before : "C";
    if (std::setlocale(LC_NUMERIC, "de-DE") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr) {
        GTEST_SKIP() << "no German locale here";
    }
    Root root;
    Recording rec(root, "mock://locale?profile=rec-test");
    dispatch(rec.session, kNumberId, std::string("\x07", 1));
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    std::setlocale(LC_NUMERIC, saved.c_str());
    EXPECT_NE(rec.file("signals.csv").find(",Half,3.5,"), std::string::npos)
        << rec.file("signals.csv");
}

/* Notes after the session is destroyed still reach events.log; a note's
 * newlines are spaces, so it is one note line whatever it holds */
TEST(RecorderTest, NotesOutliveTheSessionAndStayOneLine)
{
    Root root;
    Recording rec(root, "mock://notes");
    ecuio_session_destroy(rec.session);
    rec.session = nullptr;
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    EXPECT_EQ(st.state, ECUIO_RECORDING_DETACHED);
    EXPECT_EQ(ecuio_recorder_note(rec.recorder, "Disconnected: mock://notes"),
              ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_note(rec.recorder, "asleep\nlink  forged"),
              ECUIO_OK);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::string events = rec.file("events.log");
    EXPECT_NE(events.find("  note  Disconnected: mock://notes\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("  note  asleep link  forged\n"), std::string::npos)
        << events;
    EXPECT_EQ(events.find("\nlink  forged"), std::string::npos) << events;
}

/* A writer held back fills the ring: the worker drops whole records and
 * counts them, never blocks, and the next record that fits says where */
TEST(RecorderTest, AFullRingDropsAndCountsRecords)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://full?profile=rec-test", 512);
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_recorder_test_hold(rec.recorder, 1);
    for (int i = 0; i < 100; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    EXPECT_GT(st.records_dropped, 0u);
    const uint64_t dropped = st.records_dropped;
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 100 - dropped;
        },
        2000))
        << st.frame_rows;
    dispatch(rec.session, kNumberId, std::string("\x01", 1));
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    EXPECT_NE(events.find("  drop  the recording dropped " +
                          std::to_string(dropped) +
                          " frames: the disk fell behind\n"),
              std::string::npos)
        << events;
    EXPECT_NE(events.find(std::to_string(dropped) + " records dropped"),
              std::string::npos);
    EXPECT_EQ(lines_of(rec.file("frames.csv")).size(), 1 + 101 - dropped);
}

/* A failed write stops the recording, not the session: the state says
 * why, notes are refused, and close reports it but frees the handle */
TEST(RecorderTest, AFailedWriteStopsTheRecordingNotTheSession)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://fail?profile=rec-test");
    ecuio_recorder_test_fail_writes(rec.recorder);
    dispatch(rec.session, kNumberId, std::string("\x02", 1));
    ecuio_recorder_state_t st{};
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.state == ECUIO_RECORDING_FAILED;
        },
        2000));
    EXPECT_STREQ(st.reason, "the disk is full");
    EXPECT_EQ(ecuio_recorder_note(rec.recorder, "late"),
              ECUIO_ERR_RECORDING_FAILED);
    EXPECT_EQ(ecuio_recorder_mark(rec.recorder, "late"),
              ECUIO_ERR_RECORDING_FAILED);
    /* The session takes frames as before */
    for (int i = 0; i < 50; ++i) {
        dispatch(rec.session, kNumberId, std::string("\x02", 1));
    }
    EXPECT_EQ(rec.finish(), ECUIO_ERR_RECORDING_FAILED);
}

/* Closing the recorder while its session runs detaches it: the session
 * goes on unrecorded, and the files end whole with their summary. A
 * close while the worker dispatches waits it out (ASan and TSan see it). */
TEST(RecorderTest, ClosingWhileAttachedDetachesAndTheSessionRunsOn)
{
    Root root;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://live", &s), ECUIO_OK);
    std::atomic<int> frames{0};
    auto count = [](const ecuio_frame_t*, void* user) {
        static_cast<std::atomic<int>*>(user)->fetch_add(1);
    };
    ASSERT_EQ(ecuio_register_frame_callback(s, count, &frames), ECUIO_OK);
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char buf[1024];
    uint32_t len = 0;
    ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ASSERT_TRUE(eventually([&] { return frames.load() >= 4; }, 3000));
    EXPECT_EQ(ecuio_recorder_close(rec, "the app quit"), ECUIO_OK);
    const int at_close = frames.load();
    ASSERT_TRUE(eventually([&] { return frames.load() >= at_close + 4; }, 3000))
        << "the session runs on";
    ecuio_session_destroy(s);
    const std::string events = read_text(folder / "events.log");
    EXPECT_NE(events.find("  end  the app quit: "), std::string::npos)
        << events;
    EXPECT_GE(lines_of(read_text(folder / "frames.csv")).size(), 5u);
}

/* The replay fixture recorded: every frame as the recording has it
 * (ID, flags, bytes), and every value obd2 decodes */
TEST(RecorderTest, ARecordedReplayHoldsEveryFrameOfItsSource)
{
    const std::string source =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    const std::vector<std::string> want =
        lines_of(read_text(fs::u8path(source) / "frames.csv"));
    ASSERT_GT(want.size(), 100u);
    Root root;
    const std::string uri = "replay://" + source + "?speed=max&profile=obd2";
    Recording rec(root, uri.c_str());
    ASSERT_NE(rec.recorder, nullptr);
    std::atomic<size_t> seen{0};
    auto count = [](const ecuio_frame_t*, void* user) {
        static_cast<std::atomic<size_t>*>(user)->fetch_add(1);
    };
    ASSERT_EQ(ecuio_register_frame_callback(rec.session, count, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    ASSERT_TRUE(
        eventually([&] { return seen.load() >= want.size() - 1; }, 20000))
        << seen.load();
    ecuio_recorder_state_t st{};
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> got = lines_of(rec.file("frames.csv"));
    ASSERT_EQ(got.size(), want.size());
    EXPECT_EQ(got[0], want[0]);
    /* id, flags and data: everything after the second comma */
    auto tail = [](const std::string& row) {
        const size_t a = row.find(',');
        return row.substr(row.find(',', a + 1));
    };
    for (size_t i = 1; i < got.size(); ++i) {
        ASSERT_EQ(tail(got[i]), tail(want[i])) << "row " << i;
    }
    EXPECT_GT(lines_of(rec.file("signals.csv")).size(), 100u);
    const std::string events = rec.file("events.log");
    EXPECT_NE(events.find("\nframe flags  from an older recording\n"),
              std::string::npos);
    (void)st;
}

namespace {

/* events.log's lines of one kind: their text, and their seconds since
 * the start in ms */
struct EventLine {
    uint64_t ms;
    std::string text;
};

std::vector<EventLine> events_of(const std::string& events, const char* kind)
{
    std::vector<EventLine> out;
    const std::string mark = std::string("  ") + kind + "  ";
    for (const std::string& line : lines_of(events)) {
        const size_t plus = line.find("  +");
        const size_t at = line.find(mark);
        if (plus == std::string::npos || at == std::string::npos || at < plus)
            continue;
        const std::string secs = line.substr(plus + 3, at - plus - 3);
        const size_t dot = secs.find('.');
        if (dot == std::string::npos)
            continue;
        out.push_back(EventLine{std::stoull(secs.substr(0, dot)) * 1000 +
                                    std::stoull(secs.substr(dot + 1)),
                                line.substr(at + mark.size())});
    }
    return out;
}

/* A ring the 1-byte frames of the case's number fill to its last byte:
 * each record is its head (16), the frame's (32), the byte and the value
 * (16), so a link record (32) can't fit either */
constexpr uint32_t kFrameRecord = 65;
constexpr uint32_t kExactRing = 7 * kFrameRecord;

/* The writer held, once any drain under way has ended */
void hold_writer(ecuio_recorder_t* r)
{
    ecuio_recorder_test_hold(r, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

/* A request's reply timing as a worker publishes it */
ecuio_request_timing_t published_row(uint8_t pid, uint32_t answered)
{
    ecuio_request_timing_t r{};
    r.length = 2;
    r.request[0] = 0x01;
    r.request[1] = pid;
    r.answered = answered;
    return r;
}

} // namespace

/* Each link transition a worker publishes is a link line at its own
 * time, among the frames in order: its words, the state and the episode
 * count (ADR-0049 section 2) */
TEST(RecorderTest, ALinkTransitionIsALinkLineAtItsTime)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://link?profile=rec-test");
    ASSERT_NE(rec.session, nullptr);
    /* The test is the worker: the session never starts */
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    dispatch(rec.session, kNumberId, std::string("\x02", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        link_count_quiet(h);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    dispatch(rec.session, kNumberId, std::string("\x04", 1));
    link_on_read(h, true, false, false);
    link_on_read(h, true, false, false); /* no transition: no line */
    link_on_read(h, false, true, false);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 3u) << events;
    EXPECT_EQ(links[0].text, "vehicle not responding (state 8, 0 episodes)");
    EXPECT_EQ(links[1].text, "adapter fault (state 9, 1 episode)");
    EXPECT_EQ(links[2].text, "recovered (state 0, 1 episode)");
    /* The timeout's line sits between the two frames' times */
    const std::vector<std::string> frames = lines_of(rec.file("frames.csv"));
    ASSERT_EQ(frames.size(), 3u);
    const uint64_t first_ms = std::stoull(frames[1]) / 1000;
    const uint64_t second_ms = std::stoull(frames[2]) / 1000;
    EXPECT_GT(links[0].ms, first_ms + 10) << events;
    EXPECT_LT(links[0].ms + 10, second_ms) << events;
}

/* A replay whose file changed after attach stops, and its recording says
 * why in a link line: replay publishes the state itself, not through
 * link_health */
TEST(RecorderTest, AReplayThatStopsEarlySaysSoInALinkLine)
{
    Root root;
    fs::create_directories(root.path);
    const fs::path csv = root.path / "source.csv";
    const auto write = [&](int rows, const char* tail) {
        std::string text = "host_us,timestamp_us,id,flags,data\n";
        for (int i = 0; i < rows; ++i) {
            const std::string t = std::to_string(5000000 + 100000 * i);
            text += t + "," + t + ",0x7E8,0x0,41 0C 1F 40\n";
        }
        text += tail;
        std::ofstream(csv, std::ios::binary) << text;
    };
    write(5, "");
    const std::string uri = "replay://" + csv.generic_u8string() + "?speed=max";
    Recording rec(root, uri.c_str());
    ASSERT_NE(rec.recorder, nullptr);
    write(3, "5300000,5300000,0x7E8,0x0,41 0"); /* cut mid-row */
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    int32_t last = ECUIO_OK;
    uint32_t episodes = 0;
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_session_get_error_state(rec.session, &last, &episodes);
            return last != ECUIO_OK;
        },
        3000));
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::vector<EventLine> links =
        events_of(rec.file("events.log"), "link");
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].text, "replay stopped: its recording changed after it "
                             "was opened (state 16, 1 episode)");
}

/* A transition that finds the ring full is carried: a later one takes
 * its place, and the latest goes in ahead of the next record that fits,
 * so a drop loses frames but never the link's state */
TEST(RecorderTest, ALinkTransitionThatFindsTheRingFullIsCarried)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://carry?profile=rec-test", kExactRing);
    ASSERT_NE(rec.recorder, nullptr);
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    hold_writer(rec.recorder);
    for (int i = 0; i < 100; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    link_on_write(h, false);             /* requests failing, episode 1 */
    link_on_read(h, true, false, false); /* an adapter fault, episode 2 */
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    const uint64_t dropped = st.records_dropped;
    ASSERT_EQ(dropped, 93u) << "the ring full to its last byte";
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 100 - dropped;
        },
        2000));
    dispatch(rec.session, kNumberId, std::string("\x01", 1));
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 1u) << events;
    EXPECT_EQ(links[0].text, "adapter fault (state 9, 2 episodes)");
    const size_t link_at = events.find("  link  ");
    const size_t drop_at = events.find("  drop  the recording dropped " +
                                       std::to_string(dropped) + " frames");
    ASSERT_NE(drop_at, std::string::npos) << events;
    EXPECT_LT(link_at, drop_at) << "the carried state goes in first";
    EXPECT_EQ(lines_of(rec.file("frames.csv")).size(), 1 + 101 - dropped);
}

/* A state still carried when the worker lets go, and drops no record
 * came after, are written at the close, ahead of the summary */
TEST(RecorderTest, AStateStillCarriedAtTheEndIsWritten)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://carried?profile=rec-test", kExactRing);
    ASSERT_NE(rec.recorder, nullptr);
    link_health h(rec.session->last_error, rec.session->error_count,
                  rec.session->record_link.get());
    hold_writer(rec.recorder);
    for (int i = 0; i < 100; ++i) {
        dispatch(rec.session, kNumberId, std::string(1, static_cast<char>(i)));
    }
    link_on_write(h, false);
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    const uint64_t dropped = st.records_dropped;
    ASSERT_EQ(dropped, 93u) << "the ring full to its last byte";
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::string events = rec.file("events.log");
    const std::vector<EventLine> links = events_of(events, "link");
    ASSERT_EQ(links.size(), 1u) << events;
    EXPECT_EQ(links[0].text, "requests failing (state 10, 1 episode)");
    const size_t drop_at = events.find("  drop  the recording dropped " +
                                       std::to_string(dropped) + " frames");
    ASSERT_NE(drop_at, std::string::npos) << events;
    EXPECT_LT(drop_at, events.find("  end  ")) << events;
}

/* Reply timing reaches latency.csv every 10 s of session time and at the
 * end, each request's row only when its counts changed (ADR-0017's
 * appendix of 2026-10-31), under its channel's ECU (the mock's is
 * 0x7E8); the file is made with its first row */
TEST(RecorderTest, ReplyTimingGoesToLatencyCsvEveryTenSeconds)
{
    Root root;
    Recording rec(root, "mock://timing");
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_record_link* link = rec.session->record_link.get();
    ecuio_session_timing_t t{};
    t.interval_clock = ECUIO_TIMING_CLOCK_HOST;
    ecuio_request_timing_t rows[2] = {published_row(0x0C, 5),
                                      published_row(0x0D, 0)};
    rows[0].timeouts = 1;
    rows[0].flags = ECUIO_TIMING_LATENCY_P99;
    rows[0].latency_p50_us = 12000;
    rows[0].latency_p99_us = 14000;
    rows[0].latency_max_us = 15000;
    rows[0].intervals = 4;
    rows[0].interval_p50_us = 100000;
    rows[0].interval_max_us = 120000;

    t.snapshot_us = 5000000; /* before the first 10 s: nothing */
    ecuio_record_timing(link, t, rows, 2, false);
    /* Past the writer's idle wake, so a file made would be there */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_FALSE(fs::exists(fs::u8path(rec.folder) / "latency.csv"));
    t.snapshot_us = 10000000;
    ecuio_record_timing(link, t, rows, 2, false);
    rows[0].answered = 6;
    t.snapshot_us = 12000000; /* not due */
    ecuio_record_timing(link, t, rows, 2, false);
    rows[1].refused = 1;
    t.snapshot_us = 20400000;
    ecuio_record_timing(link, t, rows, 2, false);
    rows[1].refused = 2;
    t.snapshot_us = 21000000; /* the final one, whenever it comes */
    ecuio_record_timing(link, t, rows, 2, true);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> got = lines_of(rec.file("latency.csv"));
    const std::vector<std::string> want = {
        ECUIO_TIMING_COLUMNS,
        "10.000,0x7E8,01 0C,5,0,1,0,12000,14000,,15000,4,0,100000,,,120000,"
        "host",
        "20.400,0x7E8,01 0C,6,0,1,0,12000,14000,,15000,4,0,100000,,,120000,"
        "host",
        "20.400,0x7E8,01 0D,0,1,0,0,0,,,0,0,0,,,,,host",
        "21.000,0x7E8,01 0D,0,2,0,0,0,,,0,0,0,,,,,host",
    };
    EXPECT_EQ(got, want);
}

/* The final snapshot, the worker's last record, waits up to 1 s for the
 * writer to make room; with none, it's dropped and counted */
TEST(RecorderTest, TheFinalTimingWaitsASecondForRoom)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://final?profile=rec-test", 512);
    ASSERT_NE(rec.recorder, nullptr);
    ecuio_record_link* link = rec.session->record_link.get();
    ecuio_session_timing_t t{};
    t.snapshot_us = 3000000;
    const ecuio_request_timing_t row = published_row(0x0C, 3);
    const auto fill = [&] {
        for (int i = 0; i < 100; ++i) {
            dispatch(rec.session, kNumberId,
                     std::string(1, static_cast<char>(i)));
        }
    };

    hold_writer(rec.recorder);
    fill();
    ecuio_recorder_state_t st{};
    ecuio_recorder_get_state(rec.recorder, &st);
    const uint64_t frames_dropped = st.records_dropped;
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ecuio_recorder_test_hold(rec.recorder, 0);
    });
    auto begun = std::chrono::steady_clock::now();
    ecuio_record_timing(link, t, &row, 1, true);
    const auto waited = std::chrono::steady_clock::now() - begun;
    release.join();
    EXPECT_GE(waited, std::chrono::milliseconds(150));
    EXPECT_LT(waited, std::chrono::milliseconds(1000));
    ecuio_recorder_get_state(rec.recorder, &st);
    EXPECT_EQ(st.records_dropped, frames_dropped) << "the snapshot went in";

    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(rec.recorder, &st);
            return st.frame_rows == 100 - frames_dropped;
        },
        2000));
    hold_writer(rec.recorder);
    fill();
    ecuio_recorder_get_state(rec.recorder, &st);
    const uint64_t before = st.records_dropped;
    begun = std::chrono::steady_clock::now();
    ecuio_record_timing(link, t, &row, 1, true);
    EXPECT_GE(std::chrono::steady_clock::now() - begun,
              std::chrono::milliseconds(950));
    ecuio_recorder_get_state(rec.recorder, &st);
    EXPECT_EQ(st.records_dropped, before + 1) << "no room came";
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::vector<std::string> got = lines_of(rec.file("latency.csv"));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].rfind("3.000,0x7E8,01 0C,3,", 0), 0u) << got[1];
}

/* A replay's recording writes its source's frame flags line, so a
 * recording core made stays complete through any number of replays;
 * anything else is an older recording's (ADR-0049 section 2) */
TEST(RecorderTest, AReplayPassesOnItsSourcesFrameFlagsLine)
{
    register_profile();
    Root root;
    std::string source;
    {
        Recording rec(root, "mock://source?profile=rec-test");
        ASSERT_NE(rec.recorder, nullptr);
        dispatch(rec.session, kNumberId, std::string("\x02", 1),
                 ECUIO_FRAME_FLAG_REASSEMBLED);
        dispatch(rec.session, kNumberId, std::string("\x04", 1));
        source = rec.folder;
        ASSERT_EQ(rec.finish(), ECUIO_OK);
    }
    const auto replayed = [&](const std::string& path) {
        const std::string uri = "replay://" + path + "?speed=max";
        Recording rec(root, uri.c_str());
        EXPECT_NE(rec.recorder, nullptr);
        if (rec.recorder == nullptr)
            return std::string();
        ecuio_session_start(rec.session);
        ecuio_recorder_state_t st{};
        EXPECT_TRUE(eventually(
            [&] {
                ecuio_recorder_get_state(rec.recorder, &st);
                return st.frame_rows == 2;
            },
            3000));
        const std::string folder = rec.folder;
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        return folder;
    };
    const auto flags_line = [](const std::string& folder) {
        for (const std::string& line :
             lines_of(read_text(fs::u8path(folder) / "events.log"))) {
            if (line.rfind("frame flags  ", 0) == 0)
                return line;
        }
        return std::string();
    };

    const std::string first = replayed(source);
    EXPECT_EQ(flags_line(first), "frame flags  complete");
    const std::vector<std::string> rows =
        lines_of(read_text(fs::u8path(first) / "frames.csv"));
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NE(rows[1].find(",0x100,0x100,02"), std::string::npos) << rows[1];
    EXPECT_NE(rows[2].find(",0x100,0x0,04"), std::string::npos) << rows[2];
    EXPECT_EQ(flags_line(replayed(first + "/frames.csv")),
              "frame flags  complete")
        << "passed on again";

    /* A word core doesn't write, an older header, a .csv of another name */
    const fs::path odd = root.path / "odd";
    fs::create_directories(odd);
    fs::copy_file(fs::u8path(source) / "frames.csv", odd / "frames.csv");
    std::ofstream(odd / "events.log", std::ios::binary)
        << "ecuio session recording, format 2 (ADR-0049)\n"
           "frame flags  everything\n\n";
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  from an older recording");
    std::ofstream(odd / "events.log", std::ios::binary)
        << "Havne session recording\nframe flags  complete\n\n";
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  from an older recording");
    fs::copy_file(fs::u8path(source) / "frames.csv", odd / "other.csv");
    fs::copy_file(fs::u8path(source) / "events.log", odd / "events.log",
                  fs::copy_options::overwrite_existing);
    EXPECT_EQ(flags_line(replayed((odd / "other.csv").generic_u8string())),
              "frame flags  from an older recording");
    EXPECT_EQ(flags_line(replayed(odd.generic_u8string())),
              "frame flags  complete")
        << "the same events.log beside its own frames.csv";
}

/* The writer writes nothing before a session attaches, however long the
 * caller takes: an attach past its wakes still puts the header first,
 * and a recorder closed unattached leaves its files empty */
TEST(RecorderTest, NothingIsWrittenBeforeASessionAttaches)
{
    register_profile();
    Root root;
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    const char* caller[] = {"app  Havne test"};
    o.header_lines = caller;
    o.header_line_count = 1;
    for (const bool attach : {false, true}) {
        SCOPED_TRACE(attach ? "attached late" : "never attached");
        ecuio_recorder_t* rec = nullptr;
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
        char buf[1024];
        uint32_t len = 0;
        ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
        const fs::path folder = fs::u8path(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ecuio_session_t* s = nullptr;
        if (attach) {
            ASSERT_EQ(ecuio_session_create("mock://late?profile=rec-test", &s),
                      ECUIO_OK);
            ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
            dispatch(s, kNumberId, std::string("\x02", 1));
            ecuio_session_destroy(s);
        }
        EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
        const std::string events = read_text(folder / "events.log");
        if (!attach) {
            for (const char* f : {"frames.csv", "signals.csv", "events.log"}) {
                EXPECT_EQ(fs::file_size(folder / f), 0u) << f;
            }
            continue;
        }
        EXPECT_EQ(events.rfind("ecuio session recording, format 3", 0), 0u)
            << events;
        EXPECT_NE(events.find("\ncaller  app  Havne test\n"), std::string::npos)
            << events;
        const std::vector<std::string> frames =
            lines_of(read_text(folder / "frames.csv"));
        ASSERT_EQ(frames.size(), 2u);
        EXPECT_EQ(frames[0], "host_us,timestamp_us,id,flags,data");
    }
}

/* A sleep of the machine while recording is an asleep line, stamped as
 * the writer woke, with its length; a rise under a second, or a sleep
 * before the session attached, has none */
TEST(RecorderTest, ASleepIsAnAsleepLineWithItsLength)
{
    Root root;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://sleep", &s), ECUIO_OK);
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char buf[1024];
    uint32_t len = 0;
    ecuio_recorder_get_folder(rec, buf, sizeof buf, &len);
    const fs::path folder = fs::u8path(buf);
    ecuio_recorder_test_add_slept(rec, 5000000); /* before attach */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* The recording's zero is read inside the attach */
    const uint64_t attach_from = ecuio_clock_us();
    ASSERT_EQ(ecuio_session_record(s, rec), ECUIO_OK);
    const uint64_t attach_to = ecuio_clock_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* The sleep goes in just after a wake has taken its look, so the next
     * look is most of a wake away: a writer that stamped a wake late
     * couldn't pass. The writer counts a wake before it looks, so the
     * test lets 5 ms more go by, spinning: a sleep_for's own lateness
     * (15.6 ms on Windows) would eat into the wake */
    const uint64_t woke = ecuio_recorder_test_wakes(rec);
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ecuio_recorder_test_wakes(rec) == woke &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    const auto looked =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    while (std::chrono::steady_clock::now() < looked) {
        std::this_thread::yield();
    }
    const uint64_t added_from = ecuio_clock_us();
    ecuio_recorder_test_add_slept(rec, 2340000);
    const uint64_t added_to = ecuio_clock_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ecuio_recorder_test_add_slept(rec, 600000); /* under a second */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);

    const std::string events = read_text(folder / "events.log");
    EXPECT_EQ(events.rfind("ecuio session recording, format 3", 0), 0u)
        << "the header first, though attach came past the writer's wake";
    const std::vector<EventLine> sleeps = events_of(events, "asleep");
    ASSERT_EQ(sleeps.size(), 1u) << events;
    EXPECT_EQ(sleeps[0].text, "Asleep for 2.3 s: nothing was read meanwhile");
    /* Stamped at the wake that saw it, on the test's own clock: no
     * earlier than the sleep went in, no later than one idle wake after,
     * and a late wake's margin, kept under a wake so that a stamp a wake
     * late fails. Both from the recording's zero, somewhere in the attach;
     * the stamp is whole ms */
    constexpr uint64_t kWakeUs = 100000;  /* the writer's idle wake */
    constexpr uint64_t kMarginUs = 90000; /* a timer firing late */
    const uint64_t earliest_ms = (added_from - attach_to) / 1000;
    const uint64_t latest_ms =
        (added_to + kWakeUs + kMarginUs - attach_from) / 1000 + 1;
    ::testing::Test::RecordProperty("asleep_ms", std::to_string(sleeps[0].ms));
    ::testing::Test::RecordProperty("asleep_window_ms",
                                    std::to_string(earliest_ms) + "-" +
                                        std::to_string(latest_ms));
    EXPECT_GE(sleeps[0].ms, earliest_ms) << events;
    EXPECT_LE(sleeps[0].ms, latest_ms) << events;
}

/* The writer wakes every 100 ms while idle, so an idle recording costs a
 * phone's background session ten wakes a second, and every 10 ms while
 * the ring has records (ADR-0049 section 1) */
TEST(RecorderTest, AnIdleWriterWakesTenTimesASecond)
{
    register_profile();
    Root root;
    Recording rec(root, "mock://wakes?profile=rec-test");
    ASSERT_NE(rec.recorder, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t idle_from = ecuio_recorder_test_wakes(rec.recorder);
    /* Two seconds: a writer that waits a second wakes 3 times at most in
     * them, and the macOS runner, whose timers fire late, counted 4 a
     * second */
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint64_t idle_2s =
        ecuio_recorder_test_wakes(rec.recorder) - idle_from;
    ::testing::Test::RecordProperty("idle_wakes_2s", std::to_string(idle_2s));
    EXPECT_GE(idle_2s, 5u);
    EXPECT_LE(idle_2s, 40u) << "an idle writer wakes every 100 ms";
    const uint64_t idle = idle_2s / 2;

    /* One frame waiting at every wake: the next goes in as soon as the
     * writer has written the last, so no wake finds the ring empty and
     * none spends its time on a full one */
    uint64_t sent = 0;
    const auto busy_second = [&] {
        const uint64_t from = ecuio_recorder_test_wakes(rec.recorder);
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end) {
            ecuio_recorder_state_t st{};
            ecuio_recorder_get_state(rec.recorder, &st);
            if (st.frame_rows == sent) {
                dispatch(rec.session, kNumberId, std::string("\x02", 1));
                ++sent;
            }
            std::this_thread::yield();
        }
        return ecuio_recorder_test_wakes(rec.recorder) - from;
    };
    /* Against the idle second just measured, not a count: a desktop
     * wakes about 57 times (Windows' 15.6 ms timer), the macOS runner 18
     * (its timers fire late), and a busy wake no shorter than the idle
     * one wakes as often as idle. Up to three seconds, the best kept: a
     * runner busy with other tests has left the writer no turn for a
     * whole second (0 wakes on CI's Windows), while a writer waking at
     * the idle pace falls short in every one */
    uint64_t busy = 0;
    for (int attempt = 0; attempt < 3 && busy < idle + 5; ++attempt) {
        busy = std::max(busy, busy_second());
    }
    ::testing::Test::RecordProperty("busy_wakes", std::to_string(busy));
    EXPECT_GE(busy, idle + 5) << "a busy writer wakes every 10 ms";
}

/* The writer syncs its files to the storage every 2 s, so a power cut
 * loses at most that */
TEST(RecorderTest, TheWriterSyncsEveryTwoSeconds)
{
    Root root;
    Recording rec(root, "mock://sync");
    EXPECT_EQ(ecuio_recorder_test_syncs(rec.recorder), 0u);
    EXPECT_TRUE(eventually(
        [&] { return ecuio_recorder_test_syncs(rec.recorder) >= 1; }, 3500));
    EXPECT_LE(ecuio_recorder_test_syncs(rec.recorder), 2u);
}

namespace {

/* A file's bytes as they are */
std::string read_bytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* The signal-file fixture loaded and registered as its profile,
 * unregistered and freed at the end */
struct LoadedFile {
    std::string bytes;
    ecuio_signal_file_t* file = nullptr;
    std::string profile;

    explicit LoadedFile(const std::string& path)
        : bytes(read_bytes(fs::u8path(path)))
    {
        EXPECT_EQ(ecuio_signal_file_load(bytes.data(), bytes.size(), &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        if (file != nullptr) {
            profile = ecuio_signal_file_profile(file);
            EXPECT_EQ(ecuio_profile_register(profile.c_str(),
                                             ecuio_signal_file_set(file),
                                             nullptr),
                      ECUIO_OK);
        }
    }
    ~LoadedFile()
    {
        if (file != nullptr) {
            (void)ecuio_profile_unregister(profile.c_str());
        }
        ecuio_signal_file_free(file);
    }
    const ecuio_signal_set_t* set() const
    {
        return ecuio_signal_file_set(file);
    }
};

const std::string kExtras = std::string(ECUIO_SIGNAL_FIXTURES) + "/extras.json";

/* A recorder opened under root and given file (ecuio_recorder_signal_file)
 * before the session on uri attaches; with no file, set alone, as a set
 * no one file holds */
struct FileRecording {
    ecuio_session_t* session = nullptr;
    ecuio_recorder_t* recorder = nullptr;
    std::string folder;
    ecuio_status_t attached = ECUIO_ERR_NULL_ARGUMENT;

    FileRecording(const Root& root, const std::string& uri,
                  const ecuio_signal_file_t* file,
                  const ecuio_signal_set_t* set = nullptr)
    {
        EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        const std::string keep = root.utf8();
        o.root_utf8 = keep.c_str();
        EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK);
        if (recorder == nullptr || session == nullptr) {
            return;
        }
        char buf[1024];
        uint32_t len = 0;
        (void)ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len);
        folder = buf;
        EXPECT_EQ(file != nullptr
                      ? ecuio_recorder_signal_file(recorder, file)
                      : ecuio_recorder_take_signals(recorder, set, nullptr, 0),
                  ECUIO_OK);
        attached = ecuio_session_record(session, recorder);
    }
    ecuio_status_t finish()
    {
        ecuio_session_destroy(session);
        session = nullptr;
        const ecuio_status_t st = ecuio_recorder_close(recorder, "done");
        recorder = nullptr;
        return st;
    }
    ~FileRecording()
    {
        ecuio_session_destroy(session);
        ecuio_recorder_close(recorder, nullptr);
    }
    fs::path path(const char* name) const { return fs::u8path(folder) / name; }
};

ecuio_recording_signals_t signals_of(const std::string& folder)
{
    ecuio_recording_signals_t s{};
    ecuio_recording_t* r = nullptr;
    EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_recording_get_signals(r, &s), ECUIO_OK);
    ecuio_recording_close(r);
    return s;
}

/* signals.csv's rows without their host_us: what was decoded, in order */
std::vector<std::string> decoded_rows(const fs::path& csv)
{
    std::vector<std::string> out;
    const std::vector<std::string> rows = lines_of(read_text(csv));
    for (size_t i = 1; i < rows.size(); ++i) {
        out.push_back(rows[i].substr(rows[i].find(',') + 1));
    }
    return out;
}

} // namespace

/* The signal file that decodes a session is copied into its recording
 * byte for byte, its SHA-256 on the header after the profile, and the
 * reader says whether the bytes there are still those (ADR-0051 section
 * 4) */
TEST(RecorderTest, ASignalFileIsCopiedWithItsHashOnTheHeader)
{
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    FileRecording rec(root, "mock://x?profile=" + f.profile, f.file);
    ASSERT_EQ(rec.attached, ECUIO_OK);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    EXPECT_EQ(read_bytes(rec.path("signals.json")), f.bytes);
    const std::string sha =
        ecuio_sha256::hex_of(f.bytes.data(), f.bytes.size());
    EXPECT_NE(read_text(rec.path("events.log"))
                  .find("\nprofile  " + f.profile +
                        "\nsignal file  signals.json," + sha + "\n"),
              std::string::npos);

    ecuio_recording_signals_t s = signals_of(rec.folder);
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED |
                           ECUIO_RECORDING_SIGNALS_PRESENT |
                           ECUIO_RECORDING_SIGNALS_MATCH);
    EXPECT_EQ(s.length, f.bytes.size());
    EXPECT_EQ(std::string(s.sha256), sha);

    /* One byte changed: there, but not the bytes that decoded it */
    {
        std::ofstream out(rec.path("signals.json"), std::ios::binary);
        out << f.bytes << ' ';
    }
    s = signals_of(rec.folder);
    EXPECT_EQ(s.flags,
              ECUIO_RECORDING_SIGNALS_NAMED | ECUIO_RECORDING_SIGNALS_PRESENT);
    EXPECT_EQ(s.length, f.bytes.size() + 1);
    EXPECT_EQ(std::string(s.sha256), sha);

    /* Gone: named only */
    fs::remove(rec.path("signals.json"));
    s = signals_of(rec.folder);
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED);
    EXPECT_EQ(s.length, 0u);
}

/* A recording of a session no file decodes has neither the file nor the
 * line, and the reader names none; nor does one given a set no one file
 * holds (length 0) */
TEST(RecorderTest, ASessionNoFileDecodesNamesNone)
{
    register_profile();
    Root root;
    {
        Recording rec(root, "mock://x?profile=rec-test");
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        EXPECT_FALSE(fs::exists(fs::u8path(rec.folder) / "signals.json"));
        EXPECT_EQ(rec.file("events.log").find("signal file"),
                  std::string::npos);
        const ecuio_recording_signals_t s = signals_of(rec.folder);
        EXPECT_EQ(s.flags, 0u);
        EXPECT_STREQ(s.sha256, "");
    }
    FileRecording rec(root, "mock://y?profile=rec-test", nullptr, &kSet);
    ASSERT_EQ(rec.attached, ECUIO_OK);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    EXPECT_FALSE(fs::exists(rec.path("signals.json")));
    EXPECT_EQ(read_text(rec.path("events.log")).find("signal file"),
              std::string::npos);
}

/* The file is taken once, before attach, and only for the session it
 * decodes: one whose set the session doesn't use refuses the attach */
TEST(RecorderTest, AFileIsTakenOnlyForTheSessionItDecodes)
{
    register_profile();
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    {
        FileRecording rec(root, "mock://x?profile=rec-test", f.file);
        EXPECT_EQ(rec.attached, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("the signal file isn't what decodes the session "
                            "(its profile is rec-test)"),
                  std::string::npos)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_recorder_close(rec.recorder, nullptr), ECUIO_OK);
        rec.recorder = nullptr;
    }

    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* r = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(nullptr, f.file),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_signal_file(r, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(nullptr, f.set(), "x", 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(r, nullptr, "x", 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(r, f.set(), nullptr, 1),
              ECUIO_ERR_NULL_ARGUMENT);
    const std::string big(ECUIO_SIGNAL_FILE_MAX_BYTES + 1, ' ');
    EXPECT_EQ(ecuio_recorder_take_signals(r, f.set(), big.data(), big.size()),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_t* s = nullptr;
    const std::string uri = "mock://z?profile=" + f.profile;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(s, r), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(r, nullptr), ECUIO_OK);
}

/* A signals.json that can't be written fails the recording as any write
 * does */
TEST(RecorderTest, ASignalFileThatCantBeWrittenFailsTheRecording)
{
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* r = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
    ecuio_recorder_test_fail_writes(r);
    ASSERT_EQ(ecuio_recorder_signal_file(r, f.file), ECUIO_OK);
    ecuio_session_t* s = nullptr;
    const std::string uri = "mock://z?profile=" + f.profile;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(s, r), ECUIO_OK);
    ecuio_recorder_state_t st{};
    EXPECT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(r, &st);
            return st.state == ECUIO_RECORDING_FAILED;
        },
        2000));
    EXPECT_STREQ(st.reason, "the disk is full");
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(r, nullptr), ECUIO_ERR_RECORDING_FAILED);
}

/* A replay of a recording with the file it holds decodes the same values
 * in the same order, and its own recording names the same bytes */
TEST(RecorderTest, AReplayWithTheRecordingsFileDecodesTheSame)
{
    const std::string source =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    const size_t frames =
        lines_of(read_text(fs::u8path(source) / "frames.csv")).size() - 1;
    Root root;

    auto record = [&](const std::string& from, const LoadedFile& f) {
        FileRecording rec(
            root, "replay://" + from + "?speed=max&profile=" + f.profile,
            f.file);
        EXPECT_EQ(rec.attached, ECUIO_OK);
        std::atomic<size_t> seen{0};
        auto count = [](const ecuio_frame_t*, void* user) {
            static_cast<std::atomic<size_t>*>(user)->fetch_add(1);
        };
        EXPECT_EQ(ecuio_register_frame_callback(rec.session, count, &seen),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
        EXPECT_TRUE(eventually([&] { return seen.load() >= frames; }, 20000))
            << seen.load();
        ecuio_recorder_state_t st{};
        ecuio_recorder_get_state(rec.recorder, &st);
        EXPECT_EQ(st.records_dropped, 0u);
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        return rec.folder;
    };

    std::string first;
    {
        LoadedFile f(kExtras);
        ASSERT_NE(f.file, nullptr);
        first = record(source, f);
    }
    std::string second;
    {
        LoadedFile again(first + "/signals.json");
        ASSERT_NE(again.file, nullptr);
        second = record(first, again);
    }
    const std::vector<std::string> a =
        decoded_rows(fs::u8path(first) / "signals.csv");
    const std::vector<std::string> b =
        decoded_rows(fs::u8path(second) / "signals.csv");
    ASSERT_GT(a.size(), 100u);
    EXPECT_NE(read_text(fs::u8path(first) / "signals.csv").find(",Road Speed,"),
              std::string::npos);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "row " << i + 1;
    }
    const ecuio_recording_signals_t s1 = signals_of(first);
    const ecuio_recording_signals_t s2 = signals_of(second);
    EXPECT_EQ(s2.flags, s1.flags);
    EXPECT_STREQ(s2.sha256, s1.sha256);
}

namespace {

/* The example plugin read from the repository's plugins/, freed at the
 * end; variant 1 (my2019) registered as its profile */
struct ExamplePlugin {
    std::map<std::string, std::string> files;
    ecuio_plugin_t* plugin = nullptr;
    std::string profile;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<ExamplePlugin*>(user);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }

    ExamplePlugin()
    {
        const std::string root =
            std::string(ECUIO_PLUGINS_DIR) + "/example-sim/";
        for (const char* p : {"manifest.json", "data/base.json",
                              "data/my2019.json", "fixtures/values.json"}) {
            files[p] = read_bytes(fs::u8path(root + p));
        }
        const std::string& m = files["manifest.json"];
        EXPECT_EQ(ecuio_plugin_load(m.data(), m.size(), &read, this, &plugin),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
        EXPECT_EQ(ecuio_plugin_register(plugin, 1, name, sizeof name),
                  ECUIO_OK);
        profile = name;
    }
    ~ExamplePlugin() { EXPECT_EQ(ecuio_plugin_free(plugin), ECUIO_OK); }
    const ecuio_signal_file_t* file() const
    {
        return ecuio_plugin_variant_file(plugin, 1);
    }
};

} // namespace

/* A plugin variant's recording holds its merged file as signals.json and
 * names the plugin after it; a replay through that copy, loaded as any
 * signal file, decodes the same values (ADR-0052 section 4) */
TEST(RecorderTest, APluginVariantsRecordingHoldsItsMergedFile)
{
    ExamplePlugin p;
    ASSERT_NE(p.plugin, nullptr);
    ASSERT_EQ(p.profile, "plugin.example-sim.my2019");
    Root root;
    std::string first;
    {
        FileRecording rec(root, "mock://plugin?profile=" + p.profile, p.file());
        ASSERT_EQ(rec.attached, ECUIO_OK) << ecuio_last_error_detail();
        /* The TCM's fluid temperature and gear, as its replies read */
        for (int i = 0; i < 20; ++i) {
            dispatch(rec.session, 0x7E9,
                     std::string("\x62\xF4\x10\x1F", 4) +
                         static_cast<char>(0x40 + i));
            dispatch(rec.session, 0x7E9,
                     std::string("\x62\xF4\x11", 3) + static_cast<char>(i % 8));
        }
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        first = rec.folder;
    }
    const std::string json = read_bytes(fs::u8path(first) / "signals.json");
    ASSERT_FALSE(json.empty());
    const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
    EXPECT_NE(read_text(fs::u8path(first) / "events.log")
                  .find("\nprofile  plugin.example-sim.my2019"
                        "\nsignal file  signals.json," +
                        sha + "\nplugin  example-sim,1.0.0,my2019\n"),
              std::string::npos)
        << read_text(fs::u8path(first) / "events.log").substr(0, 600);

    LoadedFile copy(first + "/signals.json");
    ASSERT_NE(copy.file, nullptr);
    EXPECT_EQ(copy.profile, "user.example-the-simulated-car");
    std::string second;
    {
        FileRecording rec(
            root, "replay://" + first + "?speed=max&profile=" + copy.profile,
            copy.file);
        ASSERT_EQ(rec.attached, ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
        ASSERT_TRUE(eventually(
            [&] {
                ecuio_recorder_state_t st{};
                ecuio_recorder_get_state(rec.recorder, &st);
                return st.frame_rows >= 40;
            },
            10000));
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        second = rec.folder;
    }
    const std::vector<std::string> a =
        decoded_rows(fs::u8path(first) / "signals.csv");
    const std::vector<std::string> b =
        decoded_rows(fs::u8path(second) / "signals.csv");
    ASSERT_GE(a.size(), 40u);
    EXPECT_NE(read_text(fs::u8path(first) / "signals.csv")
                  .find(",Trans Fluid Temp,40.01,"),
              std::string::npos)
        << "the later file's scale, 0.01";
    EXPECT_EQ(a, b);
    EXPECT_EQ(read_bytes(fs::u8path(second) / "signals.json"), json);
    EXPECT_EQ(read_text(fs::u8path(second) / "events.log").find("\nplugin  "),
              std::string::npos)
        << "a file loaded alone names no plugin";
}

namespace {

/* A one-ECU signal file of signals, recorded on the mock and opened: the
 * reader's words when it can't open it, else its events.log */
std::string recorded_header(const std::string& name, const std::string& signals)
{
    const std::string text = R"({"havne_signals":1,"name":")" + name +
                             R"(","source":{"name":"t"},)"
                             R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" +
                             signals + "]}";
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(text.data(), text.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    if (f == nullptr) {
        return "not loaded";
    }
    const std::string profile = ecuio_signal_file_profile(f);
    EXPECT_EQ(ecuio_profile_register(profile.c_str(), ecuio_signal_file_set(f),
                                     nullptr),
              ECUIO_OK);
    std::string out;
    {
        Root root;
        FileRecording rec(root, "mock://x?profile=" + profile, f);
        EXPECT_EQ(rec.attached, ECUIO_OK);
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        ecuio_recording_t* r = nullptr;
        out = ecuio_recording_open(rec.folder.c_str(), nullptr, nullptr, &r) ==
                      ECUIO_OK
                  ? read_text(rec.path("events.log"))
                  : std::string("not opened: ") + ecuio_last_error_detail();
        ecuio_recording_close(r);
    }
    (void)ecuio_profile_unregister(profile.c_str());
    ecuio_signal_file_free(f);
    return out;
}

} // namespace

// A number and an enum of one name: the reader keeps the first signal
// line of a name, so the enum's labels, under the number's, are left out
TEST(RecorderTest, ANumberAndAnEnumOfOneNameOpen)
{
    const std::string events = recorded_header(
        "Recorder gear",
        R"({"key":"g1","name":"Gear","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"g2","name":"Gear","read":{"service":"22","number":"D101"},"field":{"byte":0},)"
        R"("kind":"enum","values":{"1":"First","2":"Second"}},)"
        R"({"key":"m","name":"Mode","read":{"service":"22","number":"D102"},"field":{"byte":0},)"
        R"("kind":"enum","values":{"1":"Eco"}})");
    EXPECT_NE(events.find("\nsignal  Gear,number,"), std::string::npos)
        << events;
    EXPECT_EQ(events.find("\nlabel  Gear,"), std::string::npos) << events;
    EXPECT_NE(events.find("\nlabel  Mode,1,Eco\n"), std::string::npos)
        << events;
}

// A line break in a name or unit is a space on the header and in the
// rows, which no line may break
TEST(RecorderTest, ALineBreakInANameOrUnitIsASpace)
{
    const std::string events = recorded_header(
        "Recorder breaks",
        R"({"key":"g","name":"Gear\nX","unit":"k\r\nPa","read":{"service":"22","number":"D100"},"field":{"byte":0}})");
    EXPECT_NE(events.find("\nsignal  Gear X,number,k  Pa,"), std::string::npos)
        << events;
}
