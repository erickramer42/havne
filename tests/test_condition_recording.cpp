// File/layer: tests/test_condition_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ConditionRecordingTest (test_core)
//
// A session's conditions and its recording (ADR-0063 section 8): the set
// copied beside the recording, a cond line for each event, the states held
// when a recording begins mid-session, the carry past a full ring, the
// markers, a set that never changes under a recorder that is attaching
// or attached, the reader giving the events back and a folder export
// keeping them. Sessions are never started, so each case is the worker
// and every session time its own.

#include "test_condition_recording_support.h"

namespace {

/* A set of one condition on engine speed, keyed key */
ecuio_status_t attach(ecuio_session_t* s, const std::string& key)
{
    return attach_json(s, set_of(on_rpm(key, R"("above":2000)")));
}

uint32_t generation_of(ecuio_session_t* s)
{
    ecuio_condition_state_t state{};
    uint32_t count = 0;
    EXPECT_EQ(ecuio_session_get_condition_states(s, &state, 1, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u);
    return state.generation;
}

/* frames.csv's host_us by each row's session time */
std::map<uint64_t, uint64_t> frame_hosts(const fs::path& folder)
{
    std::map<uint64_t, uint64_t> out;
    for (const std::string& l : lines_of(folder / "frames.csv")) {
        const size_t a = l.find(',');
        const size_t b = l.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos ||
            l.compare(0, a, "host_us") == 0) {
            continue;
        }
        out[std::stoull(l.substr(a + 1, b - a - 1))] =
            std::stoull(l.substr(0, a));
    }
    return out;
}

/* An event's flags in a cond line's words */
std::string flag_words(uint32_t flags)
{
    std::string w;
    if ((flags & ECUIO_CONDITION_FIRED) != 0) {
        w += "fired";
    }
    if ((flags & ECUIO_CONDITION_STALE) != 0) {
        w += w.empty() ? "stale" : " stale";
    }
    if ((flags & ECUIO_CONDITION_HELD) != 0) {
        w += w.empty() ? "held" : " held";
    }
    return w;
}

std::string number(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

/* ecuio_session_test_before_record_attach for a scope */
struct BeforeAttach {
    BeforeAttach(void (*hook)(void*), void* user)
    {
        ecuio_session_test_before_record_attach(hook, user);
    }
    ~BeforeAttach()
    {
        ecuio_session_test_before_record_attach(nullptr, nullptr);
    }
    BeforeAttach(const BeforeAttach&) = delete;
    BeforeAttach& operator=(const BeforeAttach&) = delete;
};

/* ecuio_session_test_after_dispatch_takes for a scope */
struct AfterTakes {
    AfterTakes(void (*hook)(void*), void* user)
    {
        ecuio_session_test_after_dispatch_takes(hook, user);
    }
    ~AfterTakes() { ecuio_session_test_after_dispatch_takes(nullptr, nullptr); }
    AfterTakes(const AfterTakes&) = delete;
    AfterTakes& operator=(const AfterTakes&) = delete;
};

/* A set attached from another thread while the recorder attaches */
struct Rival {
    ecuio_session_t* session = nullptr;
    std::thread thread;
    std::atomic<int> rc{-1};
    std::atomic<bool> returned_in_the_hook{false};
};

void attach_from_a_rival(void* user)
{
    Rival* r = static_cast<Rival*>(user);
    r->thread = std::thread([r] { r->rc.store(attach(r->session, "b")); });
    /* Long enough for an attach that doesn't wait to return many times
     * over; one that waits for the link's mutex is still waiting. A check
     * that nothing happens can't poll: under load the mutant may pass. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r->returned_in_the_hook.store(r->rc.load() != -1);
}

} // namespace

TEST(ConditionRecordingTest, ASetAttachedDuringARecordersAttachIsRefused)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(attach(s.s, "a"), ECUIO_OK);
    ASSERT_EQ(generation_of(s.s), 1u);

    Root root;
    ecuio_recorder_t* recorder = open_recorder(root);
    ASSERT_NE(recorder, nullptr);
    Rival rival;
    rival.session = s.s;
    {
        BeforeAttach hook(attach_from_a_rival, &rival);
        EXPECT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    rival.thread.join();
    EXPECT_FALSE(rival.returned_in_the_hook.load())
        << "the set's attach didn't wait for the recorder's";
    EXPECT_EQ(rival.rc.load(), ECUIO_ERR_IN_USE)
        << "a set changed under an attaching recorder";
    EXPECT_EQ(generation_of(s.s), 1u)
        << "the session judges another set than the recording's";
    EXPECT_EQ(ecuio_recorder_close(recorder, "done"), ECUIO_OK);
}

TEST(ConditionRecordingTest, ARecordingHoldsItsSetAndAnEventLineForEachChange)
{
    const std::string json = set_of(on_rpm("high", R"("above":2000)") + "," +
                                    on_rpm("low", R"("below":1500)"));
    Recorded r(json);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    engine_speed(r.session.s, 1000000, 1000);
    engine_speed(r.session.s, 2000000, 3000);
    engine_speed(r.session.s, 3000000, 1000);
    r.close();

    EXPECT_EQ(read_bytes(r.folder / "conditions.json"), json)
        << "the copy is the set's bytes";
    const std::string line = "conditions  conditions.json," +
                             ecuio_sha256::hex_of(json.data(), json.size());
    const std::vector<std::string> log = lines_of(r.folder / "events.log");
    size_t at = 0;
    size_t flags_at = 0;
    for (size_t i = 0; i < log.size(); ++i) {
        at = log[i] == line ? i : at;
        flags_at = log[i].rfind("frame flags  ", 0) == 0 ? i : flags_at;
    }
    EXPECT_NE(at, 0u) << "no line names the copy and its hash";
    EXPECT_LT(at, flags_at) << "the conditions line is a header line";

    /* Each event one line, in the callback's order, its fields exact */
    const std::vector<Cond> lines = cond_lines(r.folder);
    const std::map<uint64_t, uint64_t> hosts = frame_hosts(r.folder);
    ASSERT_EQ(lines.size(), r.events.all.size());
    ASSERT_EQ(lines.size(), 6u) << "each frame changes both conditions";
    for (size_t i = 0; i < lines.size(); ++i) {
        const ecuio_condition_event_t& e = r.events.all[i];
        const Cond& c = lines[i];
        EXPECT_EQ(c.key, e.condition == 0 ? "high" : "low") << i;
        EXPECT_EQ(c.state, e.state == ECUIO_CONDITION_ON ? "on" : "off") << i;
        EXPECT_EQ(c.flags, flag_words(e.flags)) << i;
        EXPECT_EQ(c.value, number(e.value)) << i;
        EXPECT_EQ(c.term, e.term) << i;
        EXPECT_EQ(c.t_us, e.timestamp_us) << i;
        ASSERT_EQ(hosts.count(e.timestamp_us), 1u) << i;
        EXPECT_EQ(c.host_us, hosts.at(e.timestamp_us))
            << "an event at its frame's time takes the frame's host_us";
    }
    EXPECT_EQ(lines[2].key + lines[2].state + lines[2].flags, "highonfired");
    EXPECT_FALSE(fs::exists(r.folder / "markers.csv"))
        << "no condition has mark";
}

TEST(ConditionRecordingTest, ASessionWithNoSetRecordsNoConditions)
{
    for (const bool removed : {false, true}) {
        Recorded r(removed ? set_of(on_rpm("high", R"("above":2000)")) : "");
        ASSERT_NE(r.recorder, nullptr);
        if (removed) {
            ASSERT_EQ(
                ecuio_session_set_conditions(r.session.s, nullptr, nullptr),
                ECUIO_OK);
        }
        r.record();
        engine_speed(r.session.s, 1000000, 3000);
        r.close();
        EXPECT_FALSE(fs::exists(r.folder / "conditions.json")) << removed;
        for (const std::string& l : lines_of(r.folder / "events.log")) {
            EXPECT_NE(l.rfind("conditions  ", 0), 0u) << removed;
        }
        EXPECT_TRUE(kind_lines(r.folder, "cond").empty()) << removed;
    }
}

namespace {

/* Attaches set b and a recorder from inside a frame that took set a */
struct Overtaken {
    ecuio_session_t* session = nullptr;
    ecuio_recorder_t* recorder = nullptr;
    bool done = false;
    ecuio_status_t set_rc = ECUIO_OK;
    ecuio_status_t record_rc = ECUIO_OK;
};

void attach_b_and_a_recorder(void* user)
{
    Overtaken* o = static_cast<Overtaken*>(user);
    if (o->done) {
        return;
    }
    o->done = true;
    o->set_rc = attach(o->session, "b");
    o->record_rc = ecuio_session_record(o->session, o->recorder);
}

} // namespace

TEST(ConditionRecordingTest,
     AFrameJudgedByTheSetBeforeWritesNothingIntoTheRecording)
{
    Recorded r(set_of(on_rpm("a", R"("above":2000)")));
    ASSERT_NE(r.recorder, nullptr);
    Overtaken o;
    o.session = r.session.s;
    o.recorder = r.recorder;
    {
        AfterTakes hook(attach_b_and_a_recorder, &o);
        engine_speed(r.session.s, 1000000, 3000);
    }
    ASSERT_TRUE(o.done);
    ASSERT_EQ(o.set_rc, ECUIO_OK);
    ASSERT_EQ(o.record_rc, ECUIO_OK);
    ASSERT_EQ(r.events.all.size(), 1u) << "set a judged the frame";
    EXPECT_EQ(r.events.all[0].generation, 1u);
    engine_speed(r.session.s, 2000000, 3000);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    ASSERT_EQ(lines.size(), 1u)
        << "only the frame set b judged is in b's recording";
    EXPECT_EQ(lines[0].key, "b");
    EXPECT_EQ(lines[0].t_us, 2000000u);
    EXPECT_EQ(lines[0].flags, "fired");
}

TEST(ConditionRecordingTest, AHoldsEndIsWrittenAtItsOwnTime)
{
    Recorded r(set_of(on_rpm("held", R"("above":2000)", R"(,"for":0.1)")));
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    engine_speed(r.session.s, 10000000, 3000);
    /* The frames' host gap past their session gap, so the line's time is
     * the frame's moved back, not the clamp at the frame before */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    engine_speed(r.session.s, 10250000, 3000);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    const std::map<uint64_t, uint64_t> hosts = frame_hosts(r.folder);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].state, "off") << "the first judgment, mid-hold";
    EXPECT_EQ(lines[1].state + lines[1].flags, "onfired");
    EXPECT_EQ(lines[1].t_us, 10100000u) << "the hold's own end";
    ASSERT_EQ(hosts.count(10250000), 1u);
    ASSERT_GT(hosts.at(10250000), 150000u);
    EXPECT_EQ(lines[1].host_us, hosts.at(10250000) - 150000u)
        << "the frame that settled it, less the 150 ms between";
}

TEST(ConditionRecordingTest,
     AnEventFasterThanRealTimeIsNeverBeforeTheFrameBefore)
{
    Recorded r(set_of(on_rpm("high", R"("above":2000)")));
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    /* Host time past the attach, so the frame's time can't read 0 */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine_speed(r.session.s, 1000000, 3000);
    /* A minute of session time at once, as a replay at max: the stale
     * event at 4 s lies far further back than the host time since */
    engine_speed(r.session.s, 61000000, 3000);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    const std::map<uint64_t, uint64_t> hosts = frame_hosts(r.folder);
    ASSERT_EQ(hosts.count(1000000), 1u);
    ASSERT_GE(hosts.at(1000000), 50000u);
    ASSERT_EQ(lines.size(), 3u) << "fired, stale, then judged again";
    EXPECT_EQ(lines[1].t_us, 4000000u) << "stale at the sample's bound";
    EXPECT_NE(lines[1].flags.find("stale"), std::string::npos);
    EXPECT_EQ(lines[1].host_us, hosts.at(1000000))
        << "no earlier than the frame before it, not 0";
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_GE(lines[i].host_us, lines[i - 1].host_us) << i;
    }
}

TEST(ConditionRecordingTest, ARecordingBegunMidSessionStartsFromTheStatesHeld)
{
    Recorded r(set_of(on_rpm("high", R"("above":2000)") + "," +
                      on_rpm("top", R"("above":5000)") + "," +
                      on_rpm("low", R"("below":1500)")));
    ASSERT_NE(r.recorder, nullptr);
    engine_speed(r.session.s, 1000000, 3000);
    ecuio_condition_state_t before[3]{};
    uint32_t count = 0;
    ASSERT_EQ(
        ecuio_session_get_condition_states(r.session.s, before, 3, &count),
        ECUIO_OK);
    ASSERT_EQ(before[0].state, ECUIO_CONDITION_ON);
    r.record();
    engine_speed(r.session.s, 2000000, 3100);
    engine_speed(r.session.s, 3000000, 1000);
    engine_speed(r.session.s, 4000000, 3200);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    const std::map<uint64_t, uint64_t> hosts = frame_hosts(r.folder);
    ASSERT_EQ(lines.size(), 5u)
        << "one held line, then two changes at each later frame";
    EXPECT_EQ(lines[0].key + "," + lines[0].state + "," + lines[0].flags,
              "high,on,held")
        << "the state at the first frame, once";
    EXPECT_EQ(lines[0].value, number(before[0].value));
    EXPECT_EQ(lines[0].t_us, 2000000u);
    EXPECT_EQ(lines[0].host_us, hosts.at(2000000));
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].flags.find("held"), std::string::npos) << i;
        EXPECT_GE(lines[i].t_us, 3000000u) << "the frame at 2 s changed none";
    }
}

TEST(ConditionRecordingTest, AHeldStateStandsBeforeADeadlineItsFrameSettles)
{
    Recorded r(
        set_of(on_rpm("high", R"("above":2000)", R"(,"clear_for":0.1)")));
    ASSERT_NE(r.recorder, nullptr);
    engine_speed(r.session.s, 1000000, 3000);
    engine_speed(r.session.s, 1500000, 1000); /* the clear's hold begins */
    r.record();
    engine_speed(r.session.s, 2000000, 1000); /* it ended at 1.6 s */
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].state + lines[0].flags, "onheld");
    EXPECT_EQ(lines[1].state, "off");
    EXPECT_EQ(lines[1].t_us, 1600000u);
    EXPECT_LE(lines[0].t_us, lines[1].t_us)
        << "the held state reads as standing before the change after it";
}
