// File/layer: tests/test_elm327_session_links.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

namespace {

/* An unpicked session's checks at an interval of the case's own, the
 * shipped one given back after (PickOnlyTest pins that one) */
struct CheckEvery {
    explicit CheckEvery(int64_t ms)
    {
        ecuio_set_unpicked_check_ms(static_cast<uint32_t>(ms));
    }
    ~CheckEvery() { ecuio_set_unpicked_check_ms(0); }
    CheckEvery(const CheckEvery&) = delete;
    CheckEvery& operator=(const CheckEvery&) = delete;
};
constexpr int64_t kCheckMs = 2000;

} // namespace

/* ---- Link terms (ADR-0063 step 6) ---- */

namespace {

/* The conditions callback's events, from the worker */
struct LinkEvents {
    std::mutex m;
    std::vector<ecuio_condition_event_t> events;

    static void cb(const ecuio_condition_event_t* e, uint32_t n, void* user)
    {
        auto* self = static_cast<LinkEvents*>(user);
        std::lock_guard<std::mutex> g(self->m);
        self->events.insert(self->events.end(), e, e + n);
    }
    /* An event of condition c into state, flagged LINK, with code */
    bool has(uint32_t c, uint32_t state, int32_t code)
    {
        std::lock_guard<std::mutex> g(m);
        for (const ecuio_condition_event_t& e : events) {
            if (e.condition == c && e.state == state && e.code == code &&
                (e.flags & ECUIO_CONDITION_LINK) != 0) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

/* An adapter gone silent is a hard failure the worker judges its link
 * terms on at once, and the link's return ends them */
TEST_F(Elm327SessionTest, ASilentAdapterFiresALinkTermAndItsReturnEndsIt)
{
    LinkEvents ev; /* before the session: the worker calls into it */
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C&identify=0&diagnose=0")), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(attach_whens(
        s.s, {R"({"link":"hard_failure"})", R"({"link":"not_ok"})"}));
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, LinkEvents::cb, &ev),
              ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    const ShortTimeouts timeouts(500, 0);
    set_knobs([](fake_elm327::knobs& k) { k.silent = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 4000));
    EXPECT_TRUE(wait_for(
        [&] {
            return ev.has(0, ECUIO_CONDITION_ON,
                          ECUIO_ERR_TRANSPORT_HARD_FAILURE) &&
                   ev.has(1, ECUIO_CONDITION_ON,
                          ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        },
        1000))
        << "judged where the worker published the state";
    ecuio_condition_state_t st[2] = {};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_condition_states(s.s, st, 2, &n), ECUIO_OK);
    EXPECT_EQ(st[0].state, ECUIO_CONDITION_ON);
    EXPECT_EQ(st[0].flags, 0u);
    EXPECT_EQ(st[0].fired, 1u);
    set_knobs([](fake_elm327::knobs& k) { k.silent = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    EXPECT_TRUE(wait_for(
        [&] {
            return ev.has(0, ECUIO_CONDITION_OFF, ECUIO_OK) &&
                   ev.has(1, ECUIO_CONDITION_OFF, ECUIO_OK);
        },
        1000));
    std::lock_guard<std::mutex> g(ev.m);
    for (size_t i = 1; i < ev.events.size(); ++i) {
        EXPECT_GE(ev.events[i].timestamp_us, ev.events[i - 1].timestamp_us)
            << "never back in time, " << i;
    }
}

/* A set attached while the adapter is silent, with no frame to take it,
 * is judged on the hard failure at the worker's next pass */
TEST_F(Elm327SessionTest, ASetAttachedWhileTheAdapterIsSilentReportsTheFault)
{
    LinkEvents ev; /* before the session: the worker calls into it */
    Session s;
    ASSERT_EQ(s.create(uri("?poll=0x0C&identify=0&diagnose=0")), ECUIO_OK);
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, LinkEvents::cb, &ev),
              ECUIO_OK);
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 3000));
    const ShortTimeouts timeouts(500, 0);
    set_knobs([](fake_elm327::knobs& k) { k.silent = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 4000));
    ASSERT_NO_FATAL_FAILURE(attach_whens(s.s, {R"({"link":"hard_failure"})"}));
    EXPECT_TRUE(wait_for(
        [&] {
            return ev.has(0, ECUIO_CONDITION_ON,
                          ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        },
        8000))
        << "judged on the state the worker found, with no frame";
    ecuio_condition_state_t st{};
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_condition_states(s.s, &st, 1, &n), ECUIO_OK);
    EXPECT_EQ(st.state, ECUIO_CONDITION_ON);
    EXPECT_EQ(st.fired, 1u);
    set_knobs([](fake_elm327::knobs& k) { k.silent = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    EXPECT_TRUE(wait_for(
        [&] { return ev.has(0, ECUIO_CONDITION_OFF, ECUIO_OK); }, 1000));
}

/* A listening session's quiet bus is a timeout inside the monitor's own
 * loop, which takes a set attached then */
TEST_F(Elm327SessionTest, ASetAttachedWhileTheMonitorHearsNothingReportsIt)
{
    LinkEvents ev; /* before the session: the worker calls into it */
    Session s;
    ASSERT_NO_FATAL_FAILURE(listen_on(s));
    ASSERT_EQ(ecuio_register_conditions_callback(s.s, LinkEvents::cb, &ev),
              ECUIO_OK);
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 3000));
    ASSERT_NO_FATAL_FAILURE(attach_whens(s.s, {R"({"link":"timeout"})"}));
    EXPECT_TRUE(wait_for(
        [&] {
            return ev.has(0, ECUIO_CONDITION_ON, ECUIO_ERR_VEHICLE_TIMEOUT);
        },
        2000))
        << "judged in the monitor's loop, with no message";
    EXPECT_TRUE(fake_monitoring()) << "the monitor kept running";
    on_bus(0x7E8, {0x41, 0x0C, 0x1A, 0xF8});
    EXPECT_TRUE(wait_for(
        [&] { return ev.has(0, ECUIO_CONDITION_OFF, ECUIO_OK); }, 2000));
}

/* ADR-0065 section 3: a session on a set that derives more requests than
 * a rotation holds says so in its info, asks nothing until a focus
 * picks, then the picks alone, keeps a reply-timing row for each request
 * it has asked, and asks nothing again once the focus ends */
TEST_F(Elm327SessionTest, ASetPastARotationIsAskedOnlyItsPicks)
{
    using car_sized::id_of;
    using car_sized::kMost;
    /* Before the link is ever down, so a check's first interval is it */
    const CheckEvery checks(kCheckMs);
    struct Modules {
        Modules()
        {
            EXPECT_TRUE(fake_ecu::set_module_dids(
                "7E8:1002=07;7E8:1007=2A;7E9:1003=11"));
        }
        ~Modules() { fake_ecu::set_module_dids(nullptr); }
    } modules;
    struct Profile {
        Profile()
        {
            EXPECT_EQ(
                ecuio_profile_register("elm327-car", car_sized::set(), nullptr),
                ECUIO_OK);
        }
        ~Profile()
        {
            EXPECT_EQ(ecuio_profile_unregister("elm327-car"), ECUIO_OK);
        }
    } profile; /* before the session, which holds the set */
    Session s;
    /* The probe stays on: with it a refused request leaves the rotation */
    ASSERT_EQ(s.create(uri("?profile=elm327-car&identify=0&diagnose=0")),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 2u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
    uint32_t unasked = 0;
    ASSERT_EQ(ecuio_session_get_unasked_signals(s.s, nullptr, 0, &unasked),
              ECUIO_OK);
    EXPECT_EQ(unasked, car_sized::kDefs) << "before the start, every signal";

    /* The Service 22 requests the adapter saw */
    const auto asked = [] {
        std::vector<std::string> out;
        for (const std::string& cmd : fake_commands()) {
            if (cmd.size() >= 6 && cmd.compare(0, 2, "22") == 0) {
                out.push_back(cmd.substr(0, 6));
            }
        }
        return out;
    };
    s.start();
    /* Twice the quiet run a link takes to say an ECU isn't answering */
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    EXPECT_TRUE(asked().empty()) << "nothing is asked before a pick";
    EXPECT_EQ(s.state(), ECUIO_OK)
        << "a session asking nothing isn't one an ECU doesn't answer";

    /* Two of the engine ECU's signals and one of the other module's */
    const uint16_t picks[] = {id_of(7), id_of(kMost + 3), id_of(2)};
    ASSERT_EQ(ecuio_session_set_focus(s.s, picks, 3), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_TRUE(wait_for([&] { return asked().size() >= 9; }, 5000));
    std::vector<std::string> seen = asked();
    for (const std::string& r : seen) {
        EXPECT_TRUE(r == "221002" || r == "221007" || r == "221003") << r;
    }
    for (const char* r : {"221002", "221007", "221003"}) {
        EXPECT_NE(std::count(seen.begin(), seen.end(), r), 0) << r;
    }
    const std::vector<uint8_t> engine = {0x62, 0x10, 0x07, 0x2A};
    const std::vector<uint8_t> other = {0x62, 0x10, 0x03, 0x11};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            bool a = false;
            bool b = false;
            for (const Capture::Frame& f : s.cap.frames) {
                a = a || (f.id == 0x7E8u && f.data == engine);
                b = b || (f.id == 0x7E9u && f.data == other);
            }
            return a && b;
        },
        5000))
        << "each module's own reply";
    ASSERT_EQ(ecuio_session_get_unasked_signals(s.s, nullptr, 0, &unasked),
              ECUIO_OK);
    EXPECT_EQ(unasked, car_sized::kDefs - 3);

    /* A row for each request asked, in the order the worker took them
     * (the table's), and none for the 2045 it never asked */
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_timing(s.s, &t, rows.data(),
                                            static_cast<uint32_t>(rows.size()),
                                            &count) == ECUIO_OK &&
                   count == 3 && rows[0].answered > 0 && rows[1].answered > 0 &&
                   rows[2].answered > 0;
        },
        10000))
        << count << " rows";
    EXPECT_EQ(t.rows, 3u);
    EXPECT_EQ(t.untimed, 0u);
    const uint8_t want[3][4] = {
        {0, 0x22, 0x10, 0x02}, {0, 0x22, 0x10, 0x07}, {1, 0x22, 0x10, 0x03}};
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(rows[i].channel, want[i][0]) << i;
        EXPECT_EQ(rows[i].length, 3u) << i;
        EXPECT_EQ(std::memcmp(rows[i].request, &want[i][1], 3), 0) << i;
        EXPECT_EQ(rows[i].refused, 0u) << i;
    }

    /* The focus ended: nothing more, but a request already out */
    ASSERT_EQ(ecuio_session_set_focus(s.s, nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            uint32_t n = 0;
            return ecuio_session_get_unasked_signals(s.s, nullptr, 0, &n) ==
                       ECUIO_OK &&
                   n == car_sized::kDefs;
        },
        5000))
        << "the worker took the focus's end";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint32_t at_end =
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; }),
        at_end)
        << "nothing is asked with no pick";
    EXPECT_EQ(s.state(), ECUIO_OK);

    /* A pick the ECU refuses is dropped, but it was asked: the silence
     * after it counts as any table's does. With the pick taken away
     * nothing is asked, and the timeout it left ends */
    const uint16_t refused = id_of(9);
    ASSERT_EQ(ecuio_session_set_focus(s.s, &refused, 1), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_channel_support_t engine_ecu{};
            return ecuio_session_get_channel_support(s.s, 0, &engine_ecu) ==
                       ECUIO_OK &&
                   engine_ecu.dropped == 1;
        },
        5000))
        << "the refused request left the rotation";
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000))
        << "picks that were all dropped still time out";
    /* A pick exists, so no check is asked, however long the link is
     * down: checks are a session's with nothing picked */
    const uint32_t down =
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; });
    const auto check_by = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kCheckMs * 3 / 2);
    while (std::chrono::steady_clock::now() < check_by) {
        ASSERT_EQ(s.state(), ECUIO_ERR_VEHICLE_TIMEOUT);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(
        on_fake<uint32_t>([](fake_elm327::adapter& a) { return a.requests; }),
        down)
        << "a check went out with a signal picked";
    ASSERT_EQ(ecuio_session_set_focus(s.s, nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 5000))
        << "nothing asked: nothing fails to answer";
}

namespace {

/* The car-sized set under a name of a case's own, registered before its
 * session, which holds the set */
struct CarProfile {
    const char* name;
    explicit CarProfile(const char* n) : name(n)
    {
        EXPECT_EQ(ecuio_profile_register(n, car_sized::set(), nullptr),
                  ECUIO_OK);
    }
    ~CarProfile() { EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK); }
    CarProfile(const CarProfile&) = delete;
    CarProfile& operator=(const CarProfile&) = delete;
};

/* The fake ECU's module table for a case, set before its session */
struct ModuleDids {
    explicit ModuleDids(const char* spec)
    {
        EXPECT_TRUE(fake_ecu::set_module_dids(spec));
    }
    ~ModuleDids() { fake_ecu::set_module_dids(nullptr); }
    ModuleDids(const ModuleDids&) = delete;
    ModuleDids& operator=(const ModuleDids&) = delete;
};

/* Whether both modules' identification and diagnostics have an outcome
 * for every item */
bool items_settled(ecuio_session_t* s)
{
    for (uint32_t c = 0; c < 2; ++c) {
        ecuio_channel_identity_t id{};
        ecuio_channel_diagnostics_t diag{};
        if (ecuio_session_get_channel_identity(s, c, &id) != ECUIO_OK ||
            ecuio_session_get_channel_diagnostics(s, c, &diag) != ECUIO_OK ||
            (id.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
            (diag.flags & ECUIO_DIAG_COMPLETE) == 0) {
            return false;
        }
    }
    return true;
}

/* Whether count() stops changing for quiet_ms, within limit_ms. A
 * recovery's reports read complete before its last request is out (a
 * report keeps its earlier outcomes until an item asked again settles),
 * so "nothing more is asked" is judged by the requests themselves. */
template <typename Count>
bool goes_quiet(Count count, int64_t quiet_ms, int64_t limit_ms)
{
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
    auto last = count();
    auto since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto seen = count();
        const auto now = std::chrono::steady_clock::now();
        if (seen != last) {
            last = seen;
            since = now;
        } else if (now - since >= std::chrono::milliseconds(quiet_ms)) {
            return true;
        }
    }
    return false;
}

/* The requests the fake adapter has taken */
uint32_t adapter_requests()
{
    return on_fake<uint32_t>(
        [](fake_elm327::adapter& a) { return a.requests; });
}

/* Everything it has been sent, requests among it */
uint32_t adapter_commands()
{
    return on_fake<uint32_t>(
        [](fake_elm327::adapter& a) { return a.commands; });
}

} // namespace

/* Such a session with nothing picked still asks identification and
 * diagnostics. An ECU that answers none of it isn't responding, and the
 * link says so for as long as nothing answers: an idle pass never turns
 * that into OK. While it is down the worker asks one request an
 * interval (here kCheckMs), a module in turn, the first a whole interval
 * after the items settled; the first answered ends the timeout, the
 * identification is read after all, and the checks end. A check's
 * request and its reply are in the wire log and the recording as any
 * other's. */
TEST_F(Elm327SessionTest, AnUnpickedSessionOnASilentEcuChecksItAndFindsItAwake)
{
    const CheckEvery checks(kCheckMs);
    const EnvSet frame_logging("ECUIO_SUR_LOG_FRAMES", "1");
    const LogDir logs("unpicked-check");
    const CarProfile profile("elm327-quiet-car");
    /* 0x7E9 is a module that answers the car's first DID; 0x7E8 the
     * engine ECU, which has the VIN */
    const ModuleDids modules("7E9:1000=11");
    const fs::path root =
        fs::temp_directory_path() / temp_name("ecuio-elm327-unpicked");
    std::error_code ec;
    fs::remove_all(root, ec);
    set_knobs([](fake_elm327::knobs& k) { k.no_data = true; });
    Session s;
    ASSERT_EQ(s.create(uri("?profile=elm327-quiet-car")), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.s, &info), ECUIO_OK);
    ASSERT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
    const std::string root8 = root.generic_u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char buf[1024];
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, buf, sizeof buf, &len), ECUIO_OK);
    const fs::path folder = fs::u8path(buf);
    ASSERT_EQ(ecuio_session_record(s.s, rec), ECUIO_OK);
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 10000))
        << "what it asked went unanswered";
    /* Until every item of both modules is settled, unanswered; the
     * state is read all the while, so a flap fails here in its words */
    const auto settle_by =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (!items_settled(s.s)) {
        ASSERT_EQ(s.state(), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "the timeout ended with nothing answered";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto settled_at = std::chrono::steady_clock::now();
    const uint32_t settled = adapter_requests();
    EXPECT_GT(settled, 0u);

    /* The first check, an interval on, and the timeout through it */
    const auto first_by = settled_at + std::chrono::milliseconds(8000);
    while (adapter_requests() == settled) {
        ASSERT_EQ(s.state(), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "an idle pass with nothing picked ended the timeout";
        ASSERT_LT(std::chrono::steady_clock::now(), first_by)
            << "no check within 8 s";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - settled_at);
    /* Half an interval is the bound: this thread may read the clock
     * late; PickOnlyTest pins the interval to the microsecond */
    EXPECT_GE(waited.count(), kCheckMs / 2)
        << "nothing is asked again before an interval has passed";
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckMs / 4));
    EXPECT_EQ(adapter_requests(), settled + 1) << "one request, once";
    EXPECT_EQ(s.state(), ECUIO_ERR_VEHICLE_TIMEOUT) << "its silence changes "
                                                       "nothing";

    /* The ignition on: the next check, the other module's, is answered */
    set_knobs([](fake_elm327::knobs& k) { k.no_data = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 8000))
        << "no check found the ECU awake";
    const std::vector<uint8_t> answer = {0x62, 0x10, 0x00, 0x11};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> g(s.cap.m);
            for (const Capture::Frame& f : s.cap.frames) {
                if (f.id == 0x7E9u && f.data == answer) {
                    return true;
                }
            }
            return false;
        },
        2000))
        << "the check's reply is a frame like any";
    /* The recovery: what went unanswered is asked again, and the engine
     * ECU's VIN is in */
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_channel_identity_t id{};
            return ecuio_session_get_channel_identity(s.s, 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_VIN) != 0 && items_settled(s.s);
        },
        10000))
        << "the identification was never read";
    /* Longer than an interval with no request: the checks ended */
    EXPECT_TRUE(goes_quiet(adapter_requests, kCheckMs * 3 / 2, 10000))
        << "with the link OK nothing is asked";
    EXPECT_EQ(s.state(), ECUIO_OK);

    ecuio_session_destroy(s.s);
    s.s = nullptr;
    EXPECT_EQ(ecuio_recorder_close(rec, "the test ended"), ECUIO_OK);
    const auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string frames = read(folder / "frames.csv");
    EXPECT_NE(frames.find(",0x7E9,"), std::string::npos) << frames;
    EXPECT_NE(frames.find(",62 10 00 11"), std::string::npos)
        << "the check's reply is recorded";
    const std::string events = read(folder / "events.log");
    EXPECT_NE(events.find("  link  vehicle not responding (state 8, "),
              std::string::npos)
        << events;
    EXPECT_NE(events.find("  link  recovered (state 0, "), std::string::npos)
        << events;
    EXPECT_TRUE(
        in_order(logs.only_log(), {"] > 221000", "] > 221000", "] < 7E9"}))
        << "each check's request and the reply are in the wire log";
    fs::remove_all(root, ec);
}

/* One module of two answers nothing at all, and the other nothing to
 * what it doesn't serve (the fake's refusals as silence): the silence
 * is one module's and an item's, never the vehicle's. The link is OK
 * through the unanswered items, and after the adapter dropped and came
 * back, when they are all asked again with no reply among them. */
TEST_F(Elm327SessionTest, AnUnpickedSessionWithOneSilentModuleStaysOk)
{
    struct Absent {
        Absent() { fake_ecu::absent_ecu().store(0x7E9); }
        ~Absent() { fake_ecu::absent_ecu().store(0); }
    } absent; /* before the session, whose worker reads it */
    const CarProfile profile("elm327-one-module-car");
    set_knobs([](fake_elm327::knobs& k) { k.unsupported_nrc = 0; });
    Session s;
    ASSERT_EQ(s.create(uri("?profile=elm327-one-module-car")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    /* The state is read all the while, so a timeout fails in its words */
    const auto settle_by =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(20000);
    while (!items_settled(s.s)) {
        ASSERT_EQ(s.state(), ECUIO_OK)
            << "a module's or an item's silence was taken for the vehicle's";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ecuio_channel_identity_t engine_ecu{};
    ASSERT_EQ(ecuio_session_get_channel_identity(s.s, 0, &engine_ecu),
              ECUIO_OK);
    EXPECT_NE(engine_ecu.flags & ECUIO_IDENTITY_VIN, 0u);
    uint32_t silent_items = 0;
    for (uint32_t i = 0; i < engine_ecu.items; ++i) {
        silent_items +=
            engine_ecu.list[i].outcome == ECUIO_IDENTITY_UNANSWERED ? 1u : 0u;
    }
    EXPECT_GE(silent_items, 1u)
        << "the case needs an item the live ECU leaves unanswered";
    ecuio_channel_identity_t module{};
    ASSERT_EQ(ecuio_session_get_channel_identity(s.s, 1, &module), ECUIO_OK);
    for (uint32_t i = 0; i < module.items; ++i) {
        EXPECT_EQ(module.list[i].outcome, ECUIO_IDENTITY_UNANSWERED) << i;
    }

    /* The adapter drops and is back: the recovery asks every unanswered
     * item again, the live ECU's too, and none is answered */
    ASSERT_TRUE(goes_quiet(adapter_requests, 200, 5000));
    const uint32_t before = adapter_requests();
    set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 10000));
    set_knobs([](fake_elm327::knobs& k) { k.dropped = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000));
    /* OK until the requests have stopped for 2 s. The check keeps its
     * 5 s here: one falling due before the failure ends would ask the
     * engine ECU a request it is silent on */
    uint32_t last = adapter_requests();
    auto since = std::chrono::steady_clock::now();
    const auto give_up = since + std::chrono::milliseconds(25000);
    while (std::chrono::steady_clock::now() - since <
           std::chrono::milliseconds(2000)) {
        ASSERT_EQ(s.state(), ECUIO_OK)
            << "the unanswered items asked again took the link down";
        ASSERT_LT(std::chrono::steady_clock::now(), give_up)
            << "the requests never stopped";
        const uint32_t seen = adapter_requests();
        if (seen != last) {
            last = seen;
            since = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(last, before) << "the recovery asked the unanswered items again";
    uint32_t episodes = 0;
    EXPECT_EQ(s.state(&episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u) << "the adapter's drop, and nothing since";
}

/* An adapter that drops and comes back while nothing is picked: the
 * hard failure ends once the adapter reads again, though nothing is
 * asked that a reply could answer */
TEST_F(Elm327SessionTest,
       AnUnpickedSessionLeavesAHardFailureOnceTheAdapterIsBack)
{
    const CarProfile profile("elm327-drop-car");
    Session s;
    ASSERT_EQ(s.create(uri("?profile=elm327-drop-car")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return items_settled(s.s); }, 15000));
    ASSERT_EQ(s.state(), ECUIO_OK);
    ASSERT_TRUE(goes_quiet(adapter_requests, 200, 5000));
    const auto resets = [] {
        return on_fake<uint32_t>(
            [](fake_elm327::adapter& a) { return a.resets; });
    };
    const uint32_t before = resets();
    const uint32_t asked = adapter_requests();
    set_knobs([](fake_elm327::knobs& k) { k.dropped = true; });
    ASSERT_TRUE(wait_for(
        [&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; }, 10000));
    set_knobs([](fake_elm327::knobs& k) { k.dropped = false; });
    ASSERT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }, 10000))
        << "the adapter is back and the state never left the failure";
    int32_t last = -1;
    uint32_t episodes = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.s, &last, &episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u);
    EXPECT_EQ(adapter_requests(), asked) << "nothing was asked to end it";
    EXPECT_GT(resets(), before) << "the setup ran again";
}

/* A healthy car with nothing picked: once identification and
 * diagnostics are in, the adapter is sent nothing at all, command or
 * request, for as long as nothing is picked */
TEST_F(Elm327SessionTest, AHealthyUnpickedSessionSendsNothingMore)
{
    const CarProfile profile("elm327-idle-car");
    const CheckEvery checks(kCheckMs);
    Session s;
    ASSERT_EQ(s.create(uri("?profile=elm327-idle-car")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return items_settled(s.s); }, 15000));
    ASSERT_TRUE(goes_quiet(adapter_commands, 300, 5000));
    const uint32_t commands = adapter_commands();
    const uint32_t requests = adapter_requests();
    /* One and a half of the intervals a check would take */
    ASSERT_TRUE(goes_quiet(adapter_requests, 3 * kCheckMs / 2, 3 * kCheckMs));
    EXPECT_EQ(s.state(), ECUIO_OK);
    EXPECT_EQ(adapter_requests(), requests);
    EXPECT_EQ(adapter_commands(), commands);
}

/* ?filter= asks every signal of one ECU: on a set whose modules together
 * ask more than one module's most that is the address's fault, said so */
TEST_F(Elm327SessionTest, AFilterOverASetPastAModulesMostIsABadUri)
{
    /* Two modules of kEach DIDs, no DID read by both: each within a
     * module's most, together past it */
    constexpr uint32_t kEach = 600;
    static_assert(kEach <= ECUIO_MODULE_MAX_REQUESTS &&
                      2 * kEach > ECUIO_MODULE_MAX_REQUESTS,
                  "one module fits, both on one ECU don't");
    static ecuio_channel_decl_t channels[2] = {{0x7E8, 0x7E0, 0, 0},
                                               {0x7E9, 0x7E1, 0, 0}};
    static ecuio_signal_def_t defs[2 * kEach];
    for (uint32_t i = 0; i < 2 * kEach; ++i) {
        defs[i] = car_sized::set()->defs[0];
        defs[i].id = static_cast<uint16_t>(100 + i);
        defs[i].message_id = channels[i < kEach ? 0 : 1].rx_id;
        defs[i].mux_key = 0x620000u | (0x1000u + i);
        std::snprintf(defs[i].name, sizeof defs[i].name, "Signal %u", i);
    }
    static ecuio_signal_set_t apart = {};
    apart.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    apart.count = 2 * kEach;
    apart.defs = defs;
    apart.channel_count = 2;
    apart.channels = channels;
    ecuio_validation_t v{};
    ASSERT_EQ(ecuio_profile_register("elm327-filter-apart", &apart, &v),
              ECUIO_OK)
        << v.code << " at " << v.index;
    ASSERT_EQ(
        ecuio_profile_register("elm327-filter-car", car_sized::set(), nullptr),
        ECUIO_OK);
    {
        Session s;
        EXPECT_EQ(s.create(uri("?profile=elm327-filter-apart&filter=0x7E8")),
                  ECUIO_ERR_BAD_URI);
        EXPECT_EQ(std::string(ecuio_last_error_detail()),
                  "?filter= asks every signal of the profile of one ECU, and "
                  "they ask more than 1024 requests");
        EXPECT_TRUE(fake_commands().empty()) << "refused before the port opens";
    }
    {
        /* Without ?filter= it is two modules, and connects */
        Session s;
        EXPECT_EQ(s.create(uri("?profile=elm327-filter-apart&identify=0&"
                               "diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    {
        /* The car-sized set's modules read the same DIDs: of one ECU they
         * are one module's most, which fits */
        Session s;
        EXPECT_EQ(s.create(uri("?profile=elm327-filter-car&filter=0x7E8&"
                               "identify=0&diagnose=0")),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    EXPECT_EQ(ecuio_profile_unregister("elm327-filter-car"), ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister("elm327-filter-apart"), ECUIO_OK);
}
