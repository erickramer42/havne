// File/layer: tests/test_vcan_memory.cpp | test-only, C++17, Qt-free,
//             Linux only
// Testing: HARNESS: SimMemoryBusTest (test_vcan)
//
// The simulated ECU's memory on the bus (core/src/sim_memory.h through
// sim_bus.cpp): the case as the tester through the kernel's ISO-TP
// socket, so a read's segmented reply crosses real frames at the pace
// cf= sets and an upload meets a reply the bus loses.

#include "ecuio/core.h"
#include "ecuio/sim.h"
#include "sim_memory.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using vcan_bus::Fd;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kBase = ECUIO_SIM_MEMORY_ADDRESS;

class Sim
{
public:
    explicit Sim(const std::string& uri)
    {
        status_ =
            ecuio_sim_create(uri.c_str(), vcan_bus::interface_name(), &sim_);
        detail_ = ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim_); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }

private:
    ecuio_sim_t* sim_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* A whole request on an ISO-TP socket and its whole reply within ms;
 * {} for none */
Bytes ask_on(const Fd& s, const Bytes& req, int ms = 500)
{
    if (::write(s.get(), req.data(), req.size()) !=
        static_cast<ssize_t>(req.size())) {
        ADD_FAILURE() << "the request's write failed: " << std::strerror(errno);
        return {};
    }
    uint8_t buf[4096];
    const long n = vcan_bus::read_for(s.get(), buf, sizeof buf, ms);
    return n > 0 ? Bytes(buf, buf + n) : Bytes{};
}

/* 23 44 <address> <size> */
Bytes read_of(uint32_t address, uint32_t size)
{
    return Bytes{0x23,
                 0x44,
                 static_cast<uint8_t>(address >> 24),
                 static_cast<uint8_t>(address >> 16),
                 static_cast<uint8_t>(address >> 8),
                 static_cast<uint8_t>(address),
                 static_cast<uint8_t>(size >> 24),
                 static_cast<uint8_t>(size >> 16),
                 static_cast<uint8_t>(size >> 8),
                 static_cast<uint8_t>(size)};
}

/* What the region holds from address on, the default VIN at its place */
Bytes region(uint32_t address, uint32_t size)
{
    ecuio_sim_memory m;
    m.vin = "ZZZFAKE11M0000001";
    Bytes want;
    for (uint32_t i = 0; i < size; ++i) {
        want.push_back(ecuio_sim_memory_byte(m, address + i));
    }
    return want;
}

/* The extended session entered and level 01 unlocked with the made-up
 * key (the seed reversed, each byte XORed with a letter of HAVNE) */
void unlock(const Fd& ecu)
{
    ASSERT_EQ(ask_on(ecu, {0x10, 0x03}).size(), 6u);
    const Bytes seed = ask_on(ecu, {0x27, 0x01});
    ASSERT_EQ(seed.size(), 6u);
    Bytes key = {0x27, 0x02};
    const char word[] = "HAVNE";
    for (size_t i = 0; i < 4; ++i) {
        key.push_back(static_cast<uint8_t>(seed[2 + 3 - i] ^ word[i]));
    }
    ASSERT_EQ(ask_on(ecu, key), (Bytes{0x67, 0x02}));
}

class SimMemoryBusTest : public ::testing::Test
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

// A read longer than a frame comes whole through real Consecutive
// Frames at the sim's pace, the longest one too
TEST_F(SimMemoryBusTest, AMultiFrameReadArrivesWholeUnderAFramePace)
{
    Sim sim("mock://sim?values=canned&cf=200");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd ecu = vcan_bus::open_isotp(vcan_bus::kRequest, vcan_bus::kReply);
    ASSERT_TRUE(ecu.ok());
    EXPECT_EQ(ask_on(ecu, read_of(kBase, 256)), (Bytes{0x7F, 0x23, 0x7F}));
    unlock(ecu);

    Bytes got = ask_on(ecu, read_of(kBase + 0x7FF0, 256), 2000);
    ASSERT_EQ(got.size(), 257u);
    EXPECT_EQ(got[0], 0x63);
    EXPECT_EQ(Bytes(got.begin() + 1, got.end()), region(kBase + 0x7FF0, 256))
        << "the VIN's place among them";

    got = ask_on(ecu, read_of(kBase, 4094), 5000);
    ASSERT_EQ(got.size(), 4095u) << "ISO-TP's longest message";
    EXPECT_EQ(Bytes(got.begin() + 1, got.end()), region(kBase, 4094));
}

// A block the bus loses ends an upload: the ECU's counter has moved on,
// so the block's counter asked again is refused, as is the next one's
// place in a tester that never saw the block
TEST_F(SimMemoryBusTest, AnUploadsBlockLostOnTheBusIsNotHandedOverAgain)
{
    /* The third segmented reply's frames come out of order, so the
     * kernel's receiver drops it: the two blocks before it arrive whole */
    Sim sim("mock://sim?values=canned&faults=bad_sequence:3");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd ecu = vcan_bus::open_isotp(vcan_bus::kRequest, vcan_bus::kReply);
    ASSERT_TRUE(ecu.ok());
    unlock(ecu);
    Bytes open = read_of(kBase, 4096);
    open[0] = 0x35;
    open.insert(open.begin() + 1, 0x00);
    ASSERT_EQ(ask_on(ecu, open), (Bytes{0x75, 0x20, 0x04, 0x02}));

    Bytes got = ask_on(ecu, {0x36, 0x01}, 2000);
    ASSERT_EQ(got.size(), 0x402u);
    EXPECT_EQ(Bytes(got.begin() + 2, got.end()), region(kBase, 1024));
    got = ask_on(ecu, {0x36, 0x02}, 2000);
    ASSERT_EQ(got.size(), 0x402u);
    EXPECT_EQ(Bytes(got.begin() + 2, got.end()), region(kBase + 1024, 1024));

    EXPECT_TRUE(ask_on(ecu, {0x36, 0x03}, 1500).empty()) << "the broken one";
    EXPECT_EQ(ask_on(ecu, {0x36, 0x03}), (Bytes{0x7F, 0x36, 0x73}))
        << "the ECU handed that block over once";
    EXPECT_EQ(ask_on(ecu, {0x37}), (Bytes{0x7F, 0x37, 0x24}))
        << "and bytes are still left";
}

/* A read held 6 s by its 78s, with or without a functional 3E 80 every
 * 2 s meanwhile: the session after it, as the next read finds it */
Bytes after_a_long_wait(bool keep)
{
    Sim sim("mock://sim?values=canned&faults=pending:4&pending_ms=6000:23");
    EXPECT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd ecu = vcan_bus::open_isotp(vcan_bus::kRequest, vcan_bus::kReply);
    Fd bus = vcan_bus::open_raw();
    EXPECT_TRUE(ecu.ok() && bus.ok());
    unlock(ecu);
    Bytes got = ask_on(ecu, read_of(kBase, 4));
    EXPECT_EQ(got, (Bytes{0x7F, 0x23, 0x78}));
    for (int i = 0; i < 6 && got.size() == 3 && got[2] == 0x78; ++i) {
        if (keep) {
            struct can_frame f;
            std::memset(&f, 0, sizeof f);
            f.can_id = vcan_bus::kFunctional;
            f.can_dlc = 8;
            f.data[0] = 0x02;
            f.data[1] = 0x3E;
            f.data[2] = 0x80;
            EXPECT_EQ(::write(bus.get(), &f, sizeof f),
                      static_cast<ssize_t>(sizeof f));
        }
        uint8_t buf[4096];
        const long n = vcan_bus::read_for(ecu.get(), buf, sizeof buf, 2500);
        got = n > 0 ? Bytes(buf, buf + n) : Bytes{};
    }
    EXPECT_EQ(got.size(), 5u) << "the read's own answer, 6 s on";
    return ask_on(ecu, read_of(kBase, 4));
}

// S3 runs through a module's 78s: a 3E 80 heard while it is busy holds
// the session, and the wait alone lets it lapse
TEST_F(SimMemoryBusTest, AQuietKeepAliveHoldsTheSessionThroughALongWait)
{
    const Bytes kept = after_a_long_wait(true);
    ASSERT_EQ(kept.size(), 5u);
    EXPECT_EQ(Bytes(kept.begin() + 1, kept.end()), region(kBase, 4));
    EXPECT_EQ(after_a_long_wait(false), (Bytes{0x7F, 0x23, 0x7F}))
        << "out of the session once S3 ran out";
}

} // namespace
