// File/layer: tests/test_socketcan.cpp | SocketCAN listening's rules (ADR-0032)
// Testing: UNIT: SocketCanTest (test_socketcan)

#include "test_socketcan_support.h"

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
    ASSERT_EQ(socketcan_channel_filters(rx, tx, 3, UINT32_MAX, f), 5u)
        << "0x7E0 once";
    EXPECT_EQ(f[0].id, 0x7E8u);
    EXPECT_EQ(f[0].mask, SOCKETCAN_SFF_MASK | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[1].id, 0x7E0u);
    EXPECT_EQ(f[2].id, 0x7E9u);
    EXPECT_EQ(f[3].id, 0x18DAF110u | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[3].mask, SOCKETCAN_EFF_MASK | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[4].id, 0x18DA10F1u | SOCKETCAN_EFF_FLAG);
}

/* ADR-0065 step 5: a closed module's IDs leave the list, an ID two
 * modules share stays while either hears, and none open is an empty list
 * (which passes nothing) */
TEST(SocketCanTest, TheChannelFiltersListTheOpenModulesOnly)
{
    const uint32_t rx[] = {0x7E8, 0x7E9, 0x18DAF110};
    const uint32_t tx[] = {0x7E0, 0x7E0, 0x18DA10F1};
    socketcan_filter f[6];
    ASSERT_EQ(socketcan_channel_filters(rx, tx, 3, 0x5u, f), 4u);
    EXPECT_EQ(f[0].id, 0x7E8u);
    EXPECT_EQ(f[1].id, 0x7E0u);
    EXPECT_EQ(f[2].id, 0x18DAF110u | SOCKETCAN_EFF_FLAG);
    EXPECT_EQ(f[3].id, 0x18DA10F1u | SOCKETCAN_EFF_FLAG);
    ASSERT_EQ(socketcan_channel_filters(rx, tx, 3, 0x2u, f), 2u);
    EXPECT_EQ(f[0].id, 0x7E9u);
    EXPECT_EQ(f[1].id, 0x7E0u) << "shared with a closed module, still heard";
    EXPECT_EQ(socketcan_channel_filters(rx, tx, 3, 0u, f), 0u);
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
