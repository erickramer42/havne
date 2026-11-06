// File/layer: tests/test_sha256.cpp | Qt-free, GoogleTest
// Testing: UNIT: Sha256Test (test_core)
//
// core/src/sha256.h against FIPS 180-4's examples (FIPS 180-2's
// appendix B, which NIST's example files repeat) and a message fed in
// pieces across each block boundary.

#include "sha256.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string pieces(const std::string& m, size_t step)
{
    ecuio_sha256::hasher h;
    for (size_t i = 0; i < m.size(); i += step) {
        h.add(m.data() + i, (std::min)(step, m.size() - i));
    }
    uint8_t d[32];
    h.finish(d);
    return ecuio_sha256::hex(d);
}

} // namespace

TEST(Sha256Test, TheStandardsExamplesHashAsItSays)
{
    EXPECT_EQ(
        ecuio_sha256::hex_of("", 0),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        ecuio_sha256::hex_of("abc", 3),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(
        ecuio_sha256::hex_of(two.data(), two.size()),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    EXPECT_EQ(
        ecuio_sha256::hex_of(million.data(), million.size()),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, PiecesHashAsTheWhole)
{
    /* Lengths either side of where the padding takes a second block */
    for (size_t n : {55u, 56u, 63u, 64u, 65u, 119u, 120u, 200u}) {
        std::string m;
        for (size_t i = 0; i < n; ++i) {
            m += static_cast<char>('a' + i % 26);
        }
        const std::string whole = ecuio_sha256::hex_of(m.data(), m.size());
        for (size_t step : {1u, 7u, 64u}) {
            EXPECT_EQ(pieces(m, step), whole) << n << " bytes, " << step;
        }
    }
    /* The hasher starts again after finish */
    ecuio_sha256::hasher h;
    h.add("xyz", 3);
    uint8_t d[32];
    h.finish(d);
    h.add("abc", 3);
    h.finish(d);
    EXPECT_EQ(
        ecuio_sha256::hex(d),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
