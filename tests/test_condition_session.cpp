// File/layer: tests/test_condition_session.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ConditionSessionTest (test_core)
//
// Conditions attached to a session (ADR-0063 sections 3 and 6): the
// attach and its swap, the states, the callback's place after the values
// callback, the pull queue's third kind, link terms. Most cases never
// start the session and feed it frames through
// ecuio_session_dispatch_callbacks and link changes through
// ecuio_session_dispatch_link, so each case is the worker and every time
// is its own; the swap under a running worker, the recorder and the
// replay run the mock for real. A mock session judges no link: the link
// cases set session.h's judges_link as a transport's attach does.

#include "test_condition_session_support.h"

namespace {

/* ecuio_session_test_after_conditions_handover for a scope */
struct AfterHandover {
    AfterHandover(void (*hook)(void*), void* user)
    {
        ecuio_session_test_after_conditions_handover(hook, user);
    }
    ~AfterHandover()
    {
        ecuio_session_test_after_conditions_handover(nullptr, nullptr);
    }
    AfterHandover(const AfterHandover&) = delete;
    AfterHandover& operator=(const AfterHandover&) = delete;
};

} // namespace

TEST(ConditionSessionTest, AnAttachBindsTheSetAndPublishesItsStates)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    uint32_t count = 99;
    EXPECT_EQ(ecuio_session_get_condition_states(s.s, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "no set attached";

    ASSERT_EQ(
        attach(s.s, condition(0, rpm(R"("above":3000)")) + "," +
                        condition(1, R"({"link":"not_ok"})") + "," +
                        condition(2, R"({"check":"gap"})") + "," +
                        condition(3, R"({"signal":["id:1001"],"equals":1})")),
        ECUIO_OK);
    std::vector<ecuio_condition_state_t> st = states_of(s.s);
    ASSERT_EQ(st.size(), size_t{4});
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_OFF);
    EXPECT_EQ(st[0].flags, ECUIO_CONDITION_STALE)
        << "OFF and stale before a frame";
    EXPECT_EQ(st[0].generation, 1u);
    EXPECT_EQ(st[0].since_us, 0u);
    EXPECT_EQ(st[0].reserved[0], 0u);
    EXPECT_EQ(st[0].reserved[1], 0u);
    EXPECT_EQ(st[1].flags, ECUIO_CONDITION_UNAVAILABLE);
    EXPECT_EQ(st[1].why, ECUIO_CONDITION_WHY_NO_LINK);
    EXPECT_EQ(st[2].why, ECUIO_CONDITION_WHY_NO_CHECK);
    EXPECT_EQ(st[3].why, ECUIO_CONDITION_WHY_NO_SIGNAL)
        << "the mock's id, on obd2";

    /* The serial ports' contract: all counted, the first capacity copied */
    ecuio_condition_state_t two[2];
    std::memset(two, 0x5A, sizeof two);
    EXPECT_EQ(ecuio_session_get_condition_states(s.s, two, 1, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(two[0].generation, 1u);
    EXPECT_EQ(two[1].generation, 0x5A5A5A5Au)
        << "past the capacity is untouched";

    /* A frame changes the one it judges */
    engine_speed(s.s, 1, 3500);
    st = states_of(s.s);
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(st[0].flags, 0u);
    EXPECT_EQ(st[0].since_us, kSecond);
    EXPECT_EQ(st[0].fired, 1u);
    EXPECT_EQ(st[0].value, 3500.0);
    EXPECT_EQ(st[1].flags, ECUIO_CONDITION_UNAVAILABLE);

    /* A NULL set takes the conditions away, and is a generation too */
    EXPECT_EQ(ecuio_session_set_conditions(s.s, nullptr, nullptr), ECUIO_OK);
    EXPECT_EQ(states_of(s.s).size(), size_t{0});
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    EXPECT_EQ(states_of(s.s)[0].generation, 3u);

    /* Refusals leave what is attached alone */
    EXPECT_EQ(ecuio_session_set_conditions(nullptr, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_condition_states(nullptr, nullptr, 0, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_condition_states(s.s, nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_condition_states(s.s, nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    const std::string file_json =
        R"({"havne_signals":1,"name":"Other set","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[{"key":"a","name":"A",)"
        R"("read":{"service":"22","number":"D100"},"field":{"byte":0}}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file_json.data(), file_json.size(), &file),
              ECUIO_OK);
    EXPECT_EQ(attach(s.s, condition(0, rpm(R"("above":1)")), file),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("signal file"),
              std::string::npos);
    ecuio_signal_file_free(file);
    EXPECT_EQ(states_of(s.s)[0].generation, 3u);

    /* After the close the states still read; a new set is refused */
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(attach(s.s, condition(0, rpm(R"("above":1)"))),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(states_of(s.s).size(), size_t{1});
    /* A callback isn't taken after the close; taking one away still is */
    Seen seen;
    EXPECT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(ecuio_unregister_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
}

TEST(ConditionSessionTest, AFramesEventsFollowItsValuesInOneCall)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    Seen seen;
    ASSERT_EQ(ecuio_register_frame_callback(s.s, on_frame, &seen), ECUIO_OK);
    ASSERT_EQ(ecuio_register_values_callback(s.s, on_values, &seen), ECUIO_OK);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK)
        << "the same pair twice is one";
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"), R"(,"for":1)") +
                              "," + condition(1, rpm(R"("below":9000)"))),
              ECUIO_OK);
    engine_speed(s.s, 1, 3500);
    EXPECT_EQ(seen.text(), "frame 1000000\nvalues 1000000\n"
                           "events [1000000 g1 c0 off t0 v3500] "
                           "[1000000 g1 c1 on fired t0 v3500]");
    /* A frame with nothing for the conditions: no call */
    reply(s.s, 1.5, 0x0D, 50);
    EXPECT_EQ(seen.text(), "frame 1500000\nvalues 1500000");
    /* A deadline's event comes after the values of the frame that
     * settled it, stamped earlier than them, with that frame's own */
    engine_speed(s.s, 5.5, 9500);
    EXPECT_EQ(seen.text(), "frame 5500000\nvalues 5500000\n"
                           "events [2000000 g1 c0 on fired t0 v3500] "
                           "[4000000 g1 c0 on stale t0 v3500] "
                           "[4000000 g1 c1 on stale t0 v3500] "
                           "[5500000 g1 c0 on t0 v9500] "
                           "[5500000 g1 c1 off t0 v9500]");
    /* A frame that decodes nothing still settles what is due: engine
     * speed's 4.5 s gap bounds its sample at 13.5 s, to 19 s */
    static ecuio_frame_t other;
    std::memset(&other, 0, sizeof other);
    other.arbitration_id = 0x123;
    other.timestamp_us = 19500000;
    other.data_length = 2;
    ecuio_session_dispatch_callbacks(s.s, &other);
    EXPECT_EQ(seen.text(), "frame 19500000\n"
                           "events [19000000 g1 c0 on stale t0 v9500] "
                           "[19000000 g1 c1 off stale t0 v9500]");

    EXPECT_EQ(ecuio_unregister_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    EXPECT_EQ(ecuio_unregister_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK)
        << "idempotent";
    engine_speed(s.s, 20, 100);
    EXPECT_EQ(seen.text(), "frame 20000000\nvalues 20000000");
    EXPECT_EQ(states_of(s.s)[0].state, ECUIO_CONDITION_OFF)
        << "judged with no callback";
    EXPECT_EQ(ecuio_register_conditions_callback(nullptr, on_conditions, &seen),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_register_conditions_callback(s.s, nullptr, &seen),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_unregister_conditions_callback(nullptr, on_conditions, &seen),
        ECUIO_ERR_NULL_ARGUMENT);
}

TEST(ConditionSessionTest, ConditionsAloneMakeTheSessionDecode)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    engine_speed(s.s, 1, 3500);
    const std::vector<ecuio_condition_state_t> st = states_of(s.s);
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_ON)
        << "no values callback, no queue, no recording";
    EXPECT_EQ(st[0].fired, 1u);
}

/* Three attaches with no frame between: the worker takes only the last,
 * and the two it never saw are freed by the calls after them */
TEST(ConditionSessionTest, AttachesBackToBackAreJudgedByTheLast)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":1000)"))), ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":2000)")) + "," +
                              condition(1, rpm(R"("above":2500)"))),
              ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    EXPECT_EQ(states_of(s.s).size(), size_t{1});
    engine_speed(s.s, 1, 2700);
    EXPECT_EQ(seen.text(), "events [1000000 g3 c0 off t0 v2700]");

    /* A set taken by the worker, then replaced twice before its next
     * frame: the taken one judges nothing more */
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":2000)"))), ECUIO_OK);
    engine_speed(s.s, 2, 2700);
    EXPECT_EQ(seen.text(), "events [2000000 g4 c0 on fired t0 v2700]");
    ASSERT_EQ(ecuio_session_set_conditions(s.s, nullptr, nullptr), ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("below":2000)"))), ECUIO_OK);
    ASSERT_EQ(ecuio_session_set_conditions(s.s, nullptr, nullptr), ECUIO_OK);
    engine_speed(s.s, 3, 100);
    EXPECT_EQ(seen.text(), "") << "nothing attached";
    EXPECT_EQ(states_of(s.s).size(), size_t{0});
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("below":2000)"))), ECUIO_OK);
    engine_speed(s.s, 4, 100);
    EXPECT_EQ(seen.text(), "events [4000000 g8 c0 on fired t0 v100]")
        << "every state starts again OFF and stale";
}

/* A set replaced while the worker runs: an event's index is always
 * within the set its generation names */
TEST(ConditionSessionTest, ASetSwappedUnderTheWorkersFramesKeepsItsGenerations)
{
    struct Watch {
        std::atomic<uint32_t> newest{0};
        std::atomic<uint32_t> sets{0};
        std::atomic<uint32_t> threes{0};
        std::atomic<bool> wrong{false};
    } watch;
    const auto on_events = [](const ecuio_condition_event_t* events,
                              uint32_t count, void* user) {
        Watch* w = static_cast<Watch*>(user);
        for (uint32_t i = 0; i < count; ++i) {
            /* Generation g holds one condition when g % 3 == 1, three
             * when g % 3 == 2, and none when it is a removal: a set of
             * three judged as the one before names conditions it lacks */
            const uint32_t g = events[i].generation;
            const uint32_t size = g % 3 == 1 ? 1u : g % 3 == 2 ? 3u : 0u;
            if (events[i].condition >= size || g < w->newest.load()) {
                w->wrong.store(true);
            }
            if (g > w->newest.load()) {
                w->sets.fetch_add(1);
                w->threes.fetch_add(size == 3u ? 1u : 0u);
            }
            w->newest.store(g);
        }
    };
    const std::string below =
        R"({"signal":["id:1001"],"unit":"RPM","below":9000})";
    const std::string three = condition(0, below) + "," + condition(1, below) +
                              "," + condition(2, below);
    Session s("mock://swap");
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_events, &watch),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    uint32_t generation = 0;
    /* Counts, not a time: a slow runner swaps for longer, and the bound
     * only ends a stalled worker's run */
    const uint32_t swaps = 60;
    const uint32_t sets = 8;
    const uint32_t threes = 3;
    const auto enough = [&] {
        return generation >= swaps && watch.sets.load() >= sets &&
               watch.threes.load() >= threes;
    };
    const auto bound =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!enough() && std::chrono::steady_clock::now() < bound) {
        const uint32_t kind = (generation + 1) % 3;
        ASSERT_EQ(kind == 1 ? attach(s.s, condition(0, below))
                  : kind == 2
                      ? attach(s.s, three)
                      : ecuio_session_set_conditions(s.s, nullptr, nullptr),
                  ECUIO_OK);
        ++generation;
        const std::vector<ecuio_condition_state_t> st = states_of(s.s);
        ASSERT_EQ(st.size(), size_t{kind == 1 ? 1u : kind == 2 ? 3u : 0u});
        for (const ecuio_condition_state_t& one : st) {
            ASSERT_EQ(one.generation, generation);
        }
        /* Most swaps land between two of the mock's frames (50 ms apart),
         * some with none between */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(generation % 4 * 20));
    }
    const bool judged = enough();
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_FALSE(watch.wrong.load())
        << "an event named a condition its generation's set lacks, or an "
           "older generation than one before it";
    EXPECT_TRUE(judged) << "after " << generation << " swaps in 30 s the "
                        << "worker had judged " << watch.sets.load()
                        << " sets (" << watch.threes.load() << " of three "
                        << "conditions), wanting " << sets << " (" << threes
                        << "): the worker stalled";
    EXPECT_LE(watch.newest.load(), generation);
}

TEST(ConditionSessionTest, ARecorderAttachedRefusesANewSet)
{
    Root root;
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    ecuio_recorder_t* recorder = open_recorder(root, nullptr);
    ASSERT_NE(recorder, nullptr);
    ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK)
        << "a recording begins with conditions attached";
    EXPECT_EQ(attach(s.s, condition(0, rpm(R"("above":1)"))), ECUIO_ERR_IN_USE);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("recording"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_set_conditions(s.s, nullptr, nullptr),
              ECUIO_ERR_IN_USE);
    engine_speed(s.s, 1, 3500);
    std::vector<ecuio_condition_state_t> st = states_of(s.s);
    ASSERT_EQ(st.size(), size_t{1});
    EXPECT_EQ(st[0].generation, 1u)
        << "the set attached before it is still judged";
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    EXPECT_EQ(attach(s.s, condition(0, rpm(R"("above":1)"))), ECUIO_OK)
        << "once the recording has closed";
    EXPECT_EQ(states_of(s.s)[0].generation, 2u);
}

TEST(ConditionSessionTest, AConditionsQueueTakesEachEvent)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    ecuio_queue_t* none = nullptr;
    EXPECT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_CONDITIONS, 0, &none),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_CONDITIONS,
                               ECUIO_QUEUE_MAX_CONDITIONS + 1, &none),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(s.s, 4, 8, &none), ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_queue_t* q = nullptr;
    ASSERT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_CONDITIONS, 3, &q), ECUIO_OK);
    ecuio_queue_t* frames = nullptr;
    ASSERT_EQ(ecuio_queue_open(s.s, ECUIO_QUEUE_FRAMES, 4, &frames), ECUIO_OK);

    ecuio_condition_event_t got[8];
    uint32_t count = 99;
    uint64_t dropped = 99;
    EXPECT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(dropped, 0u);
    /* The kind is the queue's own */
    ecuio_frame_t* frame_out = nullptr;
    static ecuio_frame_t one_frame;
    frame_out = &one_frame;
    EXPECT_EQ(ecuio_queue_read_frames(q, frame_out, 1, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(frames, got, 8, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(nullptr, got, 8, &count, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(q, nullptr, 8, &count, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(q, got, 8, nullptr, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, nullptr, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_conditions(q, got, 0, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT);

    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    engine_speed(s.s, 1, 100);
    engine_speed(s.s, 2, 3500);
    ASSERT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(event_text(got[0]), "1000000 g1 c0 off t0 v100");
    EXPECT_EQ(event_text(got[1]), "2000000 g1 c0 on fired t0 v3500");
    EXPECT_EQ(got[1].reserved, 0u);

    /* A full queue drops its oldest and counts it */
    for (int i = 0; i < 5; ++i) {
        engine_speed(s.s, 3 + i, i % 2 == 0 ? 100 : 3500);
    }
    ASSERT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(dropped, 2u);
    EXPECT_EQ(event_text(got[0]), "5000000 g1 c0 off t0 v100");
    EXPECT_EQ(event_text(got[2]), "7000000 g1 c0 off t0 v100");

    /* What waits at the close is still read, then the queue has ended */
    engine_speed(s.s, 9, 3500);
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    ASSERT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(ecuio_queue_read_conditions(q, got, 8, &count, &dropped, 0),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_queue_close(q);
    ecuio_queue_close(frames);
}

TEST(ConditionSessionTest, JudgingTheLargestSetAllocatesNothing)
{
    std::string all;
    for (int i = 0; i < static_cast<int>(ECUIO_CONDITION_MAX_CONDITIONS); ++i) {
        const std::string four =
            rpm(R"("above":)" + std::to_string(1000 + 50 * i)) + "," +
            rpm(R"("below":9000,"clear":9100)") + "," +
            R"({"signal":["obd2.throttle"],"unit":"%","above":)" +
            std::to_string(i) + "}," + rpm(R"("differs":1)");
        all += std::string(i == 0 ? "" : ",") +
               condition(i, R"({"all":[)" + four + "]}",
                         R"(,"for":0.5,"clear_for":0.5)");
    }
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    Seen seen;
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
    const auto run = [&](double from) {
        const unsigned long before = news_on_this_thread();
        for (int f = 0; f < 2000; ++f) {
            const double t = from + f * 0.11;
            if (f % 3 == 2) {
                reply(s.s, t, 0x11, static_cast<uint8_t>(f * 7 % 256));
            } else {
                engine_speed(s.s, t, static_cast<unsigned>(f * 977 % 9000));
            }
        }
        return news_on_this_thread() - before;
    };
    EXPECT_EQ(run(1), 0ul);
    EXPECT_GT(events.load(), 1000u) << "the frames cross many levels";
    /* The same after a swap, which the dispatch after it takes */
    const uint32_t first = events.load();
    ASSERT_EQ(attach(s.s, all), ECUIO_OK);
    EXPECT_EQ(run(300), 0ul);
    EXPECT_GT(events.load(), first + 1000u);
    ecuio_queue_close(q);
}

/* A computed signal's term: fed when the computed value is, and stale
 * once a leaf of it stops coming, with no firing on the way back */
TEST(ConditionSessionTest, ATermOnAComputedSignalGoesStaleWithItsLeaf)
{
    const std::string file_json =
        R"({"havne_signals":1,"name":"Condition boost","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"boost","name":"Boost Over Baro","kind":"computed",)"
        R"("expr":"obd2.map - obd2.baro","unit":"kPa"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file_json.data(), file_json.size(), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string profile = ecuio_signal_file_profile(file);
    ASSERT_EQ(ecuio_profile_register(profile.c_str(),
                                     ecuio_signal_file_set(file), nullptr),
              ECUIO_OK);
    {
        Session s(("mock://x?profile=" + profile).c_str());
        ASSERT_NE(s.s, nullptr);
        Seen seen;
        ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
                  ECUIO_OK);
        ASSERT_EQ(attach(s.s,
                         condition(0, R"({"signal":["key:boost"],"unit":"kPa",)"
                                      R"("above":50})"),
                         file),
                  ECUIO_OK);
        ASSERT_EQ(states_of(s.s)[0].flags, ECUIO_CONDITION_STALE)
            << "the key binds through the session's file";
        reply(s.s, 1, 0x33, 100); /* baro alone: no boost yet */
        EXPECT_EQ(seen.text(), "");
        reply(s.s, 1.5, 0x0B, 180);
        EXPECT_EQ(seen.text(), "events [1500000 g1 c0 on fired t0 v80]");
        /* MAP keeps coming, baro doesn't: boost is computed while baro is
         * within its age (to 4 s), so its last value is the frame at 4 s
         * and the term is stale 3 s after that */
        for (const double t : {2.0, 3.0, 4.0, 5.0, 6.0, 7.0}) {
            reply(s.s, t, 0x0B, 180);
        }
        EXPECT_EQ(seen.text(), "");
        reply(s.s, 7.5, 0x0B, 180);
        EXPECT_EQ(seen.text(), "events [7000000 g1 c0 on stale t0 v80]");
        /* Baro again: a value, the stale gone, and no second firing */
        reply(s.s, 8, 0x33, 90);
        EXPECT_EQ(seen.text(), "events [8000000 g1 c0 on t0 v90]");
        EXPECT_EQ(states_of(s.s)[0].fired, 1u);
    }
    EXPECT_EQ(ecuio_profile_unregister(profile.c_str()), ECUIO_OK);
    ecuio_signal_file_free(file);
}

/* Fuel pressure (PID 0A), which the mock under obd2 lists unsupported */
std::string fuel_pressure()
{
    return R"({"signal":["obd2.pid_0a"],"unit":"kPa","above":1})";
}

/* The states once the running session has marked condition 0, within 5 s */
std::vector<ecuio_condition_state_t> marked(ecuio_session_t* s)
{
    std::vector<ecuio_condition_state_t> st;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        st = states_of(s);
        if (st[0].flags == ECUIO_CONDITION_UNAVAILABLE) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return st;
}

bool not_received(const ecuio_condition_state_t& st)
{
    return st.flags == ECUIO_CONDITION_UNAVAILABLE &&
           st.why == ECUIO_CONDITION_WHY_NOT_RECEIVED;
}

/* Timing advance (PID 0E), which the demo car never sends and the test
 * can still feed */
std::string timing_advance()
{
    return R"({"signal":["obd2.pid_0e"],"unit":"\u00b0","above":30})";
}

/* A session with no requests says what its support report disclaims: the
 * mock under obd2 lists fuel pressure unsupported, so once the session
 * runs a condition on it can't be judged and says why. An any whose every
 * term is never received says so too, and reads as its sampled term
 * judges it once one is: engine speed is the demo car's, so the case
 * feeds timing advance, which it never sends. */
TEST(ConditionSessionTest, AConditionOnASignalTheMockDisclaimsSaysSo)
{
    WorkerHold hold;
    Session s("mock://x?profile=obd2");
    WorkerHold::Release unhold{hold};
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(hold.attach(s.s), ECUIO_OK);
    ASSERT_EQ(
        attach(s.s, condition(0, fuel_pressure()) + "," +
                        condition(1, R"({"any":[)" + timing_advance() + "," +
                                         fuel_pressure() + "]}")),
        ECUIO_OK);
    EXPECT_EQ(states_of(s.s)[0].flags, ECUIO_CONDITION_STALE)
        << "nothing marked before the session runs";
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    std::vector<ecuio_condition_state_t> st = marked(s.s);
    EXPECT_TRUE(not_received(st[0]))
        << "flags " << st[0].flags << ", why " << st[0].why;
    EXPECT_TRUE(not_received(st[1])) << "every term of the any";
    ASSERT_TRUE(hold.hold(5000));
    reply(s.s, 1, 0x0E, 140); /* 6 degrees */
    hold.release();
    st = states_of(s.s);
    EXPECT_TRUE(not_received(st[0]));
    EXPECT_EQ(st[1].flags & ECUIO_CONDITION_UNAVAILABLE, 0u)
        << "timing advance sampled: the any is its to judge";
    EXPECT_EQ(st[1].why, ECUIO_CONDITION_WHY_NONE);
}

/* An all with a term on a signal the session doesn't get can't turn ON,
 * whatever its other terms read: marked while engine speed reads below
 * its level (OFF), above it (the join unjudged) and below again. A sample
 * of the disclaimed signal itself, with no event (the all stays OFF),
 * ends it: the worker publishes a first sample whether or not it changes
 * a state. */
TEST(ConditionSessionTest,
     AnAllWithATermNeverSampledSaysSoWhateverItsOthersRead)
{
    WorkerHold hold;
    Session s("mock://x?profile=obd2");
    WorkerHold::Release unhold{hold};
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(hold.attach(s.s), ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, R"({"all":[)" + rpm(R"("above":3000)") +
                                           "," + fuel_pressure() + "]}")),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    EXPECT_TRUE(not_received(marked(s.s)[0]));
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    ASSERT_TRUE(hold.hold(5000));
    engine_speed(s.s, 1, 800);
    const ecuio_condition_state_t off = states_of(s.s)[0];
    engine_speed(s.s, 2, 3500);
    const ecuio_condition_state_t unjudged = states_of(s.s)[0];
    engine_speed(s.s, 3, 800);
    const std::string before = seen.text();
    reply(s.s, 4, 0x0A, 50); /* 150 kPa */
    const std::string at_sample = seen.text();
    hold.release();
    ASSERT_EQ(ecuio_unregister_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    EXPECT_TRUE(not_received(off)) << "judged OFF by engine speed";
    EXPECT_NE(off.since_us, 0u);
    EXPECT_TRUE(not_received(unjudged)) << "engine speed above its level";
    EXPECT_NE(before, "");
    EXPECT_EQ(at_sample, "") << "the sample changed no state";
    const ecuio_condition_state_t sampled = states_of(s.s)[0];
    EXPECT_EQ(sampled.flags & ECUIO_CONDITION_UNAVAILABLE, 0u);
    EXPECT_EQ(sampled.why, ECUIO_CONDITION_WHY_NONE);
}

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

/* Attaches a set whose condition 0 reads fuel pressure, once */
struct AttachFuel {
    ecuio_session_t* s = nullptr;
    ecuio_status_t status = ECUIO_ERR_INVALID_ARGUMENT;
    bool done = false;
    static void run(void* user)
    {
        auto* a = static_cast<AttachFuel*>(user);
        if (!a->done) {
            a->done = true;
            a->status = attach(a->s, condition(0, fuel_pressure()));
        }
    }
};

/* A frame taken by the set before an attach marks nothing in the newer
 * attach's states: engine speed is a first sample for that set's
 * condition 0, and the newer set's condition 0, on fuel pressure, still
 * reads not received. The test's own dispatch, with the worker held, takes
 * the set, then the hook attaches the next. */
TEST(ConditionSessionTest, AFirstSampleUnderTheSetBeforeMarksNothingInTheNext)
{
    WorkerHold hold;
    Session s("mock://x?profile=obd2");
    WorkerHold::Release unhold{hold};
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(hold.attach(s.s), ECUIO_OK);
    ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    ASSERT_TRUE(hold.hold(5000));
    AttachFuel next;
    next.s = s.s;
    {
        AfterTakes hook(&AttachFuel::run, &next);
        engine_speed(s.s, 1, 800);
    }
    hold.release();
    ASSERT_TRUE(next.done);
    ASSERT_EQ(next.status, ECUIO_OK);
    const ecuio_condition_state_t st = marked(s.s)[0];
    EXPECT_TRUE(not_received(st)) << "flags " << st.flags << ", why " << st.why;
}

/* One evaluator, two ways: the mock's worker judging its own frames, and
 * a replay of the recording of them at full speed. The events are equal
 * by condition, state, flags, term, value and time from the first frame;
 * the recording's own read is the third way, with its step. */
TEST(ConditionSessionTest, AReplayOfTheRecordingGivesTheSameEvents)
{
    const std::string conditions =
        condition(0, R"({"signal":["id:1001"],"unit":"RPM","below":500})") +
        "," +
        condition(1, R"({"signal":["id:14001"],"unit":"V","below":13})",
                  R"(,"for":1)") +
        "," +
        condition(2,
                  R"({"any":[{"signal":["id:1001"],"unit":"RPM","above":100},)"
                  R"({"signal":["id:14001"],"unit":"V","above":20}]})") +
        "," +
        condition(3, R"({"signal":["id:1001"],"unit":"RPM","equals":0})",
                  R"(,"for":0.25)");
    struct Run {
        Seen seen;
        std::atomic<uint64_t> first{UINT64_MAX};
        std::atomic<uint64_t> frames{0};
    };
    const auto on_first = [](const ecuio_frame_t* frame, void* user) {
        Run* r = static_cast<Run*>(user);
        uint64_t none = UINT64_MAX;
        r->first.compare_exchange_strong(none, frame->timestamp_us);
        r->frames.fetch_add(1);
    };
    const auto relative = [](Run& r) {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(r.seen.m);
        for (ecuio_condition_event_t e : r.seen.events) {
            e.timestamp_us -= r.first.load();
            e.generation = 0;
            out.push_back(event_text(e));
        }
        return out;
    };

    Root root;
    std::string folder;
    Run live;
    {
        Session s("mock://live");
        ASSERT_NE(s.s, nullptr);
        ASSERT_EQ(ecuio_register_frame_callback(s.s, on_first, &live),
                  ECUIO_OK);
        ASSERT_EQ(
            ecuio_register_conditions_callback(s.s, on_conditions, &live.seen),
            ECUIO_OK);
        ASSERT_EQ(attach(s.s, conditions), ECUIO_OK);
        ecuio_recorder_t* recorder = open_recorder(root, &folder);
        ASSERT_NE(recorder, nullptr);
        ASSERT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
        // Each first judgment and both holds: about 1.25 s of the mock's time
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard<std::mutex> lock(live.seen.m);
                if (live.seen.events.size() >= 6) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
        EXPECT_EQ(ecuio_recorder_close(recorder, "the test ended"), ECUIO_OK);
    }
    const std::vector<std::string> lived = relative(live);
    ASSERT_GE(lived.size(), size_t{6}) << "each first judgment and both holds";

    Run again;
    {
        const std::string uri =
            "replay://" + folder + "?profile=mock&speed=max";
        Session s(uri.c_str());
        ASSERT_NE(s.s, nullptr);
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
        ASSERT_EQ(info.replay_frames, live.frames.load())
            << "the recording holds every frame the worker judged";
        ASSERT_EQ(ecuio_register_frame_callback(s.s, on_first, &again),
                  ECUIO_OK);
        ASSERT_EQ(
            ecuio_register_conditions_callback(s.s, on_conditions, &again.seen),
            ECUIO_OK);
        ASSERT_EQ(attach(s.s, conditions), ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (again.frames.load() < info.replay_frames &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_EQ(again.frames.load(), info.replay_frames);
        EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    EXPECT_EQ(relative(again), lived);
    EXPECT_NE(lived[0].find(" g0 c0 on fired t0 v0"), std::string::npos)
        << lived[0];
}

/* The worker may take a set and judge a frame before the attach has
 * returned: the states that frame changed are the ones read afterward */
TEST(ConditionSessionTest, AFrameJudgedBeforeTheAttachReturnsShowsInTheStates)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    {
        const AfterHandover hook(
            [](void* user) {
                engine_speed(static_cast<ecuio_session_t*>(user), 1, 3500);
            },
            s.s);
        ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)")) + "," +
                                  condition(1, rpm(R"("below":500)"))),
                  ECUIO_OK);
    }
    EXPECT_EQ(seen.text(), "events [1000000 g1 c0 on fired t0 v3500] "
                           "[1000000 g1 c1 off t0 v3500]");
    const std::vector<ecuio_condition_state_t> states = states_of(s.s);
    ASSERT_EQ(states.size(), size_t{2});
    EXPECT_EQ(states[0].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(states[0].flags, 0u);
    EXPECT_EQ(states[0].since_us, kSecond);
    EXPECT_EQ(states[0].fired, 1u);
    EXPECT_EQ(states[0].value, 3500.0);
    EXPECT_EQ(states[0].generation, 1u);
    EXPECT_EQ(states[1].state, ECUIO_CONDITION_OFF);
    EXPECT_EQ(states[1].flags, 0u) << "judged, so no longer stale";
    EXPECT_EQ(states[1].generation, 1u);

    /* The same for a set that replaces one: the frame is judged by the
     * new set, and the states are its own */
    {
        const AfterHandover hook(
            [](void* user) {
                engine_speed(static_cast<ecuio_session_t*>(user), 2, 3500);
            },
            s.s);
        ASSERT_EQ(attach(s.s, condition(0, rpm(R"("above":4000)"))), ECUIO_OK);
    }
    EXPECT_EQ(seen.text(), "events [2000000 g2 c0 off t0 v3500]");
    const std::vector<ecuio_condition_state_t> next = states_of(s.s);
    ASSERT_EQ(next.size(), size_t{1});
    EXPECT_EQ(next[0].state, ECUIO_CONDITION_OFF);
    EXPECT_EQ(next[0].flags, 0u);
    EXPECT_EQ(next[0].since_us, 2 * kSecond);
    EXPECT_EQ(next[0].generation, 2u);
}

/* Two callers attaching at once each get a generation of their own */
TEST(ConditionSessionTest, AttachesFromTwoThreadsEachCountOnce)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    static constexpr int kEach = 200;
    const auto attaches = [&s] {
        for (int i = 0; i < kEach; ++i) {
            EXPECT_EQ(attach(s.s, condition(0, rpm(R"("above":3000)"))),
                      ECUIO_OK);
        }
    };
    std::thread one(attaches);
    std::thread two(attaches);
    one.join();
    two.join();
    const std::vector<ecuio_condition_state_t> states = states_of(s.s);
    ASSERT_EQ(states.size(), size_t{1});
    EXPECT_EQ(states[0].generation, uint32_t{2 * kEach});
    Seen seen;
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, on_conditions, &seen),
              ECUIO_OK);
    engine_speed(s.s, 1, 3500);
    EXPECT_EQ(seen.text(), "events [1000000 g400 c0 on fired t0 v3500]");
}
