// File/layer: tests/test_signals_support.h | GTest, core signal tests, C++17, Qt-free
// Testing: UNIT: the helpers of test_signals*.cpp (test_signals)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/signals.h>
#include <ecuio/signal_sets.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>

class SignalDecodeTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

namespace {

/* A valid 32-bit field at byte 0: only the kind, the byte order and
 * the field shape vary in the tests below. Takes ints so a test can
 * pass values no enum names. */
inline ecuio_signal_def_t four_byte_def(int kind, int byte_order = ECUIO_BYTE_ORDER_LE)
{
    ecuio_signal_def_t d = {};
    d.id = 1;
    d.kind = static_cast<uint8_t>(kind);
    d.byte_order = static_cast<uint8_t>(byte_order);
    d.start_byte = 0;
    d.bit_offset = 0;
    d.bit_length = 32;
    d.scale = 1.0;
    return d;
}

} // namespace
