// File/layer: tests/test_access_cut.cpp | test_core, Qt-free, C++17
// Testing: UNIT: AccessCutTest (test_core)
//
// The rule every capture cuts a security access and a memory read's
// answer by (access_cut.h): what it cuts and the words its line carries.

#include "../core/src/access_cut.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

/* A seed's answer and a key's request are cut past their sub-function;
 * a refusal, a bare seed request and any other service are not */
TEST(AccessCutTest, ItCutsAServiceOf27Or67PastItsSubFunction)
{
    const uint8_t key[] = {0x27, 0x02, 0x11, 0x22, 0x33, 0x44};
    const uint8_t seed[] = {0x67, 0x01, 0xAA};
    const uint8_t ask[] = {0x27, 0x01};
    const uint8_t refused[] = {0x7F, 0x27, 0x35};
    const uint8_t did[] = {0x62, 0xF1, 0x90, 0x41};
    EXPECT_TRUE(ecuio_access_cut(key, sizeof key));
    EXPECT_TRUE(ecuio_access_cut(seed, sizeof seed));
    EXPECT_FALSE(ecuio_access_cut(ask, sizeof ask));
    EXPECT_FALSE(ecuio_access_cut(refused, sizeof refused));
    EXPECT_FALSE(ecuio_access_cut(did, sizeof did));
}

/* A memory read's answer keeps its service, a transfer's its service and
 * block counter, a security access's its two; nothing else is cut, and
 * nothing with no byte after what it keeps */
TEST(AccessCutTest, ACaptureKeepsEachCutAnswersFirstBytes)
{
    const auto kept = [](std::initializer_list<uint8_t> pdu) {
        return ecuio_capture_kept(pdu.begin(), pdu.size());
    };
    EXPECT_EQ(kept({0x63, 0xAA, 0xBB}), 1u);
    EXPECT_EQ(kept({0x63, 0xAA}), 1u);
    EXPECT_EQ(kept({0x63}), 0u);
    EXPECT_EQ(kept({0x76, 0x01, 0xAA}), 2u);
    EXPECT_EQ(kept({0x76, 0x01}), 0u) << "a counter alone";
    EXPECT_EQ(kept({0x67, 0x01, 0xAA}), 2u);
    EXPECT_EQ(kept({0x27, 0x02, 0xAA}), 2u);
    EXPECT_EQ(kept({0x27, 0x01}), 0u);
    EXPECT_EQ(ecuio_capture_kept(nullptr, 0), 0u);
    /* requests, refusals and every other answer are whole */
    EXPECT_EQ(kept({0x23, 0x24, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00}), 0u);
    EXPECT_EQ(kept({0x36, 0x01, 0xAA}), 0u);
    EXPECT_EQ(kept({0x75, 0x20, 0x01, 0x02}), 0u);
    EXPECT_EQ(kept({0x77, 0xAA}), 0u);
    EXPECT_EQ(kept({0x7F, 0x23, 0x31}), 0u);
    EXPECT_EQ(kept({0x62, 0xF1, 0x90, 0x41}), 0u);
    unsigned cut = 0;
    for (unsigned sid = 0; sid < 256; ++sid) {
        cut +=
            kept({static_cast<uint8_t>(sid), 0x01, 0x02, 0x03}) != 0 ? 1u : 0u;
        EXPECT_EQ(ecuio_memory_request(static_cast<uint8_t>(sid)),
                  sid == 0x23 || sid == 0x36)
            << sid;
    }
    EXPECT_EQ(cut, 4u);
    /* the line counts and hashes what follows the bytes kept */
    const uint8_t read[] = {0x63, 0x11, 0x22, 0x33, 0x44};
    char text[ECUIO_ACCESS_CUT_TEXT];
    ecuio_capture_cut_text(read, sizeof read, 1, text, sizeof text);
    EXPECT_EQ(
        std::string(text),
        "4 bytes cut, SHA-256 "
        "1a835ed8734f86355ca5b835d824d486993aabf1913cd3a011b7446c0514b7c9");
}

/* The line names the bytes cut by their count and SHA-256 */
TEST(AccessCutTest, TheLineIsTheLengthAndSha256OfWhatWasCut)
{
    const uint8_t key[] = {0x27, 0x02, 0x11, 0x22, 0x33, 0x44};
    char text[ECUIO_ACCESS_CUT_TEXT];
    ecuio_access_cut_text(key, sizeof key, text, sizeof text);
    EXPECT_EQ(std::string(text),
              "4 bytes cut, SHA-256 "
              "1a835ed8734f86355ca5b835d824d486993aabf1913cd3a011b7446c0514b7c9");
    const uint8_t bare[] = {0x67, 0x01};
    ecuio_access_cut_text(bare, sizeof bare, text, sizeof text);
    EXPECT_EQ(std::string(text),
              "0 bytes cut, SHA-256 "
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
