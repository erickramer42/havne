// File/layer: tests/test_vcan_isotp.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: core's ISO-TP (core/src/isotp.h, ADR-0040 section 1)
//          against the kernel's can-isotp on vcan0, an implementation of
//          its own: core's receiver reassembles what the kernel sends at
//          every length to 4095 bytes, answering its First Frame with
//          Flow Control; core's sender delivers to the kernel's receiver
//          under that receiver's block size and STmin, the separation
//          kept on the wire; a sender slower than N_Cr is dropped. Each
//          case skips without the interface; ECUIO_VCAN_REQUIRED=1 makes
//          that a failure.

#include "isotp.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <linux/can/isotp.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

/* IDs of the tests' own, apart from the ECU's 0x7E0/0x7E8 */
constexpr uint32_t kTester = 0x6A0; /* core's side sends on this */
constexpr uint32_t kPeer = 0x6A8;   /* the kernel's side sends on this */

int64_t us_since(steady::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(steady::now() -
                                                                 t0)
        .count();
}

std::vector<uint8_t> counting(size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(i * 13 + n);
    }
    return v;
}

bool write_can(int fd, const isotp_can& f)
{
    struct can_frame cf{};
    cf.can_id = f.id;
    cf.len = f.len;
    std::memcpy(cf.data, f.data, f.len);
    return ::write(fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
}

/* The next frame on id within ms, as core takes one */
bool read_can(int fd, uint32_t id, int ms, isotp_can* out)
{
    const auto end = steady::now() + std::chrono::milliseconds(ms);
    while (steady::now() < end) {
        struct can_frame cf;
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                  steady::now())
                .count());
        if (vcan_bus::read_for(fd, &cf, sizeof cf, left > 0 ? left : 1) !=
            static_cast<long>(sizeof cf)) {
            continue;
        }
        if ((cf.can_id & CAN_SFF_MASK) != id) {
            continue;
        }
        out->id = id;
        out->len = cf.len;
        std::memcpy(out->data, cf.data, cf.len);
        return true;
    }
    return false;
}

/* The kernel's ISO-TP socket on the peer's side, with its receiver's Flow
 * Control and, when frame_txtime_ns is set, its sender's pace forced */
vcan_bus::Fd peer(uint8_t bs = 0, uint8_t stmin = 0,
                  uint32_t frame_txtime_ns = 0)
{
    vcan_bus::Fd s(::socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP));
    if (!s.ok()) {
        return s;
    }
    can_isotp_options opts{};
    opts.flags = CAN_ISOTP_TX_PADDING;
    opts.txpad_content = 0xCC;
    if (frame_txtime_ns != 0) {
        opts.flags |= CAN_ISOTP_FORCE_TXSTMIN;
        opts.frame_txtime = frame_txtime_ns;
    }
    can_isotp_fc_options fc{};
    fc.bs = bs;
    fc.stmin = stmin;
    fc.wftmax = 0;
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex =
        static_cast<int>(if_nametoindex(vcan_bus::interface_name()));
    addr.can_addr.tp.tx_id = kPeer;
    addr.can_addr.tp.rx_id = kTester;
    if (::setsockopt(s.get(), SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts,
                     sizeof opts) != 0 ||
        ::setsockopt(s.get(), SOL_CAN_ISOTP, CAN_ISOTP_RECV_FC, &fc,
                     sizeof fc) != 0 ||
        ::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        s.reset();
    }
    return s;
}

/* A raw socket on the tester's side that hears only the peer */
vcan_bus::Fd tester()
{
    vcan_bus::Fd s = vcan_bus::open_raw();
    if (s.ok()) {
        struct can_filter f;
        f.can_id = kPeer;
        f.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG;
        ::setsockopt(s.get(), SOL_CAN_RAW, CAN_RAW_FILTER, &f, sizeof f);
    }
    return s;
}

/* Core's receiver takes what the kernel sends: each frame as it comes,
 * Flow Control written on its First Frame, until the message is whole or
 * nothing comes for a second. The message, or empty. */
std::vector<uint8_t> receive_from_kernel(int raw, isotp_rx& rx,
                                         steady::time_point t0)
{
    isotp_can f;
    while (read_can(raw, kPeer, 1000, &f)) {
        isotp_message m;
        const isotp_result res = isotp_take(rx, f, us_since(t0), &m);
        if (res == isotp_result::first) {
            if (!write_can(raw, isotp_flow_control(kTester))) {
                return {};
            }
        } else if (res == isotp_result::complete) {
            return std::vector<uint8_t>(m.data, m.data + m.len);
        } else if (res != isotp_result::pending) {
            return {};
        }
    }
    return {};
}

/* Core's sender to the kernel's receiver, in real time: the first frame,
 * then each Flow Control read and each Consecutive Frame written when
 * isotp_tx_next says. The time each Consecutive Frame was due, in us (the
 * time the sender was asked at: a write's own latency isn't the
 * sender's), with a block's start marked by a negative time. False if the
 * send didn't end. */
bool send_to_kernel(int raw, const std::vector<uint8_t>& msg,
                    std::vector<int64_t>* sent_at)
{
    static isotp_tx t;
    t = isotp_tx();
    const steady::time_point t0 = steady::now();
    isotp_can f;
    if (!isotp_tx_start(t, kTester, msg.data(),
                        static_cast<uint16_t>(msg.size()), us_since(t0), &f) ||
        !write_can(raw, f)) {
        return false;
    }
    bool block_start = false;
    while (t.state != isotp_tx_state::idle) {
        if (t.state == isotp_tx_state::awaiting_flow) {
            isotp_can fc;
            if (!read_can(raw, kPeer, 1000, &fc) ||
                isotp_tx_flow(t, fc, us_since(t0)) != isotp_flow_result::go) {
                return false;
            }
            block_start = true;
            continue;
        }
        const int64_t at = us_since(t0);
        const isotp_tx_step step = isotp_tx_next(t, at, &f);
        if (step == isotp_tx_step::frame) {
            if (!write_can(raw, f)) {
                return false;
            }
            sent_at->push_back(block_start ? -at - 1 : at);
            block_start = false;
        } else if (step == isotp_tx_step::later) {
            std::this_thread::sleep_for(50us);
        } else {
            return false;
        }
    }
    return true;
}

class IsoTpConformanceTest : public ::testing::Test
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

/* 32 KB: kept off the stack */
isotp_rx& fresh_rx()
{
    static isotp_rx r;
    r = isotp_rx();
    r.timed = true;
    return r;
}

} // namespace

/* Every length the kernel can send to core: a Single Frame's longest, the
 * shortest First Frame, one and two Consecutive Frames, across the
 * sequence's wrap, and ISO-TP's longest */
TEST_F(IsoTpConformanceTest, CoresReceiverTakesWhatTheKernelSends)
{
    vcan_bus::Fd k = peer();
    vcan_bus::Fd raw = tester();
    ASSERT_TRUE(k.ok() && raw.ok());
    for (size_t n : {size_t{7}, size_t{8}, size_t{13}, size_t{20}, size_t{120},
                     size_t{500}, size_t{4095}}) {
        isotp_rx& rx = fresh_rx();
        const std::vector<uint8_t> msg = counting(n);
        const steady::time_point t0 = steady::now();
        ASSERT_EQ(::write(k.get(), msg.data(), msg.size()),
                  static_cast<ssize_t>(msg.size()))
            << n;
        EXPECT_EQ(receive_from_kernel(raw.get(), rx, t0), msg) << n << " bytes";
        EXPECT_EQ(rx.dropped, 0u) << n;
        EXPECT_EQ(rx.malformed, 0u) << n;
    }
}

/* The kernel's receiver asks for blocks and a separation time; core's
 * sender keeps both, and the kernel gets the message whole */
TEST_F(IsoTpConformanceTest, CoresSenderKeepsTheKernelsBlockSizeAndStmin)
{
    struct Case {
        uint8_t bs;
        uint8_t stmin;
        int64_t gap_us; /* the least time between two frames of a block */
    };
    for (const Case c : {Case{0, 0, 0}, Case{4, 5, 5000}, Case{1, 0xF5, 0},
                         Case{8, 0xF9, 900}}) {
        vcan_bus::Fd k = peer(c.bs, c.stmin);
        vcan_bus::Fd raw = tester();
        ASSERT_TRUE(k.ok() && raw.ok());
        const std::vector<uint8_t> msg = counting(300);
        std::vector<int64_t> at;
        ASSERT_TRUE(send_to_kernel(raw.get(), msg, &at))
            << "bs " << int(c.bs) << " stmin " << int(c.stmin);
        std::vector<uint8_t> got(4096);
        const long n =
            vcan_bus::read_for(k.get(), got.data(), got.size(), 2000);
        ASSERT_EQ(n, static_cast<long>(msg.size()))
            << "bs " << int(c.bs) << " stmin " << int(c.stmin);
        got.resize(static_cast<size_t>(n));
        EXPECT_EQ(got, msg);

        size_t blocks = 0;
        for (size_t i = 0; i < at.size(); ++i) {
            if (at[i] < 0) {
                ++blocks;
                continue; /* a block's first frame goes at once */
            }
            const int64_t prev = at[i - 1] < 0 ? -at[i - 1] - 1 : at[i - 1];
            EXPECT_GE(at[i] - prev, c.gap_us) << "frame " << i;
        }
        const size_t cfs = at.size();
        EXPECT_EQ(cfs, (300 - 6 + 6) / 7u);
        EXPECT_EQ(blocks, c.bs == 0 ? 1u : (cfs + c.bs - 1) / c.bs)
            << "bs " << int(c.bs);
    }
}

/* A sender slower than N_Cr (150 ms between Consecutive Frames, ISO
 * 15765-4): core's timed receiver drops the message */
TEST_F(IsoTpConformanceTest, ASenderSlowerThanNcrIsDropped)
{
    vcan_bus::Fd k = peer(0, 0, 200u * 1000u * 1000u);
    vcan_bus::Fd raw = tester();
    ASSERT_TRUE(k.ok() && raw.ok());
    isotp_rx& rx = fresh_rx();
    const std::vector<uint8_t> msg = counting(20);
    const steady::time_point t0 = steady::now();
    ASSERT_EQ(::write(k.get(), msg.data(), msg.size()),
              static_cast<ssize_t>(msg.size()));
    EXPECT_TRUE(receive_from_kernel(raw.get(), rx, t0).empty());
    EXPECT_EQ(rx.dropped, 1u);
    EXPECT_EQ(rx.completed, 0u);
}
