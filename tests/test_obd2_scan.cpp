// File/layer: tests/test_obd2_scan.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

namespace {

ecuio_reply answer_did(ecuio_request_engine* e, uint32_t c, uint16_t did,
                       int64_t now)
{
    const uint8_t reply[5] = {0x62, static_cast<uint8_t>(did >> 8),
                              static_cast<uint8_t>(did), 0x12, 0x34};
    return ecuio_engine_on_frame(e, c, reply, sizeof(reply), now, nullptr);
}

} // namespace

/* A scan takes turns with the rotation: the rotation's request, then a
 * scanned DID, and so on. A DID the ECU answers joins the rotation at
 * once; one it refuses is counted and never asked again. */
TEST(RequestEngineTest, ScanTakesTurnsWithTheRotationAndKeepsWhatItFinds) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did_c[3] = {0x22, 0x1E, 0x1C};
    const uint8_t did_d[3] = {0x22, 0x1E, 0x1D};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_c, 3), ecuio_engine_add_result::added);
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_d, 3), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));
    EXPECT_EQ(e.scan_count, 2u);
    EXPECT_EQ(e.chans[0].scan_pending, 2u);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0) << "something is due";

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation first */
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    ASSERT_EQ(e.count, 3u) << "the found DID joined the rotation";
    EXPECT_EQ(e.requests[2].bytes[2], 0x20u);
    EXPECT_EQ(e.chans[0].requests, 3u);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 1u);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 1, now));   /* 1E1D: the rotation's turn */
    EXPECT_EQ(answer_did(&e, 0, 0x1E1D, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_REFUSED);
    EXPECT_EQ(e.scan[1].nrc, 0x31u);
    EXPECT_EQ(e.count, 3u) << "a refused DID enters nothing";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 2, now));   /* 1E20, now rotating */
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the scan is over";

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_total, 2u);
    EXPECT_EQ(s.scan_done, 2u);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 1u);
    EXPECT_EQ(s.scan_polled, 1u);
    EXPECT_EQ(s.requests, 3u);
    EXPECT_EQ(s.dropped, 0u);
}

/* ?scan=first: while DIDs are left to scan, the scan sends every turn and
 * the rotation, the listed DID and the ones found, waits; once the last is
 * asked, the rotation runs. */
TEST(RequestEngineTest, ScanFirstAsksEveryDidBeforeTheRotation) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    e.scan_first = true;
    const uint8_t did_c[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did_c, 3), ecuio_engine_add_result::added);
    for (uint16_t d = 0x1E20; d < 0x1E24; ++d) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, d));
    }

    int64_t now = 0;
    for (uint16_t d = 0x1E20; d < 0x1E24; ++d) {
        ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, d, now))
            << "the scan's turn, back to back";
        if (d == 0x1E21) {
            EXPECT_EQ(answer_did(&e, 0, d, now + 4 * kMs), ecuio_reply::positive);
        } else {
            EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
                      ecuio_reply::negative);
        }
        now += ECUIO_ENGINE_GAP_US;
    }
    EXPECT_EQ(e.chans[0].requests, 2u) << "the found DID joined the rotation";
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the scan is over";
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 1, now));

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 3u);
}

/* A channel with nothing to rotate scans every turn. A DID nobody answers
 * is unanswered, not refused: the scan ends with it open, and a rescan
 * (the driver's, when the link recovers) asks it again. */
TEST(RequestEngineTest, ScanAloneRunsEveryTurnAndRescanAsksTheUnansweredAgain) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), ECUIO_ENGINE_WAIT_US);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNANSWERED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), INT64_MAX);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, 0u) << "unanswered isn't complete";
    EXPECT_EQ(s.scan_total, 2u);
    EXPECT_EQ(s.scan_done, 0u);

    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].scan_pending, 2u);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    EXPECT_EQ(e.count, 1u);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "the found DID's turn";
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_done, 2u);
}

/* A long reply can come after its wait: a positive reply naming a
 * scanned DID left unanswered answers it, whether another request is
 * out or none, so it's found and polled and the scan completes. The
 * request out when it lands keeps waiting for its own. */
TEST(RequestEngineTest, ALateReplyStillAnswersItsScannedDid) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E21));

    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNANSWERED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E21, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E20, now + 4 * kMs), ecuio_reply::none)
        << "1E21's request still waits";
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 1u);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_ONCE);

    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_UNASKED)
        << "the round's last: asked once more, since 1E20 answered";
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(answer_did(&e, 0, 0x1E21, now + 4 * kMs), ecuio_reply::none);
    EXPECT_EQ(e.scan[1].outcome, ECUIO_SCAN_FOUND);

    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
    EXPECT_EQ(s.scan_found, 2u);
    EXPECT_EQ(s.scan_polled, 2u);
    EXPECT_EQ(e.count, 2u);
}

/* A found DID is counted even when the table is full; it just isn't
 * polled, which the report says. */
TEST(RequestEngineTest, ScanFoundWithAFullTableIsCountedButNotPolled) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    /* DIDs 0x2000 on, one apart, so the table fills whatever its size */
    for (uint32_t i = 0; i < ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        const uint16_t did = static_cast<uint16_t>(0x2000 + i);
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(did >> 8),
                                static_cast<uint8_t>(did)};
        ASSERT_EQ(ecuio_engine_add(&e, 0, req, 3),
                  ecuio_engine_add_result::added);
    }
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(answer_did(&e, 0, 0x2000, now + kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E1C, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + kMs), ecuio_reply::positive);
    EXPECT_EQ(e.count, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 0u);
    ecuio_channel_support_t s;
    ecuio_engine_support(&e, 0, &s);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_polled, 0u);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE);
}

/* A found DID the table already polls adds no second request: one DID,
 * one row, as uds-generic's set has one def for it */
TEST(RequestEngineTest, ScanFindingAPolledDidAddsNoSecondRequest)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t req[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, req, 3), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + kMs), ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E1C, now));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, now + kMs), ecuio_reply::positive);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_FOUND);
    EXPECT_EQ(e.scan[0].polled, 1u) << "its request is the one polled";
    EXPECT_EQ(e.count, 1u);
}

/* A scan request the driver couldn't send stays due, as a bitmap does. */
TEST(RequestEngineTest, UnsentScanRequestStaysDue) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    const int32_t r = ecuio_engine_due(&e, 0, 0);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ecuio_engine_sent(&e, 0, r, false, 0);
    EXPECT_EQ(e.chans[0].outstanding, ECUIO_ENGINE_NONE);
    EXPECT_EQ(e.scan[0].outcome, ECUIO_SCAN_UNASKED);
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E1C, ECUIO_ENGINE_GAP_US));
    EXPECT_EQ(answer_did(&e, 0, 0x1E1C, ECUIO_ENGINE_GAP_US + kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.count, 1u);
}

TEST(RequestEngineTest, ScanAddKeepsADidOncePerChannelAndStopsAtItsLimit) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C));
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E1C)) << "kept once";
    EXPECT_EQ(e.scan_count, 1u);
    EXPECT_TRUE(ecuio_engine_scan_add(&e, 1, 0x1E1C)) << "another ECU";
    EXPECT_EQ(e.scan_count, 2u);
    EXPECT_FALSE(ecuio_engine_scan_add(&e, 2, 0x1E1C)) << "past the channels";
    for (uint32_t d = 0; e.scan_count < ECUIO_ENGINE_SCAN_MAX; ++d) {
        ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, static_cast<uint16_t>(0x2000 + d)));
    }
    EXPECT_FALSE(ecuio_engine_scan_add(&e, 0, 0x3000)) << "the scan is full";
    EXPECT_EQ(e.chans[0].scan_pending, ECUIO_ENGINE_SCAN_MAX - 1);
}

/* ---------- uds-generic: the ?did= grammar and the set it builds ---------- */

namespace {

/* Splits a query (the text after the URI's '?') and builds the
 * uds-generic set from its did= keys, as create does. */
ecuio_status_t build_uds(const std::string& query, ecuio_uds_session* out)
{
    ecuio_uri_query q;
    if (ecuio_uri::split(query.c_str(), &q) != ecuio_uri::split_result::ok) {
        return ECUIO_ERR_BAD_URI;
    }
    return ecuio_uds_build(q, out);
}

/* The requests the set's polled head derives on its own channels. */
uint32_t derived_requests(const ecuio_uds_session& u)
{
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    for (size_t i = 0; i < u.channels.size(); ++i) {
        rx[i] = u.channels[i].rx_id;
    }
    ecuio_request_engine e;
    ecuio_engine_init(&e, static_cast<uint32_t>(u.channels.size()));
    const ecuio_signal_set_t head = ecuio_uds_polled_set(u);
    EXPECT_TRUE(ecuio_engine_derive(&e, &head, rx,
                                    static_cast<uint32_t>(u.channels.size()),
                                    nullptr));
    return e.count;
}

} // namespace

TEST(UdsGenericTest, SingleDidsArePolledOnTheDefaultEcu) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1C,F190:text", &u), ECUIO_OK);
    ASSERT_EQ(u.set.count, 2u);
    EXPECT_EQ(u.polled, 2u);
    EXPECT_TRUE(u.scan.empty());
    ASSERT_EQ(u.set.channel_count, 1u);
    EXPECT_EQ(u.set.channels[0].rx_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(u.set.channels[0].tx_id, ECUIO_OBD2_TESTER_ARB_ID);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(u.set.layout_version, ECUIO_SIGNAL_SET_LAYOUT);

    const ecuio_signal_def_t& a = u.set.defs[0];
    EXPECT_EQ(a.id, ECUIO_UDS_GENERIC_SIG_ID_BASE);
    EXPECT_EQ(a.message_id, ECUIO_OBD2_ECU_ARB_ID);
    EXPECT_EQ(a.mux_offset, 0u);
    EXPECT_EQ(a.mux_length, 3u);
    EXPECT_EQ(a.mux_key, 0x621E1Cu);
    EXPECT_EQ(a.kind, ECUIO_VALUE_HEX) << "hex by default";
    EXPECT_EQ(a.byte_order, ECUIO_BYTE_ORDER_BE);
    EXPECT_EQ(a.start_byte, 3u);
    EXPECT_EQ(a.bit_length, 0u);
    EXPECT_EQ(a.byte_length, 0u) << "to the end of the reply";
    EXPECT_EQ(a.scale, 1.0);
    EXPECT_EQ(a.offset, 0.0);
    EXPECT_STREQ(a.name, "DID 1E1C");
    EXPECT_STREQ(a.unit, "");
    EXPECT_EQ(a.decimal_places, 0u);
    const ecuio_signal_def_t& b = u.set.defs[1];
    EXPECT_EQ(b.id, ECUIO_UDS_GENERIC_SIG_ID_BASE + 1);
    EXPECT_EQ(b.kind, ECUIO_VALUE_TEXT);
    EXPECT_EQ(b.mux_key, 0x62F190u);
    EXPECT_STREQ(b.name, "DID F190");

    EXPECT_EQ(derived_requests(u), 2u);

    /* The fake's reply to 1E1C, as the bridge decodes it */
    const uint8_t reply[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    char text[16] = "";
    ASSERT_EQ(ecuio_decode_text(&a, reply, sizeof(reply), text, sizeof(text)),
              ECUIO_DECODE_OK);
    EXPECT_STREQ(text, "12 34");
}

TEST(UdsGenericTest, TypesSetTheKindAndWidth) {
    const struct {
        const char* name;
        uint8_t kind;
        uint8_t bits;
    } kCases[] = {
        {"hex", ECUIO_VALUE_HEX, 0},   {"text", ECUIO_VALUE_TEXT, 0},
        {"u8", ECUIO_VALUE_UINT, 8},   {"u16", ECUIO_VALUE_UINT, 16},
        {"u24", ECUIO_VALUE_UINT, 24}, {"u32", ECUIO_VALUE_UINT, 32},
        {"i8", ECUIO_VALUE_INT, 8},    {"i16", ECUIO_VALUE_INT, 16},
        {"i24", ECUIO_VALUE_INT, 24},  {"i32", ECUIO_VALUE_INT, 32},
    };
    for (const auto& c : kCases) {
        ecuio_uds_session u;
        ASSERT_EQ(build_uds(std::string("did=1e1c:") + c.name, &u), ECUIO_OK)
            << c.name;   /* hex digits in either case */
        ASSERT_EQ(u.set.count, 1u) << c.name;
        const ecuio_signal_def_t& d = u.set.defs[0];
        EXPECT_EQ(d.kind, c.kind) << c.name;
        EXPECT_EQ(d.bit_length, c.bits) << c.name;
        EXPECT_EQ(d.byte_order, ECUIO_BYTE_ORDER_BE) << c.name;
        EXPECT_EQ(d.start_byte, 3u) << c.name;
        EXPECT_EQ(d.mux_key, 0x621E1Cu) << c.name;
        EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK)
            << c.name;
    }

    /* Big-endian, raw: 12 34 reads 0x1234 as u16, and FF reads -1 as i8 */
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1C:u16,1E1D:i8", &u), ECUIO_OK);
    const uint8_t word[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&u.set.defs[0], word, sizeof(word), &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, 0x1234);
    const uint8_t byte[4] = {0x62, 0x1E, 0x1D, 0xFF};
    ASSERT_EQ(ecuio_decode_signal(&u.set.defs[1], byte, sizeof(byte), &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, -1.0);
}

TEST(UdsGenericTest, EcuPrefixNamesTheRespondingEcu) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=7E9:1E1C,1E1C,18DAF110:F190:u8", &u), ECUIO_OK);
    ASSERT_EQ(u.set.channel_count, 3u) << "in order of first appearance";
    EXPECT_EQ(u.set.channels[0].rx_id, 0x7E9u);
    EXPECT_EQ(u.set.channels[0].tx_id, 0x7E1u);
    EXPECT_EQ(u.set.channels[1].rx_id, 0x7E8u);
    EXPECT_EQ(u.set.channels[1].tx_id, 0x7E0u);
    EXPECT_EQ(u.set.channels[2].rx_id, 0x18DAF110u);
    EXPECT_EQ(u.set.channels[2].tx_id, 0x18DA10F1u);
    ASSERT_EQ(u.set.count, 3u) << "the same DID on two ECUs is two defs";
    EXPECT_EQ(u.set.defs[0].message_id, 0x7E9u);
    EXPECT_STREQ(u.set.defs[0].name, "0x7E9 DID 1E1C");
    EXPECT_STREQ(u.set.defs[1].name, "DID 1E1C");
    EXPECT_STREQ(u.set.defs[2].name, "0x18DAF110 DID F190");
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(derived_requests(u), 3u);

    uint32_t tx = 0;
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x7E8u, &tx));
    EXPECT_EQ(tx, 0x7E0u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x9u, &tx));
    EXPECT_EQ(tx, 1u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x7FFu, &tx));
    EXPECT_EQ(tx, 0x7F7u);
    EXPECT_TRUE(ecuio_uds_tx_for_rx(0x18DAF110u, &tx));
    EXPECT_EQ(tx, 0x18DA10F1u);
    /* 8 would ask on 0, which no channel may carry (the fuzzer's find) */
    for (uint32_t rx : {0x8u, 0x7u, 0x0u, 0x800u, 0x1FFFFFFFu, 0x18DBF110u}) {
        EXPECT_FALSE(ecuio_uds_tx_for_rx(rx, &tx)) << std::hex << rx;
    }
}

/* An ECU prefix takes a range as it takes a DID, with or without a type:
 * the range's last token holds a '-', which no type name does. */
TEST(UdsGenericTest, EcuPrefixTakesARange) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=7E9:1E00-1E01,18DAF110:F190-F190:text", &u),
              ECUIO_OK);
    EXPECT_EQ(u.polled, 0u);
    ASSERT_EQ(u.scan.size(), 3u);
    EXPECT_EQ(u.scan[0].did, 0x1E00u);
    EXPECT_EQ(u.scan[0].rx_id, 0x7E9u);
    EXPECT_EQ(u.scan[1].did, 0x1E01u);
    EXPECT_EQ(u.scan[1].rx_id, 0x7E9u);
    EXPECT_EQ(u.scan[2].did, 0xF190u);
    EXPECT_EQ(u.scan[2].rx_id, 0x18DAF110u);
    ASSERT_EQ(u.set.count, 3u);
    EXPECT_STREQ(u.set.defs[0].name, "0x7E9 DID 1E00");
    EXPECT_EQ(u.set.defs[0].kind, ECUIO_VALUE_HEX) << "a range's default type";
    EXPECT_EQ(u.set.defs[2].kind, ECUIO_VALUE_TEXT);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
}

/* A range is scanned, a lone DID polled, and a DID listed both ways is
 * polled. Polled defs come first, each group in the order its DIDs first
 * appeared, so the transport can derive the rotation from the head of
 * the set. */
TEST(UdsGenericTest, RangesAreScannedAndRepeatsKeptOnce) {
    ecuio_uds_session u;
    ASSERT_EQ(build_uds("did=1E1A-1E1D,1E1C,1E1C&did=1E1B,1E1F-1E1F", &u),
              ECUIO_OK);
    ASSERT_EQ(u.set.count, 5u);
    EXPECT_EQ(u.polled, 2u);
    EXPECT_STREQ(u.set.defs[0].name, "DID 1E1B");
    EXPECT_STREQ(u.set.defs[1].name, "DID 1E1C");
    EXPECT_STREQ(u.set.defs[2].name, "DID 1E1A");
    EXPECT_STREQ(u.set.defs[3].name, "DID 1E1D");
    EXPECT_STREQ(u.set.defs[4].name, "DID 1E1F") << "a range of one scans";
    for (uint32_t i = 0; i < u.set.count; ++i) {
        EXPECT_EQ(u.set.defs[i].id, ECUIO_UDS_GENERIC_SIG_ID_BASE + i) << i;
    }
    ASSERT_EQ(u.scan.size(), 3u);
    EXPECT_EQ(u.scan[0].did, 0x1E1Au);
    EXPECT_EQ(u.scan[0].rx_id, 0x7E8u);
    EXPECT_EQ(u.scan[1].did, 0x1E1Du);
    EXPECT_EQ(u.scan[2].did, 0x1E1Fu);
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
    EXPECT_EQ(derived_requests(u), 2u) << "the scanned DIDs enrol nothing";
}

TEST(UdsGenericTest, RefusalsNameTheirCause) {
    const char* const shape =
        "?did= needs a DID like 1E1C, an ECU and a type as in 7E9:1E1C:u16, "
        "or a range like 1E00-1EFF, not '";
    const char* const types =
        "?did= needs a type of hex, text, u8, u16, u24, u32, i8, i16, i24 or "
        "i32, not '";
    const char* const ecu =
        "?did= needs an ECU response ID of 9 to 7FF, or 29-bit 18DAxxyy, not '";
    /* One ECU more than a set holds, each on a reply ID of its own */
    std::string crowd = "did=";
    for (unsigned i = 0; i <= ECUIO_MAX_CHANNELS; ++i) {
        char item[16];
        std::snprintf(item, sizeof item, "%s%03X:1E1C", i > 0 ? "," : "",
                      0x700u + i);
        crowd += item;
    }
    const struct {
        const char* query;
        std::string detail;
    } kCases[] = {
        {"did=", "?did= needs at least one DID"},
        {"did=1E1C,", std::string(shape) + "'"},
        {"did=,1E1C", std::string(shape) + "'"},
        {"did=0x1E1C", std::string(shape) + "0x1E1C'"},
        {"did=12345", std::string(shape) + "12345'"},
        {"did=1E1C-", std::string(shape) + "1E1C-'"},
        {"did=7E8:1E1C:u8:x", std::string(shape) + "7E8:1E1C:u8:x'"},
        {"did=7E8:1E1C:u8:x:y", std::string(shape) + "7E8:1E1C:u8:x:y'"},
        {"did=7E8:1E1C-:u8", std::string(shape) + "7E8:1E1C-:u8'"},
        {"did=1E1C:", std::string(types) + "'"},
        {"did=1E1C:u12", std::string(types) + "u12'"},
        {"did=1E1C:U8", std::string(types) + "U8'"},
        {"did=7E8:1E1C:7E9", std::string(types) + "7E9'"},
        {"did=7E8:7E9:1E1C", std::string(types) + "1E1C'"},
        {"did=7E9:1E1C-", std::string(shape) + "7E9:1E1C-'"},
        {"did=7E9:1E1C-1E1G", std::string(shape) + "7E9:1E1C-1E1G'"},
        {"did=1EFF-1E00",
         "?did= needs a range whose end isn't before its start, not '1EFF-1E00'"},
        {"did=7:1E1C", std::string(ecu) + "7'"},
        {"did=7:1E00-1E01", std::string(ecu) + "7'"},
        {"did=8:1E1C", std::string(ecu) + "8'"},
        {"did=:1E1C", std::string(ecu) + "'"},
        {"did=18DB1234:1E1C", std::string(ecu) + "18DB1234'"},
        {crowd.c_str(),
         "?did= names 33 ECUs, more than the 32 channels a session has"},
        {"did=0000-0400", "?did= scans 1025 DIDs, more than the 1024 a session scans"},
        {"did=0000-FFFF", "?did= scans 65536 DIDs, more than the 1024 a session scans"},
        {"did=0000-03FF,0400-0400",
         "?did= scans 1025 DIDs, more than the 1024 a session scans"},
    };
    for (const auto& c : kCases) {
        ecuio_uds_session u;
        EXPECT_EQ(build_uds(c.query, &u), ECUIO_ERR_BAD_URI) << c.query;
        EXPECT_EQ(std::string(ecuio_last_error_detail()), c.detail) << c.query;
    }

    /* One DID to poll past the engine's table */
    std::string many = "did=";
    for (unsigned i = 0; i <= ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        char one[8];
        snprintf(one, sizeof(one), "%s%04X", (i == 0) ? "" : ",", 0x1000 + i);
        many += one;
    }
    ecuio_uds_session u;
    EXPECT_EQ(build_uds(many, &u), ECUIO_ERR_BAD_URI);
    char detail[96];
    snprintf(detail, sizeof detail,
             "?did= lists %u DIDs to poll, more than the %u a session "
             "asks for; scan a range instead",
             ECUIO_PROFILE_MAX_REQUESTS + 1u, ECUIO_PROFILE_MAX_REQUESTS);
    EXPECT_STREQ(ecuio_last_error_detail(), detail);

    /* At the limits, and other keys beside did=, it builds */
    many.erase(many.rfind(','));
    ASSERT_EQ(build_uds(many + "&profile=uds-generic", &u), ECUIO_OK);
    EXPECT_EQ(u.polled, ECUIO_PROFILE_MAX_REQUESTS);
    ASSERT_EQ(build_uds("did=0000-03FF", &u), ECUIO_OK);
    EXPECT_EQ(u.scan.size(), size_t{ECUIO_ENGINE_SCAN_MAX});
    EXPECT_EQ(ecuio_validate_signal_set(&u.set, nullptr), ECUIO_VALIDATE_OK);
}

TEST(UdsGenericTest, TemplateSetIsEmptyAndValid) {
    const ecuio_signal_set_t* t = ecuio_uds_generic_template_set();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->count, 0u);
    EXPECT_EQ(t->channel_count, 0u);
    EXPECT_EQ(ecuio_validate_signal_set(t, nullptr), ECUIO_VALIDATE_OK);
    const ecuio_signal_set_t* found = nullptr;
    ASSERT_EQ(ecuio_profile_lookup(ECUIO_UDS_GENERIC_NAME, &found), ECUIO_OK);
    EXPECT_EQ(found, t) << "the registry binds the name to it";
    EXPECT_EQ(ecuio_profile_register(ECUIO_UDS_GENERIC_NAME, t, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT) << "a built-in";
}
