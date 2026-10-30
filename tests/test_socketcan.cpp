// File/layer: tests/test_socketcan.cpp | SocketCAN listening's rules (ADR-0032)
// Testing: UNIT, Qt-free, every platform: socketcan_wire.h (the interface
//          name, ?filter=, a link from canned netlink messages and the
//          verdict on it, a kernel frame to an ecuio_frame_t, the
//          clock), the URI's refusals through ecuio_session_create, and
//          ecuio_can_interfaces' arguments. The transport on a bus is
//          VcanTest's (Linux)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/socketcan.h"
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
};

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
        if (s.ctrlmode) {
            /* struct can_ctrlmode: mask, then flags */
            data = cat({data, attr(SOCKETCAN_IFLA_CAN_CTRLMODE,
                                   u32s({0xFFFFFFFFu, s.modes}))});
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

TEST(SocketCanTest, DiscoveryListsControllersAndVcanOnly)
{
    ecuio_can_interface_t item;
    ASSERT_TRUE(socketcan_interface_from(parsed(LinkSpec{}), &item));
    EXPECT_STREQ(item.name, "can0");
    EXPECT_STREQ(item.kind, "can");
    EXPECT_EQ(item.flags, ECUIO_CAN_UP | ECUIO_CAN_LISTEN_ONLY);
    EXPECT_EQ(item.bitrate, 500000u);
    for (uint32_t r : item.reserved) {
        EXPECT_EQ(r, 0u);
    }

    LinkSpec v;
    v.name = "vcan0";
    v.kind = "vcan";
    v.mtu = 72;
    v.up = false;
    v.bitrate = v.ctrlmode = false;
    ASSERT_TRUE(socketcan_interface_from(parsed(v), &item));
    EXPECT_EQ(item.flags, ECUIO_CAN_FD);
    EXPECT_EQ(item.bitrate, 0u);

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
 * (ADR-0033 §5): the two deltas agree, so the gap stands. A host clock
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
    EXPECT_NE(c.detail.find("takes no query key 'poll'"), std::string::npos)
        << c.detail;
    c = create("socketcan://vcan0?profile=obd2");
    EXPECT_EQ(c.status, ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_NE(c.detail.find("only listens"), std::string::npos) << c.detail;
    c = create("socketcan://vcan0?profile=uds-generic&did=7E0:F190");
    EXPECT_EQ(c.status, ECUIO_ERR_NOT_IMPLEMENTED) << c.detail;
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
