// File/layer: tests/test_export.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ScrubTest, ExportTest, ScrubFramesTest (test_core)
//
// scrub.h's classifier and check, and ecuio_export's pcapng: each export
// read back by a pcapng parser of the test's own, its frames' bytes and
// times, the re-segmented frames reassembled by core's ISO-TP to the
// stored message, what a scrubbed export leaves out, and both checks
// failing an export that would carry an identity value; ecuio_scrub_frames
// over frames in memory by the same rules. Each export case writes into a
// folder of its own under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <cstdint>
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
#include "ecuio/signals.h"
#include "isotp.h"
#include "scrub.h"
#include "temp_name.h"

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
    EXPECT_FALSE(vin(std::string(kVin) + "2")); /* 18 long */
    EXPECT_FALSE(vin(std::string("A") + kVin)); /* 18 long */
    EXPECT_FALSE(vin("ZZZFAKE11M000000"));      /* 16 */
    EXPECT_FALSE(vin("ZZZFAKEI1M0000001"));     /* an I */
    EXPECT_FALSE(vin("ZZZFAKEO1M0000001"));     /* an O */
    EXPECT_FALSE(vin("ZZZFAKEQ1M0000001"));     /* a Q */
    EXPECT_FALSE(vin("zzzfake11m0000001"));     /* lower case */
    EXPECT_FALSE(vin("0123456789ABCDEF0"));     /* hex */
    EXPECT_FALSE(vin("ZZZZZZZZZZZZZZZZZ"));     /* no digit */
    EXPECT_FALSE(vin("12345678901234567"));     /* no letter */
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
    EXPECT_FALSE(fs::exists(f.path / (name + ".partial")));
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

TEST(ExportTest, AFileThatExistsOrARowThatDoesntReadWritesNothing)
{
    Folder f;
    recording(f, row(0, 0x100, 0, {1}));
    f.write("out.pcapng", "mine");
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.detail, "the file exists already");
    EXPECT_EQ(read_file(f.path / "out.pcapng"), "mine");
    EXPECT_FALSE(fs::exists(f.path / "out.pcapng.partial"));

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
    a.format = ECUIO_EXPORT_FOLDER;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NOT_IMPLEMENTED);
    a.format = ECUIO_EXPORT_CSV;
    EXPECT_EQ(ecuio_export(&a, nullptr), ECUIO_ERR_NOT_IMPLEMENTED);
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

} // namespace
