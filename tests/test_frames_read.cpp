// File/layer: tests/test_frames_read.cpp | test_core, Qt-free, C++17
// Testing: UNIT: FramesReadTest (test_core)
//
// A recording's frames read back as recorded (ecuio_recording_read_frames):
// a stretch by host_us, both edges included, against a plain scan of the
// file. Each case writes into a folder of its own under TEMP/TMPDIR,
// removed at the end.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "temp_name.h"

namespace fs = std::filesystem;

namespace {

/* A folder of the case's own, removed at the end */
struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-frames-read");
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
    void write(const std::string& text) const
    {
        std::ofstream f(path / "frames.csv",
                        std::ios::binary | std::ios::trunc);
        f << text;
    }
};

const char* const kHeader = "host_us,timestamp_us,id,flags,data\n";

/* A frames.csv row at host_us, its session time 1000 more */
std::string row(uint64_t host_us, uint32_t id = 0x7E8,
                const char* data = "41 0C 0B D7", uint32_t flags = 0)
{
    char head[96];
    std::snprintf(head, sizeof head, "%llu,%llu,0x%X,0x%X,",
                  static_cast<unsigned long long>(host_us),
                  static_cast<unsigned long long>(host_us + 1000), id, flags);
    return std::string(head) + data + "\n";
}

/* One row as the callback saw it */
struct Seen {
    uint64_t host_us = 0;
    uint64_t timestamp_us = 0;
    uint32_t id = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
    bool tail_zero = true;
    bool operator==(const Seen& o) const
    {
        return host_us == o.host_us && timestamp_us == o.timestamp_us &&
               id == o.id && flags == o.flags && data == o.data;
    }
};

struct Read {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_frames_read_t summary{};
    std::vector<Seen> rows;
    std::string detail;
    size_t stop_after = 0; /* 0: never stop */

    Read(const std::string& folder, uint64_t from_us, uint64_t to_us,
         size_t stop = 0)
        : stop_after(stop)
    {
        summary.rows = 99; /* the call zeroes it */
        rc = ecuio_recording_read_frames(folder.c_str(), from_us, to_us, take,
                                         this, &summary);
        const char* d = ecuio_last_error_detail();
        detail = d != nullptr ? d : "";
    }

    static int take(void* user, const ecuio_frame_t* f, uint64_t host_us)
    {
        auto* self = static_cast<Read*>(user);
        Seen s;
        s.host_us = host_us;
        s.timestamp_us = f->timestamp_us;
        s.id = f->arbitration_id;
        s.flags = f->flags;
        s.data.assign(f->data, f->data + f->data_length);
        for (size_t i = f->data_length; i < ECUIO_FRAME_DATA_MAX; ++i) {
            if (f->data[i] != 0) {
                s.tail_zero = false;
                break;
            }
        }
        self->rows.push_back(s);
        return self->stop_after != 0 && self->rows.size() >= self->stop_after;
    }
};

uint64_t parse_u64(const std::string& t)
{
    return std::strtoull(t.c_str(), nullptr, 10);
}

/* The file's rows by a plain scan from its start */
std::vector<Seen> plain_scan(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    std::string line;
    std::getline(in, line); /* the header */
    std::vector<Seen> out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            f.push_back(field);
        }
        if (f.size() < 4) {
            continue;
        }
        Seen s;
        s.host_us = parse_u64(f[0]);
        s.timestamp_us = parse_u64(f[1]);
        s.id = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 16));
        s.flags =
            static_cast<uint32_t>(std::strtoul(f[3].c_str(), nullptr, 16));
        if (f.size() > 4) {
            std::stringstream bytes(f[4]);
            std::string b;
            while (bytes >> b) {
                s.data.push_back(
                    static_cast<uint8_t>(std::strtoul(b.c_str(), nullptr, 16)));
            }
        }
        out.push_back(s);
    }
    return out;
}

std::vector<Seen> stretch_of(const std::vector<Seen>& all, uint64_t from_us,
                             uint64_t to_us)
{
    std::vector<Seen> out;
    for (const Seen& s : all) {
        if (s.host_us >= from_us && (to_us == 0 || s.host_us <= to_us)) {
            out.push_back(s);
        }
    }
    return out;
}

/* host_us 10, 20, ... for n rows from start_us, each a fixed width */
std::string even_rows(uint64_t start_us, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += row(start_us + 10 * i);
    }
    return out;
}

std::string fixture_frames()
{
    return std::string(ECUIO_REPLAY_FIXTURES) +
           "/2026-10-25-engine-start/frames.csv";
}

} // namespace

TEST(FramesReadTest, TheFixturesStretchesAreAPlainScansRows)
{
    const std::string file = fixture_frames();
    const std::vector<Seen> all = plain_scan(file);
    ASSERT_GT(all.size(), 5000U);
    ASSERT_GT(fs::file_size(file), 4U * 64 * 1024) << "the search runs on it";

    const Read whole(file, 0, 0);
    ASSERT_EQ(whole.rc, ECUIO_OK) << whole.detail;
    EXPECT_EQ(whole.rows, all);
    EXPECT_EQ(whole.summary.rows, all.size());
    EXPECT_EQ(whole.summary.torn_rows, 0U);

    /* Edges on rows, between rows, before the first and after the last */
    const uint64_t first = all.front().host_us;
    const uint64_t last = all.back().host_us;
    const struct {
        uint64_t from_us, to_us;
    } stretches[] = {
        {all[1000].host_us, all[4000].host_us},
        {all[1000].host_us + 1, all[4000].host_us - 1},
        {all[6000].host_us, 0},
        {1, all[10].host_us},
        {all[3500].host_us, all[3500].host_us},
        {first, last},
        {last, last},
        {last / 3, last / 3 * 2},
    };
    for (const auto& s : stretches) {
        const Read r(file, s.from_us, s.to_us);
        ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
        const std::vector<Seen> want = stretch_of(all, s.from_us, s.to_us);
        EXPECT_FALSE(want.empty());
        EXPECT_EQ(r.rows, want) << "from " << s.from_us << " to " << s.to_us;
        EXPECT_EQ(r.summary.rows, want.size());
    }
}

TEST(FramesReadTest, BothEdgesAreIncludedWithEveryDuplicate)
{
    Folder dir;
    dir.write(std::string(kHeader) + row(10) + row(20) + row(20) + row(30) +
              row(30) + row(40));
    const Read r(dir.utf8(), 20, 30);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    ASSERT_EQ(r.rows.size(), 4U);
    EXPECT_EQ(r.rows.front().host_us, 20U);
    EXPECT_EQ(r.rows.back().host_us, 30U);
}

/* Duplicates at each edge spanning more than the search's last stretch,
 * in a file long enough for the search to run */
TEST(FramesReadTest, DuplicatesAtBothEdgesAreKeptAcrossTheSearch)
{
    Folder dir;
    std::string text = kHeader;
    text += even_rows(0, 40000); /* to 399,990 */
    for (int i = 0; i < 5000; ++i) {
        text += row(500000);
    }
    text += even_rows(500010, 20000); /* to 699,000 */
    for (int i = 0; i < 5000; ++i) {
        text += row(800000);
    }
    text += even_rows(800010, 40000);
    dir.write(text);
    ASSERT_GT(text.size(), 50U * 64 * 1024);

    const Read r(dir.utf8(), 500000, 800000);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.rows.size(), 5000U + 20000U + 5000U);
    ASSERT_FALSE(r.rows.empty());
    EXPECT_EQ(r.rows.front().host_us, 500000U);
    EXPECT_EQ(r.rows.back().host_us, 800000U);

    /* From a time between rows, the next row's duplicates all come */
    const Read after(dir.utf8(), 399991, 500000);
    ASSERT_EQ(after.rc, ECUIO_OK) << after.detail;
    EXPECT_EQ(after.rows.size(), 5000U);
}

TEST(FramesReadTest, ABackwardRowAtTheStartIsRefusedInFrameRowsWords)
{
    Folder dir;
    const std::string before = std::string(kHeader) + even_rows(0, 30000);
    const std::string text = before + row(300000) + row(299999) + row(300010);
    dir.write(text);
    const Read r(dir.utf8(), 300000, 0);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    const size_t at = before.size() + row(300000).size();
    EXPECT_EQ(r.detail, "frames.csv, the row at byte " + std::to_string(at) +
                            ": host_us is earlier than the row before");
    EXPECT_EQ(r.summary.rows, 1U) << "the row before was handed on";
}

TEST(FramesReadTest, ABackwardRowAtTheEndIsRefused)
{
    Folder dir;
    const std::string before = std::string(kHeader) + even_rows(0, 30000);
    const std::string text =
        before + row(300000) + row(300100) + row(300099) + row(300200);
    dir.write(text);
    const Read r(dir.utf8(), 300000, 300100);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    const size_t at = before.size() + row(300000).size() + row(300100).size();
    EXPECT_EQ(r.detail, "frames.csv, the row at byte " + std::to_string(at) +
                            ": host_us is earlier than the row before");
    EXPECT_EQ(r.summary.rows, 2U);
}

TEST(FramesReadTest, AStretchWithNoRowsIsAnEmptyRead)
{
    Folder dir;
    dir.write(std::string(kHeader) + row(10) + row(20) + row(30));
    const Read between(dir.utf8(), 21, 29);
    EXPECT_EQ(between.rc, ECUIO_OK) << between.detail;
    EXPECT_TRUE(between.rows.empty());
    EXPECT_EQ(between.summary.rows, 0U);
    const Read past(dir.utf8(), 31, 0);
    EXPECT_EQ(past.rc, ECUIO_OK) << past.detail;
    EXPECT_TRUE(past.rows.empty());
    const Read before(dir.utf8(), 0, 9);
    EXPECT_EQ(before.rc, ECUIO_OK) << before.detail;
    EXPECT_TRUE(before.rows.empty());

    Folder header_only;
    header_only.write(kHeader);
    const Read none(header_only.utf8(), 0, 0);
    EXPECT_EQ(none.rc, ECUIO_OK) << none.detail;
    EXPECT_TRUE(none.rows.empty());
    const Read none_from(header_only.utf8(), 5, 0);
    EXPECT_EQ(none_from.rc, ECUIO_OK) << none_from.detail;
}

TEST(FramesReadTest, TheFileItselfIsTakenAsItsFolder)
{
    Folder dir;
    dir.write(std::string(kHeader) + row(10) + row(20));
    const Read r((dir.path / "frames.csv").generic_u8string(), 0, 0);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.rows.size(), 2U);
}

TEST(FramesReadTest, AFolderWithNoFramesSaysSo)
{
    Folder dir;
    const Read r(dir.utf8(), 0, 0);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.detail, "the folder has no frames.csv");
}

TEST(FramesReadTest, AFileThatIsntAFramesCsvIsRefusedWithoutItsPath)
{
    Folder dir;
    dir.write("time,value\n1,2\n");
    const Read r(dir.utf8(), 0, 0);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(r.detail.find("not a Havne frames.csv"), std::string::npos)
        << r.detail;
    EXPECT_EQ(r.detail.find(dir.utf8()), std::string::npos);

    Folder empty;
    empty.write("");
    const Read e(empty.utf8(), 0, 0);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the recording's frames.csv is empty");
}

TEST(FramesReadTest, ABadRowIsRefusedWithItsByteAndWhy)
{
    Folder dir;
    const std::string before = std::string(kHeader) + row(10) + row(20);
    dir.write(before + "30,1030,7E8,0x0,41 0C\n" + row(40));
    const Read r(dir.utf8(), 0, 0);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.detail, "frames.csv, the row at byte " +
                            std::to_string(before.size()) +
                            ": id isn't 0x-prefixed hex");
    EXPECT_EQ(r.summary.rows, 2U);
    EXPECT_EQ(r.detail.find(dir.utf8()), std::string::npos);
}

TEST(FramesReadTest, ARowLongerThanAFramesIsRefused)
{
    Folder dir;
    std::string data = "00";
    for (size_t i = 1; i < ECUIO_FRAME_DATA_MAX + 200; ++i) {
        data += " 00";
    }
    const std::string before = std::string(kHeader) + row(10);
    dir.write(before + row(20, 0x7E8, data.c_str()) + row(30));
    const Read r(dir.utf8(), 0, 0);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.detail, "frames.csv, the row at byte " +
                            std::to_string(before.size()) +
                            ": the row is longer than a frame's");
}

TEST(FramesReadTest, ATornLastRowIsLeftOutAndCounted)
{
    Folder dir;
    dir.write(std::string(kHeader) + row(10) + row(20) +
              "30,1030,0x7E8,0x0,41");
    const Read r(dir.utf8(), 0, 0);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.rows.size(), 2U);
    EXPECT_EQ(r.summary.torn_rows, 1U);

    const Read stretch(dir.utf8(), 10, 30);
    EXPECT_EQ(stretch.rc, ECUIO_OK);
    EXPECT_EQ(stretch.summary.torn_rows, 1U);

    /* A stretch that ends before the torn row never reaches it */
    const Read early(dir.utf8(), 0, 15);
    EXPECT_EQ(early.rc, ECUIO_OK);
    EXPECT_EQ(early.summary.torn_rows, 0U);
}

TEST(FramesReadTest, TheCallbackStopsTheRead)
{
    Folder dir;
    dir.write(std::string(kHeader) + even_rows(10, 10));
    const Read r(dir.utf8(), 20, 0, 3);
    EXPECT_EQ(r.rc, ECUIO_ERR_CANCELED);
    EXPECT_EQ(r.rows.size(), 3U);
    EXPECT_EQ(r.summary.rows, 3U);
}

TEST(FramesReadTest, EachRowIsTheFrameAsRecorded)
{
    Folder dir;
    std::string data;
    for (int i = 0; i < 20; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, i == 0 ? "%02X" : " %02X", i + 1);
        data += b;
    }
    /* A whole message's row, then a frame with no bytes */
    dir.write(std::string(kHeader) +
              row(10, 0x7E8, data.c_str(), ECUIO_FRAME_FLAG_REASSEMBLED) +
              "20,1020,0x18DAF110,0x2,\n" + row(30, 0x7E8, "41 0C 0B D7"));
    const Read r(dir.utf8(), 0, 0);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    ASSERT_EQ(r.rows.size(), 3U);
    EXPECT_EQ(r.rows[0].flags, uint32_t(ECUIO_FRAME_FLAG_REASSEMBLED));
    EXPECT_EQ(r.rows[0].data.size(), 20U);
    EXPECT_EQ(r.rows[0].data.back(), 20);
    EXPECT_EQ(r.rows[0].timestamp_us, 1010U);
    EXPECT_EQ(r.rows[1].id, 0x18DAF110U);
    EXPECT_EQ(r.rows[1].flags, 0x2U);
    EXPECT_TRUE(r.rows[1].data.empty());
    EXPECT_EQ(r.rows[2].data, (std::vector<uint8_t>{0x41, 0x0C, 0x0B, 0xD7}));
    for (const Seen& s : r.rows) {
        EXPECT_TRUE(s.tail_zero)
            << "a longer row's bytes stay past the short one";
    }
}

/* The search finds the stretch's start without reading the rows before it:
 * a backward pair near the file's start, which a read of those rows
 * refuses, is never reached */
TEST(FramesReadTest, AStretchOfALargeFileSkipsTheRowsBeforeIt)
{
    Folder dir;
    std::string text = kHeader;
    text += row(10) + row(5);
    text += even_rows(20, 200000); /* about 6 MB */
    dir.write(text);

    const Read whole(dir.utf8(), 0, 0);
    EXPECT_EQ(whole.rc, ECUIO_ERR_LOAD_FAILED) << "the pair is there";

    const Read late(dir.utf8(), 1500000, 1500100);
    ASSERT_EQ(late.rc, ECUIO_OK) << late.detail;
    EXPECT_EQ(late.rows.size(), 11U);

    const Read last(dir.utf8(), 20 + 10 * 199999, 0);
    ASSERT_EQ(last.rc, ECUIO_OK) << last.detail;
    EXPECT_EQ(last.rows.size(), 1U);
}

TEST(FramesReadTest, ArgumentsAreChecked)
{
    Folder dir;
    dir.write(std::string(kHeader) + row(10));
    ecuio_frames_read_t s{};
    s.rows = 7;
    s.torn_rows = 7;
    EXPECT_EQ(
        ecuio_recording_read_frames(nullptr, 0, 0, Read::take, nullptr, &s),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(s.rows, 0U);
    EXPECT_EQ(s.torn_rows, 0U);
    EXPECT_EQ(ecuio_recording_read_frames(dir.utf8().c_str(), 0, 0, nullptr,
                                          nullptr, &s),
              ECUIO_ERR_NULL_ARGUMENT);
    s.rows = 7;
    EXPECT_EQ(ecuio_recording_read_frames(dir.utf8().c_str(), 20, 10,
                                          Read::take, nullptr, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(s.rows, 0U);

    /* to_us 0 is the end whatever from_us is; a NULL summary is fine */
    std::vector<int> calls;
    const auto count = [](void* user, const ecuio_frame_t*, uint64_t) {
        static_cast<std::vector<int>*>(user)->push_back(1);
        return 0;
    };
    EXPECT_EQ(ecuio_recording_read_frames(dir.utf8().c_str(), 10, 0, count,
                                          &calls, nullptr),
              ECUIO_OK);
    EXPECT_EQ(calls.size(), 1U);
    EXPECT_EQ(ecuio_recording_read_frames(dir.utf8().c_str(), 10, 10, count,
                                          &calls, nullptr),
              ECUIO_OK);
    EXPECT_EQ(calls.size(), 2U);
}
