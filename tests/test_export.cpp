// File/layer: tests/test_export.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ScrubTest, ExportTest, CsvExportTest (test_core)
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

#include "test_export_support.h"

namespace {

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

/* 2023-11-14T22:13:20.250Z */
constexpr uint64_t kUtc = 1700000000250000u;

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
                  "reassembled, 0 frames of messages holding a VIN-shaped "
                  "token");
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
    EXPECT_EQ(unbommed(e.bytes),
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
    EXPECT_EQ(unbommed(mid.bytes),
              csv_head("a scrubbed export",
                       "2 values of 2 signals, 0.002000 s to 0.002500 s", 0,
                       0) +
                  "0.002000,Coolant,C,-12.5\n"
                  "0.002500,\"Trim, bank 1\",%,3.1\n");
    CsvExported none(f, 0, 9000, 0, "none.csv");
    ASSERT_EQ(none.rc, ECUIO_OK) << none.detail;
    EXPECT_EQ(unbommed(none.bytes),
              csv_head("a scrubbed export", "0 values of 0 signals", 0, 0));
}

} // namespace
