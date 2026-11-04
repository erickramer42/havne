// File/layer: tests/test_socketcan.cpp | SocketCAN listening's rules (ADR-0032)
// Testing: UNIT, Qt-free, every platform: socketcan_wire.h (the interface
//          name, ?filter=, a link from canned netlink messages and the
//          verdict on it, a kernel frame to an ecuio_frame_t, the
//          clock), the URI's refusals through ecuio_session_create, and
//          ecuio_can_interfaces' arguments; the simulated ECU's contract
//          (SimTest, ADR-0046: its options, and the link it answers on,
//          a desk bus's controller too). The transport on a bus is
//          VcanTest's (Linux), the simulated ECU on one SimBusTest's

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/socketcan.h"
#include "ecuio/sim.h"
#include "socketcan_wire.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

/* ---- A canned RTM_NEWLINK payload: the ifinfomsg, then attributes ---- */

void put16(std::vector<uint8_t>& b, uint16_t v)
{
    uint8_t x[2];
    std::memcpy(x, &v, 2);
    b.insert(b.end(), x, x + 2);
}
void put32(std::vector<uint8_t>& b, uint32_t v)
{
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
}
void pad4(std::vector<uint8_t>& b)
{
    while (b.size() % 4 != 0) {
        b.push_back(0);
    }
}
std::vector<uint8_t> attr(uint16_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> a;
    put16(a, static_cast<uint16_t>(4 + payload.size()));
    put16(a, type);
    a.insert(a.end(), payload.begin(), payload.end());
    pad4(a);
    return a;
}
std::vector<uint8_t> str(const char* s)
{
    return std::vector<uint8_t>(s, s + std::strlen(s) + 1);
}
std::vector<uint8_t> u32s(std::initializer_list<uint32_t> vs)
{
    std::vector<uint8_t> b;
    for (uint32_t v : vs) {
        put32(b, v);
    }
    return b;
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> b;
    for (const auto& p : parts) {
        b.insert(b.end(), p.begin(), p.end());
    }
    return b;
}

constexpr uint16_t kNested = 0x8000; /* NLA_F_NESTED, which the kernel sets */

struct LinkSpec {
    const char* name = "can0";
    int32_t index = 4;
    bool up = true;
    const char* kind = "can"; /* nullptr: no IFLA_LINKINFO (eth0) */
    uint32_t mtu = 16;
    bool bitrate = true;
    uint32_t rate = 500000;
    bool ctrlmode = true;
    uint32_t modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    bool state = true;
    uint32_t can_state = SOCKETCAN_STATE_ERROR_ACTIVE;
    bool restart = true;
    uint32_t restart_ms = 0;
    bool ext = false; /* IFLA_CAN_CTRLMODE_EXT, a kernel from 6.0 */
    uint32_t supported = 0;
};

/* The Innomaker USB2CAN on the desk bus (BENCH prep #26): gs_usb from
 * Linux 6.0 says which modes its firmware offers, and one-shot isn't
 * among them (WSL's 6.18 kernel, 2026-11-03: loopback, listen-only,
 * cc-len8-dlc) */
LinkSpec usb2can()
{
    LinkSpec s;
    s.modes = 0;
    s.ext = true;
    s.supported = 0x01u /* loopback */ | SOCKETCAN_CTRLMODE_LISTENONLY |
                  0x100u /* cc-len8-dlc */;
    return s;
}

/* A controller a session may send through (ADR-0040 section 2) */
LinkSpec sender()
{
    LinkSpec s;
    s.modes = SOCKETCAN_CTRLMODE_ONE_SHOT;
    return s;
}

std::vector<uint8_t> link_msg(const LinkSpec& s)
{
    std::vector<uint8_t> m;
    m.push_back(0); /* family */
    m.push_back(0); /* pad */
    put16(m, 280);  /* ARPHRD_CAN */
    put32(m, static_cast<uint32_t>(s.index));
    put32(m, s.up ? 0x41u : 0x0u); /* IFF_UP | IFF_RUNNING */
    put32(m, 0);                   /* change */
    auto attrs = cat({attr(SOCKETCAN_IFLA_IFNAME, str(s.name)),
                      attr(SOCKETCAN_IFLA_MTU, u32s({s.mtu}))});
    if (s.kind != nullptr) {
        std::vector<uint8_t> data;
        if (s.bitrate) {
            /* struct can_bittiming: bitrate first, then 7 more fields */
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_BITTIMING,
                                   u32s({s.rate, 875, 125, 6, 7, 2, 1, 4}))});
        }
        if (s.state) {
            data = cat(
                {data, attr(SOCKETCAN_IFLA_CAN_STATE, u32s({s.can_state}))});
        }
        if (s.ctrlmode) {
            /* struct can_ctrlmode: mask, then flags */
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_CTRLMODE,
                                   u32s({0xFFFFFFFFu, s.modes}))});
        }
        if (s.restart) {
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_RESTART_MS,
                                   u32s({s.restart_ms}))});
        }
        if (s.ext) {
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_CTRLMODE_EXT | kNested,
                                   attr(SOCKETCAN_IFLA_CAN_CTRLMODE_SUPPORTED,
                                        u32s({s.supported})))});
        }
        auto info = attr(SOCKETCAN_IFLA_INFO_KIND, str(s.kind));
        if (!data.empty()) {
            info = cat({info, attr(SOCKETCAN_IFLA_INFO_DATA | kNested, data)});
        }
        attrs = cat({attrs, attr(SOCKETCAN_IFLA_LINKINFO | kNested, info)});
    }
    return cat({m, attrs});
}

socketcan_link parsed(const LinkSpec& s)
{
    const auto m = link_msg(s);
    socketcan_link l;
    EXPECT_TRUE(socketcan_parse_link(m.data(), m.size(), &l));
    return l;
}

/* ---- Frames ---- */

ecuio_frame_t converted(uint32_t can_id, std::vector<uint8_t> data,
                        bool fd = false, uint8_t fd_flags = 0,
                        uint8_t len = 0xFF)
{
    socketcan_raw in;
    in.can_id = can_id;
    in.len = len == 0xFF ? static_cast<uint8_t>(data.size()) : len;
    in.fd = fd;
    in.fd_flags = fd_flags;
    data.resize(64);
    in.data = data.data();
    ecuio_frame_t f{};
    f.arbitration_id = 0xDEAD; /* the conversion owns every field */
    f.flags = 0xFFFF;
    socketcan_to_frame(in, &f);
    return f;
}

/* ---- Sessions ---- */

struct Created {
    ecuio_status_t status;
    std::string detail;
};
Created create(const char* uri)
{
    ecuio_session_t* s = nullptr;
    const ecuio_status_t st = ecuio_session_create(uri, &s);
    Created c{st, ecuio_last_error_detail()};
    if (s != nullptr) {
        ecuio_session_destroy(s);
    }
    return c;
}

} // namespace

TEST(SocketCanTest, InterfaceNamesFollowIpLink)
{
    for (const char* ok :
         {"can0", "vcan0", "can-usb.1", "slcan_7", "abcdefghijklmno"}) {
        EXPECT_TRUE(socketcan_name_ok(ok, std::strlen(ok))) << ok;
    }
    for (const char* bad :
         {"", "abcdefghijklmnop", "can 0", "can0/", "can0?", "/dev/can0"}) {
        EXPECT_FALSE(socketcan_name_ok(bad, std::strlen(bad))) << bad;
    }
}

TEST(SocketCanTest, FiltersTakeIdsAndMasksOfTheirWidth)
{
    socketcan_filter f[SOCKETCAN_MAX_FILTERS];
    const char* one = "0x7E8";
    ASSERT_EQ(socketcan_parse_filters(one, std::strlen(one), f), 1u);
    EXPECT_EQ(f[0].id, 0x7E8u);
    EXPECT_EQ(f[0].mask, 0x7FFu | SOCKETCAN_EFF_FLAG);

    const char* list = "2024,0x18DA00F1/0x1FFF00FF,0x700/0x700";
    ASSERT_EQ(socketcan_parse_filters(list, std::strlen(list), f), 3u);
    EXPECT_EQ(f[0].id, 2024u);
    EXPECT_EQ(f[1].id, 0x18DA00F1u | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[1].mask, 0x1FFF00FFu | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[2].mask, 0x700u | SOCKETCAN_EFF_FLAG);

    for (const char* bad : {"", ",", "0x7E8,", ",0x7E8", "0x7E8,,0x7E0", "0x",
                            "7E8", "0x20000000", "0x7E8/0x800", "0x7E8/",
                            "1,2,3,4,5,6,7,8,9", "0x7E8;0x7E0"}) {
        EXPECT_EQ(socketcan_parse_filters(bad, std::strlen(bad), f), 0u) << bad;
    }
    const char* eight = "1,2,3,4,5,6,7,8";
    EXPECT_EQ(socketcan_parse_filters(eight, std::strlen(eight), f), 8u);
}

TEST(SocketCanTest, ALinkIsReadFromItsNetlinkAttributes)
{
    const socketcan_link l = parsed(LinkSpec{});
    EXPECT_STREQ(l.name, "can0");
    EXPECT_EQ(l.index, 4);
    EXPECT_TRUE(l.up);
    EXPECT_STREQ(l.kind, "can");
    EXPECT_EQ(l.mtu, 16u);
    EXPECT_TRUE(l.has_bitrate);
    EXPECT_EQ(l.bitrate, 500000u);
    EXPECT_TRUE(l.listen_only());

    LinkSpec v;
    v.name = "vcan0";
    v.kind = "vcan";
    v.mtu = 72;
    v.bitrate = v.ctrlmode = false;
    const socketcan_link vl = parsed(v);
    EXPECT_TRUE(vl.is_vcan());
    EXPECT_FALSE(vl.has_bitrate);
    EXPECT_FALSE(vl.listen_only());
    EXPECT_EQ(vl.mtu, 72u);

    LinkSpec eth;
    eth.name = "eth0";
    eth.kind = nullptr;
    const socketcan_link el = parsed(eth);
    EXPECT_STREQ(el.kind, "");
    EXPECT_FALSE(el.is_can() || el.is_vcan());
}

TEST(SocketCanTest, AMalformedNetlinkMessageIsRefused)
{
    auto m = link_msg(LinkSpec{});
    socketcan_link l;
    EXPECT_FALSE(
        socketcan_parse_link(m.data(), 15, &l)); /* short of ifinfomsg */
    /* The first attribute claims more bytes than the message holds */
    const uint16_t too_long = 0x7FFF;
    std::memcpy(m.data() + SOCKETCAN_IFINFOMSG_LEN, &too_long, 2);
    EXPECT_FALSE(socketcan_parse_link(m.data(), m.size(), &l));
}

TEST(SocketCanTest, OnlyAListenOnlyControllerOrVcanMayBeListenedTo)
{
    LinkSpec s;
    EXPECT_EQ(socketcan_judge(parsed(s), 0), socketcan_verdict::ok);
    EXPECT_EQ(socketcan_judge(parsed(s), 500000), socketcan_verdict::ok);
    EXPECT_EQ(socketcan_judge(parsed(s), 250000),
              socketcan_verdict::wrong_rate);

    LinkSpec normal;
    normal.modes = 0;
    EXPECT_EQ(socketcan_judge(parsed(normal), 0),
              socketcan_verdict::not_listen_only);
    LinkSpec unknown;
    unknown.ctrlmode = false; /* no mode reported: not known to listen only */
    EXPECT_EQ(socketcan_judge(parsed(unknown), 0),
              socketcan_verdict::not_listen_only);

    LinkSpec down;
    down.up = false;
    EXPECT_EQ(socketcan_judge(parsed(down), 0), socketcan_verdict::down);

    LinkSpec eth;
    eth.kind = nullptr;
    EXPECT_EQ(socketcan_judge(parsed(eth), 0), socketcan_verdict::not_can);
    LinkSpec bond;
    bond.kind = "bond";
    EXPECT_EQ(socketcan_judge(parsed(bond), 0), socketcan_verdict::not_can);

    LinkSpec v;
    v.kind = "vcan";
    v.bitrate = v.ctrlmode = false;
    EXPECT_EQ(socketcan_judge(parsed(v), 0), socketcan_verdict::ok);
    EXPECT_EQ(socketcan_judge(parsed(v), 500000), socketcan_verdict::ok)
        << "vcan has no rate to check";
    v.up = false;
    EXPECT_EQ(socketcan_judge(parsed(v), 0), socketcan_verdict::down);
}

/* A session that sends (ADR-0040 section 2): a controller in normal
 * mode at the rate ?baud= states, that never restarts itself, in one-shot
 * mode while that's the rule, and error-active; vcan always */
TEST(SocketCanTest, OnlyAControllerThatCanStopAtOnceMaySend)
{
    const LinkSpec ok = sender();
    EXPECT_EQ(socketcan_judge_sender(parsed(ok), 500000),
              socketcan_send_verdict::ok);
    {
        const socketcan_link l = parsed(ok);
        EXPECT_TRUE(l.has_state);
        EXPECT_EQ(l.state, SOCKETCAN_STATE_ERROR_ACTIVE);
        EXPECT_TRUE(l.has_restart_ms);
        EXPECT_EQ(l.restart_ms, 0u);
        EXPECT_TRUE(l.one_shot());
    }

    LinkSpec s = ok;
    s.modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::listen_only);
    s = ok;
    s.ctrlmode = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::listen_only)
        << "no mode reported: not known to send";
    EXPECT_EQ(socketcan_judge_sender(parsed(ok), 0),
              socketcan_send_verdict::no_rate);
    EXPECT_EQ(socketcan_judge_sender(parsed(ok), 250000),
              socketcan_send_verdict::wrong_rate);
    s = ok;
    s.restart_ms = 100;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::restarts_itself);
    s = ok;
    s.restart = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::restarts_itself)
        << "no restart-ms reported";
    s = ok;
    s.modes = 0; /* normal mode, as the USB2CAN's firmware has it */
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              SOCKETCAN_SEND_NEEDS_ONE_SHOT
                  ? socketcan_send_verdict::no_one_shot
                  : socketcan_send_verdict::ok);
    for (uint32_t state : {1u, 2u, 3u, 4u}) { /* warning .. stopped */
        s = ok;
        s.can_state = state;
        EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
                  socketcan_send_verdict::not_error_active)
            << state;
    }
    s = ok;
    s.state = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::not_error_active)
        << "no state reported";
    s = ok;
    s.up = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::down);
    s = ok;
    s.kind = nullptr;
    EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
              socketcan_send_verdict::not_can);

    LinkSpec v;
    v.name = "vcan0";
    v.kind = "vcan";
    v.bitrate = false;
    v.ctrlmode = false;
    v.state = false;
    v.restart = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(v), 0), socketcan_send_verdict::ok);
    EXPECT_EQ(socketcan_judge_sender(parsed(v), 250000),
              socketcan_send_verdict::ok)
        << "vcan has no rate";
    v.up = false;
    EXPECT_EQ(socketcan_judge_sender(parsed(v), 0),
              socketcan_send_verdict::down);

    /* the listening judge is unchanged by the new attributes */
    EXPECT_EQ(socketcan_judge(parsed(ok), 500000),
              socketcan_verdict::not_listen_only);
}

/* A kernel from 6.0 says which modes a controller's driver offers
 * (IFLA_CAN_CTRLMODE_EXT); an older one says nothing, and nothing is
 * ruled out */
TEST(SocketCanTest, TheModesADriverOffersAreRead)
{
    const socketcan_link known = parsed(usb2can());
    EXPECT_TRUE(known.has_supported);
    EXPECT_EQ(known.supported, usb2can().supported);
    EXPECT_TRUE(known.lacks(SOCKETCAN_CTRLMODE_ONE_SHOT));
    EXPECT_FALSE(known.lacks(SOCKETCAN_CTRLMODE_LISTENONLY));

    const socketcan_link older = parsed(sender());
    EXPECT_FALSE(older.has_supported);
    EXPECT_FALSE(older.lacks(SOCKETCAN_CTRLMODE_ONE_SHOT));
    EXPECT_FALSE(older.lacks(SOCKETCAN_CTRLMODE_LISTENONLY));

    LinkSpec v;
    v.kind = "vcan";
    v.ext = true; /* a vcan has no modes to lack */
    EXPECT_FALSE(parsed(v).lacks(SOCKETCAN_CTRLMODE_ONE_SHOT));

    LinkSpec cut = usb2can();
    cut.supported = 0;
    auto m = link_msg(cut);
    m.resize(m.size() - 2); /* the nest's last attribute runs past the end */
    socketcan_link l;
    EXPECT_FALSE(socketcan_parse_link(m.data(), m.size(), &l));
}

/* A driver without the mode a session needs is refused before anything
 * else, since no ip link command would help: one-shot for a requesting
 * session (the USB2CAN, down, listen-only, at another rate or restarting
 * itself alike), listen-only for a listening one */
TEST(SocketCanTest, ADriverWithoutTheModeANeedsIsRefusedFirst)
{
    LinkSpec s = usb2can();
    if (SOCKETCAN_SEND_NEEDS_ONE_SHOT) {
        EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
                  socketcan_send_verdict::one_shot_unoffered);
        s.up = false;
        EXPECT_EQ(socketcan_judge_sender(parsed(s), 500000),
                  socketcan_send_verdict::one_shot_unoffered);
        s = usb2can();
        s.modes = SOCKETCAN_CTRLMODE_LISTENONLY;
        EXPECT_EQ(socketcan_judge_sender(parsed(s), 250000),
                  socketcan_send_verdict::one_shot_unoffered);
        s.restart_ms = 100;
        EXPECT_EQ(socketcan_judge_sender(parsed(s), 0),
                  socketcan_send_verdict::one_shot_unoffered);
    }
    s = usb2can();
    s.modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    EXPECT_EQ(socketcan_judge(parsed(s), 500000), socketcan_verdict::ok)
        << "it listens";

    LinkSpec offered = sender();
    offered.ext = true;
    offered.supported =
        SOCKETCAN_CTRLMODE_ONE_SHOT | SOCKETCAN_CTRLMODE_LISTENONLY;
    EXPECT_EQ(socketcan_judge_sender(parsed(offered), 500000),
              socketcan_send_verdict::ok);
    offered.modes = 0;
    EXPECT_EQ(socketcan_judge_sender(parsed(offered), 500000),
              SOCKETCAN_SEND_NEEDS_ONE_SHOT
                  ? socketcan_send_verdict::no_one_shot
                  : socketcan_send_verdict::ok)
        << "offered, not on: ip link turns it on";

    LinkSpec deaf = sender();
    deaf.ext = true;
    deaf.supported = SOCKETCAN_CTRLMODE_ONE_SHOT;
    EXPECT_EQ(socketcan_judge(parsed(deaf), 0),
              socketcan_verdict::listen_only_unoffered);
    deaf.up = false;
    EXPECT_EQ(socketcan_judge(parsed(deaf), 0),
              socketcan_verdict::listen_only_unoffered);
    EXPECT_EQ(socketcan_judge_sender(parsed(sender()), 500000),
              socketcan_send_verdict::ok);
}

/* Every refusal's words name only the modes the controller's driver
 * offers: a driver without one-shot is told it can only listen, never
 * to turn one-shot on; one with it is told how, without "where its driver
 * offers it"; an older kernel's gets the command with that caveat */
TEST(SocketCanTest, ARefusalNamesOnlyTheModesTheDriverOffers)
{
    char why[512];
    const auto send_text = [&](const LinkSpec& s, uint32_t baud) {
        const socketcan_link l = parsed(s);
        socketcan_send_refusal(l, socketcan_judge_sender(l, baud), baud, why,
                               sizeof why);
        return std::string(why);
    };
    const auto listen_text = [&](const LinkSpec& s, uint32_t baud) {
        const socketcan_link l = parsed(s);
        socketcan_refusal(l, socketcan_judge(l, baud), baud, why, sizeof why);
        return std::string(why);
    };

    LinkSpec s = usb2can();
    for (int variant = 0; variant < 4; ++variant) {
        s = usb2can();
        s.up = variant != 1;
        s.modes = variant == 2 ? SOCKETCAN_CTRLMODE_LISTENONLY : 0u;
        s.restart_ms = variant == 3 ? 100u : 0u;
        const std::string t = send_text(s, 500000);
        EXPECT_EQ(t, "can0's driver offers no one-shot mode, so a frame that "
                     "fails would be sent again by the controller: this "
                     "profile sends requests, so nothing was sent; through "
                     "can0, use a profile that only listens, with can0 up "
                     "listen-only")
            << variant;
        EXPECT_EQ(t.find("one-shot on"), std::string::npos) << variant;
    }
    s = usb2can();
    s.supported = 0x10u; /* berr reporting alone: it can't listen either */
    EXPECT_EQ(send_text(s, 500000),
              "can0's driver offers no one-shot mode, so a frame that fails "
              "would be sent again by the controller: this profile sends "
              "requests, so nothing was sent");

    LinkSpec offered = sender();
    offered.modes = 0;
    offered.ext = true;
    offered.supported = SOCKETCAN_CTRLMODE_ONE_SHOT;
    EXPECT_EQ(send_text(offered, 500000),
              "can0 isn't in one-shot mode, so a frame that fails would be "
              "sent again by the controller: sudo ip link set can0 down, then "
              "sudo ip link set can0 up type can bitrate 500000 restart-ms 0 "
              "one-shot on");
    offered.ext = false; /* a kernel before 6.0 */
    EXPECT_EQ(send_text(offered, 500000),
              "can0 isn't in one-shot mode, so a frame that fails would be "
              "sent again by the controller: sudo ip link set can0 down, then "
              "sudo ip link set can0 up type can bitrate 500000 restart-ms 0 "
              "one-shot on (where its driver offers it)");

    /* ip link keeps a mode it isn't told to drop: going from listening
     * to asking says listen-only off */
    LinkSpec listening;
    EXPECT_EQ(send_text(listening, 500000),
              "can0 is listen-only, and this profile sends requests: sudo ip "
              "link set can0 down, then sudo ip link set can0 up type can "
              "bitrate 500000 listen-only off restart-ms 0 one-shot on");

    LinkSpec deaf = sender();
    deaf.ext = true;
    deaf.supported = SOCKETCAN_CTRLMODE_ONE_SHOT;
    const std::string t = listen_text(deaf, 0);
    EXPECT_EQ(t, "can0's driver offers no listen-only mode, so a session "
                 "listening through it could transmit on the bus: listen "
                 "through an adapter whose driver offers one");
    EXPECT_EQ(t.find("listen-only on"), std::string::npos);
    LinkSpec normal;
    normal.modes = 0;
    EXPECT_NE(listen_text(normal, 0).find("listen-only on"), std::string::npos)
        << "nothing said: the command";
    EXPECT_EQ(listen_text(LinkSpec{}, 500000), "") << "ok says nothing";
    EXPECT_EQ(send_text(sender(), 500000), "");
}

/* What discovery and havne-cli devices print: ip link's words */
TEST(SocketCanTest, StatesAndModesHaveIpLinksNames)
{
    const char* const states[] = {
        "",        "error-active", "error-warning", "error-passive",
        "bus-off", "stopped",      "sleeping"};
    for (uint32_t i = 0; i < 7; ++i) {
        EXPECT_STREQ(ecuio_can_state_name(i), states[i]) << i;
    }
    EXPECT_STREQ(ecuio_can_state_name(7), "");
    EXPECT_STREQ(ecuio_can_state_name(UINT32_MAX), "");
    const char* const modes[] = {
        "loopback",    "listen-only",    "triple-sampling",
        "one-shot",    "berr-reporting", "fd",
        "presume-ack", "fd-non-iso",     "cc-len8-dlc"};
    for (uint32_t bit = 0; bit < 9; ++bit) {
        EXPECT_STREQ(ecuio_can_mode_name(1u << bit), modes[bit]) << bit;
    }
    EXPECT_STREQ(ecuio_can_mode_name(0), "");
    EXPECT_STREQ(ecuio_can_mode_name(0x200), "") << "unnamed: TDC";
    EXPECT_STREQ(ecuio_can_mode_name(0x0A), "") << "two bits";
}

/* The first request proves the rate (ADR-0040 section 3): its echo, then
 * a reply or N_As with no error frame, proves it (the desk bus's
 * adapter echoed before reporting the missing ACK); before that, an
 * error halts at once and three requests unseen halt too; once proven,
 * nothing here changes */
TEST(SocketCanTest, AnEchoThenAReplyOrQuietProvesTheRateAndAnErrorHalts)
{
    constexpr int64_t quiet = SOCKETCAN_PROOF_QUIET_US;
    LinkSpec c = sender();
    socketcan_proof p = socketcan_proof_for(parsed(c));
    EXPECT_FALSE(p.proven);
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::unechoed, 0),
              socketcan_proof_step::send_again);
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::echoed, 1000),
              socketcan_proof_step::confirming);
    EXPECT_FALSE(p.proven) << "an echo alone proves nothing yet";
    EXPECT_EQ(socketcan_proof_settle(p, 1000 + quiet, false),
              socketcan_proof_step::carry_on)
        << "still within the quiet time";
    EXPECT_EQ(socketcan_proof_settle(p, 1000 + quiet + 1, false),
              socketcan_proof_step::proven);
    EXPECT_TRUE(p.proven);
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::error, 2000000),
              socketcan_proof_step::carry_on)
        << "a proven rate: the link health's";
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::unechoed, 2000000),
              socketcan_proof_step::carry_on)
        << "a proven rate: a failed write";
    EXPECT_FALSE(p.halted);

    /* a reply proves it at once */
    p = socketcan_proof_for(parsed(c));
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::echoed, 0),
              socketcan_proof_step::confirming);
    EXPECT_EQ(socketcan_proof_settle(p, 10, true),
              socketcan_proof_step::proven);

    /* the desk bus's adapter: the echo, then the missing ACK 1.5 ms on */
    p = socketcan_proof_for(parsed(c));
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::echoed, 0),
              socketcan_proof_step::confirming);
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::error, 1500),
              socketcan_proof_step::halt);
    EXPECT_TRUE(p.halted);
    EXPECT_EQ(socketcan_proof_settle(p, 1000000, true),
              socketcan_proof_step::carry_on)
        << "halted for the session";
    EXPECT_FALSE(p.proven);

    p = socketcan_proof_for(parsed(c));
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::error, 0),
              socketcan_proof_step::halt);
    EXPECT_TRUE(p.halted);
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::echoed, 10),
              socketcan_proof_step::carry_on)
        << "halted for the session";
    EXPECT_FALSE(p.proven);

    p = socketcan_proof_for(parsed(c));
    for (uint8_t i = 1; i < SOCKETCAN_PROOF_TRIES; ++i) {
        EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::unechoed, i),
                  socketcan_proof_step::send_again)
            << int(i);
    }
    EXPECT_EQ(socketcan_proof_on(p, socketcan_sent::unechoed, 9),
              socketcan_proof_step::halt);

    LinkSpec v;
    v.kind = "vcan";
    EXPECT_TRUE(socketcan_proof_for(parsed(v)).proven) << "vcan has no rate";
}

/* A requesting session's kernel filter: each channel's reply and request
 * IDs, exact, of their width, each once */
TEST(SocketCanTest, ARequestingSessionFiltersItsChannelsIds)
{
    const uint32_t rx[] = {0x7E8, 0x7E9, 0x18DAF110};
    const uint32_t tx[] = {0x7E0, 0x7E0, 0x18DA10F1};
    socketcan_filter f[6];
    ASSERT_EQ(socketcan_channel_filters(rx, tx, 3, f), 5u) << "0x7E0 once";
    EXPECT_EQ(f[0].id, 0x7E8u);
    EXPECT_EQ(f[0].mask, SOCKETCAN_SFF_MASK | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[1].id, 0x7E0u);
    EXPECT_EQ(f[2].id, 0x7E9u);
    EXPECT_EQ(f[3].id, 0x18DAF110u | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[3].mask, SOCKETCAN_EFF_MASK | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[4].id, 0x18DA10F1u | SOCKETCAN_EFF_FLAG);
}

TEST(SocketCanTest, DiscoveryListsControllersAndVcanOnly)
{
    ecuio_can_interface_t item;
    ASSERT_TRUE(socketcan_interface_from(parsed(LinkSpec{}), &item));
    EXPECT_STREQ(item.name, "can0");
    EXPECT_STREQ(item.kind, "can");
    EXPECT_EQ(item.flags, ECUIO_CAN_UP | ECUIO_CAN_LISTEN_ONLY);
    EXPECT_EQ(item.bitrate, 500000u);
    EXPECT_EQ(item.state, ECUIO_CAN_STATE_ERROR_ACTIVE);
    EXPECT_EQ(item.modes, ECUIO_CAN_MODE_LISTEN_ONLY);
    EXPECT_EQ(item.modes_supported, 0u) << "an older kernel says nothing";
    for (uint32_t r : item.reserved) {
        EXPECT_EQ(r, 0u);
    }

    LinkSpec usb = usb2can();
    usb.can_state = 3; /* CAN_STATE_BUS_OFF */
    ASSERT_TRUE(socketcan_interface_from(parsed(usb), &item));
    EXPECT_EQ(item.flags, ECUIO_CAN_UP | ECUIO_CAN_MODES_KNOWN);
    EXPECT_EQ(item.state, ECUIO_CAN_STATE_BUS_OFF);
    EXPECT_EQ(item.modes, 0u) << "normal mode";
    EXPECT_EQ(item.modes_supported, usb2can().supported);
    usb.state = false;
    ASSERT_TRUE(socketcan_interface_from(parsed(usb), &item));
    EXPECT_EQ(item.state, ECUIO_CAN_STATE_UNKNOWN) << "none reported";
    usb.state = true;
    usb.can_state = 6; /* CAN_STATE_MAX */
    ASSERT_TRUE(socketcan_interface_from(parsed(usb), &item));
    EXPECT_EQ(item.state, ECUIO_CAN_STATE_UNKNOWN) << "a state past the list";

    LinkSpec v;
    v.name = "vcan0";
    v.kind = "vcan";
    v.mtu = 72;
    v.up = false;
    v.bitrate = v.ctrlmode = false;
    v.ext = true;
    v.supported = SOCKETCAN_CTRLMODE_ONE_SHOT;
    ASSERT_TRUE(socketcan_interface_from(parsed(v), &item));
    EXPECT_EQ(item.flags, ECUIO_CAN_FD) << "a vcan has no modes to know";
    EXPECT_EQ(item.bitrate, 0u);
    EXPECT_EQ(item.state, ECUIO_CAN_STATE_UNKNOWN);
    EXPECT_EQ(item.modes | item.modes_supported, 0u);

    LinkSpec eth;
    eth.kind = nullptr;
    EXPECT_FALSE(socketcan_interface_from(parsed(eth), &item));
}

TEST(SocketCanTest, InterfacesCheckTheirArguments)
{
    uint32_t count = 99;
    EXPECT_EQ(ecuio_can_interfaces(nullptr, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_can_interfaces(nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_can_interfaces(nullptr, 0, &count), ECUIO_OK);
#if !ECUIO_HAVE_SOCKETCAN
    EXPECT_EQ(count, 0u);
#endif
}

TEST(SocketCanTest, AKernelFrameKeepsItsIdFlagsAndBytes)
{
    ecuio_frame_t f = converted(0x7E8, {0x03, 0x41, 0x0C, 0x1A, 0xF8, 0, 0, 0});
    EXPECT_EQ(f.arbitration_id, 0x7E8u);
    EXPECT_EQ(f.flags, 0u);
    EXPECT_EQ(f.data_length, 8u);
    EXPECT_EQ(f.data[1], 0x41);
    EXPECT_EQ(f.channel_id, 0u);
    EXPECT_EQ(f.reserved, 0u);

    f = converted(0x18DAF110u | SOCKETCAN_EFF_FLAG, {1, 2});
    EXPECT_EQ(f.arbitration_id, 0x18DAF110u);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_EQ(f.data_length, 2u);

    /* A remote frame asks for 8 bytes and carries none */
    f = converted(0x123u | SOCKETCAN_RTR_FLAG, {}, false, 0, 8);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_REMOTE));
    EXPECT_EQ(f.data_length, 0u);

    std::vector<uint8_t> twelve(12, 0xAB);
    f = converted(0x0C0, twelve, true, SOCKETCAN_FD_BRS);
    EXPECT_EQ(f.flags, ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_BITRATE_SWITCH);
    EXPECT_EQ(f.data_length, 12u);
    EXPECT_EQ(f.data[11], 0xAB);

    /* A length past the frame's room is cut to it */
    f = converted(0x100, {1, 2, 3, 4, 5, 6, 7, 8}, false, 0, 15);
    EXPECT_EQ(f.data_length, 8u);
}

TEST(SocketCanTest, AnErrorFrameKeepsItsClassAndDetail)
{
    const uint32_t id = SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_CRTL;
    ecuio_frame_t f = converted(id, {0, 0x04, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(f.arbitration_id, SOCKETCAN_ERR_CRTL);
    EXPECT_EQ(f.flags, static_cast<uint32_t>(ECUIO_FRAME_FLAG_ERROR));
    EXPECT_EQ(f.data_length, 8u);
    EXPECT_EQ(f.data[1], 0x04);
    EXPECT_FALSE(socketcan_error_is_hard(id)) << "error-passive still receives";
    EXPECT_TRUE(
        socketcan_error_is_hard(SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_BUSOFF));
    EXPECT_TRUE(
        socketcan_error_is_hard(SOCKETCAN_ERR_FLAG | SOCKETCAN_ERR_RESTARTED));
    EXPECT_FALSE(socketcan_error_is_hard(SOCKETCAN_ERR_BUSOFF))
        << "a data frame's ID";
}

TEST(SocketCanTest, TheClockTakesTheHostsTimeThenTheKernelsSpacing)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull; /* a wall clock, in ns */
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    /* A batch read at one host time keeps the kernel's 1 ms spacing */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 1000000, 9000), 6000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 2000000, 9000), 7000u);
    /* Read 2.5 s late after a stall: still the kernel's spacing */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 12000000, 2509000), 17000u);
}

/* The kernel's wall clock counts a sleep, and so does the host's
 * (ADR-0033 section 5): the two deltas agree, so the gap stands. A host clock
 * that stopped in the sleep would put the kernel's delta past the read
 * and lose the gap to its own */
TEST(SocketCanTest, ASleepKeepsItsGap)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    /* 100 s asleep, the frame read 5 ms after it arrived */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 100000000000ull, 100010000),
              100005000u);
}

TEST(SocketCanTest, AWallClockStepGivesWayToTheHostsTime)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    socketcan_clock_step(c, k0, 0);
    /* Backward: the host's 10 ms */
    EXPECT_EQ(socketcan_clock_step(c, k0 - 3000000000ull, 10000), 10000u);
    /* An hour forward: the host's 10 ms again */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 3600000000000ull, 20000), 20000u);
    /* And the kernel's spacing from the new reading on */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 3600002000000ull, 30000), 22000u);
    /* A frame with no kernel time takes the host's delta */
    EXPECT_EQ(socketcan_clock_step(c, 0, 40000), 32000u);
}

/* A Flow Control frame's own echo and the Consecutive Frames it brought
 * on, read after an error frame stamped later, across a 200 ms stall: a
 * wall-clock step's host delta would put them 200 ms late, past ISO-TP's
 * N_Cr (SocketCanRequestTest.AReplyOnTimeSurvivesAWorkerHeldUpMidMessage) */
TEST(SocketCanTest, AFrameReadOutOfOrderTakesTheNewestTime)
{
    socketcan_clock c;
    const uint64_t k0 = 1700000000000000000ull;
    EXPECT_EQ(socketcan_clock_step(c, k0, 5000), 5000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 30000000, 36000), 35000u);
    /* Stamped before the frame above, read after the stall */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 2000000, 236000), 35000u);
    EXPECT_EQ(socketcan_clock_step(c, k0 + 12000000, 236010), 35000u);
    /* The spacing runs from the newest stamp, not the one read last */
    EXPECT_EQ(socketcan_clock_step(c, k0 + 40000000, 236020), 45000u);
    EXPECT_EQ(c.host_steps, 0u);
    /* Further back than any reordering: a step, the host's delta */
    EXPECT_EQ(socketcan_clock_step(c, k0 - 200000000, 236030), 45010u);
    EXPECT_EQ(c.host_steps, 1u);
}

TEST(SocketCanTest, TheClockNeverDecreases)
{
    socketcan_clock c;
    uint64_t last = 0;
    const uint64_t steps[][2] = {{1000, 100}, {500, 200},
                                 {0, 300},    {2000, 50},
                                 {2000, 60},  {9000000000000ull, 70},
                                 {3000, 400}};
    for (const auto& s : steps) {
        const uint64_t t = socketcan_clock_step(c, s[0], s[1]);
        EXPECT_GE(t, last);
        last = t;
    }
}

TEST(SocketCanTest, TheUriIsCheckedBeforeAnyInterface)
{
#if ECUIO_HAVE_SOCKETCAN
    Created c = create("socketcan://");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("interface's name"), std::string::npos) << c.detail;
    EXPECT_EQ(create("socketcan://can 0").status, ECUIO_ERR_BAD_URI);
    c = create("socketcan://vcan0?baud=125000");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("500000 or 250000"), std::string::npos) << c.detail;
    c = create("socketcan://vcan0?filter=0x7E8,");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("?filter="), std::string::npos) << c.detail;
    c = create("socketcan://vcan0?poll=0x0C");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("?poll= belongs to a profile that asks an ECU"),
              std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&poll=0x1G");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("a PID from 0x00 to 0xFF"), std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2&filter=0x7E8,0x7E9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI)
        << "a profile that requests takes one receive ID";
    c = create("socketcan://lo?profile=obd2");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("lo isn't a CAN interface"), std::string::npos)
        << c.detail;
    c = create("socketcan://lo?profile=uds-generic&did=7E0:F190");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED) << c.detail;
#else
    const Created c = create("socketcan://can0");
    EXPECT_EQ(c.status, ECUIO_ERR_NO_TRANSPORT);
    EXPECT_NE(c.detail.find("needs Linux"), std::string::npos) << c.detail;
#endif
}

#if ECUIO_HAVE_SOCKETCAN
TEST(SocketCanTest, AnInterfaceThatIsntCanIsRefused)
{
    Created c = create("socketcan://nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no network interface is named nosuchcan9"),
              std::string::npos)
        << c.detail;
    c = create("socketcan://lo"); /* every Linux machine has the loopback */
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("lo isn't a CAN interface"), std::string::npos)
        << c.detail;
}
#endif

/* ---- The simulated ECU's contract (ADR-0046) ---- */

namespace {

struct SimCreated {
    ecuio_status_t status;
    std::string detail;
};

SimCreated sim_create(const char* uri, const char* ifname)
{
    ecuio_sim_t* sim = reinterpret_cast<ecuio_sim_t*>(1);
    SimCreated c{ecuio_sim_create(uri, ifname, &sim),
                 ecuio_last_error_detail()};
    EXPECT_EQ(sim, nullptr) << uri;
    return c;
}

} // namespace

TEST(SimTest, TheArgumentsAndTheUriAreCheckedFirst)
{
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create(nullptr, "vcan0", &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create("mock://sim", nullptr, &sim),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create("mock://sim", "vcan0", nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_sim_stats_t stats;
    EXPECT_EQ(ecuio_sim_get_stats(nullptr, &stats), ECUIO_ERR_NULL_ARGUMENT);
    ecuio_sim_destroy(nullptr);
    // the URI's refusals, on every platform, before any interface
    SimCreated c = sim_create("mock://sim?profile=obd2", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("no query key 'profile'"), std::string::npos)
        << c.detail;
    c = sim_create("socketcan://vcan0", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    c = sim_create("mock://sim?faults=silent:0", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("faults:"), std::string::npos) << c.detail;
    c = sim_create("replay://recordings/run", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
    EXPECT_NE(c.detail.find("absolute"), std::string::npos) << c.detail;
    c = sim_create("mock://sim?loop=1", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_BAD_URI);
#if !ECUIO_HAVE_SOCKETCAN
    c = sim_create("mock://sim", "vcan0");
    EXPECT_EQ(c.status, ECUIO_ERR_NO_TRANSPORT);
    EXPECT_NE(c.detail.find("needs Linux"), std::string::npos) << c.detail;
#endif
}

// The options: NULLs first, then a bit that isn't one, before the URI
TEST(SimTest, AnOptionThatIsntOneIsRefusedBeforeTheUri)
{
    ecuio_sim_t* sim = reinterpret_cast<ecuio_sim_t*>(1);
    EXPECT_EQ(
        ecuio_sim_create_with(nullptr, "vcan0", ECUIO_SIM_ALLOW_DESK_BUS, &sim),
        ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_with("mock://sim", "vcan0", 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_sim_create_with("socketcan://vcan0", "nosuchcan9",
                                    ECUIO_SIM_ALLOW_DESK_BUS << 1, &sim),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sim, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("0x2"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_sim_create_with("socketcan://vcan0", "nosuchcan9",
                                    ECUIO_SIM_ALLOW_DESK_BUS, &sim),
              ECUIO_ERR_BAD_URI)
        << "the option itself is taken";
}

/* vcan always, once up; a real controller only on a desk bus said by the
 * caller, and then only up, able to transmit and error-active: any rate,
 * no one-shot mode needed, restarting itself or not */
TEST(SimTest, ARealControllerAnswersOnlyOnADeskBusReadyToSend)
{
    using V = socketcan_sim_verdict;
    LinkSpec vcan;
    vcan.name = "vcan0";
    vcan.kind = "vcan";
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), false), V::ok);
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), true), V::ok);
    vcan.up = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(vcan), false), V::down);

    LinkSpec eth;
    eth.name = "eth0";
    eth.kind = nullptr;
    EXPECT_EQ(socketcan_judge_sim(parsed(eth), true), V::not_can);

    LinkSpec desk = usb2can(); /* normal mode, no one-shot offered */
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), false), V::real_controller);
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), true), V::ok);
    desk.rate = 250000;
    desk.restart_ms = 100;
    EXPECT_EQ(socketcan_judge_sim(parsed(desk), true), V::ok)
        << "the rate it is up at; a restart does no harm on a desk";

    LinkSpec down = usb2can();
    down.up = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(down), false), V::real_controller)
        << "no desk bus said: refused whatever its state";
    EXPECT_EQ(socketcan_judge_sim(parsed(down), true), V::down);

    LinkSpec listening = usb2can();
    listening.modes = SOCKETCAN_CTRLMODE_LISTENONLY;
    EXPECT_EQ(socketcan_judge_sim(parsed(listening), true), V::listen_only);
    LinkSpec unsaid = usb2can();
    unsaid.ctrlmode = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(unsaid), true), V::listen_only)
        << "no mode reported: not known to transmit";

    LinkSpec passive = usb2can();
    passive.can_state = 2; /* CAN_STATE_ERROR_PASSIVE */
    EXPECT_EQ(socketcan_judge_sim(parsed(passive), true), V::not_error_active);
    LinkSpec stateless = usb2can();
    stateless.state = false;
    EXPECT_EQ(socketcan_judge_sim(parsed(stateless), true),
              V::not_error_active);
}

// Each refusal names the interface and the ip link commands that change it
TEST(SimTest, ADeskBusRefusalSaysWhatToChange)
{
    using V = socketcan_sim_verdict;
    char out[512];
    const auto refusal = [&](const LinkSpec& s, V v) {
        socketcan_sim_refusal(parsed(s), v, out, sizeof out);
        return std::string(out);
    };
    LinkSpec eth;
    eth.name = "eth0";
    eth.kind = nullptr;
    EXPECT_EQ(refusal(eth, V::not_can),
              "eth0 is a non-CAN link, not vcan: the simulated ECU answers "
              "only on a virtual bus, never for a car's own ECU");
    const LinkSpec desk = usb2can();
    std::string r = refusal(desk, V::real_controller);
    EXPECT_NE(r.find("can0 is a CAN controller, not vcan"), std::string::npos)
        << r;
    EXPECT_NE(r.find("a desk bus with no car"), std::string::npos) << r;
    EXPECT_EQ(refusal(desk, V::down),
              "can0 is down: sudo ip link set can0 up type can bitrate "
              "500000 listen-only off");
    LinkSpec vcan;
    vcan.name = "vcan0";
    vcan.kind = "vcan";
    EXPECT_EQ(refusal(vcan, V::down),
              "vcan0 is down (sudo ip link set vcan0 up)");
    EXPECT_EQ(refusal(desk, V::listen_only),
              "can0 is listen-only, so the simulated ECU couldn't answer: "
              "sudo ip link set can0 down, then sudo ip link set can0 up "
              "type can bitrate 500000 listen-only off");
    LinkSpec rateless = usb2can();
    rateless.bitrate = false;
    r = refusal(rateless, V::listen_only);
    EXPECT_NE(r.find("bitrate <the bus's rate> listen-only off"),
              std::string::npos)
        << r;
    LinkSpec passive = usb2can();
    passive.can_state = 2;
    r = refusal(passive, V::not_error_active);
    EXPECT_NE(r.find("can0 is error-passive, so the bus already counts "
                     "errors"),
              std::string::npos)
        << r;
    LinkSpec stateless = usb2can();
    stateless.state = false;
    r = refusal(stateless, V::not_error_active);
    EXPECT_NE(r.find("can0 reports no controller state"), std::string::npos)
        << r;
    EXPECT_EQ(refusal(desk, V::ok), "");
}

#if ECUIO_HAVE_SOCKETCAN
// Only a virtual bus: never a car's, whatever its state
TEST(SimTest, OnlyAVcanInterfaceIsTaken)
{
    SimCreated c = sim_create("mock://sim", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
    c = sim_create("mock://sim", "lo"); /* every Linux machine has one */
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("not vcan"), std::string::npos) << c.detail;
    c = sim_create("mock://sim", "bad name!");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    // A desk bus said changes nothing for a link that isn't CAN
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_with("mock://sim", "lo",
                                    ECUIO_SIM_ALLOW_DESK_BUS, &sim),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(sim, nullptr);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("not vcan"),
              std::string::npos);
}

// A recording is read before the interface is judged: its own refusal
// first, then the interface's
TEST(SimTest, ARecordingIsReadBeforeTheInterface)
{
    SimCreated c = sim_create("replay:///no/such/recording", "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("couldn't open"), std::string::npos) << c.detail;
    const std::string fixture = std::string("replay://") +
                                ECUIO_REPLAY_FIXTURES +
                                "/2026-10-25-engine-start";
    c = sim_create(fixture.c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
    c = sim_create((fixture + "?loop=1").c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED) << "it spans time";
    // The URI's stretch reaches the loader: one past the recording's end
    // is the recording's refusal, one inside it the interface's
    c = sim_create((fixture + "?from=1000").c_str(), "nosuchcan9");
    EXPECT_EQ(c.status, ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(c.detail.find("no frames from 1000 s on"), std::string::npos)
        << c.detail;
    c = sim_create((fixture + "?from=10&to=40").c_str(), "nosuchcan9");
    EXPECT_NE(c.detail.find("no interface nosuchcan9"), std::string::npos)
        << c.detail;
}
#endif
