// File/layer: tests/test_export.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ScrubTest, ExportTest, CsvExportTest, FolderExportTest,
//          ScrubFramesTest (test_core)
//
// scrub.h's classifier and check, and ecuio_export's pcapng: each export
// read back by a pcapng parser of the test's own, its frames' bytes and
// times, the re-segmented frames reassembled by core's ISO-TP to the
// stored message, what a scrubbed export leaves out, and both checks
// failing an export that would carry an identity value; the CSV export's
// values read back as text, byte for byte; the folder export's files
// read back byte for byte, its copy exported again; ecuio_scrub_frames
// over frames in memory by the same rules. Each export case writes into a
// folder of its own under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "folder_files.h"
#include "isotp.h"
#include "open_handles.h"
#include "partial_left.h"
#include "scrub.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;

namespace {

/* The simulated ECU's VIN, the fixtures' made-up form */
const char* const kVin = "ZZZFAKE11M0000001";

// ---- the classifier and the check ----

TEST(ScrubTest, IdentificationRepliesAndReplyIdsAreNamed)
{
    const uint8_t vin[] = {0x49, 0x02, 0x01, 'A'};
    const uint8_t serial[] = {0x62, 0xF1, 0x8C, '1'};
    const uint8_t block_end[] = {0x62, 0xF1, 0xFF};
    const uint8_t did[] = {0x62, 0xD8, 0x01, '1'};
    const uint8_t below[] = {0x62, 0xF1, 0x7F, '1'};
    const uint8_t refused[] = {0x7F, 0x09, 0x12};
    EXPECT_TRUE(ecuio_scrub::identification(vin, sizeof vin));
    EXPECT_TRUE(ecuio_scrub::identification(serial, sizeof serial));
    EXPECT_TRUE(ecuio_scrub::identification(block_end, sizeof block_end));
    EXPECT_FALSE(ecuio_scrub::identification(did, sizeof did));
    EXPECT_FALSE(ecuio_scrub::identification(below, sizeof below));
    EXPECT_FALSE(ecuio_scrub::identification(refused, sizeof refused));

    EXPECT_TRUE(ecuio_scrub::reply_id(0x7E8, false));
    EXPECT_TRUE(ecuio_scrub::reply_id(0x7EF, false));
    EXPECT_FALSE(ecuio_scrub::reply_id(0x7E7, false));
    EXPECT_FALSE(ecuio_scrub::reply_id(0x7F0, false));
    EXPECT_TRUE(ecuio_scrub::reply_id(0x18DAF110, true));
    EXPECT_FALSE(ecuio_scrub::reply_id(0x18DA10F1, true));
    EXPECT_FALSE(ecuio_scrub::reply_id(0x7E8, true));
}

TEST(ScrubTest, AValueIsFoundAsItIsAndAsHexInEachForm)
{
    ecuio_scrub::values v;
    const char cal[] = "\0FAKECAL-01\0\0\0\xFF  NEXTCAL7  \0AB";
    ASSERT_TRUE(
        v.add(cal, sizeof cal - 1, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION));
    EXPECT_EQ(v.forms(), 10u); /* two pieces, five forms each; "AB" too short */
    const uint32_t ident = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1;
    auto found = [&](const std::string& text) {
        return ecuio_scrub::check_bytes(
            v, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };
    EXPECT_EQ(found("..FAKECAL-01.."), ident);
    EXPECT_EQ(found("NEXTCAL7"), ident);
    EXPECT_EQ(found("x46414B4543414C2D3031x"), ident);
    EXPECT_EQ(found("46414b4543414c2d3031"), ident);
    EXPECT_EQ(found("46 41 4B 45 43 41 4C 2D 30 31"), ident);
    EXPECT_EQ(found("46 41 4b 45 43 41 4c 2d 30 31"), ident);
    EXPECT_EQ(found("FAKECAL-0"), 0u);
    EXPECT_EQ(found("AB AB AB"), 0u);
    EXPECT_EQ(found(""), 0u);

    ecuio_scrub::values few;
    ASSERT_TRUE(few.add("SHORT", 5, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES));
    EXPECT_EQ(few.forms(), 0u); /* under kValueMin */
}

TEST(ScrubTest, AValueOrAVinAcrossTwoPiecesIsFound)
{
    ecuio_scrub::values v;
    ASSERT_TRUE(
        v.add("HAVNE-SERIAL-42", 15, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES));
    ecuio_scrub::check c(v);
    const std::string a = std::string(70000, 'x') + "HAVNE-SER";
    const std::string b = "IAL-42" + std::string(10, 'x');
    c.feed(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    EXPECT_EQ(c.found(), 0u);
    c.feed(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    EXPECT_EQ(c.finish(), ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);

    ecuio_scrub::values none;
    ecuio_scrub::check vin(none);
    vin.feed(reinterpret_cast<const uint8_t*>("..ZZZFAKE1"), 10);
    vin.feed(reinterpret_cast<const uint8_t*>("1M0000001"), 9);
    EXPECT_EQ(vin.finish(), ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
}

TEST(ScrubTest, AVinShapedTokenIsSeventeenOfTheAlphabetAlone)
{
    ecuio_scrub::values none;
    auto vin = [&](const std::string& text) {
        return ecuio_scrub::check_bytes(
                   none, reinterpret_cast<const uint8_t*>(text.data()),
                   text.size()) == ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;
    };
    EXPECT_TRUE(vin(kVin));
    EXPECT_TRUE(vin(std::string("\x01") + kVin + "\x00"));
    EXPECT_TRUE(vin(std::string("VIN: ") + kVin + ", next"));
    EXPECT_FALSE(vin("ZZZFAKE11M000000"));      /* 16 */
    EXPECT_FALSE(vin("ZZZFAKEI1M0000001"));     /* an I */
    EXPECT_FALSE(vin("ZZZFAKEO1M0000001"));     /* an O */
    EXPECT_FALSE(vin("ZZZFAKEQ1M0000001"));     /* a Q */
    EXPECT_FALSE(vin("zzzfake11m0000001"));     /* lower case */
    EXPECT_FALSE(vin("0123456789ABCDEF0"));     /* hex */
    EXPECT_FALSE(vin("ZZZZZZZZZZZZZZZZZ"));     /* no digit */
    EXPECT_FALSE(vin("12345678901234567"));     /* no letter */
}

/* Inside a longer run a window is a VIN only when its ninth character is
 * its check digit, and it has the shape: a letter, a digit, not all hex */
TEST(ScrubTest, AVinInsideALongerRunIsFoundByItsCheckDigit)
{
    ecuio_scrub::values none;
    auto vin = [&](const std::string& text) {
        return ecuio_scrub::check_bytes(
                   none, reinterpret_cast<const uint8_t*>(text.data()),
                   text.size()) == ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;
    };
    EXPECT_TRUE(vin(std::string(kVin) + "2"));       /* the first window */
    EXPECT_TRUE(vin(std::string("A") + kVin));       /* the last */
    EXPECT_TRUE(vin(std::string("B") + kVin + "C")); /* between */
    EXPECT_TRUE(vin(std::string("zz") + kVin));      /* after lower case */
    EXPECT_TRUE(vin(std::string("x ") + kVin + "2 y"));
    /* ZZZFAKE12M0000001: its ninth isn't its check digit */
    EXPECT_FALSE(vin("ZZZFAKE12M00000012"));
    EXPECT_FALSE(vin("AZZZFAKE12M0000001"));
    EXPECT_FALSE(vin("ZZZFAKE12M00000012345"));
    /* windows whose ninth is their check digit, without the shape */
    EXPECT_FALSE(vin("1234567890123456789012345678901234567890"));
    EXPECT_FALSE(vin("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));
    /* the VIN alphabet broken inside the window */
    EXPECT_FALSE(vin("ZZZFAKE11M000000I1"));
    EXPECT_FALSE(vin("ZZZFAKE11m00000012"));
    EXPECT_FALSE(vin("ZZZFAKE11 M00000012")); /* two runs, neither 17 */

    /* the recording's own value names the class before the shape does */
    ecuio_scrub::values own;
    ASSERT_TRUE(own.add(kVin, 17, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION));
    const std::string echo = std::string("\x62\xD8\x31") + kVin;
    EXPECT_EQ(
        ecuio_scrub::check_bytes(
            own, reinterpret_cast<const uint8_t*>(echo.data()), echo.size()),
        ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_TRUE(vin(echo)); /* '1' and the VIN: a window */

    ecuio_scrub::check pieces(none); /* a window across two pieces */
    pieces.feed(reinterpret_cast<const uint8_t*>("AZZZFAKE1"), 9);
    EXPECT_EQ(pieces.found(), 0u);
    pieces.feed(reinterpret_cast<const uint8_t*>("1M0000001"), 9);
    EXPECT_EQ(pieces.found(), ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
}

TEST(ScrubTest, AFailedCheckIsWordedByItsClass)
{
    EXPECT_STREQ(
        ecuio_scrub::failed_words(ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1),
        "an identification reply's value");
    EXPECT_STREQ(
        ecuio_scrub::failed_words(ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1),
        "a text signal's value");
    EXPECT_STREQ(
        ecuio_scrub::failed_words(ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1),
        "a VIN-shaped token");
}

// ---- the export ----

struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name("ecuio-export");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path / "rec", ec);
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
    void write(const std::string& name, const std::string& text) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/* 2023-11-14T22:13:20.250Z */
constexpr uint64_t kUtc = 1700000000250000u;

std::string events_log()
{
    return "ecuio session recording, format 2 (ADR-0049)\n"
           "started  2023-11-14 22:13:20.250 (UTC)\n"
           "utc  2023-11-14T22:13:20.250Z\n"
           "profile  raw\n"
           "frame flags  complete\n"
           "\n";
}

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

std::vector<uint8_t> text_bytes(std::initializer_list<uint8_t> head,
                                const std::string& text)
{
    std::vector<uint8_t> v(head);
    v.insert(v.end(), text.begin(), text.end());
    return v;
}

const uint32_t kWhole = ECUIO_FRAME_FLAG_REASSEMBLED;

/* A recording folder of frames.csv rows, with events.log (header lines
 * in place of its profile line when given) */
void recording(const Folder& f, const std::string& rows,
               const std::string& signals = "", const std::string& header = "")
{
    std::string events = events_log();
    if (!header.empty()) {
        const size_t at = events.find("profile  raw\n");
        events.replace(at, std::strlen("profile  raw\n"), header);
    }
    f.write("rec/events.log", events);
    f.write("rec/frames.csv", "host_us,timestamp_us,id,flags,data\n" + rows);
    if (!signals.empty()) {
        f.write("rec/signals.csv", "host_us,signal,value,unit\n" + signals);
    }
}

// The pcapng reader: little-endian blocks, as the export writes them

struct Packet {
    uint64_t ts = 0;
    std::vector<uint8_t> data;
    std::string comment;
    uint32_t flags = 0;
    uint32_t can_id() const
    {
        return (uint32_t{data[0]} << 24) | (uint32_t{data[1]} << 16) |
               (uint32_t{data[2]} << 8) | data[3];
    }
    std::vector<uint8_t> payload() const
    {
        return std::vector<uint8_t>(data.begin() + 8,
                                    data.begin() + 8 + data[4]);
    }
};

struct Pcap {
    std::string why; /* empty when it read */
    std::string comment;
    std::string application;
    uint16_t linktype = 0;
    int tsresol = -1;
    int interfaces = 0;
    std::vector<Packet> packets;
};

uint32_t le32(const std::string& b, size_t at)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<uint8_t>(b[at + i]);
    }
    return v;
}

uint16_t le16(const std::string& b, size_t at)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(b[at]) |
                                 (static_cast<uint8_t>(b[at + 1]) << 8));
}

template <typename Each>
bool options(const std::string& b, size_t at, size_t end, Each each)
{
    while (at + 4 <= end) {
        const uint16_t code = le16(b, at);
        const uint16_t n = le16(b, at + 2);
        if (code == 0) {
            return true;
        }
        if (at + 4 + n > end) {
            return false;
        }
        each(code, b.substr(at + 4, n));
        at += 4 + ((n + 3u) & ~3u);
    }
    return at == end;
}

Pcap read_pcapng(const std::string& b)
{
    Pcap p;
    size_t at = 0;
    bool first = true;
    while (at < b.size()) {
        if (at + 12 > b.size()) {
            p.why = "a block is cut short";
            return p;
        }
        const uint32_t type = le32(b, at);
        const uint32_t len = le32(b, at + 4);
        if (len < 12 || len % 4 != 0 || at + len > b.size() ||
            le32(b, at + len - 4) != len) {
            p.why = "a block's length is wrong";
            return p;
        }
        const size_t end = at + len - 4;
        if (first != (type == 0x0A0D0D0Au)) {
            p.why = "the section header isn't first, or isn't alone";
            return p;
        }
        first = false;
        if (type == 0x0A0D0D0Au) {
            if (le32(b, at + 8) != 0x1A2B3C4Du || le16(b, at + 12) != 1 ||
                le16(b, at + 14) != 0) {
                p.why = "the section header's magic or version is wrong";
                return p;
            }
            if (!options(b, at + 24, end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 1) {
                                 p.comment = v;
                             } else if (c == 4) {
                                 p.application = v;
                             }
                         })) {
                p.why = "the section header's options are wrong";
                return p;
            }
        } else if (type == 1) {
            ++p.interfaces;
            p.linktype = le16(b, at + 8);
            if (!options(b, at + 16, end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 9 && v.size() == 1) {
                                 p.tsresol = static_cast<uint8_t>(v[0]);
                             }
                         })) {
                p.why = "an interface's options are wrong";
                return p;
            }
        } else if (type == 6) {
            Packet k;
            if (le32(b, at + 8) != 0) {
                p.why = "a packet names another interface";
                return p;
            }
            k.ts = (uint64_t{le32(b, at + 12)} << 32) | le32(b, at + 16);
            const uint32_t cap = le32(b, at + 20);
            if (le32(b, at + 24) != cap || at + 28 + cap > end) {
                p.why = "a packet's lengths are wrong";
                return p;
            }
            k.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at + 28),
                          b.begin() +
                              static_cast<std::ptrdiff_t>(at + 28 + cap));
            if (!options(b, at + 28 + ((cap + 3u) & ~3u), end,
                         [&](uint16_t c, const std::string& v) {
                             if (c == 1) {
                                 k.comment = v;
                             } else if (c == 2 && v.size() == 4) {
                                 k.flags = le32(v, 0);
                             }
                         })) {
                p.why = "a packet's options are wrong";
                return p;
            }
            p.packets.push_back(std::move(k));
        } else {
            p.why = "a block of another type";
            return p;
        }
        at += len;
    }
    return p;
}

struct Exported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    Pcap pcap;
    Exported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
             uint64_t to_us = 0, const std::string& name = "out.pcapng")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_PCAPNG;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
            pcap = read_pcapng(bytes);
        }
    }
};

/* Neither the file nor its partial copy */
void expect_no_file(const Folder& f, const std::string& name = "out.pcapng")
{
    EXPECT_FALSE(fs::exists(f.path / name));
    EXPECT_FALSE(partial_left(f.path, name));
}

TEST(ExportTest, FramesAsTheyArrivedAreWrittenAsSocketCanHoldsThem)
{
    Folder f;
    recording(
        f,
        row(0, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
            row(1000, 0x18DAF110, ECUIO_FRAME_FLAG_EXTENDED,
                {0x03, 0x41, 0x0D, 0x32}) +
            row(2000, 0x456,
                ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                    ECUIO_FRAME_FLAG_FD_ESI,
                {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 0x44,
                 0x55, 0x66}) +
            row(3000, 0x004, ECUIO_FRAME_FLAG_ERROR, {0, 0, 8, 0, 0, 0, 0, 0}) +
            row(4000, 0x123, ECUIO_FRAME_FLAG_REMOTE, {}) +
            row(5000, 0x7E0, ECUIO_FRAME_FLAG_TX_ECHO, {0x02, 0x01, 0x0C}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_EQ(e.pcap.why, "");
    EXPECT_EQ(e.report.frames, 6u);
    EXPECT_EQ(e.report.made_up, 0u);
    EXPECT_EQ(e.report.failed_class, 0u);
    EXPECT_EQ(e.pcap.interfaces, 1);
    EXPECT_EQ(e.pcap.linktype, 227);
    EXPECT_EQ(e.pcap.tsresol, 6);
    EXPECT_EQ(e.pcap.application,
              std::string("Havne ") + ecuio_version_string());
    EXPECT_EQ(e.pcap.comment,
              std::string("Havne ") + ecuio_version_string() +
                  ", a scrubbed export (ADR-0054): 6 frames, 0 of them made up "
                  "from messages an adapter delivered whole; left out: 0 "
                  "frames of identification replies, 0 of text signals' "
                  "messages, 0 segments of messages that couldn't be "
                  "reassembled");
    ASSERT_EQ(e.pcap.packets.size(), 6u);
    const std::vector<Packet>& k = e.pcap.packets;
    for (size_t i = 0; i < k.size(); ++i) {
        EXPECT_EQ(k[i].ts, kUtc + 1000 * i) << i;
        EXPECT_EQ(k[i].comment, "") << i;
    }
    EXPECT_EQ(k[0].data.size(), 16u);
    EXPECT_EQ(k[0].can_id(), 0x7E8u);
    EXPECT_EQ(k[0].data[5], 0u);
    EXPECT_EQ(k[0].payload(),
              (std::vector<uint8_t>{0x04, 0x41, 0x0C, 0x1A, 0xF8}));
    EXPECT_EQ(k[1].can_id(), 0x80000000u | 0x18DAF110u);
    EXPECT_EQ(k[2].data.size(), 72u);
    EXPECT_EQ(k[2].can_id(), 0x456u);
    EXPECT_EQ(k[2].data[4], 12u);
    EXPECT_EQ(k[2].data[5], 0x07); /* FDF | ESI | BRS */
    EXPECT_EQ(k[3].can_id(), 0x20000004u);
    EXPECT_EQ(k[3].payload(), (std::vector<uint8_t>{0, 0, 8, 0, 0, 0, 0, 0}));
    EXPECT_EQ(k[4].can_id(), 0x40000123u);
    EXPECT_EQ(k[4].data[4], 0u);
    EXPECT_EQ(k[5].flags, 2u); /* outbound */
    EXPECT_EQ(k[0].flags, 0u);

    /* The same recording, the same bytes */
    Exported again(f, 0, 0, 0, "again.pcapng");
    ASSERT_EQ(again.rc, ECUIO_OK) << again.detail;
    EXPECT_EQ(again.bytes, e.bytes);
}

TEST(ExportTest, AMessageDeliveredWholeIsReSegmentedAndMarkedMadeUp)
{
    Folder f;
    const std::vector<uint8_t> packed = {
        0x41, 0x0C, 0x0F, 0xA0, 0x0D, 0x32, 0x05, 0x5A, 0x04, 0x80,
        0x0F, 0x40, 0x11, 0x20, 0x2F, 0x80, 0x33, 0x65, 0x46, 0x30};
    recording(f, row(1000, 0x7E8, kWhole, {0x41, 0x0C, 0x1A, 0xF8}) +
                     row(2000, 0x7E8, kWhole, packed));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_EQ(e.pcap.why, "");
    /* 1 Single Frame; a First Frame, the Flow Control, 2 Consecutive */
    EXPECT_EQ(e.report.frames, 5u);
    EXPECT_EQ(e.report.made_up, 5u);
    ASSERT_EQ(e.pcap.packets.size(), 5u);
    const std::vector<Packet>& k = e.pcap.packets;
    EXPECT_EQ(k[0].payload(),
              (std::vector<uint8_t>{0x04, 0x41, 0x0C, 0x1A, 0xF8, 0, 0, 0}));
    EXPECT_EQ(k[0].ts, kUtc + 1000);
    EXPECT_EQ(k[2].can_id(), 0x7E0u);
    EXPECT_EQ(k[2].payload(),
              (std::vector<uint8_t>{0x30, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(k[2].comment,
              "made up by Havne: the Flow Control a tester sends for this "
              "message");
    for (size_t i : {0u, 1u, 3u, 4u}) {
        EXPECT_EQ(k[i].can_id(), 0x7E8u) << i;
        EXPECT_EQ(k[i].comment,
                  "made up by Havne: the adapter delivered this frame's "
                  "message whole")
            << i;
    }
    for (size_t i = 1; i < 5; ++i) {
        EXPECT_EQ(k[i].ts, kUtc + 2000) << i;
    }
    /* The made-up frames reassemble to the stored message */
    std::unique_ptr<isotp_rx> rx(new isotp_rx);
    std::vector<uint8_t> got;
    for (const Packet& p : k) {
        isotp_can c;
        c.id = p.can_id();
        c.len = p.data[4];
        std::memcpy(c.data, p.data.data() + 8, c.len);
        isotp_message m;
        if (c.id == 0x7E8 &&
            isotp_take(*rx, c, 0, &m) == isotp_result::complete) {
            got.assign(m.data, m.data + m.len);
        }
    }
    EXPECT_EQ(got, packed);
}

TEST(ExportTest, AnIdentificationReplyIsLeftOutEveryFrameOfIt)
{
    Folder f;
    /* Delivered whole (J2534, ELM327), and as it crossed the bus (a
     * listener): the VIN reply's First Frame and two Consecutive Frames
     * around the tester's Flow Control, then a value */
    recording(
        f,
        row(1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)) +
            row(2000, 0x7DF, 0, {0x02, 0x09, 0x02, 0, 0, 0, 0, 0}) +
            row(2100, 0x7E8, 0, {0x10, 0x14, 0x49, 0x02, 0x01, 'Z', 'Z', 'Z'}) +
            row(2200, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
            row(2300, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}) +
            row(2400, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}) +
            row(3000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_EQ(e.pcap.why, "");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 4u);
    EXPECT_EQ(e.report.frames,
              3u); /* the request, the Flow Control, the value */
    ASSERT_EQ(e.pcap.packets.size(), 3u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x7DFu);
    EXPECT_EQ(e.pcap.packets[1].can_id(), 0x7E0u);
    EXPECT_EQ(e.pcap.packets[2].ts, kUtc + 3000);
    EXPECT_EQ(e.bytes.find("ZZZFAKE"), std::string::npos);
    EXPECT_EQ(e.bytes.find("000001"), std::string::npos);
    EXPECT_NE(e.pcap.comment.find("left out: 4 frames of identification"),
              std::string::npos)
        << e.pcap.comment;

    /* A raw export keeps them, and says so */
    Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 0u);
    EXPECT_EQ(raw.report.frames, 10u); /* 4 made up, 6 as they arrived */
    EXPECT_EQ(raw.report.made_up, 4u);
    EXPECT_NE(raw.pcap.comment.find("a raw export, not scrubbed"),
              std::string::npos);
    EXPECT_NE(raw.bytes.find("FAKE11M"), std::string::npos);
}

TEST(ExportTest, ASegmentNoMessageCanBeAttributedToIsLeftOutOnAReplyId)
{
    Folder f;
    recording(
        f,
        /* its First Frame came before the recording started */
        row(1000, 0x7E8, 0, {0x21, 'S', 'E', 'C', 'R', 'E', 'T', '1'}) +
            /* a First Frame never finished */
            row(2000, 0x7E9, 0, {0x10, 0x0A, 0x62, 0xD8, 0x01, 'A', 'B', 'C'}) +
            /* on a request ID: as it is */
            row(3000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}) +
            /* delivered whole but cut short */
            row(4000, 0x7E8, kWhole | ECUIO_FRAME_FLAG_TRUNCATED,
                {0x62, 0xD8}) +
            row(5000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 3u);
    EXPECT_EQ(e.report.frames, 2u);
    ASSERT_EQ(e.pcap.packets.size(), 2u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x7E0u);
    EXPECT_EQ(e.pcap.packets[1].ts, kUtc + 5000);
    EXPECT_EQ(e.bytes.find("SECRET"), std::string::npos);
}

/* The check, over each message before segmenting: a manufacturer's DID
 * that echoes the VIN in a segmented reply, which classification lets
 * through as a number signal's frames */
TEST(ExportTest, ADidEchoingTheVinInASegmentedReplyFailsTheExport)
{
    const std::string vin_reply =
        row(1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin));
    const std::vector<uint8_t> echo = text_bytes({0x62, 0xD8, 0x01}, kVin);
    {
        SCOPED_TRACE("delivered whole");
        Folder f;
        recording(f, vin_reply + row(2000, 0x7E8, kWhole, echo));
        Exported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail,
                  "the check found an identification reply's value in a "
                  "message the export would write: nothing was written");
        expect_no_file(f);
    }
    {
        SCOPED_TRACE("as it crossed the bus");
        Folder f;
        recording(
            f,
            vin_reply +
                row(2000, 0x7E8, 0,
                    {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}) +
                row(2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
                row(2200, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}) +
                row(2300, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}));
        Exported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        expect_no_file(f);
    }
    {
        SCOPED_TRACE("with no identification reply: the VIN's shape");
        Folder f;
        recording(f, row(2000, 0x7E8, kWhole, echo));
        Exported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail,
                  "the check found a VIN-shaped token in a message the export "
                  "would write: nothing was written");
        expect_no_file(f);
        EXPECT_EQ(e.detail.find(kVin), std::string::npos);

        Exported raw(f, ECUIO_EXPORT_RAW);
        EXPECT_EQ(raw.rc, ECUIO_OK) << raw.detail;
        EXPECT_EQ(raw.report.failed_class, 0u);
    }
}

TEST(ExportTest, ATextSignalsValueInAFrameFailsTheExport)
{
    Folder f;
    recording(f,
              row(1000, 0x7E8, kWhole,
                  text_bytes({0x62, 0xD8, 0x02}, "ECM-HavneSim")),
              "1000,ECU name,ECM-HavneSim,\n1000,RPM,1726,rpm\n");
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
    EXPECT_EQ(e.detail,
              "the check found a text signal's value in a message the export "
              "would write: nothing was written");
    expect_no_file(f);
}

/* A text or hex def's message is left out by classification, every
 * segment of it, counted: here uds-generic's DIDs outside the
 * identification block, from the recording's own did= list */
TEST(ExportTest, ATextSignalsMessagesAreLeftOutByTheirDef)
{
    const std::string header =
        "uri  elm327://COM3?profile=uds-generic&did=D802:text,D803:u16,D804\n"
        "profile  uds-generic\n";
    const std::string name = "ECM-HavneSim";
    Folder f;
    recording(
        f,
        row(1000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x02}, name)) +
            /* as it crossed the bus: a First Frame, the tester's Flow
             * Control, a Consecutive Frame */
            row(2000, 0x7E8, 0, {0x10, 0x0F, 0x62, 0xD8, 0x02, 'E', 'C', 'M'}) +
            row(2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
            row(2200, 0x7E8, 0, {0x21, '-', 'H', 'a', 'v', 'n', 'e', 'S'}) +
            row(2300, 0x7E8, 0, {0x22, 'i', 'm', 0, 0, 0, 0, 0}) +
            row(3000, 0x7E8, kWhole, {0x62, 0xD8, 0x03, 0x12, 0x34}) +
            row(4000, 0x7E8, kWhole, {0x62, 0xD8, 0x04, 0x01, 0x02}),
        "1000,DID D802,ECM-HavneSim,\n3000,DID D803,4660,\n", header);
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    /* D802 whole, D802's three segments, D804 (hex) */
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 5u);
    EXPECT_EQ(e.report.frames, 2u); /* the Flow Control, D803's frame */
    ASSERT_EQ(e.pcap.packets.size(), 2u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x7E0u);
    EXPECT_EQ(e.pcap.packets[1].payload(),
              (std::vector<uint8_t>{0x05, 0x62, 0xD8, 0x03, 0x12, 0x34, 0, 0}));
    EXPECT_EQ(e.bytes.find("HavneS"), std::string::npos);
    EXPECT_NE(e.pcap.comment.find("5 of text signals' messages"),
              std::string::npos)
        << e.pcap.comment;

    /* The same value in a number DID's frames still fails the export */
    Folder g;
    recording(
        g,
        row(1000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x02}, name)) +
            row(3000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x03}, name)),
        "1000,DID D802,ECM-HavneSim,\n", header);
    Exported planted(g);
    EXPECT_EQ(planted.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(planted.report.failed_class,
              ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
    expect_no_file(g);
}

/* The check over the written bytes: a frame no message holds (a stray
 * Consecutive Frame on a request ID, written as it is) carrying an
 * identification reply's value */
TEST(ExportTest, AValueInTheWrittenBytesFailsTheExport)
{
    Folder f;
    recording(
        f, row(1000, 0x7E8, kWhole, text_bytes({0x62, 0xF1, 0x8C}, "SN0042X")) +
               row(2000, 0x7E0, 0, {0x21, 'S', 'N', '0', '0', '4', '2', 'X'}));
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(e.detail,
              "the check found an identification reply's value in the "
              "export's file: nothing was written");
    expect_no_file(f);
}

TEST(ExportTest, ARangeTakesItsFramesAlone)
{
    Folder f;
    std::string rows;
    for (uint64_t t = 0; t < 10; ++t) {
        rows += row(t * 1000000, 0x100, 0, {static_cast<uint8_t>(t)});
    }
    recording(f, rows);
    Exported e(f, 0, 3000000, 6000000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    ASSERT_EQ(e.pcap.packets.size(), 4u);
    EXPECT_EQ(e.report.frames, 4u);
    EXPECT_EQ(e.pcap.packets[0].payload(), (std::vector<uint8_t>{3}));
    EXPECT_EQ(e.pcap.packets[3].payload(), (std::vector<uint8_t>{6}));
    Exported tail(f, 0, 8000000, 0, "tail.pcapng");
    ASSERT_EQ(tail.rc, ECUIO_OK) << tail.detail;
    EXPECT_EQ(tail.pcap.packets.size(), 2u);
}

/* The check reads the messages a range writes: a DID echoing the VIN
 * before the range, whole or as it crossed the bus, writes nothing and
 * fails nothing */
TEST(ExportTest, AVinEchoOutsideTheRangeIsNotChecked)
{
    Folder f;
    std::string rows =
        row(1000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x01}, kVin)) +
        row(2000, 0x7E8, 0, {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}) +
        row(2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
        row(2200, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}) +
        row(2300, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'});
    for (uint64_t t = 3; t < 7; ++t) {
        rows += row(t * 1000000, 0x100, 0, {static_cast<uint8_t>(t)});
    }
    recording(f, rows);
    Exported e(f, 0, 3000000, 6000000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.failed_class, 0u);
    EXPECT_EQ(e.report.frames, 4u);
    Exported all(f, 0, 0, 0, "all.pcapng");
    EXPECT_EQ(all.rc, ECUIO_ERR_LOAD_FAILED) << "the echo, in range, fails";
    EXPECT_EQ(all.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
}

TEST(ExportTest, AFileThatExistsOrARowThatDoesntReadWritesNothing)
{
    Folder f;
    recording(f, row(0, 0x100, 0, {1}));
    f.write("out.pcapng", "mine");
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the file exists already");
    EXPECT_EQ(read_file(f.path / "out.pcapng"), "mine");
    EXPECT_FALSE(partial_left(f.path, "out.pcapng"));

    recording(f, row(0, 0x100, 0, {1}) + "5,5,0x100,0x0,1\n");
    Exported bad(f, 0, 0, 0, "bad.pcapng");
    EXPECT_EQ(bad.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(
        bad.detail,
        "frames.csv line 3: data isn't hex byte pairs separated by spaces");
    expect_no_file(f, "bad.pcapng");

    Folder empty;
    std::error_code ec;
    fs::remove(empty.path / "rec" / "frames.csv", ec);
    Exported none(empty);
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(
        none.detail.rfind("the recording's frames.csv can't be opened: ", 0),
        0u)
        << none.detail;
    EXPECT_EQ(none.detail.find(empty.utf8("")), std::string::npos);
}

TEST(ExportTest, ArgumentsAreCheckedBeforeAnythingIsRead)
{
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_PCAPNG;
    o.folder_utf8 = "rec";
    o.out_path_utf8 = "out.pcapng";
    ecuio_export_report_t r{};
    r.frames = 7;
    EXPECT_EQ(ecuio_export(nullptr, &r), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.frames, 0u);
    ecuio_export_options_t a = o;
    a.folder_utf8 = nullptr;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    a = o;
    a.out_path_utf8 = nullptr;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    a = o;
    a.flags = 0x2;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.reserved[0] = 1;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.from_us = 5;
    a.to_us = 4;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.format = 0;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.format = 4;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.format = ECUIO_EXPORT_FOLDER; /* built: the folder isn't there */
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_LOAD_FAILED);
    a.format = ECUIO_EXPORT_CSV;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_LOAD_FAILED);

    /* The CSV's shape (ADR-0056 section 5): the wide CSV's fields alone */
    a = o;
    a.layout = 1; /* a pcapng has no layout */
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.format = ECUIO_EXPORT_CSV;
    a.layout = 2;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.layout = ECUIO_CSV_LONG;
    a.row_ms = 100; /* the long form has no rows */
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.row_ms = 0;
    a.units = ECUIO_UNITS_IMPERIAL;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.format = ECUIO_EXPORT_CSV;
    a.units = 2;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.units = 0;
    a.column_count = 1; /* a count with no names */
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    const char* none[] = {nullptr};
    a.columns_utf8 = none;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    const char* empty[] = {""};
    a.columns_utf8 = empty;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    const char* rpm[] = {"RPM"};
    a.columns_utf8 = rpm;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_LOAD_FAILED)
        << "a named column reads the recording";

    /* The estimate: the wide CSV's alone, each interval answered */
    ecuio_export_estimate_t est[2];
    const uint32_t rows_ms[2] = {0, 100};
    a = o;
    a.format = ECUIO_EXPORT_CSV;
    EXPECT_EQ(ecuio_export_estimate(nullptr, rows_ms, 2, est),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_export_estimate(&a, nullptr, 2, est),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_export_estimate(&a, rows_ms, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_export_estimate(&a, rows_ms, 0, est),
              ECUIO_ERR_INVALID_ARGUMENT);
    a.layout = ECUIO_CSV_LONG;
    EXPECT_EQ(ecuio_export_estimate(&a, rows_ms, 2, est),
              ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    EXPECT_EQ(ecuio_export_estimate(&a, rows_ms, 2, est),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a pcapng has no estimate";
    a.format = ECUIO_EXPORT_CSV;
    a.out_path_utf8 = nullptr; /* nothing is written */
    EXPECT_EQ(ecuio_export_estimate(&a, rows_ms, 2, est),
              ECUIO_ERR_LOAD_FAILED);
}

// ---- the CSV export: signals.csv's values as samples ----

/* The long form (ECUIO_CSV_LONG), a row a value */
struct CsvExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    CsvExported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
                uint64_t to_us = 0, const std::string& name = "out.csv")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.layout = ECUIO_CSV_LONG;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
        }
    }
};

/* The wide form (ECUIO_CSV_WIDE, the default): the columns named, or
 * none for the header's focus line or every number signal */
struct WideExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::string bytes;
    WideExported(const Folder& f, std::vector<const char*> columns = {},
                 uint32_t row_ms = 0, uint32_t units = ECUIO_UNITS_METRIC,
                 uint32_t flags = 0, uint64_t from_us = 0, uint64_t to_us = 0,
                 const std::string& name = "wide.csv")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        o.row_ms = row_ms;
        o.units = units;
        o.column_count = static_cast<uint32_t>(columns.size());
        o.columns_utf8 = columns.empty() ? nullptr : columns.data();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        if (fs::exists(f.path / name)) {
            bytes = read_file(f.path / name);
        }
    }
};

/* A wide file's lines, the CRLFs checked and dropped */
std::vector<std::string> wide_lines(const std::string& bytes)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < bytes.size()) {
        const size_t nl = bytes.find("\r\n", at);
        EXPECT_NE(nl, std::string::npos)
            << "a line without CRLF: " << bytes.substr(at);
        if (nl == std::string::npos) {
            out.push_back(bytes.substr(at));
            break;
        }
        out.push_back(bytes.substr(at, nl - at));
        at = nl + 2;
    }
    return out;
}

/* Three signals polled in turn every 100 ms, from 100 ms: RPM, Speed,
 * Throttle, the throttle to one decimal; sweeps sweeps of them */
std::string polled_rows(int sweeps, int from_ms = 100)
{
    std::string s;
    for (int i = 0; i < sweeps; ++i) {
        const int t = from_ms + 100 * i;
        char b[160];
        std::snprintf(b, sizeof b,
                      "%d000,RPM,%d,rpm\n%d000,Speed,%d,km/h\n"
                      "%d000,Throttle,%d.5,%%\n",
                      t, 800 + i, t + 30, i / 2, t + 60, 10 + i);
        s += b;
    }
    return s;
}

std::string csv_head(const char* kind, const std::string& counts,
                     uint64_t ident, uint64_t text)
{
    char b[512];
    std::snprintf(b, sizeof b,
                  "# Havne %s, %s (ADR-0054): %s; left out: %llu values of "
                  "identification signals, %llu of text signals\n"
                  "time_s,signal,unit,value\n",
                  ecuio_version_string(), kind, counts.c_str(),
                  static_cast<unsigned long long>(ident),
                  static_cast<unsigned long long>(text));
    return b;
}

/* Each value a row, in host_us order, as recorded: a negative number
 * (which starts with '-' and is no formula), a name a comma makes CSV
 * quote, and no frames.csv at all (its values alone are a recording) */
TEST(CsvExportTest, EachValueIsARowAsRecorded)
{
    Folder f;
    recording(f, "",
              "1000,RPM,1726,rpm\n2000,Coolant,-12.5,C\n"
              "2500,\"Trim, bank 1\",3.1,%\n3000,RPM,1730,rpm\n");
    fs::remove(f.path / "rec" / "frames.csv");
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes,
              csv_head("a scrubbed export",
                       "4 values of 3 signals, 0.001000 s to 0.003000 s", 0,
                       0) +
                  "0.001000,RPM,rpm,1726\n"
                  "0.002000,Coolant,C,-12.5\n"
                  "0.002500,\"Trim, bank 1\",%,3.1\n"
                  "0.003000,RPM,rpm,1730\n");
    EXPECT_EQ(e.report.values, 4u);
    EXPECT_EQ(e.report.signal_count, 3u);
    EXPECT_EQ(e.report.frames, 0u);
    EXPECT_EQ(e.report.made_up, 0u);

    /* A range takes its values alone, and says where they run */
    CsvExported mid(f, 0, 1500, 2600, "mid.csv");
    ASSERT_EQ(mid.rc, ECUIO_OK) << mid.detail;
    EXPECT_EQ(mid.bytes,
              csv_head("a scrubbed export",
                       "2 values of 2 signals, 0.002000 s to 0.002500 s", 0,
                       0) +
                  "0.002000,Coolant,C,-12.5\n"
                  "0.002500,\"Trim, bank 1\",%,3.1\n");
    CsvExported none(f, 0, 9000, 0, "none.csv");
    ASSERT_EQ(none.rc, ECUIO_OK) << none.detail;
    EXPECT_EQ(none.bytes,
              csv_head("a scrubbed export", "0 values of 0 signals", 0, 0));
}

/* By the def first: a text DID's values and a number DID keyed in the
 * identification block (62 F18C) are left out, as the pcapng leaves
 * their frames out; then by kind: a value that isn't a number, and a
 * signal the header declares text even where its value reads as one.
 * Raw keeps every value. */
TEST(CsvExportTest, TextAndIdentificationSignalsAreLeftOut)
{
    const std::string header =
        "uri  elm327://COM3?profile=uds-generic&did=D802:text,D803:u16,"
        "F18C:u16\n"
        "profile  uds-generic\n"
        "signal  Code,text,,0,0\n";
    Folder f;
    recording(f, "",
              "1000,DID D802,ECM-HavneSim,\n2000,DID D803,4660,\n"
              "3000,DID F18C,424242,\n4000,DID D803,4661,\n"
              "5000,Note,hello there,\n6000,Code,123456,\n",
              header);
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes,
              csv_head("a scrubbed export",
                       "2 values of 1 signals, 0.002000 s to 0.004000 s", 1,
                       3) +
                  "0.002000,DID D803,,4660\n"
                  "0.004000,DID D803,,4661\n");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 3u);

    CsvExported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.csv");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.report.values, 6u);
    EXPECT_EQ(raw.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 0u);
    EXPECT_NE(raw.bytes.find("a raw export, not scrubbed"), std::string::npos);
    EXPECT_NE(raw.bytes.find("0.005000,Note,,hello there\n"),
              std::string::npos);
}

/* The check over the written bytes: a unit echoing an identification
 * reply's value from frames.csv, a name shaped as a VIN */
TEST(CsvExportTest, AnIdentityValueInTheFileFailsTheExport)
{
    {
        SCOPED_TRACE("a calibration ID from the frames, as a unit");
        Folder f;
        recording(f,
                  row(1000, 0x7E8, kWhole,
                      text_bytes({0x49, 0x04, 0x01}, "FAKECAL-01")),
                  "2000,RPM,1726,FAKECAL-01\n");
        CsvExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail,
                  "the check found an identification reply's value in the "
                  "export's file: nothing was written");
        expect_no_file(f, "out.csv");
    }
    {
        SCOPED_TRACE("a VIN-shaped name");
        Folder f;
        recording(f, "", std::string("2000,") + kVin + ",1,\n");
        CsvExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail.find(kVin), std::string::npos);
        expect_no_file(f, "out.csv");
    }
}

/* A spreadsheet runs a cell that starts with = + - @, a tab or a CR: a
 * name, a unit or a raw text value starting so fails the export (no
 * escape, ADR-0049 section 8); a number never does */
TEST(CsvExportTest, AFormulasStartFailsTheExport)
{
    const auto refused = [](const std::string& signals, uint32_t flags,
                            const char* expected) {
        Folder f;
        recording(f, "", signals);
        CsvExported e(f, flags);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED) << signals;
        EXPECT_EQ(e.detail, expected);
        expect_no_file(f, "out.csv");
    };
    refused("1000,Boost,3,-x\n", 0,
            "the signal 'Boost''s unit starts as a spreadsheet formula does "
            "(=, +, -, @, a tab or a carriage return): nothing was written");
    refused("1000,=SUM(A1),3,kPa\n", 0,
            "the signal '=SUM(A1)''s name starts as a spreadsheet formula "
            "does (=, +, -, @, a tab or a carriage return): nothing was "
            "written");
    refused("1000,Note,@cmd,\n", ECUIO_EXPORT_RAW,
            "the signal 'Note''s text value starts as a spreadsheet formula "
            "does (=, +, -, @, a tab or a carriage return): nothing was "
            "written");

    /* Scrubbed, that text value is left out, so nothing fails */
    Folder f;
    recording(f, "", "1000,Note,@cmd,\n2000,Boost,-3,kPa\n");
    CsvExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.values, 1u);
    EXPECT_NE(e.bytes.find("0.002000,Boost,kPa,-3\n"), std::string::npos);
}

TEST(CsvExportTest, ARecordingWithoutValuesOrWithABadRowWritesNothing)
{
    Folder f;
    recording(f, row(1000, 0x7E8, 0, {0x02, 0x41, 0x0C}));
    CsvExported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the folder has no signals.csv");
    expect_no_file(f, "out.csv");

    Folder g;
    recording(g, "", "1000,RPM,1726,rpm\nnot a row\n");
    CsvExported bad(g);
    EXPECT_EQ(bad.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(bad.detail.rfind("signals.csv line 3: ", 0), 0u) << bad.detail;
    expect_no_file(g, "out.csv");
}

// ---- the folder export: a scrubbed copy of the recording ----

// ---- The wide CSV (ADR-0056 sections 4 and 5) ----

/* A row a sweep: once every column has a sample newer than the last
 * row, at the completing sample's time in whole ms; the columns every
 * number signal as first met, their decimals the most their values
 * show; CRLF, no comment line, the report's rows and cells */
TEST(CsvExportTest, WideWritesARowPerSweepOverEveryNumberSignal)
{
    Folder f;
    recording(f, "", polled_rows(3));
    fs::remove(f.path / "rec" / "frames.csv");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h),Throttle (%)\r\n"
                       "160,800,0,10.5\r\n"
                       "260,801,0,11.5\r\n"
                       "360,802,1,12.5\r\n");
    EXPECT_EQ(e.report.rows, 3u);
    EXPECT_EQ(e.report.values, 9u);
    EXPECT_EQ(e.report.signal_count, 3u);
    EXPECT_EQ(e.report.frames, 0u);

    /* A range: the rows its samples complete; the time stays the
     * recording's */
    WideExported mid(f, {}, 0, ECUIO_UNITS_METRIC, 0, 200000, 300000,
                     "mid.csv");
    ASSERT_EQ(mid.rc, ECUIO_OK) << mid.detail;
    EXPECT_EQ(mid.bytes, "Time (ms),RPM (rpm),Speed (km/h),Throttle (%)\r\n"
                         "260,801,0,11.5\r\n");
    WideExported none(f, {}, 0, ECUIO_UNITS_METRIC, 0, 9000000, 0, "none.csv");
    ASSERT_EQ(none.rc, ECUIO_OK) << none.detail;
    EXPECT_EQ(none.bytes, "Time (ms)\r\n");
    EXPECT_EQ(none.report.rows, 0u);
}

/* A signal that stops answering leaves the rule after 1 s of silence
 * (3x its usual interval being shorter): the rows go on without it,
 * its cells empty, and it rejoins with its next sample */
TEST(CsvExportTest, WideAColumnThatStopsLeavesTheRuleAndRejoins)
{
    std::string rows;
    for (int t = 100; t <= 3000; t += 100) {
        rows += std::to_string(t) + "000,RPM," + std::to_string(t) + ",rpm\n";
        if (t == 100 || t == 200 || t == 2500) {
            rows += std::to_string(t + 50) + "000,Speed," +
                    std::to_string(t / 100) + ",km/h\n";
        }
    }
    Folder f;
    recording(f, "", rows);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_EQ(got.size(), 17u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),Speed (km/h)");
    EXPECT_EQ(got[1], "150,100,1");
    EXPECT_EQ(got[2], "250,200,2");
    /* Speed is quiet from 250 ms: at 1300 ms the gap passes 1 s and the
     * sweeps go on without it */
    EXPECT_EQ(got[3], "1300,1300,") << "the first row after the silence";
    EXPECT_EQ(got[4], "1400,1400,");
    EXPECT_EQ(got[15], "2500,2500,");
    /* Its sample at 2550 ms rejoins; the next RPM completes the row */
    EXPECT_EQ(got[16], "2600,2600,25");
    EXPECT_EQ(e.report.rows, 16u);
    EXPECT_EQ(e.report.values, 16u + 3u);
}

/* A slow signal's own interval sets its silence, 3x it: one every
 * 500 ms leaves the rule 1.5 s after its last, not 1 s */
TEST(CsvExportTest, WideASlowColumnsIntervalSetsItsSilence)
{
    std::string rows;
    for (int t = 100; t <= 4000; t += 100) {
        rows += std::to_string(t) + "000,RPM," + std::to_string(t) + ",rpm\n";
        if (t == 500 || t == 1000 || t == 1500) {
            rows += std::to_string(t + 10) + "000,Slow," +
                    std::to_string(t / 500) + ",\n";
        }
    }
    Folder f;
    recording(f, "", rows);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_GE(got.size(), 5u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),RPM (rpm),Slow");
    EXPECT_EQ(got[1], "510,500,1");
    EXPECT_EQ(got[2], "1010,1000,2");
    EXPECT_EQ(got[3], "1510,1500,3");
    EXPECT_EQ(got[4], "3100,3100,")
        << "quiet 1.6 s at 3100 ms, past 3x its 500 ms; not at 2600";
    EXPECT_EQ(got.size(), 4u + 10u) << "then a row an RPM to 4000 ms";
}

/* A grid: a row at each interval from the recording's zero with the
 * newest values at its time, held; a step with no new cell skipped;
 * the last values on the step after the last sample */
TEST(CsvExportTest, WideAGridHoldsTheNewestValuesAndSkipsAnEmptyStep)
{
    Folder f;
    recording(f, "",
              "50000,RPM,800,rpm\n60000,Speed,3,km/h\n120000,RPM,810,rpm\n"
              "530000,RPM,820,rpm\n");
    WideExported e(f, {}, 100);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "100,800,3\r\n"
                       "200,810,3\r\n"
                       "600,820,3\r\n");
    EXPECT_EQ(e.report.rows, 3u);
    EXPECT_EQ(e.report.values, 6u);

    /* A --from past the zero keeps the grid on the zero's multiples */
    WideExported late(f, {}, 100, ECUIO_UNITS_METRIC, 0, 55000, 0, "late.csv");
    ASSERT_EQ(late.rc, ECUIO_OK) << late.detail;
    EXPECT_EQ(late.bytes, "Time (ms),Speed (km/h),RPM (rpm)\r\n"
                          "100,3,\r\n"
                          "200,3,810\r\n"
                          "600,3,820\r\n");
}

/* Each cell in its signal's decimal places from the header's signal
 * line, converted to the units asked for (one more place where the
 * conversion needs it), the head cell in the unit shown */
TEST(CsvExportTest, WideASweepRowsTimeIsTheCompletingSamplesToTheNearestMs)
{
    Folder f;
    recording(f, "",
              "100600,RPM,800,rpm\n100700,Speed,3,km/h\n"
              "200000,RPM,801,rpm\n200500,Speed,4,km/h\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "101,800,3\r\n"
                       "201,801,4\r\n");
}

TEST(CsvExportTest, WideAGridAppliesTheSilenceAtTheStepItWrites)
{
    /* Speed answers once; RPM's sample at 1050 ms finds it 990 ms quiet
     * (in the rule), the grid's step at 1100 ms finds it past 1 s: the
     * step writes RPM alone */
    Folder f;
    recording(f, "",
              "50000,RPM,800,rpm\n60000,Speed,3,km/h\n"
              "1050000,RPM,810,rpm\n1250000,RPM,820,rpm\n");
    WideExported e(f, {}, 100);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm),Speed (km/h)\r\n"
                       "100,800,3\r\n"
                       "1100,810,\r\n"
                       "1300,820,\r\n");
    EXPECT_EQ(e.report.values, 4u);
}

TEST(CsvExportTest, WideAGridJumpsAGapWithNothingNew)
{
    /* Two samples 1e9 s apart at --rows 1: a step at a time would be
     * 1e12 steps; the file is two rows and the export is quick */
    Folder f;
    recording(f, "", "0,RPM,800,rpm\n1000000000000000,RPM,801,rpm\n");
    const auto begun = std::chrono::steady_clock::now();
    WideExported e(f, {}, 1);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm)\r\n"
                       "0,800\r\n"
                       "1000000000000,801\r\n");
    EXPECT_EQ(e.report.rows, 2u);
    EXPECT_LT(std::chrono::steady_clock::now() - begun,
              std::chrono::seconds(5));
}

TEST(CsvExportTest, WideAValuePastTheFixedDigitsIsWrittenAsRecorded)
{
    Folder f;
    recording(f, "", "100000,RPM,1e17,rpm\n200000,RPM,-2.5e16,rpm\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),RPM (rpm)\r\n"
                       "100,1e+17\r\n"
                       "200,-2.5e+16\r\n");
    EXPECT_EQ(e.report.values, 2u);
}

TEST(CsvExportTest, WideCellsFollowTheHeadersDecimalsAndTheUnits)
{
    const std::string header = "profile  obd2\n"
                               "signal  Speed,number,km/h,0,1,0\n"
                               "signal  Boost,number,kPa,1,2,0\n";
    Folder f;
    recording(f, "", "1000,Speed,100,km/h\n2000,Boost,101.3,kPa\n", header);
    WideExported metric(f);
    ASSERT_EQ(metric.rc, ECUIO_OK) << metric.detail;
    EXPECT_EQ(metric.bytes, "Time (ms),Speed (km/h),Boost (kPa)\r\n"
                            "2,100,101.3\r\n");
    WideExported imperial(f, {}, 0, ECUIO_UNITS_IMPERIAL, 0, 0, 0,
                          "imperial.csv");
    ASSERT_EQ(imperial.rc, ECUIO_OK) << imperial.detail;
    EXPECT_EQ(imperial.bytes, "Time (ms),Speed (mph),Boost (psi)\r\n"
                              "2,62,14.69\r\n");
}

/* The columns named, in their order: one the recording lacks is empty,
 * and leaves the rule after 1 s so the rows go on; a text or
 * identification signal, or a name twice, is refused */
TEST(CsvExportTest, WideNamedColumnsComeInTheirOrder)
{
    Folder f;
    recording(f, "", polled_rows(12));
    WideExported e(f, {"Speed", "RPM", "Nothing"});
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::vector<std::string> got = wide_lines(e.bytes);
    ASSERT_GE(got.size(), 2u) << e.bytes;
    EXPECT_EQ(got[0], "Time (ms),Speed (km/h),RPM (rpm),Nothing");
    EXPECT_EQ(got[1], "130,0,800,")
        << "the first sweep: Nothing has no sample, so it holds no row up";
    EXPECT_EQ(got.size(), 13u) << "a row a sweep";
    EXPECT_EQ(e.report.signal_count, 3u);

    const std::string header =
        "uri  mock://x?profile=uds-generic&did=D802:text,"
        "F18C:u16\n"
        "profile  uds-generic\n";
    Folder g;
    recording(g, "",
              "1000,DID D802,ECM,\n2000,DID F18C,4242,\n3000,RPM,1,rpm\n",
              header);
    WideExported text(g, {"DID D802"});
    EXPECT_EQ(text.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(text.detail.find("text signal"), std::string::npos)
        << text.detail;
    WideExported ident(g, {"DID F18C"});
    EXPECT_EQ(ident.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(ident.detail.find("identification signal"), std::string::npos)
        << ident.detail;
    WideExported twice(g, {"RPM", "RPM"});
    EXPECT_EQ(twice.rc, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(twice.detail.find("named twice"), std::string::npos)
        << twice.detail;
    EXPECT_FALSE(fs::exists(g.path / "wide.csv"));
}

/* No columns named: the header's focus line picks them, in its order;
 * without one, every number signal */
TEST(CsvExportTest, WideTakesTheFocusLinesColumnsWhenNoneAreNamed)
{
    Folder f;
    recording(f, "", polled_rows(2), "profile  raw\nfocus  Speed,RPM\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),Speed (km/h),RPM (rpm)\r\n"
                       "130,0,800\r\n"
                       "230,0,801\r\n");
    WideExported all(f, {"Throttle"}, 0, ECUIO_UNITS_METRIC, 0, 0, 0,
                     "all.csv");
    ASSERT_EQ(all.rc, ECUIO_OK) << all.detail;
    EXPECT_EQ(all.bytes, "Time (ms),Throttle (%)\r\n160,10.5\r\n260,11.5\r\n");
}

/* A focus line that names a column twice, or a text or identification
 * signal, is the recording's fault: LOAD_FAILED with the reason, not the
 * caller's INVALID_ARGUMENT, and no file */
TEST(CsvExportTest, WideAFocusLineThatCannotBeColumnsIsABadRecording)
{
    const std::string rows =
        "1000,DID D802,ECM,\n2000,DID F18C,4242,\n3000,RPM,1,rpm\n";
    const struct {
        const char* focus;
        const char* reason;
    } cases[] = {{"RPM,RPM", "named twice"},
                 {"DID D802", "text signal"},
                 {"DID F18C", "identification signal"}};
    for (const auto& c : cases) {
        Folder g;
        recording(g, "", rows,
                  std::string("uri  mock://x?profile=uds-generic&did="
                              "D802:text,F18C:u16\n"
                              "profile  uds-generic\n"
                              "focus  ") +
                      c.focus + "\n");
        WideExported e(g);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED) << c.focus;
        EXPECT_NE(e.detail.find(c.reason), std::string::npos) << e.detail;
        EXPECT_FALSE(fs::exists(g.path / "wide.csv")) << c.focus;
    }
}

/* Text and identification signals are never columns, scrubbed, and
 * their values are counted left out as the long form counts them; the
 * header's number signals come first, in its order, with no sample
 * dropped */
TEST(CsvExportTest, WideTextAndIdentificationSignalsAreNeverColumns)
{
    const std::string header =
        "uri  elm327://COM3?profile=uds-generic&did=D802:text,D803:u16,"
        "F18C:u16\n"
        "profile  uds-generic\n"
        "signal  Code,text,,0,0\n"
        "signal  Quiet,number,,0,0\n"
        "signal  DID D803,number,,0,0\n";
    Folder f;
    recording(f, "",
              "1000,DID D802,ECM-HavneSim,\n2000,DID D803,4660,\n"
              "3000,DID F18C,424242,\n4000,DID D803,4661,\n"
              "5000,Note,hello there,\n6000,Code,123456,\n7000,Extra,7,\n",
              header);
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    /* Extra, in the rule from the start, holds the rows until its first
     * sample, which completes the one row */
    EXPECT_EQ(e.bytes, "Time (ms),DID D803,Extra\r\n7,4661,7\r\n");
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 3u);
}

/* A name holding a comma is quoted in the head cell; a formula's start
 * in a name fails the export, leaving no file */
TEST(CsvExportTest, WideQuotesAHeadAndRefusesAFormula)
{
    Folder f;
    recording(f, "",
              "1000,\"Trim, bank 1\",3.1,%\n2000,\"Trim, bank 1\",3.2,%\n");
    WideExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.bytes, "Time (ms),\"Trim, bank 1 (%)\"\r\n1,3.1\r\n2,3.2\r\n");
    Folder g;
    recording(g, "", "1000,=RPM,5,rpm\n");
    WideExported bad(g);
    EXPECT_EQ(bad.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(bad.detail.find("formula"), std::string::npos) << bad.detail;
    EXPECT_FALSE(fs::exists(g.path / "wide.csv"));
}

/* The estimate answers several intervals in one pass with what each
 * export then writes: the rows, the bytes exactly, the columns */
TEST(ExportTest, TheEstimateMatchesTheFileForEachInterval)
{
    Folder f;
    recording(f, "", polled_rows(30));
    const std::string folder = f.utf8("rec");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    const uint32_t intervals[3] = {0, 100, 250};
    ecuio_export_estimate_t est[3];
    ASSERT_EQ(ecuio_export_estimate(&o, intervals, 3, est), ECUIO_OK)
        << ecuio_last_error_detail();
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(intervals[i]);
        const std::string name = "est" + std::to_string(i) + ".csv";
        WideExported e(f, {}, intervals[i], ECUIO_UNITS_METRIC, 0, 0, 0, name);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(est[i].rows, e.report.rows);
        EXPECT_EQ(est[i].bytes, e.bytes.size());
        EXPECT_EQ(est[i].columns, 3u);
        EXPECT_EQ(est[i].flags, 0u);
        EXPECT_GT(est[i].rows, 0u);
    }
    EXPECT_GT(est[1].rows, est[2].rows) << "a 250 ms grid thins the sweeps";

    /* Named columns (one quoted, one the recording lacks), a range and
     * imperial units: the estimate is still the file's */
    Folder g;
    recording(g, "",
              polled_rows(10) + "2000000,\"Boost, abs\",101,kPa\n"
                                "2100000,\"Boost, abs\",102.5,kPa\n");
    const std::string gfolder = g.utf8("rec");
    const char* columns[3] = {"Throttle", "Boost, abs", "Nothing"};
    ecuio_export_options_t n{};
    n.format = ECUIO_EXPORT_CSV;
    n.folder_utf8 = gfolder.c_str();
    n.from_us = 500000;
    n.to_us = 2050000;
    n.units = ECUIO_UNITS_IMPERIAL;
    n.column_count = 3;
    n.columns_utf8 = columns;
    ecuio_export_estimate_t named[3];
    ASSERT_EQ(ecuio_export_estimate(&n, intervals, 3, named), ECUIO_OK)
        << ecuio_last_error_detail();
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(intervals[i]);
        const std::string name = "named" + std::to_string(i) + ".csv";
        WideExported e(g, {"Throttle", "Boost, abs", "Nothing"}, intervals[i],
                       ECUIO_UNITS_IMPERIAL, 0, 500000, 2050000, name);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(named[i].rows, e.report.rows);
        EXPECT_EQ(named[i].bytes, e.bytes.size());
        EXPECT_EQ(named[i].columns, 3u);
        EXPECT_GT(named[i].rows, 0u);
        EXPECT_EQ(wide_lines(e.bytes)[0],
                  "Time (ms),Throttle (%),\"Boost, abs (psi)\",Nothing");
    }
}

/* The wide CSV has 16384 columns at most, found or named: the refusal
 * names the limit, comes from the survey (a few seconds for 16384
 * columns, not minutes) and leaves no file */
TEST(ExportTest, TheWideCsvRefusesMoreThanTheColumnLimit)
{
    const auto names = [](size_t n) {
        std::string rows;
        for (size_t i = 0; i < n; ++i) {
            rows += std::to_string(1000 * (i + 1)) + ",S" + std::to_string(i) +
                    ",1,\n";
        }
        return rows;
    };
    Folder at;
    recording(at, "", names(16384));
    WideExported ok(at);
    ASSERT_EQ(ok.rc, ECUIO_OK) << ok.detail;
    EXPECT_EQ(ok.report.signal_count, 16384u);
    Folder over;
    recording(over, "", names(16385));
    WideExported found(over);
    EXPECT_EQ(found.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(found.detail,
              "the wide CSV is limited to 16384 columns: pick the columns to "
              "keep");
    EXPECT_FALSE(fs::exists(over.path / "wide.csv"));
    EXPECT_FALSE(partial_left(over.path, "wide.csv"));

    /* The estimate refuses it the same way */
    {
        const std::string folder = over.utf8("rec");
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_CSV;
        o.folder_utf8 = folder.c_str();
        const uint32_t ms = 0;
        ecuio_export_estimate_t est;
        EXPECT_EQ(ecuio_export_estimate(&o, &ms, 1, &est),
                  ECUIO_ERR_LOAD_FAILED);
    }

    /* Declared in events.log's header: the same limit */
    {
        std::string header = "profile  obd2\n";
        for (size_t i = 0; i < 16385; ++i) {
            header += "signal  H" + std::to_string(i) + ",number,,0,0,0\n";
        }
        Folder declared;
        recording(declared, "", "1000,H0,1,\n", header);
        WideExported d(declared);
        EXPECT_EQ(d.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_NE(d.detail.find("limited to 16384 columns"), std::string::npos)
            << d.detail;
    }

    /* Named: the limit holds for the picks, which the survey reads first */
    std::vector<std::string> store;
    for (size_t i = 0; i < 16385; ++i) {
        store.push_back("S" + std::to_string(i));
    }
    std::vector<const char*> picks;
    for (size_t i = 0; i < 16384; ++i) {
        picks.push_back(store[i].c_str());
    }
    Folder few;
    recording(few, "", names(3));
    WideExported named(few, picks, 0, ECUIO_UNITS_METRIC, 0, 0, 0, "n.csv");
    ASSERT_EQ(named.rc, ECUIO_OK) << named.detail;
    picks.push_back(store[16384].c_str());
    WideExported named_over(few, picks, 0, ECUIO_UNITS_METRIC, 0, 0, 0,
                            "o.csv");
    EXPECT_EQ(named_over.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(named_over.detail.find("limited to 16384 columns"),
              std::string::npos)
        << named_over.detail;
    EXPECT_FALSE(fs::exists(few.path / "o.csv"));
}

/* The estimate takes 64 intervals in a call, no more, and refuses the
 * 65th before it allocates or reads anything */
TEST(ExportTest, TheEstimateRefusesMoreThanSixtyFourIntervals)
{
    Folder f;
    recording(f, "", polled_rows(5));
    const std::string folder = f.utf8("rec");
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.folder_utf8 = folder.c_str();
    std::vector<uint32_t> ms(65, 100);
    std::vector<ecuio_export_estimate_t> est(65);
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 64, est.data()), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_GT(est[63].rows, 0u);
    est[0].rows = 7;
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 65, est.data()),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(est[0].rows, 7u) << "refused before out was touched";
    /* A count no array could back: refused before any allocation */
    EXPECT_EQ(ecuio_export_estimate(&o, ms.data(), 0xFFFFFFFFu, est.data()),
              ECUIO_ERR_INVALID_ARGUMENT);
}

struct FolderExported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::string detail;
    std::map<std::string, std::string> files;
    FolderExported(const Folder& f, uint32_t flags = 0, uint64_t from_us = 0,
                   uint64_t to_us = 0, const std::string& name = "copy")
    {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(name);
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_FOLDER;
        o.flags = flags;
        o.from_us = from_us;
        o.to_us = to_us;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        rc = ecuio_export(&o, &report);
        detail = ecuio_last_error_detail();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(f.path / name, ec)) {
            files[e.path().filename().generic_u8string()] = read_file(e.path());
        }
    }
};

const char* const kBomHeader = "\xEF\xBB\xBFhost_us,signal,value,unit\n";

const char* const kColumns =
    "columns  local time, seconds since the recording started (host_us / "
    "1e6 in the CSV files), kind, text\n";

/* A message an adapter delivered whole, longer than a frame: one row */
const std::vector<uint8_t> kWholeReply = {0x41, 0x0C, 0x1A, 0xF8, 0x0D,
                                          0x32, 0x05, 0x5A, 0x04, 0x80};
/* Twelve bytes with no FD flag, which no CAN frame carries */
const std::vector<uint8_t> kTwelve = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

/* A recording with every file the export knows and one it doesn't: an
 * ELM327 session through the app begun after start (a focus and the
 * link's state on the header), a VIN reply in its frames and its
 * values, a marker, reply timing and a file of the user's */
void whole_recording(const Folder& f)
{
    f.write("rec/events.log",
            std::string("ecuio session recording, format 3 (ADR-0049)\n"
                        "started  2023-11-14 22:13:20.250 (UTC)\n"
                        "utc  2023-11-14T22:13:20.250Z\n"
                        "core  0.5.1\n"
                        "uri  elm327://COM3?profile=obd2&serial=115200&"
                        "filter=7E8&label=Daily\n"
                        "profile  obd2\n"
                        "frame flags  complete\n"
                        "focus  RPM\n"
                        "link  recovered (state 0, 1 episode)\n"
                        "signal  RPM,number,rpm,0,1\n"
                        "signal  VIN,text,,0,0\n"
                        "caller  Havne 0.5.1\n"
                        "caller  vehicle  Daily driver\n"
                        "caller  words of the caller's own\n") +
                kColumns +
                "\n"
                "22:13:21.250  +1.000  note  Connected to the Daily driver\n"
                "22:13:22.250  +2.000  link  vehicle not responding (state "
                "8, 1 episode)\n"
                "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                "was read meanwhile\n"
                "22:13:24.250  +4.000  end  Disconnected by the user: 3 "
                "frames, 2 values, 0 records dropped, 0 notes dropped\n");
    f.write(
        "rec/frames.csv",
        "host_us,timestamp_us,id,flags,data\n" +
            row(1000000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
            row(1500000, 0x7E8, kWhole, kWholeReply) +
            row(2000000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)) +
            row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
            row(3200000, 0x456, 0, kTwelve));
    f.write("rec/signals.csv", std::string(kBomHeader) +
                                   "1000000,RPM,1726,rpm\n"
                                   "2500000,VIN," +
                                   kVin + ",\n3000000,RPM,1730,rpm\n");
    f.write("rec/markers.csv",
            "host_us,label\n1500000,\"pull, 2nd gear\"\n9000000,late\n");
    f.write("rec/latency.csv",
            "t_s,ecu,request,answered\n1.000,7E8,01 0C,5\n3.000,7E8,01 "
            "0C,9\n");
    f.write("rec/notes.txt", "the user's own file\n");
}

std::string export_line(const char* kind, uint64_t frames, uint64_t ident,
                        uint64_t values, uint64_t text, uint64_t markers,
                        uint64_t lines, unsigned others)
{
    char b[1024];
    std::snprintf(
        b, sizeof b,
        "export  %s (ADR-0054), Havne %s: frames.csv %llu frames (left out: "
        "%llu of identification replies, 0 of text signals' messages, 0 "
        "segments of messages that couldn't be reassembled); signals.csv "
        "%llu values (left out: 0 of identification signals, %llu of text "
        "signals); markers.csv %llu markers, their labels as typed; "
        "events.log %llu lines left out (the caller's notes, its other "
        "lines and lines of a kind the export doesn't know); other files "
        "left out: %u\n",
        kind, ecuio_version_string(), static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(ident),
        static_cast<unsigned long long>(values),
        static_cast<unsigned long long>(text),
        static_cast<unsigned long long>(markers),
        static_cast<unsigned long long>(lines), others);
    return b;
}

const char* const kHead =
    "ecuio session recording, format 3 (ADR-0049)\n"
    "started  2023-11-14 22:13:20.250 (UTC)\n"
    "utc  2023-11-14T22:13:20.250Z\n"
    "core  0.5.1\n"
    "uri  elm327://<address>?profile=obd2&serial=115200&filter=7E8\n"
    "profile  obd2\n"
    "frame flags  complete\n"
    "focus  RPM\n"
    "link  recovered (state 0, 1 episode)\n"
    "signal  RPM,number,rpm,0,1\n"
    "signal  VIN,text,,0,0\n"
    "caller  Havne 0.5.1\n"
    "caller  vehicle  <vehicle>\n";

/* Each file rewritten by its classes: the URI's port and a key that
 * isn't core's out, the vehicle's label, the caller's other line and
 * its note out, the VIN reply's frame and the VIN's value out, the
 * kept rows as recorded (a message delivered whole one row, a frame the
 * pcapng couldn't carry, signals.csv's byte order mark), the user's
 * file out and counted, the export's own line before the columns */
TEST(FolderExportTest, EachFileIsCopiedThroughItsClasses)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.size(), 5u);
    EXPECT_EQ(e.files["events.log"],
              std::string(kHead) +
                  export_line("a scrubbed copy", 4, 1, 2, 1, 2, 2, 1) +
                  kColumns +
                  "\n"
                  "22:13:22.250  +2.000  link  vehicle not responding "
                  "(state 8, 1 episode)\n"
                  "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                  "was read meanwhile\n"
                  "22:13:24.250  +4.000  end  <why>: 3 frames, 2 values, 0 "
                  "records dropped, 0 notes dropped\n");
    EXPECT_EQ(e.files["frames.csv"],
              "host_us,timestamp_us,id,flags,data\n" +
                  row(1000000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}) +
                  row(1500000, 0x7E8, kWhole, kWholeReply) +
                  row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
                  row(3200000, 0x456, 0, kTwelve));
    EXPECT_EQ(e.files["signals.csv"],
              std::string(kBomHeader) +
                  "1000000,RPM,1726,rpm\n3000000,RPM,1730,rpm\n");
    EXPECT_EQ(e.files["markers.csv"],
              "host_us,label\n1500000,\"pull, 2nd gear\"\n9000000,late\n");
    EXPECT_EQ(e.files["latency.csv"],
              "t_s,ecu,request,answered\n1.000,7E8,01 0C,5\n3.000,7E8,01 "
              "0C,9\n");
    EXPECT_EQ(e.report.frames, 4u);
    EXPECT_EQ(e.report.made_up, 0u);
    EXPECT_EQ(e.report.values, 2u);
    EXPECT_EQ(e.report.signal_count, 1u);
    EXPECT_EQ(e.report.files, 5u);
    EXPECT_EQ(e.report.files_left_out, 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 2u);
    EXPECT_FALSE(partial_left(f.path, "copy"));

    /* The copy is a recording: exported again as CSV, its values are
     * the source's */
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.layout = ECUIO_CSV_LONG;
    const std::string copy = f.utf8("copy");
    const std::string out = f.utf8("copy.csv");
    o.folder_utf8 = copy.c_str();
    o.out_path_utf8 = out.c_str();
    ecuio_export_report_t r{};
    ASSERT_EQ(ecuio_export(&o, &r), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_NE(read_file(f.path / "copy.csv")
                  .find("time_s,signal,unit,value\n1.000000,RPM,rpm,1726\n"),
              std::string::npos);
    /* And wide, the default: the copied focus line picks the columns */
    const std::string wide = f.utf8("copy-wide.csv");
    o.layout = ECUIO_CSV_WIDE;
    o.out_path_utf8 = wide.c_str();
    ASSERT_EQ(ecuio_export(&o, &r), ECUIO_OK) << ecuio_last_error_detail();
    const std::vector<std::string> wl =
        wide_lines(read_file(f.path / "copy-wide.csv"));
    ASSERT_EQ(wl.size(), 3u);
    EXPECT_EQ(wl[0], "Time (ms),RPM (rpm)");
    EXPECT_EQ(wl[1], "1000,1726");
    EXPECT_EQ(wl[2], "3000,1730");
    EXPECT_EQ(r.rows, 2u);
}

/* A range takes each file's rows and lines in it alone; the end line
 * stays, the header whole */
TEST(FolderExportTest, ARangeTakesEachFilesRowsInItAlone)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f, 0, 1500000, 3500000);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files["events.log"],
              std::string(kHead) +
                  export_line("a scrubbed copy", 3, 1, 1, 1, 1, 1, 1) +
                  kColumns +
                  "\n"
                  "22:13:22.250  +2.000  link  vehicle not responding "
                  "(state 8, 1 episode)\n"
                  "22:13:23.250  +3.000  asleep  Asleep for 1.0 s: nothing "
                  "was read meanwhile\n"
                  "22:13:24.250  +4.000  end  <why>: 3 frames, 2 values, 0 "
                  "records dropped, 0 notes dropped\n");
    EXPECT_EQ(e.files["frames.csv"],
              "host_us,timestamp_us,id,flags,data\n" +
                  row(1500000, 0x7E8, kWhole, kWholeReply) +
                  row(3000000, 0x123, 0, {0x01, 0x02, 0x03}) +
                  row(3200000, 0x456, 0, kTwelve));
    EXPECT_EQ(e.files["signals.csv"],
              std::string(kBomHeader) + "3000000,RPM,1730,rpm\n");
    EXPECT_EQ(e.files["markers.csv"],
              "host_us,label\n1500000,\"pull, 2nd gear\"\n");
    EXPECT_EQ(e.files["latency.csv"],
              "t_s,ecu,request,answered\n3.000,7E8,01 0C,9\n");
}

/* Raw keeps every row and line as recorded, the user's file still out */
TEST(FolderExportTest, ARawCopyKeepsEveryRowAndLine)
{
    Folder f;
    whole_recording(f);
    FolderExported e(f, ECUIO_EXPORT_RAW);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    const std::string source = read_file(f.path / "rec" / "events.log");
    const size_t columns = source.find("columns  ");
    EXPECT_EQ(e.files["events.log"],
              source.substr(0, columns) +
                  export_line("a raw copy, not scrubbed", 5, 0, 3, 0, 2, 0, 1) +
                  source.substr(columns));
    EXPECT_EQ(e.files["frames.csv"], read_file(f.path / "rec" / "frames.csv"));
    EXPECT_EQ(e.files["signals.csv"],
              read_file(f.path / "rec" / "signals.csv"));
    EXPECT_EQ(e.files.count("notes.txt"), 0u);
    EXPECT_EQ(e.report.files_left_out, 1u);
}

/* A mock's label is the user's words; its profile and DID list stay, so
 * the copy decodes as the source did */
TEST(FolderExportTest, AMocksLabelIsLeftOutOfItsUri)
{
    Folder f;
    recording(f, row(1000, 0x7E8, 0, {0x02, 0x41, 0x00}), "",
              "uri  mock://Daily driver?profile=uds-generic&did=D802:text,"
              "D803:u16\nprofile  uds-generic\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_NE(e.files["events.log"].find(
                  "\nuri  mock://<label>?profile=uds-generic&did=D802:text,"
                  "D803:u16\n"),
              std::string::npos)
        << e.files["events.log"];
    EXPECT_EQ(e.files["events.log"].find("Daily"), std::string::npos);
}

/* A log of another format (format 1's, before ADR-0049) is a file the
 * export doesn't know: left out and counted, the frames still copied */
TEST(FolderExportTest, AnEventsLogOfAnotherFormatIsLeftOut)
{
    Folder f;
    recording(f, row(1000, 0x123, 0, {0x01}));
    f.write("rec/events.log", "Havne session recording (ADR-0017)\n"
                              "uri  elm327://COM3\n");
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files.size(), 1u);
    EXPECT_EQ(e.files.count("frames.csv"), 1u);
    EXPECT_EQ(e.report.files, 1u);
    EXPECT_EQ(e.report.files_left_out, 1u);
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], 0u);
}

/* A value the classes don't catch, in any file, fails the whole copy:
 * the VIN typed as a marker's label, a calibration ID in signals.json,
 * a DID echoing the VIN across the frames of a segmented reply (which
 * no row holds whole, so the check reassembles it), a VIN's shape in a
 * message delivered whole (each message checked as bytes, not only the
 * rows as text) */
TEST(FolderExportTest, AnIdentityValueInAnyFileFailsTheCopy)
{
    {
        SCOPED_TRACE("a VIN's shape as a marker's label");
        Folder f;
        whole_recording(f);
        /* not the recording's VIN, which the check finds as its value */
        f.write("rec/markers.csv",
                "host_us,label\n1500000,ZZZFAKE22M0000002\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail, "the check found a VIN-shaped token in the "
                            "copy's markers.csv: nothing was written");
        EXPECT_EQ(e.report.files, 0u);
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a DID echoing the VIN as it crossed the bus");
        Folder f;
        recording(
            f,
            row(1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)) +
                row(2000, 0x7E8, 0,
                    {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}) +
                row(2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}) +
                row(2200, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}) +
                row(2300, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}),
            "2000,RPM,1726,rpm\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail,
                  "the check found an identification reply's value in a "
                  "message the export would write: nothing was written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a VIN's shape in a message delivered whole, which "
                     "frames.csv holds only as hex");
        Folder f;
        recording(
            f, row(2000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x01}, kVin)),
            "2000,RPM,1726,rpm\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(e.detail,
                  "the check found a VIN-shaped token in a message the export "
                  "would write: nothing was written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    {
        SCOPED_TRACE("a calibration ID in signals.json");
        Folder f;
        recording(f,
                  row(1000, 0x7E8, kWhole,
                      text_bytes({0x49, 0x04, 0x01}, "FAKECAL-01")),
                  "2000,RPM,1726,rpm\n");
        f.write("rec/signals.json",
                R"({"havne_signals":1,"name":"T","notes":"FAKECAL-01",)"
                R"("source":{"name":"t"},"ecus":[{"id":"pcm","rx":"7E8"}],)"
                R"("signals":[{"key":"a","name":"A","read":{"service":"22",)"
                R"("number":"D100"},"field":{"byte":0}}]})"
                "\n");
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(e.detail, "the check found an identification reply's "
                            "value in the copy's signals.json: nothing was "
                            "written");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
}

/* A folder that exists, a source with no recording, a bad row, a name
 * or unit a spreadsheet would run: nothing is written */
TEST(FolderExportTest, ARefusedCopyWritesNothing)
{
    Folder f;
    whole_recording(f);
    fs::create_directories(f.path / "copy");
    FolderExported there(f);
    EXPECT_EQ(there.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(there.detail, "the folder exists already");

    Folder empty;
    FolderExported none(empty);
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(none.detail, "the folder has no frames.csv or signals.csv");
    EXPECT_FALSE(fs::exists(empty.path / "copy"));

    Folder bad;
    whole_recording(bad);
    bad.write("rec/latency.csv", "t_s,ecu\nsoon,7E8\n");
    FolderExported b(bad);
    EXPECT_EQ(b.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(b.detail, "latency.csv line 2: t_s isn't a time in seconds");
    EXPECT_FALSE(partial_left(bad.path, "copy"));

    Folder formula;
    recording(formula, "", "1000,=RPM,1726,rpm\n");
    FolderExported s(formula);
    EXPECT_EQ(s.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(s.detail.find("starts as a spreadsheet formula"),
              std::string::npos)
        << s.detail;
    FolderExported raw(formula, ECUIO_EXPORT_RAW);
    EXPECT_EQ(raw.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_FALSE(fs::exists(formula.path / "copy"));
}

// ---- frames in memory (ecuio_scrub_frames) ----

struct Mem {
    uint64_t t = 0;
    uint32_t id = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

struct Source {
    std::vector<Mem> frames;
    size_t calls = 0;
    size_t cancel_at = SIZE_MAX; /* the call that cancels */
    uint16_t length_past = 0;    /* a data_length to give frame 0 instead */
};

int give(void* user, size_t index, ecuio_frame_t* out)
{
    auto& s = *static_cast<Source*>(user);
    if (s.calls++ == s.cancel_at) {
        return 1;
    }
    const Mem& m = s.frames.at(index);
    out->timestamp_us = m.t;
    out->arbitration_id = m.id;
    out->flags = m.flags;
    out->data_length = static_cast<uint16_t>(m.data.size());
    std::memcpy(out->data, m.data.data(), m.data.size());
    if (index == 0 && s.length_past != 0) {
        out->data_length = s.length_past;
    }
    return 0;
}

struct Scrubbed {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_export_report_t report{};
    std::vector<uint8_t> keep;
    std::string detail;
    Scrubbed(Source& s, const ecuio_signal_set_t* set = nullptr)
        : keep(s.frames.size(), 7)
    {
        rc = ecuio_scrub_frames(s.frames.size(), give, &s, set, 0, keep.data(),
                                &report);
        detail = ecuio_last_error_detail();
    }
};

std::vector<uint8_t> none_kept(size_t n)
{
    return std::vector<uint8_t>(n, 0);
}

/* The VIN reply delivered whole, then as it crossed the bus around the
 * tester's request and Flow Control, then a value */
TEST(ScrubFramesTest, AnIdentificationReplyIsLeftOutEveryFrameOfIt)
{
    Source s;
    s.frames = {
        {1000, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)},
        {2000, 0x7DF, 0, {0x02, 0x09, 0x02, 0, 0, 0, 0, 0}},
        {2100, 0x7E8, 0, {0x10, 0x14, 0x49, 0x02, 0x01, 'Z', 'Z', 'Z'}},
        {2200, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {2300, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}},
        {2400, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}},
        {3000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 1, 0, 1, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 4u);
    EXPECT_EQ(r.report.frames, 3u);
    EXPECT_EQ(r.report.made_up, 0u);
    EXPECT_EQ(r.report.failed_class, 0u);
    EXPECT_EQ(s.calls, 2 * s.frames.size()) << "each frame asked for twice";
}

TEST(ScrubFramesTest, ASegmentNoMessageCanBeAttributedToIsLeftOutOnAReplyId)
{
    Source s;
    s.frames = {
        /* its First Frame came before the ring's oldest frame */
        {1000, 0x7E8, 0, {0x21, 'S', 'E', 'C', 'R', 'E', 'T', '1'}},
        /* a First Frame never finished */
        {2000, 0x7E9, 0, {0x10, 0x0A, 0x62, 0xD8, 0x01, 'A', 'B', 'C'}},
        /* on a request ID: as it is */
        {3000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}},
        /* delivered whole but cut short: a row holds it as it is */
        {4000, 0x7E8, kWhole | ECUIO_FRAME_FLAG_TRUNCATED, {0x62, 0xD8}},
        {5000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 1, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
    EXPECT_EQ(r.report.frames, 3u);
}

/* A text or hex def of the caller's set: its messages left out, whole
 * or segmented; with no set they stay */
TEST(ScrubFramesTest, ATextDefsMessagesAreLeftOutByTheSetsDef)
{
    const std::string json =
        R"({"havne_signals": 1, "name": "Scrub test", )"
        R"("source": {"name": "test_export"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
        R"("signals": [{"key": "ecu_name", "name": "ECU name", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "D802"}, )"
        R"("field": {"byte": 0}, "kind": "text"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    Source s;
    s.frames = {
        {1000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x02}, "ECM-HavneSim")},
        {2000, 0x7E8, 0, {0x10, 0x0F, 0x62, 0xD8, 0x02, 'E', 'C', 'M'}},
        {2100, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {2200, 0x7E8, 0, {0x21, '-', 'H', 'a', 'v', 'n', 'e', 'S'}},
        {2300, 0x7E8, 0, {0x22, 'i', 'm', 0, 0, 0, 0, 0}},
        {3000, 0x7E8, kWhole, {0x62, 0xD8, 0x03, 0x01, 0x02}},
    };
    Scrubbed r(s, ecuio_signal_file_set(file));
    EXPECT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], 4u);
    EXPECT_EQ(r.report.frames, 2u);

    Scrubbed all(s);
    EXPECT_EQ(all.rc, ECUIO_OK) << all.detail;
    EXPECT_EQ(all.keep, std::vector<uint8_t>(6, 1));
    ecuio_signal_file_free(file);
}

/* The check: a DID echoing the VIN, whole or segmented, and a VIN shape
 * with no identification reply; nothing kept, the class alone named */
TEST(ScrubFramesTest, AnIdentityValueOrAVinShapeInAKeptFrameFailsIt)
{
    const Mem vin_reply{1000, 0x7E8, kWhole,
                        text_bytes({0x49, 0x02, 0x01}, kVin)};
    const Mem echo{2000, 0x7E8, kWhole, text_bytes({0x62, 0xD8, 0x01}, kVin)};
    {
        SCOPED_TRACE("delivered whole");
        Source s;
        s.frames = {vin_reply, echo};
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(r.report.frames, 0u);
        EXPECT_EQ(r.keep, none_kept(2));
        EXPECT_EQ(r.detail,
                  "the check found an identification reply's value in a "
                  "frame the export would write: nothing was written");
    }
    {
        SCOPED_TRACE("as it crossed the bus, after the reply");
        Source s;
        s.frames = {
            {500, 0x7E8, 0, {0x10, 0x14, 0x62, 0xD8, 0x01, 'Z', 'Z', 'Z'}},
            {600, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
            {700, 0x7E8, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}},
            {800, 0x7E8, 0, {0x22, '0', '0', '0', '0', '0', '0', '1'}},
            vin_reply,
        };
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
        EXPECT_EQ(r.keep, none_kept(5));
    }
    {
        SCOPED_TRACE("with no identification reply: the VIN's shape");
        Source s;
        s.frames = {echo};
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(r.report.failed_class, ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1);
        EXPECT_EQ(r.detail,
                  "the check found a VIN-shaped token in a frame the export "
                  "would write: nothing was written");
        EXPECT_EQ(r.detail.find(kVin), std::string::npos);
        EXPECT_EQ(r.keep, none_kept(1));
    }
}

TEST(ScrubFramesTest, ArgumentsACancelAndAFrameTooLongAreRefused)
{
    Source s;
    s.frames = {{1000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
                {2000, 0x7E8, 0, {0x04, 0x41, 0x0C, 0x1A, 0xF9}}};
    uint8_t keep[2] = {7, 7};
    ecuio_export_report_t r{};
    r.frames = 9;
    EXPECT_EQ(ecuio_scrub_frames(2, nullptr, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.frames, 0u);
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 1, keep, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(s.calls, 0u) << "refused before any frame is asked for";
    EXPECT_EQ(ecuio_scrub_frames(0, give, &s, nullptr, 0, nullptr, &r),
              ECUIO_OK);
    EXPECT_EQ(r.frames, 0u);

    s.cancel_at = 3; /* the second pass's second frame */
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(keep[0] + keep[1], 0);
    EXPECT_EQ(s.calls, 4u);

    s.cancel_at = SIZE_MAX;
    s.calls = 0;
    s.length_past = ECUIO_FRAME_DATA_MAX + 1;
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "frame 0 holds more bytes than a frame can");

    s.length_past = 0;
    EXPECT_EQ(ecuio_scrub_frames(2, give, &s, nullptr, 0, keep, &r), ECUIO_OK);
    EXPECT_EQ(keep[0] + keep[1], 2);
    EXPECT_EQ(r.frames, 2u);
}

// ---- a message torn every way, and frames no message reads ----

/* A serial's reply (62 F1 8C, 33 bytes) as it crossed the bus, a Single
 * Frame of length 0 (malformed) between its first Consecutive Frame and
 * its second */
std::vector<Mem> serial_with_a_malformed_frame()
{
    const std::vector<uint8_t> msg =
        text_bytes({0x62, 0xF1, 0x8C}, "SERIALFAKE-0123456789ABCDEFGHI");
    auto cf = [&msg](uint8_t sn, size_t at) {
        std::vector<uint8_t> d{static_cast<uint8_t>(0x20 | sn)};
        for (size_t k = 0; k < 7; ++k) {
            d.push_back(at + k < msg.size() ? msg[at + k] : 0);
        }
        return d;
    };
    return {
        {1000,
         0x7E8,
         0,
         {0x10, 0x21, msg[0], msg[1], msg[2], msg[3], msg[4], msg[5]}},
        {2000, 0x7E8, 0, cf(1, 6)},
        {3000, 0x7E8, 0, {0, 0, 0, 0, 0, 0, 0, 0}},
        {4000, 0x7E8, 0, cf(2, 13)},
        {5000, 0x7E8, 0, cf(3, 20)},
        {6000, 0x7E8, 0, cf(4, 27)},
        {7000, 0x7E0, 0, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}},
    };
}

/* The malformed frame ends the message under way, so every frame of it
 * is torn: none of them pends into a message no frame list holds */
TEST(ScrubFramesTest, AMalformedFrameInAMessageTearsAllOfIt)
{
    Source s;
    s.frames = serial_with_a_malformed_frame();
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 6u);
    EXPECT_EQ(r.report.frames, 1u);
}

TEST(FolderExportTest, AMalformedFrameInAMessageTearsAllOfIt)
{
    Folder f;
    std::string rows;
    for (const Mem& m : serial_with_a_malformed_frame()) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    recording(f, rows);
    FolderExported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.files["frames.csv"],
              "host_us,timestamp_us,id,flags,data\n" +
                  row(7000, 0x7E0, 0, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}));
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 6u);
}

/* Torn segments on any 18DA ID: a reply to a tester at F9, and one at F1
 * (whose First Frames came before the frames began) */
TEST(ScrubFramesTest, ATornSegmentOnAny18DaIdIsLeftOut)
{
    const uint32_t x = ECUIO_FRAME_FLAG_EXTENDED;
    Source s;
    s.frames = {
        {1000, 0x18DAF910, x, text_bytes({0x21}, "FAKE11M")},
        {2000, 0x18DAF910, x, text_bytes({0x22}, "0000001")},
        {3000, 0x18DAF110, x, text_bytes({0x21}, "FAKE11M")},
        {4000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 0, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 3u);
}

TEST(ExportTest, ATornSegmentOnAny18DaIdIsLeftOut)
{
    const uint32_t x = ECUIO_FRAME_FLAG_EXTENDED;
    Folder f;
    recording(f, row(1000, 0x18DAF910, x, text_bytes({0x21}, "FAKE11M")) +
                     row(2000, 0x18DAF910, x, text_bytes({0x22}, "0000001")) +
                     row(3000, 0x7E0, 0, {0x21, 0, 0, 0, 0, 0, 0, 0}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x7E0u);
    EXPECT_EQ(e.bytes.find("FAKE11M"), std::string::npos);
}

/* A CAN FD frame on a reply ID, longer than classic ISO-TP's: its ISO-TP
 * form isn't read, so it's left out; one on another ID is kept */
TEST(ScrubFramesTest, AnFdFrameOnAReplyIdIsLeftOut)
{
    const uint32_t fd = ECUIO_FRAME_FLAG_FD;
    Source s;
    s.frames = {
        {1000, 0x7E8, fd,
         text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000001")},
        {2000, 0x18DAF110, fd | ECUIO_FRAME_FLAG_EXTENDED,
         text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000002")},
        {3000, 0x123, fd, std::vector<uint8_t>(12, 0x55)},
        {4000, 0x7E8, fd, {0x04, 0x41, 0x0C, 0x1A, 0xF8}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 1}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 2u);
}

TEST(ExportTest, AnFdFrameOnAReplyIdIsLeftOut)
{
    Folder f;
    recording(
        f, row(1000, 0x7E8, ECUIO_FRAME_FLAG_FD,
               text_bytes({0x00, 0x13, 0x49, 0x04, 0x01}, "CALIDFAKE0000001")) +
               row(2000, 0x123, ECUIO_FRAME_FLAG_FD,
                   std::vector<uint8_t>(12, 0x55)));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE], 1u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x123u);
    EXPECT_EQ(e.bytes.find("CALIDFAKE"), std::string::npos);
    /* raw keeps it */
    Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.pcap.packets.size(), 2u);
}

/* A hex def's value is searched as its bytes: three zero bytes are too
 * short to search for, so a Flow Control's zero padding doesn't fail the
 * copy; six bytes are searched, in every form */
TEST(FolderExportTest, AHexValueIsSearchedAsItsBytes)
{
    const std::string header = "profile  raw\nsignal  DID D100,text,,0,0\n";
    {
        Folder f;
        recording(f, row(2000, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}),
                  "1000,DID D100,00 00 00,\n", header);
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_OK) << e.detail;
    }
    {
        Folder f;
        recording(f,
                  row(2000, 0x7E0, 0,
                      {0x07, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x00}),
                  "1000,DID D100,de ad be ef 01 02,\n", header);
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
    }
}

/* The VIN read from a module on 0x726/0x72E, an ID ISO 15765-4 doesn't
 * give, so no reassembly takes it: its First Frame says what it is, and
 * the Consecutive Frames on 0x72E go with it until 0x72E's next Single or
 * First Frame */
std::vector<Mem> vin_on_another_id()
{
    const std::vector<uint8_t> p = text_bytes({0x62, 0xF1, 0x90}, kVin);
    return {
        {1000, 0x726, 0, {0x03, 0x22, 0xF1, 0x90, 0, 0, 0, 0}},
        {2000, 0x72E, 0, {0x10, 0x14, p[0], p[1], p[2], p[3], p[4], p[5]}},
        {3000, 0x726, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {4000, 0x72E, 0, {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}},
        {5000,
         0x72E,
         0,
         {0x22, p[13], p[14], p[15], p[16], p[17], p[18], p[19]}},
        /* past its length, before its ID's next SF or FF: still its */
        {6000, 0x72E, 0, {0x23, 0, 0, 0, 0, 0, 0, 0}},
        /* a refusal on 0x72E ends it; a CF after that is no reply's */
        {7000, 0x72E, 0, {0x03, 0x7F, 0x22, 0x31, 0, 0, 0, 0}},
        {8000, 0x72E, 0, {0x21, 1, 2, 3, 4, 5, 6, 7}},
        /* an identification reply in one frame, on a 29-bit ID */
        {9000,
         0x18FEF100,
         ECUIO_FRAME_FLAG_EXTENDED,
         {0x07, 0x49, 0x06, 0x01, 0xDE, 0xAD, 0xBE, 0xEF}},
    };
}

TEST(ScrubFramesTest, AnIdentificationReplyOnAnyIdIsLeftOut)
{
    Source s;
    s.frames = vin_on_another_id();
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{1, 0, 1, 0, 0, 0, 1, 1, 0}));
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 5u);

    /* its bytes gathered, the VIN is searched for in what's kept */
    s.frames.push_back(
        {10000, 0x123, kWhole,
         text_bytes({0x62, 0xD8, 0x01}, std::string(kVin) + "1")});
    Scrubbed found(s);
    EXPECT_EQ(found.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(found.report.failed_class,
              ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
}

TEST(ExportTest, AnIdentificationReplyOnAnyIdIsLeftOut)
{
    Folder f;
    std::string rows;
    for (const Mem& m : vin_on_another_id()) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    recording(f, rows);
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 5u);
    EXPECT_EQ(e.pcap.packets.size(), 4u);
    EXPECT_EQ(e.bytes.find("FAKE11M"), std::string::npos);
    EXPECT_EQ(e.bytes.find("0000001"), std::string::npos);
    FolderExported copy(f);
    ASSERT_EQ(copy.rc, ECUIO_OK) << copy.detail;
    EXPECT_EQ(copy.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 5u);
    EXPECT_EQ(copy.files["frames.csv"].find("0x72E,0x0,10 "),
              std::string::npos);
}

std::vector<Mem> a_short_reply_on_another_id();

/* frames.csv rows of frames in memory */
std::string rows_of(const std::vector<Mem>& frames)
{
    std::string rows;
    for (const Mem& m : frames) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    return rows;
}

/* A kept message repeating the VIN, so a check that knows it fails */
Mem echo_of_the_vin(uint64_t t)
{
    return {t, 0x123, kWhole,
            text_bytes({0x62, 0xD8, 0x01}, std::string(kVin) + "1")};
}

/* Each frame left out, and the VIN then known (kept, an echo of it fails
 * the check) */
void expect_identification(std::vector<Mem> frames, const char* what)
{
    Source s;
    s.frames = frames;
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << what << ": " << r.detail;
    EXPECT_EQ(r.keep, none_kept(frames.size())) << what;
    EXPECT_EQ(r.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION],
              frames.size())
        << what;
    s.frames.push_back(echo_of_the_vin(frames.back().t + 1000));
    Scrubbed found(s);
    EXPECT_EQ(found.rc, ECUIO_ERR_LOAD_FAILED) << what;
    EXPECT_EQ(found.report.failed_class,
              ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1)
        << what;
}

/* A VIN reply in extended addressing (byte 0 the tester's address, the
 * PCI in byte 1) on a manufacturer module's ID */
TEST(ScrubFramesTest, AnExtendedAddressingIdentificationReplyIsLeftOut)
{
    const std::vector<uint8_t> p = text_bytes({0x62, 0xF1, 0x90}, kVin);
    expect_identification(
        {
            {1000, 0x612, 0, {0xF1, 0x10, 0x14, p[0], p[1], p[2], p[3], p[4]}},
            {2000, 0x612, 0, {0xF1, 0x21, p[5], p[6], p[7], p[8], p[9], p[10]}},
            {3000,
             0x612,
             0,
             {0xF1, 0x22, p[11], p[12], p[13], p[14], p[15], p[16]}},
            {4000, 0x612, 0, {0xF1, 0x23, p[17], p[18], p[19], 0, 0, 0}},
        },
        "extended");
    /* the match is on the payload after the PCI, never elsewhere: 49 in
     * the address byte or past the payload's start is no reply */
    Source s;
    s.frames = {
        {1000, 0x612, 0, {0x49, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0}},
        {2000, 0x612, 0, {0xF1, 0x03, 0x01, 0x49, 0x02, 0, 0, 0}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{1, 1}));
}

/* CAN FD's forms on a manufacturer module's ID: a Single Frame's escape
 * (its length in byte 1) and a First Frame's (12 bits of zero, then 32) */
TEST(ScrubFramesTest, CanFdEscapesOnAnotherIdAreRead)
{
    const uint32_t fd = ECUIO_FRAME_FLAG_FD;
    std::vector<uint8_t> single =
        text_bytes({0x00, 0x14, 0x49, 0x02, 0x01}, kVin);
    single.resize(24, 0);
    expect_identification({{1000, 0x6A0, fd, single}}, "Single Frame escape");

    const std::vector<uint8_t> p = text_bytes({0x49, 0x02, 0x01}, kVin);
    std::vector<uint8_t> first = {0x10, 0, 0, 0, 0, 0x14};
    first.insert(first.end(), p.begin(), p.begin() + 10);
    std::vector<uint8_t> next = {0x21};
    next.insert(next.end(), p.begin() + 10, p.end());
    next.resize(12, 0);
    expect_identification({{1000, 0x6A0, fd, first}, {2000, 0x6A0, fd, next}},
                          "First Frame escape");
}

/* ISO 15765-4's IDs are read in the normal layout alone, and a Single
 * Frame takes the escape's form only in a frame longer than 8 bytes and
 * the nibble's only in one of 8 or less: an adapter's payload with no PCI,
 * a CAN FD frame whose byte 1 is 49, or a classic one whose byte 2 is, is
 * no identification reply */
TEST(ScrubFramesTest, OnlyTheFormsTheStandardsGiveAreRead)
{
    const uint32_t ext = ECUIO_FRAME_FLAG_EXTENDED;
    const uint32_t fd = ECUIO_FRAME_FLAG_FD;
    const std::vector<uint8_t> extended_sf = {0xF1, 0x05, 0x49, 0x02,
                                              0x01, 'A',  'B',  0};
    std::vector<uint8_t> nibble_sf = {0x0B, 0x49, 0x02, 0x01, 'A', 'B'};
    nibble_sf.resize(12, 0);
    std::vector<uint8_t> extended_nibble_sf = {0xF1, 0x0A, 0x49, 0x02, 0x01};
    extended_nibble_sf.resize(12, 0);
    std::vector<uint8_t> escape_sf = {0x00, 0x05, 0x49, 0x02, 0x01, 'A', 'B'};
    escape_sf.resize(12, 0);
    const std::vector<uint8_t> classic_escape_sf = {0x00, 0x3C, 0x49, 0x00};
    Source s;
    s.frames = {
        {1000, 0x7E8, 0, extended_sf},
        {2000, 0x7DF, 0, extended_sf},
        {3000, 0x18DAF110, ext, extended_sf},
        {4000, 0x612, 0, extended_sf},
        {5000, 0x6A0, fd, nibble_sf},
        {6000, 0x612, fd, extended_nibble_sf},
        {7000, 0x6A0, fd, escape_sf},
        {8000, 0x3E9, 0, classic_escape_sf},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{1, 1, 1, 0, 1, 1, 0, 1}));
}

/* An identification reply's First Frame whose length can't be right is
 * one all the same: its Consecutive Frames are followed, and their bytes
 * known */
TEST(ScrubFramesTest, AnIdentificationFirstFrameOfABadLengthIsFollowed)
{
    const std::vector<uint8_t> p = text_bytes({0x49, 0x02, 0x01}, kVin);
    for (const uint8_t length : {uint8_t{0x05}, uint8_t{0x00}}) {
        expect_identification(
            {
                {1000,
                 0x72E,
                 0,
                 {0x10, length, p[0], p[1], p[2], p[3], p[4], p[5]}},
                {2000,
                 0x72E,
                 0,
                 {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}},
                {3000,
                 0x72E,
                 0,
                 {0x22, p[13], p[14], p[15], p[16], p[17], p[18], p[19]}},
            },
            length == 0 ? "length zero" : "length short of its frame");
    }
}

/* The follow ends at a frame on its ID that isn't a Consecutive Frame, and
 * after ECUIO_FRAME_DATA_MAX bytes: what comes after is kept */
TEST(ScrubFramesTest, AFollowEndsAtAnotherFrameAndAtItsLimit)
{
    Source s;
    s.frames = {
        {1000, 0x72E, 0, {0x10, 0x14, 0x49, 0x02, 0x01, 'Z', 'Z', 'Z'}},
        {2000, 0x72E, 0, {0x21, 'F', 'A', 'K', 'E', '1', '1', 'M'}},
        {3000, 0x72E, 0, {0x45, 0x01, 0x02, 0x03}},
        {4000, 0x72E, 0, {0x22, 0, 0, 0, 0, 0, 0, 0}},
    };
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 0, 1, 1}));

    /* a bad length followed: its First Frame's 6 bytes, then 7 a frame */
    const size_t followed = (ECUIO_FRAME_DATA_MAX - 6 + 6) / 7;
    const size_t past = 20;
    s.frames = {
        {1000, 0x72E, 0, {0x10, 0x05, 0x49, 0x02, 0x01, 'Z', 'Z', 'Z'}}};
    for (size_t i = 0; i < followed + past; ++i) {
        s.frames.push_back({2000 + i,
                            0x72E,
                            0,
                            {static_cast<uint8_t>(0x20 | ((i + 1) & 0x0F)), 0,
                             0, 0, 0, 0, 0, 0}});
    }
    Scrubbed capped(s);
    ASSERT_EQ(capped.rc, ECUIO_OK) << capped.detail;
    std::vector<uint8_t> keep(1 + followed, 0);
    keep.resize(1 + followed + past, 1);
    EXPECT_EQ(capped.keep, keep);
}

/* An identification reply on another ID that a new Single Frame there
 * ends before it's whole: its bytes so far are identity values all the
 * same */
TEST(ScrubFramesTest, AReplyEndedBeforeItsWholeIsSearchedFor)
{
    Source s;
    s.frames = a_short_reply_on_another_id();
    s.frames.insert(s.frames.begin() + 2,
                    {2500, 0x72E, 0, {0x03, 0x7F, 0x22, 0x31, 0, 0, 0, 0}});
    Scrubbed r(s);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
}

/* A manufacturer's DID echoing the VIN, segmented on its own ID in either
 * layout, no frame of it holding the VIN whole: checked as one message,
 * whole or cut short by the recording's end, it fails every export */
std::vector<std::vector<Mem>> the_vin_echoed_in_segments()
{
    const std::vector<uint8_t> v = text_bytes({0x49, 0x02, 0x01}, kVin);
    const std::vector<Mem> known = {
        {1000, 0x7DF, 0, {0x02, 0x09, 0x02, 0, 0, 0, 0, 0}},
        {1100, 0x7E8, 0, {0x10, 0x14, v[0], v[1], v[2], v[3], v[4], v[5]}},
        {1200, 0x7E0, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {1300, 0x7E8, 0, {0x21, v[6], v[7], v[8], v[9], v[10], v[11], v[12]}},
        {1400,
         0x7E8,
         0,
         {0x22, v[13], v[14], v[15], v[16], v[17], v[18], v[19]}},
    };
    const std::vector<uint8_t> p = text_bytes({0x62, 0xD1, 0x00}, kVin);
    std::vector<Mem> normal = known;
    normal.push_back(
        {2000, 0x6F1, 0, {0x10, 0x14, p[0], p[1], p[2], p[3], p[4], p[5]}});
    normal.push_back(
        {2100, 0x6F1, 0, {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}});
    normal.push_back({2200,
                      0x6F1,
                      0,
                      {0x22, p[13], p[14], p[15], p[16], p[17], p[18], p[19]}});
    std::vector<Mem> extended = known;
    extended.push_back(
        {2000, 0x612, 0, {0xF1, 0x10, 0x14, p[0], p[1], p[2], p[3], p[4]}});
    extended.push_back(
        {2100, 0x612, 0, {0xF1, 0x21, p[5], p[6], p[7], p[8], p[9], p[10]}});
    extended.push_back(
        {2200,
         0x612,
         0,
         {0xF1, 0x22, p[11], p[12], p[13], p[14], p[15], p[16]}});
    extended.push_back(
        {2300, 0x612, 0, {0xF1, 0x23, p[17], p[18], p[19], 0, 0, 0}});
    /* one the recording ends before its length: checked with what it holds */
    std::vector<Mem> cut = normal;
    cut[cut.size() - 3].data[1] = 0x30;
    return {normal, extended, cut};
}

TEST(ScrubFramesTest, TheVinEchoedInSegmentsOnAnyIdIsFound)
{
    for (const std::vector<Mem>& frames : the_vin_echoed_in_segments()) {
        Source s;
        s.frames = frames;
        Scrubbed r(s);
        EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED) << frames.back().id;
        EXPECT_EQ(r.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1)
            << frames.back().id;
        EXPECT_EQ(r.keep, none_kept(frames.size()));
    }
}

TEST(ExportTest, TheVinEchoedInSegmentsOnAnyIdIsFound)
{
    for (const std::vector<Mem>& frames : the_vin_echoed_in_segments()) {
        Folder f;
        recording(f, rows_of(frames));
        Exported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED) << frames.back().id;
        EXPECT_EQ(e.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1)
            << frames.back().id;
        EXPECT_FALSE(fs::exists(f.path / "out.pcapng"));
        FolderExported copy(f);
        EXPECT_EQ(copy.rc, ECUIO_ERR_LOAD_FAILED) << frames.back().id;
        EXPECT_EQ(copy.report.failed_class,
                  ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1)
            << frames.back().id;
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
        EXPECT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    }
}

/* The rename into place never replaces a name taken, by a file or a
 * folder, in each of its ways (POSIX: the system's, a link or a
 * reservation, the check before rename) */
TEST(ExportTest, TheRenameIntoPlaceNeverReplacesATakenName)
{
#ifdef _WIN32
    const int ways = 1;
#else
    const int ways = 3;
#endif
    for (int way = 0; way < ways; ++way) {
#ifndef _WIN32
        ecuio_files::rename_test_from = way;
#endif
        Folder f;
        f.write("from", "the export's");
        f.write("taken", "another's");
        fs::create_directories(f.path / "from-folder" / "inner");
        fs::create_directories(f.path / "empty-folder");
        fs::create_directories(f.path / "full-folder" / "theirs");
        std::string why;
        EXPECT_FALSE(ecuio_files::rename_to(f.utf8("from"), f.utf8("taken"),
                                            false, &why))
            << way;
        EXPECT_FALSE(why.empty()) << way;
        EXPECT_EQ(read_file(f.path / "taken"), "another's") << way;
        EXPECT_EQ(read_file(f.path / "from"), "the export's") << way;
        for (const char* to : {"empty-folder", "full-folder", "taken"}) {
            EXPECT_FALSE(ecuio_files::rename_to(f.utf8("from-folder"),
                                                f.utf8(to), false, &why))
                << way << " " << to;
            EXPECT_TRUE(fs::is_directory(f.path / "from-folder" / "inner"))
                << way << " " << to;
        }
        EXPECT_TRUE(fs::is_empty(f.path / "empty-folder")) << way;
        EXPECT_TRUE(fs::is_directory(f.path / "full-folder" / "theirs")) << way;
        EXPECT_EQ(read_file(f.path / "taken"), "another's") << way;

        EXPECT_TRUE(
            ecuio_files::rename_to(f.utf8("from"), f.utf8("free"), false, &why))
            << way << ": " << why;
        EXPECT_EQ(read_file(f.path / "free"), "the export's") << way;
        EXPECT_FALSE(fs::exists(f.path / "from")) << way;
        EXPECT_TRUE(ecuio_files::rename_to(f.utf8("from-folder"),
                                           f.utf8("free-folder"), false, &why))
            << way << ": " << why;
        EXPECT_TRUE(fs::is_directory(f.path / "free-folder" / "inner")) << way;
        EXPECT_FALSE(fs::exists(f.path / "from-folder")) << way;
    }
#ifndef _WIN32
    ecuio_files::rename_test_from = 0;
#endif
}

/* A recording's signals.json that doesn't load refuses a scrubbed export
 * of every format, in words, leaving nothing; a raw one doesn't read it */
TEST(ExportTest, ASignalsJsonThatDoesntLoadRefusesAScrubbedExport)
{
    const std::string why = "the recording's signals.json doesn't load (";
    const std::string end =
        "): a scrubbed export can't tell which signals are text";
    auto refused = [&](const std::string& detail) {
        return detail.rfind(why, 0) == 0 &&
               detail.size() > why.size() + end.size() &&
               detail.compare(detail.size() - end.size(), end.size(), end) ==
                   0 &&
               detail.find("line 1, column") != std::string::npos;
    };
    Folder f;
    whole_recording(f);
    f.write("rec/signals.json", "{\"havne_signals\": 1,\n");
    {
        Exported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_TRUE(refused(e.detail)) << e.detail;
        EXPECT_FALSE(fs::exists(f.path / "out.pcapng"));
        EXPECT_FALSE(partial_left(f.path, "out.pcapng"));
    }
    {
        CsvExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_TRUE(refused(e.detail)) << e.detail;
    }
    {
        FolderExported e(f);
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_TRUE(refused(e.detail)) << e.detail;
        EXPECT_FALSE(fs::exists(f.path / "copy"));
        EXPECT_FALSE(partial_left(f.path, "copy"));
    }
    Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
    EXPECT_EQ(raw.rc, ECUIO_OK) << raw.detail;

    /* one there that can't be opened (a folder of the name) */
    Folder g;
    whole_recording(g);
    fs::create_directories(g.path / "rec" / "signals.json");
    Exported e(g);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail.rfind(why, 0), 0u) << e.detail;
}

/* An identification reply on another ID that the frames end before it's
 * whole: its bytes so far are identity values all the same, so a message
 * repeating them fails the check */
std::vector<Mem> a_short_reply_on_another_id()
{
    const std::vector<uint8_t> p = text_bytes({0x62, 0xF1, 0x90}, kVin);
    return {
        {1000, 0x72E, 0, {0x10, 0x14, p[0], p[1], p[2], p[3], p[4], p[5]}},
        {2000, 0x72E, 0, {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}},
        {3000, 0x123, kWhole, text_bytes({0x62, 0xD8, 0x01}, "-ZZZFAKE11M-")},
    };
}

TEST(ScrubFramesTest, AShortIdentificationReplyOnAnotherIdIsSearchedFor)
{
    Source s;
    s.frames = a_short_reply_on_another_id();
    Scrubbed r(s);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
}

TEST(ExportTest, AShortIdentificationReplyOnAnotherIdIsSearchedFor)
{
    Folder f;
    std::string rows;
    for (const Mem& m : a_short_reply_on_another_id()) {
        rows += row(m.t, m.id, m.flags, m.data);
    }
    recording(f, rows);
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_FALSE(fs::exists(f.path / "out.pcapng"));
}

/* The folder's entries by name, each a file's bytes or "<folder>" */
std::map<std::string, std::string> entries(const fs::path& dir)
{
    std::map<std::string, std::string> m;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        m[e.path().filename().generic_u8string()] =
            e.is_directory() ? std::string("<folder>") : read_file(e.path());
    }
    return m;
}

/* An export writes under a name of its own: another's partial (a crashed
 * export's, or one under way: the next names this process would take
 * among them) is never in the way and never touched, and the export
 * leaves none of its own */
TEST(ExportTest, AnotherExportsPartialIsNeverInTheWayNorTouched)
{
    Folder f;
    recording(f, row(1000, 0x7E0, 0, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}));
    f.write("rec/signals.csv", "host_us,signal,value,unit\n1000,RPM,800,rpm\n");
    const struct {
        uint32_t format;
        const char* name;
    } cases[] = {{ECUIO_EXPORT_PCAPNG, "out.pcapng"},
                 {ECUIO_EXPORT_CSV, "out.csv"},
                 {ECUIO_EXPORT_FOLDER, "copy"}};
    for (const auto& c : cases) {
        const std::string out = f.utf8(c.name);
        f.write(std::string(c.name) + ".partial", "a crashed export's");
        f.write(std::string(c.name) + ".partial-1-0", "a crashed export's");
        /* the next two names this process takes, made by another */
        const std::string taken = ecuio_files::partial_name(out);
        const std::string stem = taken.substr(0, taken.rfind('-') + 1);
        const unsigned n = static_cast<unsigned>(
            std::stoul(taken.substr(taken.rfind('-') + 1)));
        for (unsigned k = n + 1; k <= n + 2; ++k) {
            std::ofstream(fs::u8path(stem + std::to_string(k)),
                          std::ios::binary)
                << "an export under way";
        }
        auto before = entries(f.path);
        const std::string folder = f.utf8("rec");
        ecuio_export_options_t o{};
        o.format = c.format;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        ASSERT_EQ(ecuio_export(&o, nullptr), ECUIO_OK)
            << c.name << ": " << ecuio_last_error_detail();
        auto after = entries(f.path);
        ASSERT_EQ(after.count(c.name), 1u) << c.name;
        after.erase(c.name);
        before.erase("rec");
        after.erase("rec");
        EXPECT_EQ(after, before) << c.name
                                 << ": another's left as it was, "
                                    "none of its own left";
    }
}

/* Out of memory at each of an export's allocations in turn: every one
 * returns (never terminates), and none leaves its file or a .partial */
TEST(ExportTest, OutOfMemoryAnywhereLeavesNoFileBehind)
{
    Folder f;
    whole_recording(f);
    const uint32_t formats[] = {ECUIO_EXPORT_PCAPNG, ECUIO_EXPORT_CSV,
                                ECUIO_EXPORT_FOLDER};
    for (uint32_t format : formats) {
        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8("oom");
        ecuio_export_options_t o{};
        o.format = format;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        const unsigned long before = news_on_this_thread();
        ASSERT_EQ(ecuio_export(&o, nullptr), ECUIO_OK) << format;
        const unsigned long news = news_on_this_thread() - before;
        std::error_code ec;
        fs::remove_all(f.path / "oom", ec);
        ASSERT_GT(news, 10u);
        unsigned long refused = 0;
        const unsigned long handles = open_handles();
        for (unsigned long n = 1; n <= news; ++n) {
            fail_new_on_this_thread(n);
            const ecuio_status_t rc = ecuio_export(&o, nullptr);
            fail_new_on_this_thread(0);
            if (rc != ECUIO_OK) {
                ++refused;
                ASSERT_TRUE(rc == ECUIO_ERR_OUT_OF_MEMORY ||
                            rc == ECUIO_ERR_LOAD_FAILED)
                    << format << " at " << n << ": " << rc;
                ASSERT_FALSE(fs::exists(f.path / "oom")) << format << " " << n;
            }
            ASSERT_FALSE(partial_left(f.path, "oom")) << format << " at " << n;
            fs::remove_all(f.path / "oom", ec);
        }
        EXPECT_GT(refused, 0u) << format;
        EXPECT_EQ(open_handles(), handles)
            << format << ": a handle left open (a folder's listing)";
    }
}

} // namespace
