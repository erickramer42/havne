// File/layer: tests/test_j2534_hold_bus.cpp | test_j2534_health, HARNESS
// Testing: HARNESS: J2534HealthTest (Known Issue #42 on the fake DLL:
//          ECUIO_FAKE_LOST_UNDER_SEGMENTS at the loader seam, and the
//          worker's hold against it through the surrogate)

#include "j2534_health_fixture.h"

#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using j2534_health::J2534HealthTest;

namespace {

/* 0x7E8's 1E20 answers in two frames, 0x7E9's D801 in one (with the
 * 4-byte ID, the fake's rule: over 4 + 7 bytes is segmented) */
constexpr const char* kModules = "7E8:1E20=0102030405;7E9:D801=00";
constexpr const char* kTwoModules =
    "?profile=uds-generic&did=7E8:1E20,7E9:D801&identify=0&diagnose=0";
constexpr uint8_t kLong[7] = {0x00, 0x00, 0x07, 0xE0, 0x22, 0x1E, 0x20};
constexpr uint8_t kShort[7] = {0x00, 0x00, 0x07, 0xE1, 0x22, 0xD8, 0x01};
/* 0x7E8's refusal, 7F 22 31: a Single Frame of the long reply's own ID */
constexpr uint8_t kOwnShort[7] = {0x00, 0x00, 0x07, 0xE0, 0x22, 0x1E, 0x21};

/* The frames delivered from each module; outlives every session */
struct ById {
    std::atomic<int> e8{0};
    std::atomic<int> e9{0};
};
ById g_by_id;

void by_id_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<ById*>(user_data);
    if (frame->arbitration_id == 0x7E8) {
        c->e8.fetch_add(1);
    } else if (frame->arbitration_id == 0x7E9) {
        c->e9.fetch_add(1);
    }
}

bool is_long(const fake_control::Write& w)
{
    return w.size >= 4 && w.data[3] == 0xE0;
}

/* The writes the page recorded */
std::vector<fake_control::Write> recorded(fake_control::Page* page)
{
    const uint32_t n = fake_control::writes(page) < fake_control::kWriteDepth
                           ? fake_control::writes(page)
                           : fake_control::kWriteDepth;
    std::vector<fake_control::Write> ws(n);
    for (uint32_t i = 0; i < n; ++i) {
        EXPECT_TRUE(fake_control::recorded_write(page, i, &ws[i]));
    }
    return ws;
}

/* Whether a write to 0x7E1 falls between one of the recorded writes to
 * 0x7E0 from index first on and its reply's whole arrival (reply_us
 * after it) */
bool short_inside_a_window(fake_control::Page* page, uint32_t first,
                           uint32_t reply_us)
{
    const std::vector<fake_control::Write> ws = recorded(page);
    const uint32_t n = static_cast<uint32_t>(ws.size());
    for (uint32_t i = first; i < n; ++i) {
        for (uint32_t k = i + 1; is_long(ws[i]) && k < n; ++k) {
            if (!is_long(ws[k]) && ws[k].us - ws[i].us < reply_us) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

/* The knob at the loader seam, the fake's reply delay set for each write
 * (a long reply's First Frame comes at its write plus that delay, whole
 * 50 ms later): a Single Frame from another ID is lost when it arrives
 * inside a long reply's window, written after it or before it, and only
 * then; one of the long reply's own ID is kept; with the knob off
 * nothing is lost */
TEST_F(J2534HealthTest, TheFakeLosesASingleFrameUnderAnotherModulesSegments)
{
    struct HandleGuard {
        ecuio_j2534_handle_t* h = nullptr;
        ~HandleGuard() { (void)ecuio_j2534_close(h); }
    } guard;
    ecuio_j2534_device_t dev = {};
    strncpy_s(dev.dll_path, sizeof(dev.dll_path), dll.c_str(), _TRUNCATE);
    ASSERT_EQ(ecuio_j2534_open(&dev, &guard.h), ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    unsigned long ch = 0;
    ASSERT_EQ(ecuio_j2534_connect_channel(guard.h, 0, 0, &ch), ECUIO_OK);
    const uint8_t mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t rx[4] = {0x00, 0x00, 0x07, 0xE8};
    const uint8_t tx[4] = {0x00, 0x00, 0x07, 0xE0};
    unsigned long fid = 0;
    ASSERT_EQ(ecuio_j2534_start_msg_filter(guard.h, ch,
                                           ECUIO_PT_FLOW_CONTROL_FILTER, mask,
                                           4, rx, 4, tx, 4, 0, &fid),
              ECUIO_OK);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MODULE_DIDS", kModules));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "50"));

    /* Two writes, the second gap_ms after the first, each with its reply
     * delay; "after" and "before": the Single Frame written after the
     * long request or before it. The replies read from each ID. */
    struct Case {
        const char* name;
        bool lose;
        const uint8_t* first;
        int first_ms;
        int gap_ms;
        const uint8_t* second;
        int second_ms;
        int e8;
        int e9;
    };
    const Case cases[] = {
        {"off", false, kLong, 4, 0, kShort, 4, 1, 1},
        {"after, inside", true, kLong, 4, 0, kShort, 4, 1, 0},
        {"before, inside", true, kShort, 60, 20, kLong, 4, 1, 0},
        {"after, before the First Frame", true, kLong, 30, 0, kShort, 4, 1, 1},
        {"after, past the whole reply", true, kLong, 4, 0, kShort, 80, 1, 1},
        {"before, before the First Frame", true, kShort, 4, 20, kLong, 4, 1, 1},
        {"before, past the whole reply", true, kShort, 100, 0, kLong, 4, 1, 1},
        {"the long reply's own ID, inside", true, kLong, 4, 0, kOwnShort, 4, 2,
         0},
    };
    uint32_t lost = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_NO_FATAL_FAILURE(
            knob("ECUIO_FAKE_LOST_UNDER_SEGMENTS", c.lose ? "1" : "0"));
        for (int k = 0; k < 2; ++k) {
            const std::string ms =
                std::to_string(k == 0 ? c.first_ms : c.second_ms);
            ASSERT_NO_FATAL_FAILURE(
                knob("ECUIO_FAKE_REPLY_DELAY_MS", ms.c_str()));
            uint32_t written = 0;
            ASSERT_EQ(ecuio_j2534_write_frame(guard.h, ch,
                                              k == 0 ? c.first : c.second, 7, 0,
                                              0, &written),
                      ECUIO_OK);
            ASSERT_EQ(written, 1u);
            if (k == 0 && c.gap_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(c.gap_ms));
            }
        }
        /* Every reply has arrived 150 ms after the last write */
        int e8 = 0;
        int e9 = 0;
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < end) {
            ecuio_pt_rx_t f = {};
            bool got = false;
            ASSERT_EQ(ecuio_j2534_read_msg(guard.h, ch, &f, 10, &got),
                      ECUIO_OK);
            if (!got) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            ASSERT_GE(f.data_size, 4u);
            e8 += f.data[3] == 0xE8 ? 1 : 0;
            e9 += f.data[3] == 0xE9 ? 1 : 0;
        }
        lost += c.e8 + c.e9 < 2 ? 1u : 0u;
        EXPECT_EQ(e8, c.e8) << "from 0x7E8";
        EXPECT_EQ(e9, c.e9) << "from 0x7E9";
        EXPECT_EQ(fake_control::lost_under_segments(page), lost);
    }
    EXPECT_EQ(ecuio_j2534_stop_msg_filter(guard.h, ch, fid), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_disconnect_channel(guard.h, ch), ECUIO_OK);
}

/* Two modules on the VX losing as Known Issue #42 does: with the hold,
 * once the first lap has learned 0x7E8's reply is segmented, no reply is
 * lost, no write to 0x7E1 lands inside a 0x7E8 reply's window, and both
 * modules keep a pace. The first lap's loss leaves 0x7E1 silent until its
 * next reply, and a silent module's request holds no one, so 0x7E0's next
 * goes beside it; from the lap after, 0x7E0's request waits for 0x7E1's
 * reply again (the reply delay, 4 ms, after its write). */
TEST_F(J2534HealthTest, VxTheHoldLosesNoReplyUnderSegments)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MODULE_DIDS", kModules));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "6"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LOST_UNDER_SEGMENTS", "1"));
    g_by_id.e8 = 0;
    g_by_id.e9 = 0;
    ASSERT_NO_FATAL_FAILURE(start_session(kTwoModules));
    ASSERT_EQ(ecuio_register_frame_callback(session, by_id_cb, &g_by_id),
              ECUIO_OK);
    ASSERT_TRUE(wait_for_frames(10, 5000)) << "replies didn't arrive";
    /* The first lap may lose 0x7E9's reply: 0x7E8's isn't known long */
    const uint32_t first_lap = fake_control::lost_under_segments(page);
    EXPECT_LE(first_lap, 1u);
    const int e8 = g_by_id.e8.load();
    const int e9 = g_by_id.e9.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    EXPECT_EQ(fake_control::lost_under_segments(page), first_lap);
    EXPECT_GE(g_by_id.e8.load() - e8, 10) << "0x7E8's pace";
    EXPECT_GE(g_by_id.e9.load() - e9, 10) << "0x7E9's pace";
    close_session();
    EXPECT_FALSE(short_inside_a_window(page, 2, (4 + 6) * 1000));
    const std::vector<fake_control::Write> ws = recorded(page);
    for (size_t k = 6; k < ws.size(); ++k) {
        if (is_long(ws[k]) && !is_long(ws[k - 1])) {
            EXPECT_GE(ws[k].us - ws[k - 1].us, 3000u)
                << "write " << k << " didn't wait for 0x7E1's reply";
        }
    }
}

/* With the hold, a module it held asks next: each write to 0x7E0 is
 * followed by one to 0x7E1 before the next to 0x7E0 */
TEST_F(J2534HealthTest, VxTheHoldGivesTheHeldModuleTheNextTurn)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MODULE_DIDS", kModules));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "6"));
    ASSERT_NO_FATAL_FAILURE(start_session(kTwoModules));
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake_control::writes(page) < fake_control::kWriteDepth &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close_session();
    ASSERT_GE(fake_control::writes(page), fake_control::kWriteDepth);
    bool long_waiting = false;
    for (uint32_t i = 0; i < fake_control::kWriteDepth; ++i) {
        fake_control::Write w{};
        ASSERT_TRUE(fake_control::recorded_write(page, i, &w));
        if (is_long(w)) {
            EXPECT_FALSE(long_waiting) << "write " << i << ": 0x7E1 skipped";
        }
        long_waiting = is_long(w);
    }
}

/* ?hold=0 on the same bus: replies are lost and 0x7E1 is asked inside
 * 0x7E8's windows, which pins the key and shows the session cases see
 * the knob */
TEST_F(J2534HealthTest, VxHoldOffLosesUnderSegments)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_VX", "1"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MODULE_DIDS", kModules));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "6"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LOST_UNDER_SEGMENTS", "1"));
    const std::string query = std::string(kTwoModules) + "&hold=0";
    ASSERT_NO_FATAL_FAILURE(start_session(query.c_str()));
    ASSERT_TRUE(wait_for_frames(10, 5000)) << "replies didn't arrive";
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake_control::lost_under_segments(page) < 3 &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close_session();
    EXPECT_GE(fake_control::lost_under_segments(page), 3u);
    EXPECT_TRUE(short_inside_a_window(page, 2, (4 + 6) * 1000));
}

/* A held module, or one whose chosen request waits, waits on the other's
 * reply: the worker reads on the slice, not 1 ms after 1 ms for a floor
 * long past, over passes too (each reply 40 ms, four of the 10 ms
 * slices: about 5 reads for each write, 14 with each pass reading 1 ms).
 * The fake's default mode answers an empty read at once, so the reads the
 * page counts follow the read timeouts the worker asks. */
TEST_F(J2534HealthTest, AHeldModuleWaitsOnTheOthersReplyNotItsFloor)
{
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_MODULE_DIDS", kModules));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_REPLY_DELAY_MS", "40"));
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_LONG_REPLY_DELAY_MS", "6"));
    ASSERT_NO_FATAL_FAILURE(start_session(kTwoModules));
    ASSERT_TRUE(wait_for_frames(10, 5000)) << "replies didn't arrive";
    const uint32_t reads0 = fake_control::reads(page);
    const uint32_t writes0 = fake_control::writes(page);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const uint32_t reads = fake_control::reads(page) - reads0;
    const uint32_t writes = fake_control::writes(page) - writes0;
    close_session();
    ASSERT_GE(writes, 10u);
    EXPECT_LE(reads, 9 * writes) << reads << " reads for " << writes;
}
