// File/layer: tests/test_vcan_gateway.cpp | test-only, C++17, Qt-free,
//             Linux only
// Testing: HARNESS: SimGatewayBusTest (test_vcan)
//
// The simulated gateway on the bus (sim_bus.cpp's take, route() before
// the module's busy check): the case as the tester through the kernel's
// ISO-TP sockets and a raw listener. A refusal is a Single Frame on the
// module's reply ID; a silent route sends nothing while the gateway
// answers on its own; a 16-byte seed's key, segmented both ways, opens
// the module; the lockout's delay runs on the bus; another node on the
// gateway's reply ID silences the sim; and a requesting socketcan://
// session lists the gated DID refused 33.

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "vcan_bus.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vcan_bus::Fd;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kGw = 0x6F0;
constexpr uint32_t kGwReply = 0x6F8;
constexpr uint32_t kModule = 0x7E1;
constexpr uint32_t kModuleReply = 0x7E9;

const std::string kBase = "mock://sim?modules=2&gateway=6F0:6F8&behind=7E9";
const Bytes kD100 = {0x22, 0xD1, 0x00};
const Bytes kD100Answer = {0x62, 0xD1, 0x00, 0x82};

class Sim
{
public:
    explicit Sim(const std::string& uri)
    {
        status_ =
            ecuio_sim_create(uri.c_str(), vcan_bus::interface_name(), &sim_);
        detail_ = ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim_); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
    ecuio_status_t status() const { return status_; }
    const std::string& detail() const { return detail_; }
    ecuio_sim_stats_t stats() const
    {
        ecuio_sim_stats_t s{};
        EXPECT_EQ(ecuio_sim_get_stats(sim_, &s), ECUIO_OK);
        return s;
    }
    ecuio_sim_gateway_t gateway() const
    {
        ecuio_sim_gateway_t g{};
        EXPECT_EQ(ecuio_sim_get_gateway(sim_, &g), ECUIO_OK);
        return g;
    }

private:
    ecuio_sim_t* sim_ = nullptr;
    ecuio_status_t status_ = ECUIO_ERR_NO_TRANSPORT;
    std::string detail_;
};

/* A whole request on an ISO-TP socket and its whole reply within ms;
 * {} for none */
Bytes ask_on(const Fd& s, const Bytes& req, int ms = 500)
{
    if (::write(s.get(), req.data(), req.size()) !=
        static_cast<ssize_t>(req.size())) {
        ADD_FAILURE() << "the request's write failed: " << std::strerror(errno);
        return {};
    }
    uint8_t buf[4096];
    const long n = vcan_bus::read_for(s.get(), buf, sizeof buf, ms);
    return n > 0 ? Bytes(buf, buf + n) : Bytes{};
}

bool put(int fd, uint32_t id, std::initializer_list<uint8_t> data)
{
    can_frame f{};
    f.can_id = id;
    f.len = 8;
    std::memset(f.data, vcan_bus::kPad, sizeof f.data);
    std::copy_n(data.begin(), std::min<size_t>(data.size(), sizeof f.data),
                f.data);
    return ::write(fd, &f, sizeof f) == static_cast<ssize_t>(sizeof f);
}

/* The frames on id heard within ms */
std::vector<can_frame> frames_on(int fd, uint32_t id, int ms)
{
    std::vector<can_frame> got;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              end - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return got;
        }
        can_frame f{};
        if (vcan_bus::read_for(fd, &f, sizeof f, static_cast<int>(left)) > 0 &&
            (f.can_id & CAN_SFF_MASK) == id) {
            got.push_back(f);
        }
    }
}

/* The made-up transform's key request for a seed reply, as sim.h states
 * it: the seed reversed, each byte XORed with a letter of HAVNE */
Bytes made_up_key(const Bytes& seed_reply)
{
    const size_t n = seed_reply.size() - 2;
    Bytes key = {0x27, static_cast<uint8_t>(seed_reply[1] + 1)};
    const char word[] = "HAVNE";
    for (size_t i = 0; i < n; ++i) {
        key.push_back(static_cast<uint8_t>(seed_reply[2 + n - 1 - i] ^
                                           word[i % 5]));
    }
    return key;
}

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class SimGatewayBusTest : public ::testing::Test
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
    }
};

} // namespace

// The gateway's refusal is one Single Frame on the module's reply ID, as
// a module's own would be: nothing on the wire says the gateway wrote it
TEST_F(SimGatewayBusTest, ARefusalIsASingleFrameOnTheModulesId)
{
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    Sim sim(kBase + "&route=22:refuse");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd module = vcan_bus::open_isotp(kModule, kModuleReply);
    ASSERT_TRUE(module.ok());
    EXPECT_EQ(ask_on(module, kD100), (Bytes{0x7F, 0x22, 0x22}));
    const std::vector<can_frame> on = frames_on(raw.get(), kModuleReply, 100);
    ASSERT_EQ(on.size(), 1u);
    EXPECT_EQ(on[0].data[0], 0x03) << "a Single Frame of 3 bytes";
    EXPECT_EQ(on[0].data[1], 0x7F);
    EXPECT_EQ(on[0].data[3], 0x22);
    EXPECT_EQ(ask_on(module, {0x3E, 0x00}), (Bytes{0x7E, 0x00}))
        << "the module's own answer forwarded";
    EXPECT_EQ(sim.gateway().refused, 1u);
    EXPECT_EQ(sim.gateway().forwarded, 1u);
}

// silent: no frame on the module's reply ID, while the gateway answers
// on its own
TEST_F(SimGatewayBusTest, ASilentRouteSendsNothingWhileTheGatewayAnswers)
{
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    Sim sim(kBase + "&route=22:silent");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    ASSERT_TRUE(put(raw.get(), kModule, {0x03, 0x22, 0xD1, 0x00}));
    EXPECT_TRUE(frames_on(raw.get(), kModuleReply, 200).empty());
    ASSERT_TRUE(put(raw.get(), kGw, {0x02, 0x3E, 0x00}));
    const std::vector<can_frame> on = frames_on(raw.get(), kGwReply, 200);
    ASSERT_EQ(on.size(), 1u);
    EXPECT_EQ(on[0].data[0], 0x02);
    EXPECT_EQ(on[0].data[1], 0x7E);
    EXPECT_EQ(sim.gateway().dropped, 1u);
    EXPECT_EQ(sim.stats().flags, 0u);
}

// The 16-byte level: the seed comes segmented (the tester's Flow
// Control), its 18-byte key goes segmented (the gateway's Flow
// Control), and the gated module answers once unlocked
TEST_F(SimGatewayBusTest, ASixteenByteSeedsKeyOpensTheModule)
{
    Sim sim(kBase + "&sa=key&route=reads:gated");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd gw = vcan_bus::open_isotp(kGw, kGwReply);
    Fd module = vcan_bus::open_isotp(kModule, kModuleReply);
    ASSERT_TRUE(gw.ok() && module.ok());
    EXPECT_EQ(ask_on(module, kD100), (Bytes{0x7F, 0x22, 0x33}));
    EXPECT_EQ(ask_on(gw, {0x10, 0x03}).at(0), 0x50);
    const Bytes seed = ask_on(gw, {0x27, 0x03});
    ASSERT_EQ(seed.size(), 18u);
    EXPECT_EQ(ask_on(gw, made_up_key(seed)), (Bytes{0x67, 0x04}));
    EXPECT_EQ(ask_on(module, kD100), kD100Answer);
    EXPECT_EQ(sim.gateway().unlocked_level, 0x03);
}

// sa_delay on the bus: the third wrong key locks it out, a 27 inside the
// delay is 37 (counted too early), one after it is answered
TEST_F(SimGatewayBusTest, TheLockoutsDelayRunsOnTheBus)
{
    Sim sim(kBase + "&sa=key&sa_delay=200&route=reads:gated");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    Fd gw = vcan_bus::open_isotp(kGw, kGwReply);
    ASSERT_TRUE(gw.ok());
    EXPECT_EQ(ask_on(gw, {0x10, 0x03}).at(0), 0x50);
    for (int k = 0; k < 3; ++k) {
        Bytes key = made_up_key(ask_on(gw, {0x27, 0x01}));
        key.back() ^= 0x01;
        EXPECT_EQ(ask_on(gw, key),
                  (Bytes{0x7F, 0x27, static_cast<uint8_t>(k < 2 ? 0x35 : 0x36)}));
    }
    const auto locked = std::chrono::steady_clock::now();
    EXPECT_EQ(ask_on(gw, {0x27, 0x01}), (Bytes{0x7F, 0x27, 0x37}));
    EXPECT_LT(std::chrono::steady_clock::now() - locked, 150ms)
        << "the 37 must come inside the delay to judge it";
    std::this_thread::sleep_until(locked + 300ms);
    const Bytes seed = ask_on(gw, {0x27, 0x01});
    ASSERT_EQ(seed.size(), 6u) << "answered after the delay";
    EXPECT_EQ(ask_on(gw, made_up_key(seed)), (Bytes{0x67, 0x02}));
    const ecuio_sim_gateway_t g = sim.gateway();
    EXPECT_EQ(g.lockouts, 1u);
    EXPECT_EQ(g.too_early, 1u);
}

// A frame from another node on the gateway's reply ID silences the sim
// for good: a gateway already answers there
TEST_F(SimGatewayBusTest, AnotherNodeOnTheGatewaysReplyIdYields)
{
    Fd raw = vcan_bus::open_raw();
    ASSERT_TRUE(raw.ok());
    Sim sim(kBase);
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    ASSERT_TRUE(put(raw.get(), kGwReply, {0x02, 0x7E, 0x00}));
    EXPECT_TRUE(wait_for(
        [&] { return (sim.stats().flags & ECUIO_SIM_YIELDED) != 0; }, 2000ms));
    ASSERT_TRUE(put(raw.get(), kGw, {0x02, 0x3E, 0x00}));
    EXPECT_TRUE(frames_on(raw.get(), kGwReply, 200).empty());
}

// A requesting socketcan:// session on the module behind a locked gateway
// lists its gated DID refused 33 (a security access drop)
TEST_F(SimGatewayBusTest, ASessionListsTheGatedDidAsRefused)
{
    static ecuio_signal_def_t def{};
    def.id = 9100;
    def.message_id = kModuleReply;
    def.mux_length = 3;
    def.mux_key = 0x62D100u;
    def.kind = ECUIO_VALUE_UINT;
    def.byte_order = ECUIO_BYTE_ORDER_BE;
    def.start_byte = 3;
    def.bit_length = 8;
    def.scale = 1.0;
    std::snprintf(def.name, sizeof def.name, "Behind Gateway D100");
    static ecuio_channel_decl_t channel{kModuleReply, kModule, 0, 0, {}};
    static ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &def;
    set.channel_count = 1;
    set.channels = &channel;
    ASSERT_EQ(ecuio_profile_register("sim-gw-bus-d100", &set, nullptr),
              ECUIO_OK);

    Sim sim(kBase + "&sa=key&route=22:gated");
    ASSERT_EQ(sim.status(), ECUIO_OK) << sim.detail();
    const std::string uri = std::string("socketcan://") +
                            vcan_bus::interface_name() +
                            "?profile=sim-gw-bus-d100";
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_channel_support_t sup{};
    const bool dropped = wait_for(
        [&] {
            return ecuio_session_get_channel_support(s, 0, &sup) == ECUIO_OK &&
                   sup.dropped >= 1;
        },
        5000ms);
    ecuio_session_close(s);
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_profile_unregister("sim-gw-bus-d100"), ECUIO_OK);
    ASSERT_TRUE(dropped);
    EXPECT_EQ(sup.rx_id, kModuleReply);
    EXPECT_EQ(sup.dropped_list[0].request[0], 0x22);
    EXPECT_EQ(sup.dropped_list[0].request[1], 0xD1);
    EXPECT_EQ(sup.dropped_list[0].request[2], 0x00);
    EXPECT_EQ(sup.dropped_list[0].reason, ECUIO_DROP_ACCESS);
    EXPECT_EQ(sup.dropped_list[0].nrc, 0x33);
    EXPECT_GE(sim.gateway().refused, 1u);
}
