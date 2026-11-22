// File/layer: tests/test_j2534_stream_probe.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* ====================================================================
 * The probe (ADR-0015 section 4): the supported-PID walk before the rotation,
 * and dropping only on explicit answers.
 * ==================================================================== */

/* The fake's ECU supports every linear PID J1979 defines, with 01, 41
 * and 7F, and its bitmaps say so, so the walk asks 0x00 to 0xA0 (whose
 * last bit is clear) and drops nothing. The expected bitmaps are worked
 * out here from the PIDs by SAE J1979's layout (PID base + k is bit
 * 0x80 >> ((k - 1) % 8) of byte (k - 1) / 8), not taken from the fake:
 *   0x00: 01 04-08 | 09-10 | 11 14-18 | 19-1B 1F 20  ->  9F FF 9F E3
 *   0x20: 21-40, every one                         ->  FF FF FF FF
 *   0x40: 41-48 | 49-50 | 52-58 | 59-5E 60         ->  FF FF 7F FD
 *   0x60: 61-64 66-68 | | | 7F 80                  ->  F7 00 00 03
 *   0x80: | 8E | | A0                              ->  00 04 00 01
 *   0xA0: A2 A6 | | |                              ->  44 00 00 00 */
TEST_F(J2534AdoptionTest, ProbeWalksTheChainBeforeTheRotation)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s?identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 8, 200);
    expect_write(0, 0x01, 0x00);
    expect_write(1, 0x01, 0x20);
    expect_write(2, 0x01, 0x40);
    expect_write(3, 0x01, 0x60);
    expect_write(4, 0x01, 0x80);
    expect_write(5, 0x01, 0xA0);
    expect_write(6, 0x01, 0x04); /* the rotation: two fast, one slow */
    expect_write(7, 0x01, 0x0C);

    ecuio_channel_support_t s = {};
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_COMPLETE) != 0;
    }, 2000)) << "flags " << s.flags;
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.rx_id, 0x7E8u);
    EXPECT_EQ(s.ranges, 0x3Fu);
    const uint8_t want[32] = {0x9F, 0xFF, 0x9F, 0xE3, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0x7F, 0xFD, 0xF7, 0x00, 0x00, 0x03,
                              0x00, 0x04, 0x00, 0x01, 0x44, 0x00, 0x00, 0x00};
    EXPECT_EQ(memcmp(s.pid_bitmap, want, sizeof(want)), 0);
    /* want's 100 set bits, less the chain's 20, 40, 60, 80 and A0 */
    EXPECT_EQ(s.pids_supported, 95u);
    EXPECT_EQ(s.pids_through, 0xC0u) << "A0's bitmap covers A1 to C0";
    EXPECT_EQ(s.requests, 92u);
    EXPECT_EQ(s.dropped, 0u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* The bitmaps alone drop what they mark unsupported: 0x12 is clear in
 * 0x00's, 0x5F in 0x40's, and 0xC3 lies past the chain's end. The ECU
 * answers nothing for an unsupported PID here (as J1979 lets it), so
 * without the walk those three would each wait out 100 ms every pass. */
/* ADR-0039 Appendix A through the surrogate: once its PIDs have
 * answered alone, a request carries several and the fake's ECU answers
 * each; ?pack=0 asks one PID per request; an ECU answering a pack for
 * its first PID alone, three in a row, ends packing on the channel */
TEST_F(J2534AdoptionTest, PackedRequestsReachTheFakesEcu)
{
    const auto flags_when = [](ecuio_session_t* session, uint32_t want,
                               int budget_ms) {
        ecuio_channel_support_t s = {};
        (void)wait_for_support(session, s,
                               [want](const ecuio_channel_support_t& r) {
                                   return (r.flags & want) != 0;
                               },
                               budget_ms);
        return s.flags;
    };
    const auto open = [](const char* query) {
        char uri[700];
        snprintf(uri, sizeof(uri), "j2534://%s?identify=0&diagnose=0%s",
                 FAKE_J2534_DLL_PATH, query);
        ecuio_session_t* session = nullptr;
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
        EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
        return session;
    };

    ecuio_session_t* session = open("");
    ASSERT_NE(session, nullptr);
    uint32_t flags = flags_when(session, ECUIO_SUPPORT_PACKED, 10000);
    EXPECT_TRUE(flags & ECUIO_SUPPORT_PACKED);
    EXPECT_FALSE(flags & ECUIO_SUPPORT_UNPACKED);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    session = open("&pack=0");
    ASSERT_NE(session, nullptr);
    unsigned long before = 0;
    get_write_count(&before);
    wait_for_writes(get_write_count, before + 150, 1000);
    flags = flags_when(session, ECUIO_SUPPORT_PACKED, 0);
    EXPECT_FALSE(flags & (ECUIO_SUPPORT_PACKED | ECUIO_SUPPORT_UNPACKED));
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    KnobGuard first("ECUIO_FAKE_PACK_FIRST_ONLY", "1");
    session = open("");
    ASSERT_NE(session, nullptr);
    flags = flags_when(session, ECUIO_SUPPORT_UNPACKED, 10000);
    EXPECT_TRUE(flags & ECUIO_SUPPORT_UNPACKED);
    EXPECT_FALSE(flags & ECUIO_SUPPORT_PACKED);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST_F(J2534AdoptionTest, ProbeDropsWhatTheBitmapsMarkUnsupported)
{
    static const uint8_t kPids[] = {0x12, 0x0C, 0x5F, 0xC3};
    static ecuio_signal_def_t defs[4];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-bitmaps", kPids, 4,
                                                 defs, &set));
    KnobGuard silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");

    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=test-probe-bitmaps&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 11, 300);
    for (unsigned long i = 6; i < 11; ++i) {
        expect_write(i, 0x01, 0x0C);
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.requests, 4u);
    ASSERT_EQ(s.dropped, 3u);
    const uint8_t gone[3] = {0x12, 0x5F, 0xC3};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(s.dropped_list[i].request[0], 0x01u) << i;
        EXPECT_EQ(s.dropped_list[i].request[1], gone[i]) << i;
        EXPECT_EQ(s.dropped_list[i].reason, ECUIO_DROP_BITMAP) << i;
    }

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?probe=0 sends the set as written: no walk, and the PIDs the ECU
 * refuses (7F 01 12 for 0x12, 0x5F and 0xC3) stay in the rotation. */
TEST_F(J2534AdoptionTest, ProbeOffSendsTheSetAsWritten)
{
    static const uint8_t kPids[] = {0x12, 0x0C, 0x5F, 0xC3};
    static ecuio_signal_def_t defs[4];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-off", kPids, 4,
                                                 defs, &set));
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=test-probe-off&probe=0&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 8, 300);
    for (unsigned long i = 0; i < 8; ++i) {
        expect_write(i, 0x01, kPids[i % 4]);
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.flags, 0u);
    EXPECT_EQ(s.generation, 0u);
    EXPECT_EQ(s.requests, 4u);
    EXPECT_EQ(s.dropped, 0u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A DID the ECU refuses with 7F 22 31 (request out of range) leaves after
 * its first answer; the one it answers stays. No Service 01, so no walk. */
TEST_F(J2534AdoptionTest, NegativeReplyDropsTheDidTheEcuRefuses)
{
    static ecuio_signal_def_t defs[2] = {};
    for (int i = 0; i < 2; ++i) {
        defs[i].id = static_cast<uint16_t>(1 + i);
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].start_byte = 3;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].message_id = ECUIO_OBD2_ECU_ARB_ID;
        defs[i].mux_length = 3;
    }
    defs[0].mux_key = 0x621E1Du;   /* the fake's ECU doesn't know it */
    defs[1].mux_key = 0x621E1Cu;
    std::snprintf(defs[0].name, sizeof defs[0].name, "%s", "DID 1E1D");
    std::snprintf(defs[1].name, sizeof defs[1].name, "%s", "DID 1E1C");
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    set.channel_count = ecuio_obd2_signal_set()->channel_count;
    set.channels = ecuio_obd2_signal_set()->channels;
    ASSERT_EQ(ecuio_profile_register("test-probe-did", &set, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=test-probe-did&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 6, 300);
    expect_write(0, 0x22, 0x1E);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    for (unsigned long i = 0; i < 6; ++i) {
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 7u) << "write " << i;
        EXPECT_EQ(wbuf[6], (i == 0) ? 0x1Du : 0x1Cu) << "write " << i;
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.flags, 0u) << "no walk";
    ASSERT_EQ(s.dropped, 1u);
    EXPECT_EQ(s.dropped_list[0].length, 3u);
    EXPECT_EQ(s.dropped_list[0].request[2], 0x1Du);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_NOT_SUPPORTED);
    EXPECT_EQ(s.dropped_list[0].nrc, 0x31u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* uds-generic (ADR-0020): ?did= builds the session's set, so a listed
 * DID is asked for, 22 <DID> to the default ECU's 0x7E0, and its reply
 * decodes through the def the URI made, as 0x1234 from the fake's bytes. */
TEST_F(J2534AdoptionTest, UdsGenericPollsItsDidsAndDecodesTheReply)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=uds-generic&did=1E1C:u16",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->count, 1u);
    EXPECT_STREQ(set->defs[0].name, "DID 1E1C");
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 1, 100);

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 7u);
    const unsigned char want_req[7] = {0x00, 0x00, 0x07, 0xE0, 0x22, 0x1E, 0x1C};
    EXPECT_EQ(memcmp(wbuf, want_req, sizeof(want_req)), 0);

    ASSERT_GE(cap.count.load(), 1);
    ecuio_frame_t f = {};
    f.arbitration_id = cap.arbitration_ids[0];
    memcpy(f.data, cap.payloads[0], cap.payload_lens[0]);
    f.data_length = cap.payload_lens[0];
    ASSERT_EQ(ecuio_def_applies_to_frame(&set->defs[0], &f), 1);
    double v = 0.0;
    ASSERT_EQ(ecuio_decode_signal(&set->defs[0], f.data, f.data_length, &v),
              ECUIO_DECODE_OK);
    EXPECT_EQ(v, double{(0x12 << 8) | 0x34});

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* An <ecu>: prefix names the ECU by its response ID and asks it 8
 * below: 7E9:1E1C writes to 0x7E1, and the fake answers from 0x7E9. */
TEST_F(J2534AdoptionTest, UdsGenericEcuPrefixAsksThatEcu)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=uds-generic&did=7E9:1E1C",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_STREQ(ecuio_session_signal_set(session)->defs[0].name, "0x7E9 DID 1E1C");
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 1, 100);

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 7u);
    const unsigned char want_req[7] = {0x00, 0x00, 0x07, 0xE1, 0x22, 0x1E, 0x1C};
    EXPECT_EQ(memcmp(wbuf, want_req, sizeof(want_req)), 0);
    ASSERT_GE(cap.count.load(), 1);
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E9u);
    const unsigned char want_reply[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
    ASSERT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 5u);
    EXPECT_EQ(memcmp(cap.payloads[0], want_reply, sizeof(want_reply)), 0);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Two ECUs in one session answer each its own: the engine ECU's DID
 * 1E1C on 0x7E8, and a module's (ECUIO_FAKE_MODULE_DIDS) D801 on 0x7E9,
 * which refuses the engine's DID; each reply carries its ECU's ID */
TEST_F(J2534AdoptionTest, TwoEcusInOneSessionAnswerEachItsOwn)
{
    KnobGuard module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://"
             "%s?profile=uds-generic&did=1E1C,7E9:D801&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 4, 200);

    bool engine = false, module_ok = false;
    const int seen = cap.count.load();
    const int n = seen < 8 ? seen : 8; /* the capture keeps the first 8 */
    for (int i = 0; i < n; ++i) {
        const unsigned short len = cap.payload_lens[i];
        if (cap.arbitration_ids[i] == 0x7E8u) {
            const unsigned char want[5] = {0x62, 0x1E, 0x1C, 0x12, 0x34};
            EXPECT_EQ(len, 5u) << "frame " << i;
            engine =
                engine || (len == 5 && memcmp(cap.payloads[i], want, 5) == 0);
        } else if (cap.arbitration_ids[i] == 0x7E9u) {
            const unsigned char want[5] = {0x62, 0xD8, 0x01, 0x00, 0x12};
            EXPECT_EQ(len, 5u) << "frame " << i;
            module_ok = module_ok ||
                        (len == 5 && memcmp(cap.payloads[i], want, 5) == 0);
        } else {
            ADD_FAILURE() << "a reply on 0x" << std::hex
                          << cap.arbitration_ids[i];
        }
    }
    EXPECT_TRUE(engine) << "the engine ECU's 1E1C on 0x7E8";
    EXPECT_TRUE(module_ok) << "the module's D801 on 0x7E9";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

} // namespace

#endif // _WIN32
