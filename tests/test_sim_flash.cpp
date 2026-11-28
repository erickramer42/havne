#include "sim_memory_support.h"

#include "../core/src/sim_session.h"
#include "../core/src/sim_uri.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using sim_memory_test::Bytes;
using sim_memory_test::Node;
using sim_memory_test::upload_of;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
constexpr uint32_t kSector = ECUIO_SIM_FLASH_SECTOR_BYTES;
const Bytes kProgramming = {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};
const Bytes kCheck = {0x31, 0x01, 0xFF, 0x01};

Bytes span_of(uint8_t head0, uint8_t head1, uint32_t address, uint32_t size)
{
    return {head0,
            head1,
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

Bytes erase_of(uint32_t address, uint32_t size)
{
    Bytes q = span_of(0x31, 0x01, address, size);
    q.insert(q.begin() + 2, {0xFF, 0x00});
    return q;
}

Bytes download_of(uint32_t address, uint32_t size)
{
    return span_of(0x34, 0x00, address, size);
}

std::vector<uint8_t> image_of(uint32_t size, uint8_t salt = 0)
{
    std::vector<uint8_t> b(size);
    for (uint32_t i = 0; i < size; ++i) {
        b[i] = static_cast<uint8_t>(i * 29 + 11 + salt);
    }
    return b;
}

Node flash_node(const ecuio_sim_flash_faults& f = {})
{
    Node n;
    EXPECT_TRUE(ecuio_sim_flash_arm(n.s.flash, n.s.memory, f));
    EXPECT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    return n;
}

Bytes block_of(uint8_t counter, const std::vector<uint8_t>& img, uint32_t at,
               uint32_t n)
{
    Bytes q = {0x36, counter};
    q.insert(q.end(), img.begin() + at, img.begin() + at + n);
    return q;
}

void write_sector(Node& n, uint32_t address, const std::vector<uint8_t>& img)
{
    ASSERT_EQ(n.ask(erase_of(address, kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    ASSERT_EQ(n.ask(download_of(address, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    uint8_t counter = 1;
    for (uint32_t at = 0; at < kSector; at += 1024, ++counter) {
        ASSERT_EQ(n.ask(block_of(counter, img, at, 1024)),
                  (Bytes{0x76, counter}));
    }
    ASSERT_EQ(n.ask({0x37}), (Bytes{0x77}));
}

Bytes read_back(Node& n, uint32_t address, uint32_t size)
{
    EXPECT_EQ(n.ask(upload_of(address, size)), (Bytes{0x75, 0x20, 0x04, 0x02}));
    Bytes got;
    uint8_t counter = 1;
    while (got.size() < size) {
        const Bytes b = n.ask({0x36, counter++});
        if (b.size() < 2 || b[0] != 0x76) {
            ADD_FAILURE() << "the upload stopped";
            return got;
        }
        got.insert(got.end(), b.begin() + 2, b.end());
    }
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x77}));
    return got;
}

} // namespace

TEST(SimFlashTest, AnErasedSectorTakesTheImageAndTheCheckPasses)
{
    Node n = flash_node();
    const std::vector<uint8_t> img = image_of(kSector);
    write_sector(n, kCode + kSector, img);
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x00}));
    EXPECT_EQ(read_back(n, kCode + kSector, kSector),
              Bytes(img.begin(), img.end()));
    const Bytes before = read_back(n, kCode, 16);
    for (uint32_t i = 0; i < 16; ++i) {
        EXPECT_EQ(before[i], ecuio_sim_memory_fill(kCode + i))
            << "a sector not erased keeps its bytes";
    }
    EXPECT_EQ(n.s.flash.erases, 1u);
    EXPECT_EQ(n.s.flash.downloads, 1u);
}

TEST(SimFlashTest, AnErasedSectorLeftUnwrittenFailsTheCheck)
{
    Node n = flash_node();
    ASSERT_EQ(n.ask(erase_of(kCode, 2 * kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    EXPECT_EQ(read_back(n, kCode, 4), (Bytes{0xFF, 0xFF, 0xFF, 0xFF}));
    write_sector(n, kCode, image_of(kSector));
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x01}));
    write_sector(n, kCode + kSector, image_of(kSector, 3));
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x00}));
}

TEST(SimFlashTest, TheRefusalsComeInTheirOrder)
{
    Node off;
    ASSERT_TRUE(ecuio_sim_flash_arm(off.s.flash, off.s.memory, {}));
    EXPECT_EQ(off.ask(erase_of(kCode, kSector)), (Bytes{0x7F, 0x31, 0x7F}))
        << "outside the programming session";
    ASSERT_EQ(off.ask({0x10, 0x02}), kProgramming);
    EXPECT_EQ(off.ask(erase_of(kCode, kSector)), (Bytes{0x7F, 0x31, 0x33}))
        << "locked";

    Node n = flash_node();
    EXPECT_EQ(n.ask(erase_of(kCode + 16, kSector)), (Bytes{0x7F, 0x31, 0x31}))
        << "not on a sector's start";
    EXPECT_EQ(n.ask(erase_of(kCode, kSector + 1)), (Bytes{0x7F, 0x31, 0x31}));
    EXPECT_EQ(n.ask(download_of(kCode, kSector)), (Bytes{0x7F, 0x34, 0x70}))
        << "a sector not erased takes no download";
    ASSERT_EQ(n.ask(erase_of(kCode, kSector)), (Bytes{0x71, 0x01, 0xFF, 0x00}));
    Bytes compressed = download_of(kCode, kSector);
    compressed[1] = 0x11;
    EXPECT_EQ(n.ask(compressed), (Bytes{0x7F, 0x34, 0x31}));
    ASSERT_EQ(n.ask(download_of(kCode, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    EXPECT_EQ(n.ask(download_of(kCode, kSector)), (Bytes{0x7F, 0x34, 0x22}))
        << "one download at a time";
    EXPECT_EQ(n.ask(erase_of(kCode, kSector)), (Bytes{0x7F, 0x31, 0x22}))
        << "no erase under a download";
    const std::vector<uint8_t> img = image_of(kSector);
    EXPECT_EQ(n.ask(block_of(2, img, 0, 1024)), (Bytes{0x7F, 0x36, 0x73}));
    EXPECT_EQ(n.ask({0x37}), (Bytes{0x7F, 0x37, 0x24}))
        << "an exit before the last block";
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x01}))
        << "a check under a download fails";
}

TEST(SimFlashTest, EachFaultStrikesWhereItIsNamed)
{
    const std::vector<uint8_t> img = image_of(kSector);
    ecuio_sim_flash_faults f;
    f.counter = true;
    Node counter = flash_node(f);
    ASSERT_EQ(counter.ask(erase_of(kCode, kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    ASSERT_EQ(counter.ask(download_of(kCode, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    EXPECT_EQ(counter.ask(block_of(1, img, 0, 1024)),
              (Bytes{0x7F, 0x36, 0x73}));
    EXPECT_EQ(counter.ask(block_of(1, img, 0, 1024)), (Bytes{0x76, 0x01}))
        << "once: the block sent again is taken";

    f = {};
    f.volts = true;
    Node volts = flash_node(f);
    ASSERT_EQ(volts.ask(erase_of(kCode, kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    ASSERT_EQ(volts.ask(download_of(kCode, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    EXPECT_EQ(volts.ask(block_of(1, img, 0, 1024)), (Bytes{0x7F, 0x36, 0x93}));
    EXPECT_EQ(volts.ask(block_of(1, img, 0, 1024)), (Bytes{0x76, 0x01}));

    f = {};
    f.general = true;
    Node general = flash_node(f);
    ASSERT_EQ(general.ask(erase_of(kCode, kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    ASSERT_EQ(general.ask(download_of(kCode, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    EXPECT_EQ(general.ask(block_of(1, img, 0, 1024)), (Bytes{0x76, 0x01}));
    EXPECT_EQ(general.ask(block_of(2, img, 1024, 1024)),
              (Bytes{0x7F, 0x36, 0x72}));

    f = {};
    f.silent = true;
    Node silent = flash_node(f);
    ASSERT_EQ(silent.ask(erase_of(kCode, kSector)),
              (Bytes{0x71, 0x01, 0xFF, 0x00}));
    ASSERT_EQ(silent.ask(download_of(kCode, kSector)),
              (Bytes{0x74, 0x20, 0x04, 0x02}));
    EXPECT_EQ(silent.ask(block_of(1, img, 0, 1024)), (Bytes{0x76, 0x01}));
    EXPECT_EQ(silent.ask(block_of(2, img, 1024, 1024)), (Bytes{0x76, 0x02}));
    EXPECT_EQ(silent.ask(block_of(3, img, 2048, 1024)), Bytes{});
    EXPECT_EQ(silent.ask(kCheck), Bytes{}) << "the power is gone";

    f = {};
    f.erase = true;
    Node erase = flash_node(f);
    EXPECT_EQ(erase.ask(erase_of(kCode, kSector)), (Bytes{0x7F, 0x31, 0x22}));

    f = {};
    f.check = true;
    Node check = flash_node(f);
    write_sector(check, kCode, img);
    EXPECT_EQ(check.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x01}));
}

TEST(SimFlashTest, AResetKeepsWhatWasWritten)
{
    Node n = flash_node();
    const std::vector<uint8_t> img = image_of(kSector, 7);
    write_sector(n, kCode, img);
    EXPECT_EQ(n.ask({0x11, 0x01}), (Bytes{0x51, 0x01}));
    ASSERT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    EXPECT_EQ(read_back(n, kCode, kSector), Bytes(img.begin(), img.end()));
}

TEST(SimFlashTest, TheUrisFlashKeyTakesItsWords)
{
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?flash=counter,silent", &c, why,
                                  sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.flash);
    EXPECT_TRUE(c.flash_faults.counter);
    EXPECT_TRUE(c.flash_faults.silent);
    EXPECT_FALSE(c.flash_faults.general);
    EXPECT_EQ(ecuio_sim_parse_uri("mock://sim?flash=burn", &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
    EXPECT_STREQ(why, "flash takes 1, or counter, general, volts, check, "
                      "erase, silent, boot[:<sectors>], locked and "
                      "pending:<ms>, comma-separated");
    ASSERT_EQ(ecuio_sim_parse_uri("mock://sim?flash=boot:3,locked,pending:250",
                                  &c, why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_TRUE(c.flash_faults.boot);
    EXPECT_EQ(c.flash_faults.boot_written, 3u);
    EXPECT_TRUE(c.flash_faults.locked);
    EXPECT_EQ(c.flash_faults.pending_ms, 250u);
    for (const char* bad :
         {"mock://sim?flash=boot:0", "mock://sim?flash=boot:65",
          "mock://sim?flash=pending", "mock://sim?flash=pending:600001",
          "mock://sim?flash=boot:x"}) {
        EXPECT_EQ(ecuio_sim_parse_uri(bad, &c, why, sizeof why),
                  ecuio_sim_uri_result::bad)
            << bad;
    }
    EXPECT_EQ(ecuio_sim_parse_uri("replay://C:/x?flash=1", &c, why, sizeof why),
              ecuio_sim_uri_result::bad)
        << "a replay ECU has no memory to write";
}

TEST(SimFlashTest, APartWrittenModuleAnswersAsItsBootloaderUntilChecked)
{
    ecuio_sim_flash_faults f;
    f.boot = true;
    f.boot_written = 1;
    Node n;
    ASSERT_TRUE(ecuio_sim_flash_arm(n.s.flash, n.s.memory, f));
    EXPECT_EQ(n.ask({0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}));
    EXPECT_EQ(n.ask({0x22, 0xF1, 0x90}), (Bytes{0x7F, 0x22, 0x11}));
    EXPECT_EQ(n.ask({0x01, 0x0C}, true), Bytes{}) << "no functional answer";
    EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7E, 0x00}));
    ASSERT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    EXPECT_EQ(read_back(n, kCode, 4)[0], ecuio_sim_memory_fill(kCode))
        << "the first sector is written";
    EXPECT_EQ(read_back(n, kCode + kSector, 4),
              (Bytes{0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x01}));
    EXPECT_EQ(n.ask({0x11, 0x01}), (Bytes{0x51, 0x01}));
    EXPECT_EQ(n.ask({0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}))
        << "a failed check keeps the bootloader";

    ASSERT_EQ(n.ask({0x10, 0x02}), kProgramming);
    n.unlock();
    for (uint32_t i = 1; i < ECUIO_SIM_FLASH_SECTORS; ++i) {
        write_sector(n, kCode + i * kSector, image_of(kSector));
    }
    EXPECT_EQ(n.ask(kCheck), (Bytes{0x71, 0x01, 0xFF, 0x01, 0x00}));
    EXPECT_EQ(n.ask({0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}))
        << "still in the programming session";
    EXPECT_EQ(n.ask({0x11, 0x01}), (Bytes{0x51, 0x01}));
    const Bytes app = n.ask({0x01, 0x00});
    ASSERT_FALSE(app.empty());
    EXPECT_EQ(app[0], 0x41) << "the application runs again";
}

TEST(SimFlashTest, AnEraseLeavesTheBootloaderUntilACheckPasses)
{
    Node n = flash_node();
    ASSERT_EQ(n.ask(erase_of(kCode, kSector)), (Bytes{0x71, 0x01, 0xFF, 0x00}));
    EXPECT_EQ(n.ask({0x10, 0x01}).at(0), 0x50);
    EXPECT_EQ(n.ask({0x01, 0x00}), (Bytes{0x7F, 0x01, 0x11}));
}

TEST(SimFlashTest, ALockedBootloaderRefusesEverySeed)
{
    ecuio_sim_flash_faults f;
    f.locked = true;
    Node n;
    ASSERT_TRUE(ecuio_sim_flash_arm(n.s.flash, n.s.memory, f));
    ASSERT_EQ(n.ask({0x10, 0x02}), kProgramming);
    EXPECT_EQ(n.ask({0x27, 0x01}), (Bytes{0x7F, 0x27, 0x36}));
    EXPECT_EQ(n.ask({0x27, 0x01}), (Bytes{0x7F, 0x27, 0x36}));
    EXPECT_EQ(n.s.security.unlocked, 0u);
}

TEST(SimFlashTest, APendingModuleAnswersOnlyStillWorkingForItsTime)
{
    ecuio_sim_flash_faults f;
    f.pending_ms = 300;
    Node n;
    ASSERT_TRUE(ecuio_sim_flash_arm(n.s.flash, n.s.memory, f));
    n.now = 1000000;
    EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7F, 0x3E, 0x78}));
    n.now += 299999;
    EXPECT_EQ(n.ask({0x10, 0x02}), (Bytes{0x7F, 0x10, 0x78}));
    n.now += 1;
    EXPECT_EQ(n.ask({0x3E, 0x00}), (Bytes{0x7E, 0x00}));
}
