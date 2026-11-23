// File/layer: tests/test_j2534_stream.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534QueryParamTest, J2534StreamTest
//          (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

void page_connect_params(unsigned long* baud, unsigned long* flags,
                         unsigned long* protocol)
{
    uint32_t p = 0, f = 0, b = 0;
    fake_control::last_connect(fake_page(), &p, &f, &b);
    *baud = b;
    *flags = f;
    *protocol = p;
}

/* Shared helper: create + start a session on a URI and read what
 * PassThruConnect was asked, from the fake's page. */
class J2534QueryParamTest : public ::testing::Test {
protected:
    fn_get_connect_params get_params = page_connect_params;

    void SetUp() override
    {
        ASSERT_NE(fake_page(), nullptr);
        fake_control::reset(fake_page());
    }

    void TearDown() override
    {
        if (fake_page() != nullptr) fake_control::reset(fake_page());
    }

    ecuio_status_t start_session_with_uri(const char* uri)
    {
        ecuio_session_t* session = nullptr;
        ecuio_status_t rc = ecuio_session_create(uri, &session);
        if (rc != ECUIO_OK) {
            return rc;
        }
        rc = ecuio_session_start(session);
        /* Connect happened at start; observe params before teardown. */
        unsigned long baud = 0, flags = 0, proto = 0;
        get_params(&baud, &flags, &proto);
        last_baud = baud;
        last_flags = flags;
        last_protocol = proto;
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        return rc;
    }

    unsigned long last_baud = 0;
    unsigned long last_flags = 0;
    unsigned long last_protocol = 0;
};

TEST_F(J2534QueryParamTest, DefaultConnectUsesIso15765At500k)
{
    char uri[512];
    /* ?profile=raw: the slice-2 default is obd2 (scheduler mode);
     * this test pins the CONNECT defaults, so it opts out of the
     * scheduler to keep the legacy posture it has always asserted. */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    /* Unset baud (sentinel 0) resolves to the ISO 15765 500k default;
     * flags default 0. */
    EXPECT_EQ(last_protocol, ECUIO_PT_PROTOCOL_ISO15765);
    EXPECT_EQ(last_baud, ECUIO_PT_BAUD_ISO15765_500K);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, BaudParameterReachesPassThruConnect)
{
    char uri[600];
    snprintf(uri, sizeof(uri),
             "j2534://%s?baud=250000&profile=raw", FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_baud, 250000u);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, FlagsAcceptHexAndDecimalAndCombineWithBaud)
{
    char uri[600];

    snprintf(uri, sizeof(uri),
             "j2534://%s?baud=125000&flags=0x20&profile=raw",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_baud, 125000u);
    EXPECT_EQ(last_flags, 0x20u);

    /* Decimal flags, reversed order, last-wins duplicate */
    snprintf(uri, sizeof(uri),
             "j2534://%s?flags=10&baud=500000&baud=250000&profile=raw",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_baud, 250000u);
    EXPECT_EQ(last_flags, 10u);
}

/* ?hold= (Known Issue #42's hold, j2534_hold.h) takes 0 and 1, the last
 * of several winning; its refusals are MalformedQueriesRejectedAtSessionCreate's */
TEST_F(J2534QueryParamTest, HoldTakesZeroOrOne)
{
    char uri[600];
    for (const char* q : {"?hold=0", "?hold=1", "?hold=1&hold=0"}) {
        snprintf(uri, sizeof(uri), "j2534://%s%s&profile=raw",
                 FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(start_session_with_uri(uri), ECUIO_OK) << q;
    }
}

/* A 29-bit channel opens 29-bit: CAN_29BIT_ID joins the connect flags
 * whenever the channels' IDs are wider than 11 bits, on top of any
 * ?flags=, so the channel matches the width its filter and writes use. */
TEST_F(J2534QueryParamTest, Wide29BitChannelSetsConnectFlag)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x18DAF110&txid=0x18DA10F1",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_flags, ECUIO_PT_CAN_29BIT_ID);

    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x18DAF110&txid=0x18DA10F1&flags=0x20",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_flags, ECUIO_PT_CAN_29BIT_ID | 0x20u);

    /* 11-bit: the flags stay exactly as given */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw&filter=0x7E8",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_flags, 0u);
}

/* Profile-declared channels follow the same rule. Channels mixing 11- and
 * 29-bit IDs would need CAN_ID_BOTH, which the transport doesn't request,
 * so that set is rejected at create. Test-only profiles, registered under
 * throwaway names with static storage (the registry keeps the pointer). */
TEST_F(J2534QueryParamTest, ProfileChannelWidthDecidesConnectFlag)
{
    static const ecuio_channel_decl_t kWide[] = {
        {0x18DAF110u, 0x18DA10F1u, 0, 0}};
    static const ecuio_channel_decl_t kMixed[] = {
        {0x7E8u, 0x7E0u, 0, 0}, {0x18DAF110u, 0x18DA10F1u, 0, 0}};
    static ecuio_signal_set_t wide = {};
    wide.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    wide.channel_count = 1;
    wide.channels = kWide;
    static ecuio_signal_set_t mixed = {};
    mixed.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    mixed.channel_count = 2;
    mixed.channels = kMixed;
    ASSERT_EQ(ecuio_profile_register("test-29bit", &wide, nullptr), ECUIO_OK);
    ASSERT_EQ(ecuio_profile_register("test-mixed-width", &mixed, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-29bit",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_flags, ECUIO_PT_CAN_29BIT_ID);

    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-mixed-width",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_EQ(session, nullptr);
}

TEST_F(J2534QueryParamTest, MalformedQueriesRejectedAtSessionCreate)
{
    ecuio_session_t* session = nullptr;

    /* Selector-miss still wins over a good query */
    EXPECT_EQ(ecuio_session_create("j2534://nonexistent-vendor?baud=250000",
                                   &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);

    /* Query splits from path; a nonexistent path fails AT LOAD:
     * proves the filename never got '?baud=...' glued onto it. */
    EXPECT_EQ(ecuio_session_create("j2534://C:\\does\\not\\exist.dll?baud=250000",
                                   &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(session, nullptr);

    /* Malformed queries must reject AT PARSE, before any DLL load:
     * asserted against the REAL fake path so a load success would
     * prove the parse wrongly accepted the query. */
    char uri[600];
    const char* bad_queries[] = {
        "?baud=abc",
        "?baud=0",
        "?baud=0x",
        "?flags=0x100000000", /* past a 32-bit unsigned long */
        "?txid=0x2000000f",   /* past the 29-bit CAN ID range */
        "?filter=0",
        "?",
        "?band=250000",
        "?baud=250000&",
        "?baud=250000&&flags=1",
        "?poll=",
        "?poll=0x100",
        "?poll=-1",
        "?probe=",
        "?probe=2",
        "?probe=00",
        "?probe=no",
        "?probe=0x0",
        "?probe=1&probe=",
        "?Probe=0",
        "?identify=",
        "?identify=2",
        "?identify=yes",
        "?identify=0x1",
        "?diagnose=",
        "?diagnose=2",
        "?diagnose=no",
        "?pack=",
        "?pack=on",
        "?gap=",
        "?gap=-1",
        "?gap=1001",
        "?gap=0x5",
        "?gap=1.5",
        "?gap=5ms",
        "?gap= 5",
        "?packframes=",
        "?packframes=0",
        "?packframes=16",
        "?packframes=0x2",
        "?packframes=+1",
        "?packframes=-1",
        "?packframes=1.5",
        "?packframes= 1",
        "?pack=0&packframes=1",
        "?hold=",
        "?hold=2",
        "?hold=yes",
        "?hold=0x1",
        "?hold=1&hold=",
        "?Hold=0",
        /* uds-generic (ADR-0020): its DIDs come from ?did=, which no
         * other profile takes */
        "?did=1E1C",
        "?profile=uds-generic",
        "?profile=uds-generic&did=",
        "?profile=uds-generic&did=1E1C,",
        "?profile=uds-generic&did=0x1E1C",
        "?profile=uds-generic&did=1EFF-1E00",
        "?profile=uds-generic&did=1E1C:u12",
        "?profile=uds-generic&did=7:1E1C",
        "?profile=uds-generic&did=7E8:7E9:1E1C",
        "?profile=uds-generic&did=7E8:1E1C:u8:x:y",
        /* 33 pairs, one past the most a query holds */
        "?probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1"
        "&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1"
        "&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1"
        "&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1"
        "&probe=1",
    };
    for (const char* q : bad_queries) {
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI)
            << "query: " << q;
        EXPECT_EQ(session, nullptr) << "query: " << q;
    }

    EXPECT_EQ(ecuio_session_create("j2534://junk", &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);

    /* A selector longer than any DLL path can't name an adapter. */
    const std::string long_selector =
        "j2534://" + std::string(ECUIO_J2534_DLL_PATH_LEN + 44, 'a');
    EXPECT_EQ(ecuio_session_create(long_selector.c_str(), &session),
              ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the adapter selector is longer than any DLL path");
    EXPECT_EQ(session, nullptr);
}

/* create splits the query once and judges its keys against the ones the
 * transport reads, so an unknown key is named with them, before the
 * values are read; then a bad value is named with what it should be. */
TEST_F(J2534QueryParamTest, RefusedQueriesNameTheirCause)
{
    char uri[700];
    ecuio_session_t* session = nullptr;
    snprintf(uri, sizeof(uri), "j2534://%s?baud=abc&band=250000",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "j2534:// takes no query key 'band', only baud, flags, "
                 "filter, txid, poll, probe, identify, diagnose, pack, scan, "
                 "gap, profile, did, "
                 "session, keepalive_ms, packframes and hold");
    snprintf(uri, sizeof(uri), "j2534://%s?hold=yes", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(), "?hold= needs 0 or 1, not 'yes'");
    snprintf(uri, sizeof(uri), "j2534://%s?probe=yes", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(), "?probe= needs 0 or 1, not 'yes'");
    snprintf(uri, sizeof(uri), "j2534://%s?identify=yes", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(), "?identify= needs 0 or 1, not 'yes'");
    snprintf(uri, sizeof(uri), "j2534://%s?diagnose=yes", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?diagnose= needs 0 or 1, not 'yes'");
    snprintf(uri, sizeof(uri), "j2534://%s?gap=1001", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?gap= needs a time in ms from 0 to 1000, not '1001'");
    snprintf(uri, sizeof(uri), "j2534://%s?packframes=16", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?packframes= needs a frame count from 1 to 15, not '16'");
    snprintf(uri, sizeof(uri), "j2534://%s?pack=0&packframes=1",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?packframes= sizes Service 01 packs: it can't go with "
                 "?pack=0");
    /* ?probe=0 and ?poll= pack nothing either */
    snprintf(uri, sizeof(uri), "j2534://%s?probe=0&packframes=2",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?packframes= sizes Service 01 packs: it can't go with "
                 "?probe=0");
    snprintf(uri, sizeof(uri), "j2534://%s?poll=0x0C&packframes=1",
             FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "?packframes= sizes Service 01 packs: it can't go with "
                 "?poll=");

    /* Both values are taken, with any profile, and every gap from 0 to
     * the most. */
    for (const char* q :
         {"?profile=raw&probe=0", "?probe=1", "?probe=0&probe=1",
          "?profile=raw&identify=0", "?identify=1&probe=0",
          "?profile=raw&diagnose=0", "?diagnose=1", "?gap=0", "?gap=1000",
          "?profile=raw&gap=20", "?packframes=1", "?packframes=15",
          "?probe=1&pack=1&packframes=2"}) {
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(start_session_with_uri(uri), ECUIO_OK) << "query: " << q;
    }
}

/* A path-like selector must be absolute: a relative one would follow
 * the DLL search order, the current directory included, and could load
 * another DLL of that name. Refused at create, before any load, so even
 * the fake's own file name, which the test's directory holds, is
 * refused. */
TEST_F(J2534QueryParamTest, RelativeDllPathsAreRefused)
{
    ecuio_session_t* session = nullptr;
    for (const char* u : {"j2534://fake_j2534.dll", "j2534://Release/fake_j2534.dll",
                          "j2534://.\\fake_j2534.dll", "j2534://\\fake_j2534.dll",
                          "j2534://C:fake_j2534.dll?profile=raw"}) {
        EXPECT_EQ(ecuio_session_create(u, &session), ECUIO_ERR_BAD_URI) << u;
        EXPECT_EQ(session, nullptr) << u;
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the DLL path must be absolute, like "
                     "C:\\path\\to\\vendor.dll") << u;
    }
    /* Drive paths with either separator still reach the loader. (A UNC
     * path would too; a miss there costs a network lookup.) */
    for (const char* u : {"j2534://C:/ecuio-not-a-real-dir/x.dll",
                          "j2534://C:\\ecuio-not-a-real-dir\\x.dll"}) {
        EXPECT_EQ(ecuio_session_create(u, &session), ECUIO_ERR_LOAD_FAILED) << u;
        EXPECT_EQ(session, nullptr) << u;
    }
}

TEST(J2534StreamTest, DirectLoadUriDeliversInjectedFramesExactly)
{
    /* A passive profile that declares no channel (one plain def, no
     * requests) and no ?filter=: frames dispatch verbatim, id 0, with no
     * scheduler writes interfering with exact-count assertions. Raw
     * listens on the engine ECU instead (RawListensToTheEngineEcu). */
    static ecuio_signal_def_t defs[1];
    defs[0] = ecuio_signal_def_t{};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].message_id = 0x123;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    static ecuio_signal_set_t set;
    set = ecuio_signal_set_t{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-passive-verbatim", &set, nullptr),
              ECUIO_OK);
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-passive-verbatim",
             FAKE_J2534_DLL_PATH);
    ASSERT_NE(fake_page(), nullptr);
    fake_control::reset(fake_page());

    const auto before_create = std::chrono::steady_clock::now();
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    EXPECT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);

    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    const fn_inject_frame inject = page_inject;

    const unsigned char f0[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};
    const unsigned char f1[8] = {0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0};
    const unsigned char f2[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};

    /* Vendor stamps an hour into the adapter's clock, as a real one's
     * can be at connect. */
    inject(f0, 8, 4000000000ul, 0, 1);   /* standard */
    inject(f1, 8, 4000001000ul, 1, 1);   /* EXTENDED: exercises the 29-bit path */
    inject(f2, 8, 4000002000ul, 0, 1);   /* standard */

    /* Worker polls at 20ms; budget generously, assert EXACTLY 3:
     * a fourth would mean the fake fabricated traffic. */
    for (int i = 0; i < 200 && cap.count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(cap.count.load(), 3);

    /* Payload fidelity in arrival order (FIFO preserved) */
    ASSERT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[1], f1, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[2], f2, 8), 0);

    /* Flag translation: fake's RxStatus bit -> frame flags word; an ISO
     * 15765 channel's message is the adapter's reassembly */
    EXPECT_EQ(cap.frame_flags[0], ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_EQ(cap.frame_flags[1],
              ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_EQ(cap.frame_flags[2], ECUIO_FRAME_FLAG_REASSEMBLED);

    /* No channel on this session: frames dispatch VERBATIM with id 0,
     * pinned so the channel path can't silently change this one. */
    EXPECT_EQ(cap.arbitration_ids[0], 0u);
    EXPECT_EQ(cap.arbitration_ids[1], 0u);
    EXPECT_EQ(cap.arbitration_ids[2], 0u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);

    /* Session time (j2534_clock.h): the first frame the host's since
     * attach, not the vendor's hour; then the vendor's 1000 us steps. */
    const uint64_t since_create = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - before_create).count());
    EXPECT_LE(cap.timestamps[0], since_create);
    EXPECT_EQ(cap.timestamps[1] - cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[2] - cap.timestamps[1], uint64_t{1000});

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);     /* joins worker */
    ecuio_session_destroy(session);
    fake_control::reset(fake_page());
}

TEST(J2534StreamTest, StartIsIdempotent)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST(J2534StreamTest, UnknownVendorNameRejected)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("j2534://no-such-vendor-xyz",
                                   &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

} // namespace

#endif // _WIN32
