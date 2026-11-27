#include "sim_memory_support.h"

#include "../core/src/sim_session.h"
#include "../core/src/sim_uri.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using sim_memory_test::Bytes;
using sim_memory_test::Node;
using sim_memory_test::nrc;
using sim_memory_test::read_of;
using sim_memory_test::Unbound;
using sim_memory_test::upload_of;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
const Bytes kProgramming = {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};
const Bytes kStart = {0x31, 0x01, 0x02, 0x01};
const Bytes kStarted = {0x71, 0x01, 0x02, 0x01};

Bytes download_of(uint32_t address, uint32_t size)
{
    return {0x34,
            0x00,
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

/* A node in programming, unlocked, wanting a helper */
Node kernel_node(const ecuio_sim_kernel_faults& f = {})
{
    Node n;
    n.s.kernel.on = true;
    n.s.kernel.faults = f;
    EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    return n;
}

template <typename Ask> void start_helper(Ask&& ask, uint32_t size = 2000)
{
    std::vector<uint8_t> img(size);
    ASSERT_EQ(ecuio_sim_kernel_image(img.data(), size), size);
    ASSERT_EQ(ask(download_of(ECUIO_SIM_KERNEL_RAM_ADDRESS, size)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    uint8_t counter = 1;
    for (uint32_t at = 0; at < size; at += 1024) {
        const uint32_t n = size - at < 1024 ? size - at : 1024;
        Bytes q = {0x36, counter};
        q.insert(q.end(), img.begin() + at, img.begin() + at + n);
        ASSERT_EQ(ask(q), (Bytes{0x76, counter}));
        ++counter;
    }
    ASSERT_EQ(ask({0x37}), (Bytes{0x77}));
    ASSERT_EQ(ask(kStart), kStarted);
}

/* An upload of the code region's first bytes, whole: 35, one 36, 37 */
void upload_code(Node& n)
{
    ASSERT_EQ(n.ask(upload_of(kCode, 16)), (Bytes{0x75, 0x20, 0x04, 0x02}));
    const Bytes block = n.ask({0x36, 0x01});
    ASSERT_EQ(block.size(), 18u);
    EXPECT_EQ(block[0], 0x76);
    ASSERT_EQ(n.ask({0x37}), (Bytes{0x77}));
}

} // namespace

TEST(SimKernelTest, ABareReadOfTheCodeWaitsForTheHelper)
{
    Node n = kernel_node();
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x7F))
        << "the loader has no read of its own";
    EXPECT_EQ(n.ask(upload_of(kCode, 16)), nrc(0x35, 0x7F));
    EXPECT_EQ(n.ask(read_of(ECUIO_SIM_MEMORY_ADDRESS, 16))[0], 0x63)
        << "region A is no helper's";
    n.s.security.unlocked = 0;
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x33))
        << "the lock is judged first";
    n.s.security.unlocked = 1;
    Bytes cut = read_of(kCode, 16);
    cut.pop_back();
    EXPECT_EQ(n.ask(cut), nrc(0x23, 0x13)) << "and the length before it";
    start_helper([&](const Bytes& q) { return n.ask(q); });
    EXPECT_EQ(n.ask(read_of(kCode, 16)).size(), 17u)
        << "through the helper, the code reads";
    upload_code(n);
    EXPECT_TRUE(n.s.kernel.running) << "it stays for the second pass";
    upload_code(n);
}

TEST(SimKernelTest, ASessionChangeOrALapseDropsTheHelper)
{
    Node n = kernel_node();
    start_helper([&](const Bytes& q) { return n.ask(q); });
    EXPECT_EQ(n.ask({0x10, 0x01})[0], 0x50);
    EXPECT_FALSE(n.s.kernel.running);
    EXPECT_TRUE(n.s.kernel.on);
    EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x7F));
    start_helper([&](const Bytes& q) { return n.ask(q); });
    n.now += ECUIO_SIM_S3_US;
    EXPECT_EQ(n.ask(read_of(kCode, 16)), nrc(0x23, 0x7F))
        << "S3 lapsed into the default session";
    EXPECT_FALSE(n.s.kernel.running);
}

TEST(SimKernelTest, AHelperThatRunsOnceEndsAtItsUploadsExit)
{
    ecuio_sim_kernel_faults f;
    f.once = true;
    Node n = kernel_node(f);
    start_helper([&](const Bytes& q) { return n.ask(q); });
    upload_code(n);
    EXPECT_FALSE(n.s.kernel.running);
    EXPECT_EQ(n.ask(upload_of(kCode, 16)), nrc(0x35, 0x7F))
        << "a second pass downloads and starts it again";
    start_helper([&](const Bytes& q) { return n.ask(q); });
    upload_code(n);
    EXPECT_EQ(n.s.kernel.downloads, 2u);
}

TEST(SimKernelTest, ASilentHelperAnswersNothingOnceStarted)
{
    ecuio_sim_kernel_faults f;
    f.silent = true;
    Node n = kernel_node(f);
    start_helper([&](const Bytes& q) { return n.ask(q); });
    EXPECT_EQ(n.ask(upload_of(kCode, 16)), Bytes{});
    EXPECT_TRUE(n.handled);
    EXPECT_EQ(n.ask({0x3E, 0x00}), Bytes{});
}

TEST(SimKernelTest, WithoutTheKnobTheCodeReadsBareAndTheKernelIsNoOnes)
{
    Node n;
    EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    EXPECT_EQ(n.ask(read_of(kCode, 16)).size(), 17u) << "L12's default";
    n.ask(download_of(ECUIO_SIM_KERNEL_RAM_ADDRESS, 100));
    EXPECT_FALSE(n.s.kernel.downloading) << "no 34 is the kernel's";
}

TEST(SimKernelTest, AFunctionalQuieterAndKeepAliveAreTakenUnanswered)
{
    Node n;
    const Bytes quiet = {0x10, 0x82};
    const Bytes keep = {0x3E, 0x80};
    EXPECT_FALSE(ecuio_sim_session_functional(n.s, false, 0, quiet.data(), 2))
        << "a node with no memory to program";
    for (const Bytes& other : {Bytes{0x10, 0x02}, Bytes{0x3E, 0x00},
                               Bytes{0x11, 0x81}, Bytes{0x10, 0x81}}) {
        EXPECT_FALSE(
            ecuio_sim_session_functional(n.s, true, 0, other.data(), 2));
    }
    EXPECT_FALSE(n.s.programming);
    n.s.security.unlocked = 1;
    ASSERT_TRUE(ecuio_sim_session_functional(n.s, true, 0, quiet.data(), 2));
    EXPECT_TRUE(n.s.programming);
    EXPECT_EQ(n.s.security.unlocked, 0u) << "a session entered locks";
    n.unlock();
    ASSERT_TRUE(ecuio_sim_session_functional(n.s, true, 0, quiet.data(), 2));
    EXPECT_NE(n.s.security.unlocked, 0u)
        << "a second quieter leaves the session it is in";
    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(ecuio_sim_session_functional(
            n.s, true, i * (ECUIO_SIM_S3_US / 2), keep.data(), 2));
    }
    EXPECT_TRUE(n.s.programming) << "3E 80 holds S3 off";
    n.now = 2 * ECUIO_SIM_S3_US + ECUIO_SIM_S3_US - 1;
    EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    EXPECT_TRUE(n.s.programming);
}

TEST(SimKernelTest, TheUrisKernelKeyIsTheEngineEcusAndItsWords)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://x?kernel=1", &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.kernel);
    EXPECT_FALSE(c.kernel_faults.once);
    ASSERT_EQ(
        ecuio_sim_parse_uri("mock://x?kernel=once,silent", &c, why, sizeof why),
        ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.kernel);
    EXPECT_TRUE(c.kernel_faults.once);
    EXPECT_TRUE(c.kernel_faults.silent);
    EXPECT_FALSE(c.kernel_faults.storm);
    for (const char* bad : {"mock://x?kernel=0", "mock://x?kernel=",
                            "mock://x?kernel=once,", "mock://x?kernel=loud"}) {
        EXPECT_EQ(ecuio_sim_parse_uri(bad, &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
        EXPECT_EQ(std::string(why), "kernel takes 1, or once, storm, silent "
                                    "and refuse, comma-separated")
            << bad;
    }

    Unbound u("mock://x?kernel=1");
    EXPECT_EQ(u.ask(0x7DF, {0x10, 0x82}), Bytes{}) << "a quieter is silent";
    u.ask(0x7E0, {0x27, 0x01});
    EXPECT_EQ(u.ask(0x7E0, sim_memory_test::key_for(0x01)), (Bytes{0x67, 0x02}))
        << "the quieter entered programming straight from default";
    EXPECT_EQ(u.ask(0x7E0, read_of(kCode, 16)), nrc(0x23, 0x7F));
    start_helper([&](const Bytes& q) { return u.ask(0x7E0, q); });
    EXPECT_EQ(u.ask(0x7E0, read_of(kCode, 16)).size(), 17u);
    u.t += ECUIO_SIM_S3_US - 200000;
    EXPECT_EQ(u.ask(0x7DF, {0x3E, 0x80}), Bytes{});
    u.t += ECUIO_SIM_S3_US - 200000;
    EXPECT_EQ(u.ask(0x7E0, read_of(kCode, 16)).size(), 17u)
        << "the functional keep-alive held the session";
}
