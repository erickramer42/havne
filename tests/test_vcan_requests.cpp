// File/layer: tests/test_vcan_requests.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanRequestTest (test_vcan)

#include "test_vcan_requests_support.h"

/* The probe walks the fake's bitmaps (00 to A0) and the rotation runs:
 * whole replies, the service byte first, from the ECU's reply ID */
TEST_F(SocketCanRequestTest, TheProbeWalksTheFakeEcusBitmaps)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_support_t sup{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                       ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
        },
        5000ms));
    EXPECT_EQ(sup.rx_id, 0x7E8u);
    EXPECT_EQ(sup.ranges, 0x3Fu) << "the bitmaps at 0x00 to 0xA0";
    EXPECT_EQ(sup.dropped, 0u) << "the fake supports every obd2 PID";

    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.backend, static_cast<uint32_t>(ECUIO_BACKEND_SOCKETCAN));
    EXPECT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    EXPECT_EQ(info.tx_ids[0], 0x7E0u);
    int32_t e = 0;
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_error_state(s.get(), &e, &n), ECUIO_OK);
    EXPECT_EQ(e, ECUIO_OK);
    EXPECT_EQ(n, 0u);
}

/* Identification's replies span frames (the VIN's 20 bytes: a First
 * Frame and two Consecutive Frames): the kernel's ISO-TP sends the rest
 * only after core's Flow Control */
TEST_F(SocketCanRequestTest, IdentificationReadsTheVinAcrossFrames)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_channel_identity_t id{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        8000ms));
    ASSERT_GT(id.items, 0u);
    EXPECT_EQ(id.list[0].request[0], 0x09);
    EXPECT_EQ(id.list[0].request[1], 0x02);
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(id.list[0].text, fake_ecu::kVin);
}

/* Two ECUs on the bus in one session: the engine ECU's 1E1C from 0x7E8,
 * and a module's D801 (fake_ecu's table) from a second ECU on
 * 0x7E1/0x7E9, each through its own ISO-TP socket; each reply comes
 * from its own ECU */
TEST_F(SocketCanRequestTest, TwoEcusInOneSessionAnswerEachItsOwn)
{
    struct Module {
        Module() { EXPECT_TRUE(fake_ecu::set_module_dids("7E9:D801=00 12")); }
        ~Module() { fake_ecu::set_module_dids(nullptr); }
    } module;
    struct Replies {
        std::mutex mutex;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> got;
        static void cb(const ecuio_frame_t* f, void* user)
        {
            auto* r = static_cast<Replies*>(user);
            std::lock_guard<std::mutex> lock(r->mutex);
            r->got.emplace_back(
                f->arbitration_id,
                std::vector<uint8_t>(f->data, f->data + f->data_length));
        }
    } replies;
    vcan_bus::Ecu engine;
    vcan_bus::Ecu second(-1, vcan_bus::interface_name(), 0x7E1, 0x7E9);
    ASSERT_TRUE(engine.ok() && second.ok());
    Session s("?profile=uds-generic&did=1E1C,7E9:D801&identify=0&diagnose=0",
              Replies::cb, &replies);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const std::vector<uint8_t> want_engine = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    const std::vector<uint8_t> want_module = {0x62, 0xD8, 0x01, 0x00, 0x12};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(replies.mutex);
            int a = 0;
            int b = 0;
            for (const auto& r : replies.got) {
                a += r.first == 0x7E8u && r.second == want_engine;
                b += r.first == 0x7E9u && r.second == want_module;
            }
            return a >= 3 && b >= 3;
        },
        5000ms))
        << "three of each ECU's own replies";
    std::lock_guard<std::mutex> lock(replies.mutex);
    for (const auto& r : replies.got) {
        EXPECT_TRUE((r.first == 0x7E8u && r.second == want_engine) ||
                    (r.first == 0x7E9u && r.second == want_module))
            << "a reply on 0x" << std::hex << r.first;
    }
    EXPECT_GT(second.answered(), 0u);
}

/* ADR-0065 section 3: a session on a set that derives more requests than
 * a rotation holds says so, puts nothing on the bus until a focus picks
 * and stays out of "not responding" meanwhile, then asks the picks
 * alone; a pick the ECU refuses still times the link out, and the
 * timeout ends when the pick is taken away */
TEST_F(SocketCanRequestTest, ASetPastARotationIsAskedOnlyItsPicks)
{
    using car_sized::id_of;
    using car_sized::kMost;
    struct Module {
        Module()
        {
            EXPECT_TRUE(fake_ecu::set_module_dids("7E8:1007=2A;7E9:1003=11"));
        }
        ~Module() { fake_ecu::set_module_dids(nullptr); }
    } module;
    struct Profile {
        Profile()
        {
            EXPECT_EQ(ecuio_profile_register("socketcan-car", car_sized::set(),
                                             nullptr),
                      ECUIO_OK);
        }
        ~Profile()
        {
            EXPECT_EQ(ecuio_profile_unregister("socketcan-car"), ECUIO_OK);
        }
    } profile; /* before the session, which holds the set */
    struct Replies {
        std::mutex mutex;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> got;
        static void cb(const ecuio_frame_t* f, void* user)
        {
            auto* r = static_cast<Replies*>(user);
            std::lock_guard<std::mutex> lock(r->mutex);
            r->got.emplace_back(
                f->arbitration_id,
                std::vector<uint8_t>(f->data, f->data + f->data_length));
        }
    } replies;
    vcan_bus::Ecu engine;
    vcan_bus::Ecu second(-1, vcan_bus::interface_name(), 0x7E1, 0x7E9);
    ASSERT_TRUE(engine.ok() && second.ok());
    /* The probe stays on: with it a refused request leaves the rotation */
    Session s("?profile=socketcan-car&identify=0&diagnose=0", Replies::cb,
              &replies);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const auto state = [&] {
        int32_t last = -1;
        uint32_t episodes = 0;
        EXPECT_EQ(ecuio_session_get_error_state(s.get(), &last, &episodes),
                  ECUIO_OK);
        return last;
    };
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 2u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
    EXPECT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);

    /* Twice the quiet run a link takes to say an ECU isn't answering */
    std::this_thread::sleep_for(2000ms);
    EXPECT_EQ(engine.answered(), 1u)
        << "the start's probe alone: nothing is asked before a pick";
    EXPECT_EQ(second.answered(), 0u);
    EXPECT_EQ(state(), ECUIO_OK)
        << "a session asking nothing isn't one an ECU doesn't answer";

    /* One signal of each module */
    const uint16_t picks[] = {id_of(kMost + 3), id_of(7)};
    ASSERT_EQ(ecuio_session_set_focus(s.get(), picks, 2), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::vector<uint8_t> want_engine = {0x62, 0x10, 0x07, 0x2A};
    const std::vector<uint8_t> want_other = {0x62, 0x10, 0x03, 0x11};
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(replies.mutex);
            int a = 0;
            int b = 0;
            for (const auto& r : replies.got) {
                a += r.first == 0x7E8u && r.second == want_engine;
                b += r.first == 0x7E9u && r.second == want_other;
            }
            return a >= 3 && b >= 3;
        },
        5000ms))
        << "three of each module's pick";
    {
        std::lock_guard<std::mutex> lock(replies.mutex);
        for (const auto& r : replies.got) {
            EXPECT_TRUE((r.first == 0x7E8u && r.second == want_engine) ||
                        (r.first == 0x7E9u && r.second == want_other))
                << "a reply on 0x" << std::hex << r.first;
        }
    }
    EXPECT_EQ(state(), ECUIO_OK);

    /* A pick the ECU refuses is dropped, but it was asked: the silence
     * after it counts as any table's does */
    const uint16_t refused = id_of(9);
    ASSERT_EQ(ecuio_session_set_focus(s.get(), &refused, 1), ECUIO_OK);
    ASSERT_TRUE(
        wait_for([&] { return state() == ECUIO_ERR_VEHICLE_TIMEOUT; }, 5000ms))
        << "picks that were all dropped still time out";
    /* The pick taken away: nothing is asked, and its timeout ends */
    ASSERT_EQ(ecuio_session_set_focus(s.get(), nullptr, 0), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return state() == ECUIO_OK; }, 5000ms))
        << "nothing asked: nothing fails to answer";
}

namespace {

/* Whether both modules' identification and diagnostics have an outcome
 * for every item */
bool items_settled(const Session& s)
{
    for (uint32_t c = 0; c < 2; ++c) {
        ecuio_channel_identity_t id{};
        ecuio_channel_diagnostics_t diag{};
        ecuio_channel_support_t sup{};
        if (ecuio_session_get_channel_support(s.get(), c, &sup) == ECUIO_OK &&
            (sup.flags & ECUIO_SUPPORT_CLOSED) != 0) {
            continue; /* no answer to the start's probe, nothing picked */
        }
        if (ecuio_session_get_channel_identity(s.get(), c, &id) != ECUIO_OK ||
            ecuio_session_get_channel_diagnostics(s.get(), c, &diag) !=
                ECUIO_OK ||
            (id.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
            (diag.flags & ECUIO_DIAG_COMPLETE) == 0) {
            return false;
        }
    }
    return true;
}

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

/* A request the session put on the bus: its ID and its Single Frame */
struct Asked {
    uint32_t id;
    std::vector<uint8_t> data;
};

/* Takes what a raw listener has heard without waiting: the Single
 * Frames on the car-sized set's two request IDs */
void take_asked(int listener, std::vector<Asked>& asked)
{
    can_frame f{};
    while (vcan_bus::read_for(listener, &f, sizeof f, 0) ==
           static_cast<long>(sizeof f)) {
        const uint32_t id = f.can_id & CAN_SFF_MASK;
        if ((f.can_id & (CAN_ERR_FLAG | CAN_EFF_FLAG)) == 0 &&
            (id == 0x7E0 || id == 0x7E1) && f.len >= 1 &&
            (f.data[0] >> 4) == 0) {
            asked.push_back({id, std::vector<uint8_t>(f.data, f.data + f.len)});
        }
    }
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

/* Whether a request is 22 10 00, the car-sized set's first DID */
bool asks_did(const Asked& a, uint8_t low)
{
    return a.data.size() >= 4 && a.data[0] == 0x03 && a.data[1] == 0x22 &&
           a.data[2] == 0x10 && a.data[3] == low;
}

bool asks_the_first_did(const Asked& a)
{
    return asks_did(a, 0x00);
}

} // namespace

/* Such a session with nothing picked still asks identification and
 * diagnostics, each module's. One module of two isn't on the bus (so the
 * start's probe leaves it closed), and the other answers nothing to what
 * it doesn't serve (its refusals are silence): the silence is an item's,
 * never the vehicle's. The link is OK through the unanswered items, and after a
 * bus-off and the bus clean again, when they are all asked again with
 * no reply among them. */
TEST_F(SocketCanRequestTest, AnUnpickedSessionWithOneSilentModuleStaysOk)
{
    const CarProfile profile("socketcan-one-module-car");
    vcan_bus::Ecu engine(0); /* silent where it would refuse; none on 0x7E1 */
    ASSERT_TRUE(engine.ok());
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s("?profile=socketcan-one-module-car");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
    ASSERT_NE(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
    /* The state is read all the while, so a timeout fails in its words */
    const auto settle_by = std::chrono::steady_clock::now() + 12000ms;
    while (!items_settled(s)) {
        ASSERT_EQ(state_of(s), ECUIO_OK)
            << "one module's silence was taken for the vehicle's";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(5ms);
    }
    ecuio_channel_identity_t engine_ecu{};
    ASSERT_EQ(ecuio_session_get_channel_identity(s.get(), 0, &engine_ecu),
              ECUIO_OK);
    EXPECT_NE(engine_ecu.flags & ECUIO_IDENTITY_VIN, 0u);
    uint32_t silent_items = 0;
    for (uint32_t i = 0; i < engine_ecu.items; ++i) {
        silent_items +=
            engine_ecu.list[i].outcome == ECUIO_IDENTITY_UNANSWERED ? 1u : 0u;
    }
    EXPECT_GE(silent_items, 1u)
        << "the case needs an item the live ECU leaves unanswered";
    /* The module off the bus gave the start's probe no answer: closed
     * with nothing picked, so its silence is never asked */
    ecuio_channel_support_t module{};
    ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 1, &module), ECUIO_OK);
    EXPECT_NE(module.flags & ECUIO_SUPPORT_CLOSED, 0u);
    uint32_t episodes = 0;
    EXPECT_EQ(state_of(s, &episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u);

    /* A bus-off and the bus clean again: the recovery asks every
     * unanswered item again, the live ECU's too, and none is answered */
    std::vector<Asked> asked;
    const auto asked_now = [&] {
        take_asked(bus.get(), asked);
        return asked.size();
    };
    ASSERT_TRUE(goes_quiet(asked_now, 200, 5000));
    const size_t before = asked.size();
    ASSERT_TRUE(put(bus.get(), CAN_ERR_FLAG | CAN_ERR_BUSOFF,
                    {0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_TRUE(wait_for(
        [&] { return state_of(s) == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
        5000ms));
    ASSERT_TRUE(wait_for([&] { return state_of(s) == ECUIO_OK; }, 5000ms));
    /* OK until the requests have stopped for 2 s. The check keeps its
     * 5 s here: one falling due before the failure ends would ask the
     * engine ECU a request it is silent on */
    size_t last = asked_now();
    auto since = std::chrono::steady_clock::now();
    const auto give_up = since + 20000ms;
    while (std::chrono::steady_clock::now() - since < 2000ms) {
        ASSERT_EQ(state_of(s), ECUIO_OK)
            << "the unanswered items asked again took the link down";
        ASSERT_LT(std::chrono::steady_clock::now(), give_up)
            << "the requests never stopped";
        const size_t seen = asked_now();
        if (seen != last) {
            last = seen;
            since = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GT(last, before) << "the recovery asked the unanswered items again";
    EXPECT_EQ(state_of(s, &episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u) << "the bus-off, and nothing since";
}

/* On a bus with no ECU the link says not responding and keeps saying
 * so: an idle pass never turns that into OK. While it is down the
 * worker asks one request an interval (here kCheckMs) of the module
 * standing in for the silent start's probe (the first: the car asks no
 * PID 0C), the first a whole interval after the items settled. The
 * first answered (a refusal is an answer) ends the timeout, the
 * identification is read after all, and the checks end. */
TEST_F(SocketCanRequestTest,
       AnUnpickedSessionOnASilentBusChecksItAndFindsItAwake)
{
    const CheckEvery checks(kCheckMs);
    struct Module {
        Module() { EXPECT_TRUE(fake_ecu::set_module_dids("7E9:1000=11")); }
        ~Module() { fake_ecu::set_module_dids(nullptr); }
    } module_dids; /* 0x7E9, once on the bus, answers the car's first DID */
    const CarProfile profile("socketcan-quiet-car");
    vcan_bus::Fd listener = vcan_bus::open_raw();
    ASSERT_TRUE(listener.ok());
    Session s("?profile=socketcan-quiet-car");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ASSERT_TRUE(wait_for(
        [&] { return state_of(s) == ECUIO_ERR_VEHICLE_TIMEOUT; }, 10000ms))
        << "what it asked went unanswered";
    const auto settle_by = std::chrono::steady_clock::now() + 12000ms;
    while (!items_settled(s)) {
        ASSERT_EQ(state_of(s), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "the timeout ended with nothing answered";
        ASSERT_LT(std::chrono::steady_clock::now(), settle_by)
            << "identification and diagnostics never settled";
        std::this_thread::sleep_for(5ms);
    }
    const auto settled_at = std::chrono::steady_clock::now();
    std::vector<Asked> asked;
    take_asked(listener.get(), asked);
    const size_t settled = asked.size();
    EXPECT_GT(settled, 0u);

    /* The first check, an interval on, and the timeout through it */
    const auto first_by = settled_at + 8000ms;
    while (asked.size() == settled) {
        ASSERT_EQ(state_of(s), ECUIO_ERR_VEHICLE_TIMEOUT)
            << "an idle pass with nothing picked ended the timeout";
        ASSERT_LT(std::chrono::steady_clock::now(), first_by)
            << "no check within 8 s";
        std::this_thread::sleep_for(5ms);
        take_asked(listener.get(), asked);
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - settled_at);
    /* Half an interval is the bound: this thread may read the clock
     * late; PickOnlyTest pins the interval to the microsecond */
    EXPECT_GE(waited.count(), kCheckMs / 2)
        << "nothing is asked again before an interval has passed";
    EXPECT_EQ(asked[settled].id, 0x7E0u) << "the engine ECU's";
    EXPECT_TRUE(asks_the_first_did(asked[settled])) << "its first request";
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckMs / 4));
    take_asked(listener.get(), asked);
    EXPECT_EQ(asked.size(), settled + 1) << "one request, once";
    EXPECT_EQ(state_of(s), ECUIO_ERR_VEHICLE_TIMEOUT)
        << "its silence changes nothing";

    /* The ECUs on the bus: the next check, the engine ECU's next row, is
     * answered, refused; the other module stays closed */
    vcan_bus::Ecu engine;
    vcan_bus::Ecu module(-1, vcan_bus::interface_name(), 0x7E1, 0x7E9);
    ASSERT_TRUE(engine.ok());
    ASSERT_TRUE(module.ok());
    ASSERT_TRUE(wait_for([&] { return state_of(s) == ECUIO_OK; }, 8000ms))
        << "no check found the vehicle awake";
    take_asked(listener.get(), asked);
    ASSERT_GT(asked.size(), settled + 1);
    EXPECT_EQ(asked[settled + 1].id, 0x7E0u) << "the engine ECU's again";
    EXPECT_TRUE(asks_did(asked[settled + 1], 0x01)) << "its next row";
    EXPECT_EQ(module.answered(), 0u) << "the module not standing in";
    /* The recovery: what went unanswered is asked again, and the engine
     * ECU's VIN is in */
    ASSERT_TRUE(wait_for(
        [&] {
            ecuio_channel_identity_t id{};
            return ecuio_session_get_channel_identity(s.get(), 0, &id) ==
                       ECUIO_OK &&
                   (id.flags & ECUIO_IDENTITY_VIN) != 0 && items_settled(s);
        },
        10000ms))
        << "the identification was never read";
    EXPECT_GE(engine.answered(), 1u);
    /* Longer than an interval with no request: the checks ended */
    EXPECT_TRUE(goes_quiet(
        [&] {
            take_asked(listener.get(), asked);
            return asked.size();
        },
        kCheckMs * 3 / 2, 10000))
        << "with the link OK nothing is asked";
    uint32_t episodes = 0;
    EXPECT_EQ(state_of(s, &episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 0u) << "a vehicle not responding is no episode";
}

/* A bus-off while nothing is picked: the hard failure ends once the
 * bus reads clean again, with nothing asked that a reply could answer */
TEST_F(SocketCanRequestTest,
       AnUnpickedSessionLeavesAHardFailureOnceTheBusReadsClean)
{
    const CarProfile profile("socketcan-bus-off-car");
    vcan_bus::Ecu engine;
    vcan_bus::Ecu module(-1, vcan_bus::interface_name(), 0x7E1, 0x7E9);
    ASSERT_TRUE(engine.ok());
    ASSERT_TRUE(module.ok());
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s("?profile=socketcan-bus-off-car");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ASSERT_TRUE(wait_for([&] { return items_settled(s); }, 12000ms));
    ASSERT_EQ(state_of(s), ECUIO_OK);
    std::vector<Asked> asked;
    ASSERT_TRUE(goes_quiet(
        [&] {
            take_asked(bus.get(), asked);
            return asked.size();
        },
        200, 5000));
    const size_t before = asked.size();
    ASSERT_TRUE(put(bus.get(), CAN_ERR_FLAG | CAN_ERR_BUSOFF,
                    {0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_TRUE(wait_for(
        [&] { return state_of(s) == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
        5000ms));
    ASSERT_TRUE(wait_for([&] { return state_of(s) == ECUIO_OK; }, 5000ms))
        << "the bus reads clean and the state never left the failure";
    uint32_t episodes = 0;
    EXPECT_EQ(state_of(s, &episodes), ECUIO_OK);
    EXPECT_EQ(episodes, 1u);
    take_asked(bus.get(), asked);
    EXPECT_EQ(asked.size(), before) << "nothing was asked to end it";
}

/* The start's probe (request_probe.h) on the bus: the engine ECU answers
 * the functional 01 00 on a socket of the probe's own, so a set that
 * doesn't declare it gains it as a module, open for RANGE; ?filter=
 * joins nothing */
TEST_F(SocketCanRequestTest, AnUndeclaredAnswererJoinsTheTable)
{
    vcan_bus::Ecu engine;
    ASSERT_TRUE(engine.ok());
    {
        Session s("?profile=uds-generic&did=7B8:D801&identify=0&diagnose=0");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
        ASSERT_EQ(info.module_count, 2u);
        ecuio_channel_decl_t m{};
        ASSERT_EQ(ecuio_session_get_module(s.get(), 1, &m), ECUIO_OK);
        EXPECT_EQ(m.rx_id, 0x7E8u);
        EXPECT_EQ(m.tx_id, 0x7E0u);
        ecuio_channel_support_t sup{};
        ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 1, &sup),
                  ECUIO_OK);
        EXPECT_NE(sup.open_reasons & ECUIO_MODULE_REASON_RANGE, 0u);
    }
    Session f("?profile=obd2&filter=0x7B8&identify=0&diagnose=0");
    ASSERT_EQ(f.status(), ECUIO_OK) << f.detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(f.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 1u);
}

/* SocketCAN keeps no session log: the probe's time and answerers go to
 * standard error */
TEST_F(SocketCanRequestTest, TheProbeWritesItsLineToStandardError)
{
    vcan_bus::Ecu engine;
    ASSERT_TRUE(engine.ok());
    testing::internal::CaptureStderr();
    {
        Session s("?profile=obd2&identify=0&diagnose=0");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    }
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("socketcan: emissions probe: 1 functional 01 00 in "),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("answered by 7E8"), std::string::npos) << err;
}
