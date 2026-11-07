// File/layer: tests/test_candump_import.cpp | test_core, Qt-free, C++17
// Testing: UNIT: CandumpTest, ImportTest (test_core)
//
// candump.h's line parser, and ecuio_import's candump log in as a
// recording: its frames.csv and events.log, the report, each refusal, and
// the folder replay and the reader take. Each import case writes into a
// folder of its own under TEMP/TMPDIR, removed at the end.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "candump.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "partial_left.h"
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;

namespace {

// ---- the parser ----

// The parsed line points into its text (the interface's name), so the
// text is a copy of its own, declared before the line, and never moved
struct Parsed {
    std::string text;
    bool ok = false;
    ecuio_candump::line l;
    std::string why;
    explicit Parsed(const std::string& t) : text(t)
    {
        const char* w = nullptr;
        ok = ecuio_candump::parse_line(text.data(), text.size(), l, &w);
        why = w != nullptr ? w : "";
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    std::string iface() const
    {
        // the name lies within the text this Parsed owns
        EXPECT_TRUE(l.iface >= text.data() &&
                    l.iface + l.iface_len <= text.data() + text.size());
        return std::string(l.iface, l.iface_len);
    }
    std::vector<uint8_t> data() const
    {
        return std::vector<uint8_t>(l.data, l.data + l.length);
    }
};

std::string refused(const std::string& text)
{
    Parsed p(text);
    EXPECT_FALSE(p.ok) << text;
    EXPECT_FALSE(p.why.empty()) << text;
    return p.why;
}

TEST(CandumpTest, AClassicFrameReadsItsTimeInterfaceIdAndBytes)
{
    Parsed p("(1700000000.250001) can0 7E8#04410C1AF8");
    ASSERT_TRUE(p.ok) << p.why;
    EXPECT_EQ(p.l.seconds, 1700000000u);
    EXPECT_EQ(p.l.microseconds, 250001u);
    EXPECT_EQ(p.iface(), "can0");
    EXPECT_EQ(p.l.id, 0x7E8u);
    EXPECT_EQ(p.l.flags, 0u);
    EXPECT_EQ(p.data(), (std::vector<uint8_t>{0x04, 0x41, 0x0C, 0x1A, 0xF8}));
}

TEST(CandumpTest, EachFormOfAFrameReadsAsCansendWritesIt)
{
    Parsed ext("(1.000000) vcan0 18DAF110#0262F190");
    ASSERT_TRUE(ext.ok) << ext.why;
    EXPECT_EQ(ext.l.id, 0x18DAF110u);
    EXPECT_EQ(ext.l.flags, ECUIO_FRAME_FLAG_EXTENDED);

    Parsed none("(1.000000) can0 123#");
    ASSERT_TRUE(none.ok) << none.why;
    EXPECT_EQ(none.l.length, 0u);

    Parsed dots("(1.000000) can0 123#11.2233.44");
    ASSERT_TRUE(dots.ok) << dots.why;
    EXPECT_EQ(dots.data(), (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}));

    Parsed lower("(1.000000) can0 7e8#0a0B");
    ASSERT_TRUE(lower.ok) << lower.why;
    EXPECT_EQ(lower.l.id, 0x7E8u);
    EXPECT_EQ(lower.data(), (std::vector<uint8_t>{0x0A, 0x0B}));

    Parsed dlc("(1.000000) can0 123#1122334455667788_C");
    ASSERT_TRUE(dlc.ok) << dlc.why;
    EXPECT_EQ(dlc.l.length, 8u);

    Parsed rtr("(1.000000) can0 123#R");
    ASSERT_TRUE(rtr.ok) << rtr.why;
    EXPECT_EQ(rtr.l.flags, ECUIO_FRAME_FLAG_REMOTE);
    EXPECT_EQ(rtr.l.length, 0u);
    Parsed rtr3("(1.000000) can0 123#R3");
    ASSERT_TRUE(rtr3.ok) << rtr3.why;
    EXPECT_EQ(rtr3.l.length, 3u);
    Parsed rtr8("(1.000000) can0 123#R8_9");
    ASSERT_TRUE(rtr8.ok) << rtr8.why;
    EXPECT_EQ(rtr8.l.length, 8u);
}

TEST(CandumpTest, AnFdFramesFlagsDigitSetsBitrateSwitchAndErrorState)
{
    Parsed fd("(1.000000) can0 123##3AABB");
    ASSERT_TRUE(fd.ok) << fd.why;
    EXPECT_EQ(fd.l.fd_flags, 3u);
    EXPECT_EQ(fd.l.flags, ECUIO_FRAME_FLAG_FD |
                              ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                              ECUIO_FRAME_FLAG_FD_ESI);
    EXPECT_EQ(fd.data(), (std::vector<uint8_t>{0xAA, 0xBB}));

    Parsed esi("(1.000000) can0 123##2");
    ASSERT_TRUE(esi.ok) << esi.why;
    EXPECT_EQ(esi.l.flags, ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_FD_ESI);

    Parsed plain("(1.000000) can0 123##0");
    ASSERT_TRUE(plain.ok) << plain.why;
    EXPECT_EQ(plain.l.flags, ECUIO_FRAME_FLAG_FD);

    const std::string max = "(1.000000) can0 123##1" + std::string(128, 'E');
    Parsed full(max);
    ASSERT_TRUE(full.ok) << full.why;
    EXPECT_EQ(full.l.length, 64u);
    EXPECT_EQ(refused(max + "EE"), "an FD frame holds at most 64 data bytes");
}

TEST(CandumpTest, AnErrorFrameKeepsItsClassBitsAsItsId)
{
    Parsed e("(1.000000) can0 20000004#0000000000000000");
    ASSERT_TRUE(e.ok) << e.why;
    EXPECT_EQ(e.l.id, 0x4u);
    EXPECT_EQ(e.l.flags, ECUIO_FRAME_FLAG_ERROR);
    EXPECT_EQ(e.l.length, 8u);
    EXPECT_EQ(refused("(1.000000) can0 60000004#00"),
              "an error frame's ID holds no other flag");
    EXPECT_EQ(refused("(1.000000) can0 20000004#R"),
              "an error frame isn't a remote frame");
    EXPECT_EQ(refused("(1.000000) can0 20000004##000"),
              "an error frame isn't an FD frame");
}

TEST(CandumpTest, ADirectionAtTheEndMarksOurOwnTransmit)
{
    Parsed tx("(1.000000) can0 7E0#0201 T");
    ASSERT_TRUE(tx.ok) << tx.why;
    EXPECT_EQ(tx.l.flags, ECUIO_FRAME_FLAG_TX_ECHO);
    EXPECT_EQ(tx.data(), (std::vector<uint8_t>{0x02, 0x01}));
    Parsed rx("(1.000000) can0 7E8#0441 R");
    ASSERT_TRUE(rx.ok) << rx.why;
    EXPECT_EQ(rx.l.flags, 0u);
    EXPECT_EQ(rx.data(), (std::vector<uint8_t>{0x04, 0x41}));
}

TEST(CandumpTest, EveryLineTheFormatDoesntHoldIsRefusedWithWhy)
{
    EXPECT_EQ(refused(""),
              "the line doesn't start with its time in parentheses");
    EXPECT_EQ(refused("1.000000 can0 123#"),
              "the line doesn't start with its time in parentheses");
    EXPECT_EQ(refused("(1.00000) can0 123#"),
              "the time's fraction isn't six digits");
    EXPECT_EQ(refused("(1.0000000) can0 123#"),
              "the time's fraction isn't six digits");
    EXPECT_EQ(refused("(.000000) can0 123#"),
              "the time isn't <seconds>.<microseconds>");
    EXPECT_EQ(refused("(1234567890123.000000) can0 123#"),
              "the time's seconds are past what a log holds");
    EXPECT_EQ(refused("(1.000000)can0 123#"),
              "a space doesn't follow the time");
    EXPECT_FALSE(Parsed("(1.000000) can0123456789abc 123#").ok);
    EXPECT_FALSE(Parsed("(1.000000) ca/n0 123#").ok);
    EXPECT_FALSE(Parsed("(1.000000)  123#").ok);
    EXPECT_EQ(refused("(1.000000) can0 800#"), "an 11-bit ID is at most 7FF");
    EXPECT_EQ(refused("(1.000000) can0 40000000#"),
              "a 29-bit ID is at most 1FFFFFFF");
    EXPECT_EQ(refused("(1.000000) can0 7E8F#00"),
              "the frame isn't <id>#<data>, its ID 3 or 8 hex digits");
    EXPECT_EQ(refused("(1.000000) can0 7E8 00"),
              "the frame isn't <id>#<data>, its ID 3 or 8 hex digits");
    EXPECT_EQ(refused("(1.000000) can0 12345#81:00:12345678#11"),
              "a CAN XL frame isn't read");
    EXPECT_EQ(refused("(1.000000) can0 123#112233445566778899"),
              "a classic frame holds at most 8 data bytes");
    EXPECT_EQ(refused("(1.000000) can0 123#123"),
              "the data isn't hex byte pairs");
    EXPECT_EQ(refused("(1.000000) can0 123#1G"),
              "the data isn't hex byte pairs");
    EXPECT_EQ(refused("(1.000000) can0 123#.11"),
              "a '.' stands only between two data bytes");
    EXPECT_EQ(refused("(1.000000) can0 123#11."),
              "a '.' stands only between two data bytes");
    EXPECT_EQ(refused("(1.000000) can0 123#11..22"),
              "a '.' stands only between two data bytes");
    EXPECT_EQ(refused("(1.000000) can0 123#11_9"),
              "a DLC after '_' comes only with eight data bytes");
    EXPECT_EQ(refused("(1.000000) can0 123#1122334455667788_8"),
              "a DLC after '_' is 9 to F");
    EXPECT_EQ(refused("(1.000000) can0 123#R9"),
              "a remote frame's length is one digit, 0 to 8");
    EXPECT_EQ(refused("(1.000000) can0 123#R3_9"),
              "a DLC after '_' comes only with a length of 8");
    EXPECT_EQ(refused("(1.000000) can0 123##G"),
              "an FD frame's flags aren't one hex digit");
    EXPECT_EQ(refused("(1.000000) can0 123#" +
                      std::string(ecuio_candump::kLineMax, '1')),
              "the line is longer than a candump line can be");
}

// ---- the import ----

/* A folder of the case's own, removed at the end */
struct Folder {
    fs::path path;
    explicit Folder(const char* prefix = "ecuio-import")
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name(prefix);
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

    std::string utf8(const std::string& name = "") const
    {
        return (name.empty() ? path : path / fs::u8path(name))
            .generic_u8string();
    }
    std::string write(const std::string& name, const std::string& text) const
    {
        std::ofstream f(path / fs::u8path(name), std::ios::binary);
        f << text;
        return utf8(name);
    }
};

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream s(text);
    std::string line;
    while (std::getline(s, line)) {
        out.push_back(line);
    }
    return out;
}

struct Imported {
    ecuio_status_t rc = ECUIO_ERR_NOT_IMPLEMENTED;
    ecuio_import_report_t report{};
    std::string detail;
    Imported(const std::string& in, const std::string& out,
             const char* iface = nullptr)
    {
        ecuio_import_options_t o{};
        o.format = ECUIO_IMPORT_CANDUMP;
        o.in_path_utf8 = in.c_str();
        o.out_folder_utf8 = out.c_str();
        o.interface_utf8 = iface;
        rc = ecuio_import(&o, &report);
        detail = ecuio_last_error_detail();
    }
};

/* No folder and no partial folder left at out */
void expect_nothing_left(const Folder& f, const std::string& name)
{
    EXPECT_FALSE(fs::exists(f.path / name)) << name;
    EXPECT_FALSE(partial_left(f.path, name)) << name;
}

/* An engine ECU's replies on can0 through a tester's requests: a Single
 * Frame (rpm 1726), then a packed reply of 10 bytes in a First Frame and
 * a Consecutive Frame around the tester's Flow Control (rpm 1000) */
const char* const kTwoReplies =
    "(1700000000.250000) can0 7E0#02010C0000000000\n"
    "(1700000000.254000) can0 7E8#04410C1AF8\n"
    "(1700000000.350000) can0 7E0#05010C0D0504\n"
    "(1700000000.354000) can0 7E8#100A410C0FA00D32\n"
    "(1700000000.354500) can0 7E0#3000000000000000\n"
    "(1700000000.355000) can0 7E8#21055A0480\n";

TEST(ImportTest, ALogBecomesARecordingOfItsFramesAsTheyArrived)
{
    Folder f;
    const std::string in = f.write("engine.log", kTwoReplies);
    Imported imp(in, f.utf8("rec"));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.lines, 6u);
    EXPECT_EQ(imp.report.frames, 6u);
    EXPECT_EQ(imp.report.error_frames, 0u);
    EXPECT_EQ(imp.report.interfaces, 1u);
    EXPECT_EQ(imp.report.utc_start_us, 1700000000250000u);
    EXPECT_EQ(imp.report.duration_us, 105000u);
    EXPECT_EQ(read_file(f.path / "rec" / "frames.csv"),
              "host_us,timestamp_us,id,flags,data\n"
              "0,0,0x7E0,0x0,02 01 0C 00 00 00 00 00\n"
              "4000,4000,0x7E8,0x0,04 41 0C 1A F8\n"
              "100000,100000,0x7E0,0x0,05 01 0C 0D 05 04\n"
              "104000,104000,0x7E8,0x0,10 0A 41 0C 0F A0 0D 32\n"
              "104500,104500,0x7E0,0x0,30 00 00 00 00 00 00 00\n"
              "105000,105000,0x7E8,0x0,21 05 5A 04 80\n");
    const std::vector<std::string> events =
        lines_of(read_file(f.path / "rec" / "events.log"));
    const std::vector<std::string> want = {
        "ecuio session recording, format 2 (ADR-0049)",
        "started  2023-11-14 22:13:20.250 (UTC)",
        "utc  2023-11-14T22:13:20.250Z",
        std::string("core  ") + ecuio_version_string(),
        "import  candump log, interface can0",
        "profile  raw",
        "frame flags  complete",
        "columns  UTC time, seconds since the recording started (host_us / "
        "1e6 in the CSV files), kind, text",
        "",
        "22:13:20.355  +0.105  end  import: 6 frames, 0 values, 0 records "
        "dropped, 0 notes dropped",
    };
    EXPECT_EQ(events, want);
    EXPECT_FALSE(partial_left(f.path, "rec"));
    EXPECT_FALSE(fs::exists(f.path / "rec" / "signals.csv"));
}

TEST(ImportTest, TheRecordingReplaysAndItsFramesDecodeThroughAProfile)
{
    Folder f;
    const std::string in = f.write("engine.log", kTwoReplies);
    const std::string out = f.utf8("rec");
    Imported imp(in, out);
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;

    ecuio_session_t* s = nullptr;
    const std::string uri = "replay://" + out;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t si{};
    EXPECT_EQ(ecuio_session_get_info(s, &si), ECUIO_OK);
    EXPECT_EQ(si.replay_frames, 6u);
    EXPECT_EQ(si.replay_span_ms, 105u);
    ecuio_session_destroy(s);

    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(
        ecuio_recording_open_frames(out.c_str(), obd2, nullptr, nullptr, &r),
        ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    EXPECT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_RECORDING_INFO_FRAMES, 0u);
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_CORE);
    EXPECT_EQ(info.utc_start_us, 1700000000250000u);
    bool rpm = false;
    for (uint32_t k = 0; k < info.signal_count; ++k) {
        ecuio_recording_signal_t sig{};
        ASSERT_EQ(ecuio_recording_get_signal(r, k, &sig), ECUIO_OK);
        if (std::strcmp(sig.name, "RPM") == 0) {
            rpm = true;
            EXPECT_EQ(sig.samples, 2u); /* the Single Frame and the packed
                                           reply reassembled */
            EXPECT_DOUBLE_EQ(sig.lowest, 1000.0);
            EXPECT_DOUBLE_EQ(sig.highest, 1726.0);
            EXPECT_EQ(sig.first_us, 4000u);
            EXPECT_EQ(sig.last_us, 105000u);
        }
    }
    EXPECT_TRUE(rpm);
    ecuio_recording_close(r);
}

TEST(ImportTest, ErrorRemoteFdAndOwnFramesKeepTheirFlags)
{
    Folder f;
    const std::string in =
        f.write("flags.log", "(10.000000) can0 20000004#0000080000000000\r\n"
                             "\r\n"
                             "(10.000100) can0 123#R4\r\n"
                             "(10.000200) can0 456##3AABB\r\n"
                             "(10.000300) can0 7E0#0201 T\r\n"
                             "(10.000400) can0 18DAF110#0441 R\r\n");
    Imported imp(in, f.utf8("rec"));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(imp.report.lines, 6u);
    EXPECT_EQ(imp.report.frames, 5u);
    EXPECT_EQ(imp.report.error_frames, 1u);
    EXPECT_EQ(read_file(f.path / "rec" / "frames.csv"),
              "host_us,timestamp_us,id,flags,data\n"
              "0,0,0x4,0x10,00 00 08 00 00 00 00 00\n"
              "100,100,0x123,0x2,\n"
              "200,200,0x456,0x20C,AA BB\n"
              "300,300,0x7E0,0x20,02 01\n"
              "400,400,0x18DAF110,0x1,04 41\n");
}

TEST(ImportTest, AFrameStampedJustBeforeTheOneBeforeTakesItsTime)
{
    Folder f;
    const std::string in = f.write("cpus.log", "(5.200000) can0 100#01\n"
                                               "(5.150000) can0 100#02\n"
                                               "(5.300000) can0 100#03\n");
    Imported imp(in, f.utf8("rec"));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_EQ(read_file(f.path / "rec" / "frames.csv"),
              "host_us,timestamp_us,id,flags,data\n"
              "0,0,0x100,0x0,01\n"
              "0,0,0x100,0x0,02\n"
              "100000,100000,0x100,0x0,03\n");

    const std::string back = f.write("back.log", "(5.200000) can0 100#01\n"
                                                 "(5.300000) can0 100#02\n"
                                                 "(5.199999) can0 100#03\n");
    Imported late(back, f.utf8("late"));
    EXPECT_EQ(late.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(late.detail,
              "line 3: its time is more than 0.1 s before the frame before it");
    expect_nothing_left(f, "late");
}

TEST(ImportTest, ALogOfSeveralInterfacesTakesTheOneNamed)
{
    Folder f;
    const std::string in = f.write("two.log", "(1.000000) can0 100#01\n"
                                              "(1.100000) can1 200#02\n"
                                              "(1.200000) can0 100#03\n"
                                              "(1.300000) can1 200#04\n");
    Imported none(in, f.utf8("none"));
    EXPECT_EQ(none.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(none.detail,
              "the log has several interfaces: name one of can0 and can1");
    expect_nothing_left(f, "none");

    Imported other(in, f.utf8("other"), "can9");
    EXPECT_EQ(other.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(other.detail,
              "the log has no interface 'can9': it has can0 and can1");
    expect_nothing_left(f, "other");

    Imported one(in, f.utf8("rec"), "can1");
    ASSERT_EQ(one.rc, ECUIO_OK) << one.detail;
    EXPECT_EQ(one.report.lines, 4u);
    EXPECT_EQ(one.report.frames, 2u);
    EXPECT_EQ(one.report.interfaces, 2u);
    EXPECT_EQ(one.report.utc_start_us, 1100000u);
    EXPECT_EQ(one.report.duration_us, 200000u);
    EXPECT_EQ(read_file(f.path / "rec" / "frames.csv"),
              "host_us,timestamp_us,id,flags,data\n"
              "0,0,0x200,0x0,02\n"
              "200000,200000,0x200,0x0,04\n");
    EXPECT_NE(read_file(f.path / "rec" / "events.log")
                  .find("\nimport  candump log, interface can1\n"),
              std::string::npos);
}

TEST(ImportTest, ALineTheFormatDoesntHoldRefusesTheWholeLogByItsNumber)
{
    Folder f;
    const std::string in = f.write("bad.log", "(1.000000) can0 100#01\n"
                                              "(1.100000) can0 100#0\n");
    Imported imp(in, f.utf8("rec"));
    EXPECT_EQ(imp.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(imp.detail, "line 2: the data isn't hex byte pairs");
    expect_nothing_left(f, "rec");

    const std::string empty = f.write("empty.log", "\n\n");
    Imported nothing(empty, f.utf8("empty"));
    EXPECT_EQ(nothing.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(nothing.detail, "the log holds no frames");
    expect_nothing_left(f, "empty");

    Imported missing(f.utf8("missing.log"), f.utf8("missing"));
    EXPECT_EQ(missing.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(missing.detail.rfind("the log can't be opened: ", 0), 0u)
        << missing.detail;
    EXPECT_EQ(missing.detail.find(f.utf8()), std::string::npos);
    expect_nothing_left(f, "missing");
}

TEST(ImportTest, AFolderThatExistsIsLeftAlone)
{
    Folder f;
    const std::string in = f.write("engine.log", kTwoReplies);
    fs::create_directories(f.path / "rec");
    f.write("rec/keep.txt", "mine");
    Imported imp(in, f.utf8("rec"));
    EXPECT_EQ(imp.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(imp.detail, "the recording's folder exists already");
    EXPECT_EQ(read_file(f.path / "rec" / "keep.txt"), "mine");
    EXPECT_FALSE(fs::exists(f.path / "rec" / "frames.csv"));
    EXPECT_FALSE(partial_left(f.path, "rec"));

    /* A trailing separator names the same folder */
    Imported slash(in, f.utf8("rec") + "/");
    EXPECT_EQ(slash.rc, ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(slash.detail, "the recording's folder exists already");
}

TEST(ImportTest, AFolderWhoseNameIsntAsciiIsWritten)
{
    Folder f;
    const std::string in = f.write(u8"motor-Ω测.log", kTwoReplies);
    Imported imp(in, f.utf8(u8"opptak-Ω测"));
    ASSERT_EQ(imp.rc, ECUIO_OK) << imp.detail;
    EXPECT_TRUE(fs::exists(f.path / fs::u8path(u8"opptak-Ω测") / "frames.csv"));
}

TEST(ImportTest, ArgumentsAreCheckedBeforeAnythingIsRead)
{
    ecuio_import_options_t o{};
    o.format = ECUIO_IMPORT_CANDUMP;
    o.in_path_utf8 = "in.log";
    o.out_folder_utf8 = "out";
    ecuio_import_report_t r{};
    r.lines = 7;
    EXPECT_EQ(ecuio_import(nullptr, &r), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(r.lines, 0u); /* zeroed first */
    ecuio_import_options_t a = o;
    a.in_path_utf8 = nullptr;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    a = o;
    a.out_folder_utf8 = nullptr;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    a = o;
    a.flags = 1;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.reserved[3] = 1;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.format = 0;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.format = 3;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a = o;
    a.column_map_utf8 = "{}";
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    /* A CSV needs its column map, and takes no interface */
    a = o;
    a.format = ECUIO_IMPORT_CSV;
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
    a.column_map_utf8 = "{}";
    a.interface_utf8 = "can0";
    EXPECT_EQ(ecuio_import(&a, nullptr), ECUIO_ERR_INVALID_ARGUMENT);
}

/* Out of memory at each of an import's allocations in turn: every one
 * returns (never terminates), and none leaves its folder or a partial */
TEST(ImportTest, OutOfMemoryAnywhereLeavesNoFolderBehind)
{
    Folder f;
    const std::string in = f.write("engine.log", kTwoReplies);
    const std::string out = f.utf8("rec");
    ecuio_import_options_t o{};
    o.format = ECUIO_IMPORT_CANDUMP;
    o.in_path_utf8 = in.c_str();
    o.out_folder_utf8 = out.c_str();
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_import(&o, nullptr), ECUIO_OK);
    const unsigned long news = news_on_this_thread() - before;
    std::error_code ec;
    fs::remove_all(f.path / "rec", ec);
    ASSERT_GT(news, 10u);
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        fail_new_on_this_thread(n);
        const ecuio_status_t rc = ecuio_import(&o, nullptr);
        fail_new_on_this_thread(0);
        if (rc != ECUIO_OK) {
            ++refused;
            ASSERT_TRUE(rc == ECUIO_ERR_OUT_OF_MEMORY ||
                        rc == ECUIO_ERR_LOAD_FAILED)
                << "at " << n << ": " << rc;
            ASSERT_FALSE(fs::exists(f.path / "rec")) << "at " << n;
        }
        ASSERT_FALSE(partial_left(f.path, "rec")) << "at " << n;
        fs::remove_all(f.path / "rec", ec);
    }
    EXPECT_GT(refused, 0u);
}

} // namespace
