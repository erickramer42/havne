// File/layer: tests/test_j2534_stream_requests.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest, J2534StreamTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

TEST_F(J2534AdoptionTest, RequestLoopWritesMode01AndReceivesStrippedEcho)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_count(cap, 3, 100);   /* ~1 s budget for 3 requests */

    const int received = cap.count.load();
    unsigned long wc = 0;
    get_write_count(&wc);

    /* Time-driven producer: assert the floor, not an exact count. */
    EXPECT_GE(received, 3);
    EXPECT_GE(wc, 3u);
    /* Every echo is CAUSED by a write, and the next request waits for
     * the reply, so writes >= received with at most a couple in flight.
     * A runaway write loop blows this delta up. */
    if (wc >= static_cast<unsigned long>(received)) {
        EXPECT_LE(wc - static_cast<unsigned long>(received), 2u);
    } else {
        ADD_FAILURE() << "received more echoes than writes: "
                      << received << " > " << wc;
    }

    /* ---- TX framing: 6 wire bytes, adapter-framed logical request ---- */
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wlen, 6u);                     /* adapter-framed, no PCI */
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xE0u);
    EXPECT_EQ(wbuf[4], 0x01u);               /* Mode 01 */
    EXPECT_EQ(wbuf[5], 0x0Cu);               /* PID */
    EXPECT_NE(wflags & ECUIO_PT_TX_ISO15765_FRAME_PAD, 0u);   /* adapter pads */

    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payloads[0][0], 0x41u);    /* mode echo at offset 0 */
    EXPECT_EQ(cap.payloads[0][1], 0x0Cu);
    EXPECT_EQ(cap.payloads[0][2], 0x1Fu);
    EXPECT_EQ(cap.payloads[0][3], 0x40u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 4u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?poll=0x00 requests PID 0x00, the supported-PIDs bitmap for 0x01-0x20,
 * which "0 = no poll" would make unreachable. The fake answers it with an
 * all-zero bitmap: [41][00][4 bitmap bytes]. */
TEST_F(J2534AdoptionTest, PollZeroRequestsSupportedPidBitmap)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x00",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_count(cap, 1, 100);
    ASSERT_GE(cap.count.load(), 1) << "no reply to the PID 0x00 request";

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[4], 0x01u);   /* Mode 01 */
    EXPECT_EQ(wbuf[5], 0x00u);   /* PID 0x00 */

    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payloads[0][0], 0x41u);
    EXPECT_EQ(cap.payloads[0][1], 0x00u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 6u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST_F(J2534AdoptionTest, FilterRegisteredOnSessionStart)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);

    EXPECT_NE(fid, 0u);
    EXPECT_EQ(ftype, ECUIO_PT_FLOW_CONTROL_FILTER);
    EXPECT_EQ(len, 4u);                       /* bare 4-byte CAN IDs */
    EXPECT_EQ(mask[0], 0xFFu); EXPECT_EQ(mask[1], 0xFFu);
    EXPECT_EQ(mask[2], 0xFFu); EXPECT_EQ(mask[3], 0xFFu);
    EXPECT_EQ(pattern[0], 0x00u); EXPECT_EQ(pattern[1], 0x00u);
    EXPECT_EQ(pattern[2], 0x07u); EXPECT_EQ(pattern[3], 0xE8u);
    /* FC is the bare tester ID, same 4-byte shape, not exposed by
     * the page's last filter; equal-length passing is the seam under test. */

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* DETACH ORDERING: StopMsgFilter called exactly once with registered ID
 * BEFORE disconnect. The fake publishes the stop on its page. */
TEST_F(J2534AdoptionTest, DetachStopsFilterBeforeDisconnect)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    unsigned long registered_fid = 0, dummy_ftype = 0, dummy_len = 0;
    unsigned char dummy_mask[12], dummy_pattern[12];
    get_last_filter(&registered_fid, &dummy_ftype,
                    dummy_mask, dummy_pattern, &dummy_len);

    /* Stop count should be zero before close */
    unsigned long stop_fid = 0;
    get_last_stop(&stop_fid);
    EXPECT_EQ(stop_fid, 0u);

    /* Close invokes detach, which should stop the filter */
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);

    get_last_stop(&stop_fid);
    EXPECT_EQ(stop_fid, registered_fid);

    ecuio_session_destroy(session);
}

/* COUPLING RULE: ?poll= needs a channel to receive on. Under raw the
 * profile declares none, so this is rejected: at ATTACH, not at
 * parse, because the rule consults the profile's channels, which
 * parse_query can't see. Still before any DLL load. */
TEST_F(J2534AdoptionTest, PollWithoutChannelRejectedBeforeDllLoad)
{
    ecuio_session_t* session = nullptr;
    char uri[700];

    /* A passive profile that declares no channel, with ?poll= */
    static ecuio_signal_def_t plain[1];
    plain[0] = ecuio_signal_def_t{};
    plain[0].id = 1;
    plain[0].kind = ECUIO_VALUE_UINT;
    plain[0].message_id = 0x123;
    plain[0].bit_length = 8;
    plain[0].scale = 1.0;
    static ecuio_signal_set_t passive;
    passive = ecuio_signal_set_t{};
    passive.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    passive.count = 1;
    passive.defs = plain;
    ASSERT_EQ(ecuio_profile_register("test-passive-poll", &passive, nullptr),
              ECUIO_OK);
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-passive-poll&poll=0x0C",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);

    /* The same rule for a profile whose own requests have no channel. */
    static const uint8_t kPids[] = {0x0C};
    static ecuio_signal_def_t defs[1];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(
        register_pid_profile("test-no-channel", kPids, 1, defs, &set));
    set.channel_count = 0;
    set.channels = nullptr;
    ASSERT_EQ(ecuio_profile_register("test-no-channel", &set, nullptr),
              ECUIO_OK);
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-no-channel",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the profile requests PIDs but declares no channel to "
                 "receive on");
    EXPECT_EQ(session, nullptr);
}

/* PASSIVE RECEIVE: ?profile=raw&filter=0x7E8 without ?poll=, filter
 * registers, no writes generated, frames still stripped and stamped. */
/* Raw with no ?filter= listens on the default profile's channel, the
 * engine ECU (ADR-0013's appendix of 2026-10-26): its filter is 0x7E8,
 * a reply on 0x7E8 arrives stripped and stamped, a frame for another ID
 * doesn't, and ?poll= alone asks the engine ECU, on 0x7E0 */
TEST_F(J2534AdoptionTest, RawListensToTheEngineEcu)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw", FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    ASSERT_EQ(info.channel_count, 1u);
    EXPECT_EQ(info.rx_ids[0], 0x7E8u);
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);
    EXPECT_NE(fid, 0u) << "a filter, for the engine ECU";
    EXPECT_EQ(pattern[2], 0x07);
    EXPECT_EQ(pattern[3], 0xE8);
    const unsigned char other[7] = {0x00, 0x00, 0x07, 0xE9, 0x41, 0x0C, 0x01};
    const unsigned char reply[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    inject(other, sizeof(other), 1000, 0, 1);
    inject(reply, sizeof(reply), 2000, 0, 1);
    wait_for_count(cap, 1, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_EQ(cap.count.load(), 1) << "the 0x7E9 frame isn't the engine ECU's";
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payload_lens[0], 4u);
    EXPECT_EQ(cap.payloads[0][0], 0x41);
    EXPECT_EQ(cap.payloads[0][3], 0x40);

    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw&poll=0x0C",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_writes(get_write_count, 1, 300);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[2], 0x07);
    EXPECT_EQ(wbuf[3], 0xE0) << "asked on the engine ECU's request ID";
    EXPECT_EQ(wbuf[4], 0x01);
    EXPECT_EQ(wbuf[5], 0x0C);
}

TEST_F(J2534AdoptionTest, PassiveReceiveWithFilterNoPoll)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    EXPECT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);

    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Write count should remain zero */
    unsigned long wc = 0;
    get_write_count(&wc);
    EXPECT_EQ(wc, 0u);

    /* Inject a frame that matches the filter */
    unsigned char frame[12] = {
        0x00, 0x00, 0x07, 0xE8,   /* 4-byte rx_id BE */
        0x02, 0x01, 0x0C, 0x00,   /* 8-byte data field */
        0x00, 0x00, 0x00, 0x00
    };
    inject(frame, sizeof(frame), 1000, 0, 1);

    /* Wait for read -> strip -> dispatch */
    wait_for_count(cap, 1, 50);
    EXPECT_EQ(cap.count.load(), 1u);

    /* Header stripped: payload starts at index 0, length = 8 */
    EXPECT_EQ(cap.payloads[0][0], 0x02u);
    EXPECT_EQ(cap.payloads[0][1], 0x01u);
    EXPECT_EQ(cap.payloads[0][2], 0x0Cu);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* FILTER GATING: ECUIO_FAKE_FILTER_GATING=1 makes ReadMsgs drop non-matching
 * frames. The fake drains them (not head-of-line blocking). */
TEST_F(J2534AdoptionTest, FilterGatingDropsNonMatchingFrames)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Arm gating on the fake's page (TearDown's reset disarms it) */
    set_gating(1);

    /* 12-byte device-shaped frames: [ID BE][data] */
    const unsigned char match[12] = {
        0x00, 0x00, 0x07, 0xE8,   0x02, 0x01, 0x0C, 0x00, 0, 0, 0, 0
    };
    const unsigned char nomatch[12] = {
        0x00, 0x00, 0x07, 0xEA,   0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0
    };
    inject(match,   sizeof(match),   1000, 0, 1);
    inject(nomatch, sizeof(nomatch), 1100, 0, 1);
    inject(match,   sizeof(match),   1200, 0, 1);

    /* Exactly the 2 matching frames: the non-matching one is DRAINED
     * by the fake, not parked at the queue head (head-of-line check).
     * No poll on this URI, so nothing else can dispatch. */
    wait_for_count(cap, 2, 50);
    EXPECT_EQ(cap.count.load(), 2);

    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.arbitration_ids[1], 0x7E8u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* TXID OVERRIDE: ?txid=0x7DF for broadcast, default is 0x7E0 */
TEST_F(J2534AdoptionTest, TxidOverrideChangesRequestTarget)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C&txid=0x7DF",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_count(cap, 3, 100);

    /* Check recorded write uses 0x7DF not 0x7E0 */
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xDFu);
    EXPECT_EQ(wbuf[4], 0x01u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

// ---------------------------------------------------------------------------
// ADR-0013: j2534 profile selection (obd2 is the default; the
// ?profile= override rides the strict grammar through parse_query)
// ---------------------------------------------------------------------------

TEST(J2534StreamTest, DefaultProfileIsObd2)
{
    /* SLICE 2 FLIP: j2534:// with no override selects the obd2 set:
     * decode + multi-PID scheduler. Create/destroy without start: the
     * assertion is about selection, not streaming, so no filter
     * coupling fires (coupling lives in session_start). */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_obd2_signal_set());
    ecuio_session_destroy(session);
}

TEST(J2534StreamTest, RawProfileRemainsSelectableOnJ2534)
{
    /* The escape hatch: log-only sessions on the real transport. */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=raw";
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), nullptr);
    ecuio_session_destroy(session);
}

TEST(J2534StreamTest, ProfileOverrideMockSelectsMockSet)
{
    /* The override crosses the transport boundary: the SAME j2534
     * session can carry a named profile's set. Also proves parse_query
     * admits the profile key alongside every other strict key. */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=mock";
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_mock_signal_set());
    ecuio_session_destroy(session);
}

TEST(J2534StreamTest, UnknownProfileRejectedAtCreate)
{
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=bogus";
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session),
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

} // namespace

#endif // _WIN32
