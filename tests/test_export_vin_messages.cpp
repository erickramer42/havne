// File/layer: tests/test_export_vin_messages.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ExportTest, FolderExportTest, ScrubFramesTest (test_core)
//
// A message the VIN window rule takes (17 VIN characters inside a longer
// run, by the check digit) is left out by the plan, every frame of it, and
// counted, by the pcapng, the folder copy's frames.csv and
// ecuio_scrub_frames alike: delivered whole, segmented on a diagnostic
// reply ID, segmented on a manufacturer's own ID (any_id_messages' joins),
// a CAN FD frame. One holding the recording's VIN still fails the export;
// a range leaves out only what it holds; raw keeps it. The VINs are made up.

#include "test_export_support.h"

namespace {

const uint32_t kVinClass = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED;

/* A manufacturer DID's text (D801, outside the identification block)
 * holding a VIN inside a longer run */
std::vector<uint8_t> did_text()
{
    return text_bytes({0x62, 0xD8, 0x01}, std::string("a") + kVin);
}

/* The DID's reply segmented on reply ID rx (request ID tx), then a frame
 * of its own on 0x123: rows 0-4 */
std::vector<Mem> segmented(uint32_t rx, uint32_t tx)
{
    const std::vector<uint8_t> p = did_text(); /* 21 bytes */
    return {
        {1000, rx, 0, {0x10, 0x15, p[0], p[1], p[2], p[3], p[4], p[5]}},
        {1100, tx, 0, {0x30, 0, 0, 0, 0, 0, 0, 0}},
        {1200, rx, 0, {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}},
        {1300, rx, 0, {0x22, p[13], p[14], p[15], p[16], p[17], p[18], p[19]}},
        {1400, rx, 0, {0x23, p[20], 0, 0, 0, 0, 0, 0}},
        {2000, 0x123, 0, {0x01, 0x02}},
    };
}

void expect_no_vin(const std::string& bytes)
{
    EXPECT_EQ(bytes.find("FAKE11M"), std::string::npos);
    EXPECT_EQ(bytes.find("0000001"), std::string::npos);
}

TEST(ExportTest, AMessageDeliveredWholeTheWindowRuleTakesIsLeftOut)
{
    Folder f;
    recording(f, row(1000, 0x7E8, kWhole, did_text()) +
                     row(2000, 0x123, 0, {0x01, 0x02}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[kVinClass], 1u);
    EXPECT_EQ(e.report.frames, 1u);
    EXPECT_EQ(e.report.made_up, 0u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    EXPECT_EQ(e.pcap.packets[0].can_id(), 0x123u);
    EXPECT_NE(e.pcap.comment.find(", 1 frames of messages holding a "
                                  "VIN-shaped token"),
              std::string::npos)
        << e.pcap.comment;
    expect_no_vin(e.bytes);

    FolderExported copy(f);
    ASSERT_EQ(copy.rc, ECUIO_OK) << copy.detail;
    EXPECT_EQ(copy.report.left_out[kVinClass], 1u);
    EXPECT_EQ(copy.files["frames.csv"], "host_us,timestamp_us,id,flags,data\n" +
                                            row(2000, 0x123, 0, {0x01, 0x02}));
    EXPECT_NE(copy.files["events.log"].find(
                  ", 1 of messages holding a VIN-shaped token)"),
              std::string::npos);
    EXPECT_NE(copy.files["masks.txt"].find(
                  "0 VIN-shaped tokens masked, 1 frames of messages holding "
                  "one left out"),
              std::string::npos)
        << copy.files["masks.txt"];

    Exported raw(f, ECUIO_EXPORT_RAW, 0, 0, "raw.pcapng");
    ASSERT_EQ(raw.rc, ECUIO_OK) << raw.detail;
    EXPECT_EQ(raw.report.left_out[kVinClass], 0u);
    EXPECT_GT(raw.report.made_up, 0u); /* kept, re-segmented */
}

TEST(ExportTest, ASegmentedMessageTheWindowRuleTakesIsLeftOutWhole)
{
    for (const uint32_t rx : {0x7E8u, 0x7A8u}) {
        SCOPED_TRACE(rx); /* a reply ID, and a manufacturer's own */
        Folder f;
        recording(f, rows_of(segmented(rx, rx - 8)));
        Exported e(f);
        ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
        EXPECT_EQ(e.report.left_out[kVinClass], 4u);
        EXPECT_EQ(e.report.frames, 2u);
        ASSERT_EQ(e.pcap.packets.size(), 2u);
        EXPECT_EQ(e.pcap.packets[0].can_id(), rx - 8); /* the flow control */
        EXPECT_EQ(e.pcap.packets[1].can_id(), 0x123u);
        expect_no_vin(e.bytes);

        FolderExported copy(f);
        ASSERT_EQ(copy.rc, ECUIO_OK) << copy.detail;
        EXPECT_EQ(copy.report.left_out[kVinClass], 4u);
        EXPECT_EQ(copy.report.frames, 2u);
        const std::vector<Mem> m = segmented(rx, rx - 8);
        EXPECT_EQ(copy.files["frames.csv"],
                  "host_us,timestamp_us,id,flags,data\n" +
                      rows_of({m[1], m[5]}));
    }
}

TEST(ScrubFramesTest, AMessageTheWindowRuleTakesIsLeftOut)
{
    for (const uint32_t rx : {0x7E8u, 0x7A8u}) {
        SCOPED_TRACE(rx);
        Source s;
        s.frames = segmented(rx, rx - 8);
        s.frames.push_back({3000, 0x7E8, kWhole, did_text()});
        Scrubbed r(s);
        ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
        EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 1, 0, 0, 0, 1, 0}));
        EXPECT_EQ(r.report.left_out[kVinClass], 5u);
        EXPECT_EQ(r.report.frames, 2u);
    }
}

/* A CAN FD frame's payload is a message of its own */
TEST(ExportTest, ACanFdFrameTheWindowRuleTakesIsLeftOut)
{
    const std::string text = std::string("a") + kVin + "bb";
    const std::vector<uint8_t> payload(text.begin(), text.end()); /* 20 */
    Folder f;
    recording(f, row(1000, 0x456, ECUIO_FRAME_FLAG_FD, payload) +
                     row(2000, 0x123, 0, {0x01}));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[kVinClass], 1u);
    ASSERT_EQ(e.pcap.packets.size(), 1u);
    expect_no_vin(e.bytes);

    Source s;
    s.frames = {{1000, 0x456, ECUIO_FRAME_FLAG_FD, payload},
                {2000, 0x123, 0, {0x01}}};
    Scrubbed r(s);
    ASSERT_EQ(r.rc, ECUIO_OK) << r.detail;
    EXPECT_EQ(r.keep, (std::vector<uint8_t>{0, 1}));
}

/* Left out outside the range too, but counted only inside it */
TEST(ExportTest, AMessageTheWindowRuleTakesOutsideTheRangeIsNotCounted)
{
    Folder f;
    recording(f, rows_of(segmented(0x7A8, 0x7A0)));
    Exported e(f, 0, 1800, 0);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[kVinClass], 0u);
    EXPECT_EQ(e.report.frames, 1u);
    Exported part(f, 0, 0, 1250, "part.pcapng");
    ASSERT_EQ(part.rc, ECUIO_OK) << part.detail;
    EXPECT_EQ(part.report.left_out[kVinClass], 2u); /* the FF and a CF */
    EXPECT_EQ(part.report.frames, 1u);
    expect_no_vin(part.bytes);
}

/* A message the rule takes holding the recording's VIN fails the export,
 * as the second check would: its identity value, not a mask's count */
TEST(ExportTest, AMessageTheWindowRuleTakesHoldingTheVinFails)
{
    const std::vector<Mem> frames = {
        {500, 0x7E8, kWhole, text_bytes({0x49, 0x02, 0x01}, kVin)},
        {1000, 0x7E8, kWhole, did_text()}};
    Folder f;
    recording(f, rows_of(frames));
    Exported e(f);
    EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(e.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(e.detail, "the check found an identification reply's value in "
                        "a message the export would write: nothing was "
                        "written");
    expect_no_file(f);

    Source s;
    s.frames = frames;
    Scrubbed r(s);
    EXPECT_EQ(r.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(r.report.failed_class, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1);
    EXPECT_EQ(r.keep, none_kept(2));
}

/* A join's frames another class left out are counted there alone: an
 * identification reply on a manufacturer's ID whose text holds a window */
TEST(ExportTest, AJoinsFramesAlreadyLeftOutAreCountedOnce)
{
    const std::vector<uint8_t> p =
        text_bytes({0x62, 0xF1, 0x8C}, std::string("a") + kVin);
    const std::vector<Mem> frames = {
        {1000, 0x7A8, 0, {0x10, 0x15, p[0], p[1], p[2], p[3], p[4], p[5]}},
        {1200, 0x7A8, 0, {0x21, p[6], p[7], p[8], p[9], p[10], p[11], p[12]}},
        {1300, 0x7A8, 0,
         {0x22, p[13], p[14], p[15], p[16], p[17], p[18], p[19]}},
        {1400, 0x7A8, 0, {0x23, p[20], 0, 0, 0, 0, 0, 0}},
        {2000, 0x123, 0, {0x01}}};
    Folder f;
    recording(f, rows_of(frames));
    Exported e(f);
    ASSERT_EQ(e.rc, ECUIO_OK) << e.detail;
    EXPECT_EQ(e.report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], 4u);
    EXPECT_EQ(e.report.left_out[kVinClass], 0u);
    EXPECT_EQ(e.report.frames, 1u);
    FolderExported copy(f);
    ASSERT_EQ(copy.rc, ECUIO_OK) << copy.detail;
    EXPECT_EQ(copy.report.frames, 1u);
}

} // namespace
