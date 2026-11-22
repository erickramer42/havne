// File/layer: tests/test_vcan_talk.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanTalkTest (test_vcan)
//
// A talk through a requesting socketcan:// session on the bus (ADR-0066
// section 2.4, core/src/socketcan_send.h), handed over by the test seam
// (ecuio_session_test_talk): a request longer than a frame crosses to the
// kernel's ISO-TP and to the simulated ECU's, a send that can't finish is
// never sent again, and nothing is on the bus between a seed and its key
// (both ECUs strict: any request between spends the seed).

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/sim.h"
#include "fake_ecus.h"
#include "sim_security.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <linux/can/error.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<uint8_t>;

/* The seam's numbers (internal.h) */
constexpr int kPlain = 0;
constexpr int kSeed = 1;
constexpr int kKey = 2;
constexpr int kTaken = 0;
constexpr int kTooLong = 2;
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

/* Hands the session a talk for the engine ECU; kind and window_ms as the
 * seam's */
bool hand(const Session& s, const Bytes& pdu, uint8_t echo, int kind = kPlain,
          uint32_t window_ms = 1000)
{
    return ecuio_session_test_talk(s.get(), 0, pdu.data(),
                                   static_cast<uint32_t>(pdu.size()), echo,
                                   kind, window_ms) == ECUIO_OK;
}

/* Waits for the result of the talk handed over */
bool ended(const Session& s, Ended* out,
           std::chrono::milliseconds limit = 5000ms)
{
    *out = Ended{};
    return wait_for(
        [&] {
            return ecuio_session_test_talk_result(s.get(), &out->offer,
                                                  &out->end, &out->nrc) == 1;
        },
        limit);
}

bool talk(const Session& s, const Bytes& pdu, uint8_t echo, int kind,
          Ended* out, uint32_t window_ms = 1000)
{
    return hand(s, pdu, echo, kind, window_ms) && ended(s, out);
}

/* The link's state and its failure episodes */
int32_t state_of(const Session& s, uint32_t* episodes = nullptr)
{
    int32_t state = 0;
    uint32_t count = 0;
    (void)ecuio_session_get_error_state(s.get(), &state, &count);
    if (episodes != nullptr) {
        *episodes = count;
    }
    return state;
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

/* A request of three frames that is no key: 2C 01 and 16 bytes */
Bytes long_request()
{
    Bytes req = {0x2C, 0x01};
    for (uint8_t i = 0; i < 16; ++i) {
        req.push_back(static_cast<uint8_t>(0xA0 + i));
    }
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

/* The same for a frame on id that starts with head */
int index_starting(const std::vector<BusFrame>& frames, uint32_t id,
                   const Bytes& head, size_t from = 0)
{
    for (size_t i = from; i < frames.size(); ++i) {
        if (frames[i].id == id &&
            std::equal(head.begin(), head.end(), frames[i].data)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/* Adds what waits on the raw socket to seen: a listener's buffer holds a
 * few hundred frames, so a case that waits drains as it goes */
void collect(const vcan_bus::Fd& bus, std::vector<BusFrame>& seen)
{
    for (const BusFrame& f : drain(bus)) {
        seen.push_back(f);
    }
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

/* A plain talk the engine ECU answers in one frame, in any session: a
 * guest's read (core's own 10 03 and 3E 00 are no talk) */
const Bytes kRead = {0x22, 0x1E, 0x1C};

/* One module, no session to enter and nothing asked but its rotation */
const char* const kOneModule =
    "?profile=uds-generic&did=D200:u16&identify=0&diagnose=0";
/* One module with nothing to read, only two DIDs to scan: a talk handed
 * over before the start is the session's first request */
const char* const kScanOnly =
    "?profile=uds-generic&did=D200-D201&identify=0&diagnose=0";
/* The extended session, tester present after half a second with nothing
 * sent */
const char* const kKeepAlive =
    "?profile=uds-generic&did=D200:u16&session=03&keepalive_ms=500"
    "&identify=0&diagnose=0";

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
        fake_ecu::security_strict() = true;
    }
    void TearDown() override { clean(); }

    static void clean()
    {
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
        fake_ecu::security_strict() = false;
    }

    /* The whole unlock through a session, of an ECU that is strict: the
     * seed of the 16-byte level, its key (18 bytes, so a First Frame, the
     * ECU's Flow Control and two Consecutive Frames), then the gated DID */
    static void unlock_and_read(Session& s, const vcan_bus::Fd& bus)
    {
        Ended e;
        ASSERT_TRUE(talk(s, {0x27, 0x03}, 2, kSeed, &e));
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
        ASSERT_TRUE(talk(s, key, 2, kKey, &e));
        ASSERT_EQ(e.offer, kTaken);
        EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        /* On the bus: the First Frame, the ECU's Flow Control, then the
         * two Consecutive Frames, with nothing of the session's between,
         * nor between the seed's reply and the key */
        const std::vector<BusFrame> frames = drain(bus);
        const int ff = index_of(frames, vcan_bus::kRequest, 0x10);
        ASSERT_GE(ff, 0);
        for (int i = 0; i < ff; ++i) {
            EXPECT_NE(frames[i].id, vcan_bus::kRequest)
                << "a request between the seed and its key starts "
                << int{frames[i].data[0]} << " " << int{frames[i].data[1]};
        }
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

        ASSERT_NO_FATAL_FAILURE(read_gated(s));
    }

    /* The gated DID answers: the ECU is unlocked, and the link is well */
    static void read_gated(Session& s)
    {
        Ended e;
        ASSERT_TRUE(talk(s, {0x22, 0xD2, 0x10}, 3, kPlain, &e));
        EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        EXPECT_TRUE(wait_for(
            [&] {
                return s.replies.last({0x62, 0xD2, 0x10}) ==
                       Bytes{0x62, 0xD2, 0x10, 0x4B, 0x2D};
            },
            2000ms));
        uint32_t episodes = 0;
        EXPECT_EQ(state_of(s, &episodes), ECUIO_OK);
        EXPECT_EQ(episodes, 0u);
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
    ecuio_sim_test_set_strict(sim, 1);
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    {
        Session s;
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        unlock_and_read(s, bus);
    }
    ecuio_sim_destroy(sim);
}

/* The key comes 2.5 s after its seed, with tester present due after half
 * a second and the session's read as often as the bus takes it: nothing
 * is on the bus in between, and the strict simulated ECU unlocks. */
TEST_F(SocketCanTalkTest, NothingIsOnTheBusBetweenASeedAndItsKey)
{
    ecuio_sim_t* sim = nullptr;
    ASSERT_EQ(ecuio_sim_create_with("mock://sim?values=canned",
                                    vcan_bus::interface_name(), 0, &sim),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_sim_test_set_strict(sim, 1);
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    {
        Session s(kKeepAlive);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        std::vector<BusFrame> seen;
        Ended e;
        ASSERT_TRUE(talk(s, {0x27, 0x01}, 2, kSeed, &e, 4000));
        ASSERT_EQ(e.offer, kTaken);
        ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        collect(bus, seen);
        std::this_thread::sleep_for(2500ms);
        const Bytes key = key_for(0x01);
        ASSERT_EQ(key.size(), 6u);
        ASSERT_TRUE(talk(s, key, 2, kKey, &e));
        ASSERT_EQ(e.offer, kTaken) << "inside its window";
        EXPECT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
        collect(bus, seen);
        const int seed =
            index_starting(seen, vcan_bus::kRequest, {0x02, 0x27, 0x01});
        ASSERT_GE(seed, 0);
        const int sent = index_starting(seen, vcan_bus::kRequest,
                                        {0x06, 0x27, 0x02}, seed + 1);
        ASSERT_GT(sent, seed);
        for (int i = seed + 1; i < sent; ++i) {
            EXPECT_EQ(seen[i].id, vcan_bus::kReply)
                << "between the seed and its key, to " << seen[i].id << ": "
                << int{seen[i].data[1]} << " " << int{seen[i].data[2]};
        }
        ASSERT_NO_FATAL_FAILURE(read_gated(s));
    }
    ecuio_sim_destroy(sim);
}

/* A key offered after its seed's window is refused at the door, and what
 * the bus carries next is tester present, held back meanwhile. */
TEST_F(SocketCanTalkTest, AKeyAfterItsWindowIsRefusedAndTesterPresentFollows)
{
    vcan_bus::Ecu ecu;
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s(kKeepAlive);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    std::vector<BusFrame> seen;
    Ended e;
    ASSERT_TRUE(talk(s, {0x27, 0x01}, 2, kSeed, &e, 800));
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    collect(bus, seen);
    std::this_thread::sleep_for(1100ms);
    collect(bus, seen);
    ASSERT_TRUE(talk(s, key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer);
    EXPECT_EQ(e.end, 0);
    collect(bus, seen);
    const int seed =
        index_starting(seen, vcan_bus::kRequest, {0x02, 0x27, 0x01});
    ASSERT_GE(seed, 0);
    int next = -1;
    for (size_t i = static_cast<size_t>(seed) + 1; i < seen.size(); ++i) {
        if (seen[i].id == vcan_bus::kRequest) {
            next = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GT(next, seed) << "the window's end lets the session go on";
    EXPECT_EQ(seen[next].data[1], 0x3E)
        << "after the seed: " << int{seen[next].data[1]} << " "
        << int{seen[next].data[2]};
    EXPECT_EQ(index_starting(seen, vcan_bus::kRequest, {0x06, 0x27, 0x02}), -1)
        << "the key reached the bus";
}

/* While the link isn't OK no window stands: a bus-off between a seed and
 * its key, with the ECU gone so nothing brings the link back, and the
 * key is refused. */
TEST_F(SocketCanTalkTest, ABusOffEndsTheWindow)
{
    auto ecu = std::make_unique<vcan_bus::Ecu>();
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s(kKeepAlive);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    Ended e;
    ASSERT_TRUE(talk(s, {0x27, 0x01}, 2, kSeed, &e, 4000));
    ASSERT_EQ(e.end, kAnswered) << "nrc " << int{e.nrc};
    ecu.reset();
    can_frame off{};
    off.can_id = CAN_ERR_FLAG | CAN_ERR_BUSOFF;
    off.len = CAN_ERR_DLC;
    ASSERT_EQ(::write(bus.get(), &off, sizeof off),
              static_cast<ssize_t>(sizeof off));
    ASSERT_TRUE(wait_for(
        [&] { return state_of(s) == ECUIO_ERR_TRANSPORT_HARD_FAILURE; },
        5000ms));
    std::this_thread::sleep_for(150ms); /* the pass that took the frame ends */
    ASSERT_TRUE(talk(s, key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer)
        << "the seed may not have outlived what failed; end " << e.end;
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(
        index_starting(drain(bus), vcan_bus::kRequest, {0x06, 0x27, 0x02}), -1)
        << "the key reached the bus";
}

/* A session whose requests go to the functional address (?txid=0x7DF):
 * attach marks the channel, so it takes no seed and no talk longer than
 * a frame. */
TEST_F(SocketCanTalkTest, AFunctionalChannelTakesNoSeedAndNoLongTalk)
{
    vcan_bus::Ecu ecu;
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s("?profile=uds-generic&did=D200:u16&txid=0x7DF&identify=0"
              "&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    Ended e;
    ASSERT_TRUE(talk(s, {0x27, 0x01}, 2, kSeed, &e));
    EXPECT_EQ(e.offer, kRefusedOffer);
    Bytes read = {0x22, 0xD3, 0x01, 0xD3, 0x02, 0xD3, 0x03, 0xD3};
    ASSERT_TRUE(talk(s, read, 3, kPlain, &e));
    EXPECT_EQ(e.offer, kTooLong) << "8 bytes to every module at once";
    read.pop_back();
    ASSERT_TRUE(talk(s, read, 3, kPlain, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent);
    const std::vector<BusFrame> frames = drain(bus);
    EXPECT_GE(
        index_starting(frames, vcan_bus::kFunctional, {0x07, 0x22, 0xD3, 0x01}),
        0)
        << "the 7 bytes went to the functional address";
    for (const BusFrame& f : frames) {
        if (f.id != vcan_bus::kReply) {
            EXPECT_NE(f.data[1], 0x27) << "a seed went to " << f.id;
            EXPECT_NE(f.data[0] & 0xF0, 0x10) << "a First Frame to " << f.id;
        }
    }
}

TEST_F(SocketCanTalkTest, ALongRequestWithNoFlowControlIsUnsentAndTheNextGoes)
{
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s(kOneModule); /* no ECU on the bus */
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const Bytes request = long_request();
    Ended e;
    ASSERT_TRUE(talk(s, request, 2, kPlain, &e));
    ASSERT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent) << "N_Bs passed with no Flow Control";
    /* To the link that is a request unanswered, one ECU's silence, where
     * a failed write would count an episode */
    uint32_t episodes = 0;
    EXPECT_NE(state_of(s, &episodes), ECUIO_ERR_REQUEST_FAILED);
    EXPECT_EQ(episodes, 0u);
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
    ASSERT_TRUE(talk(s, kRead, 3, kPlain, &e));
    EXPECT_EQ(e.end, kUnanswered);
    /* An ECU comes: the sender is free, and a long request goes whole */
    vcan_bus::Ecu ecu;
    ASSERT_TRUE(talk(s, kRead, 3, kPlain, &e));
    EXPECT_EQ(e.end, kAnswered);
    (void)drain(bus);
    ASSERT_TRUE(talk(s, request, 2, kPlain, &e));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_NE(e.end, kUnsent) << "it reached the ECU, whatever it answered";
    EXPECT_GE(index_of(drain(bus), vcan_bus::kRequest, 0x22), 0)
        << "its last Consecutive Frame";
    EXPECT_TRUE(wait_for([&] { return state_of(s) == ECUIO_OK; }, 2000ms));
    (void)state_of(s, &episodes);
    EXPECT_EQ(episodes, 0u);
}

/* One module silent at a long request, three times over: each ends
 * unsent and counts no failed write, the link stays OK, and the engine
 * ECU is asked on. */
TEST_F(SocketCanTalkTest, NoFlowControlFromOneModuleLeavesTheLinkOk)
{
    vcan_bus::Ecu ecu; /* the engine ECU; nothing answers for 0x7E9 */
    Session s("?profile=uds-generic&did=1E1C:u16,7E9:D201:u16"
              "&identify=0&diagnose=0"); /* a DID it has in any session */
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const Bytes request = long_request();
    for (int i = 0; i < 3; ++i) {
        const unsigned before = ecu.answered();
        Ended e;
        ASSERT_EQ(ecuio_session_test_talk(s.get(), 1, request.data(), 18, 2,
                                          kPlain, 0),
                  ECUIO_OK);
        ASSERT_TRUE(ended(s, &e));
        ASSERT_EQ(e.offer, kTaken);
        EXPECT_EQ(e.end, kUnsent) << i;
        uint32_t episodes = 0;
        EXPECT_EQ(state_of(s, &episodes), ECUIO_OK) << i;
        EXPECT_EQ(episodes, 0u) << i;
        EXPECT_TRUE(wait_for([&] { return ecu.answered() > before; }, 2000ms))
            << "the engine ECU is asked on";
    }
}

TEST_F(SocketCanTalkTest, AnOverflowFlowControlEndsTheSendUnsent)
{
    vcan_bus::Fd bus = vcan_bus::open_raw();
    ASSERT_TRUE(bus.ok());
    Session s(kOneModule);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ASSERT_TRUE(hand(s, long_request(), 2));
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
    ASSERT_TRUE(ended(s, &e, 3000ms));
    EXPECT_EQ(e.offer, kTaken);
    EXPECT_EQ(e.end, kUnsent);
    uint32_t episodes = 0;
    (void)state_of(s, &episodes);
    EXPECT_EQ(episodes, 0u) << "the ECU's answer is no failed write";
    std::this_thread::sleep_for(200ms);
    for (const BusFrame& f : drain(bus)) {
        if (f.id == vcan_bus::kRequest) {
            EXPECT_NE(f.data[0] & 0xF0, 0x20) << "no Consecutive Frame went";
            EXPECT_NE(f.data[0], 0x10) << "nor the request again";
        }
    }
}

/* A slow ECU: its Flow Control asks 127 ms between Consecutive Frames,
 * so the request takes longer to send than a reply may take to come. The
 * session sends nothing else meanwhile, to this module or the other, the
 * wait for the reply runs from the last frame, and the other module's
 * Flow Control (an overflow) isn't the send's. */
TEST_F(SocketCanTalkTest, ASlowEcusPaceIsKeptWithNothingElseSentMeanwhile)
{
    constexpr uint32_t kModuleRequest = 0x7E1;
    constexpr uint32_t kModuleReply = 0x7E9;
    const Bytes request = long_request();
    /* A run whose Flow Control came after N_Bs (the case was starved)
     * tests nothing and runs again, three at most */
    for (int run = 0; run < 3; ++run) {
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        Session s("?profile=uds-generic&did=D200:u16,7E9:D201:u16"
                  "&identify=0&diagnose=0");
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_TRUE(hand(s, request, 2));
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
            put(bus, vcan_bus::kReply, {0x02, 0x6C, 0x01});
        }
        Ended e;
        ASSERT_TRUE(ended(s, &e, 3000ms));
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
        EXPECT_EQ(sent, request);
        return;
    }
}

/* An ECU that refuses a long request straight after its First Frame: the
 * talk settles refused and the send ends with it. Its Flow Control,
 * should one still come, brings no Consecutive Frame: that frame would
 * put a one-shot out with no talk behind it. */
TEST_F(SocketCanTalkTest, ARefusalAfterTheFirstFrameEndsTheSend)
{
    const Bytes request = long_request();
    /* A run whose refusal came after N_Bs (the case was starved) tests
     * nothing and runs again, three at most */
    for (int run = 0; run < 3; ++run) {
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        Session s(kOneModule);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_TRUE(hand(s, request, 2));
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
        put(bus, vcan_bus::kReply, {0x03, 0x7F, 0x2C, 0x22});
        put(bus, vcan_bus::kReply, {0x30, 0x00, 0x00});
        Ended e;
        ASSERT_TRUE(ended(s, &e, 3000ms));
        if (e.end == kUnsent && run < 2) {
            continue;
        }
        EXPECT_EQ(e.offer, kTaken);
        ASSERT_EQ(e.end, kRefused);
        EXPECT_EQ(e.nrc, 0x22);
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
        ASSERT_TRUE(hand(s, request, 2));
        EXPECT_TRUE(
            see([&] { return index_of(seen, vcan_bus::kRequest, 0x10) >= 0; }));
        ASSERT_TRUE(ended(s, &e, 3000ms));
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
    for (int run = 0; run < 3; ++run) { /* a starved run again, as above */
        vcan_bus::Fd bus = vcan_bus::open_raw();
        ASSERT_TRUE(bus.ok());
        /* handed over before the start: the session's first request */
        Session s(kScanOnly, false);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        ASSERT_TRUE(hand(s, long_request(), 2));
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
            put(bus, vcan_bus::kReply, {0x02, 0x6C, 0x01});
        }
        Ended e;
        ASSERT_TRUE(ended(s, &e, 4000ms));
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
        ASSERT_TRUE(hand(s, read, 3));
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
        ASSERT_TRUE(ended(s, &e, 3000ms));
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
    /* A 27 is taken only as a seed or a key: a plain one never */
    for (const Bytes& pdu :
         {Bytes{0x2E, 0xF1, 0x90, 0x00}, Bytes{0x10, 0x02}, Bytes{0x31, 0x01},
          Bytes{0x27, 0x81}, Bytes{0x27, 0x01}, Bytes{0x27, 0x02, 0x00, 0x00},
          Bytes{0x14, 0xFF, 0xFF, 0xFF}}) {
        ASSERT_TRUE(talk(s, pdu, 1, kPlain, &e)) << int{pdu[0]};
        EXPECT_EQ(e.offer, kRefusedOffer) << int{pdu[0]} << " " << int{pdu[1]};
        EXPECT_EQ(e.end, 0);
    }
    ASSERT_TRUE(talk(s, {0x27, 0x02}, 2, kSeed, &e));
    EXPECT_EQ(e.offer, kRefusedOffer) << "a seed's sub-function is odd";
    ASSERT_TRUE(talk(s, {0x27, 0x01}, 2, kSeed, &e, 4001));
    EXPECT_EQ(e.offer, kRefusedOffer) << "a window past the session's own";
    ASSERT_TRUE(talk(s, key_for(0x01), 2, kKey, &e));
    EXPECT_EQ(e.offer, kRefusedOffer) << "a key with no seed before it";
    ASSERT_TRUE(talk(s, kRead, 3, kPlain, &e));
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
    const uint8_t* const p = long_one.data();
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, p, 61, 1, kPlain, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, p, 0, 1, kPlain, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 9, p, 3, 1, kPlain, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a channel the session lacks";
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, p, 3, 1, 3, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a kind the seam has no name for";
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, p, 3, 1, -1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(nullptr, 0, p, 3, 1, kPlain, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk(s.get(), 0, nullptr, 3, 1, kPlain, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_test_talk_result(s.get(), &e.offer, &e.end, &e.nrc),
              0)
        << "there is none";
    Session listening("?profile=raw");
    ASSERT_EQ(listening.status(), ECUIO_OK) << listening.detail();
    EXPECT_EQ(ecuio_session_test_talk(listening.get(), 0, p, 3, 1, kPlain, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a session that asks nothing sends nothing";
}
