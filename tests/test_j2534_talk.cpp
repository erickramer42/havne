// File/layer: tests/test_j2534_talk.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534TalkTest (test_j2534_health); Cross-arch
//
// A talk through a J2534 session (ADR-0066 section 2.4), handed over by
// the test seam (ecuio_session_test_talk): a request longer than a frame
// goes to the adapter whole, in one write on the ECU's physical ID, and
// the adapter segments it; between a seed and its key the session writes
// nothing. Every session runs in a surrogate (j2534_backend_fixture.h),
// so the request also crosses the pipe, and the fake DLL's engine ECU
// answers 27 as the simulated ECU does, strict under its knob.

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
constexpr int kPlain = 0;
constexpr int kSeed = 1;
constexpr int kKey = 2;
constexpr int kTaken = 0;
constexpr int kTooLong = 2;
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

/* A level's seed as the ECU answers it */
Bytes seed_of(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes reply = {0x67, level};
    reply.insert(reply.end(), l->seed, l->seed + l->bytes);
    return reply;
}

/* A request of 18 bytes that is no key */
Bytes long_read()
{
    Bytes pdu = {0x2C, 0x01};
    for (uint8_t k = 0; k < 16; ++k) {
        pdu.push_back(static_cast<uint8_t>(0xA0 + k));
    }
    return pdu;
}

/* A plain talk the engine ECU answers in one frame, in any session: a
 * guest's read (core's own 10 03 and 3E 00 are no talk) */
const Bytes kRead = {0x22, 0x1E, 0x1C};
const Bytes kGated = {0x22, 0xD2, 0x10};
const Bytes kGatedData = {0x62, 0xD2, 0x10, 0x4B, 0x2D};

/* ?gap=150 spaces the requests, so the page's first 16 writes hold the
 * whole exchange; the session's own reads are one DID */
const char* const kQuery =
    "?profile=uds-generic&did=1E1C&session=03&identify=0&diagnose=0&gap=150";
/* and tester present after half a second with nothing sent */
const char* const kKeepAlive =
    "?profile=uds-generic&did=1E1C&session=03&keepalive_ms=500&identify=0"
    "&diagnose=0&gap=150";

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

    /* A session whose engine ECU wants its key straight after its seed */
    void start_strict(const char* query)
    {
        ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_SECURITY_STRICT", "1"));
        ASSERT_NO_FATAL_FAILURE(start_session(query));
    }

    /* Hands the session a talk for the engine ECU and waits for its
     * result */
    bool talk(const Bytes& pdu, uint8_t echo, int kind, Ended* out,
              uint32_t window_ms = 1000, int budget_ms = 8000)
    {
        *out = Ended{};
        if (ecuio_session_test_talk(session, 0, pdu.data(),
                                    static_cast<uint32_t>(pdu.size()), echo,
                                    kind, window_ms) != ECUIO_OK) {
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

    /* The session's link state, once it is want or the budget is spent */
    int32_t state_after_waiting_for(int32_t want, int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        int32_t state = 0;
        uint32_t count = 0;
        do {
            (void)ecuio_session_get_error_state(session, &state, &count);
            if (state == want) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return state;
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

    /* Where the first kept write whose request starts with head stands,
     * -1 for nowhere; *w is that write */
    int write_index(const Bytes& head, fake_control::Write* w)
    {
        for (uint32_t i = 0; fake_control::recorded_write(page, i, w); ++i) {
            if (w->size >= 4 + head.size() &&
                memcmp(w->data + 4, head.data(), head.size()) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
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

TEST_F(J2534TalkTest, ASeedAndItsEighteenByteKeyUnlockEachInOneWrite)
{
    ASSERT_NO_FATAL_FAILURE(start_strict(kQuery));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x03}, 2, kSeed, &e));
    ASSERT_EQ(e.offer, kTaken);
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    EXPECT_TRUE(wait_for_reply(seed_of(0x03)))
        << "a talk's reply reaches the callbacks as any" << replies.text();

    const Bytes key = key_for(0x03);
    ASSERT_EQ(key.size(), 18u);
    ASSERT_TRUE(talk(key, 2, kKey, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc} << writes_text();
    EXPECT_TRUE(wait_for_reply({0x67, 0x04})) << replies.text();
    ASSERT_TRUE(talk(kGated, 3, kPlain, &e));
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    EXPECT_TRUE(wait_for_reply(kGatedData))
        << "the gated DID answers once unlocked" << replies.text();

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

/* The key comes 2.5 s after its seed, with tester present due after half
 * a second and the session's read every 150 ms: the adapter is handed
 * nothing in between, and the strict ECU unlocks. */
TEST_F(J2534TalkTest, NothingIsWrittenBetweenASeedAndItsKey)
{
    ASSERT_NO_FATAL_FAILURE(start_strict(kKeepAlive));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x01}, 2, kSeed, &e, 4000));
    ASSERT_EQ(e.offer, kTaken);
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const Bytes key = key_for(0x01);
    ASSERT_TRUE(talk(key, 2, kKey, &e));
    ASSERT_EQ(e.offer, kTaken) << "inside its window";
    EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc} << writes_text();
    ASSERT_TRUE(talk(kGated, 3, kPlain, &e));
    EXPECT_EQ(e.end, kAnswered) << "the strict ECU unlocked";

    fake_control::Write seed;
    fake_control::Write sent_key;
    const int at = write_index({0x27, 0x01}, &seed);
    ASSERT_GE(at, 0) << writes_text();
    ASSERT_EQ(write_index({0x27, 0x02}, &sent_key), at + 1)
        << "a write between the seed and its key" << writes_text();
    EXPECT_GE(sent_key.us - seed.us, 2400000u);
}

/* A key offered after its seed's window is refused at the door, and what
 * the session writes next is tester present, held back meanwhile. */
TEST_F(J2534TalkTest, AKeyAfterItsWindowIsRefusedAndTesterPresentFollows)
{
    ASSERT_NO_FATAL_FAILURE(start_strict(kKeepAlive));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x01}, 2, kSeed, &e, 800));
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    ASSERT_TRUE(talk(key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer);
    EXPECT_EQ(e.end, 0);
    ASSERT_TRUE(wait_for_reply({0x7E, 0x00})) << writes_text();
    fake_control::Write seed;
    fake_control::Write next;
    const int at = write_index({0x27, 0x01}, &seed);
    ASSERT_GE(at, 0) << writes_text();
    ASSERT_TRUE(fake_control::recorded_write(page, at + 1, &next))
        << writes_text();
    EXPECT_EQ(next.data[4], 0x3E) << writes_text();
    EXPECT_GE(next.us - seed.us, 800000u) << "not before the window's end";
    EXPECT_TRUE(writes_of({0x27, 0x02}).empty()) << writes_text();
}

/* While the link isn't OK no window stands: a key offered across an
 * adapter's failure is refused. A second module keeps the session
 * reading through the window, so the failed read is seen. */
TEST_F(J2534TalkTest, AnAdaptersFailureEndsTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(
        start_strict("?profile=uds-generic&did=1E1C,7E9:D801:u16&session=03"
                     "&identify=0&diagnose=0&probe=0&gap=150"));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x01}, 2, kSeed, &e, 4000));
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", "7"));
    ASSERT_EQ(state_after_waiting_for(ECUIO_ERR_TRANSPORT_HARD_FAILURE, 3000),
              ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(talk(key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer)
        << "the seed may not have outlived what failed";
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_READ_STATUS", nullptr));
    EXPECT_EQ(state_after_waiting_for(ECUIO_OK, 10000), ECUIO_OK);
}

/* A session whose requests go to the functional address (?txid=0x7DF):
 * attach marks the channel, so it takes no seed and no talk longer than
 * a frame. */
TEST_F(J2534TalkTest, AFunctionalChannelTakesNoSeedAndNoLongTalk)
{
    ASSERT_NO_FATAL_FAILURE(
        start_session("?profile=uds-generic&did=1E1C&txid=0x7DF&identify=0"
                      "&diagnose=0&gap=150"));
    Ended e;
    ASSERT_TRUE(talk({0x27, 0x01}, 2, kSeed, &e));
    EXPECT_EQ(e.offer, kRefusedOffer);
    Bytes read = {0x22, 0xD3, 0x01, 0xD3, 0x02, 0xD3, 0x03, 0xD3};
    ASSERT_TRUE(talk(read, 3, kPlain, &e));
    EXPECT_EQ(e.offer, kTooLong) << "8 bytes to every module at once";
    read.pop_back();
    ASSERT_TRUE(talk(read, 3, kPlain, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent);
    const std::vector<fake_control::Write> sent = writes_of({0x22, 0xD3, 0x01});
    ASSERT_EQ(sent.size(), 1u) << writes_text();
    const uint8_t functional[4] = {0x00, 0x00, 0x07, 0xDF};
    EXPECT_EQ(memcmp(sent[0].data, functional, 4), 0);
    EXPECT_TRUE(writes_of({0x27}).empty()) << writes_text();
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
    ASSERT_TRUE(talk(pdu, 3, kPlain, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent) << "end " << e.end;
    const std::vector<fake_control::Write> sent = writes_of({0x22, 0xA1, 0xA2});
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].size, 64u);
    EXPECT_EQ(memcmp(sent[0].data + 4, pdu.data(), pdu.size()), 0);
    /* and one byte more, or a kind there is none of, never leaves the
     * caller */
    pdu.push_back(0x00);
    EXPECT_EQ(ecuio_session_test_talk(session, 0, pdu.data(), 61, 3, kPlain, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(session, 0, pdu.data(), 3, 3, 3, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(session, 0, pdu.data(), 3, 3, -1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST_F(J2534TalkTest, NothingOutsideTheCeilingIsWritten)
{
    ASSERT_NO_FATAL_FAILURE(start_session(kQuery));
    Ended e;
    /* the last two: a 27 that is no seed, and a key with no window */
    for (const Bytes& pdu :
         {Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x10, 0x02}, Bytes{0x31, 0x01},
          Bytes{0x27, 0x81}, Bytes{0x14, 0xFF, 0xFF, 0xFF},
          Bytes{0x27, 0x01}}) {
        ASSERT_TRUE(talk(pdu, 2, kPlain, &e)) << int{pdu[0]};
        EXPECT_EQ(e.offer, kRefusedOffer) << int{pdu[0]};
        EXPECT_EQ(e.end, 0);
    }
    ASSERT_TRUE(talk(key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer) << "a key with no seed before it";
    ASSERT_TRUE(talk(kRead, 3, kPlain, &e));
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
    ASSERT_TRUE(talk(kRead, 3, kPlain, &e));
    ASSERT_EQ(e.end, kAnswered);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", "7"));
    ASSERT_TRUE(talk(long_read(), 2, kPlain, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent);
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_WRITE_STATUS", nullptr));
    /* Writes go again: the session's own requests do, the talk doesn't */
    ASSERT_TRUE(talk(kRead, 3, kPlain, &e, 0, 15000));
    EXPECT_EQ(e.end, kAnswered);
    EXPECT_TRUE(writes_of({0x2C, 0x01}).empty())
        << "a talk is never sent twice, nor once late";
}

#endif /* _WIN32 */
