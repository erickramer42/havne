// File/layer: tests/test_base64.cpp | C++17, Qt-free, every platform
// Testing: UNIT: Base64Test (test_signals)
//
// core/src/base64.h by itself: RFC 4648's vectors, every length and byte
// back as it was, and every text but the one of some bytes refused.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "base64.h" /* core/src */

#include "plugin_conversation_support.h"

using conversation_test::base64;
using wasm_builder::bytes;

TEST(Base64Test, TheRfcsVectorsDecode)
{
    const struct {
        const char* text;
        const char* bytes;
    } rows[] = {{"", ""},
                {"Zg==", "f"},
                {"Zm8=", "fo"},
                {"Zm9v", "foo"},
                {"Zm9vYg==", "foob"},
                {"Zm9vYmE=", "fooba"},
                {"Zm9vYmFy", "foobar"}};
    for (const auto& r : rows) {
        std::vector<uint8_t> out = {0xA5};
        ASSERT_TRUE(ecuio_base64_decode(r.text, std::strlen(r.text), &out))
            << r.text;
        EXPECT_EQ(std::string(out.begin(), out.end()), r.bytes) << r.text;
        size_t n = 99;
        ASSERT_TRUE(ecuio_base64_size(r.text, std::strlen(r.text), &n));
        EXPECT_EQ(n, std::strlen(r.bytes)) << r.text;
    }
}

TEST(Base64Test, EveryLengthAndByteComesBackAsItWas)
{
    for (size_t n = 0; n <= 259; ++n) {
        bytes in(n);
        for (size_t i = 0; i < n; ++i) {
            in[i] = static_cast<uint8_t>(i * 7 + n);
        }
        const std::string text = base64(in);
        std::vector<uint8_t> out;
        ASSERT_TRUE(ecuio_base64_decode(text.data(), text.size(), &out)) << n;
        EXPECT_EQ(out, in) << n;
    }
    /* every digit of the alphabet, at each place of a group */
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t d = 0; d < alphabet.size(); ++d) {
        EXPECT_EQ(ecuio_base64_value(alphabet[d]), static_cast<int>(d));
    }
    int taken = 0;
    for (int c = 0; c < 256; ++c) {
        taken += ecuio_base64_value(static_cast<char>(c)) >= 0 ? 1 : 0;
    }
    EXPECT_EQ(taken, 64) << "and no other character";
}

TEST(Base64Test, OnlyTheOneTextOfSomeBytesDecodes)
{
    const char* const refused[] = {
        "Zg",               /* no padding */
        "Zg=",              /* not a multiple of 4 */
        "Zm9vY",            /* a group cut short */
        "Z===",             /* three pads */
        "====",             /* nothing but padding */
        "=g==",             /* padding first */
        "Zg=v",             /* padding inside a group */
        "Zg==Zg==",         /* padding before the end */
        "Zm8=Zm8=",         /* and one pad there */
        "Zm9v\n",           /* a line break */
        "Zm 9",             /* a space */
        "Zm9-",             /* the URL alphabet */
        "Zm9_",     "Zh==", /* bits no byte takes, after one byte */
        "Zm9=",             /* and after two */
        "Zm9v\x80",         /* a byte past ASCII */
    };
    for (const char* text : refused) {
        std::vector<uint8_t> out;
        EXPECT_FALSE(ecuio_base64_decode(text, std::strlen(text), &out))
            << '"' << text << '"';
    }
    const char nul[] = {'Z', 'm', '\0', 'v'};
    std::vector<uint8_t> out;
    EXPECT_FALSE(ecuio_base64_decode(nul, sizeof nul, &out)) << "a NUL";
    size_t n = 0;
    EXPECT_FALSE(ecuio_base64_size("Zm9", 3, &n));
    EXPECT_TRUE(ecuio_base64_size("Zm9v", 4, &n));
    EXPECT_EQ(n, 3u);
}
