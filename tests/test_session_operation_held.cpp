#include "plugin_kernel_support.h"
#include "session_operation_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace session_operation;
using engine_talk_test::kMs;
using kernel_test::Kernel;

constexpr uint32_t kBytes = 0x4000;
constexpr uint16_t kBlock = 0x22;
constexpr int64_t kHeldUs = 6000 * kMs;
const std::vector<size_t> kHeldAt = {100, 250, 400};
const char kNoKeepAlive[] = R"([{"request": "10 82"}])";

conversation_test::Made long_read(const std::string& functional = {})
{
    Kernel k;
    k.blob.resize(2000);
    EXPECT_EQ(ecuio_sim_kernel_image(k.blob.data(), 2000), 2000u);
    if (!functional.empty()) {
        k.functional = functional;
    }
    k.ranges =
        R"({"step": "upload", "name": "code", "session": "programming", )"
        R"("address": "00080000", "bytes": )" +
        std::to_string(kBytes) +
        R"(, "address_bytes": 4, "size_bytes": 4, "dfi": "00"})";
    conversation_test::Made m = made();
    m.operations = k.json();
    m.files_extra = R"(, "kernel.bin": {"kind": "bin", "base64": ")" +
                    conversation_test::base64(k.blob) + "\"}";
    return m;
}

bool is_block_ask(const Bytes& q)
{
    return q.size() == 2 && q[0] == 0x36;
}

/* The module's answer is fixed at the ask, as its S3 counts from it */
void hold_blocks(Bench& b, std::vector<int64_t>* asked_at)
{
    b.steps_while_out = true;
    b.node.s.kernel.on = true;
    b.node.s.memory.faults.block = kBlock;
    auto nth = std::make_shared<size_t>(0);
    b.script = [&b, asked_at, nth](const Bytes& q, size_t, Answer* out) {
        if (!is_block_ask(q) || !b.node.s.kernel.running) {
            return false;
        }
        const size_t n = ++*nth;
        if (std::find(kHeldAt.begin(), kHeldAt.end(), n) == kHeldAt.end()) {
            return false;
        }
        asked_at->push_back(b.now);
        const Bytes wait = {0x7F, 0x36, 0x78};
        out->frames = {wait, wait, wait, b.node.ask(q)};
        out->pending_us = kHeldUs / 3;
        return true;
    };
}

size_t keepalives_in(const Bench& b, int64_t from, int64_t to)
{
    size_t n = 0;
    for (size_t i = 0; i < b.functional.size(); ++i) {
        n += b.functional[i].second == Bytes{0x3E, 0x80} &&
                     b.functional_at[i] > from && b.functional_at[i] <= to
                 ? 1u
                 : 0u;
    }
    return n;
}

ecuio_status_t start_read(Bench& b)
{
    const ecuio_operation_request_t q = request(ECUIO_OPERATION_MAY_UNLOCK);
    return ecuio_session_operation_start(b.s, &q, b.folder.str().c_str());
}

} // namespace

TEST(SessionOperationTest,
     TheKeepAliveHoldsTheProgrammingSessionThroughLongWaits)
{
    Bench b;
    std::vector<int64_t> held;
    hold_blocks(b, &held);
    ASSERT_NO_FATAL_FAILURE(b.start(long_read()));
    ASSERT_EQ(start_read(b), ECUIO_OK) << ecuio_last_error_detail();
    const int64_t began = b.now;
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    ASSERT_EQ(held.size(), kHeldAt.size());
    EXPECT_GT(b.now - began, 3 * ECUIO_SIM_S3_US);
    for (const int64_t t : held) {
        EXPECT_GE(keepalives_in(b, t, t + kHeldUs), 2u)
            << "the module's S3 outlasted by its wait at " << t;
    }
    size_t first = 0;
    while (first < b.functional.size() &&
           b.functional[first].second != Bytes{0x3E, 0x80}) {
        ++first;
    }
    ASSERT_LT(first, b.functional.size());
    for (size_t i = first + 1; i < b.functional.size(); ++i) {
        EXPECT_LE(b.functional_at[i] - b.functional_at[i - 1],
                  2000 * kMs + 2 * ECUIO_ENGINE_GAP_US + 100 * kMs)
            << i;
    }
    size_t blocks = 0;
    for (const Bytes& q : b.sent) {
        blocks += is_block_ask(q) ? 1u : 0u;
    }
    EXPECT_EQ(blocks, 2u * (kBytes / (kBlock - 2u)))
        << "the code read twice, no block asked again";
    EXPECT_EQ(b.node.s.kernel.downloads, 1u);
    EXPECT_EQ(b.node.s.kernel.starts, 1u);
    const std::vector<std::string> names = b.files();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(image_at((b.folder.path / names[0]).generic_u8string()),
              memory_of(ECUIO_SIM_DEFAULT_VIN, 0x00080000, kBytes));
}

TEST(SessionOperationTest, WithNoKeepAliveTheFirstLongWaitLosesTheSession)
{
    Bench b;
    std::vector<int64_t> held;
    hold_blocks(b, &held);
    ASSERT_NO_FATAL_FAILURE(b.start(long_read(kNoKeepAlive)));
    ASSERT_EQ(start_read(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_NE(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(held.size(), 1u) << v.words;
    EXPECT_EQ(keepalives_in(b, 0, b.now), 0u);
    EXPECT_FALSE(b.node.s.programming) << "S3 ran out";
    EXPECT_TRUE(b.files().empty());
}

TEST(SessionOperationTest, AStrayRefusalOfTheKeepAliveLeavesTheUploadWhole)
{
    Bench b;
    b.node.s.kernel.on = true;
    b.node.s.memory.faults.block = kBlock;
    auto nth = std::make_shared<size_t>(0);
    b.script = [&b, nth](const Bytes& q, size_t, Answer* out) {
        if (!is_block_ask(q) || !b.node.s.kernel.running || ++*nth % 50 != 0) {
            return false;
        }
        out->frames = {Bytes{0x7F, 0x3E, 0x21}, b.node.ask(q)};
        out->pending_us = 2 * kMs;
        return true;
    };
    ASSERT_NO_FATAL_FAILURE(b.start(long_read()));
    ASSERT_EQ(start_read(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_GE(*nth, 50u);
    const std::vector<std::string> names = b.files();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(image_at((b.folder.path / names[0]).generic_u8string()),
              memory_of(ECUIO_SIM_DEFAULT_VIN, 0x00080000, kBytes));
}
