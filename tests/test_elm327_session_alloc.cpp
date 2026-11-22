// File/layer: tests/test_elm327_session_alloc.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest, Elm327LogTest (test_elm327)

#include "test_elm327_support.h"

#include "new_counter.h"

/* Out of memory at each of create's allocations in turn, for a session
 * of six modules on the adapter: OK or OUT_OF_MEMORY, nothing thrown,
 * a session handed back only with OK, every module in it, and no
 * refused create's wire log left open. Each create
 * finds the log folder empty: a wire log's open lists it, so logs left
 * there would move every later allocation's place. */
TEST_F(Elm327SessionTest, OutOfMemoryAnywhereInCreateIsAnsweredNotThrown)
{
    SixModules modules;
    LogDir logs("oom-create");
    const auto emptied = [&] {
        for (const std::string& name : logs.names()) {
            fs::remove(logs.path() / name);
        }
    };
    const std::string u = uri(kSixModulesQuery);
    ecuio_session_t* s = nullptr;
    emptied();
    const unsigned long before = new_counter::on_this_thread();
    ASSERT_EQ(ecuio_session_create(u.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    const unsigned long news = new_counter::on_this_thread() - before;
    ecuio_session_destroy(s);
    ASSERT_GT(news, 5u);
    const uint32_t count = 6; /* SixModules, the engine ECU among them */
    const unsigned long handles = open_handles();
    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        s = nullptr;
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        emptied();
        new_counter::fail_new(n);
        try {
            rc = ecuio_session_create(u.c_str(), &s);
        } catch (...) {
            threw = true;
        }
        new_counter::fail_new(0);
        ASSERT_FALSE(threw) << "create threw at allocation " << n;
        if (rc == ECUIO_OK) {
            ASSERT_NE(s, nullptr) << "at " << n;
            ecuio_session_info_t info{};
            EXPECT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
            EXPECT_EQ(info.module_count, count) << "at " << n;
            ecuio_channel_support_t last{};
            EXPECT_EQ(ecuio_session_get_channel_support(s, count - 1, &last),
                      ECUIO_OK)
                << "at " << n;
            ecuio_session_destroy(s);
        } else {
            ++refused;
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(s, nullptr) << "at " << n;
        }
    }
    EXPECT_GT(refused, 0u);
    EXPECT_EQ(open_handles(), handles) << "a refused create left a file open";
}

/* Out of memory at each of a log's opens' allocations in turn: the open
 * catches it and opens no log, and the listing it prunes by is closed
 * whatever the throw, no handle left behind. The folder starts empty: an
 * open that fails after its file is made prunes nothing, so a run's logs
 * would add to the next run's listing */
TEST(Elm327LogTest, OutOfMemoryInAnOpenLeavesNoHandleOpen)
{
    LogDir dir("out-of-memory");
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir.path(), ec)) {
        fs::remove(e.path(), ec);
    }
    for (int i = 0; i < 8; ++i) {
        std::ofstream(dir.path() / ("ecuio-elm327.20261027-17253" +
                                    std::to_string(i) + "-123.log"));
    }
    elm327_log log;
    const unsigned long before = new_counter::on_this_thread();
    elm327_log_open(log);
    const unsigned long news = new_counter::on_this_thread() - before;
    ASSERT_TRUE(elm327_log_is_open(log));
    elm327_log_close(log);
    ASSERT_GT(news, 8u) << "each listed name is a copy";
    const unsigned long handles = open_handles();
    for (unsigned long n = 1; n <= news; ++n) {
        new_counter::fail_new(n);
        elm327_log_open(log);
        new_counter::fail_new(0);
        elm327_log_close(log);
    }
    EXPECT_EQ(open_handles(), handles) << "a listing left open";
}

/* What a frame costs the worker: no operator new between two frames once
 * the session runs, for its frames and its values (ADR-0026 section 3), with
 * the wire log taking every request and reply too */

namespace {

struct WorkerNews {
    static constexpr int kFrom = 8;
    int to = 40;
    std::atomic<int> frames{0};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
    std::atomic<int> batches{0};
};

void worker_news_cb(const ecuio_frame_t*, void* user)
{
    auto* w = static_cast<WorkerNews*>(user);
    const int n = ++w->frames;
    if (n == WorkerNews::kFrom) {
        w->at_from = new_counter::on_this_thread();
    } else if (n == w->to) {
        w->at_to = new_counter::on_this_thread();
    }
}

void worker_news_values_cb(const ecuio_value_batch_t*, void* user)
{
    ++static_cast<WorkerNews*>(user)->batches;
}

} // namespace

TEST_F(Elm327SessionTest, TheWorkerAllocatesNothingPerFrame)
{
    for (const char* lines : {static_cast<const char*>(nullptr), "1"}) {
        const EnvSet frame_logging("ECUIO_SUR_LOG_FRAMES", lines);
        WorkerNews w;
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri("?probe=0&identify=0").c_str(), &s),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(s, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_values_callback(s, worker_news_values_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
        const bool reached =
            wait_for([&] { return w.frames.load() >= w.to; }, 8000);
        EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
        ecuio_session_destroy(s);
        const char* mode = lines ? "with request lines" : "without";
        ASSERT_TRUE(reached) << w.frames.load() << " frames, " << mode;
        EXPECT_GT(w.batches.load(), 0) << "the values path ran, " << mode;
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << w.to << ", " << mode;
    }
}

/* The same for a monitor's messages, single and multi-frame, with its
 * lines in the wire log or not */
TEST_F(Elm327SessionTest, ListeningAllocatesNothingPerFrame)
{
    for (const char* lines : {static_cast<const char*>(nullptr), "1"}) {
        const EnvSet frame_logging("ECUIO_SUR_LOG_FRAMES", lines);
        WorkerNews w;
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri("?profile=raw").c_str(), &s),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_register_frame_callback(s, worker_news_cb, &w),
                  ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
        ASSERT_TRUE(wait_for(fake_monitoring, 3000));
        std::atomic<bool> done{false};
        std::thread feeder([&] {
            for (int i = 0; !done.load(); ++i) {
                if (i % 2 == 0) {
                    on_bus(0x7E8, {0x41, 0x0C, 0x1A, 0xF8});
                } else {
                    on_bus(0x7E8, kVinReply);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        const bool reached =
            wait_for([&] { return w.frames.load() >= w.to; }, 8000);
        done = true;
        feeder.join();
        EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
        ecuio_session_destroy(s);
        const char* mode = lines ? "with frame lines" : "without";
        ASSERT_TRUE(reached) << w.frames.load() << " frames, " << mode;
        EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
            << "operator new on the worker between frames " << WorkerNews::kFrom
            << " and " << w.to << ", " << mode;
    }
}

namespace {

/* A worker's operator new calls between frame from and frame to, set
 * once the link has come back */
struct LinkNews {
    static constexpr int kFrom = 8;
    std::atomic<int> frames{0};
    std::atomic<int> to{INT32_MAX};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
    std::atomic<bool> reached{false};
    std::atomic<uint32_t> events{0};
};

void link_news_cb(const ecuio_frame_t*, void* user)
{
    auto* w = static_cast<LinkNews*>(user);
    const int n = ++w->frames;
    if (n == LinkNews::kFrom) {
        w->at_from = new_counter::on_this_thread();
    } else if (n == w->to.load()) {
        w->at_to = new_counter::on_this_thread();
        w->reached = true;
    }
}

void link_news_events(const ecuio_condition_event_t*, uint32_t count,
                      void* user)
{
    static_cast<LinkNews*>(user)->events.fetch_add(count);
}

} // namespace

/* The worker judges the link's changes and delivers their events with no
 * operator new, as it does a frame's */
TEST_F(Elm327SessionTest, TheWorkerAllocatesNothingAcrossTheLinkChangesItJudges)
{
    LinkNews w;
    Session s;
    ASSERT_EQ(s.create(uri("?probe=0&identify=0")), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(attach_whens(
        s.s, {R"({"link":"not_ok"})", R"({"link":"timeout"})",
              R"({"all":[{"signal":["obd2.rpm"],"unit":"RPM","above":1},)"
              R"({"link":"not_ok"}]})"}));
    ASSERT_EQ(ecuio_register_frame_callback(s.s, link_news_cb, &w), ECUIO_OK);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, link_news_events, &w),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    ASSERT_TRUE(
        wait_for([&] { return w.frames.load() > LinkNews::kFrom; }, 8000));
    set_knobs([](fake_elm327::knobs& k) { k.no_data = true; });
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000));
    set_knobs([](fake_elm327::knobs& k) { k.no_data = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    w.to = w.frames.load() + 8;
    ASSERT_TRUE(wait_for([&] { return w.reached.load(); }, 8000));
    EXPECT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_GE(w.events.load(), 7u)
        << "the first judgment, then ON and OFF for each condition";
    EXPECT_EQ(w.at_to.load() - w.at_from.load(), 0u)
        << "operator new on the worker across the link's changes";
}
