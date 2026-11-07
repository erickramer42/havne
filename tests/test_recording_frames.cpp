// File/layer: tests/test_recording_frames.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecordingFramesTest (test_core)
//
// The reader opening a recording through its frames
// (ecuio_recording_open_frames). Each case writes into a folder of its own
// under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "temp_name.h"

namespace fs = std::filesystem;

namespace {

template <typename Predicate> bool eventually(Predicate pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/* A folder of the case's own, removed at the end */
struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name("ecuio-frames");
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

    std::string utf8() const { return path.generic_u8string(); }
    void write(const char* name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

const char* const kFramesHeader = "host_us,timestamp_us,id,flags,data\n";

/* A frames.csv row at t_us, its bytes as recorded */
std::string row(uint64_t t_us, uint32_t id, const std::vector<uint8_t>& data,
                uint32_t flags = 0)
{
    char head[96];
    std::snprintf(head, sizeof head, "%llu,%llu,0x%X,0x%X,",
                  static_cast<unsigned long long>(t_us),
                  static_cast<unsigned long long>(t_us), id, flags);
    std::string out = head;
    for (size_t i = 0; i < data.size(); ++i) {
        char b[4];
        std::snprintf(b, sizeof b, i == 0 ? "%02X" : " %02X", data[i]);
        out += b;
    }
    return out + "\n";
}

struct Opened {
    ecuio_recording_t* r = nullptr;
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    Opened(const std::string& folder, const ecuio_signal_set_t* set)
    {
        rc = set != nullptr
                 ? ecuio_recording_open_frames(folder.c_str(), set, nullptr,
                                               nullptr, &r)
                 : ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r);
    }
    ~Opened() { ecuio_recording_close(r); }
    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;

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
        return s;
    }
    /* A signal's index by name, or UINT32_MAX */
    uint32_t find(const char* name) const
    {
        for (uint32_t k = 0; k < info().signal_count; ++k) {
            if (std::strcmp(signal(k).name, name) == 0) {
                return k;
            }
        }
        return UINT32_MAX;
    }
    std::vector<ecuio_recording_column_t>
    columns(uint32_t k, uint64_t t0, uint64_t t1, uint32_t n,
            ecuio_status_t want = ECUIO_OK) const
    {
        std::vector<ecuio_recording_column_t> c(n);
        EXPECT_EQ(ecuio_recording_query(r, &k, 1, t0, t1, c.data(), n), want);
        return c;
    }
    uint64_t samples(uint32_t k, uint64_t t0, uint64_t t1, uint32_t n) const
    {
        uint64_t total = 0;
        for (const ecuio_recording_column_t& c : columns(k, t0, t1, n)) {
            total += c.samples;
        }
        return total;
    }
    std::string text_at(uint32_t k, uint64_t t, uint64_t* at) const
    {
        char buf[256];
        EXPECT_EQ(ecuio_recording_text_at(r, k, t, buf, sizeof buf, at),
                  ECUIO_OK);
        return buf;
    }
};

const ecuio_signal_set_t* profile(const char* name)
{
    const ecuio_signal_set_t* set = nullptr;
    EXPECT_EQ(ecuio_profile_lookup(name, &set), ECUIO_OK) << name;
    return set;
}

/* Records a session to its end (a replay's last frame) or for n frames,
 * and gives the recording's folder */
std::string record(const Folder& root, const std::string& uri, uint64_t n)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK) << uri;
    if (session == nullptr) {
        return "";
    }
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* rec = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024] = "";
    uint32_t len = 0;
    EXPECT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(session, rec), ECUIO_OK);
    std::atomic<uint64_t> frames{0};
    auto count = [](const ecuio_frame_t*, void* user) {
        static_cast<std::atomic<uint64_t>*>(user)->fetch_add(1);
    };
    EXPECT_EQ(ecuio_register_frame_callback(session, count, &frames), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_session_info_t si{};
    EXPECT_EQ(ecuio_session_get_info(session, &si), ECUIO_OK);
    const uint64_t want = n != 0 ? n : si.replay_frames;
    EXPECT_TRUE(eventually([&] { return frames.load() >= want; }, 30000));
    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    return folder;
}

/* The recording through its frames reads as through its signals.csv: the
 * same signals with samples, their summaries, every query's columns,
 * zoomed out and in, and the values in force */
void expect_same(const std::string& folder, const ecuio_signal_set_t* set)
{
    Opened rows(folder, nullptr);
    Opened frames(folder, set);
    ASSERT_EQ(rows.rc, ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(frames.rc, ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_recording_info_t ri = rows.info();
    const ecuio_recording_info_t fi = frames.info();
    EXPECT_EQ(fi.flags, ri.flags | ECUIO_RECORDING_INFO_FRAMES);
    EXPECT_EQ(fi.format, ri.format);
    EXPECT_EQ(fi.utc_start_us, ri.utc_start_us);
    EXPECT_EQ(fi.marker_count, ri.marker_count);
    std::vector<std::pair<uint32_t, uint32_t>> pairs; /* rows, frames */
    for (uint32_t k = 0; k < ri.signal_count; ++k) {
        const ecuio_recording_signal_t s = rows.signal(k);
        const uint32_t f = frames.find(s.name);
        if (s.samples == 0) {
            EXPECT_EQ(f, UINT32_MAX) << s.name << " never decoded";
            continue;
        }
        ASSERT_NE(f, UINT32_MAX) << s.name;
        pairs.emplace_back(k, f);
        const ecuio_recording_signal_t g = frames.signal(f);
        EXPECT_STREQ(g.unit, s.unit) << s.name;
        EXPECT_EQ(g.flags, s.flags) << s.name;
        EXPECT_EQ(g.decimal_places, s.decimal_places) << s.name;
        EXPECT_EQ(g.plot_rank, s.plot_rank) << s.name;
        EXPECT_EQ(g.group, s.group) << s.name;
        EXPECT_EQ(g.samples, s.samples) << s.name;
        EXPECT_EQ(g.lowest, s.lowest) << s.name;
        EXPECT_EQ(g.highest, s.highest) << s.name;
        EXPECT_EQ(g.first_us, s.first_us) << s.name;
        EXPECT_EQ(g.last_us, s.last_us) << s.name;
    }
    EXPECT_EQ(fi.signal_count, pairs.size()) << "only those that decoded";
    ASSERT_FALSE(pairs.empty());
    const uint64_t end = ri.duration_us + 1;
    struct Range {
        uint64_t t0, t1;
        uint32_t n;
    };
    std::vector<Range> ranges = {{0, end, 100}, {0, end, 400}};
    for (uint64_t s = 0; s * 1000000 < end; s += 3) {
        ranges.push_back({s * 1000000, s * 1000000 + 1000000, 50});
        ranges.push_back({s * 1000000 + 400000, s * 1000000 + 1700000, 13});
    }
    for (const auto& p : pairs) {
        const std::string name = rows.signal(p.first).name;
        for (const Range& g : ranges) {
            const auto a = rows.columns(p.first, g.t0, g.t1, g.n);
            const auto b = frames.columns(p.second, g.t0, g.t1, g.n);
            for (uint32_t c = 0; c < g.n; ++c) {
                ASSERT_EQ(b[c].samples, a[c].samples)
                    << name << " " << g.t0 << ".." << g.t1 << " col " << c;
                ASSERT_EQ(b[c].lowest, a[c].lowest) << name << " col " << c;
                ASSERT_EQ(b[c].highest, a[c].highest) << name << " col " << c;
            }
        }
        for (uint64_t t = 0; t < end; t += 700000) {
            uint64_t at_a = 0;
            uint64_t at_b = 0;
            const std::string va = rows.text_at(p.first, t, &at_a);
            const std::string vb = frames.text_at(p.second, t, &at_b);
            ASSERT_EQ(vb, va) << name << " at " << t;
            ASSERT_EQ(at_b, at_a) << name << " at " << t;
        }
    }
}

} // namespace

TEST(RecordingFramesTest, ARecordingsFramesDecodeToItsOwnValues)
{
    /* The fixture's car (J2534, whole messages) under obd2 */
    Folder root;
    const std::string uri = "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
                            "/2026-10-25-engine-start?speed=max&profile=obd2";
    const std::string folder = record(root, uri, 0);
    ASSERT_FALSE(folder.empty());
    expect_same(folder, profile("obd2"));
}

TEST(RecordingFramesTest, TheMocksFramesDecodeToItsOwnValuesBoostToo)
{
    Folder root;
    const std::string folder = record(root, "mock://demo", 50);
    ASSERT_FALSE(folder.empty());
    expect_same(folder, profile("mock"));
    Opened frames(folder, profile("mock"));
    ASSERT_EQ(frames.rc, ECUIO_OK);
    EXPECT_NE(frames.find("Boost"), UINT32_MAX) << "the computed def";
}

TEST(RecordingFramesTest, ARawRecordingHasValuesThroughAProfile)
{
    /* A listener's frames.csv, as they crossed the bus: a single frame, a
     * packed reply in a First and a Consecutive Frame, the tester's Flow
     * Control, and no signals.csv at all */
    Folder f;
    std::string csv = kFramesHeader;
    csv += row(100000, 0x7DF, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0});
    csv += row(110000, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8, 0, 0, 0});
    csv +=
        row(1200000, 0x7E8, {0x10, 0x0A, 0x41, 0x0C, 0x1B, 0x58, 0x0D, 0x32});
    csv += row(1201000, 0x7E0, {0x30, 0x00, 0x00, 0, 0, 0, 0, 0});
    csv += row(1210000, 0x7E8, {0x21, 0x05, 0x7B, 0x0F, 0x41, 0, 0, 0});
    f.write("frames.csv", csv);

    Opened none(f.utf8(), nullptr);
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED) << "no signals.csv";
    Opened r(f.utf8(), profile("obd2"));
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(r.info().flags,
              ECUIO_RECORDING_INFO_FRAMES | ECUIO_RECORDING_INFO_NO_SUMMARY);
    EXPECT_EQ(r.info().signal_count, 4u) << "RPM, speed, coolant, intake";
    const uint32_t rpm = r.find("RPM");
    ASSERT_NE(rpm, UINT32_MAX);
    const ecuio_recording_signal_t s = r.signal(rpm);
    EXPECT_EQ(s.samples, 2u);
    EXPECT_EQ(s.lowest, 1726.0);
    EXPECT_EQ(s.highest, 1750.0);
    EXPECT_EQ(s.first_us, 110000u);
    EXPECT_EQ(s.last_us, 1210000u) << "a message's time is its last frame's";
    const uint32_t speed = r.find("Vehicle Speed");
    ASSERT_NE(speed, UINT32_MAX);
    EXPECT_EQ(r.signal(speed).highest, 50.0);
    uint64_t at = 0;
    EXPECT_EQ(r.text_at(rpm, 1500000, &at), "1750");
    EXPECT_EQ(at, 1210000u);
    EXPECT_EQ(r.samples(rpm, 1000000, 2000000, 40), 1u) << "read again";
}

TEST(RecordingFramesTest, TwoSendersOnOneIdTearTheirMessagesNotSpliceThem)
{
    /* Two First Frames on 0x7E8 before either completes: had the second
     * taken the first's Consecutive Frame, a reply made of both would
     * decode (RPM 0x2222 / 4 at 1.03 s) */
    Folder f;
    std::string csv = kFramesHeader;
    csv +=
        row(1000000, 0x7E8, {0x10, 0x0A, 0x41, 0x0C, 0x11, 0x11, 0x0D, 0x01});
    csv +=
        row(1010000, 0x7E8, {0x10, 0x0A, 0x41, 0x0C, 0x22, 0x22, 0x0D, 0x02});
    csv += row(1020000, 0x7E8, {0x21, 0x05, 0x7B, 0x0F, 0x41, 0, 0, 0});
    csv += row(1030000, 0x7E8, {0x21, 0x05, 0x7C, 0x0F, 0x42, 0, 0, 0});
    csv += row(2000000, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8, 0, 0, 0});
    f.write("frames.csv", csv);
    Opened r(f.utf8(), profile("obd2"));
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.find("RPM");
    ASSERT_NE(rpm, UINT32_MAX);
    EXPECT_EQ(r.signal(rpm).samples, 1u) << "the single frame's alone";
    EXPECT_EQ(r.signal(rpm).first_us, 2000000u);
    EXPECT_EQ(r.find("Vehicle Speed"), UINT32_MAX);
    EXPECT_EQ(r.samples(rpm, 1000000, 2000000, 10), 0u) << "read again";
}

TEST(RecordingFramesTest, AMessageAcrossASecondDecodesWhenItsSecondIsReadAgain)
{
    /* Each second a reply's First Frame comes at .990 and its Consecutive
     * Frame at the next second's .005: reading a second again starts where
     * no message was under way */
    Folder f;
    std::string csv = kFramesHeader;
    for (uint64_t s = 0; s < 6; ++s) {
        const uint64_t t = s * 1000000;
        csv += row(t + 990000, 0x7E8,
                   {0x10, 0x0A, 0x41, 0x0C, 0x1A, static_cast<uint8_t>(s), 0x0D,
                    0x32});
        csv += row(t + 1005000, 0x7E8, {0x21, 0x05, 0x7B, 0x0F, 0x41, 0, 0, 0});
        csv += row(t + 1500000, 0x7E8, {0x03, 0x41, 0x0D, 0x10, 0, 0, 0, 0});
    }
    f.write("frames.csv", csv);
    Opened r(f.utf8(), profile("obd2"));
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.find("RPM");
    ASSERT_NE(rpm, UINT32_MAX);
    EXPECT_EQ(r.signal(rpm).samples, 6u);
    ecuio_recording_test_set_cache_limit(r.r, 0); /* every query reads */
    for (uint64_t s = 1; s <= 6; ++s) {
        const uint64_t t = s * 1000000;
        const auto c = r.columns(rpm, t, t + 1000000, 20);
        EXPECT_EQ(c[0].samples, 1u) << "second " << s;
        EXPECT_EQ(c[0].lowest, (0x1A00 + (s - 1)) / 4.0) << "second " << s;
    }
}

TEST(RecordingFramesTest, ASecondReadAgainDecodesFromItsMark)
{
    /* The decoder's state at a second's start: a computed value whose
     * other input came the second before, and a packed reply split by a
     * length a reply seconds earlier taught (PID 0C one byte long here) */
    const std::string file = R"({
  "havne_signals": 1,
  "name": "Frames test",
  "source": { "name": "Written for the test" },
  "ecus": [ { "id": "pcm", "rx": "7E8" } ],
  "signals": [
    { "key": "a", "name": "Input A", "ecu": "pcm",
      "read": { "service": "22", "number": "1111" },
      "field": { "byte": 0, "bits": 8 } },
    { "key": "b", "name": "Input B", "ecu": "pcm",
      "read": { "service": "22", "number": "2222" },
      "field": { "byte": 0, "bits": 8 } },
    { "key": "d", "name": "A less B", "kind": "computed", "expr": "a - b" }
  ]
})";
    ecuio_signal_file_t* sf = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &sf), ECUIO_OK)
        << ecuio_last_error_detail();
    Folder f;
    std::string csv = kFramesHeader;
    for (uint64_t s = 0; s < 5; ++s) {
        const uint64_t t = s * 1000000;
        csv += row(t + 100000, 0x7E8,
                   {0x62, 0x11, 0x11, static_cast<uint8_t>(50 + s)},
                   ECUIO_FRAME_FLAG_REASSEMBLED);
        csv += row(t + 600000, 0x7E8,
                   {0x62, 0x22, 0x22, static_cast<uint8_t>(10 + s)},
                   ECUIO_FRAME_FLAG_REASSEMBLED);
    }
    f.write("frames.csv", csv);
    {
        Opened r(f.utf8(), ecuio_signal_file_set(sf));
        ecuio_signal_file_free(sf); /* the reader keeps its own copy */
        ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
        const uint32_t d = r.find("A less B");
        ASSERT_NE(d, UINT32_MAX);
        EXPECT_EQ(r.signal(d).samples, 9u) << "every frame but the first";
        ecuio_recording_test_set_cache_limit(r.r, 0);
        for (uint64_t s = 1; s < 5; ++s) {
            const uint64_t t = s * 1000000;
            const auto c = r.columns(d, t, t + 1000000, 10);
            /* at .1: A this second less B the second before */
            EXPECT_EQ(c[1].samples, 1u) << "second " << s;
            EXPECT_EQ(c[1].lowest, (50.0 + s) - (10.0 + s - 1)) << s;
        }
    }

    Folder g;
    csv = kFramesHeader;
    csv += row(100000, 0x7E8, {0x41, 0x0C, 0x1A}, ECUIO_FRAME_FLAG_REASSEMBLED);
    csv += row(2100000, 0x7E8, {0x41, 0x0C, 0x1A, 0x0D, 0x32},
               ECUIO_FRAME_FLAG_REASSEMBLED);
    g.write("frames.csv", csv);
    Opened r(g.utf8(), profile("obd2"));
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t speed = r.find("Vehicle Speed");
    ASSERT_NE(speed, UINT32_MAX);
    EXPECT_EQ(r.find("RPM"), UINT32_MAX) << "one byte holds no RPM";
    ecuio_recording_test_set_cache_limit(r.r, 0);
    const auto c = r.columns(speed, 2000000, 3000000, 10);
    EXPECT_EQ(c[1].samples, 1u);
    EXPECT_EQ(c[1].lowest, 50.0);
}

TEST(RecordingFramesTest, AReassembledMessageIsTimedByItsFrames)
{
    /* A computed value's inputs from messages on the wire: B at 0.1 s is
     * past its age bound (3 s) when A comes at 5.0 s, then both come
     * within it */
    const std::string file = R"({
  "havne_signals": 1,
  "name": "Frames ages",
  "source": { "name": "Written for the test" },
  "ecus": [ { "id": "pcm", "rx": "7E8" } ],
  "signals": [
    { "key": "a", "name": "Input A", "ecu": "pcm",
      "read": { "service": "22", "number": "1111" },
      "field": { "byte": 0, "bits": 8 } },
    { "key": "b", "name": "Input B", "ecu": "pcm",
      "read": { "service": "22", "number": "2222" },
      "field": { "byte": 0, "bits": 8 } },
    { "key": "d", "name": "A less B", "kind": "computed", "expr": "a - b" }
  ]
})";
    ecuio_signal_file_t* sf = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &sf), ECUIO_OK)
        << ecuio_last_error_detail();
    Folder f;
    std::string csv = kFramesHeader;
    csv += row(100000, 0x7E8, {0x04, 0x62, 0x22, 0x22, 9, 0, 0, 0});
    csv += row(5000000, 0x7E8, {0x04, 0x62, 0x11, 0x11, 30, 0, 0, 0});
    csv += row(5500000, 0x7E8, {0x04, 0x62, 0x22, 0x22, 10, 0, 0, 0});
    csv += row(6000000, 0x7E8, {0x04, 0x62, 0x11, 0x11, 31, 0, 0, 0});
    f.write("frames.csv", csv);
    Opened r(f.utf8(), ecuio_signal_file_set(sf));
    ecuio_signal_file_free(sf);
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t d = r.find("A less B");
    ASSERT_NE(d, UINT32_MAX);
    const ecuio_recording_signal_t s = r.signal(d);
    EXPECT_EQ(s.samples, 2u) << "not at 5.0 s, B 4.9 s old";
    EXPECT_EQ(s.first_us, 5500000u);
    EXPECT_EQ(s.lowest, 20.0);
    EXPECT_EQ(s.highest, 21.0);
}

TEST(RecordingFramesTest, TheOpenRefusesWhatItMust)
{
    Folder f;
    f.write("frames.csv", std::string(kFramesHeader) +
                              row(1000, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8}));
    const ecuio_signal_set_t* obd2 = profile("obd2");
    ecuio_recording_t* r = nullptr;
    const std::string folder = f.utf8();
    auto open = [&](const char* dir, const ecuio_signal_set_t* set,
                    ecuio_recording_t** out) {
        if (out != nullptr) {
            *out = reinterpret_cast<ecuio_recording_t*>(&f);
        }
        const ecuio_status_t rc =
            ecuio_recording_open_frames(dir, set, nullptr, nullptr, out);
        if (out != nullptr) {
            EXPECT_EQ(*out, nullptr);
        }
        return rc;
    };
    EXPECT_EQ(open(nullptr, obd2, &r), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(open(folder.c_str(), nullptr, &r), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(open(folder.c_str(), obd2, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_signal_set_t bad = *obd2;
    bad.layout_version = 0;
    EXPECT_EQ(open(folder.c_str(), &bad, &r), ECUIO_ERR_INVALID_ARGUMENT);

    Folder empty;
    const std::string none = empty.utf8();
    EXPECT_EQ(open(none.c_str(), obd2, &r), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), "the folder has no frames.csv");

    const struct {
        std::string text;
        const char* why;
    } refused[] = {
        {"host_us,signal,value,unit\n", "frames.csv's header row isn't "
                                        "host_us,timestamp_us,id,flags,data"},
        {std::string(kFramesHeader) + "5,5,0x7E8,0x0,04 41\n" +
             "4,4,0x7E8,0x0,04 41\n",
         "frames.csv line 3: host_us is earlier than the row before it"},
        {std::string(kFramesHeader) + "5,5,7E8,0x0,04 41\n",
         "frames.csv line 2: id isn't 0x-prefixed hex"},
    };
    for (const auto& c : refused) {
        f.write("frames.csv", c.text);
        EXPECT_EQ(open(folder.c_str(), obd2, &r), ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), c.why);
        EXPECT_EQ(std::string(ecuio_last_error_detail()).find(folder),
                  std::string::npos);
    }

    /* A torn last row is dropped and counted; the header's signals are
     * the set's, not events.log's */
    f.write("events.log", "ecuio session recording, format 2 (ADR-0049)\n"
                          "signal  Gone,number,rpm,0,1\n\n");
    f.write("frames.csv", std::string(kFramesHeader) +
                              row(1000, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
                              "2000,2000,0x7E8,0x0,04 41 0C");
    Opened o(folder, obd2);
    ASSERT_EQ(o.rc, ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(o.info().rows_torn, 1u);
    EXPECT_EQ(o.info().format, ECUIO_RECORDING_FORMAT_CORE);
    EXPECT_EQ(o.info().signal_count, 1u);
    EXPECT_EQ(o.find("Gone"), UINT32_MAX);
    EXPECT_NE(o.find("RPM"), UINT32_MAX);

    /* A header line signals.csv's open refuses: its signals aren't read */
    f.write("events.log", "ecuio session recording, format 3 (ADR-0049)\n"
                          "label  Nothing,1,One\n\n");
    f.write("signals.csv", "host_us,signal,value,unit\n");
    EXPECT_EQ(Opened(folder, nullptr).rc, ECUIO_ERR_LOAD_FAILED)
        << "signals.csv's open refuses the label";
    Opened o3(folder, obd2);
    EXPECT_EQ(o3.rc, ECUIO_OK) << ecuio_last_error_detail();
}

TEST(RecordingFramesTest, AFileChangedAfterTheOpenIsSaid)
{
    Folder f;
    std::string csv = kFramesHeader;
    for (uint64_t s = 0; s < 3; ++s) {
        csv += row(s * 1000000 + 1000, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8});
    }
    f.write("frames.csv", csv);
    Opened r(f.utf8(), profile("obd2"));
    ASSERT_EQ(r.rc, ECUIO_OK) << ecuio_last_error_detail();
    const uint32_t rpm = r.find("RPM");
    f.write("frames.csv", std::string(kFramesHeader) + "garbage\n");
    (void)r.columns(rpm, 1000000, 2000000, 10, ECUIO_ERR_RECORDING_CHANGED);
}
