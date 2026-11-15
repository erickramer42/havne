// File/layer: tests/test_armed_recorder.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ArmedRecorderTest (test_core)
//
// The armed recorder (ADR-0063 section 7): nothing on disk before a
// firing, a window from before ahead of it to after past the last record
// condition turning OFF or stale, windows joined, a window around a gap
// starting before the gap, a full ring cutting the pre-roll and counting
// it, the held lines and the notes a window takes. Sessions are never
// started, and the recordings' clock is the test's
// (ecuio_recorder_test_set_clock), each frame's host time its session
// time, so a case's windows fall at exact times.

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/recording.h>
#include <ecuio/signal_sets.h>
#include "temp_name.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

/* The recordings' clock is the case's: at(t) puts it t microseconds past
 * a zero far from 0, as ecuio_clock_us reads */
constexpr uint64_t kZero = 1000000000000ULL;
constexpr uint64_t kMs = 1000;
constexpr uint64_t kS = 1000000;

struct TestClock {
    TestClock() { ecuio_recorder_test_set_clock(1, kZero); }
    ~TestClock() { ecuio_recorder_test_set_clock(0, 0); }
    TestClock(const TestClock&) = delete;
    TestClock& operator=(const TestClock&) = delete;
    void at(uint64_t t_us) { ecuio_recorder_test_set_clock(1, kZero + t_us); }
};

/* A scratch folder of the case's own, removed at the end */
struct Root {
    fs::path path;
    Root()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-armed-recorder");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~Root()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_u8string(); }
    /* The windows' folders, by name, which is by time */
    std::vector<fs::path> folders() const
    {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            out.push_back(e.path());
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

std::string read_bytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const fs::path& p)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(read_bytes(p));
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

/* events.log's lines of a kind: what follows the kind */
std::vector<std::string> kind_lines(const fs::path& folder, const char* kind)
{
    const std::string key = std::string("  ") + kind + "  ";
    std::vector<std::string> out;
    for (const std::string& l : lines_of(folder / "events.log")) {
        const size_t at = l.find(key);
        if (at != std::string::npos && at < 24) {
            out.push_back(l.substr(at + key.size()));
        }
    }
    return out;
}

/* events.log's header line of a key, "" with none */
std::string header_line(const fs::path& folder, const std::string& key)
{
    for (const std::string& l : lines_of(folder / "events.log")) {
        if (l.empty()) {
            break;
        }
        if (l.rfind(key + "  ", 0) == 0) {
            return l.substr(key.size() + 2);
        }
    }
    return "";
}

/* A cond line's fields: key, state, flags, then host_us */
struct Cond {
    std::string key, state, flags;
    uint64_t host_us = 0;
};

std::vector<Cond> cond_lines(const fs::path& folder)
{
    std::vector<Cond> out;
    for (const std::string& l : kind_lines(folder, "cond")) {
        std::vector<std::string> f;
        std::stringstream in(l);
        std::string field;
        while (std::getline(in, field, ',')) {
            f.push_back(field);
        }
        EXPECT_EQ(f.size(), 7u) << l;
        if (f.size() == 7) {
            out.push_back(Cond{f[0], f[1], f[2], std::stoull(f[5])});
        }
    }
    return out;
}

/* frames.csv's rows as (host_us, session time) */
std::vector<std::pair<uint64_t, uint64_t>> frame_rows(const fs::path& folder)
{
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (const std::string& l : lines_of(folder / "frames.csv")) {
        const size_t a = l.find(',');
        const size_t b = l.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos ||
            l.compare(0, a, "host_us") == 0) {
            continue;
        }
        out.emplace_back(std::stoull(l.substr(0, a)),
                         std::stoull(l.substr(a + 1, b - a - 1)));
    }
    return out;
}

/* PID 0C (a quarter turn a count) or 11 (a 255th of the travel) on 0x7E8
 * at a session time, dispatched as the worker would */
void pid_frame(ecuio_session_t* s, uint64_t t_us, uint8_t pid, unsigned raw,
               uint16_t bytes)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = t_us;
    f.data[0] = 0x41;
    f.data[1] = pid;
    if (bytes == 2) {
        f.data[2] = static_cast<uint8_t>(raw >> 8);
        f.data[3] = static_cast<uint8_t>(raw & 0xFF);
    } else {
        f.data[2] = static_cast<uint8_t>(raw);
    }
    f.data_length = static_cast<uint16_t>(2 + bytes);
    ecuio_session_dispatch_callbacks(s, &f);
}

std::string set_of(const std::string& conditions)
{
    return R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
}

/* A condition on engine speed above a level, then anything after */
std::string rpm_above(const std::string& key, unsigned level,
                      const std::string& rest)
{
    return R"({"key":")" + key + R"(","name":"N )" + key +
           R"(","when":{"signal":["obd2.rpm"],"unit":"RPM","above":)" +
           std::to_string(level) + "}" + rest + "}";
}

/* An armed recording of a never-started obd2 mock session, on the
 * test's clock */
struct Armed {
    TestClock clock;
    Root root;
    ecuio_session_t* s = nullptr;
    ecuio_recorder_t* rec = nullptr;

    explicit Armed(const std::string& json, uint32_t ring_bytes = 0)
    {
        EXPECT_EQ(ecuio_session_create("mock://armed?profile=obd2", &s),
                  ECUIO_OK);
        ecuio_condition_set_t* set = nullptr;
        EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_set_conditions(s, set, nullptr), ECUIO_OK);
        ecuio_condition_set_free(set);
        ecuio_record_options_t o{};
        const std::string keep = root.utf8();
        o.root_utf8 = keep.c_str();
        o.flags = ECUIO_RECORD_ARMED;
        o.ring_bytes = ring_bytes;
        EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
        EXPECT_EQ(ecuio_session_record(s, rec), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Armed()
    {
        ecuio_session_destroy(s);
        ecuio_recorder_close(rec, nullptr);
    }
    Armed(const Armed&) = delete;
    Armed& operator=(const Armed&) = delete;

    void rpm(uint64_t t_us, unsigned turns)
    {
        clock.at(t_us);
        pid_frame(s, t_us, 0x0C, turns * 4, 2);
    }
    void throttle(uint64_t t_us, unsigned percent)
    {
        clock.at(t_us);
        pid_frame(s, t_us, 0x11, percent * 255 / 100, 1);
    }
    void note(uint64_t t_us, const char* text)
    {
        clock.at(t_us);
        EXPECT_EQ(ecuio_recorder_note(rec, text), ECUIO_OK);
    }
    ecuio_recorder_state_t state() const
    {
        ecuio_recorder_state_t st{};
        EXPECT_EQ(ecuio_recorder_get_state(rec, &st), ECUIO_OK);
        return st;
    }
    std::string folder() const
    {
        char buf[1024];
        uint32_t len = 0;
        EXPECT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len),
                  ECUIO_OK);
        return std::string(buf, std::min<size_t>(len, sizeof buf - 1));
    }
    /* Two of the writer's wakes, so one whole loop ran after the last
     * frame went in */
    void settle()
    {
        const uint64_t w0 = ecuio_recorder_test_wakes(rec);
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ecuio_recorder_test_wakes(rec) < w0 + 2 &&
               std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_GE(ecuio_recorder_test_wakes(rec), w0 + 2);
    }
    /* The session gone, the writer's last pass seen (DRAINED): every
     * frame is written or let go (frames_passed), then the recorder
     * closed */
    ecuio_recorder_state_t finish(uint64_t frames)
    {
        ecuio_session_destroy(s);
        s = nullptr;
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        ecuio_recorder_state_t st = state();
        while ((st.flags & ECUIO_RECORDER_DRAINED) == 0 &&
               std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            st = state();
        }
        EXPECT_NE(st.flags & ECUIO_RECORDER_DRAINED, 0u);
        EXPECT_EQ(st.frame_rows + st.frames_passed + st.records_dropped, frames)
            << "drained, every frame is taken";
        EXPECT_EQ(ecuio_recorder_close(rec, "done"), ECUIO_OK);
        rec = nullptr;
        return st;
    }
};

} // namespace

/* Nothing reaches the disk before a firing, and closing a recorder that
 * never fired leaves no folder; every frame is let go and counted */
TEST(ArmedRecorderTest,
     NothingIsWrittenBeforeAFiringAndAnUnfiredCloseLeavesNone)
{
    Armed a(set_of(rpm_above("high", 2000, R"(,"record":{"before":1})")));
    for (uint64_t k = 0; k < 30; ++k) {
        a.rpm(k * 100 * kMs, 1000);
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    ecuio_recorder_state_t st = a.state();
    EXPECT_EQ(st.state, ECUIO_RECORDING_ARMED);
    EXPECT_EQ(st.windows, 0u);
    EXPECT_EQ(st.frame_rows, 0u);
    EXPECT_EQ(a.folder(), "");
    EXPECT_TRUE(a.root.folders().empty());
    st = a.finish(30);
    EXPECT_EQ(st.frames_passed, 30u);
    EXPECT_EQ(st.windows, 0u);
    EXPECT_TRUE(a.root.folders().empty()) << "an unfired close leaves nothing";
}

/* A window holds the frames from before ahead of the firing to after past
 * the condition's OFF, counts from its first frame, names its trigger,
 * holds a line for a condition already ON, and takes the notes of its own
 * time alone. Its folder is given only once its header is written. */
TEST(ArmedRecorderTest, AWindowRunsFromBeforeItsFiringToAfterItsEnd)
{
    Armed a(set_of(
        rpm_above("high", 2000, R"(,"record":{"before":1,"after":0.5})") + "," +
        rpm_above("turning", 500, "")));
    a.note(500 * kMs, "before the window");
    for (uint64_t k = 0; k <= 32; ++k) {
        a.rpm(k * 100 * kMs, k >= 30 ? 3000 : 1000);
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    ecuio_recorder_state_t st = a.state();
    EXPECT_EQ(st.state, ECUIO_RECORDING_ACTIVE);
    EXPECT_EQ(st.windows, 1u);
    const fs::path open = fs::u8path(a.folder());
    ASSERT_FALSE(open.empty());
    EXPECT_EQ(header_line(open, "trigger"), "high,1000 ms")
        << "the folder is given with its header written";
    a.note(3200 * kMs, "inside the window");
    ASSERT_NO_FATAL_FAILURE(a.settle());
    {
        /* A set holds for the recording's length, armed or not */
        const std::string json = set_of(rpm_above("other", 1, ""));
        ecuio_condition_set_t* set = nullptr;
        ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_set_conditions(a.s, set, nullptr),
                  ECUIO_ERR_IN_USE);
        ecuio_condition_set_free(set);
    }
    for (uint64_t k = 33; k <= 60; ++k) {
        a.rpm(k * 100 * kMs, k <= 34 ? 3000 : 1000);
    }
    st = a.finish(61);
    EXPECT_EQ(st.windows, 1u);
    EXPECT_EQ(st.preroll_short, 0u);
    EXPECT_EQ(st.records_dropped, 0u);
    /* ON at 3.0 s, OFF at 3.5 s: 2.0 s to 4.0 s */
    EXPECT_EQ(st.frame_rows, 21u);
    EXPECT_EQ(st.frame_rows + st.frames_passed, 61u);

    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const fs::path& w = folders[0];
    EXPECT_EQ(w, open);
    const auto rows = frame_rows(w);
    ASSERT_EQ(rows.size(), 21u);
    EXPECT_EQ(rows.front(), std::make_pair(uint64_t{0}, 2 * kS))
        << "a window counts from its first frame";
    EXPECT_EQ(rows.back(), std::make_pair(2 * kS, 4 * kS));
    EXPECT_EQ(lines_of(w / "signals.csv").size(), 22u);
    EXPECT_TRUE(fs::exists(w / "conditions.json"));
    EXPECT_FALSE(header_line(w, "conditions").empty());
    EXPECT_EQ(header_line(w, "link"), "healthy (state 0, 0 episodes)");

    const std::vector<Cond> conds = cond_lines(w);
    ASSERT_EQ(conds.size(), 3u);
    EXPECT_EQ(conds[0].key, "turning");
    EXPECT_EQ(conds[0].flags, "held");
    EXPECT_EQ(conds[0].host_us, 0u);
    EXPECT_EQ(conds[1].key, "high");
    EXPECT_EQ(conds[1].state, "on");
    EXPECT_EQ(conds[1].flags, "fired");
    EXPECT_EQ(conds[1].host_us, 1 * kS);
    EXPECT_EQ(conds[2].key, "high");
    EXPECT_EQ(conds[2].state, "off");
    EXPECT_EQ(conds[2].host_us, 1500 * kMs);

    const std::vector<std::string> notes = kind_lines(w, "note");
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0], "inside the window");
    const std::vector<std::string> ends = kind_lines(w, "end");
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_EQ(ends[0].rfind("the window ended: 21 frames, 21 values", 0), 0u)
        << ends[0];

    /* Every reader takes it as any recording */
    ecuio_recording_t* opened = nullptr;
    const std::string path = w.generic_u8string();
    ASSERT_EQ(ecuio_recording_open(path.c_str(), nullptr, nullptr, &opened),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(opened, &info), ECUIO_OK);
    EXPECT_EQ(info.condition_events, 3u);
    ecuio_recording_close(opened);
}

/* Two conditions' windows that overlap are one window; a firing past the
 * end, with its before short of it, opens another folder */
TEST(ArmedRecorderTest, OverlappingWindowsJoinAndALaterFiringOpensAnother)
{
    Armed a(set_of(
        rpm_above("rpm", 2000, R"(,"record":{"before":1,"after":0.5})") +
        R"(,{"key":"wot","name":"WOT","when":{"signal":["obd2.throttle"],)"
        R"("unit":"%","above":50},"record":{"before":1,"after":0.5}})"));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 120; ++k) {
        const uint64_t t = k * 100 * kMs;
        a.rpm(t,
              k >= 30 && k <= 31 ? 3000 : (k >= 80 && k <= 81 ? 3000 : 1000));
        a.throttle(t, k >= 36 && k <= 37 ? 80 : 10);
        frames += 2;
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 2u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 2u);
    /* rpm 3.0-3.2 s and wot 3.6-3.8 s: one window, 2.0 s to 4.3 s */
    const auto first = frame_rows(folders[0]);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.front().second, 2 * kS);
    EXPECT_EQ(first.back().second, 4300 * kMs);
    EXPECT_EQ(header_line(folders[0], "trigger"), "rpm,1000 ms");
    /* rpm 8.0-8.2 s: 7.0 s to 8.7 s */
    const auto second = frame_rows(folders[1]);
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second.front().second, 7 * kS);
    EXPECT_EQ(second.back().second, 8700 * kMs);
    EXPECT_EQ(st.frame_rows, first.size() + second.size());
}

/* A window past its end stays open while a firing could still reach back
 * into it: the writer wakes between the end and that firing */
TEST(ArmedRecorderTest, AWindowPastItsEndWaitsOutTheLongestBefore)
{
    Armed a(set_of(
        rpm_above("rpm", 2000, R"(,"record":{"before":1,"after":0.5})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 60; ++k) {
        const uint64_t t = k * 100 * kMs;
        a.rpm(t, (k >= 30 && k <= 31) || (k >= 45 && k <= 46) ? 3000 : 1000);
        ++frames;
        if (k == 40) {
            /* 0.3 s past the first end (3.7 s): the firing at 4.5 s
             * reaches back to 3.5 s */
            ASSERT_NO_FATAL_FAILURE(a.settle());
        }
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    /* rpm 3.0-3.2 s and 4.5-4.7 s: one window, 2.0 s to 5.2 s */
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().second, 2 * kS);
    EXPECT_EQ(rows.back().second, 5200 * kMs);
    EXPECT_EQ(rows.size(), 33u);
}

/* A window whose end is known and that is still open at the close takes
 * the carried events up to its end, none past it: the writer held, the
 * ring filled up to the end, the frames after that dropped and their
 * events carried */
TEST(ArmedRecorderTest, TheCloseWritesTheCarriedEventsUpToAWindowsEnd)
{
    Armed a(set_of(rpm_above("high", 2000,
                             R"(,"record":{"before":1,"after":0.5})") +
                   "," + rpm_above("turning", 500, "")),
            2048);
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 16; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 && k <= 11 ? 3000 : 1000);
        ++frames;
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    ecuio_recorder_test_hold(a.rec, 1);
    ASSERT_NO_FATAL_FAILURE(a.settle());
    /* The window ends at 1.7 s: frames there until the ring is full */
    for (int i = 0; i < 100; ++i) {
        a.rpm(1700 * kMs, 1000);
        ++frames;
    }
    /* Dropped: turning OFF at the end (its host time later), then
     * flapping past it */
    a.clock.at(1750 * kMs);
    pid_frame(a.s, 1700 * kMs, 0x0C, 400 * 4, 2);
    ++frames;
    for (uint64_t k = 0; k < 10; ++k) {
        a.rpm((2200 + k * 100) * kMs, k % 2 == 0 ? 600 : 400);
        ++frames;
    }
    ecuio_recorder_test_hold(a.rec, 0);
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_GE(st.records_dropped, 11u) << "the last frames found no room";
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    size_t offs = 0;
    for (const Cond& c : cond_lines(folders[0])) {
        offs += c.key == "turning" && c.state == "off" ? 1 : 0;
    }
    EXPECT_EQ(offs, 1u) << "the OFF at the end, none from past it";
    const std::vector<std::string> lines = lines_of(folders[0] / "events.log");
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("  +1.750  end  the window ended: "),
              std::string::npos)
        << "the end at the carried event's time: " << lines.back();
}

/* A note after a window's known end, with no record past it before the
 * session goes, is in no window: the window ends at its last record */
TEST(ArmedRecorderTest, ANoteAfterAClearedWindowsEndIsInNoWindow)
{
    Armed a(set_of(
        rpm_above("high", 2000, R"(,"record":{"before":1,"after":0.5})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 15; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 && k <= 11 ? 3000 : 1000);
        ++frames;
    }
    /* The window ends at 1.7 s; its last record is at 1.5 s */
    a.note(1900 * kMs, "after the end");
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    EXPECT_TRUE(kind_lines(folders[0], "note").empty());
    const std::vector<std::string> lines = lines_of(folders[0] / "events.log");
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("  end  the window ended: "), std::string::npos)
        << lines.back();
}

/* A firing at the newest frame's time, its before the longest, can reach
 * back to a window's end exactly: the window waits until the scan is past
 * that, not at it */
TEST(ArmedRecorderTest, AFiringReachingBackToAWindowsEndExactlyJoinsIt)
{
    Armed a(set_of(
        rpm_above("high", 2000, R"(,"record":{"before":1,"after":0.5})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 27; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 && k <= 11 ? 3000 : 1000);
        ++frames;
    }
    /* The end at 1.7 s, the scan at 2.7 s: the longest before past it */
    ASSERT_NO_FATAL_FAILURE(a.settle());
    a.rpm(2700 * kMs, 3000);
    a.rpm(2800 * kMs, 1000);
    frames += 2;
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().second, 0u);
    EXPECT_EQ(rows.back().second, 2800 * kMs);
}

/* Two conditions firing before the window opens: the one whose before
 * reaches further back moves the start */
TEST(ArmedRecorderTest, ALongerBeforeFiringBeforeTheOpenMovesTheStartBack)
{
    Armed a(set_of(
        rpm_above("rpm", 2000, R"(,"record":{"before":1,"after":0.5})") +
        R"(,{"key":"wot","name":"WOT","when":{"signal":["obd2.throttle"],)"
        R"("unit":"%","above":50},"record":{"before":2,"after":0.5}})"));
    ecuio_recorder_test_hold(a.rec, 1);
    ASSERT_NO_FATAL_FAILURE(a.settle());
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 50; ++k) {
        const uint64_t t = k * 100 * kMs;
        a.rpm(t, k >= 30 && k <= 33 ? 3000 : 1000);
        a.throttle(t, k >= 32 && k <= 33 ? 80 : 10);
        frames += 2;
    }
    ecuio_recorder_test_hold(a.rec, 0);
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    /* rpm at 3.0 s reaches 2.0 s, wot at 3.2 s reaches 1.2 s */
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().second, 1200 * kMs);
    EXPECT_EQ(header_line(folders[0], "trigger"), "rpm,1000 ms");
}

/* A recorder closed while its window is open and its session still runs
 * ends the window with the close's why */
TEST(ArmedRecorderTest, ACloseMidWindowEndsItWithTheClosesWhy)
{
    Armed a(set_of(rpm_above("high", 2000, R"(,"record":{"before":0.5})")));
    for (uint64_t k = 0; k <= 30; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 ? 3000 : 1000);
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    EXPECT_EQ(a.state().state, ECUIO_RECORDING_ACTIVE);
    a.clock.at(3100 * kMs);
    EXPECT_EQ(ecuio_recorder_close(a.rec, "stopped by the test"), ECUIO_OK);
    a.rec = nullptr;
    /* The session goes on with no recorder */
    a.rpm(3200 * kMs, 3000);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().second, 500 * kMs);
    EXPECT_EQ(rows.back().second, 3000 * kMs);
    const std::vector<std::string> lines = lines_of(folders[0] / "events.log");
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("  end  stopped by the test: 26 frames"),
              std::string::npos)
        << lines.back();
}

/* A window whose folder can't be made fails the recording, and counts
 * none of its frames as written */
TEST(ArmedRecorderTest, AWindowWhoseFolderCantBeMadeFailsAndWritesNoRow)
{
    Armed a(set_of(rpm_above("high", 2000, R"(,"record":{"before":0.5})")));
    std::error_code ec;
    fs::remove_all(a.root.path, ec);
    ASSERT_FALSE(ec);
    for (uint64_t k = 0; k <= 20; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 ? 3000 : 1000);
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    const ecuio_recorder_state_t st = a.state();
    EXPECT_EQ(st.state, ECUIO_RECORDING_FAILED);
    EXPECT_EQ(st.windows, 0u);
    EXPECT_EQ(st.frame_rows, 0u);
    EXPECT_EQ(st.value_rows, 0u);
}

/* Many windows in one second: each its own folder, the names in the order
 * the windows opened */
TEST(ArmedRecorderTest, WindowsInOneSecondAreNamedInTheOrderTheyOpened)
{
    Armed a(set_of(
        rpm_above("high", 2000, R"(,"record":{"before":0.01,"after":0.01})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k < 125; ++k) {
        a.rpm(k * 10 * kMs, k % 5 == 0 ? 3000 : 1000);
        ++frames;
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 25u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 25u);
    uint64_t last = 0;
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto rows = frame_rows(folders[i]);
        ASSERT_FALSE(rows.empty()) << folders[i].filename().string();
        /* Each window from 10 ms before its firing, every 50 ms */
        EXPECT_EQ(rows.front().second, i == 0 ? 0 : (i * 50 - 10) * kMs)
            << folders[i].filename().string();
        EXPECT_TRUE(i == 0 || rows.front().second > last);
        last = rows.front().second;
    }
}

/* A record condition whose input goes stale ends its window after its
 * after, as an OFF would; the signal answering again opens another */
TEST(ArmedRecorderTest, AStaleInputEndsAWindowAndItsReturnOpensAnother)
{
    Armed a(
        set_of(R"({"key":"high","name":"High","when":{"signal":["obd2.rpm"],)"
               R"("unit":"RPM","above":2000,"max_age":0.5},)"
               R"("record":{"before":0.5,"after":1}})"));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 80; ++k) {
        const uint64_t t = k * 100 * kMs;
        /* Engine speed from 1.0 s to 2.0 s and from 5.0 s on; the
         * throttle's frames settle the age bound between */
        if ((k >= 10 && k <= 20) || k >= 50) {
            a.rpm(t, 3000);
        } else {
            a.throttle(t, 10);
        }
        ++frames;
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 2u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 2u);
    /* Stale at 2.5 s: 0.5 s to 3.5 s */
    const auto first = frame_rows(folders[0]);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.front().second, 500 * kMs);
    EXPECT_EQ(first.back().second, 3500 * kMs);
    bool stale = false;
    for (const Cond& c : cond_lines(folders[0])) {
        stale = stale || (c.key == "high" && c.flags == "stale");
    }
    EXPECT_TRUE(stale) << "the window's end is its stale line's";
    /* Fresh again at 5.0 s: from 4.5 s to the session's end */
    const auto second = frame_rows(folders[1]);
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second.front().second, 4500 * kMs);
    EXPECT_EQ(second.back().second, 8 * kS);
    const std::vector<std::string> ends = kind_lines(folders[1], "end");
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_EQ(ends[0].rfind("done: ", 0), 0u)
        << "a window still ON ends at the close, in its words: " << ends[0];
}

/* A gap's events come only at its end, the ON and the stale together, and
 * the window still starts before ahead of the ON, before the gap */
TEST(ArmedRecorderTest, AWindowAroundAGapStartsBeforeTheGap)
{
    Armed a(set_of(rpm_above(
        "held", 2000, R"(,"for":0.3,"record":{"before":1,"after":0.5})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 12; ++k) {
        a.rpm(k * 100 * kMs, k >= 10 ? 3000 : 1000);
        ++frames;
    }
    ASSERT_NO_FATAL_FAILURE(a.settle());
    EXPECT_EQ(a.state().windows, 0u) << "nothing is known before the gap ends";
    /* Quiet until 10 s, then one frame settles the hold and the age bound */
    a.rpm(10 * kS, 1000);
    ++frames;
    for (uint64_t k = 101; k <= 120; ++k) {
        a.rpm(k * 100 * kMs, 1000);
        ++frames;
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.windows, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    /* ON at the hold's end, 1.3 s: from 0.3 s, ahead of the gap at 1.2 s */
    EXPECT_EQ(rows.front().second, 300 * kMs);
    EXPECT_EQ(rows.back().second, 10 * kS)
        << "the frame that settled the window's end is in it";
    const std::vector<Cond> conds = cond_lines(folders[0]);
    ASSERT_GE(conds.size(), 2u);
    EXPECT_EQ(conds[0].flags, "fired");
    EXPECT_EQ(conds[0].host_us, 1 * kS);
}

/* A ring too small for the pre-roll lets its oldest go before their time:
 * the window starts later than its before asks, nothing is dropped, and
 * the state counts the window as cut */
TEST(ArmedRecorderTest, AFullRingCutsThePreRollAndCountsIt)
{
    Armed a(set_of(rpm_above("high", 2000, R"(,"record":{"before":5})")), 2048);
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 62; ++k) {
        a.rpm(k * 100 * kMs, k >= 60 && k <= 61 ? 3000 : 1000);
        ++frames;
        ASSERT_NO_FATAL_FAILURE(a.settle());
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.records_dropped, 0u);
    EXPECT_EQ(st.windows, 1u);
    EXPECT_EQ(st.preroll_short, 1u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_GT(rows.front().second, 1 * kS) << "the pre-roll is cut";
    EXPECT_LT(rows.front().second, 6 * kS) << "and still holds some";
}

/* A ring that holds the whole pre-roll cuts none */
TEST(ArmedRecorderTest, ARingThatHoldsThePreRollCutsNone)
{
    Armed a(set_of(rpm_above("high", 2000, R"(,"record":{"before":5})")));
    uint64_t frames = 0;
    for (uint64_t k = 0; k <= 62; ++k) {
        a.rpm(k * 100 * kMs, k >= 60 && k <= 61 ? 3000 : 1000);
        ++frames;
    }
    const ecuio_recorder_state_t st = a.finish(frames);
    EXPECT_EQ(st.preroll_short, 0u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().second, 1 * kS);
}

/* Windows are measured in session time, as every condition rule is: with
 * the recording's clock standing still (a replay at max) the window is the
 * one real time gives */
TEST(ArmedRecorderTest, AWindowIsMeasuredInSessionTimeWhateverTheClock)
{
    Armed a(set_of(
        rpm_above("high", 2000, R"(,"record":{"before":1,"after":0.5})")));
    for (uint64_t k = 0; k <= 60; ++k) {
        pid_frame(a.s, k * 100 * kMs, 0x0C,
                  (k >= 30 && k <= 34 ? 3000 : 1000) * 4, 2);
    }
    const ecuio_recorder_state_t st = a.finish(61);
    EXPECT_EQ(st.windows, 1u);
    EXPECT_EQ(st.frame_rows, 21u);
    const std::vector<fs::path> folders = a.root.folders();
    ASSERT_EQ(folders.size(), 1u);
    const auto rows = frame_rows(folders[0]);
    ASSERT_EQ(rows.size(), 21u);
    EXPECT_EQ(rows.front(), std::make_pair(uint64_t{0}, 2 * kS));
    EXPECT_EQ(rows.back(), std::make_pair(uint64_t{0}, 4 * kS))
        << "the recording's clock stood still";
}
