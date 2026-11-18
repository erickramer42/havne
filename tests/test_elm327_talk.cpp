// File/layer: tests/test_elm327_talk.cpp | GTest, HARNESS tier, headless
//             CI job (every platform)
// Testing: HARNESS: Elm327TalkTest (test_elm327)
//
// A talk through an ELM327 session (ADR-0066 section 2.4), handed over by
// the test seam (ecuio_session_test_talk), against the fake ELM327
// (fake_elm327.h), whose engine ECU answers 27 as the simulated ECU does.
// An ELM327 sends a request of one frame, so a talk is 7 bytes at most:
// it is typed whole, with no response-count digit, and a longer one is
// never typed.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_elm327.h"
#include "sim_security.h" /* core/src */

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

/* The seam's numbers (internal.h) */
constexpr int kTaken = 0;
constexpr int kTooLong = 2;
constexpr int kRefusedOffer = 3;
constexpr int kAnswered = 1;
constexpr int kRefused = 2;
constexpr int kUnanswered = 3;

#ifdef _WIN32
constexpr char kPort[] = "COM7";
#else
constexpr char kPort[] = "/dev/fake-elm327";
#endif

/* One DID of the session's own and two it scans (the ECU has neither of
 * those), in the extended session security access answers in; ?gap=
 * spaces the requests, so the fake's first records hold a case's talks */
const std::string kUri = std::string("elm327://") + kPort +
                         "?profile=uds-generic&did=1E1C&did=1E1D-1E1E"
                         "&session=03&identify=0&diagnose=0&gap=100";

struct Ended {
    int offer = -1;
    int end = 0;
    uint8_t nrc = 0;
};

/* The engine ECU's whole replies the session delivered */
struct Replies {
    std::mutex mutex;
    std::vector<Bytes> got;

    static void on_frame(const ecuio_frame_t* f, void* user)
    {
        auto* self = static_cast<Replies*>(user);
        if (f->arbitration_id != 0x7E8u) {
            return;
        }
        const std::lock_guard<std::mutex> lock(self->mutex);
        self->got.emplace_back(f->data, f->data + f->data_length);
    }

    bool has(const Bytes& want)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const Bytes& b : got) {
            if (b == want) {
                return true;
            }
        }
        return false;
    }
};

Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

std::string hex_of(const Bytes& b)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (const uint8_t byte : b) {
        s += digits[byte >> 4];
        s += digits[byte & 0x0F];
    }
    return s;
}

/* What the adapter was sent, in order (the fake's first records) */
std::vector<std::string> typed()
{
    std::vector<std::string> out;
    fake_elm327::with([&](fake_elm327::adapter& a) {
        for (uint32_t i = 0; i < a.commands && i < a.kRecordDepth; ++i) {
            out.emplace_back(a.recorded[i]);
        }
    });
    return out;
}

/* How often command was typed exactly, and the header (the last AT SH)
 * its first went out under */
uint32_t typed_exactly(const std::string& command, std::string* header)
{
    uint32_t n = 0;
    std::string last;
    for (const std::string& c : typed()) {
        if (c.rfind("ATSH", 0) == 0) {
            last = c;
        } else if (c == command) {
            if (n == 0 && header != nullptr) {
                *header = last;
            }
            ++n;
        }
    }
    return n;
}

class Elm327TalkTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;
    Replies replies;
    ecuio_session_t* session = nullptr;

    void SetUp() override
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
    }

    void TearDown() override
    {
        ecuio_session_destroy(session);
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
    }

    void start_session(const std::string& uri = kUri)
    {
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(
            ecuio_register_frame_callback(session, Replies::on_frame, &replies),
            ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }

    /* Hands the session a talk for the engine ECU and waits for its
     * result */
    bool talk(const Bytes& pdu, uint8_t echo, bool first, Ended* out)
    {
        *out = Ended{};
        if (ecuio_session_test_talk(session, 0, pdu.data(),
                                    static_cast<uint32_t>(pdu.size()), echo,
                                    first ? 1 : 0) != ECUIO_OK) {
            return false;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ecuio_session_test_talk_result(session, &out->offer, &out->end,
                                               &out->nrc) == 1) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

} // namespace

TEST_F(Elm327TalkTest, ASeedAndItsKeyAreTypedWholeWithNoCountDigitAndUnlock)
{
    ASSERT_NO_FATAL_FAILURE(start_session());
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x01}, 2, false, &e));
    ASSERT_EQ(e.offer, kTaken);
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    const ecuio_sim_security_level* l = ecuio_sim_security_find(0x01);
    Bytes seed = {0x67, 0x01};
    seed.insert(seed.end(), l->seed, l->seed + l->bytes);
    EXPECT_TRUE(replies.has(seed))
        << "a talk's reply reaches the callbacks as any";

    const Bytes key = key_for(0x01);
    ASSERT_TRUE(talk(key, 2, true, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    ASSERT_TRUE(talk({0x22, 0xD2, 0x10}, 3, false, &e));
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    EXPECT_TRUE(replies.has({0x62, 0xD2, 0x10, 0x4B, 0x2D}))
        << "the gated DID answers once unlocked";

    /* Each typed once, as its bytes alone: the session's own one-shot (a
     * scanned DID) carries a response-count digit, a talk none */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (typed_exactly("221E1D1", nullptr) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(typed_exactly("221E1D1", nullptr), 1u);
    std::string header;
    EXPECT_EQ(typed_exactly("2701", &header), 1u);
    EXPECT_EQ(header, "ATSH7E0") << "the ECU's physical ID";
    EXPECT_EQ(typed_exactly(hex_of(key), &header), 1u);
    EXPECT_EQ(header, "ATSH7E0");
    EXPECT_EQ(typed_exactly("22D210", nullptr), 1u);
    for (const std::string& c : typed()) {
        if (c.rfind("27", 0) == 0) {
            EXPECT_TRUE(c == "2701" || c == hex_of(key)) << c;
        }
    }
    int32_t state = 0;
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_error_state(session, &state, &count), ECUIO_OK);
    EXPECT_EQ(state, ECUIO_OK);
}

TEST_F(Elm327TalkTest, SevenBytesAreTypedAndEightNever)
{
    ASSERT_NO_FATAL_FAILURE(start_session());
    /* A key of five bytes with no seed out: the ECU refuses it (24), and
     * what the case reads is the request typed */
    const Bytes seven = {0x27, 0x02, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    Ended e;
    ASSERT_TRUE(talk(seven, 2, true, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kRefused);
    EXPECT_EQ(e.nrc, ECUIO_SIM_NRC_SEQUENCE);
    EXPECT_EQ(typed_exactly("2702A1A2A3A4A5", nullptr), 1u)
        << "refused, and not asked again";

    Bytes eight = seven;
    eight.push_back(0xA6);
    ASSERT_TRUE(talk(eight, 2, true, &e));
    EXPECT_EQ(e.offer, kTooLong) << "an ELM327 sends one frame";
    EXPECT_EQ(e.end, 0);
    /* the session goes on, and nothing of the eight was typed */
    ASSERT_TRUE(talk({0x3E, 0x00}, 2, false, &e));
    EXPECT_EQ(e.end, kAnswered);
    for (const std::string& c : typed()) {
        EXPECT_NE(c.rfind("2702A1A2A3A4A5A", 0), 0u) << c;
    }
}

TEST_F(Elm327TalkTest, NothingOutsideTheCeilingIsTyped)
{
    ASSERT_NO_FATAL_FAILURE(start_session());
    Ended e;
    for (const Bytes& pdu :
         {Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x10, 0x02}, Bytes{0x31, 0x01},
          Bytes{0x27, 0x81}, Bytes{0x14, 0xFF, 0xFF, 0xFF}}) {
        ASSERT_TRUE(talk(pdu, 1, true, &e)) << int{pdu[0]};
        EXPECT_EQ(e.offer, kRefusedOffer) << int{pdu[0]};
        EXPECT_EQ(e.end, 0);
    }
    ASSERT_TRUE(talk({0x3E, 0x00}, 2, false, &e));
    EXPECT_EQ(e.end, kAnswered) << "the session is none the worse";
    for (const std::string& c : typed()) {
        if (c.rfind("AT", 0) == 0 || c.rfind("ST", 0) == 0) {
            continue;
        }
        EXPECT_TRUE(c.rfind("1003", 0) == 0 || c.rfind("22", 0) == 0 ||
                    c.rfind("3E00", 0) == 0)
            << c << " was typed";
    }
}

/* The session's own read of a DID whose reply the count digit cut is
 * taken back and asked again, and again when the ECU, still sending the
 * rest, answers NO DATA. A talk for the same DID that gets in ahead of
 * the read's next turn meets that NO DATA and is never taken back: it
 * was typed, so it ends unanswered, not unsent. */
TEST_F(Elm327TalkTest, ATalkTypedIntoACutRepliesRestIsNotTakenBack)
{
    /* 5 s a Consecutive Frame: the ECU is busy for the whole case */
    fake_elm327::with([](fake_elm327::adapter& a) { a.k.cut_gap_ms = 5000; });
    ASSERT_NO_FATAL_FAILURE(
        start_session(std::string("elm327://") + kPort +
                      "?profile=uds-generic&did=F1A0&identify=0"
                      "&diagnose=0"));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (typed_exactly("22F1A01", nullptr) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(typed_exactly("22F1A01", nullptr), 1u)
        << "the session's read, cut";
    Ended e;
    ASSERT_TRUE(talk({0x22, 0xF1, 0xA0}, 3, true, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnanswered) << "typed: never unsent";
}
