// File/layer: tests/test_recorder_focus.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderTest (test_core)

#include "test_recorder_support.h"

/* The header holds the focus as it stands at the attach, the picks in
 * their order, and the link line only once the session has started;
 * both sit between the frame flags and the signal lines, where the
 * reader passes unknown keys by */
TEST(RecorderTest, TheHeaderHoldsTheFocusAndTheLinkAtTheAttach)
{
    register_profile();
    Root root;
    {
        Recording rec(root, "mock://focus?profile=rec-test", 0, nullptr, {}, 0,
                      false);
        ASSERT_NE(rec.recorder, nullptr);
        const uint16_t picks[2] = {2, 1};
        ASSERT_EQ(ecuio_session_set_focus(rec.session, picks, 2), ECUIO_OK);
        rec.attach();
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        const size_t flags = events.find("\nframe flags  complete\n");
        const size_t focus = events.find("\nfocus  101::0:t0,100::0.0:8\n");
        const size_t signal = events.find("\nsignal  ");
        EXPECT_NE(flags, std::string::npos) << events;
        EXPECT_NE(focus, std::string::npos) << events;
        EXPECT_NE(signal, std::string::npos) << events;
        EXPECT_TRUE(flags < focus && focus < signal) << events;
        EXPECT_EQ(events.find("\nlink  "), std::string::npos)
            << "not started: no link line: " << events;
        EXPECT_EQ(events.find("\nmin sweep  "), std::string::npos)
            << "no minimum time, no line: " << events;
    }
    {
        /* A front end's default pick is named "pick", never "focus" */
        Recording rec(root, "mock://focus-pick?profile=rec-test", 0, nullptr,
                      {}, 0, false);
        ASSERT_NE(rec.recorder, nullptr);
        const uint16_t picks[2] = {2, 1};
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, picks, 2, 0,
                                               ECUIO_FOCUS_DEFAULT_PICK),
                  ECUIO_OK);
        rec.attach();
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        const size_t flags = events.find("\nframe flags  complete\n");
        const size_t pick = events.find("\npick  101::0:t0,100::0.0:8\n");
        const size_t signal = events.find("\nsignal  ");
        EXPECT_NE(pick, std::string::npos) << events;
        EXPECT_TRUE(flags < pick && pick < signal) << events;
        EXPECT_EQ(events.find("\nfocus  "), std::string::npos) << events;
    }
    {
        Recording rec(root, "mock://focus-floored?profile=rec-test", 0, nullptr,
                      {}, 0, false);
        ASSERT_NE(rec.recorder, nullptr);
        const uint16_t pick = 1;
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, &pick, 1, 250, 0),
                  ECUIO_OK);
        rec.attach();
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        EXPECT_NE(events.find("\nfocus  100::0.0:8\n"), std::string::npos)
            << events;
        EXPECT_EQ(events.find("\nmin sweep  "), std::string::npos)
            << "the mock asks nothing, so nothing is paced: " << events;
    }
    {
        Recording rec(root, "mock://focus-started?profile=rec-test", 0, nullptr,
                      {}, 0, false);
        ASSERT_NE(rec.recorder, nullptr);
        ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
        const uint16_t pick = 1;
        ASSERT_EQ(ecuio_session_set_focus(rec.session, &pick, 1), ECUIO_OK);
        rec.attach();
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        const size_t focus = events.find("\nfocus  100::0.0:8\n");
        const size_t link =
            events.find("\nlink  healthy (state 0, 0 episodes)\n");
        const size_t signal = events.find("\nsignal  ");
        EXPECT_NE(focus, std::string::npos) << events;
        EXPECT_NE(link, std::string::npos) << events;
        EXPECT_TRUE(focus < link && link < signal) << events;
    }
}

namespace {

/* The cap's worth of signals with the longest names, some holding a
 * quote or a comma, so a focus of them all is past a row's bound */
constexpr uint32_t kWideDefs = ECUIO_PROFILE_MAX_REQUESTS;
std::vector<ecuio_signal_def_t> g_wide_defs;
ecuio_signal_set_t g_wide_set;

std::string wide_name(uint32_t k)
{
    char head[24];
    std::snprintf(head, sizeof head, "Wide %04u%s", k,
                  k % 7 == 0    ? " \"q\""
                  : k % 11 == 0 ? " a,b"
                                : "");
    std::string name = head;
    name.resize(sizeof(ecuio_signal_def_t{}.name) - 1, 'x');
    return name;
}

void register_wide_profile()
{
    static bool done = false;
    if (done) {
        return;
    }
    g_wide_defs.assign(kWideDefs, ecuio_signal_def_t{});
    for (uint32_t k = 0; k < kWideDefs; ++k) {
        ecuio_signal_def_t& d = g_wide_defs[k];
        d.id = static_cast<uint16_t>(1000 + k);
        d.message_id = 0x300;
        d.kind = ECUIO_VALUE_UINT;
        d.bit_length = 8;
        d.scale = 1;
        const std::string name = wide_name(k);
        std::memcpy(d.name, name.data(), name.size());
    }
    g_wide_set = ecuio_signal_set_t{};
    g_wide_set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    g_wide_set.count = kWideDefs;
    g_wide_set.defs = g_wide_defs.data();
    ASSERT_EQ(ecuio_profile_register("rec-wide", &g_wide_set, nullptr),
              ECUIO_OK);
    done = true;
}

/* Every id of the wide set, last first, then all of them again */
std::vector<uint16_t> wide_picks_twice()
{
    std::vector<uint16_t> ids;
    for (int round = 0; round < 2; ++round) {
        for (uint32_t k = kWideDefs; k-- > 0;) {
            ids.push_back(static_cast<uint16_t>(1000 + k));
        }
    }
    return ids;
}

/* The wide set's keys, last first: every def reads one address, so the
 * first keeps it and each later one goes by its id */
std::vector<std::string> wide_keys_last_first()
{
    std::vector<std::string> keys;
    for (uint32_t k = kWideDefs; k-- > 0;) {
        keys.push_back(k == 0 ? std::string("300::0.0:8")
                              : "id:" + std::to_string(1000 + k));
    }
    return keys;
}

/* A focus line's names as the readers split them */
void split_names(const std::string& text, std::vector<std::string>* out)
{
    const char* p = text.data();
    const char* end = p + text.size();
    std::string name;
    bool last = false;
    while (!last && ecuio_recording_rows::next_field(p, end, name, &last)) {
        out->push_back(name);
    }
}

/* events.log's activity lines of one kind, the text after the kind */
std::vector<std::string> activity_of(const std::string& events,
                                     const std::string& kind)
{
    std::vector<std::string> out;
    for (const std::string& line : lines_of(events)) {
        const size_t at = line.find("  " + kind + "  ");
        if (line.size() > 14 && line[2] == ':' && line[12] == ' ' &&
            line[14] == '+' && at != std::string::npos &&
            line.find("  ", 14) == at) {
            out.push_back(line.substr(at + kind.size() + 4));
        }
    }
    return out;
}

} // namespace

/* A focus past a row's room takes as many header lines as its names
 * need, each well inside a row, and a pick given twice is named once */
TEST(RecorderTest, AFocusPastALinesRoomTakesSeveralLinesAndEachPickOnce)
{
    register_wide_profile();
    Root root;
    Recording rec(root, "mock://wide?profile=rec-wide", 0, nullptr, {}, 0,
                  false);
    ASSERT_NE(rec.recorder, nullptr);
    const std::vector<uint16_t> picks = wide_picks_twice();
    ASSERT_EQ(ecuio_session_set_focus(rec.session, picks.data(),
                                      static_cast<uint32_t>(picks.size())),
              ECUIO_OK);
    rec.attach();
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::string events = rec.file("events.log");
    size_t lines = 0;
    std::vector<std::string> written;
    for (const std::string& line : lines_of(events)) {
        if (line.rfind("focus  ", 0) == 0) {
            ++lines;
            EXPECT_LE(line.size(), 7u + 4096u);
            split_names(line.substr(7), &written);
        }
    }
    EXPECT_GE(lines, 2u);
    EXPECT_EQ(written, wide_keys_last_first());
}

/* A focus changed after the attach is an activity line at its time, of
 * the header's kind, its names quoted as the header quotes them, "more"
 * lines past one line's room, "none" for an ended focus; a scrubbed
 * folder export keeps the lines */
TEST(RecorderTest, AFocusChangedAfterTheAttachIsAnActivityLine)
{
    register_profile();
    register_wide_profile();
    Root root;
    {
        Recording rec(root, "mock://change?profile=rec-test");
        ASSERT_NE(rec.recorder, nullptr);
        const uint16_t half = 1;
        ASSERT_EQ(ecuio_session_set_focus(rec.session, &half, 1), ECUIO_OK);
        const uint16_t both[3] = {2, 1, 2};
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, both, 3, 0,
                                               ECUIO_FOCUS_DEFAULT_PICK),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus(rec.session, nullptr, 0), ECUIO_OK);
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        EXPECT_EQ(activity_of(events, "focus"),
                  (std::vector<std::string>{"1 signal: 100::0.0:8",
                                            "none: every signal asked"}))
            << events;
        EXPECT_EQ(activity_of(events, "pick"),
                  (std::vector<std::string>{"2 signals: 101::0:t0,100::0.0:8"}))
            << events;
        const size_t one = events.find("  focus  1 signal: 100::0.0:8\n");
        const size_t two =
            events.find("  pick  2 signals: 101::0:t0,100::0.0:8\n");
        const size_t none = events.find("  focus  none: every signal asked\n");
        EXPECT_TRUE(one < two && two < none) << events;
        EXPECT_EQ(events.find("\nfocus  "), std::string::npos)
            << "no focus at the attach, no header line: " << events;
    }
    {
        Recording rec(root, "mock://change-wide?profile=rec-wide");
        ASSERT_NE(rec.recorder, nullptr);
        const std::vector<uint16_t> picks = wide_picks_twice();
        ASSERT_EQ(ecuio_session_set_focus(rec.session, picks.data(),
                                          static_cast<uint32_t>(picks.size())),
                  ECUIO_OK);
        const std::string folder = rec.folder;
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = read_text(fs::u8path(folder) / "events.log");
        const std::vector<std::string> lines = activity_of(events, "focus");
        ASSERT_GE(lines.size(), 2u) << events;
        const std::string first = std::to_string(kWideDefs) + " signals: ";
        ASSERT_EQ(lines[0].rfind(first, 0), 0u) << lines[0];
        std::vector<std::string> names;
        split_names(lines[0].substr(first.size()), &names);
        for (size_t k = 1; k < lines.size(); ++k) {
            ASSERT_EQ(lines[k].rfind("more: ", 0), 0u) << lines[k];
            EXPECT_LE(lines[k].size(), 6u + 4096u);
            split_names(lines[k].substr(6), &names);
        }
        EXPECT_EQ(names, wide_keys_last_first());

        const std::string copy = root.utf8() + "/copy";
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_FOLDER;
        o.folder_utf8 = folder.c_str();
        o.out_path_utf8 = copy.c_str();
        ecuio_export_report_t report{};
        ASSERT_EQ(ecuio_export(&o, &report), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(
            activity_of(read_text(fs::u8path(copy) / "events.log"), "focus"),
            lines)
            << "a scrubbed copy keeps them";
    }
}

/* A focus line comes only when the picks or their kind change: a call
 * that repeats the focus the header or the last line named writes none,
 * and on a session that asks nothing (the mock) a minimum alone is no
 * change; a whole set asked again is no line either */
TEST(RecorderTest, AFocusLineComesOnlyOnAChange)
{
    register_profile();
    Root root;
    {
        Recording rec(root, "mock://same?profile=rec-test", 0, nullptr, {}, 0,
                      false);
        ASSERT_NE(rec.recorder, nullptr);
        const uint16_t half = 1;
        ASSERT_EQ(ecuio_session_set_focus(rec.session, &half, 1), ECUIO_OK);
        rec.attach();
        ASSERT_EQ(ecuio_session_set_focus(rec.session, &half, 1), ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, &half, 1, 250, 0),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, &half, 1, 0,
                                               ECUIO_FOCUS_DEFAULT_PICK),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus_with(rec.session, &half, 1, 0,
                                               ECUIO_FOCUS_DEFAULT_PICK),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus(rec.session, nullptr, 0), ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus(rec.session, nullptr, 0), ECUIO_OK);
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        EXPECT_NE(events.find("\nfocus  100::0.0:8\n"), std::string::npos)
            << events;
        EXPECT_EQ(activity_of(events, "pick"),
                  std::vector<std::string>{"1 signal: 100::0.0:8"})
            << events;
        EXPECT_EQ(activity_of(events, "focus"),
                  std::vector<std::string>{"none: every signal asked"})
            << events;
    }
    {
        Recording rec(root, "mock://whole?profile=rec-test");
        ASSERT_NE(rec.recorder, nullptr);
        ASSERT_EQ(ecuio_session_set_focus(rec.session, nullptr, 0), ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_focus(rec.session, nullptr, 0), ECUIO_OK);
        ASSERT_EQ(rec.finish(), ECUIO_OK);
        const std::string events = rec.file("events.log");
        EXPECT_TRUE(activity_of(events, "focus").empty()) << events;
    }
}

/* A focus change's line goes through the note queue: with the queue
 * full it is counted as a dropped note and no line is written, while
 * the focus is taken */
TEST(RecorderTest, AFocusLineIntoAFullNoteQueueIsCountedDropped)
{
    constexpr uint32_t kNotes = 4;
    register_profile();
    Root root;
    Recording rec(root, "mock://full?profile=rec-test", 0, nullptr, {}, kNotes);
    ASSERT_NE(rec.recorder, nullptr);
    ASSERT_NO_FATAL_FAILURE(hold_writer(rec.recorder));
    ecuio_recorder_state_t st{};
    for (uint32_t i = 0; i <= kNotes && st.notes_dropped == 0; ++i) {
        ASSERT_EQ(ecuio_recorder_note(rec.recorder, "a queued note"), ECUIO_OK);
        ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    }
    ASSERT_EQ(st.notes_dropped, 1u) << "the queue never filled";
    const uint16_t half = 1;
    ASSERT_EQ(ecuio_session_set_focus(rec.session, &half, 1), ECUIO_OK);
    ASSERT_EQ(ecuio_recorder_get_state(rec.recorder, &st), ECUIO_OK);
    EXPECT_EQ(st.notes_dropped, 2u);
    ecuio_recorder_test_hold(rec.recorder, 0);
    ASSERT_EQ(rec.finish(), ECUIO_OK);
    const std::string events = rec.file("events.log");
    EXPECT_TRUE(activity_of(events, "focus").empty()) << events;
    EXPECT_NE(events.find("a queued note"), std::string::npos) << events;
}

/* A session takes one recorder at a time: a second is refused while the
 * first is attached and taken once the first has closed, counting from
 * its own attach; the refusal left it as it was, so its recording has
 * one header, one columns line and its signal file */
TEST(RecorderTest, ASecondRecorderAttachesOnceTheFirstHasClosed)
{
    Root root;
    Recording rec(root, "mock://second", 0, nullptr, {}, 0, false);
    ASSERT_NE(rec.recorder, nullptr);
    ASSERT_EQ(ecuio_session_start(rec.session), ECUIO_OK);
    rec.attach();
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    const char* caller[] = {"app  the second"};
    o.header_lines = caller;
    o.header_line_count = 1;
    ecuio_recorder_t* second = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &second), ECUIO_OK);
    const std::string file = "{\"the mock's\": \"file\"}";
    ASSERT_EQ(ecuio_recorder_take_signals(second,
                                          rec.session->active_signal_set,
                                          file.data(), file.size()),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_record(rec.session, second),
              ECUIO_ERR_INVALID_ARGUMENT);
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(second, buf, sizeof buf, &len),
              ECUIO_OK);
    const std::string folder = buf;

    EXPECT_EQ(ecuio_recorder_close(rec.recorder, "the first"), ECUIO_OK);
    rec.recorder = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t before = ecuio_clock_us();
    EXPECT_EQ(ecuio_session_record(rec.session, second), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recorder_state_t st{};
    ASSERT_TRUE(eventually(
        [&] {
            ecuio_recorder_get_state(second, &st);
            return st.frame_rows >= 2;
        },
        5000))
        << st.frame_rows << " frame rows";
    ecuio_session_destroy(rec.session);
    rec.session = nullptr;
    EXPECT_EQ(ecuio_recorder_close(second, "the second"), ECUIO_OK);
    const uint64_t after = ecuio_clock_us();
    const std::vector<std::string> frames =
        lines_of(read_text(fs::u8path(folder) / "frames.csv"));
    ASSERT_GE(frames.size(), 3u);
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_LE(std::stoull(frames[i]), after - before)
            << frames[i] << ": counted from the second attach";
    }
    const std::string events = read_text(fs::u8path(folder) / "events.log");
    const auto count = [&](const char* what) {
        size_t n = 0;
        for (size_t at = events.find(what); at != std::string::npos;
             at = events.find(what, at + 1))
            ++n;
        return n;
    };
    EXPECT_EQ(count("ecuio session recording, format 4"), 1u) << events;
    EXPECT_EQ(count("\ncolumns  "), 1u) << events;
    EXPECT_EQ(count("\nsignal file  "), 1u) << events;
    EXPECT_EQ(count("\ncaller  app  the second\n"), 1u) << events;
    EXPECT_EQ(read_text(fs::u8path(folder) / "signals.json"), file);
}
