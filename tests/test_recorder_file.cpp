// File/layer: tests/test_recorder_file.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)

#include "test_recorder_support.h"

/* The writer syncs its files to the storage every 2 s, so a power cut
 * loses at most that */
TEST(RecorderTest, TheWriterSyncsEveryTwoSeconds)
{
    Root root;
    Recording rec(root, "mock://sync");
    EXPECT_EQ(ecuio_recorder_test_syncs(rec.recorder), 0u);
    EXPECT_TRUE(eventually(
        [&] { return ecuio_recorder_test_syncs(rec.recorder) >= 1; }, 3500));
    EXPECT_LE(ecuio_recorder_test_syncs(rec.recorder), 2u);
}

namespace {

/* A file's bytes as they are */
std::string read_bytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* The signal-file fixture loaded and registered as its profile,
 * unregistered and freed at the end */
struct LoadedFile {
    std::string bytes;
    ecuio_signal_file_t* file = nullptr;
    std::string profile;

    explicit LoadedFile(const std::string& path)
        : bytes(read_bytes(fs::u8path(path)))
    {
        EXPECT_EQ(ecuio_signal_file_load(bytes.data(), bytes.size(), &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        if (file != nullptr) {
            profile = ecuio_signal_file_profile(file);
            EXPECT_EQ(ecuio_profile_register(profile.c_str(),
                                             ecuio_signal_file_set(file),
                                             nullptr),
                      ECUIO_OK);
        }
    }
    ~LoadedFile()
    {
        if (file != nullptr) {
            (void)ecuio_profile_unregister(profile.c_str());
        }
        ecuio_signal_file_free(file);
    }
    const ecuio_signal_set_t* set() const
    {
        return ecuio_signal_file_set(file);
    }
};

const std::string kExtras = std::string(ECUIO_SIGNAL_FIXTURES) + "/extras.json";

/* A recorder opened under root and given file (ecuio_recorder_signal_file)
 * before the session on uri attaches; with no file, set alone, as a set
 * no one file holds */
struct FileRecording {
    ecuio_session_t* session = nullptr;
    ecuio_recorder_t* recorder = nullptr;
    std::string folder;
    ecuio_status_t attached = ECUIO_ERR_NULL_ARGUMENT;

    FileRecording(const Root& root, const std::string& uri,
                  const ecuio_signal_file_t* file,
                  const ecuio_signal_set_t* set = nullptr)
    {
        EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        const std::string keep = root.utf8();
        o.root_utf8 = keep.c_str();
        EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK);
        if (recorder == nullptr || session == nullptr) {
            return;
        }
        char buf[1024];
        uint32_t len = 0;
        (void)ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len);
        folder = buf;
        EXPECT_EQ(file != nullptr
                      ? ecuio_recorder_signal_file(recorder, file)
                      : ecuio_recorder_take_signals(recorder, set, nullptr, 0),
                  ECUIO_OK);
        attached = ecuio_session_record(session, recorder);
    }
    ecuio_status_t finish()
    {
        ecuio_session_destroy(session);
        session = nullptr;
        const ecuio_status_t st = ecuio_recorder_close(recorder, "done");
        recorder = nullptr;
        return st;
    }
    ~FileRecording()
    {
        ecuio_session_destroy(session);
        ecuio_recorder_close(recorder, nullptr);
    }
    fs::path path(const char* name) const { return fs::u8path(folder) / name; }
};

ecuio_recording_signals_t signals_of(const std::string& folder)
{
    ecuio_recording_signals_t s{};
    ecuio_recording_t* r = nullptr;
    EXPECT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_recording_get_signals(r, &s), ECUIO_OK);
    ecuio_recording_close(r);
    return s;
}

/* signals.csv's rows without their host_us: what was decoded, in order */
std::vector<std::string> decoded_rows(const fs::path& csv)
{
    std::vector<std::string> out;
    const std::vector<std::string> rows = lines_of(read_text(csv));
    for (size_t i = 1; i < rows.size(); ++i) {
        out.push_back(rows[i].substr(rows[i].find(',') + 1));
    }
    return out;
}

} // namespace

/* The signal file that decodes a session is copied into its recording
 * byte for byte, its SHA-256 on the header after the profile, and the
 * reader says whether the bytes there are still those (ADR-0051 section
 * 4) */
TEST(RecorderTest, ASignalFileIsCopiedWithItsHashOnTheHeader)
{
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    FileRecording rec(root, "mock://x?profile=" + f.profile, f.file);
    ASSERT_EQ(rec.attached, ECUIO_OK);
    ASSERT_EQ(rec.finish(), ECUIO_OK);

    EXPECT_EQ(read_bytes(rec.path("signals.json")), f.bytes);
    const std::string sha =
        ecuio_sha256::hex_of(f.bytes.data(), f.bytes.size());
    EXPECT_NE(read_text(rec.path("events.log"))
                  .find("\nprofile  " + f.profile +
                        "\nsignal file  signals.json," + sha + "\n"),
              std::string::npos);

    ecuio_recording_signals_t s = signals_of(rec.folder);
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED |
                           ECUIO_RECORDING_SIGNALS_PRESENT |
                           ECUIO_RECORDING_SIGNALS_MATCH);
    EXPECT_EQ(s.length, f.bytes.size());
    EXPECT_EQ(std::string(s.sha256), sha);

    /* One byte changed: there, but not the bytes that decoded it */
    {
        std::ofstream out(rec.path("signals.json"), std::ios::binary);
        out << f.bytes << ' ';
    }
    s = signals_of(rec.folder);
    EXPECT_EQ(s.flags,
              ECUIO_RECORDING_SIGNALS_NAMED | ECUIO_RECORDING_SIGNALS_PRESENT);
    EXPECT_EQ(s.length, f.bytes.size() + 1);
    EXPECT_EQ(std::string(s.sha256), sha);

    /* Gone: named only */
    fs::remove(rec.path("signals.json"));
    s = signals_of(rec.folder);
    EXPECT_EQ(s.flags, ECUIO_RECORDING_SIGNALS_NAMED);
    EXPECT_EQ(s.length, 0u);
}

/* A recording of a session no file decodes has neither the file nor the
 * line, and the reader names none; nor does one given a set no one file
 * holds (length 0) */
TEST(RecorderTest, ASessionNoFileDecodesNamesNone)
{
    register_profile();
    Root root;
    {
        Recording rec(root, "mock://x?profile=rec-test");
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        EXPECT_FALSE(fs::exists(fs::u8path(rec.folder) / "signals.json"));
        EXPECT_EQ(rec.file("events.log").find("signal file"),
                  std::string::npos);
        const ecuio_recording_signals_t s = signals_of(rec.folder);
        EXPECT_EQ(s.flags, 0u);
        EXPECT_STREQ(s.sha256, "");
    }
    FileRecording rec(root, "mock://y?profile=rec-test", nullptr, &kSet);
    ASSERT_EQ(rec.attached, ECUIO_OK);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    EXPECT_FALSE(fs::exists(rec.path("signals.json")));
    EXPECT_EQ(read_text(rec.path("events.log")).find("signal file"),
              std::string::npos);
}

/* The file is taken once, before attach, and only for the session it
 * decodes: one whose set the session doesn't use refuses the attach */
TEST(RecorderTest, AFileIsTakenOnlyForTheSessionItDecodes)
{
    register_profile();
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    {
        FileRecording rec(root, "mock://x?profile=rec-test", f.file);
        EXPECT_EQ(rec.attached, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("the signal file isn't what decodes the session "
                            "(its profile is rec-test)"),
                  std::string::npos)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_recorder_close(rec.recorder, nullptr), ECUIO_OK);
        rec.recorder = nullptr;
    }

    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* r = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(nullptr, f.file),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_signal_file(r, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(nullptr, f.set(), "x", 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(r, nullptr, "x", 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_recorder_take_signals(r, f.set(), nullptr, 1),
              ECUIO_ERR_NULL_ARGUMENT);
    const std::string big(ECUIO_SIGNAL_SET_MAX_BYTES + 1, ' ');
    EXPECT_EQ(ecuio_recorder_take_signals(r, f.set(), big.data(), big.size()),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a byte past a set's written copy";
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_t* s = nullptr;
    const std::string uri = "mock://z?profile=" + f.profile;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(s, r), ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_signal_file(r, f.file),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(r, nullptr), ECUIO_OK);
}

/* A signals.json that can't be written fails the recording as any write
 * does */
TEST(RecorderTest, ASignalFileThatCantBeWrittenFailsTheRecording)
{
    LoadedFile f(kExtras);
    ASSERT_NE(f.file, nullptr);
    Root root;
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* r = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
    ecuio_recorder_test_fail_writes(r);
    ASSERT_EQ(ecuio_recorder_signal_file(r, f.file), ECUIO_OK);
    ecuio_session_t* s = nullptr;
    const std::string uri = "mock://z?profile=" + f.profile;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(s, r), ECUIO_OK);
    ecuio_recorder_state_t st{};
    EXPECT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(r, &st);
            return st.state == ECUIO_RECORDING_FAILED;
        },
        2000));
    EXPECT_STREQ(st.reason, "the disk is full");
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_recorder_close(r, nullptr), ECUIO_ERR_RECORDING_FAILED);
}

/* A replay of a recording with the file it holds decodes the same values
 * in the same order, and its own recording names the same bytes */
TEST(RecorderTest, AReplayWithTheRecordingsFileDecodesTheSame)
{
    const std::string source =
        std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
    const size_t frames =
        lines_of(read_text(fs::u8path(source) / "frames.csv")).size() - 1;
    Root root;

    auto record = [&](const std::string& from, const LoadedFile& f) {
        FileRecording rec(
            root, "replay://" + from + "?speed=max&profile=" + f.profile,
            f.file);
        EXPECT_EQ(rec.attached, ECUIO_OK);
        std::atomic<size_t> seen{0};
        auto count = [](const ecuio_frame_t*, void* user) {
            static_cast<std::atomic<size_t>*>(user)->fetch_add(1);
        };
        EXPECT_EQ(ecuio_register_frame_callback(rec.session, count, &seen),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
        EXPECT_TRUE(eventually([&] { return seen.load() >= frames; }, 20000))
            << seen.load();
        ecuio_recorder_state_t st{};
        ecuio_recorder_get_state(rec.recorder, &st);
        EXPECT_EQ(st.records_dropped, 0u);
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        return rec.folder;
    };

    std::string first;
    {
        LoadedFile f(kExtras);
        ASSERT_NE(f.file, nullptr);
        first = record(source, f);
    }
    std::string second;
    {
        LoadedFile again(first + "/signals.json");
        ASSERT_NE(again.file, nullptr);
        second = record(first, again);
    }
    const std::vector<std::string> a =
        decoded_rows(fs::u8path(first) / "signals.csv");
    const std::vector<std::string> b =
        decoded_rows(fs::u8path(second) / "signals.csv");
    ASSERT_GT(a.size(), 100u);
    EXPECT_NE(read_text(fs::u8path(first) / "signals.csv").find(",road_speed,"),
              std::string::npos)
        << "a signal at a base signal's address, under its author's key";
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "row " << i + 1;
    }
    const ecuio_recording_signals_t s1 = signals_of(first);
    const ecuio_recording_signals_t s2 = signals_of(second);
    EXPECT_EQ(s2.flags, s1.flags);
    EXPECT_STREQ(s2.sha256, s1.sha256);
}

namespace {

/* The example plugin read from the repository's plugins/, freed at the
 * end; variant 1 (my2019) registered as its profile */
struct ExamplePlugin {
    std::map<std::string, std::string> files;
    ecuio_plugin_t* plugin = nullptr;
    std::string profile;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<ExamplePlugin*>(user);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }

    ExamplePlugin()
    {
        const std::string root =
            std::string(ECUIO_PLUGINS_DIR) + "/example-sim/";
        for (const char* p : {"manifest.json", "data/base.json",
                              "data/my2019.json", "fixtures/values.json"}) {
            files[p] = read_bytes(fs::u8path(root + p));
        }
        const std::string& m = files["manifest.json"];
        EXPECT_EQ(ecuio_plugin_load(m.data(), m.size(), &read, this, &plugin),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
        EXPECT_EQ(ecuio_plugin_register(plugin, 1, name, sizeof name),
                  ECUIO_OK);
        profile = name;
    }
    ~ExamplePlugin() { EXPECT_EQ(ecuio_plugin_free(plugin), ECUIO_OK); }
    const ecuio_signal_file_t* file() const
    {
        return ecuio_plugin_variant_file(plugin, 1);
    }
};

} // namespace

/* A plugin variant's recording holds its merged file as signals.json and
 * names the plugin after it; a replay through that copy, loaded as any
 * signal file, decodes the same values (ADR-0052 section 4) */
TEST(RecorderTest, APluginVariantsRecordingHoldsItsMergedFile)
{
    ExamplePlugin p;
    ASSERT_NE(p.plugin, nullptr);
    ASSERT_EQ(p.profile, "plugin.example-sim.my2019");
    Root root;
    std::string first;
    {
        FileRecording rec(root, "mock://plugin?profile=" + p.profile, p.file());
        ASSERT_EQ(rec.attached, ECUIO_OK) << ecuio_last_error_detail();
        /* The TCM's fluid temperature and gear, as its replies read */
        for (int i = 0; i < 20; ++i) {
            dispatch(rec.session, 0x7E9,
                     std::string("\x62\xF4\x10\x1F", 4) +
                         static_cast<char>(0x40 + i));
            dispatch(rec.session, 0x7E9,
                     std::string("\x62\xF4\x11", 3) + static_cast<char>(i % 8));
        }
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        first = rec.folder;
    }
    const std::string json = read_bytes(fs::u8path(first) / "signals.json");
    ASSERT_FALSE(json.empty());
    const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());
    EXPECT_NE(read_text(fs::u8path(first) / "events.log")
                  .find("\nprofile  plugin.example-sim.my2019"
                        "\nsignal file  signals.json," +
                        sha + "\nplugin  example-sim,1.0.0,my2019\n"),
              std::string::npos)
        << read_text(fs::u8path(first) / "events.log").substr(0, 600);

    LoadedFile copy(first + "/signals.json");
    ASSERT_NE(copy.file, nullptr);
    EXPECT_EQ(copy.profile, "user.example-the-simulated-car");
    std::string second;
    {
        FileRecording rec(
            root, "replay://" + first + "?speed=max&profile=" + copy.profile,
            copy.file);
        ASSERT_EQ(rec.attached, ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
        ASSERT_TRUE(eventually(
            [&] {
                ecuio_recorder_state_t st{};
                ecuio_recorder_get_state(rec.recorder, &st);
                return st.frame_rows >= 40;
            },
            10000));
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        second = rec.folder;
    }
    const std::vector<std::string> a =
        decoded_rows(fs::u8path(first) / "signals.csv");
    const std::vector<std::string> b =
        decoded_rows(fs::u8path(second) / "signals.csv");
    ASSERT_GE(a.size(), 40u);
    EXPECT_NE(read_text(fs::u8path(first) / "signals.csv")
                  .find(",7E9:62F410:3.0:16,40.01,"),
              std::string::npos)
        << "the later file's scale, 0.01";
    EXPECT_EQ(a, b);
    EXPECT_EQ(read_bytes(fs::u8path(second) / "signals.json"), json);
    EXPECT_EQ(read_text(fs::u8path(second) / "events.log").find("\nplugin  "),
              std::string::npos)
        << "a file loaded alone names no plugin";
}

namespace {

/* A one-ECU signal file of signals, recorded on the mock and opened: the
 * reader's words when it can't open it, else its events.log */
std::string recorded_header(const std::string& name, const std::string& signals)
{
    const std::string text = R"({"havne_signals":1,"name":")" + name +
                             R"(","source":{"name":"t"},)"
                             R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" +
                             signals + "]}";
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(text.data(), text.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    if (f == nullptr) {
        return "not loaded";
    }
    const std::string profile = ecuio_signal_file_profile(f);
    EXPECT_EQ(ecuio_profile_register(profile.c_str(), ecuio_signal_file_set(f),
                                     nullptr),
              ECUIO_OK);
    std::string out;
    {
        Root root;
        FileRecording rec(root, "mock://x?profile=" + profile, f);
        EXPECT_EQ(rec.attached, ECUIO_OK);
        EXPECT_EQ(rec.finish(), ECUIO_OK);
        ecuio_recording_t* r = nullptr;
        out = ecuio_recording_open(rec.folder.c_str(), nullptr, nullptr, &r) ==
                      ECUIO_OK
                  ? read_text(rec.path("events.log"))
                  : std::string("not opened: ") + ecuio_last_error_detail();
        ecuio_recording_close(r);
    }
    (void)ecuio_profile_unregister(profile.c_str());
    ecuio_signal_file_free(f);
    return out;
}

} // namespace

// A number and an enum of one name can't be made in a signal file (a set's
// names are one each, so a recording keys its values by name): the file is
// refused at load, and a number beside an enum of another name opens
TEST(RecorderTest, ANumberAndAnEnumOfOneNameAreRefusedAtLoad)
{
    const std::string same =
        R"({"havne_signals":1,"name":"Recorder gear","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"g1","name":"Gear","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"g2","name":"Gear","read":{"service":"22","number":"D101"},"field":{"byte":0},)"
        R"("kind":"enum","values":{"1":"First","2":"Second"}}]})";
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(same.data(), same.size(), &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(f, nullptr);
    const std::string events = recorded_header(
        "Recorder gear",
        R"({"key":"g1","name":"Gear","read":{"service":"22","number":"D100"},"field":{"byte":0}},)"
        R"({"key":"m","name":"Mode","read":{"service":"22","number":"D102"},"field":{"byte":0},)"
        R"("kind":"enum","values":{"1":"Eco"}})");
    EXPECT_NE(events.find("\nsignal  7E8:62D100:3.0:8,Gear,number,"),
              std::string::npos)
        << events;
    EXPECT_EQ(events.find("\nlabel  7E8:62D100:3.0:8,"), std::string::npos)
        << events;
    EXPECT_NE(events.find("\nlabel  7E8:62D102:3.0:8,1,Eco\n"),
              std::string::npos)
        << events;
}

// A line break in a name or unit is a space on the header and in the
// rows, which no line may break
TEST(RecorderTest, ALineBreakInANameOrUnitIsASpace)
{
    const std::string events = recorded_header(
        "Recorder breaks",
        R"({"key":"g","name":"Gear\nX","unit":"k\r\nPa","read":{"service":"22","number":"D100"},"field":{"byte":0}})");
    EXPECT_NE(events.find("\nsignal  7E8:62D100:3.0:8,Gear X,number,k  Pa,"),
              std::string::npos)
        << events;
}
