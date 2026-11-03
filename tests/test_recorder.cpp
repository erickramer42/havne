// File/layer: tests/test_recorder.cpp | test_core, Qt-free, C++17
// Testing: HARNESS: RecorderTest: the recorder (ADR-0049 sections 1 to
//          4) against sessions fed by hand through
//          ecuio_session_dispatch_callbacks (never started, so every
//          frame is the test's) and against the mock and the replay
//          fixture; the seams in internal.h for a held writer, a failed
//          write and the syncs
//
// Each case records into a folder of its own under TEMP/TMPDIR, removed
// at the end.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

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
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") /
            (std::string("ecuio-recorder-") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
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

    /* A cut folder name says how long it is */
    char small[8];
    ASSERT_EQ(ecuio_recorder_get_folder(rec, small, sizeof small, &len),
              ECUIO_OK);
    EXPECT_EQ(std::strlen(small), 7u);
    EXPECT_EQ(len, std::strlen(buf));
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
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
    EXPECT_EQ(ev[0], "ecuio session recording, format 2 (ADR-0049)");
    EXPECT_EQ(ev[1].rfind("started  ", 0), 0u);
    EXPECT_EQ(ev[2].rfind("utc  ", 0), 0u);
    EXPECT_EQ(ev[2].back(), 'Z');
    EXPECT_NE(events.find("\nuri  mock://rec?profile=rec-test\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nprofile  rec-test\n"), std::string::npos);
    EXPECT_NE(events.find("\nframe flags  complete\n"), std::string::npos);
    EXPECT_NE(events.find("\nsignal  Half,number,\xC2\xB0"
                          "C,1,2\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nsignal  Words,text,,0,0\n"), std::string::npos);
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
