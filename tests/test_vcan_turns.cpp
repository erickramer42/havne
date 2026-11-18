// File/layer: tests/test_vcan_turns.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanTurnsTest (test_vcan)
//
// A busy ECU's pace on SocketCAN doesn't wait on another ECU's: the worker
// sends one request at a time but waits only for its echo, not its reply,
// so each channel's one outstanding request overlaps the others' and the
// channels' order (next_chan) decides only which of two due requests is
// written first. An adapter that asks one channel at a time (ELM327) takes
// turns instead (request_turns.h).

#include "ecuio/core.h"
#include "fake_ecus.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class SocketCanTurnsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string why;
        if (!vcan_bus::available(&why)) {
            const char* required = std::getenv("ECUIO_VCAN_REQUIRED");
            if (required != nullptr && std::string(required) == "1") {
                FAIL() << "ECUIO_VCAN_REQUIRED=1, but " << why;
            }
            GTEST_SKIP() << why;
        }
    }
};

/* What went on the bus: a request (on 0x7E0 or 0x7E1) or a reply */
struct Event {
    uint32_t id;
    bool request;
    Clock::time_point at;
};

/* Two ECUs played on one raw socket, each answering a Single Frame
 * 22 <DID> with 62 <DID> 2A delay later: the engine ECU (0x7E0 to 0x7E8)
 * and a module (0x7E1 to 0x7E9). Everything is logged in bus order. */
class SlowEcus
{
public:
    explicit SlowEcus(std::chrono::milliseconds delay)
        : fd_(vcan_bus::open_raw()), delay_(delay)
    {
        if (fd_.ok()) {
            thread_ = std::thread([this] { run(); });
        }
    }
    ~SlowEcus()
    {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    SlowEcus(const SlowEcus&) = delete;
    SlowEcus& operator=(const SlowEcus&) = delete;
    bool ok() const { return fd_.ok(); }
    std::vector<Event> events()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_;
    }

private:
    struct Due {
        Clock::time_point at;
        can_frame f;
    };

    void run()
    {
        std::vector<Due> due;
        while (!stop_) {
            can_frame f{};
            if (vcan_bus::read_for(fd_.get(), &f, sizeof f, 1) ==
                static_cast<long>(sizeof f)) {
                take(f, due);
            }
            const Clock::time_point now = Clock::now();
            for (size_t i = 0; i < due.size();) {
                if (due[i].at > now) {
                    ++i;
                    continue;
                }
                if (::write(fd_.get(), &due[i].f, sizeof due[i].f) ==
                    static_cast<ssize_t>(sizeof due[i].f)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    log_.push_back({due[i].f.can_id, false, Clock::now()});
                }
                due.erase(due.begin() + static_cast<long>(i));
            }
        }
    }

    void take(const can_frame& f, std::vector<Due>& due)
    {
        const uint32_t id = f.can_id & CAN_SFF_MASK;
        if ((f.can_id & (CAN_EFF_FLAG | CAN_ERR_FLAG)) != 0 ||
            (id != 0x7E0 && id != 0x7E1) || f.len < 4 || f.data[0] != 0x03 ||
            f.data[1] != 0x22) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            log_.push_back({id, true, Clock::now()});
        }
        Due d{Clock::now() + delay_, can_frame{}};
        d.f.can_id = id + 8;
        d.f.len = 8;
        const uint8_t reply[8] = {0x04, 0x62, f.data[2], f.data[3],
                                  0x2A, 0,    0,         0};
        std::memcpy(d.f.data, reply, sizeof reply);
        due.push_back(d);
    }

    vcan_bus::Fd fd_;
    std::chrono::milliseconds delay_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Event> log_;
};

} // namespace

/* Six DIDs on the engine ECU and one on a module, each ECU answering
 * 30 ms late: the module's requests go out while one of the engine ECU's
 * is out, so neither waits for the other's reply (taking turns, none
 * would overlap and each would wait both replies) */
TEST_F(SocketCanTurnsTest, AOneRequestEcuDoesntSlowABusyOne)
{
    SlowEcus ecus(30ms);
    ASSERT_TRUE(ecus.ok());
    const std::string uri =
        std::string("socketcan://") + vcan_bus::interface_name() +
        "?profile=uds-generic&did=7E8:D801,7E8:D802,7E8:D803,7E8:D804,"
        "7E8:D805,7E8:D806,7E9:D901&identify=0&diagnose=0";
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    const Clock::time_point end = Clock::now() + 10s;
    while (Clock::now() < end) {
        const std::vector<Event> ev = ecus.events();
        const auto asks = [&ev](uint32_t id) {
            return std::count_if(ev.begin(), ev.end(), [id](const Event& e) {
                return e.request && e.id == id;
            });
        };
        if (asks(0x7E0) >= 40 && asks(0x7E1) >= 20) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    ecuio_session_close(s);
    ecuio_session_destroy(s);

    const std::vector<Event> ev = ecus.events();
    bool engine_out = false;
    int module_asks = 0;
    int overlapped = 0;
    std::vector<int64_t> gaps;
    Clock::time_point last{};
    for (const Event& e : ev) {
        if (e.id == 0x7E0 || e.id == 0x7E8) {
            engine_out = e.request;
            if (e.request) {
                if (last != Clock::time_point{}) {
                    gaps.push_back(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            e.at - last)
                            .count());
                }
                last = e.at;
            }
        } else if (e.id == 0x7E1 && e.request) {
            ++module_asks;
            overlapped += engine_out ? 1 : 0;
        }
    }
    ASSERT_GE(module_asks, 20) << "the module was asked too seldom";
    ASSERT_GE(gaps.size(), 20u) << "the engine ECU was asked too seldom";
    EXPECT_GE(overlapped * 2, module_asks)
        << "the module's requests waited for the engine ECU's replies";
    std::sort(gaps.begin(), gaps.end());
    const int64_t median_us = gaps[gaps.size() / 2];
    EXPECT_GE(median_us, 30000) << "a request before its reply";
    EXPECT_LT(median_us, 4 * 30000) << "the engine ECU's pace";
}
