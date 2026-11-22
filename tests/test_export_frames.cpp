// File/layer: tests/test_export_frames.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ScrubFramesTest, ExportTest, ExportCancelTest (test_core)

#include "test_export_support.h"

namespace {

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

/* A signals.json past the loader's bound is refused whole, never loaded
 * from its first MiB: a file that loads, padded to the bound, decodes;
 * one byte more refuses a scrubbed export in words naming the bound */
TEST(ExportTest, ASignalsJsonPastTheLoadersBoundRefusesAScrubbedExport)
{
    const std::string json =
        R"({"havne_signals": 1, "name": "Bound test", )"
        R"("source": {"name": "test_export"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
        R"("signals": [{"key": "ecu_name", "name": "ECU name", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "D802"}, )"
        R"("field": {"byte": 0}, "kind": "text"}]})";
    for (const size_t size :
         {static_cast<size_t>(ECUIO_SIGNAL_FILE_MAX_BYTES) + 1,
          static_cast<size_t>(ECUIO_SIGNAL_SET_MAX_BYTES),
          ECUIO_SIGNAL_SET_MAX_BYTES + size_t{1}}) {
        Folder f;
        whole_recording(f);
        f.write("rec/signals.json",
                json + std::string(size - json.size(), ' '));
        FolderExported e(f);
        if (size <= ECUIO_SIGNAL_SET_MAX_BYTES) {
            EXPECT_EQ(e.rc, ECUIO_OK) << e.detail;
            EXPECT_EQ(e.files["signals.json"].size(), size);
            continue;
        }
        EXPECT_EQ(e.rc, ECUIO_ERR_LOAD_FAILED);
        EXPECT_EQ(e.detail,
                  "the recording's signals.json doesn't load (it is past a "
                  "recording's limit of 4194304 bytes for it): a scrubbed "
                  "export can't tell which signals are text");
        EXPECT_FALSE(fs::exists(f.path / "copy"));
    }
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

/* What an export wrote at p: a file's bytes under "", or each file of a
 * folder by name */
std::map<std::string, std::string> written_at(const fs::path& p)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        for (const auto& e : fs::directory_iterator(p, ec)) {
            out[e.path().filename().generic_u8string()] = read_file(e.path());
        }
    } else if (fs::exists(p, ec)) {
        out[""] = read_file(p);
    }
    return out;
}

/* A cancel that never stops is asked as each pass and file begins and
 * changes nothing: the same bytes as ecuio_export's, and a NULL cancel
 * is ecuio_export */
TEST(ExportCancelTest, ACancelThatNeverStopsWritesTheExportsBytes)
{
    Folder f;
    whole_recording(f);
    for (const Kind& k : kKinds) {
        SCOPED_TRACE(k.name);
        const std::string plain = std::string(k.name) + "-plain";
        const std::string asked = std::string(k.name) + "-asked";
        const std::string none = std::string(k.name) + "-null";
        ASSERT_EQ(export_as(f, k, plain, nullptr), ECUIO_OK);
        Asks asks;
        ASSERT_EQ(export_as(f, k, asked, &asks), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(asks.count, k.asks);
        const auto bytes = written_at(f.path / plain);
        EXPECT_FALSE(bytes.empty());
        EXPECT_EQ(written_at(f.path / asked), bytes);

        const std::string folder = f.utf8("rec");
        const std::string out = f.utf8(none);
        ecuio_export_options_t o{};
        o.format = k.format;
        o.layout = k.layout;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = out.c_str();
        ASSERT_EQ(ecuio_export_with(&o, nullptr, nullptr, nullptr), ECUIO_OK);
        EXPECT_EQ(written_at(f.path / none), bytes);
    }
    ecuio_export_estimate_t plain[2] = {};
    ecuio_export_estimate_t asked[2] = {};
    Asks asks;
    ASSERT_EQ(estimate_of(f, nullptr, plain), ECUIO_OK);
    ASSERT_EQ(estimate_of(f, &asks, asked), ECUIO_OK);
    EXPECT_EQ(asks.count, 2u) << "the survey's pass and the walk's";
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(asked[i].rows, plain[i].rows);
        EXPECT_EQ(asked[i].bytes, plain[i].bytes);
        EXPECT_EQ(asked[i].columns, plain[i].columns);
    }
}

} // namespace
