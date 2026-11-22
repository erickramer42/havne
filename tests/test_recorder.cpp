// File/layer: tests/test_recorder.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)
//
// Each case records into a folder of its own under TEMP/TMPDIR, removed
// at the end.

#include "test_recorder_support.h"

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
    EXPECT_FALSE(fs::exists(folder / "latency.csv"));

    /* A cut folder name says how long it is */
    char small[8];
    ASSERT_EQ(ecuio_recorder_get_folder(rec, small, sizeof small, &len),
              ECUIO_OK);
    EXPECT_EQ(std::strlen(small), 7u);
    EXPECT_EQ(len, std::strlen(buf));
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
}

/* The folder is the root as the caller spelled it, made absolute: a
 * relative root joins the working directory, "." and ".." go, and a
 * link in it stays (macOS's temporary directory is one, /var to
 * /private/var, and the app shows the folder relative to the root it
 * named) */
TEST(RecorderTest, TheFolderKeepsTheRootAsTheCallerSpelledIt)
{
    Root root;
    std::error_code ec;
    fs::create_directories(root.path / "real", ec);
    ASSERT_FALSE(ec) << ec.message();
    auto parent_of_folder = [](const std::string& r) {
        ecuio_record_options_t o{};
        o.root_utf8 = r.c_str();
        ecuio_recorder_t* rec = nullptr;
        EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
            << ecuio_last_error_detail();
        char buf[2048] = "";
        uint32_t len = 0;
        if (rec != nullptr) {
            EXPECT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len),
                      ECUIO_OK);
            EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
        }
        return fs::u8path(buf).parent_path();
    };
    EXPECT_EQ(parent_of_folder(root.utf8() + "/real/./x/../y"),
              root.path / "real" / "y");

    const std::string relative = temp_name("ecuio-recorder-relative");
    const fs::path cwd = fs::current_path();
    EXPECT_EQ(parent_of_folder(relative), cwd / relative);
    fs::remove_all(cwd / relative, ec);

#ifndef _WIN32
    /* Making a link on Windows needs a privilege; GetFullPathNameW never
     * follows one */
    fs::create_directory_symlink(root.path / "real", root.path / "link", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(parent_of_folder(root.utf8() + "/link"), root.path / "link");
#endif
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

/* No out, header lines missing for their count, an empty root: each
 * refused before anything is made, the root left as it was */
TEST(RecorderTest, OpenRefusesEachBadArgument)
{
    Root root;
    const std::string r = root.utf8();
    ecuio_record_options_t o{};
    o.root_utf8 = r.c_str();
    EXPECT_EQ(ecuio_recorder_open(&o, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_recorder_t* rec = nullptr;
    o.header_line_count = 1;
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(rec, nullptr);
    o.header_line_count = 0;
    o.root_utf8 = "";
    EXPECT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rec, nullptr);
    EXPECT_FALSE(fs::exists(root.path)) << "nothing made";
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
    o.flags = ECUIO_RECORD_ARMED << 1;
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

    /* A started session takes a recorder (ADR-0056 section 3); a
     * recorder that has had a session takes no other */
    ecuio_session_t* started = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://started", &started), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(started), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(started, rec), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_record(started, rec), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(started);

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://first", &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_record(s, rec), ECUIO_ERR_INVALID_ARGUMENT)
        << "a recorder whose session is gone takes no other";
    ecuio_recorder_t* fresh = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &fresh), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, fresh), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, fresh), ECUIO_ERR_INVALID_ARGUMENT);

    /* One recorder holds one session; a session holds one recorder at a
     * time */
    ecuio_session_t* other = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://other", &other), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(other, fresh), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_recorder_t* second = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &second), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(s, second), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_close(other);
    EXPECT_EQ(ecuio_session_record(other, second), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(other);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(second, nullptr), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_close(fresh, nullptr), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);

    /* An armed recorder (ADR-0063 section 7) needs a condition that
     * records: none attached, or a set whose only record is switched off,
     * is refused with the words, the recorder left for a later try */
    ecuio_record_options_t armed{};
    armed.root_utf8 = r.c_str();
    armed.flags = ECUIO_RECORD_ARMED;
    ecuio_recorder_t* waiting = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&armed, &waiting), ECUIO_OK);
    ecuio_session_t* bare = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://armed", &bare), ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(bare, waiting), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(
        std::string(ecuio_last_error_detail()).rfind("no condition records", 0),
        0u)
        << ecuio_last_error_detail();
    const auto attach = [bare](const std::string& conditions) {
        const std::string json =
            R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
        ecuio_condition_set_t* set = nullptr;
        EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_set_conditions(bare, set, nullptr), ECUIO_OK);
        ecuio_condition_set_free(set);
    };
    const std::string when =
        R"("when":{"signal":["obd2.rpm"],"unit":"RPM","above":2000})";
    attach(R"({"key":"marked","name":"M",)" + when + R"(,"mark":true})");
    EXPECT_EQ(ecuio_session_record(bare, waiting), ECUIO_ERR_INVALID_ARGUMENT);
    attach(R"({"key":"off","name":"O","enabled":false,)" + when +
           R"(,"record":{"before":5}})");
    EXPECT_EQ(ecuio_session_record(bare, waiting), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("enabled"),
              std::string::npos)
        << ecuio_last_error_detail();
    attach(R"({"key":"on","name":"N",)" + when + R"(,"record":{"before":5}})");
    EXPECT_EQ(ecuio_session_record(bare, waiting), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(waiting, &st), ECUIO_OK);
    EXPECT_EQ(st.state, ECUIO_RECORDING_ARMED);
    EXPECT_EQ(st.windows, 0u);
    uint32_t len = 1;
    char folder[8] = "x";
    ASSERT_EQ(ecuio_recorder_get_folder(waiting, folder, sizeof folder, &len),
              ECUIO_OK);
    EXPECT_EQ(len, 0u) << "no folder before the first window";
    EXPECT_STREQ(folder, "");
    ecuio_session_destroy(bare);
    EXPECT_EQ(ecuio_recorder_close(waiting, nullptr), ECUIO_OK);
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
    EXPECT_NE(values[1].find(",100::0.0:8,1.5,\xC2\xB0"
                             "C"),
              std::string::npos)
        << values[1];
    EXPECT_NE(values[2].find(",101::0:t0,\"A,B \"\"q\"\"\","),
              std::string::npos)
        << values[2];

    const std::string events = rec.file("events.log");
    const std::vector<std::string> ev = lines_of(events);
    ASSERT_GE(ev.size(), 10u) << events;
    EXPECT_EQ(ev[0], "ecuio session recording, format 4 (ADR-0049)");
    EXPECT_EQ(ev[1].rfind("started  ", 0), 0u);
    EXPECT_EQ(ev[2].rfind("utc  ", 0), 0u);
    EXPECT_EQ(ev[2].back(), 'Z');
    EXPECT_NE(events.find("\nuri  mock://rec?profile=rec-test\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nprofile  rec-test\n"), std::string::npos);
    EXPECT_NE(events.find("\nframe flags  complete\n"), std::string::npos);
    EXPECT_NE(events.find("\nsignal  100::0.0:8,Half,number,\xC2\xB0"
                          "C,1,2,3\n"),
              std::string::npos);
    EXPECT_NE(events.find("\nsignal  101::0:t0,Words,text,,0,0,0\n"),
              std::string::npos);
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

/* Format 3 (ADR-0049's appendix of 2026-11-05): an enum's raw number in
 * signals.csv and its labels on label lines after its signal line, read
 * back through the reader. A label's comma and quotes are CSV-quoted, a
 * line break in one is a space; only the first def of a name writes
 * labels (the reader keeps that one's signal line), and an enum with no
 * entries or a number writes none. */
constexpr uint32_t kEnumId = 0x110;
ecuio_signal_def_t kEnumDefs[4];
ecuio_enum_entry_t kEnumEntries[4];
ecuio_signal_set_t kEnumSet;

ecuio_enum_entry_t entry(uint32_t raw, const char* label)
{
    ecuio_enum_entry_t e{};
    e.raw = raw;
    std::strcpy(e.label, label);
    return e;
}

void register_enum_profile()
{
    static bool done = false;
    if (done)
        return;
    std::memset(kEnumDefs, 0, sizeof kEnumDefs);
    for (int i = 0; i < 4; ++i) {
        ecuio_signal_def_t& d = kEnumDefs[i];
        d.id = static_cast<uint16_t>(i + 1);
        d.message_id = kEnumId + static_cast<uint32_t>(i);
        d.kind = ECUIO_VALUE_ENUM;
        d.bit_length = 8;
        d.scale = 1.0;
    }
    std::strcpy(kEnumDefs[0].name, "Fuel, System");
    kEnumDefs[0].table_first = 0;
    kEnumDefs[0].table_count = 3;
    kEnumDefs[0].plot_rank = 1;
    kEnumDefs[0].group = ECUIO_SIGNAL_GROUP_FUEL;
    std::strcpy(kEnumDefs[1].name, "Fuel, Spare");
    kEnumDefs[1].table_first = 3;
    kEnumDefs[1].table_count = 1;
    std::strcpy(kEnumDefs[2].name, "Mode"); /* no entries */
    kEnumDefs[3].kind = ECUIO_VALUE_UINT;
    std::strcpy(kEnumDefs[3].name, "Count");
    kEnumEntries[0] = entry(1, "Open loop, cold");
    kEnumEntries[1] = entry(2, "Closed \"loop\"");
    kEnumEntries[2] = entry(4294967295u, "Two\nlines");
    kEnumEntries[3] = entry(9, "Other");
    kEnumSet = ecuio_signal_set_t{};
    kEnumSet.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    kEnumSet.count = 4;
    kEnumSet.defs = kEnumDefs;
    kEnumSet.enum_count = 4;
    kEnumSet.enums = kEnumEntries;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("rec-enum", &kEnumSet, &why), ECUIO_OK)
        << "code " << why.code << " at " << why.index;
    done = true;
}

TEST(RecorderTest, AnEnumsNumberIsInTheRowsAndItsLabelsInTheHeader)
{
    register_enum_profile();
    Root root;
    Recording rec(root, "mock://enum?profile=rec-enum");
    ASSERT_NE(rec.recorder, nullptr);
    dispatch(rec.session, kEnumId, std::string("\x02", 1));
    dispatch(rec.session, kEnumId, std::string("\x03", 1)); /* no label */
    dispatch(rec.session, kEnumId + 3, std::string("\x07", 1));
    const std::string folder = rec.folder;
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    const std::vector<std::string> values = lines_of(rec.file("signals.csv"));
    ASSERT_EQ(values.size(), 4u);
    EXPECT_NE(values[1].find(",110::0.0:8,2,"), std::string::npos) << values[1];
    EXPECT_NE(values[2].find(",110::0.0:8,3,"), std::string::npos) << values[2];
    EXPECT_NE(values[3].find(",113::0.0:8,7,"), std::string::npos) << values[3];

    const std::string events = rec.file("events.log");
    EXPECT_EQ(events.rfind("ecuio session recording, format 4 (ADR-0049)\n", 0),
              0u);
    /* Each signal's key, then its name; a label by its signal's key */
    EXPECT_NE(events.find("\nsignal  110::0.0:8,\"Fuel, System\",enum,,0,1,3\n"
                          "label  110::0.0:8,1,\"Open loop, cold\"\n"
                          "label  110::0.0:8,2,\"Closed \"\"loop\"\"\"\n"
                          "label  110::0.0:8,4294967295,Two lines\n"
                          "signal  111::0.0:8,\"Fuel, Spare\",enum,,0,0,0\n"
                          "label  111::0.0:8,9,Other\n"
                          "signal  112::0.0:8,Mode,enum,,0,0,0\n"
                          "signal  113::0.0:8,Count,number,,0,0,0\n"),
              std::string::npos)
        << events;

    /* Read back: the number as a number, the word beside it */
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_EQ(info.format, ECUIO_RECORDING_FORMAT_KEYS);
    ASSERT_EQ(info.signal_count, 4u);
    ecuio_recording_signal_t fuel{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 0, &fuel), ECUIO_OK);
    EXPECT_STREQ(fuel.name, "Fuel, System");
    EXPECT_EQ(fuel.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(fuel.samples, 2u);
    EXPECT_EQ(fuel.lowest, 2.0);
    EXPECT_EQ(fuel.highest, 3.0);
    ecuio_enum_entry_t got[4] = {};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_recording_get_labels(r, 0, got, 4, &count), ECUIO_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(got[0].raw, 1u);
    EXPECT_STREQ(got[0].label, "Open loop, cold");
    EXPECT_STREQ(got[1].label, "Closed \"loop\"");
    EXPECT_EQ(got[2].raw, 4294967295u);
    EXPECT_STREQ(got[2].label, "Two lines");
    EXPECT_STREQ(ecuio_recording_enum_label(r, 0, 2), "Closed \"loop\"");
    EXPECT_STREQ(ecuio_recording_enum_label(r, 1, 9), "Other");
    ecuio_recording_signal_t mode{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 2, &mode), ECUIO_OK);
    EXPECT_EQ(mode.flags, ECUIO_RECORDING_SIGNAL_ENUM);
    EXPECT_EQ(ecuio_recording_get_labels(r, 2, nullptr, 0, &count), ECUIO_OK);
    EXPECT_EQ(count, 0u);
    ecuio_recording_signal_t plain{};
    ASSERT_EQ(ecuio_recording_get_signal(r, 3, &plain), ECUIO_OK);
    EXPECT_EQ(plain.flags, 0u);
    ecuio_recording_close(r);
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
    EXPECT_NE(rec.file("signals.csv").find(",100::0.0:8,3.5,"),
              std::string::npos)
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
