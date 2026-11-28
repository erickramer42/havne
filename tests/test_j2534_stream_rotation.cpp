// File/layer: tests/test_j2534_stream_rotation.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* The last FLOW_CONTROL filter's flow-control message, its 4-byte ID */
uint32_t page_last_flow_id()
{
    const fake_control::Filter f = fake_control::last_filter(fake_page());
    return (uint32_t(f.flow[0]) << 24) | (uint32_t(f.flow[1]) << 16) |
           (uint32_t(f.flow[2]) << 8) | f.flow[3];
}

// ---------------------------------------------------------------------------
// ADR-0013: obd2 scheduler mode, rotation, coupling, drain
// ---------------------------------------------------------------------------

/* ROTATION: ?profile=obd2&filter=0x7E8, the worker requests the obd2
 * set's PIDs in its two rate classes (ADR-0039 section 3): two fast ones (04,
 * 0C, 0D, 11, 10, then the rest in PID order), then one slow one (05,
 * 0F, 2F, 42, 5C, then the rest), each class round-robin. Order is asserted from the fake's recorded
 * WRITES (deterministic, one per reply), not from received frames.
 * ?probe=0 leaves out the bitmaps the probe asks first
 * (ProbeWalksTheChainBeforeTheRotation). */
TEST_F(J2534AdoptionTest, Obd2SchedulerRotatesPidsInSetOrder)
{
    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=obd2&filter=0x7E8&probe=0&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* The page keeps the first 16 writes: the ten's fast PIDs, then the
     * table's in PID order, beside the ten's slow ones. */
    wait_for_writes(get_write_count, 16, 250);
    const uint8_t want[16] = {0x04, 0x0C, 0x05, 0x0D, 0x11, 0x0F, 0x10, 0x06,
                              0x2F, 0x08, 0x0A, 0x42, 0x0B, 0x0E, 0x5C, 0x22};
    for (unsigned long i = 0; i < 16; ++i) {
        expect_write(i, 0x01, want[i]);
    }

    /* RX: the next request waits for the reply to the last, so the
     * first three received frames answer the first three requests in
     * order. */
    wait_for_count(cap, 3, 100);
    ASSERT_GE(cap.count.load(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(cap.arbitration_ids[i], 0x7E8u);
        EXPECT_EQ(cap.payloads[i][0], 0x41u);
    }
    EXPECT_EQ(cap.payloads[0][1], 0x04u);
    EXPECT_EQ(cap.payloads[1][1], 0x0Cu);
    EXPECT_EQ(cap.payloads[1][2], 0x1Fu);
    EXPECT_EQ(cap.payloads[1][3], 0x40u);
    EXPECT_EQ(cap.payloads[2][1], 0x05u);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* FLAGSHIP: a bare j2534:// URI derives its channel from the obd2
 * profile: filter pattern 7E8, requests to 7E0, scheduler running.
 * No ?filter= needed; when present, it's an override. */
TEST_F(J2534AdoptionTest, BareJ2534UriDerivesObd2ChannelAndStarts)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_obd2_signal_set());
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Derived filter: exact-match mask, pattern 00 00 07 E8 */
    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);
    EXPECT_NE(fid, 0u);
    EXPECT_EQ(ftype, ECUIO_PT_FLOW_CONTROL_FILTER);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(mask[0], 0xFFu); EXPECT_EQ(mask[1], 0xFFu);
    EXPECT_EQ(mask[2], 0xFFu); EXPECT_EQ(mask[3], 0xFFu);
    EXPECT_EQ(pattern[0], 0x00u); EXPECT_EQ(pattern[1], 0x00u);
    EXPECT_EQ(pattern[2], 0x07u); EXPECT_EQ(pattern[3], 0xE8u);

    /* Derived tx: requests go to 7E0, the probe's first bitmap first */
    wait_for_writes(get_write_count, 3, 200);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xE0u);
    EXPECT_EQ(wbuf[4], 0x01u);
    EXPECT_EQ(wbuf[5], 0x00u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* OVERRIDE SEMANTICS: a lone ?txid= under the obd2 default keeps
 * the derived 7E8 filter but rewrites the request target: the
 * broadcast-request pattern (?txid=0x7DF) without restating the
 * channel. Mixed override + derived channel. */
TEST_F(J2534AdoptionTest, TxidOverrideRewritesDerivedTransmitTarget)
{
    char uri[512];
    snprintf(uri, sizeof(uri),
             "j2534://%s?txid=0x7DF", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);
    EXPECT_NE(fid, 0u);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(pattern[3], 0xE8u);          /* rx stays derived 7E8 */

    wait_for_writes(get_write_count, 3, 200);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xDFu);   /* tx 7DF */

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A functional request (?txid=0x7DF) still goes out on 0x7DF, one
 * frame, but the reply's Flow Control goes on the physical request ID
 * paired with the channel's rx (ISO 15765-4 section 6.3.2.1): the filter's
 * flow-control message names it, so a segmented reply completes. A
 * physical request's is its own ID */
TEST_F(J2534AdoptionTest, AFunctionalRequestsFlowControlGoesToThePhysicalId)
{
    struct Case {
        const char* query;
        uint32_t flow;
        uint8_t tx_low;
    };
    for (const Case& k :
         {Case{"?txid=0x7DF", 0x7E0, 0xDF},
          Case{"?profile=raw&filter=0x7E9&poll=0x00&txid=0x7DF", 0x7E1, 0xDF},
          Case{"", 0x7E0, 0xE0}}) {
        SCOPED_TRACE(k.query);
        fake_control::reset(fake_page());
        char uri[700];
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH,
                 k.query);
        ecuio_session_t* session = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
        unsigned long fid = 0, ftype = 0, len = 0;
        unsigned char mask[12] = {}, pattern[12] = {};
        get_last_filter(&fid, &ftype, mask, pattern, &len);
        EXPECT_EQ(ftype,
                  static_cast<unsigned long>(ECUIO_PT_FLOW_CONTROL_FILTER));
        EXPECT_EQ(page_last_flow_id(), k.flow);
        wait_for_writes(get_write_count, 1, 200);
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(0, wbuf, &wlen, &wflags);
        EXPECT_EQ(wbuf[3], k.tx_low) << "the request's own ID";
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
    }
}

/* ROUTING: each request goes to the channel of the def it was derived
 * from. The set's first def reads a 62 <DID> reply from a second ECU, so
 * it asks 0x7E1 for 22 12 0C; the second reads 41 0C from the first, so
 * PID 0x0C's request goes to 0x7E0, though the DID's key also ends in
 * 0x0C. The channels are served side by side, the first channel's
 * request first. ?probe=0: the fake doesn't know DID 0x120C, and the
 * probe would drop it at its first refusal, and walk 0x7E0's bitmaps
 * first. Test-only profile, static storage (the registry keeps the
 * pointer). */
TEST_F(J2534AdoptionTest, RequestsGoToTheirDefsChannel)
{
    static const ecuio_channel_decl_t kChannels[] = {
        {0x7E8u, 0x7E0u, 0, 0, {}}, {0x7E9u, 0x7E1u, 0, 0, {}}};
    static ecuio_signal_def_t defs[2] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].start_byte = 3;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    defs[0].message_id = 0x7E9u;
    defs[0].mux_length = 3;
    defs[0].mux_key = 0x62120Cu;
    defs[1] = defs[0];
    defs[1].id = 2;
    defs[1].start_byte = 2;
    defs[1].message_id = 0x7E8u;
    defs[1].mux_length = 2;
    defs[1].mux_key = 0x410Cu;
    std::snprintf(defs[1].name, sizeof defs[1].name, "%s", "Engine RPM");
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    set.channel_count = 2;
    set.channels = kChannels;
    ASSERT_EQ(ecuio_profile_register("test-mode01-routing", &set, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=test-mode01-routing&probe=0&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 4, 200);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    for (unsigned long i = 0; i < 4; i += 2) {
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[2], 0x07u) << "write " << i;
        EXPECT_EQ(wbuf[3], 0xE0u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        EXPECT_EQ(wbuf[5], 0x0Cu) << "write " << i;
        get_recorded_write(i + 1, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 7u) << "write " << i + 1;
        EXPECT_EQ(wbuf[2], 0x07u) << "write " << i + 1;
        EXPECT_EQ(wbuf[3], 0xE1u) << "write " << i + 1;
        EXPECT_EQ(wbuf[4], 0x22u) << "write " << i + 1;
        EXPECT_EQ(wbuf[5], 0x12u) << "write " << i + 1;
        EXPECT_EQ(wbuf[6], 0x0Cu) << "write " << i + 1;
    }

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* SERVICE 0x22 (ADR-0015 section 2): a def keyed 62 <DID> enrols the request
 * 22 <DID>, 7 bytes on the wire, and decodes the reply. The fake's ECU
 * knows DID 0x1E1C and answers 62 1E 1C 12 34, so the value is 0x1234,
 * worked out here from the reply's bytes, not from the def. */
TEST_F(J2534AdoptionTest, DidDefRequestsItsDidAndDecodesTheReply)
{
    static ecuio_signal_def_t def = {};
    def.id = 1;
    def.kind = ECUIO_VALUE_UINT;
    def.byte_order = ECUIO_BYTE_ORDER_BE;
    def.start_byte = 3;
    def.bit_length = 16;
    def.scale = 1.0;
    def.message_id = ECUIO_OBD2_ECU_ARB_ID;
    def.mux_length = 3;
    def.mux_key = 0x621E1Cu;
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &def;
    set.channel_count = ecuio_obd2_signal_set()->channel_count;
    set.channels = ecuio_obd2_signal_set()->channels;
    ASSERT_EQ(ecuio_profile_register("test-did", &set, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-did",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 1, 100);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);   /* then read */
    ecuio_session_destroy(session);

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 7u);
    const unsigned char want_req[7] = {0x00, 0x00, 0x07, 0xE0,
                                       0x22, 0x1E, 0x1C};
    EXPECT_EQ(memcmp(wbuf, want_req, sizeof(want_req)), 0);

    ASSERT_GE(cap.count.load(), 1);
    ASSERT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 5u);
    const unsigned char want_reply[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    EXPECT_EQ(memcmp(cap.payloads[0], want_reply, sizeof(want_reply)), 0);
    ecuio_frame_t f = {};
    f.arbitration_id = cap.arbitration_ids[0];
    memcpy(f.data, cap.payloads[0], 5);
    f.data_length = 5;
    ASSERT_EQ(ecuio_def_applies_to_frame(&def, &f), 1);
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&def, f.data, f.data_length, &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, double{(0x12 << 8) | 0x34});
}

/* BACKLOG: inject more frames than the replies the requests bring and
 * assert every one is delivered beside them, none dropped and none
 * head-of-line-blocking the replies. Uses a local 32-slot capture: the
 * shared StreamCapture's 8 slots would truncate under request traffic
 * + backlog combined. */
TEST_F(J2534AdoptionTest, RequestModeDeliversABacklogBesideItsReplies)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    struct DrainCapture {
        std::atomic<int> count{0};
        unsigned char payloads[32][8]{};
        uint32_t ids[32]{};
    } dcap;
    auto drain_cb = [](const ecuio_frame_t* frame, void* user_data) {
        DrainCapture* c = static_cast<DrainCapture*>(user_data);
        const int i = c->count.fetch_add(1);
        if (i < 32) {
            const unsigned short n = (frame->data_length < 8)
                                     ? frame->data_length : 8;
            memcpy(c->payloads[i], frame->data, n);
            c->ids[i] = frame->arbitration_id;
        }
    };
    ASSERT_EQ(ecuio_register_frame_callback(session, drain_cb, &dcap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    for (int i = 0; i < 12; ++i) {
        unsigned char frame[12] = {};
        frame[2] = 0x07; frame[3] = 0xE8;      /* rx id BE 0x7E8 */
        frame[4] = static_cast<unsigned char>(0xB0 + i);   /* marker */
        frame[5] = 0xEE;
        inject(frame, sizeof(frame), 9000u + i, 0, 1);
    }

    auto seen_all_markers = [&dcap]() {
        bool seen[12] = {};
        int n = dcap.count.load();
        if (n > 32) n = 32;
        for (int i = 0; i < n; ++i) {
            const unsigned char b = dcap.payloads[i][0];
            if (b >= 0xB0 && b <= 0xBB) {
                seen[b - 0xB0] = true;
            }
        }
        for (bool s : seen) {
            if (!s) return false;
        }
        return true;
    };

    /* Read back to back with the replies; budget 4 s for CI variance. */
    for (int i = 0; i < 400 && !seen_all_markers(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(seen_all_markers());

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?poll= under a profile with requests overrides the rotation. If attach
 * enrolled the set's requests too, ?poll=0x0C under obd2 would rotate
 * the whole set and the URI's PID would be one among ten. Five
 * consecutive writes pin it: rotation would give 0x04, 0x05, 0x0C,
 * 0x0D, 0x0F. */
TEST_F(J2534AdoptionTest, PollOverridesSchedulerRotation)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&poll=0x0C", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session), ecuio_obd2_signal_set());

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 5, 200);   /* 5 requests, 2 s budget */

    unsigned long wc = 0;
    get_write_count(&wc);
    ASSERT_GE(wc, 5u) << "poll loop produced too few writes";

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    for (unsigned long i = 0; i < 5; ++i) {
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;   /* Mode 01 */
        EXPECT_EQ(wbuf[5], 0x0Cu) << "write " << i;   /* the URI's PID */
    }

    /* The derived channel still applies: request target 7E0, padded. */
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wbuf[2], 0x07u);
    EXPECT_EQ(wbuf[3], 0xE0u);
    EXPECT_NE(wflags & ECUIO_PT_TX_ISO15765_FRAME_PAD, 0u);

    /* And the echo still lands stripped with the 7E8 identity. */
    wait_for_count(cap, 1, 100);
    ASSERT_GE(cap.count.load(), 1);
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payloads[0][0], 0x41u);
    EXPECT_EQ(cap.payloads[0][1], 0x0Cu);
    EXPECT_EQ(cap.payloads[0][2], 0x1Fu);   /* canned RPM 2000 */
    EXPECT_EQ(cap.payloads[0][3], 0x40u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

} // namespace

#endif // _WIN32
