// File/layer: tests/test_j2534_talk.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534TalkTest (test_j2534_health); Cross-arch
//
// A talk through a J2534 session (ADR-0066 section 2.4), handed over by
// the test seam (ecuio_session_test_talk): a request longer than a frame
// goes to the adapter whole, in one write on the ECU's physical ID, and
// the adapter segments it. Every session runs in a surrogate
// (j2534_backend_fixture.h), so the request also crosses the pipe.

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h"

#ifdef _WIN32
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "j2534_backend_fixture.h"
#include "sim_security.h"

namespace {

using Bytes = std::vector<uint8_t>;

/* The seam's numbers (internal.h) */
constexpr int kTaken = 0;
constexpr int kRefusedOffer = 3;
constexpr int kAnswered = 1;
constexpr int kUnsent = 4;

/* The whole messages the session delivered from the engine ECU */
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

    /* What came, one message per line, for a failure's message */
    std::string text()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        std::string out = "\n";
        char hex[8];
        for (const Bytes& b : got) {
            out += " ";
            for (const uint8_t byte : b) {
                snprintf(hex, sizeof hex, " %02X", byte);
                out += hex;
            }
            out += "\n";
        }
        return out;
    }
};

struct Ended {
    int offer = -1;
    int end = 0;
    uint8_t nrc = 0;
};

Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

/* ?gap=150 spaces the requests, so the page's first 16 writes hold the
 * whole exchange; the session's own reads are one DID */
const char* const kQuery =
    "?profile=uds-generic&did=1E1C&session=03&identify=0&diagnose=0&gap=150";

class J2534TalkTest : public j2534_backends::BackendTest
{
protected:
    ecuio_session_t* session = nullptr;
    Replies replies;

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }

    void start_session(const char* query)
    {
        const std::string u = uri(query);
        ASSERT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_OK)
            << u << ": " << ecuio_last_error_detail();
        ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
        ASSERT_EQ(
            ecuio_register_frame_callback(session, Replies::on_frame, &replies),
            ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }

    /* Hands the session a talk for the engine ECU and waits for its
     * result */
    bool talk(const Bytes& pdu, uint8_t echo, bool first, Ended* out,
              int budget_ms = 8000)
    {
        *out = Ended{};
        if (ecuio_session_test_talk(session, 0, pdu.data(),
                                    static_cast<uint32_t>(pdu.size()), echo,
                                    first ? 1 : 0) != ECUIO_OK) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ecuio_session_test_talk_result(session, &out->offer, &out->end,
                                               &out->nrc) == 1) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool wait_for_reply(const Bytes& want, int budget_ms = 3000)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (replies.has(want)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    /* The page's kept writes whose request (after the 4-byte ID) starts
     * with head */
    std::vector<fake_control::Write> writes_of(const Bytes& head)
    {
        std::vector<fake_control::Write> out;
        fake_control::Write w;
        for (uint32_t i = 0; fake_control::recorded_write(page, i, &w); ++i) {
            if (w.size >= 4 + head.size() &&
                memcmp(w.data + 4, head.data(), head.size()) == 0) {
                out.push_back(w);
            }
        }
        return out;
    }

    /* The page's kept writes, one per line, for a failure's message */
    std::string writes_text()
    {
        std::string out = "\n";
        fake_control::Write w;
        char hex[8];
        for (uint32_t i = 0; fake_control::recorded_write(page, i, &w); ++i) {
            out += "  " + std::to_string(w.us / 1000) + " ms:";
            for (uint32_t b = 0; b < w.size; ++b) {
                snprintf(hex, sizeof hex, " %02X", w.data[b]);
                out += hex;
            }
            out += "\n";
        }
        return out;
    }
};

} // namespace

/* The fake DLL answers a 27 with its echo (byte 0 + 0x40, the rest as
 * written): what the case reads is the write. The unlock itself is
 * SocketCanTalkTest's, against the simulated ECU. */
TEST_F(J2534TalkTest, AnEighteenByteKeyGoesOutInOneWrite)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kQuery));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x03}, 2, false, &e));
    ASSERT_EQ(e.offer, kTaken);
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};

    const Bytes key = key_for(0x03);
    ASSERT_EQ(key.size(), 18u);
    ASSERT_TRUE(talk(key, 2, true, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    Bytes echoed = key;
    echoed[0] = 0x67;
    EXPECT_TRUE(wait_for_reply(echoed))
        << "a talk's reply reaches the callbacks as any" << replies.text();
    EXPECT_TRUE(replies.has({0x67, 0x03})) << replies.text();

    /* What the adapter was handed: the key whole, once, on the ECU's
     * physical ID, padded as every request; the seed's request likewise */
    const std::vector<fake_control::Write> keys = writes_of({0x27, 0x04});
    ASSERT_EQ(keys.size(), 1u) << "a key goes out once" << writes_text();
    EXPECT_EQ(keys[0].size, 4u + 18u);
    const uint8_t physical[4] = {0x00, 0x00, 0x07, 0xE0};
    EXPECT_EQ(memcmp(keys[0].data, physical, 4), 0)
        << "never the functional ID";
    EXPECT_EQ(memcmp(keys[0].data + 4, key.data(), key.size()), 0);
    EXPECT_NE(keys[0].tx_flags & ECUIO_PT_TX_ISO15765_FRAME_PAD, 0u);
    const std::vector<fake_control::Write> seeds = writes_of({0x27, 0x03});
    ASSERT_EQ(seeds.size(), 1u);
    EXPECT_EQ(seeds[0].size, 6u);
    EXPECT_EQ(memcmp(seeds[0].data, physical, 4), 0);
    int32_t state = 0;
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_error_state(session, &state, &count), ECUIO_OK);
    EXPECT_EQ(state, ECUIO_OK);
}

TEST_F(J2534TalkTest, TheLongestTalkCrossesThePipeWhole)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kQuery));
    /* 60 bytes: with the ID, the 64 one write holds. The engine ECU
     * refuses the DID; the request is what the case reads */
    Bytes pdu(60, 0x00);
    pdu[0] = 0x22;
    for (size_t i = 1; i < pdu.size(); ++i) {
        pdu[i] = static_cast<uint8_t>(0xA0 + i);
    }
    Ended e;
    ASSERT_TRUE(talk(pdu, 1, false, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent) << "end " << e.end;
    const std::vector<fake_control::Write> sent = writes_of({0x22, 0xA1, 0xA2});
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].size, 64u);
    EXPECT_EQ(memcmp(sent[0].data + 4, pdu.data(), pdu.size()), 0);
    /* and one byte more never leaves the caller */
    pdu.push_back(0x00);
    EXPECT_EQ(ecuio_session_test_talk(session, 0, pdu.data(), 61, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST_F(J2534TalkTest, NothingOutsideTheCeilingIsWritten)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kQuery));
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
    fake_control::Write w;
    for (uint32_t i = 0; fake_control::recorded_write(page, i, &w); ++i) {
        const uint8_t sid = w.data[4];
        EXPECT_TRUE(sid == 0x10 || sid == 0x22 || sid == 0x3E)
            << "write " << i << ": service " << int{sid};
        if (sid == 0x10) {
            EXPECT_EQ(w.data[5], 0x03) << "write " << i;
        }
    }
}

TEST_F(J2534TalkTest, AWriteTheAdapterRefusesSettlesItUnsentAndItIsNotSentAgain)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kQuery));
    Ended e;
    ASSERT_TRUE(talk({0x3E, 0x00}, 2, false, &e));
    ASSERT_EQ(e.end, kAnswered);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));
    const Bytes key = key_for(0x01);
    ASSERT_TRUE(talk(key, 2, true, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", nullptr));
    /* Writes go again: the session's own requests do, the key doesn't */
    ASSERT_TRUE(talk({0x3E, 0x00}, 2, false, &e, 15000));
    EXPECT_EQ(e.end, kAnswered);
    EXPECT_TRUE(writes_of({0x27, 0x02}).empty())
        << "a key is never sent twice for one seed, nor once late";
}

#endif /* _WIN32 */
