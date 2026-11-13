// File/layer: tests/test_condition_recording.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ConditionRecordingTest (test_core)
//
// A session's conditions and its recording (ADR-0063 section 8): the set
// copied beside the recording, a cond line for each event, the states held
// when a recording begins mid-session, the carry past a full ring, the
// markers, and a set that never changes under a recorder that is
// attaching or attached. Sessions are never started, so each case is the
// worker and every session time its own.

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/recording.h>
#include <ecuio/signal_sets.h>
#include "session.h" /* the record link, for a link transition */
#include "sha256.h"
#include "temp_name.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/* test_core.cpp's: operator new's calls on this thread */
unsigned long news_on_this_thread();

namespace {

namespace fs = std::filesystem;

struct Session {
    ecuio_session_t* s = nullptr;
    explicit Session(const char* uri)
    {
        EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Session() { ecuio_session_destroy(s); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

/* A set file of the conditions' JSON */
std::string set_of(const std::string& conditions)
{
    return R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
}

/* One condition on engine speed: its key, its test and anything after */
std::string on_rpm(const std::string& key, const std::string& test,
                   const std::string& rest = "")
{
    return R"({"key":")" + key + R"(","name":"N )" + key +
           R"(","when":{"signal":["obd2.rpm"],"unit":"RPM",)" + test + "}" +
           rest + "}";
}

ecuio_status_t attach_json(ecuio_session_t* s, const std::string& json)
{
    ecuio_condition_set_t* set = nullptr;
    EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_status_t st = ecuio_session_set_conditions(s, set, nullptr);
    ecuio_condition_set_free(set);
    return st;
}

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

/* Engine speed (PID 0C, a quarter of a turn per count) on 0x7E8 at a session
 * time, dispatched as the worker would */
void engine_speed(ecuio_session_t* s, uint64_t t_us, unsigned turns)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    const unsigned raw = turns * 4;
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = t_us;
    f.data[0] = 0x41;
    f.data[1] = 0x0C;
    f.data[2] = static_cast<uint8_t>(raw >> 8);
    f.data[3] = static_cast<uint8_t>(raw & 0xFF);
    f.data_length = 4;
    ecuio_session_dispatch_callbacks(s, &f);
}

/* Throttle position (PID 11, a 255th of the travel per count) likewise */
void throttle(ecuio_session_t* s, uint64_t t_us, uint8_t a)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = t_us;
    f.data[0] = 0x41;
    f.data[1] = 0x11;
    f.data[2] = a;
    f.data_length = 3;
    ecuio_session_dispatch_callbacks(s, &f);
}

/* The session's events, as its conditions callback gave them */
struct Events {
    std::mutex m;
    std::vector<ecuio_condition_event_t> all;
    static void take(const ecuio_condition_event_t* events, uint32_t count,
                     void* user)
    {
        Events* self = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(self->m);
        self->all.insert(self->all.end(), events, events + count);
    }
};

/* A scratch folder of the case's own, removed at the end */
struct Root {
    fs::path path;
    Root()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-condition-recording");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~Root()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_u8string(); }
};

ecuio_recorder_t* open_recorder(const Root& root, uint32_t ring_bytes = 0)
{
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    o.ring_bytes = ring_bytes;
    ecuio_recorder_t* recorder = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
        << ecuio_last_error_detail();
    return recorder;
}

fs::path folder_of(const ecuio_recorder_t* recorder)
{
    char buf[1024];
    uint32_t len = 0;
    EXPECT_EQ(ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len),
              ECUIO_OK);
    return fs::u8path(buf);
}

/* A file's bytes as they are */
std::string read_bytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* A text file's lines, carriage returns dropped */
std::vector<std::string> lines_of(const fs::path& p)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(read_bytes(p));
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

/* events.log's lines of a kind ("cond", "drop"): what follows the kind */
std::vector<std::string> kind_lines(const fs::path& folder, const char* kind)
{
    const std::string key = std::string("  ") + kind + "  ";
    std::vector<std::string> out;
    for (const std::string& l : lines_of(folder / "events.log")) {
        const size_t at = l.find(key);
        if (at != std::string::npos && at < 24) {
            out.push_back(l.substr(at + key.size()));
        }
    }
    return out;
}

/* The drop lines that count lost condition events, in the file's order */
std::vector<std::string> lost_lines(const fs::path& folder)
{
    std::vector<std::string> out;
    for (const std::string& l : kind_lines(folder, "drop")) {
        if (l.rfind("the recording lost ", 0) == 0) {
            out.push_back(l);
        }
    }
    return out;
}

std::string lost_line(uint64_t n)
{
    return "the recording lost " + std::to_string(n) +
           (n == 1 ? " condition event" : " condition events") +
           ": the disk fell behind";
}

/* A cond line's seven fields */
struct Cond {
    std::string key, state, flags, value;
    uint64_t term = 0, host_us = 0, t_us = 0;
};

std::vector<Cond> cond_lines(const fs::path& folder)
{
    std::vector<Cond> out;
    for (const std::string& l : kind_lines(folder, "cond")) {
        std::vector<std::string> f;
        std::stringstream in(l);
        std::string field;
        while (std::getline(in, field, ',')) {
            f.push_back(field);
        }
        EXPECT_EQ(f.size(), 7u) << l;
        if (f.size() != 7) {
            continue;
        }
        out.push_back(Cond{f[0], f[1], f[2], f[3], std::stoull(f[4]),
                           std::stoull(f[5]), std::stoull(f[6])});
    }
    return out;
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

/* A recording made by hand: set attached, recorder attached, closed */
struct Recorded {
    Root root;
    Session session{"mock://x?profile=obd2"};
    Events events;
    ecuio_recorder_t* recorder = nullptr;
    fs::path folder;

    explicit Recorded(const std::string& json, uint32_t ring_bytes = 0)
    {
        EXPECT_EQ(ecuio_register_conditions_callback(session.s, Events::take,
                                                     &events),
                  ECUIO_OK);
        if (!json.empty()) {
            EXPECT_EQ(attach_json(session.s, json), ECUIO_OK)
                << ecuio_last_error_detail();
        }
        recorder = open_recorder(root, ring_bytes);
        if (recorder != nullptr) {
            folder = folder_of(recorder);
        }
    }
    void record()
    {
        ASSERT_EQ(ecuio_session_record(session.s, recorder), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    /* Until the writer has drained what the ring held when called: two
     * wakes, since a wake counts itself before it reads the hold, and the
     * second's loop starts after the first's drain has ended */
    void drained() const
    {
        const uint64_t from = ecuio_recorder_test_wakes(recorder);
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ecuio_recorder_test_wakes(recorder) < from + 2) {
            if (std::chrono::steady_clock::now() >= give_up) {
                ADD_FAILURE() << "the writer never woke twice";
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ecuio_recorder_state_t state() const
    {
        ecuio_recorder_state_t st{};
        EXPECT_EQ(ecuio_recorder_get_state(recorder, &st), ECUIO_OK);
        return st;
    }
    void close()
    {
        if (recorder != nullptr) {
            EXPECT_EQ(ecuio_recorder_close(recorder, "done"), ECUIO_OK);
            recorder = nullptr;
        }
    }
    ~Recorded() { close(); }
};

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
    /* The recording's clock past the hold's distance from its frame, so
     * the line's time is the frame's moved back, not the clamp at zero */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    engine_speed(r.session.s, 10000000, 3000);
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

TEST(ConditionRecordingTest, AFullRingCarriesEveryEvent)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; ++i) {
        engine_speed(r.session.s, 1000000 + i * 10000u,
                     i % 2 == 0 ? 3000 : 1000);
    }
    ASSERT_GT(r.state().records_dropped, 0u) << "the ring filled";
    ecuio_recorder_test_hold(r.recorder, 0);
    /* The writer drains the ring; then a frame takes the carry in ahead */
    r.drained();
    engine_speed(r.session.s, 5000000, 3000); /* a change: its event too */
    const uint64_t dropped = r.state().records_dropped;
    EXPECT_EQ(r.state().conditions_lost, 0u);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    ASSERT_EQ(lines.size(), r.events.all.size())
        << "every event written though " << dropped << " frames were not";
    for (size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].t_us, r.events.all[i].timestamp_us) << i;
    }
    EXPECT_TRUE(kind_lines(r.folder, "drop").size() >= 1u)
        << "the frames dropped are said";
}

TEST(ConditionRecordingTest, ChangesPastTheCarryAreCountedLost)
{
    /* The carry and its count go in through the ring, or the close writes
     * them with the writer still held */
    for (const bool through_the_ring : {true, false}) {
        SCOPED_TRACE(through_the_ring ? "through the ring" : "at the close");
        Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
        ASSERT_NE(r.recorder, nullptr);
        r.record();
        ecuio_recorder_test_hold(r.recorder, 1);
        constexpr int kFrames = 600;
        for (int i = 0; i < kFrames; ++i) {
            engine_speed(r.session.s, 1000000 + i * 10000u,
                         i % 2 == 0 ? 3000 : 1000);
        }
        const uint64_t lost = r.state().conditions_lost;
        EXPECT_GT(lost, 0u);
        if (through_the_ring) {
            /* A drained ring takes what fits of the carry, 40 bytes per
             * event after a record's head: four drains empty a full one */
            ecuio_recorder_test_hold(r.recorder, 0);
            for (int i = 0; i < 4; ++i) {
                r.drained();
                engine_speed(r.session.s, 8000000 + i * 10000u,
                             i % 2 == 0 ? 3000 : 1000);
            }
            r.drained();
            EXPECT_EQ(r.state().conditions_lost, lost) << "nothing more lost";
        }
        r.close();

        const std::vector<Cond> lines = cond_lines(r.folder);
        EXPECT_EQ(lines.size() + lost, r.events.all.size());
        EXPECT_GE(lines.size(), 256u) << "the carry, and what the ring held";
        EXPECT_EQ(lost_lines(r.folder),
                  std::vector<std::string>{lost_line(lost)});
        size_t at = 0;
        for (const Cond& c : lines) {
            while (at < r.events.all.size() &&
                   r.events.all[at].timestamp_us != c.t_us) {
                ++at;
            }
            ASSERT_LT(at, r.events.all.size())
                << "a line at " << c.t_us
                << " the session never gave, or out of order";
        }
    }
}

TEST(ConditionRecordingTest, EachRunOfLostEventsHasADropLineOfItsOwn)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    uint64_t t = 1000000;
    bool high = false;
    const auto flood = [&](int frames) {
        for (int i = 0; i < frames; ++i, t += 10000) {
            high = !high;
            engine_speed(r.session.s, t, high ? 3000 : 1000);
        }
    };
    /* A run, a part of the carry taken in, a second run, then the rest */
    ecuio_recorder_test_hold(r.recorder, 1);
    flood(600);
    const uint64_t first = r.state().conditions_lost;
    ASSERT_GT(first, 0u);
    ecuio_recorder_test_hold(r.recorder, 0);
    r.drained();
    flood(1);
    ecuio_recorder_test_hold(r.recorder, 1);
    flood(600);
    const uint64_t second = r.state().conditions_lost - first;
    ASSERT_GT(second, 0u);
    ecuio_recorder_test_hold(r.recorder, 0);
    for (int i = 0; i < 4; ++i) {
        r.drained();
        flood(1);
    }
    r.close();

    EXPECT_EQ(lost_lines(r.folder),
              (std::vector<std::string>{lost_line(first), lost_line(second)}));
    EXPECT_EQ(cond_lines(r.folder).size() + first + second,
              r.events.all.size());
}

TEST(ConditionRecordingTest, ALinkTransitionFollowsTheEventsCarriedBeforeIt)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    for (int i = 0; i < 200; ++i) {
        engine_speed(r.session.s, 1000000 + i * 10000u,
                     i % 2 == 0 ? 3000 : 1000);
    }
    ecuio_recorder_test_hold(r.recorder, 0);
    r.drained();
    /* The worker's next record is a link transition, after them all */
    ecuio_record_link_state(r.session.s->record_link.get(),
                            ECUIO_ERR_VEHICLE_TIMEOUT, 0);
    r.close();

    size_t last_cond = 0;
    size_t link = 0;
    const std::vector<std::string> log = lines_of(r.folder / "events.log");
    for (size_t i = 0; i < log.size(); ++i) {
        const size_t at = log[i].find("  cond  ");
        last_cond = at != std::string::npos && at < 24 ? i : last_cond;
        link =
            log[i].find("  link  vehicle not responding") != std::string::npos
                ? i
                : link;
    }
    ASSERT_NE(link, 0u) << "the transition is written";
    EXPECT_EQ(cond_lines(r.folder).size(), r.events.all.size());
    EXPECT_GT(link, last_cond)
        << "a cond line carried from before the transition comes after it";
}

TEST(ConditionRecordingTest, AFiringOfAConditionWithMarkIsAMarker)
{
    Recorded r(set_of(R"({"key":"high","name":"High revs","mark":true,)"
                      R"("when":{"signal":["obd2.rpm"],"unit":"RPM",)"
                      R"("above":2000}},)" +
                      on_rpm("plain", R"("above":2500)")));
    ASSERT_NE(r.recorder, nullptr);
    engine_speed(r.session.s, 1000000, 3000); /* ON before the recording */
    r.record();
    engine_speed(r.session.s, 2000000, 3000); /* its held line */
    engine_speed(r.session.s, 3000000, 1000);
    engine_speed(r.session.s, 4000000, 3000);
    engine_speed(r.session.s, 5000000, 1000);
    engine_speed(r.session.s, 6000000, 3000);
    r.close();

    std::vector<uint64_t> fired;
    for (const Cond& c : cond_lines(r.folder)) {
        if (c.key == "high" && c.flags == "fired") {
            fired.push_back(c.host_us);
        }
    }
    ASSERT_EQ(fired.size(), 2u);
    const std::vector<std::string> markers = lines_of(r.folder / "markers.csv");
    ASSERT_EQ(markers.size(), 3u) << "the header and a row per firing";
    EXPECT_EQ(markers[0], "host_us,label");
    for (size_t i = 0; i < fired.size(); ++i) {
        EXPECT_EQ(markers[i + 1], std::to_string(fired[i]) + ",High revs");
    }
}

TEST(ConditionRecordingTest,
     DispatchWithARecorderAndTheLargestSetAllocatesNothing)
{
    std::string all;
    for (int i = 0; i < static_cast<int>(ECUIO_CONDITION_MAX_CONDITIONS); ++i) {
        const std::string rpm = R"({"signal":["obd2.rpm"],"unit":"RPM",)";
        const std::string four =
            rpm + R"("above":)" + std::to_string(1000 + 50 * i) + "}," + rpm +
            R"("below":9000,"clear":9100},)" +
            R"({"signal":["obd2.throttle"],"unit":"%","above":)" +
            std::to_string(i) + "}," + rpm + R"("above":500})";
        all += std::string(i == 0 ? "" : ",") + R"({"key":"c)" +
               std::to_string(i) + R"(","name":"C)" + std::to_string(i) +
               R"(","mark":true,"when":{"all":[)" + four +
               R"(]},"clear_for":0.5})";
    }
    /* A small ring held: frames dropped, events carried, then lost */
    Recorded r(set_of(all), 4096);
    ASSERT_NE(r.recorder, nullptr);
    /* Most conditions ON before the recording: its first frame holds them */
    for (int f = 0; f < 50; ++f) {
        const uint64_t t = 1000000 + f * 110000u;
        if (f % 2 == 0) {
            engine_speed(r.session.s, t, 5000);
        } else {
            throttle(r.session.s, t, 255);
        }
    }
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    r.events.all.reserve(1u << 20); /* the case's own callback's room */
    const unsigned long before = news_on_this_thread();
    for (int f = 0; f < 2000; ++f) {
        const uint64_t t = 10000000 + f * 110000u;
        if (f % 3 == 2) {
            throttle(r.session.s, t, static_cast<uint8_t>(f * 7 % 256));
        } else {
            engine_speed(r.session.s, t, static_cast<unsigned>(f * 977 % 9000));
        }
    }
    EXPECT_EQ(news_on_this_thread() - before, 0ul);
    EXPECT_GT(r.state().conditions_lost, 0u) << "the carry overflowed";
    r.close();
    bool held = false;
    for (const Cond& c : cond_lines(r.folder)) {
        held = held || c.flags.find("held") != std::string::npos;
    }
    EXPECT_TRUE(held) << "the counted frames began with held states";
}
