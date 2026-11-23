// File/layer: tests/test_access_cut.cpp | test_core, Qt-free, C++17
// Testing: UNIT: AccessCutTest (test_core)
//
// The rule every capture cuts a security access by (access_cut.h): what
// it cuts and the words its line carries.

#include "../core/src/access_cut.h"

#include <gtest/gtest.h>

#include <cstdint>
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
