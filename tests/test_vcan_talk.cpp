// File/layer: tests/test_vcan_talk.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanTalkTest (test_vcan)
//
// A talk through a requesting socketcan:// session on the bus (ADR-0066
// section 2.4, core/src/socketcan_send.h), handed over by the test seam
// (ecuio_session_test_talk): a request longer than a frame crosses to the
// kernel's ISO-TP and to the simulated ECU's, and a send that can't
// finish is never sent again.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/sim.h"
#include "fake_ecus.h"
#include "sim_security.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<uint8_t>;

/* The seam's numbers (internal.h) */
constexpr int kTaken = 0;
constexpr int kRefusedOffer = 3;
constexpr int kAnswered = 1;
constexpr int kRefused = 2;
constexpr int kUnanswered = 3;
constexpr int kUnsent = 4;

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/* The whole messages a session delivered from the engine ECU */
struct Replies {
    std::mutex mutex;
    std::vector<Bytes> got;

    static void on_frame(const ecuio_frame_t* f, void* user)
    {
        auto* self = static_cast<Replies*>(user);
        if (f->arbitration_id != vcan_bus::kReply ||
            (f->flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
            return;
        }
        const std::lock_guard<std::mutex> lock(self->mutex);
        self->got.emplace_back(f->data, f->data + f->data_length);
    }

    /* The newest reply that starts with head, empty for none */
    Bytes last(const Bytes& head)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it = got.rbegin(); it != got.rend(); ++it) {
            if (it->size() >= head.size() &&
                std::equal(head.begin(), head.end(), it->begin())) {
                return *it;
            }
        }
        return {};
    }
};

/* A started uds-generic session in the extended session, its replies
 * collected; closed and destroyed when it goes */
class Session
{
public:
    explicit Session(const std::string& query =
                         "?profile=uds-generic&did=D200:u16&session=03",
                     bool started = true)
    {
        const std::string uri =
            std::string("socketcan://") + vcan_bus::interface_name() + query;
        status_ = ecuio_session_create(uri.c_str(), &s_);
        detail_ = ecuio_last_error_detail();
        if (status_ == ECUIO_OK) {
            status_ =
                ecuio_register_frame_callback(s_, Replies::on_frame, &replies);
        }
        if (status_ == ECUIO_OK && started) {
            status_ = ecuio_session_start(s_);
        }
    }
    /* A session made not started */
    ecuio_status_t start() { return ecuio_session_start(s_); }
    ~Session()
    {
        if (s_ != nullptr) {
            ecuio_session_close(s_);
            ecuio_session_destroy(s_);
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_session_t* get() const { return s_; }
    Replies replies;

private:
    ecuio_session_t* s_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

struct Ended {
    int offer = -1;
    int end = 0;
    uint8_t nrc = 0;
};

/* Hands the session a talk for the engine ECU and waits for its result */
bool talk(const Session& s, const Bytes& pdu, uint8_t echo, bool first,
          Ended* out, std::chrono::milliseconds limit = 5000ms)
{
    *out = Ended{};
    if (ecuio_session_test_talk(s.get(), 0, pdu.data(),
                                static_cast<uint32_t>(pdu.size()), echo,
                                first ? 1 : 0) != ECUIO_OK) {
        return false;
    }
    return wait_for(
        [&] {
            return ecuio_session_test_talk_result(s.get(), &out->offer,
                                                  &out->end, &out->nrc) == 1;
        },
        limit);
}

/* A level's key request by the simulated ECU's made-up transform */
Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

/* A frame as a raw listener read it */
struct BusFrame {
    uint32_t id = 0;
    uint8_t data[8] = {};
    uint8_t len = 0;
};

/* Every classic frame waiting on a raw socket, in the bus's order */
std::vector<BusFrame> drain(const vcan_bus::Fd& bus)
{
    std::vector<BusFrame> out;
    can_frame f{};
    while (vcan_bus::read_for(bus.get(), &f, sizeof f, 0) ==
           static_cast<long>(sizeof f)) {
        BusFrame b;
        b.id = f.can_id & CAN_EFF_MASK;
        b.len = f.len;
        std::memcpy(b.data, f.data, sizeof b.data);
        out.push_back(b);
    }
    return out;
}

/* Where the first frame on id whose first byte is pci stands, -1 for none */
int index_of(const std::vector<BusFrame>& frames, uint32_t id, uint8_t pci,
             size_t from = 0)
{
    for (size_t i = from; i < frames.size(); ++i) {
        if (frames[i].id == id && frames[i].data[0] == pci) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void put(const vcan_bus::Fd& bus, uint32_t id, const Bytes& data)
{
    can_frame f{};
    f.can_id = vcan_bus::can_id(id);
    f.len = 8;
    std::memcpy(f.data, data.data(), data.size() < 8 ? data.size() : 8);
    ASSERT_EQ(::write(bus.get(), &f, sizeof f), static_cast<ssize_t>(sizeof f));
}

/* Requesting sessions on vcan created while it lives start with the
 * rate unproven, as on a real controller, with this quiet time */
class Unproven
{
public:
    explicit Unproven(int64_t quiet_us)
    {
        ecuio_socketcan_set_vcan_unproven(quiet_us);
    }
    ~Unproven() { ecuio_socketcan_set_vcan_unproven(0); }
    Unproven(const Unproven&) = delete;
    Unproven& operator=(const Unproven&) = delete;
};

/* One module, no session to enter and nothing asked but its rotation */
const char* const kOneModule =
    "?profile=uds-generic&did=D200:u16&identify=0&diagnose=0";

class SocketCanTalkTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string why;
        if (!vcan_bus::available(&why)) {
            const char* required = std::getenv("ECUIO_VCAN_REQUIRED");
            if (required != nullptr && std::string(required) == "1") {
                FAIL() << "ECUIO_VCAN_REQUIRED=1, but " << why;
            }
            GTEST_SKIP() << why;
        }
        clean();
    }
    void TearDown() override { clean(); }

    static void clean()
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
    }

    /* The whole unlock through a session: the seed of the 16-byte level,
     * its key (18 bytes, so a First Frame, the ECU's Flow Control and two
     * Consecutive Frames), then the gated DID */
    static void unlock_and_read(Session& s, const vcan_bus::Fd& bus)
    {
        Ended e;
        ASSERT_TRUE(talk(s, {0x27, 0x03}, 2, false, &e));
        ASSERT_EQ(e.offer, kTaken);
        ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        const ecuio_sim_security_level* l = ecuio_sim_security_find(0x03);
        Bytes want = {0x67, 0x03};
        want.insert(want.end(), l->seed, l->seed + l->bytes);
        ASSERT_TRUE(wait_for(
            [&] { return s.replies.last({0x67, 0x03}) == want; }, 2000ms))
            << "the seed, 18 bytes, reached the session's callbacks";
        (void)drain(bus);

        const Bytes key = key_for(0x03);
        ASSERT_EQ(key.size(), 18u);
        ASSERT_TRUE(talk(s, key, 2, true, &e));
        ASSERT_EQ(e.offer, kTaken);
        EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        /* On the bus: the First Frame, the ECU's Flow Control, then the
         * two Consecutive Frames, with nothing of the session's between */
        const std::vector<BusFrame> frames = drain(bus);
        const int ff = index_of(frames, vcan_bus::kRequest, 0x10);
        ASSERT_GE(ff, 0);
        EXPECT_EQ(frames[ff].data[1], 18);
        EXPECT_EQ(frames[ff].data[2], 0x27);
        EXPECT_EQ(frames[ff].data[3], 0x04);
        const int fc = index_of(frames, vcan_bus::kReply, 0x30, ff);
        const int cf1 = index_of(frames, vcan_bus::kRequest, 0x21, ff);
        const int cf2 = index_of(frames, vcan_bus::kRequest, 0x22, ff);
        ASSERT_GT(fc, ff) << "the ECU's Flow Control";
        ASSERT_GT(cf1, fc) << "no Consecutive Frame before it";
        ASSERT_GT(cf2, cf1);
        for (int i = ff + 1; i < cf2; ++i) {
            if (frames[i].id == vcan_bus::kRequest) {
                EXPECT_TRUE(i == cf1)
                    << "another request went between: frame " << i << " starts "
                    << int{frames[i].data[0]};
            }
        }
        Bytes sent(frames[ff].data + 2, frames[ff].data + 8);
        sent.insert(sent.end(), frames[cf1].data + 1, frames[cf1].data + 8);
        sent.insert(sent.end(), frames[cf2].data + 1, frames[cf2].data + 6);
        EXPECT_EQ(sent, key);

        ASSERT_TRUE(talk(s, {0x22, 0xD2, 0x10}, 3, false, &e));
        EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        EXPECT_TRUE(wait_for(
            [&] {
                return s.replies.last({0x62, 0xD2, 0x10}) ==
                       Bytes{0x62, 0xD2, 0x10, 0x4B, 0x2D};
            },
            2000ms));
        int32_t state = 0;
        uint32_t count = 0;
        ASSERT_EQ(ecuio_session_get_error_state(s.get(), &state, &count),
                  ECUIO_OK);
        EXPECT_EQ(state, ECUIO_OK);
    }
};

} // namespace

TEST_F(SocketCanTalkTest, AnEighteenByteKeyCrossesToTheKernelsIsoTp)
{
    vcan_bus::Ecu ecu; /* the fakes' ECU behind the kernel's can-isotp */
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s;
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    unlock_and_read(s, bus);
}

TEST_F(SocketCanTalkTest, AnEighteenByteKeyUnlocksTheSimulatedEcu)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create_with("mock://sim?values=canned",
                                    vcan_bus::interface_name(), 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    {
        Session s;
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        unlock_and_read(s, bus);
    }
    ecuio_sim_destroy(sim);
}

TEST_F(SocketCanTalkTest, ALongRequestWithNoFlowControlIsUnsentAndTheNextGoes)
{
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s; /* no ECU on the bus */
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const Bytes key = key_for(0x03);
    Ended e;
    /* first: on a silent bus the lists ahead of a talk wait 100 ms an item */
    ASSERT_TRUE(talk(s, key, 2, true, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent) << "N_Bs passed with no Flow Control";
    std::this_thread::sleep_for(300ms);
    std::vector<BusFrame> frames = drain(bus);
    size_t first_frames = 0;
    for (const BusFrame& f : frames) {
        if (f.id == vcan_bus::kRequest) {
            first_frames += f.data[0] == 0x10 ? 1 : 0;
            EXPECT_NE(f.data[0] & 0xF0, 0x20) << "no Consecutive Frame went";
        }
    }
    EXPECT_EQ(first_frames, 1u) << "and the request is not sent again";
    /* a request of one frame: sent, and unanswered */
    ASSERT_TRUE(talk(s, {0x3E, 0x00}, 2, true, &e));
    EXPECT_EQ(e.end, kUnanswered);
    /* An ECU comes: the sender is free, and a long request goes whole */
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(talk(s, {0x3E, 0x00}, 2, false, &e));
    EXPECT_EQ(e.end, kAnswered);
    ASSERT_TRUE(talk(s, key, 2, false, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent) << "it reached the ECU, whatever it answered";
    EXPECT_NE(e.end, kUnanswered);
}

TEST_F(SocketCanTalkTest, AnOverflowFlowControlEndsTheSendUnsent)
{
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s;
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const Bytes key = key_for(0x03);
    ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, key.data(), 18, 2, 1),
              ECUIO_OK);
    /* The case is the ECU: it answers the First Frame with an overflow */
    std::vector<BusFrame> seen;
    ASSERT_TRUE(wait_for(
        [&] {
            for (const BusFrame& f : drain(bus)) {
                seen.push_back(f);
            }
            return index_of(seen, vcan_bus::kRequest, 0x10) >= 0;
        },
        3000ms));
    put(bus, vcan_bus::kReply, {0x32, 0x00, 0x00});
    Ended e;
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_test_talk_result(s.get(), &e.offer, &e.end,
                                                  &e.nrc) == 1;
        },
        3000ms));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent);
    std::this_thread::sleep_for(200ms);
    for (const BusFrame& f : drain(bus)) {
        if (f.id == vcan_bus::kRequest) {
            EXPECT_NE(f.data[0] & 0xF0, 0x20) << "no Consecutive Frame went";
            EXPECT_NE(f.data[0], 0x10) << "nor the request again";
        }
    }
}

/* A slow ECU: its Flow Control asks 127 ms between Consecutive Frames,
 * so the key takes longer to send than a reply may take to come. The
 * session sends nothing else meanwhile, to this module or the other, the
 * wait for the reply runs from the last frame, and the other module's
 * Flow Control (an overflow) isn't the send's. */
TEST_F(SocketCanTalkTest, ASlowEcusPaceIsKeptWithNothingElseSentMeanwhile)
{
    constexpr uint32_t kModuleRequest = 0x7E1;
    constexpr uint32_t kModuleReply = 0x7E9;
    const Bytes key = key_for(0x03);
    /* A run whose Flow Control came after N_Bs (the case was starved)
     * tests nothing and runs again, three at most */
    for (int run = 0; run < 3; ++run) {
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        Session s("?profile=uds-generic&did=D200:u16,7E9:D201:u16"
                  "&identify=0&diagnose=0");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, key.data(), 18, 2, 1),
                  ECUIO_OK);
        /* The case is both ECUs */
        std::vector<BusFrame> seen;
        const auto see = [&](uint8_t pci, std::chrono::milliseconds limit) {
            return wait_for(
                [&] {
                    for (const BusFrame& f : drain(bus)) {
                        seen.push_back(f);
                    }
                    return index_of(seen, vcan_bus::kRequest, pci) >= 0;
                },
                limit);
        };
        ASSERT_TRUE(see(0x10, 3000ms));
        put(bus, kModuleReply, {0x32, 0x00, 0x00});
        put(bus, vcan_bus::kReply, {0x30, 0x00, 0x7F});
        const bool whole = see(0x22, 1000ms);
        if (whole) {
            put(bus, vcan_bus::kReply, {0x02, 0x67, 0x04});
        }
        Ended e;
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_test_talk_result(s.get(), &e.offer, &e.end,
                                                      &e.nrc) == 1;
            },
            3000ms));
        if (e.end == kUnsent && run < 2) {
            continue;
        }
        ASSERT_TRUE(whole) << "end " << e.end;
        EXPECT_EQ(e.end, kAnswered) << "its wait ran from the last frame";
        const int ff = index_of(seen, vcan_bus::kRequest, 0x10);
        const int cf1 = index_of(seen, vcan_bus::kRequest, 0x21, ff);
        const int cf2 = index_of(seen, vcan_bus::kRequest, 0x22, ff);
        ASSERT_GT(cf1, ff);
        ASSERT_GT(cf2, cf1);
        for (int i = ff + 1; i < cf2; ++i) {
            if (seen[i].id == vcan_bus::kRequest ||
                seen[i].id == kModuleRequest) {
                EXPECT_TRUE(i == cf1)
                    << "a request went between: frame " << i << " to "
                    << seen[i].id << " starts " << int{seen[i].data[0]};
            }
        }
        Bytes sent(seen[ff].data + 2, seen[ff].data + 8);
        sent.insert(sent.end(), seen[cf1].data + 1, seen[cf1].data + 8);
        sent.insert(sent.end(), seen[cf2].data + 1, seen[cf2].data + 6);
        EXPECT_EQ(sent, key);
        return;
    }
}

/* An ECU that refuses a long request straight after its First Frame: the
 * talk settles refused and the send ends with it. Its Flow Control,
 * should one still come, brings no Consecutive Frame: that frame would
 * put a one-shot out with no talk behind it. */
TEST_F(SocketCanTalkTest, ARefusalAfterTheFirstFrameEndsTheSend)
{
    const Bytes key = key_for(0x03);
    /* A run whose refusal came after N_Bs (the case was starved) tests
     * nothing and runs again, three at most */
    for (int run = 0; run < 3; ++run) {
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        Session s(kOneModule);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, key.data(), 18, 2, 1),
                  ECUIO_OK);
        /* The case is the ECU */
        std::vector<BusFrame> seen;
        const auto see = [&](const std::function<bool()>& done) {
            return wait_for(
                [&] {
                    for (const BusFrame& f : drain(bus)) {
                        seen.push_back(f);
                    }
                    return done();
                },
                3000ms);
        };
        ASSERT_TRUE(
            see([&] { return index_of(seen, vcan_bus::kRequest, 0x10) >= 0; }));
        put(bus, vcan_bus::kReply, {0x03, 0x7F, 0x27, 0x24});
        put(bus, vcan_bus::kReply, {0x30, 0x00, 0x00});
        Ended e;
        const auto ended = [&] {
            return wait_for(
                [&] {
                    return ecuio_session_test_talk_result(s.get(), &e.offer,
                                                          &e.end, &e.nrc) == 1;
                },
                3000ms);
        };
        ASSERT_TRUE(ended());
        if (e.end == kUnsent && run < 2) {
            continue;
        }
        EXPECT_EQ(e.offer, kTaken);
        ASSERT_EQ(e.end, kRefused);
        EXPECT_EQ(e.nrc, 0x24);
        /* The rotation goes on, with nothing more of the request */
        const auto rotation = [&] {
            size_t n = 0;
            for (const BusFrame& f : seen) {
                n += f.id == vcan_bus::kRequest && f.data[0] == 0x03 &&
                             f.data[1] == 0x22
                         ? 1
                         : 0;
            }
            return n;
        };
        const size_t before = rotation();
        ASSERT_TRUE(see([&] { return rotation() >= before + 2; }))
            << "the next requests go";
        for (const BusFrame& f : seen) {
            if (f.id == vcan_bus::kRequest) {
                EXPECT_NE(f.data[0] & 0xF0, 0x20)
                    << "a Consecutive Frame of a request already answered";
            }
        }
        /* and the sender is free: a long request starts again */
        seen.clear();
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, key.data(), 18, 2, 1),
                  ECUIO_OK);
        EXPECT_TRUE(
            see([&] { return index_of(seen, vcan_bus::kRequest, 0x10) >= 0; }));
        ASSERT_TRUE(ended());
        EXPECT_EQ(e.offer, kTaken);
        EXPECT_EQ(e.end, kUnsent) << "no Flow Control this time";
        return;
    }
}

/* On a link not yet proven, the ECU's Flow Control is an ECU answering
 * at this rate: the request's Consecutive Frames follow it at once, where
 * an echo alone would have them wait the quiet time out, past any ECU's
 * wait for the next frame. */
TEST_F(SocketCanTalkTest, AFlowControlProvesTheRateSoTheSendGoesOn)
{
    Unproven unproven(2000000);
    const Bytes key = key_for(0x03);
    for (int run = 0; run < 3; ++run) { /* a starved run again, as above */
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        /* handed over before the start: the session's first request */
        Session s(kOneModule, false);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, key.data(), 18, 2, 1),
                  ECUIO_OK);
        ASSERT_EQ(s.start(), ECUIO_OK);
        std::vector<BusFrame> seen;
        const auto see = [&](uint8_t pci, std::chrono::milliseconds limit) {
            return wait_for(
                [&] {
                    for (const BusFrame& f : drain(bus)) {
                        seen.push_back(f);
                    }
                    return index_of(seen, vcan_bus::kRequest, pci) >= 0;
                },
                limit);
        };
        ASSERT_TRUE(see(0x10, 3000ms));
        ASSERT_EQ(seen.size(), 1u) << "nothing went before the First Frame";
        put(bus, vcan_bus::kReply, {0x30, 0x00, 0x00});
        const bool whole = see(0x22, 1000ms); /* half the quiet time */
        if (whole) {
            put(bus, vcan_bus::kReply, {0x02, 0x67, 0x04});
        }
        Ended e;
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_test_talk_result(s.get(), &e.offer, &e.end,
                                                      &e.nrc) == 1;
            },
            4000ms));
        if (e.end == kUnsent && run < 2) {
            continue;
        }
        ASSERT_TRUE(whole) << "the send waited for the quiet time: end "
                           << e.end;
        EXPECT_EQ(e.end, kAnswered);
        return;
    }
}

/* A session of one module keeps the ECU's pace: the worker wakes for the
 * send's next frame, where the engine alone would wake it 100 ms on. The
 * request's eight Consecutive Frames at STmin 20 ms: the median gap no
 * shorter, and well under the engine's wait. */
TEST_F(SocketCanTalkTest, ConsecutiveFramesKeepAnEcusPaceInASessionOfOneModule)
{
    using clock = std::chrono::steady_clock;
    Bytes read = {0x22}; /* 29 DIDs: 59 bytes */
    for (uint8_t k = 0; k < 29; ++k) {
        read.push_back(0xD3);
        read.push_back(k);
    }
    for (int run = 0; run < 3; ++run) { /* a starved run again, as above */
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        Session s(kOneModule);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 0, read.data(),
                                          static_cast<uint32_t>(read.size()), 3,
                                          1),
                  ECUIO_OK);
        /* The case is the ECU: each of the request's Consecutive Frames
         * with the time it was read */
        std::vector<clock::time_point> at;
        bool flowed = false;
        const clock::time_point limit = clock::now() + 5000ms;
        while (at.size() < 8 && clock::now() < limit) {
            can_frame f{};
            if (vcan_bus::read_for(bus.get(), &f, sizeof f, 50) !=
                    static_cast<long>(sizeof f) ||
                (f.can_id & CAN_EFF_MASK) != vcan_bus::kRequest) {
                continue;
            }
            if (f.data[0] == 0x10 && !flowed) {
                EXPECT_EQ(f.data[1], 59);
                put(bus, vcan_bus::kReply, {0x30, 0x00, 0x14});
                flowed = true;
            } else if ((f.data[0] & 0xF0) == 0x20) {
                at.push_back(clock::now());
            }
        }
        if (at.size() == 8) {
            put(bus, vcan_bus::kReply, {0x05, 0x62, 0xD3, 0x00, 0x12, 0x34});
        }
        Ended e;
        ASSERT_TRUE(wait_for(
            [&] {
                return ecuio_session_test_talk_result(s.get(), &e.offer, &e.end,
                                                      &e.nrc) == 1;
            },
            3000ms));
        if (e.end == kUnsent && run < 2) {
            continue;
        }
        ASSERT_EQ(at.size(), 8u) << "end " << e.end;
        EXPECT_EQ(e.end, kAnswered);
        std::vector<int64_t> gaps;
        for (size_t i = 1; i < at.size(); ++i) {
            gaps.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(at[i] -
                                                                      at[i - 1])
                    .count());
        }
        std::sort(gaps.begin(), gaps.end());
        const int64_t median = gaps[gaps.size() / 2];
        EXPECT_GE(median, 19000) << "the ECU asked 20 ms between frames";
        EXPECT_LT(median, 80000) << "the worker woke for each frame";
        return;
    }
}

TEST_F(SocketCanTalkTest, NothingOutsideTheCeilingReachesTheBus)
{
    vcan_bus::Ecu ecu;
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s;
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    Ended e;
    for (const Bytes& pdu :
         {Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x10, 0x02}, Bytes{0x31, 0x01},
          Bytes{0x27, 0x81}, Bytes{0x14, 0xFF, 0xFF, 0xFF}}) {
        ASSERT_TRUE(talk(s, pdu, 1, true, &e)) << int{pdu[0]};
        EXPECT_EQ(e.offer, kRefusedOffer) << int{pdu[0]};
        EXPECT_EQ(e.end, 0);
    }
    ASSERT_TRUE(talk(s, {0x3E, 0x00}, 2, false, &e));
    EXPECT_EQ(e.end, kAnswered) << "the session is none the worse";
    for (const BusFrame& f : drain(bus)) {
        if (f.id != vcan_bus::kRequest || (f.data[0] & 0xF0) == 0x30) {
            continue; /* the ECU's frames, and the session's Flow Control */
        }
        const uint8_t* req = (f.data[0] & 0xF0) == 0 ? f.data + 1 : f.data + 2;
        EXPECT_NE(req[0], 0x2E);
        EXPECT_NE(req[0], 0x31);
        EXPECT_NE(req[0], 0x14);
        EXPECT_NE(req[0], 0x27);
        EXPECT_FALSE(req[0] == 0x10 && req[1] != 0x03 && req[1] != 0x01)
            << "10 " << int{req[1]} << " went out";
    }
    /* What the seam itself refuses */
    const Bytes long_one(61, 0x22);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, long_one.data(), 61, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, long_one.data(), 0, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 9, long_one.data(), 3, 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a channel the session lacks";
    EXPECT_EQ(ecuio_session_test_talk(nullptr, 0, long_one.data(), 3, 1, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, nullptr, 3, 1, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk_result(s.get(), &e.offer, &e.end, &e.nrc),
              0)
        << "there is none";
    Session listening("?profile=raw");
    ASSERT_EQ(listening.status(), ECUIO_OK) << listening.detail();
    EXPECT_EQ(
        ecuio_session_test_talk(listening.get(), 0, long_one.data(), 3, 1, 0),
        ECUIO_ERR_INVALID_ARGUMENT)
        << "a session that asks nothing sends nothing";
}
