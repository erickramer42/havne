// File/layer: tests/test_j2534_modules.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534ModulesTest (test_j2534_health); Cross-arch
//
// Each module's FLOW_CONTROL filter follows its state on the fake DLL,
// through the surrogate (ADR-0065 step 5): a filter for each module open
// or closing, stopped once it has closed, at most J2534's 10 with one
// kept back for a visit. The fake publishes the filters it holds now and
// the most at once; ECUIO_FAKE_FILTER_MAX makes it refuse a start as an
// adapter out of filters does. The 11-bit cases run the start's probe,
// which the fake's engine ECU (7E8, undeclared) answers, so it joins the
// table as RANGE and the rest are the picks'; they name a module by its
// reply ID less 7E8 (0 for 7E8). The 29-bit cases run without the
// probe, so every module is RANGE and the budget alone decides.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/signal_sets.h"

#ifdef _WIN32
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "j2534_backend_fixture.h"
#include "start_probe_seam.h"

namespace {

/* 7E9 to 7EE, one u8 DID each (1000 on); the fake's engine ECU (7E8)
 * answers the probe and joins the table after them */
const char kSmallDids[] =
    "7E9:1000=11;7EA:1001=12;7EB:1002=13;7EC:1003=14;7ED:1004=15;7EE:1005=16";
const char kSmallQuery[] = "?profile=uds-generic&identify=0&diagnose=0&did="
                           "7E9:1000:u8,7EA:1001:u8,7EB:1002:u8,7EC:1003:u8,"
                           "7ED:1004:u8,7EE:1005:u8";
constexpr uint32_t kJoined = 6; /* 7E8's channel */

/* 11 made-up 29-bit ECUs, 0x40 to 0x4A */
constexpr int kWide = 11;

uint32_t wide_id(int i)
{
    return 0x18DAF140u + static_cast<uint32_t>(i);
}

/* The first named of them answer from their DID alone; the rest answer
 * as the engine ECU (the emissions services, a clear) */
std::string wide_dids(int named = kWide)
{
    std::string spec;
    char row[48];
    for (int i = 0; i < named; ++i) {
        snprintf(row, sizeof row, "%s%08X:4A00=%02X", i == 0 ? "" : ";",
                 static_cast<unsigned>(wide_id(i)), i);
        spec += row;
    }
    return spec;
}

std::string wide_query(bool diagnose)
{
    std::string q = "?profile=uds-generic&identify=0";
    q += diagnose ? "&did=" : "&diagnose=0&did=";
    char item[32];
    for (int i = 0; i < kWide; ++i) {
        snprintf(item, sizeof item, "%s%08X:4A00:u8", i == 0 ? "" : ",",
                 static_cast<unsigned>(wide_id(i)));
        q += item;
    }
    return q;
}

/* The frames each reply ID delivered, and the pending replies seen */
struct Heard {
    std::mutex mutex;
    std::map<uint32_t, int> frames;
    std::map<uint32_t, int> pending;

    static void on_frame(const ecuio_frame_t* f, void* user)
    {
        auto* self = static_cast<Heard*>(user);
        const std::lock_guard<std::mutex> lock(self->mutex);
        if (f->data_length >= 3 && f->data[0] == 0x7F && f->data[2] == 0x78) {
            ++self->pending[f->arbitration_id];
            return;
        }
        ++self->frames[f->arbitration_id];
    }

    int of(uint32_t rx)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = frames.find(rx);
        return it == frames.end() ? 0 : it->second;
    }

    int pending_of(uint32_t rx)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = pending.find(rx);
        return it == pending.end() ? 0 : it->second;
    }
};

class J2534ModulesTest : public j2534_backends::BackendTest
{
protected:
    Heard heard; /* outlives the session, which TearDown closes */
    std::unique_ptr<StartProbe> probe_;
    std::string spec_;
    std::unique_ptr<j2534_backends::EnvSwap> dids_;
    ecuio_session_t* session = nullptr;

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }

    void create(const std::string& query)
    {
        const std::string u = uri(query.c_str());
        ASSERT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_OK)
            << u << ": " << ecuio_last_error_detail();
        ASSERT_EQ(
            ecuio_register_frame_callback(session, Heard::on_frame, &heard),
            ECUIO_OK);
    }

    /* The six 11-bit modules, the start's probe on for the case, and the
     * fake dropping a reply no filter it holds matches */
    void small_session()
    {
        ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_GATING", "1"));
        probe_ = std::make_unique<StartProbe>();
        dids_ = std::make_unique<j2534_backends::EnvSwap>(
            "ECUIO_FAKE_MODULE_DIDS", kSmallDids);
        create(kSmallQuery);
    }

    /* The eleven 29-bit modules, with no probe: every one RANGE */
    void wide_session(bool diagnose, int named = kWide)
    {
        spec_ = wide_dids(named);
        dids_ = std::make_unique<j2534_backends::EnvSwap>(
            "ECUIO_FAKE_MODULE_DIDS", spec_.c_str());
        create(wide_query(diagnose));
    }

    /* The picks: the small set's modules by number (1 is 7E9) */
    void pick(std::initializer_list<int> modules)
    {
        std::vector<uint16_t> ids;
        for (const int m : modules) {
            ids.push_back(static_cast<uint16_t>(ECUIO_UDS_GENERIC_SIG_ID_BASE +
                                                static_cast<uint32_t>(m - 1)));
        }
        ASSERT_EQ(ecuio_session_set_focus(session, ids.data(),
                                          static_cast<uint32_t>(ids.size())),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }

    uint32_t support(uint32_t c)
    {
        ecuio_channel_support_t sup{};
        EXPECT_EQ(ecuio_session_get_channel_support(session, c, &sup),
                  ECUIO_OK);
        return sup.flags;
    }

    uint32_t reasons(uint32_t c)
    {
        ecuio_channel_support_t sup{};
        EXPECT_EQ(ecuio_session_get_channel_support(session, c, &sup),
                  ECUIO_OK);
        return sup.open_reasons;
    }

    uint32_t diagnostics(uint32_t c) { return report(c).flags; }

    ecuio_channel_diagnostics_t report(uint32_t c)
    {
        ecuio_channel_diagnostics_t d{};
        EXPECT_EQ(ecuio_session_get_channel_diagnostics(session, c, &d),
                  ECUIO_OK);
        return d;
    }

    uint32_t active() const { return fake_control::filters_active(page); }
    uint32_t peak() const { return fake_control::filters_peak(page); }
    uint32_t refused() const { return fake_control::filters_refused(page); }
    uint32_t started() const { return fake_control::filters_started(page); }

    static bool wait_for(const std::function<bool()>& done, int ms = 5000)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done()) {
            if (std::chrono::steady_clock::now() >= until) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    static uint32_t small_rx(int m)
    {
        return 0x7E8u + static_cast<uint32_t>(m);
    }

    /* A small set's module's support flags, by number */
    uint32_t flags_of(int m)
    {
        return support(m == 0 ? kJoined : static_cast<uint32_t>(m - 1));
    }
};

} // namespace

/* A pick of two set before the start: the start's filters are the probe's
 * answerer's and the picks', never the six the set declares */
TEST_F(J2534ModulesTest, StartRegistersOnlyThePickedModulesFilters)
{
    ASSERT_NO_FATAL_FAILURE(small_session());
    EXPECT_EQ(active(), 0u) << "the probe's filters stopped at the attach";
    fake_control::clear_filters_peak(page);
    ASSERT_NO_FATAL_FAILURE(pick({2, 3}));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        return heard.of(small_rx(2)) > 0 && heard.of(small_rx(3)) > 0;
    }));
    EXPECT_EQ(active(), 3u);
    EXPECT_EQ(peak(), 3u) << "the start registered no filter of a module "
                             "nothing picks";
    EXPECT_EQ(flags_of(0) & (ECUIO_SUPPORT_CLOSED | ECUIO_SUPPORT_NO_ROOM), 0u)
        << "7E8, RANGE, is open";
    for (const int m : {1, 4, 5}) {
        EXPECT_EQ(heard.of(small_rx(m)), 0) << m;
        EXPECT_NE(flags_of(m) & ECUIO_SUPPORT_CLOSED, 0u) << m;
    }
}

/* A module the pick leaves keeps its filter while its request is out (a
 * pending reply holds it) and stops it once the reply is in */
TEST_F(J2534ModulesTest, AClosedModulesFilterStopsAfterItsLastReply)
{
    ASSERT_NO_FATAL_FAILURE(small_session());
    ASSERT_NO_FATAL_FAILURE(pick({1, 2}));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        return heard.of(small_rx(1)) > 0 && heard.of(small_rx(2)) > 0;
    }));
    ASSERT_EQ(active(), 3u);

    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_HELD_SERVICE", "22"));
    ASSERT_TRUE(wait_for([&] { return heard.pending_of(small_rx(2)) > 0; }))
        << "7EA's request never went pending";
    ASSERT_NO_FATAL_FAILURE(pick({1}));
    /* closing: no reason wants it, its report not yet closed */
    ASSERT_TRUE(wait_for([&] { return reasons(1) == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(flags_of(2) & ECUIO_SUPPORT_CLOSED, 0u);
    EXPECT_EQ(active(), 3u) << "7EA's filter stopped with its reply still due";

    const uint8_t reply[8] = {0x00, 0x00, 0x07, 0xEA, 0x62, 0x10, 0x01, 0x12};
    ASSERT_TRUE(fake_control::post(page, reply, sizeof(reply), 0, 0));
    EXPECT_TRUE(wait_for([&] { return active() == 2u; }, 2000))
        << active() << " filters held after 7EA's reply";
    EXPECT_NE(flags_of(2) & ECUIO_SUPPORT_CLOSED, 0u);
}

/* The fake holds three: the third picked module's filter is refused at
 * the start, which goes on; the module waits with no room, asked nothing,
 * and the rest are read */
TEST_F(J2534ModulesTest, ARefusedFilterLeavesOneModuleWaitingAndTheRestOpen)
{
    ASSERT_NO_FATAL_FAILURE(small_session());
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_MAX", "3"));
    ASSERT_NO_FATAL_FAILURE(pick({1, 2, 3}));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_TRUE(wait_for([&] {
        return heard.of(small_rx(1)) > 0 && heard.of(small_rx(2)) > 0;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_NE(flags_of(3) & ECUIO_SUPPORT_NO_ROOM, 0u);
    EXPECT_EQ(flags_of(0) & ECUIO_SUPPORT_NO_ROOM, 0u)
        << "RANGE's filter goes first";
    EXPECT_EQ(heard.of(small_rx(3)), 0);
    EXPECT_EQ(active(), 3u);
    int32_t health = ECUIO_ERR_INVALID_ARGUMENT;
    uint32_t episodes = 0;
    ASSERT_EQ(ecuio_session_get_error_state(session, &health, &episodes),
              ECUIO_OK);
    EXPECT_EQ(health, ECUIO_OK) << "a refused filter is no link fault";

    /* the budget fell to the three held: a fifth module waits unasked */
    ASSERT_NO_FATAL_FAILURE(pick({1, 2, 3, 4}));
    ASSERT_TRUE(
        wait_for([&] { return (flags_of(4) & ECUIO_SUPPORT_NO_ROOM) != 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(refused(), 1u) << "a filter asked of an adapter that said no";
}

/* After a refusal, a module the pick leaves frees its filter for the one
 * waiting */
TEST_F(J2534ModulesTest, AFreedFilterOpensTheModuleWaitingForOne)
{
    ASSERT_NO_FATAL_FAILURE(small_session());
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_MAX", "3"));
    ASSERT_NO_FATAL_FAILURE(pick({1, 2, 3}));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        return (flags_of(3) & ECUIO_SUPPORT_NO_ROOM) != 0 &&
               heard.of(small_rx(1)) > 0;
    }));
    ASSERT_NO_FATAL_FAILURE(pick({2, 3}));
    EXPECT_TRUE(wait_for([&] { return heard.of(small_rx(3)) > 0; }))
        << "7EB never read after 7E9 closed";
    EXPECT_NE(flags_of(1) & ECUIO_SUPPORT_CLOSED, 0u);
    EXPECT_EQ(active(), 3u);
}

/* A refusal mid-session, below the budget: the module asking waits with
 * no room and the five already open stay open, the picks reading */
TEST_F(J2534ModulesTest, ARefusalBelowTheBudgetClosesNoShowingModule)
{
    ASSERT_NO_FATAL_FAILURE(small_session());
    ASSERT_NO_FATAL_FAILURE(pick({1, 2, 3, 4}));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        for (int m = 1; m < 5; ++m) {
            if (heard.of(small_rx(m)) == 0) {
                return false;
            }
        }
        return true;
    }));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_MAX", "5"));
    ASSERT_NO_FATAL_FAILURE(pick({1, 2, 3, 4, 5}));
    ASSERT_TRUE(
        wait_for([&] { return (flags_of(5) & ECUIO_SUPPORT_NO_ROOM) != 0; }));
    int before[5] = {};
    for (int m = 0; m < 5; ++m) {
        before[m] = heard.of(small_rx(m));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (int m = 0; m < 5; ++m) {
        /* 7E8 joined with no row of its own: it holds its filter, unasked */
        EXPECT_TRUE(m == 0 || heard.of(small_rx(m)) > before[m])
            << m << " stopped reading";
        EXPECT_EQ(flags_of(m) & (ECUIO_SUPPORT_CLOSED | ECUIO_SUPPORT_NO_ROOM),
                  0u)
            << m;
    }
    EXPECT_EQ(heard.of(small_rx(5)), 0);
    EXPECT_EQ(active(), 5u);
}

/* Eleven modules, every one wanted: live data holds nine filters and
 * keeps the tenth free */
TEST_F(J2534ModulesTest, TheCeilingIsTenBeforeAnyRefusalAndLiveDataTakesNine)
{
    ASSERT_NO_FATAL_FAILURE(wide_session(false));
    fake_control::clear_filters_peak(page);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        for (int i = 0; i < 9; ++i) {
            if (heard.of(wide_id(i)) == 0) {
                return false;
            }
        }
        return true;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(active(), 9u);
    EXPECT_EQ(peak(), 9u);
    for (const uint32_t c : {9u, 10u}) {
        EXPECT_NE(support(c) & ECUIO_SUPPORT_NO_ROOM, 0u) << c;
    }
}

/* A read over the modules visits the two waiting for a filter one at a
 * time, in the filter kept back: the count reaches ten and comes back to
 * nine */
TEST_F(J2534ModulesTest, AVisitUsesTheReserveAndFreesIt)
{
    ASSERT_NO_FATAL_FAILURE(wide_session(true));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        for (uint32_t c = 0; c < 9; ++c) {
            if ((diagnostics(c) & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
        }
        return true;
    }));
    fake_control::clear_filters_peak(page);
    ASSERT_EQ(ecuio_session_read_diagnostics(session), ECUIO_OK);
    EXPECT_TRUE(wait_for([&] {
        return (diagnostics(9) & ECUIO_DIAG_COMPLETE) != 0 &&
               (diagnostics(10) & ECUIO_DIAG_COMPLETE) != 0;
    }));
    for (const uint32_t c : {9u, 10u}) {
        EXPECT_EQ(diagnostics(c) & ECUIO_DIAG_NOT_OPENED, 0u) << c;
    }
    EXPECT_EQ(peak(), 10u);
    EXPECT_TRUE(wait_for([&] { return active() == 9u; }, 2000)) << active();
}

/* The adapter holds nine: a visit's filter is refused, the read of each
 * module waiting ends NOT_OPENED, and the nine keep reading */
TEST_F(J2534ModulesTest, AVisitAfterARefusalEndsNotOpened)
{
    ASSERT_NO_FATAL_FAILURE(wide_session(true));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        for (uint32_t c = 0; c < 9; ++c) {
            if ((diagnostics(c) & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
        }
        return true;
    }));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_MAX", "9"));
    ASSERT_EQ(ecuio_session_read_diagnostics(session), ECUIO_OK);
    EXPECT_TRUE(wait_for([&] {
        return (diagnostics(9) & ECUIO_DIAG_NOT_OPENED) != 0 &&
               (diagnostics(10) & ECUIO_DIAG_NOT_OPENED) != 0;
    }));
    const int before = heard.of(wide_id(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(heard.of(wide_id(0)), before);
    EXPECT_EQ(active(), 9u);
    EXPECT_EQ(refused(), 1u) << "the second visit asked for a filter again";
}

/* A clear of a module waiting for a live filter (the tenth, answering as
 * the engine ECU) takes one visit, the 04 and the read after: its filter
 * starts once and stops once, the clear ends CLEARED, and the next clear
 * is taken. No set's signal reads 0C, so the clear allows a running
 * engine and a moving vehicle. */
TEST_F(J2534ModulesTest, AClearOfAModuleWaitingForAFilterStartsItsFilterOnce)
{
    constexpr uint32_t kFlags =
        ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING;
    ASSERT_NO_FATAL_FAILURE(wide_session(true, 9));
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        for (uint32_t c = 0; c < 9; ++c) {
            if ((diagnostics(c) & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
        }
        return true;
    }));
    ASSERT_EQ(ecuio_session_read_diagnostics(session), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] {
        return (diagnostics(9) & ECUIO_DIAG_COMPLETE) != 0 &&
               (diagnostics(10) & ECUIO_DIAG_COMPLETE) != 0 && active() == 9u;
    }));
    ASSERT_NE(support(9) & ECUIO_SUPPORT_NO_ROOM, 0u);
    fake_control::clear_filters_peak(page);
    ASSERT_EQ(ecuio_session_clear_diagnostics_with(
                  session, 9, report(9).generation, kFlags),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_TRUE(wait_for([&] {
        const ecuio_channel_diagnostics_t d = report(9);
        return d.clear == ECUIO_CLEAR_CLEARED &&
               (d.flags & ECUIO_DIAG_CLEARING) == 0 &&
               (d.flags & ECUIO_DIAG_COMPLETE) != 0 && active() == 9u;
    })) << "clear "
        << int{report(9).clear} << ", filters " << active();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(started(), 1u) << "the visit's filter started again";
    EXPECT_EQ(active(), 9u) << "the visit's filter not stopped once";
    EXPECT_NE(support(9) & ECUIO_SUPPORT_NO_ROOM, 0u) << "waiting again";
    ecuio_status_t again = ECUIO_ERR_IN_USE;
    EXPECT_TRUE(wait_for(
        [&] {
            again = ecuio_session_clear_diagnostics_with(
                session, 9, report(9).generation, kFlags);
            return again != ECUIO_ERR_IN_USE;
        },
        2000));
    EXPECT_EQ(again, ECUIO_OK) << "a later clear refused";
}

#endif /* _WIN32 */
