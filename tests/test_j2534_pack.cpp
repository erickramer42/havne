// File/layer: tests/test_j2534_pack.cpp | GTest, HARNESS tier, Windows
//             only (test_j2534_stream)
// Testing: HARNESS: J2534PackTest (test_j2534_stream)
//
// Service 01 packs sized by their reply's CAN frames (?packframes=,
// ADR-0039 Appendix A) through the surrogate and the fake vendor DLL:
// the adapter frames the reply, so the transport sends the same request
// either way, and every reply the session delivers fits the limit.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "obd2_reply.h" /* core/src: J1979's record lengths */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

/* Service 01 positive replies delivered, and the packs among them */
struct Replies {
    std::mutex m;
    uint32_t replies = 0;
    uint32_t packed = 0;
    uint32_t longest_packed = 0;
};

/* A reply of two or more records, each at J1979's length (the fakes'
 * ECU answers so), filling it exactly: a pack's */
bool a_pack(const uint8_t* d, uint32_t len)
{
    uint32_t at = 1;
    uint32_t records = 0;
    while (at < len) {
        const uint8_t n = ECUIO_OBD2_PID_BYTES[d[at]];
        if (n == 0 || at + 1u + n > len) {
            return false;
        }
        at += 1u + n;
        ++records;
    }
    return records >= 2;
}

void on_frame(const ecuio_frame_t* f, void* user)
{
    auto* r = static_cast<Replies*>(user);
    if (f->data_length < 3 || f->data[0] != 0x41 || f->data[1] % 0x20 == 0) {
        return;
    }
    const std::lock_guard<std::mutex> g(r->m);
    ++r->replies;
    if (a_pack(f->data, f->data_length)) {
        ++r->packed;
        if (f->data_length > r->longest_packed) {
            r->longest_packed = f->data_length;
        }
    }
}

template <typename Done> bool wait_for(Done done, int ms)
{
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/* The longest pack's reply a session on query delivers once packed,
 * over its next 200 Service 01 replies; *packed the packs among them */
uint32_t longest_reply(const char* query, uint32_t* packed)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?identify=0&diagnose=0%s",
             FAKE_J2534_DLL_PATH, query);
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
        << ecuio_last_error_detail();
    if (s == nullptr) {
        return 0;
    }
    Replies r;
    EXPECT_EQ(ecuio_register_frame_callback(s, on_frame, &r), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_channel_support_t sup{};
    EXPECT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_channel_support(s, 0, &sup) == ECUIO_OK &&
                   (sup.flags & ECUIO_SUPPORT_PACKED) != 0;
        },
        10000));
    {
        const std::lock_guard<std::mutex> g(r.m);
        r.replies = r.packed = r.longest_packed = 0;
    }
    EXPECT_TRUE(wait_for(
        [&] {
            const std::lock_guard<std::mutex> g(r.m);
            return r.replies >= 200;
        },
        10000));
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_session_destroy(s);
    *packed = r.packed;
    return r.longest_packed;
}

} // namespace

/* With no limit some packs' replies take a First Frame and more; under
 * packframes=1 none passes one Single Frame (7 bytes), under 2 none
 * passes a First Frame and one Consecutive Frame (13), and packs still
 * go out */
TEST(J2534PackTest, EveryReplyFitsTheLimitThroughTheFake)
{
    uint32_t packed = 0;
    EXPECT_GT(longest_reply("", &packed), 7u) << "six PIDs span frames";
    EXPECT_GT(packed, 0u);
    packed = 0;
    EXPECT_LE(longest_reply("&packframes=1", &packed), 7u);
    EXPECT_GT(packed, 0u) << "packs still go out";
    packed = 0;
    const uint32_t two = longest_reply("&packframes=2", &packed);
    EXPECT_GT(two, 7u);
    EXPECT_LE(two, 13u) << "a First Frame and one Consecutive Frame";
}
