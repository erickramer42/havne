// File/layer: tests/test_vcan_socketcan.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: the SocketCAN transport (ADR-0032) on the simulated
//          bus (vcan_bus.h): discovery, frames in order with their flags,
//          the filter, the mock's drive cycle decoding off the bus, a
//          quiet bus and back, an overflow, error frames (flagged, never
//          decoded, bus-off a hard failure), no allocation per frame, and
//          (as root) the interface going down and coming back. Each case
//          skips without the interface; ECUIO_VCAN_REQUIRED=1 makes that a
//          failure. Holds the binary's new_counter.h.

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/socketcan.h"
#include "new_counter.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <linux/can/error.h>

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

/* A frame as the tests compare it, without the 4 KB payload */
struct Seen {
    uint32_t id;
    uint32_t flags;
    uint64_t t_us;
    std::vector<uint8_t> data;
};

/* What a session delivered, filled on its worker */
struct Collector {
    std::mutex mutex;
    std::vector<Seen> frames;
    std::vector<std::vector<double>> values; /* each batch's numbers */
    std::chrono::milliseconds stall_first{0};
    std::atomic<unsigned long> news_at_10{0}, news_at_200{0};
    std::atomic<int> count{0};
    std::atomic<int> overflow_at{0}; /* the first frame flagged, 1-based */

    static void on_frame(const ecuio_frame_t* f, void* user)
    {
        auto* c = static_cast<Collector*>(user);
        const int n = ++c->count;
        if (n == 1 && c->stall_first.count() > 0) {
            std::this_thread::sleep_for(c->stall_first);
        }
        if ((f->flags & ECUIO_FRAME_FLAG_OVERFLOW) &&
            c->overflow_at.load() == 0) {
            c->overflow_at = n;
        }
        if (n <= 10) { /* the first ten kept; past them only counted */
            std::lock_guard<std::mutex> lock(c->mutex);
            c->frames.push_back(
                {f->arbitration_id, f->flags, f->timestamp_us,
                 std::vector<uint8_t>(f->data, f->data + f->data_length)});
        }
        if (n == 10) {
            c->news_at_10 = new_counter::on_this_thread();
        } else if (n == 200) {
            c->news_at_200 = new_counter::on_this_thread();
        }
    }

    static void on_values(const ecuio_value_batch_t* b, void* user)
    {
        auto* c = static_cast<Collector*>(user);
        std::vector<double> v;
        for (uint32_t i = 0; i < b->count; ++i) {
            v.push_back(b->values[i].value);
        }
        std::lock_guard<std::mutex> lock(c->mutex);
        c->values.push_back(std::move(v));
    }

    std::vector<Seen> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames;
    }
};

/* A started session on the bus, closed and destroyed when it goes */
class Session
{
public:
    explicit Session(const std::string& query, Collector* c,
                     bool values = false)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
        if (status_ != ECUIO_OK) {
            return;
        }
        ecuio_register_frame_callback(s_, Collector::on_frame, c);
        if (values) {
            ecuio_register_values_callback(s_, Collector::on_values, c);
        }
        status_ = ecuio_session_start(s_);
    }
    ~Session()
    {
        if (s_ != nullptr) {
            ecuio_session_close(s_);
            ecuio_session_destroy(s_);
        }
    }
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_session_t* get() const { return s_; }
    int32_t state() const
    {
        int32_t e = 0;
        uint32_t n = 0;
        ecuio_session_get_error_state(s_, &e, &n);
        return e;
    }

private:
    ecuio_session_t* s_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* A raw socket that writes classic and FD frames */
vcan_bus::Fd writer()
{
    vcan_bus::Fd fd = vcan_bus::open_raw();
    const int on = 1;
    if (fd.ok()) {
        ::setsockopt(fd.get(), SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof on);
    }
    return fd;
}

bool send(int fd, canid_t id, const std::vector<uint8_t>& data,
          bool fd_frame = false, uint8_t fd_flags = 0)
{
    if (fd_frame) {
        canfd_frame f{};
        f.can_id = id;
        f.len = static_cast<uint8_t>(data.size());
        f.flags = fd_flags;
        if (!data.empty()) {
            std::memcpy(f.data, data.data(), data.size());
        }
        return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
    }
    can_frame f{};
    f.can_id = id;
    f.len = static_cast<uint8_t>(data.size());
    // An RTR frame has no data, and an empty vector's data() may be null,
    // which memcpy mustn't get even for no bytes (UBSan's nonnull check)
    if (!data.empty()) {
        std::memcpy(f.data, data.data(), data.size());
    }
    return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
}

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = 3000ms)
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

class SocketCanBusTest : public ::testing::Test
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

} // namespace

TEST_F(SocketCanBusTest, DiscoveryListsTheBus)
{
    ecuio_can_interface_t list[32];
    uint32_t count = 0;
    ASSERT_EQ(ecuio_can_interfaces(list, 32, &count), ECUIO_OK);
    bool found = false;
    for (uint32_t i = 0; i < count && i < 32; ++i) {
        if (std::strcmp(list[i].name, vcan_bus::interface_name()) == 0) {
            found = true;
            EXPECT_STREQ(list[i].kind, "vcan");
            EXPECT_TRUE(list[i].flags & ECUIO_CAN_UP);
            EXPECT_FALSE(list[i].flags & ECUIO_CAN_LISTEN_ONLY);
            EXPECT_EQ(list[i].bitrate, 0u);
        }
    }
    EXPECT_TRUE(found) << vcan_bus::interface_name() << " not listed";
}

TEST_F(SocketCanBusTest, FramesArriveInOrderWithTheirFlags)
{
    Collector c;
    Session s("?filter=0x123,0x321,0x0C0,0x18DAF110", &c);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.backend, ECUIO_BACKEND_SOCKETCAN);
    EXPECT_STREQ(info.profile, "raw");
    EXPECT_EQ(info.baud, 0u);
    EXPECT_FALSE(info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY)
        << "vcan has no controller";

    vcan_bus::Fd w = writer();
    ASSERT_TRUE(w.ok());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(send(w.get(), 0x123, {1, 2, 3, 4, 5, 6, 7, 8}));
    ASSERT_TRUE(send(w.get(), 0x18DAF110 | CAN_EFF_FLAG, {0x02, 0x10}));
    ASSERT_TRUE(send(w.get(), 0x321 | CAN_RTR_FLAG, {}));
    std::vector<uint8_t> twelve(12);
    for (size_t i = 0; i < twelve.size(); ++i) {
        twelve[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ASSERT_TRUE(send(w.get(), 0x0C0, twelve, true, CANFD_BRS));
    ASSERT_TRUE(wait_for([&] { return c.count.load() >= 4; }))
        << c.count.load();

    const auto f = c.snapshot();
    ASSERT_EQ(f.size(), 4u);
    EXPECT_EQ(f[0].id, 0x123u);
    EXPECT_EQ(f[0].flags, 0u);
    EXPECT_EQ(f[0].data, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(f[1].id, 0x18DAF110u);
    EXPECT_EQ(f[1].flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(f[1].data, (std::vector<uint8_t>{0x02, 0x10}));
    EXPECT_EQ(f[2].id, 0x321u);
    EXPECT_EQ(f[2].flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_REMOTE));
    EXPECT_TRUE(f[2].data.empty());
    EXPECT_EQ(f[3].id, 0x0C0u);
    EXPECT_EQ(f[3].flags,
              ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_BITRATE_SWITCH);
    EXPECT_EQ(f[3].data, twelve);
    for (size_t i = 1; i < f.size(); ++i) {
        EXPECT_GE(f[i].t_us, f[i - 1].t_us) << "frame " << i;
    }
    EXPECT_EQ(s.state(), ECUIO_OK);
}

TEST_F(SocketCanBusTest, TheFilterKeepsOnlyItsIdsAndTheirWidth)
{
    Collector c;
    Session s("?filter=0x7E8,0x18DA00F1/0x1FFF00FF", &c);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    vcan_bus::Fd w = writer();
    ASSERT_TRUE(w.ok());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(send(w.get(), 0x7E0, {1})); /* not listed */
    ASSERT_TRUE(send(w.get(), 0x7E8, {2})); /* listed */
    ASSERT_TRUE(send(w.get(), 0x7E8 | CAN_EFF_FLAG,
                     {3})); /* 29-bit 0x7E8: another width */
    ASSERT_TRUE(
        send(w.get(), 0x18DA10F1 | CAN_EFF_FLAG, {4})); /* passes the mask */
    ASSERT_TRUE(send(w.get(), 0x18DA10F2 | CAN_EFF_FLAG, {5})); /* doesn't */
    ASSERT_TRUE(wait_for([&] { return c.count.load() >= 2; }));
    std::this_thread::sleep_for(200ms);
    const auto f = c.snapshot();
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].data, std::vector<uint8_t>{2});
    EXPECT_EQ(f[1].id, 0x18DA10F1u);
    EXPECT_EQ(f[1].data, std::vector<uint8_t>{4});
}

TEST_F(SocketCanBusTest, TheMocksDriveCycleDecodesOffTheBus)
{
    /* The mock transport's own frames and values for 30 ticks */
    struct Taken {
        std::mutex mutex;
        std::vector<Seen> frames;
        std::vector<std::vector<double>> values;
        bool taking = false; /* the frame now dispatching was kept */
    } mock;
    ecuio_session_t* m = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://bus", &m), ECUIO_OK);
    ecuio_register_frame_callback(
        m,
        [](const ecuio_frame_t* f, void* u) {
            auto* t = static_cast<Taken*>(u);
            std::lock_guard<std::mutex> lock(t->mutex);
            t->taking = t->frames.size() < 30;
            if (t->taking) {
                t->frames.push_back(
                    {f->arbitration_id, f->flags, 0,
                     std::vector<uint8_t>(f->data, f->data + f->data_length)});
            }
        },
        &mock);
    ecuio_register_values_callback(
        m,
        [](const ecuio_value_batch_t* b, void* u) {
            auto* t = static_cast<Taken*>(u);
            std::vector<double> v;
            for (uint32_t i = 0; i < b->count; ++i) {
                v.push_back(b->values[i].value);
            }
            std::lock_guard<std::mutex> lock(t->mutex);
            if (t->taking) { /* a batch follows its frame (ADR-0024) */
                t->values.push_back(std::move(v));
            }
        },
        &mock);
    ASSERT_EQ(ecuio_session_start(m), ECUIO_OK);
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(mock.mutex);
            return mock.frames.size() >= 30;
        },
        5000ms));
    ecuio_session_close(m);
    ecuio_session_destroy(m);

    /* The same frames on the bus, heard through ?profile=mock */
    Collector c;
    Session s("?profile=mock&filter=0x0C0,0x18FF0C00", &c, true);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    vcan_bus::Fd w = writer();
    ASSERT_TRUE(w.ok());
    std::this_thread::sleep_for(50ms);
    for (const Seen& f : mock.frames) {
        const canid_t id = (f.flags & ECUIO_FRAME_FLAG_EXTENDED)
                               ? (f.id | CAN_EFF_FLAG)
                               : f.id;
        ASSERT_TRUE(
            send(w.get(), id, f.data, (f.flags & ECUIO_FRAME_FLAG_FD) != 0));
    }
    ASSERT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> lock(c.mutex);
        return c.values.size() >= mock.values.size();
    }));
    std::lock_guard<std::mutex> lock(c.mutex);
    ASSERT_FALSE(mock.values.empty());
    ASSERT_EQ(c.values.size(), mock.values.size());
    for (size_t i = 0; i < mock.values.size(); ++i) {
        EXPECT_EQ(c.values[i], mock.values[i]) << "batch " << i;
    }
}

TEST_F(SocketCanBusTest, AQuietBusIsVehicleTimeoutUntilAFrameComes)
{
    Collector c;
    Session s("?filter=0x5A5", &c); /* an ID nothing else here sends */
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    EXPECT_TRUE(wait_for([&] {
        return s.state() == ECUIO_ERR_VEHICLE_TIMEOUT;
    })) << "state "
        << s.state();
    vcan_bus::Fd w = writer();
    ASSERT_TRUE(send(w.get(), 0x5A5, {1}));
    EXPECT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }))
        << "state " << s.state();
    int32_t e = 0;
    uint32_t episodes = 99;
    ecuio_session_get_error_state(s.get(), &e, &episodes);
    EXPECT_EQ(episodes, 0u) << "a quiet bus is no failure";
}

TEST_F(SocketCanBusTest, FramesLostToAFullSocketAreFlagged)
{
    Collector c;
    c.stall_first = 500ms; /* the worker stops reading while the bus floods */
    Session s("?filter=0x5A6", &c);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    vcan_bus::Fd w = writer();
    std::this_thread::sleep_for(50ms);
    constexpr int kSent = 5000;
    for (int i = 0; i < kSent; ++i) {
        const uint8_t b0 = static_cast<uint8_t>(i),
                      b1 = static_cast<uint8_t>(i >> 8);
        if (!send(w.get(), 0x5A6, {b0, b1})) {
            std::this_thread::sleep_for(
                1ms); /* ENOBUFS: the writer's own queue */
            --i;
        }
    }
    ASSERT_TRUE(wait_for([&] { return c.count.load() >= 10; }));
    std::this_thread::sleep_for(800ms);
    const int before = c.count.load();
    EXPECT_LT(before, kSent)
        << "the socket held the whole flood: make it bigger";
    EXPECT_EQ(c.overflow_at.load(), 0)
        << "flagged before any frame queued after the loss";
    /* The kernel stamps a frame with the drop count when it queues it, so
     * the first one queued after the loss carries it */
    ASSERT_TRUE(send(w.get(), 0x5A6, {0xFF, 0xFF}));
    ASSERT_TRUE(wait_for([&] { return c.count.load() > before; }));
    EXPECT_EQ(c.overflow_at.load(), before + 1)
        << "the frame after the loss carries OVERFLOW";
    EXPECT_EQ(s.state(), ECUIO_OK) << "a loss isn't a link failure";
}

TEST_F(SocketCanBusTest, TheWorkerAllocatesNothingPerFrame)
{
    Collector c;
    Session s("?filter=0x5A7", &c);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    vcan_bus::Fd w = writer();
    std::this_thread::sleep_for(50ms);
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(send(w.get(), 0x5A7, {static_cast<uint8_t>(i)}));
        if (i % 50 == 49) {
            std::this_thread::sleep_for(20ms);
        }
    }
    ASSERT_TRUE(wait_for([&] { return c.count.load() >= 200; }));
    EXPECT_EQ(c.news_at_200.load(), c.news_at_10.load())
        << "operator new on the worker between frames 10 and 200";
}

/* Error frames pass every session's ID filter (CAN_RAW_ERR_FILTER), so
 * this case's reach the other cases' sessions only if they ran at once:
 * test_vcan runs serially (RUN_SERIAL) */
TEST_F(SocketCanBusTest, ErrorFramesAreFlaggedAndOnlyBusOffFails)
{
    Collector c;
    Session s("?profile=mock&filter=0x5A9", &c, true);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    vcan_bus::Fd w = writer();
    ASSERT_TRUE(w.ok());
    std::this_thread::sleep_for(50ms);

    /* The controller's warning: reported, and the link stands */
    ASSERT_TRUE(send(w.get(), CAN_ERR_FLAG | CAN_ERR_CRTL,
                     {0, CAN_ERR_CRTL_RX_WARNING, 0, 0, 0, 0, 0, 0}));
    ASSERT_TRUE(wait_for([&] { return c.count.load() >= 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_NE(s.state(), ECUIO_ERR_TRANSPORT_HARD_FAILURE);

    /* Bus-off with a bus error: its class bits are 0x0C0, the mock's
     * telemetry ID, and its detail bytes decode as no signal */
    const std::vector<uint8_t> detail{0x10, 0x20, 0x30, 0x40,
                                      0x50, 0x60, 0x70, 0x80};
    ASSERT_TRUE(send(w.get(), CAN_ERR_FLAG | CAN_ERR_BUSERROR | CAN_ERR_BUSOFF,
                     detail));
    EXPECT_TRUE(wait_for([&] {
        return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
    })) << "state "
        << s.state();

    /* A data frame heard: the link is back, still one episode */
    ASSERT_TRUE(send(w.get(), 0x5A9, {1}));
    EXPECT_TRUE(wait_for([&] { return s.state() == ECUIO_OK; }))
        << "state " << s.state();
    int32_t e = 0;
    uint32_t episodes = 0;
    ecuio_session_get_error_state(s.get(), &e, &episodes);
    EXPECT_EQ(episodes, 1u);

    const auto f = c.snapshot();
    ASSERT_EQ(f.size(), 3u);
    EXPECT_EQ(f[0].id, static_cast<uint32_t>(CAN_ERR_CRTL));
    EXPECT_EQ(f[0].flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_ERROR));
    EXPECT_EQ(f[0].data[1], CAN_ERR_CRTL_RX_WARNING);
    EXPECT_EQ(f[1].id, 0x0C0u);
    EXPECT_EQ(f[1].flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_ERROR));
    EXPECT_EQ(f[1].data, detail);
    EXPECT_EQ(f[2].id, 0x5A9u);
    EXPECT_EQ(f[2].flags, 0u);
    std::lock_guard<std::mutex> lock(c.mutex);
    EXPECT_TRUE(c.values.empty())
        << c.values.size() << " batches decoded from error frames";
}

/* Needs CAP_NET_ADMIN to set the interface down: CI's linux-build runs it
 * again with sudo and ECUIO_VCAN_ROOT_REQUIRED=1, which makes a skip a
 * failure */
TEST_F(SocketCanBusTest, TheInterfaceGoingDownIsAHardFailureThatRecovers)
{
    if (::geteuid() != 0) {
        const char* required = std::getenv("ECUIO_VCAN_ROOT_REQUIRED");
        if (required != nullptr && std::string(required) == "1") {
            FAIL() << "ECUIO_VCAN_ROOT_REQUIRED=1, but not root";
        }
        GTEST_SKIP() << "needs root to set " << vcan_bus::interface_name()
                     << " down";
    }
    Collector c;
    Session s("?filter=0x5A8", &c);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const std::string name = vcan_bus::interface_name();
    ASSERT_EQ(std::system(("ip link set " + name + " down").c_str()), 0);
    const bool failed =
        wait_for([&] { return s.state() == ECUIO_ERR_TRANSPORT_HARD_FAILURE; });
    ASSERT_EQ(std::system(("ip link set " + name + " up").c_str()), 0);
    EXPECT_TRUE(failed) << "state " << s.state();
    /* Back up: the worker reopens on its backoff, and a frame is heard */
    vcan_bus::Fd w = writer();
    ASSERT_TRUE(wait_for(
        [&] {
            send(w.get(), 0x5A8, {1});
            return s.state() == ECUIO_OK;
        },
        8000ms))
        << "state " << s.state();
    int32_t e = 0;
    uint32_t episodes = 0;
    ecuio_session_get_error_state(s.get(), &e, &episodes);
    EXPECT_EQ(episodes, 1u);
}
