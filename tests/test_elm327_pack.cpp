// File/layer: tests/test_elm327_pack.cpp | GTest, HARNESS tier, headless
//             CI job (every platform)
// Testing: HARNESS: Elm327PackTest (test_elm327)
//
// Service 01 packs sized by their reply's CAN frames (?packframes=,
// ADR-0039 Appendix A) over ELM327, against the fake ELM327
// (fake_elm327.h): the count digit each pack is typed with, and the
// OBDX Pro VX's late prompt after a reply of more than one frame. Judged
// on the fake's command record and request stamps (the first 96), not on
// counts in a wall-clock window.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "fake_elm327.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
constexpr char kPort[] = "COM7";
#else
constexpr char kPort[] = "/dev/fake-elm327";
#endif

template <typename Done> bool wait_for(Done done, int ms)
{
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

struct Session {
    ecuio_session_t* s = nullptr;
    ~Session() { ecuio_session_destroy(s); }
    ecuio_status_t create(const std::string& query)
    {
        const std::string uri = std::string("elm327://") + kPort + query;
        return ecuio_session_create(uri.c_str(), &s);
    }
};

uint32_t fake_counter(uint32_t fake_elm327::adapter::* field)
{
    uint32_t n = 0;
    fake_elm327::with([&](fake_elm327::adapter& a) { n = a.*field; });
    return n;
}

/* The packs the fake was typed (01, two or more PIDs, the digit), each
 * with its digit and the fakes' ECU's reply length */
struct TypedPack {
    std::string cmd;
    unsigned digit;
    size_t reply;
};

std::vector<TypedPack> typed_packs()
{
    std::vector<std::string> cmds;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        for (uint32_t i = 0; i < a.commands && i < a.kRecordDepth; ++i) {
            cmds.emplace_back(a.recorded[i]);
        }
    });
    std::vector<TypedPack> out;
    for (const std::string& cmd : cmds) {
        if (cmd.size() < 7 || cmd.size() % 2 == 0 ||
            cmd.compare(0, 2, "01") != 0) {
            continue;
        }
        unsigned char req[8] = {};
        const size_t len = (cmd.size() - 1) / 2;
        for (size_t i = 0; i < len; ++i) {
            req[i] = static_cast<unsigned char>(
                std::stoul(cmd.substr(2 * i, 2), nullptr, 16));
        }
        unsigned char reply[fake_ecu::kReplyMax] = {};
        size_t n = 0;
        EXPECT_TRUE(fake_ecu::answer(req, len, -1, reply, &n)) << cmd;
        out.push_back({cmd,
                       static_cast<unsigned>(
                           std::stoul(cmd.substr(cmd.size() - 1), nullptr, 16)),
                       n});
    }
    return out;
}

/* A session on query against the VX's late prompt (prompt_ms): the
 * median gap between the fake's last n recorded requests, in ms, once it
 * has recorded them all */
double median_request_gap_ms(const std::string& query, uint32_t prompt_ms,
                             size_t n)
{
    fake_elm327::installed installed;
    fake_elm327::with([prompt_ms](fake_elm327::adapter& a) {
        a.k.multi_frame_prompt_ms = prompt_ms;
    });
    Session s;
    EXPECT_EQ(s.create(query), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_start(s.s), ECUIO_OK);
    EXPECT_TRUE(wait_for(
        [] {
            return fake_counter(&fake_elm327::adapter::requests) >=
                   fake_elm327::adapter::kRecordDepth;
        },
        20000));
    std::vector<int64_t> at;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        at.assign(a.request_us, a.request_us + a.kRecordDepth);
    });
    std::vector<int64_t> gaps;
    for (size_t i = at.size() - n; i < at.size(); ++i) {
        gaps.push_back(at[i] - at[i - 1]);
    }
    std::sort(gaps.begin(), gaps.end());
    return static_cast<double>(gaps[gaps.size() / 2]) / 1000.0;
}

} // namespace

/* ?packframes=1: every pack's reply is one Single Frame, so every pack is
 * typed with the digit 1, and the report says packs were answered */
TEST(Elm327PackTest, OneFramePacksAreTypedWithTheDigitOne)
{
    fake_elm327::installed installed;
    {
        Session s;
        ASSERT_EQ(s.create("?identify=0&diagnose=0&packframes=1"), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_session_start(s.s), ECUIO_OK);
        ASSERT_TRUE(wait_for(
            [] {
                return fake_counter(&fake_elm327::adapter::commands) >=
                       fake_elm327::adapter::kRecordDepth;
            },
            10000));
        ecuio_channel_support_t sup{};
        ASSERT_EQ(ecuio_session_get_channel_support(s.s, 0, &sup), ECUIO_OK);
        EXPECT_TRUE(sup.flags & ECUIO_SUPPORT_PACKED);
    }
    /* the first 96 commands hold the setup, the walk and a lap alone */
    const std::vector<TypedPack> packs = typed_packs();
    EXPECT_GE(packs.size(), 3u);
    for (const TypedPack& p : packs) {
        EXPECT_EQ(p.digit, 1u) << p.cmd;
        EXPECT_LE(p.reply, 7u) << p.cmd;
    }
}

/* The VX in ELM mode prints its prompt late after a reply of more than
 * one frame (the fake's multi_frame_prompt_ms): packs of six wait it out
 * each time, one-frame packs never do */
TEST(Elm327PackTest, OneFramePacksMissTheVxsLatePrompt)
{
    constexpr uint32_t kPromptMs = 200;
    constexpr size_t kLast = 24; /* past the walk and the first lap */
    const double six =
        median_request_gap_ms("?identify=0&diagnose=0", kPromptMs, kLast);
    const double one = median_request_gap_ms(
        "?identify=0&diagnose=0&packframes=1", kPromptMs, kLast);
    EXPECT_GE(six, kPromptMs) << "a pack of six waits out the prompt";
    EXPECT_LT(one, kPromptMs / 2.0) << "a one-frame pack doesn't";
}
