// File/layer: tests/test_condition_session_link.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ConditionSessionTest (test_core)

#include "test_condition_session_support.h"

/* ---- Link terms (ADR-0063 step 6) ---- */

namespace {

std::string link_term(const char* state)
{
    return std::string(R"({"link":")") + state + R"("})";
}

/* A change of the link as link_set_state makes it: the state published,
 * then judged at a session time */
void link_change(ecuio_session_t* s, double seconds, int32_t state)
{
    s->last_error.store(state);
    ecuio_session_dispatch_link(s, state,
                                static_cast<uint64_t>(seconds * 1e6 + 0.5));
}

const std::string kTimeout = " k" + std::to_string(ECUIO_ERR_VEHICLE_TIMEOUT);

} // namespace

/* A change of the link is judged as a frame is, in a call of its own on
 * the worker: its events flagged LINK, a link term's code the link's
 * state, into the states, the callback and the queue */
TEST(ConditionSessionTest, ALinkChangeIsJudgedAsAFrameIsInACallOfItsOwn)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_frame_callback(s.s, on_frame, &seen), ECUIO_OK);
    ASSERT_EQ(ecuio_register_values_callback(s.s, on_values, &seen), ECUIO_OK);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ecuio_queue_t* q = nullptr;
    ASSERT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_CONDITIONS, 16, &q), ECUIO_OK);
    ASSERT_EQ(
        attach(s.s, condition(0, link_term("timeout")) + "," +
                        condition(1, R"({"all":[)" + rpm(R"("above":3000)") +
                                         "," + link_term("not_ok") + "]}")),
        ECUIO_OK);
    std::vector<ecuio_condition_state_t> st = states_of(s.s);
    ASSERT_EQ(st.size(), size_t{2});
    EXPECT_EQ(st[0].flags, ECUIO_CONDITION_STALE) << "available, not judged";

    /* The set taken at a frame starts from the link's state, OK here */
    engine_speed(s.s, 1, 3500);
    EXPECT_EQ(seen.text(), "frame 1000000\nvalues 1000000\n"
                           "events [1000000 g1 c0 off t0 v0] "
                           "[1000000 g1 c1 off t0 v3500]");
    link_change(s.s, 2, ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(seen.text(),
              "events [2000000 g1 c0 on fired link t0 v0" + kTimeout +
                  "] [2000000 g1 c1 on fired link t1 v0" + kTimeout + "]")
        << "no frame, no values: the events alone";
    st = states_of(s.s);
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(st[0].since_us, 2 * kSecond);
    EXPECT_EQ(st[1].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(st[1].flags, 0u);
    /* The same state again is no change; one stamped earlier than the
     * time reached is judged at that time */
    link_change(s.s, 2.5, ECUIO_ERR_VEHICLE_TIMEOUT);
    link_change(s.s, 1.5, ECUIO_OK);
    EXPECT_EQ(seen.text(), "events [2500000 g1 c0 off link t0 v0] "
                           "[2500000 g1 c1 off link t1 v0]");

    ecuio_condition_event_t got[8];
    uint32_t count = 0;
    uint64_t dropped = 0;
    ASSERT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_OK);
    ASSERT_EQ(count, 6u);
    EXPECT_EQ(event_text(got[2]),
              "2000000 g1 c0 on fired link t0 v0" + kTimeout);
    EXPECT_EQ(got[0].flags & ECUIO_CONDITION_LINK, 0u);
    ecuio_queue_close(q);

    /* A session that judges no link binds no link term */
    Session mock("mock://x?profile=obd2");
    ASSERT_EQ(attach(mock.s, condition(0, link_term("timeout"))), ECUIO_OK);
    EXPECT_EQ(states_of(mock.s)[0].why, ECUIO_CONDITION_WHY_NO_LINK);
    link_change(mock.s, 1, ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(states_of(mock.s)[0].flags, ECUIO_CONDITION_UNAVAILABLE);
}

/* A set attached since the last call is taken by a link change too, and
 * judged on the state it brings; one taken at a frame starts from the
 * state the worker last published */
TEST(ConditionSessionTest, ASetTakenAtALinkChangeOrAFrameStartsFromTheLink)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, link_term("timeout"))), ECUIO_OK);
    link_change(s.s, 1, ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(seen.text(),
              "events [1000000 g1 c0 on fired link t0 v0" + kTimeout + "]");
    ASSERT_EQ(attach(s.s, condition(0, link_term("timeout"))), ECUIO_OK);
    engine_speed(s.s, 2, 1000);
    EXPECT_EQ(seen.text(),
              "events [2000000 g2 c0 on fired t0 v0" + kTimeout + "]")
        << "the timeout still stands";
    /* A set with no link term takes nothing from a link change */
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    link_change(s.s, 3, ECUIO_OK);
    EXPECT_EQ(seen.text(), "");
    engine_speed(s.s, 4, 3500);
    EXPECT_EQ(seen.text(), "events [4000000 g3 c0 on fired t0 v3500]");
}

namespace {

/* events.log's link and cond lines after its header, in order: "link",
 * or "cond on" / "cond off" */
std::vector<std::string> link_and_cond_lines(const std::string& folder)
{
    std::vector<std::string> kinds;
    std::ifstream log(fs::u8path(folder) / "events.log");
    std::string line;
    bool body = false;
    while (std::getline(log, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            body = true;
        } else if (body && line.find("  link  ") != std::string::npos) {
            kinds.emplace_back("link");
        } else if (body && line.find("  cond  ") != std::string::npos) {
            kinds.emplace_back(line.find(",on,") != std::string::npos
                                   ? "cond on"
                                   : "cond off");
        }
    }
    return kinds;
}

} // namespace

/* link_health.h's transitions reach the conditions on the worker that
 * judges them, at the session's time, after the link's own line in a
 * recording */
TEST(ConditionSessionTest, LinkHealthsTransitionsReachTheConditionsAfterItsLine)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(
        attach(s.s, condition(0, link_term("timeout"), R"(,"mark":true)")),
        ECUIO_OK);
    Root root;
    std::string folder;
    ecuio_recorder_t* recorder = open_recorder(root, &folder);
    ASSERT_NE(recorder, nullptr);
    ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK)
        << ecuio_last_error_detail();
    const host_clock::time_point epoch = host_clock::now();
    {
        link_health h(s.s->last_error, s.s->error_count, s.s->record_link.get(),
                      s.s, epoch);
        for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
            link_on_request_timeout(h);
        }
        EXPECT_EQ(s.s->last_error.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
        link_on_read(h, false, true, false);
    }
    const uint64_t since = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - epoch)
            .count());
    {
        std::lock_guard<std::mutex> lock(seen.m);
        ASSERT_EQ(seen.events.size(), 2u);
        const ecuio_condition_event_t& on = seen.events[0];
        EXPECT_EQ(on.state, ECUIO_CONDITION_ON);
        EXPECT_EQ(on.flags, ECUIO_CONDITION_FIRED | ECUIO_CONDITION_LINK);
        EXPECT_EQ(on.code, ECUIO_ERR_VEHICLE_TIMEOUT);
        EXPECT_LE(on.timestamp_us, since) << "the session's time, from epoch";
        EXPECT_EQ(seen.events[1].state, ECUIO_CONDITION_OFF);
        EXPECT_EQ(seen.events[1].code, ECUIO_OK);
        EXPECT_GE(seen.events[1].timestamp_us, on.timestamp_us);
        EXPECT_LE(seen.events[1].timestamp_us, since);
    }
    EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    const std::vector<std::string> want = {"link", "cond on", "link",
                                           "cond off"};
    EXPECT_EQ(link_and_cond_lines(folder), want);
    std::ifstream markers(fs::u8path(folder) / "markers.csv");
    const std::string all((std::istreambuf_iterator<char>(markers)),
                          std::istreambuf_iterator<char>());
    EXPECT_NE(all.find("C0"), std::string::npos) << all;
}

/* Judging link changes allocates nothing: the largest set, half its
 * terms on the link, with frames and link changes in turn and a
 * recording taking their events */
TEST(ConditionSessionTest, JudgingLinkChangesAllocatesNothing)
{
    std::string all;
    for (int i = 0; i < static_cast<int>(ECUIO_CONDITION_MAX_CONDITIONS); ++i) {
        const std::string level =
            rpm(R"("above":)" + std::to_string(1000 + 50 * i));
        const std::string four =
            i % 2 == 0 ? R"({"all":[)" + level + "," + link_term("not_ok") +
                             "," + rpm(R"("below":9000,"clear":9100)") + "," +
                             rpm(R"("differs":1)") + "]}"
                       : R"({"any":[)" + level + "," + link_term("timeout") +
                             "," + link_term("hard_failure") + "," +
                             rpm(R"("below":0)") + "]}";
        all += std::string(i == 0 ? "" : ",") +
               condition(i, four, R"(,"for":0.1,"clear_for":0.1,"mark":true)");
    }
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    const auto count_only = [](const ecuio_condition_event_t*, uint32_t count,
                               void* user) {
        static_cast<std::atomic<uint32_t>*>(user)->fetch_add(count);
    };
    std::atomic<uint32_t> events{0};
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, count_only, &events),
              ECUIO_OK);
    ecuio_queue_t* q = nullptr;
    ASSERT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_CONDITIONS, 64, &q), ECUIO_OK);
    ASSERT_EQ(attach(s.s, all), ECUIO_OK);
    Root root;
    ecuio_recorder_t* recorder = open_recorder(root, nullptr);
    ASSERT_NE(recorder, nullptr);
    ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK);
    const int32_t states[] = {ECUIO_OK, ECUIO_ERR_VEHICLE_TIMEOUT, ECUIO_OK,
                              ECUIO_ERR_TRANSPORT_HARD_FAILURE,
                              ECUIO_ERR_REQUEST_FAILED};
    /* The first frame takes the set and the recorder's held states, both
     * before the count */
    engine_speed(s.s, 0.5, 900);
    link_change(s.s, 0.6, ECUIO_OK);
    const unsigned long before = news_on_this_thread();
    for (int f = 0; f < 2000; ++f) {
        const double t = 1 + f * 0.11;
        if (f % 4 == 3) {
            link_change(s.s, t, states[(f / 4) % 5]);
        } else {
            engine_speed(s.s, t, static_cast<unsigned>(f * 977 % 9000));
        }
    }
    EXPECT_EQ(news_on_this_thread() - before, 0ul);
    EXPECT_GT(events.load(), 1000u) << "the changes cross many levels";
    EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    ecuio_queue_close(q);
}

/* A recording begun while a link condition is ON holds that state at its
 * first judgment, a link change's as a frame's */
TEST(ConditionSessionTest,
     ARecordingBegunWithALinkConditionOnHoldsItAtTheNextChange)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    ASSERT_EQ(attach(s.s, condition(0, link_term("not_ok"))), ECUIO_OK);
    link_change(s.s, 1, ECUIO_ERR_VEHICLE_TIMEOUT);
    ASSERT_EQ(states_of(s.s)[0].state, ECUIO_CONDITION_ON);
    Root root;
    std::string folder;
    ecuio_recorder_t* recorder = open_recorder(root, &folder);
    ASSERT_NE(recorder, nullptr);
    ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK);
    link_change(s.s, 2, ECUIO_OK);
    EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    std::vector<std::string> conds;
    std::ifstream log(fs::u8path(folder) / "events.log");
    std::string line;
    while (std::getline(log, line)) {
        const size_t at = line.find("  cond  ");
        if (at != std::string::npos) {
            const std::string f = line.substr(at + 8);
            conds.push_back(f.substr(0, f.find(',', f.find(',', 3) + 1)));
        }
    }
    const std::vector<std::string> want = {"c0,on,held", "c0,off,"};
    EXPECT_EQ(conds, want);
}

/* A transport that counts its frames on the adapter's clock gives link
 * health its last frame's times: a change is stamped on that clock, the
 * last frame's time plus the host's since it came */
TEST(ConditionSessionTest, ALinkChangeIsStampedOnTheFramesClock)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, link_term("hard_failure"))), ECUIO_OK);
    const host_clock::time_point epoch = host_clock::now();
    const uint64_t frame_us = 50 * kSecond; /* the adapter's clock, ahead */
    const uint64_t frame_host_us = 0;       /* came at the epoch */
    link_health h(s.s->last_error, s.s->error_count, s.s->record_link.get(),
                  s.s, epoch, &frame_us, &frame_host_us);
    link_on_read(h, true, false, false);
    const uint64_t since = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - epoch)
            .count());
    std::lock_guard<std::mutex> lock(seen.m);
    ASSERT_EQ(seen.events.size(), 1u);
    EXPECT_GE(seen.events[0].timestamp_us, frame_us);
    EXPECT_LE(seen.events[0].timestamp_us, frame_us + since);
}

/* A set attached during a fault, with no frame to take it, is judged at
 * the worker's next pass on the state it finds; a set a frame or a change
 * takes leaves nothing waiting */
TEST(ConditionSessionTest, ASetAttachedDuringAFaultIsJudgedAtTheWorkersNextPass)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    const host_clock::time_point epoch = host_clock::now();
    link_health h(s.s->last_error, s.s->error_count, s.s->record_link.get(),
                  s.s, epoch);
    link_on_read(h, true, false, false);
    link_judge_waiting(h);
    EXPECT_EQ(ecuio_session_conditions_waiting(s.s), 0);
    ASSERT_EQ(attach(s.s, condition(0, link_term("hard_failure"))), ECUIO_OK);
    EXPECT_EQ(ecuio_session_conditions_waiting(s.s), 1);
    link_judge_waiting(h);
    EXPECT_EQ(ecuio_session_conditions_waiting(s.s), 0);
    {
        std::lock_guard<std::mutex> lock(seen.m);
        ASSERT_EQ(seen.events.size(), 1u);
        EXPECT_EQ(seen.events[0].state, ECUIO_CONDITION_ON);
        EXPECT_EQ(seen.events[0].flags,
                  ECUIO_CONDITION_FIRED | ECUIO_CONDITION_LINK);
        EXPECT_EQ(seen.events[0].code, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        EXPECT_EQ(seen.events[0].generation, 1u);
    }
    EXPECT_EQ(states_of(s.s)[0].state, ECUIO_CONDITION_ON);
    (void)seen.text();
    link_judge_waiting(h);
    EXPECT_EQ(seen.text(), "") << "nothing waits: no second judgment";

    ASSERT_EQ(attach(s.s, condition(0, link_term("hard_failure"))), ECUIO_OK);
    engine_speed(s.s, 5, 1000);
    EXPECT_EQ(ecuio_session_conditions_waiting(s.s), 0) << "a frame took it";
    ASSERT_EQ(attach(s.s, condition(0, link_term("not_ok"))), ECUIO_OK);
    link_on_read(h, false, true, false);
    EXPECT_EQ(ecuio_session_conditions_waiting(s.s), 0) << "a change took it";
}

/* Through a fault a change is stamped no earlier than the fault's start
 * plus the host's time since, whatever the stamps of frames an adapter
 * held meanwhile; a healthy link's change is on the frames' clock alone */
TEST(ConditionSessionTest, AFaultsEndIsStampedNoEarlierThanItsStartPlusTheHosts)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, link_term("hard_failure"))), ECUIO_OK);
    const host_clock::time_point epoch = host_clock::now();
    uint64_t frame_us = 50 * kSecond;
    uint64_t frame_host_us = 0;
    link_health h(s.s->last_error, s.s->error_count, s.s->record_link.get(),
                  s.s, epoch, &frame_us, &frame_host_us);
    const auto event = [&](size_t i) {
        std::lock_guard<std::mutex> lock(seen.m);
        return i < seen.events.size() ? seen.events[i]
                                      : ecuio_condition_event_t{};
    };
    const auto host_us = [&] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                host_clock::now() - epoch)
                .count());
    };
    link_on_read(h, true, false, false);
    const uint64_t on_host = host_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    /* A frame the adapter held through the fault, stamped near its start */
    frame_us = 50 * kSecond + 1000;
    frame_host_us = host_us();
    link_on_read(h, false, true, false);
    const uint64_t off_host = host_us();
    const ecuio_condition_event_t on = event(0);
    const ecuio_condition_event_t off = event(1);
    ASSERT_EQ(on.state, ECUIO_CONDITION_ON);
    ASSERT_EQ(off.state, ECUIO_CONDITION_OFF);
    EXPECT_GE(off.timestamp_us - on.timestamp_us, frame_host_us - on_host)
        << "the fault lasted the host's time";
    EXPECT_LE(off.timestamp_us - on.timestamp_us, off_host);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    frame_us = off.timestamp_us + 1000;
    frame_host_us = host_us();
    link_on_read(h, true, false, false);
    const ecuio_condition_event_t again = event(2);
    ASSERT_EQ(again.state, ECUIO_CONDITION_ON);
    EXPECT_GE(again.timestamp_us, frame_us);
    EXPECT_LT(again.timestamp_us, frame_us + 100000)
        << "from OK, the frames' clock: no floor from the change before";
}

/* A deadline a link change settles goes in a recording no earlier than
 * the link line it follows, though its own time is before it */
TEST(ConditionSessionTest, ADeadlineALinkChangeSettlesFollowsTheLinksLine)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    s.s->judges_link = true;
    ASSERT_EQ(
        attach(s.s, condition(0, rpm(R"("above":3000)"), R"(,"for":0.02)") +
                        "," + condition(1, link_term("hard_failure"))),
        ECUIO_OK);
    Root root;
    std::string folder;
    ecuio_recorder_t* recorder = open_recorder(root, &folder);
    ASSERT_NE(recorder, nullptr);
    ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK);
    /* The session's clock 1 s ahead of the host's since its epoch */
    const host_clock::time_point epoch = host_clock::now();
    const uint64_t frame_us = kSecond;
    const uint64_t frame_host_us = 0;
    link_health h(s.s->last_error, s.s->error_count, s.s->record_link.get(),
                  s.s, epoch, &frame_us, &frame_host_us);
    engine_speed(s.s, 1, 3500);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    link_on_read(h, true, false, false);
    EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    std::vector<std::pair<double, std::string>> lines;
    std::ifstream log(fs::u8path(folder) / "events.log");
    std::string line;
    bool body = false;
    while (std::getline(log, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            body = true;
            continue;
        }
        /* "<clock>  +<seconds>  <kind>  <text>" */
        const size_t at = line.find("  +");
        const size_t gap =
            at == std::string::npos ? at : line.find("  ", at + 3);
        if (!body || gap == std::string::npos) {
            continue;
        }
        const std::string kind = line.substr(gap + 2, 4);
        if (kind == "link" || kind == "cond") {
            lines.emplace_back(
                std::stod(line.substr(at + 3, gap - at - 3)),
                kind + (line.find("c0,on,") != std::string::npos ? " c0" : ""));
        }
    }
    ASSERT_GE(lines.size(), 2u);
    bool c0 = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_GE(lines[i].first, lines[i - 1].first)
            << lines[i].second << " before the line ahead of it";
        c0 = c0 || lines[i].second == "cond c0";
    }
    EXPECT_TRUE(c0) << "the hold's deadline settled at the change";
}
