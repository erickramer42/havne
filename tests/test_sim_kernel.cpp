#include "sim_kernel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct Node {
    ecuio_sim_kernel k;
    ecuio_sim_security security;
    bool programming = true;

    Node()
    {
        k.on = true;
        security.unlocked = 0x01;
    }

    Bytes ask(const Bytes& q)
    {
        uint8_t out[ECUIO_SIM_MEMORY_REPLY_MAX] = {};
        const uint32_t n =
            ecuio_sim_kernel_answer(k, security, programming, q.data(),
                                    static_cast<uint32_t>(q.size()), out);
        return Bytes(out, out + n);
    }
};

Bytes download(uint32_t address, uint32_t size, uint8_t dfi = 0x00)
{
    return {0x34,
            dfi,
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

Bytes image(uint32_t size)
{
    Bytes b(size);
    EXPECT_EQ(ecuio_sim_kernel_image(b.data(), size), size);
    return b;
}

const Bytes kStart = {0x31, 0x01, 0x02, 0x01};
const Bytes kStarted = {0x71, 0x01, 0x02, 0x01};

Bytes refused(uint8_t sid, uint8_t nrc)
{
    return {0x7F, sid, nrc};
}

/* The image sent as the module names its blocks: 1024 data bytes each,
 * a short last one */
uint32_t send(Node& n, const Bytes& img, uint32_t data_per_block = 1024)
{
    uint32_t blocks = 0;
    uint8_t counter = 1;
    for (size_t at = 0; at < img.size(); at += data_per_block) {
        const size_t len = std::min<size_t>(data_per_block, img.size() - at);
        Bytes q = {0x36, counter};
        q.insert(q.end(), img.begin() + static_cast<long>(at),
                 img.begin() + static_cast<long>(at + len));
        EXPECT_EQ(n.ask(q), (Bytes{0x76, counter})) << blocks;
        ++counter;
        ++blocks;
    }
    return blocks;
}

} // namespace

TEST(SimKernelTest, AHelperDownloadedAndStartedRuns)
{
    Node n;
    const uint32_t size = 5 * 1024 + 256;
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, size)),
              (Bytes{0x74, 0x20, 0x04, 0x02}))
        << "1024 data bytes a block, as the module names them";
    EXPECT_EQ(send(n, image(size)), 6u);
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x77}));
    EXPECT_FALSE(n.k.running);
    EXPECT_EQ(n.ask(kStart), kStarted);
    EXPECT_TRUE(n.k.running);
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x24)) << "it runs already";
    EXPECT_EQ(n.k.downloads, 1u);
    EXPECT_EQ(n.k.starts, 1u);
}

TEST(SimKernelTest, ADownloadsRefusalsComeInTheirOrder)
{
    Node n;
    n.programming = false;
    EXPECT_EQ(n.ask({0x34}), refused(0x34, 0x7F)) << "the session first";
    n.programming = true;
    EXPECT_EQ(n.ask({0x34, 0x00, 0x44}), refused(0x34, 0x13));
    Bytes q = download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 64);
    q.pop_back();
    EXPECT_EQ(n.ask(q), refused(0x34, 0x13)) << "not the identifier's";
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 64, 0x11)),
              refused(0x34, 0x31))
        << "a data format but 00";
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS - 1, 64)),
              refused(0x34, 0x31));
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS + 1,
                             ECUIO_SIM_KERNEL_RAM_BYTES)),
              refused(0x34, 0x31))
        << "its last byte past the window";
    EXPECT_EQ(n.ask(download(ECUIO_SIM_MEMORY_ADDRESS, 64)),
              refused(0x34, 0x31))
        << "the calibration region is no RAM";
    EXPECT_EQ(
        n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, ECUIO_SIM_KERNEL_HEAD)),
        refused(0x34, 0x31))
        << "no room past the head";
    n.security.unlocked = 0;
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 64)),
              refused(0x34, 0x33));
    n.security.unlocked = 1;
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS,
                             ECUIO_SIM_KERNEL_RAM_BYTES))[0],
              0x74)
        << "the whole window";
    EXPECT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 64)),
              refused(0x34, 0x70))
        << "one open";
}

TEST(SimKernelTest, ABlocksRefusalsComeInTheirOrder)
{
    Node n;
    ASSERT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 2000))[0], 0x74);
    EXPECT_EQ(n.ask({0x36, 0x01}), refused(0x36, 0x13)) << "no data";
    Bytes over(ECUIO_SIM_KERNEL_BLOCK + 1, 0x00);
    over[0] = 0x36;
    over[1] = 0x01;
    EXPECT_EQ(n.ask(over), refused(0x36, 0x13)) << "past the block length";
    Bytes full(ECUIO_SIM_KERNEL_BLOCK, 0x00);
    full[0] = 0x36;
    full[1] = 0x02;
    n.security.unlocked = 0;
    EXPECT_EQ(n.ask(full), refused(0x36, 0x33));
    n.security.unlocked = 1;
    EXPECT_EQ(n.ask(full), refused(0x36, 0x73)) << "the first counter is 01";
    full[1] = 0x01;
    EXPECT_EQ(n.ask(full), (Bytes{0x76, 0x01}));
    EXPECT_EQ(n.ask({0x37}), refused(0x37, 0x24)) << "bytes left";
    Bytes rest(2 + 2000 - 1024 + 1, 0x00);
    rest[0] = 0x36;
    rest[1] = 0x02;
    EXPECT_EQ(n.ask(rest), refused(0x36, 0x13)) << "past the bytes left";
    rest.pop_back();
    EXPECT_EQ(n.ask(rest), (Bytes{0x76, 0x02}));
    EXPECT_EQ(n.ask({0x37, 0x00}), refused(0x37, 0x13));
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x77}));
}

TEST(SimKernelTest, TheCounterWrapsFFTo00)
{
    Node n;
    const uint32_t size = 300 * 16 + 5;
    ASSERT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, size))[0], 0x74);
    EXPECT_EQ(send(n, image(size), 16), 301u);
    EXPECT_EQ(n.k.counter, static_cast<uint8_t>(302 % 256));
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x77}));
    EXPECT_EQ(n.ask(kStart), kStarted) << "short blocks are taken too";
}

TEST(SimKernelTest, AStartsRefusalsComeInTheirOrder)
{
    Node n;
    n.programming = false;
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x7F));
    n.programming = true;
    EXPECT_EQ(n.ask({0x31, 0x01, 0x02}), refused(0x31, 0x13));
    EXPECT_EQ(n.ask({0x31, 0x03, 0x02, 0x01}), refused(0x31, 0x12));
    EXPECT_EQ(n.ask({0x31, 0x01, 0xFF, 0x00}), refused(0x31, 0x31));
    EXPECT_EQ(n.ask({0x31, 0x01, 0x02, 0x01, 0x00}), refused(0x31, 0x13));
    n.security.unlocked = 0;
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x33));
    n.security.unlocked = 1;
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x24)) << "nothing downloaded";
    ASSERT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 100))[0], 0x74);
    Bytes img = image(100);
    img[50] ^= 0x01;
    send(n, img);
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x24)) << "no 37 yet";
    ASSERT_EQ(n.ask({0x37}), (Bytes{0x77}));
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x22)) << "a byte changed";
    EXPECT_FALSE(n.k.running);
}

TEST(SimKernelTest, ASessionChangedDropsTheHelperAndAOnceHelperEndsAtAnUpload)
{
    Node n;
    const auto run = [&]() {
        ASSERT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 100))[0], 0x74);
        send(n, image(100));
        ASSERT_EQ(n.ask({0x37}), (Bytes{0x77}));
        ASSERT_EQ(n.ask(kStart), kStarted);
    };
    run();
    ecuio_sim_kernel_upload_done(n.k);
    EXPECT_TRUE(n.k.running) << "it stays for a second read";
    ecuio_sim_kernel_end(n.k);
    EXPECT_FALSE(n.k.running);
    EXPECT_TRUE(n.k.on);
    EXPECT_EQ(n.k.downloads, 1u) << "the counts stay";
    n.k.faults.once = true;
    run();
    ecuio_sim_kernel_upload_done(n.k);
    EXPECT_FALSE(n.k.running);
    EXPECT_TRUE(n.k.faults.once);
}

TEST(SimKernelTest, TheFaultsBreakTheStartOrSilenceTheHelper)
{
    Node n;
    const auto loaded = [&]() {
        ecuio_sim_kernel_end(n.k);
        ASSERT_EQ(n.ask(download(ECUIO_SIM_KERNEL_RAM_ADDRESS, 100))[0], 0x74);
        send(n, image(100));
        ASSERT_EQ(n.ask({0x37}), (Bytes{0x77}));
    };
    n.k.faults.storm = true;
    loaded();
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x78));
    EXPECT_FALSE(n.k.running);
    n.k.faults.storm = false;
    n.k.faults.refuse = true;
    loaded();
    EXPECT_EQ(n.ask(kStart), refused(0x31, 0x22));
    n.k.faults.refuse = false;
    n.k.faults.silent = true;
    loaded();
    EXPECT_EQ(n.ask(kStart), kStarted);
    EXPECT_EQ(n.ask(kStart), Bytes{}) << "nothing once it runs";
    EXPECT_EQ(n.ask({0x34}), Bytes{});
}

TEST(SimKernelTest, ItAnswersOnlyItsOwnServices)
{
    ecuio_sim_kernel k;
    const uint8_t q34[1] = {0x34};
    const uint8_t q31[1] = {0x31};
    const uint8_t q36[1] = {0x36};
    const uint8_t q37[1] = {0x37};
    EXPECT_FALSE(ecuio_sim_kernel_service(k, q34)) << "off";
    k.on = true;
    EXPECT_TRUE(ecuio_sim_kernel_service(k, q34));
    EXPECT_TRUE(ecuio_sim_kernel_service(k, q31));
    EXPECT_FALSE(ecuio_sim_kernel_service(k, q36)) << "an upload's";
    EXPECT_FALSE(ecuio_sim_kernel_service(k, q37));
    k.downloading = true;
    EXPECT_TRUE(ecuio_sim_kernel_service(k, q36));
    EXPECT_TRUE(ecuio_sim_kernel_service(k, q37));
    for (unsigned sid = 0; sid <= 0xFF; ++sid) {
        const uint8_t q[1] = {static_cast<uint8_t>(sid)};
        EXPECT_EQ(ecuio_sim_kernel_service(k, q),
                  sid == 0x34 || sid == 0x31 || sid == 0x36 || sid == 0x37)
            << std::hex << sid;
    }
}
