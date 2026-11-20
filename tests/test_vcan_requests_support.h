// File/layer: tests/test_vcan_requests_support.h | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: the helpers of test_vcan_requests*.cpp (test_vcan)

#pragma once

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/sim.h"
#include "car_sized_set.h"
#include "fake_diagnostics.h"
#include "fake_ecus.h"
#include "vcan_bus.h"
#include "vcan_recording_ecu.h"

#include <gtest/gtest.h>

#include <linux/can/error.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <set>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

} // namespace

/* A started session on the bus, closed and destroyed when it goes; a
 * frame callback, when given, sees it from its first frame */
class Session
{
public:
    explicit Session(const std::string& query,
                     ecuio_frame_callback_t callback = nullptr,
                     void* user_data = nullptr)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
        if (status_ == ECUIO_OK && callback != nullptr) {
            status_ = ecuio_register_frame_callback(s_, callback, user_data);
        }
        if (status_ == ECUIO_OK) {
            status_ = ecuio_session_start(s_);
        }
    }
    ~Session()
    {
        if (s_ != nullptr) {
            ecuio_session_close(s_);
            ecuio_session_destroy(s_);
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_session_t* get() const { return s_; }

private:
    ecuio_session_t* s_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

namespace {

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

} // namespace

class SocketCanRequestTest : public ::testing::Test
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
        fake_ecu::reset_diagnostics();
    }
    void TearDown() override { fake_ecu::reset_diagnostics(); }
};

/* A frame of up to 8 bytes onto the bus from a raw socket */
inline bool put(int fd, canid_t id, const std::vector<uint8_t>& data)
{
    can_frame f{};
    f.can_id = id;
    f.len = static_cast<uint8_t>(data.size());
    std::memcpy(f.data, data.data(), data.size());
    return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
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

inline int32_t state_of(const Session& s, uint32_t* episodes = nullptr)
{
    int32_t last = -1;
    uint32_t count = 0;
    EXPECT_EQ(ecuio_session_get_error_state(s.get(), &last, &count), ECUIO_OK);
    if (episodes != nullptr) {
        *episodes = count;
    }
    return last;
}

} // namespace
