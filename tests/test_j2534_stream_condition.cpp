// File/layer: tests/test_j2534_stream_condition.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534ConditionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* ---- Link terms (ADR-0063 step 6) ---- */

class J2534ConditionTest : public J2534AdoptionTest
{};

/* Closes and frees its session, so a failed ASSERT leaves no worker
 * calling into the case's own objects */
struct OwnedSession {
    ecuio_session_t* s = nullptr;
    OwnedSession() = default;
    OwnedSession(const OwnedSession&) = delete;
    OwnedSession& operator=(const OwnedSession&) = delete;
    ~OwnedSession()
    {
        if (s != nullptr) {
            (void)ecuio_session_close(s);
            ecuio_session_destroy(s);
        }
    }
};

/* One condition on this when, attached */
void attach_when(ecuio_session_t* s, const std::string& when)
{
    const std::string json =
        R"({"havne_conditions":1,"conditions":[{"key":"c0","name":"C0",)"
        R"("when":)" +
        when + "}]}";
    ecuio_condition_set_t* set = nullptr;
    ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_set_conditions(s, set, nullptr), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_condition_set_free(set);
}

struct ConditionEvents {
    std::mutex m;
    std::vector<ecuio_condition_event_t> events;

    static void cb(const ecuio_condition_event_t* e, uint32_t n, void* user)
    {
        auto* self = static_cast<ConditionEvents*>(user);
        std::lock_guard<std::mutex> g(self->m);
        self->events.insert(self->events.end(), e, e + n);
    }
    /* The first event of condition 0 into state flagged LINK, at or after
     * after_us, or none */
    bool find(uint32_t state, ecuio_condition_event_t* out,
              uint64_t after_us = 0)
    {
        std::lock_guard<std::mutex> g(m);
        for (const ecuio_condition_event_t& e : events) {
            if (e.condition == 0 && e.state == state &&
                e.timestamp_us >= after_us &&
                (e.flags & ECUIO_CONDITION_LINK) != 0) {
                *out = e;
                return true;
            }
        }
        return false;
    }
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms)
{
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

std::vector<std::string> file_lines(const std::filesystem::path& p)
{
    std::vector<std::string> out;
    std::ifstream in(p, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

/* The bus gone quiet (the fake's replies held back a minute, so none
 * comes back stamped inside the quiet) is VEHICLE_TIMEOUT, on which the
 * worker judges a link term at once; an armed recorder keeps a
 * window around it, from before the timeout to after the link's return,
 * its link lines each ahead of the events they gave. The window starts
 * after the bus's last frame, so the timeout's line is in it only at its
 * own session time. VX mode, so the
 * frames' session time runs on a clock (the default mode's stands
 * still). */
TEST_F(J2534ConditionTest, AQuietBusFiresALinkTermAndAnArmedWindowHoldsIt)
{
    KnobGuard vx("ECUIO_FAKE_VX", "1");
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ConditionEvents ev; /* before the session: the worker calls into them */
    StreamCapture cap;
    OwnedSession owned;
    ASSERT_EQ(ecuio_session_create(uri, &owned.s), ECUIO_OK);
    ecuio_session_t* session = owned.s;
    const std::string json =
        R"({"havne_conditions":1,"conditions":[{"key":"quiet","name":"Quiet",)"
        R"("when":{"link":"timeout"},"record":{"before":0.5,"after":0.5}}]})";
    ecuio_condition_set_t* set = nullptr;
    ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_set_conditions(session, set, nullptr), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_condition_set_free(set);
    ASSERT_EQ(
        ecuio_register_conditions_callback(session, ConditionEvents::cb, &ev),
        ECUIO_OK);
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);

    namespace fs = std::filesystem;
    const char* tmp = std::getenv("TEMP");
    const fs::path root =
        fs::path(tmp != nullptr ? tmp : ".") / temp_name("ecuio-j2534-armed");
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::string root_utf8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_utf8.c_str();
    o.flags = ECUIO_RECORD_ARMED;
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(session, rec), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_until([&] { return cap.count.load() >= 20; }, 5000));

    ecuio_condition_event_t on{};
    {
        KnobGuard quiet("ECUIO_FAKE_REPLY_DELAY_MS", "60000");
        ASSERT_TRUE(
            wait_until([&] { return ev.find(ECUIO_CONDITION_ON, &on); }, 5000))
            << "no link event for the quiet bus";
        int32_t le = 0;
        uint32_t episodes = 0;
        ASSERT_EQ(ecuio_session_get_error_state(session, &le, &episodes),
                  ECUIO_OK);
        EXPECT_EQ(le, ECUIO_ERR_VEHICLE_TIMEOUT);
        EXPECT_EQ(on.code, ECUIO_ERR_VEHICLE_TIMEOUT);
        EXPECT_NE(on.flags & ECUIO_CONDITION_FIRED, 0u);
        EXPECT_TRUE(wait_until(
            [&] {
                ecuio_recorder_state_t st{};
                return ecuio_recorder_get_state(rec, &st) == ECUIO_OK &&
                       st.windows == 1;
            },
            2000))
            << "the window opens at the timeout, with the bus still quiet";
    }
    ecuio_condition_event_t off{};
    ASSERT_TRUE(wait_until(
        [&] { return ev.find(ECUIO_CONDITION_OFF, &off, on.timestamp_us); },
        10000))
        << "the link's return ends the condition";
    EXPECT_EQ(off.code, ECUIO_OK);
    EXPECT_GT(off.timestamp_us, on.timestamp_us);
    const int at_return = cap.count.load();
    ASSERT_TRUE(
        wait_until([&] { return cap.count.load() >= at_return + 20; }, 5000));
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    owned.s = nullptr;
    ecuio_recorder_state_t st{};
    ASSERT_EQ(ecuio_recorder_get_state(rec, &st), ECUIO_OK);
    EXPECT_EQ(st.windows, 1u);
    EXPECT_EQ(ecuio_recorder_close(rec, "done"), ECUIO_OK);

    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        folders.push_back(e.path());
    }
    ASSERT_EQ(folders.size(), 1u);
    std::vector<std::string> order;
    bool body = false;
    std::string trigger;
    for (const std::string& l : file_lines(folders[0] / "events.log")) {
        if (!body && l.rfind("trigger  ", 0) == 0) {
            trigger = l.substr(9);
        }
        body = body || l.empty();
        if (body && l.find("  link  ") != std::string::npos) {
            order.push_back(l.substr(l.find("  link  ") + 8, 7));
        } else if (body && l.find("  cond  quiet,") != std::string::npos) {
            order.push_back(l.find("quiet,on,") != std::string::npos ? "on"
                                                                     : "off");
        }
    }
    EXPECT_EQ(trigger, "quiet,500 ms");
    const std::vector<std::string> want = {"vehicle", "on", "recover", "off"};
    EXPECT_EQ(order, want);
    /* Every row of the window lies from before ahead of the firing to
     * after past its end, at the frames' own session time */
    const std::vector<std::string> rows = file_lines(folders[0] / "frames.csv");
    ASSERT_GT(rows.size(), 2u);
    for (size_t i = 1; i < rows.size(); ++i) {
        const size_t a = rows[i].find(',');
        const size_t b = rows[i].find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) {
            continue;
        }
        const uint64_t t = std::stoull(rows[i].substr(a + 1, b - a - 1));
        EXPECT_GE(t + 500000, on.timestamp_us) << rows[i];
    }
    fs::remove_all(root, ec);
}

/* A set attached while the bus is quiet is judged on the timeout at the
 * worker's next pass. The fake keeps queueing its replies meanwhile
 * (ECUIO_FAKE_READ_STATUS=9), so the reads that end the quiet bring frames
 * stamped inside it, and the return is still stamped the quiet's length
 * after its start on the host's clock. VX mode, so the frames' session
 * time runs on a clock. */
TEST_F(J2534ConditionTest, ASetAttachedOnAQuietBusFiresAndItsEndKeepsItsLength)
{
    KnobGuard vx("ECUIO_FAKE_VX", "1");
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ConditionEvents ev; /* before the session: the worker calls into them */
    StreamCapture cap;
    OwnedSession owned;
    ASSERT_EQ(ecuio_session_create(uri, &owned.s), ECUIO_OK);
    ASSERT_EQ(
        ecuio_register_conditions_callback(owned.s, ConditionEvents::cb, &ev),
        ECUIO_OK);
    ASSERT_EQ(ecuio_register_frame_callback(owned.s, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(owned.s), ECUIO_OK);
    ASSERT_TRUE(wait_until([&] { return cap.count.load() >= 20; }, 5000));

    ecuio_condition_event_t on{};
    std::chrono::steady_clock::time_point on_seen;
    std::chrono::steady_clock::time_point let_go;
    {
        KnobGuard quiet("ECUIO_FAKE_READ_STATUS", "9");
        ASSERT_TRUE(wait_until(
            [&] {
                int32_t le = 0;
                uint32_t episodes = 0;
                return ecuio_session_get_error_state(owned.s, &le, &episodes) ==
                           ECUIO_OK &&
                       le == ECUIO_ERR_VEHICLE_TIMEOUT;
            },
            5000))
            << "the bus never read as quiet";
        ASSERT_NO_FATAL_FAILURE(attach_when(owned.s, R"({"link":"timeout"})"));
        ASSERT_TRUE(
            wait_until([&] { return ev.find(ECUIO_CONDITION_ON, &on); }, 2000))
            << "the set waited for a frame the quiet bus never brought";
        on_seen = std::chrono::steady_clock::now();
        EXPECT_EQ(on.code, ECUIO_ERR_VEHICLE_TIMEOUT);
        EXPECT_NE(on.flags & ECUIO_CONDITION_FIRED, 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        let_go = std::chrono::steady_clock::now();
    }
    ecuio_condition_event_t off{};
    ASSERT_TRUE(wait_until(
        [&] { return ev.find(ECUIO_CONDITION_OFF, &off, on.timestamp_us); },
        10000))
        << "the link's return ends the condition";
    EXPECT_EQ(off.code, ECUIO_OK);
    const uint64_t held = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(let_go - on_seen)
            .count());
    EXPECT_GE(off.timestamp_us - on.timestamp_us, held)
        << "the return is stamped at the held frames' time";
}

} // namespace

#endif // _WIN32
