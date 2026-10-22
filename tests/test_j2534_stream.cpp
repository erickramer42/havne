// File/layer: tests/test_j2534_stream.cpp | GTest, HARNESS tier
// Testing: End-to-end j2534:// session: URI routing -> attach ->
//          connect -> worker poll -> InjectFrame -> ReadMsgs ->
//          callback dispatch. Exact-count assertions for INJECTED
//          frames (the fake emits nothing we did not inject).
//          Request-loop assertions are floors with a bounded budget:
//          that producer is a TIME-DRIVEN worker, not a test action,
//          so an exact count would encode a race, not a contract.
//          Query-param tests: ?baud=/?flags= values observed at
//          PassThruConnect via the fake's GetLastConnectParams hook;
//          malformed queries rejected at session_create (no DLL load).
//          Adoption tests (ADR-0012): ?filter=/?txid=/?poll= — filter
//          registration shape, request framing, header strip + ID
//          stamp, stop-on-detach, gating, passive receive.
//          ADR-0013: j2534 defaults to the obd2 profile (scheduler +
//          mux decode). Raw-contract tests opt out with ?profile=raw;
//          the obd2 block covers rotation, coupling, and drain.
//          Link health (Known Issue #11), in-proc: hard and quiet
//          reads, episode counting, recovery, untouched vendor counts,
//          the write latch and its priority, unmatched frames, backoff
//          growth and its capped step; plus ?poll= precedence over the
//          profile's rotation.

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "j2534_backoff.h"   /* core/src: the worker's backoff step */

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#endif

#ifdef _WIN32

namespace {

/* ---- fake-DLL hook signatures ---------------------------------------
 * EVERY hook a suite resolves needs a typedef HERE, inside the
 * namespace, before first use. Appending tests that skip this step
 * produces a wall of undeclared-identifier errors (C2065/C3646) —
 * which is exactly what happened when the adoption block landed
 * outside this scope with five signatures never declared. */
typedef void (*fn_inject_frame)(const unsigned char*, unsigned long,
                                unsigned long, unsigned long, unsigned long);
typedef void (*fn_get_connect_params)(unsigned long*, unsigned long*,
                                      unsigned long*);
typedef void (*fn_set_filter_gating)(int);
typedef void (*fn_get_write_count)(unsigned long*);
typedef void (*fn_get_recorded_write)(unsigned long, unsigned char*,
                                      unsigned long*, unsigned long*);
typedef void (*fn_get_last_filter_start)(unsigned long*, unsigned long*,
                                         unsigned char*, unsigned char*,
                                         unsigned long*);
typedef void (*fn_get_last_filter_stop)(unsigned long*);

struct StreamCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][8]{};
    unsigned short payload_lens[8]{};   /* post-strip length */
    unsigned int frame_flags[8]{};      /* ECUIO_FRAME_FLAG_* */
    uint32_t arbitration_ids[8]{};      /* identity stamped by the transport */
    uint64_t timestamps[8]{};           /* pass-through contract */
};

void capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    StreamCapture* cap = static_cast<StreamCapture*>(user_data);
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        const unsigned short n = (frame->data_length < 8)
                                 ? frame->data_length : 8;
        memcpy(cap->payloads[i], frame->data, n);
        cap->payload_lens[i] = frame->data_length;
        cap->frame_flags[i] = static_cast<unsigned int>(frame->flags);
        cap->arbitration_ids[i] = frame->arbitration_id;
        cap->timestamps[i] = frame->timestamp_us;
    }
}

/* Shared helper: create + start a session on a URI, resolve the
 * GetLastConnectParams hook from the (already-loaded) fake module. */
class J2534QueryParamTest : public ::testing::Test {
protected:
    HMODULE mod = nullptr;
    fn_get_connect_params get_params = nullptr;

    void SetUp() override
    {
        mod = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
        ASSERT_NE(mod, nullptr);
        get_params = reinterpret_cast<fn_get_connect_params>(
            GetProcAddress(mod, "GetLastConnectParams"));
        ASSERT_NE(get_params, nullptr);
    }

    void TearDown() override
    {
        if (mod != nullptr) {
            FreeLibrary(mod);
        }
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
    static const ecuio_channel_decl_t kWide[] = {{0x18DAF110u, 0x18DA10F1u}};
    static const ecuio_channel_decl_t kMixed[] = {{0x7E8u, 0x7E0u},
                                               {0x18DAF110u, 0x18DA10F1u}};
    static ecuio_signal_set_t wide = {};
    wide.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    wide.channel_count = 1;
    wide.channels = kWide;
    static ecuio_signal_set_t mixed = {};
    mixed.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    mixed.channel_count = 2;
    mixed.channels = kMixed;
    ASSERT_EQ(ecuio_profile_register("test-29bit", &wide), ECUIO_OK);
    ASSERT_EQ(ecuio_profile_register("test-mixed-width", &mixed), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-29bit",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), ECUIO_OK);
    EXPECT_EQ(last_flags, ECUIO_PT_CAN_29BIT_ID);

    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-mixed-width",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_NO_TRANSPORT);
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

    /* Query splits from path; a nonexistent path fails AT LOAD —
     * proves the filename never got '?baud=...' glued onto it. */
    EXPECT_EQ(ecuio_session_create("j2534://C:\\does\\not\\exist.dll?baud=250000",
                                   &session),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(session, nullptr);

    /* Malformed queries must reject AT PARSE, before any DLL load —
     * asserted against the REAL fake path so a load success would
     * prove the parse wrongly accepted the query. */
    char uri[600];
    const char* bad_queries[] = {
        "?baud=abc",
        "?baud=0",
        "?",
        "?band=250000",
        "?baud=250000&",
        "?baud=250000&&flags=1",
        "?poll=",
        "?poll=0x100",
        "?poll=-1",
    };
    for (const char* q : bad_queries) {
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_NO_TRANSPORT)
            << "query: " << q;
        EXPECT_EQ(session, nullptr) << "query: " << q;
    }

    EXPECT_EQ(ecuio_session_create("j2534://junk", &session),
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

TEST(J2534StreamTest, DirectLoadUriDeliversInjectedFramesExactly)
{
    char uri[512];
    /* ?profile=raw: legacy verbatim passive mode — id-0 dispatch,
     * no scheduler writes interfering with exact-count assertions. */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    EXPECT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);

    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Map our own reference for the hook — same module instance
     * core loaded, so the queue state is shared. */
    HMODULE mod = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    ASSERT_NE(mod, nullptr);
    auto inject = reinterpret_cast<fn_inject_frame>(
        GetProcAddress(mod, "InjectFrame"));
    ASSERT_NE(inject, nullptr);

    const unsigned char f0[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};
    const unsigned char f1[8] = {0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0};
    const unsigned char f2[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};

    inject(f0, 8, 1000, 0, 1);   /* standard */
    inject(f1, 8, 2000, 1, 1);   /* EXTENDED — exercises the 29-bit path */
    inject(f2, 8, 3000, 0, 1);   /* standard */

    /* Worker polls at 20ms; budget generously, assert EXACTLY 3 —
     * a fourth would mean the fake fabricated traffic. */
    for (int i = 0; i < 200 && cap.count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(cap.count.load(), 3);

    /* Payload fidelity in arrival order (FIFO preserved) */
    ASSERT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[1], f1, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[2], f2, 8), 0);

    /* Flag translation: fake's RxStatus bit -> frame flags word. */
    EXPECT_EQ(cap.frame_flags[0], 0u);
    EXPECT_EQ(cap.frame_flags[1], ECUIO_FRAME_FLAG_EXTENDED);
    EXPECT_EQ(cap.frame_flags[2], 0u);

    /* No filter on this URI: frames dispatch VERBATIM with id 0 —
     * the pre-adoption contract, pinned so the adoption path can't
     * silently change the legacy one. */
    EXPECT_EQ(cap.arbitration_ids[0], 0u);
    EXPECT_EQ(cap.arbitration_ids[1], 0u);
    EXPECT_EQ(cap.arbitration_ids[2], 0u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);

    /* Timestamps pass through UNTRANSLATED — documented v0.3.0
     * decision (vendor-defined units, see j2534_ptypes.h). */
    EXPECT_EQ(cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[1], uint64_t{2000});
    EXPECT_EQ(cap.timestamps[2], uint64_t{3000});

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);     /* joins worker */
    ecuio_session_destroy(session);
    FreeLibrary(mod);
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

/* ====================================================================
 * Adoption suite (ADR-0012) — ?filter= / ?txid= / ?poll=. Every URI
 * opts out of the obd2 default with ?profile=raw, so these pin the
 * raw-profile contract.
 * ==================================================================== */

class J2534AdoptionTest : public ::testing::Test {
protected:
    HMODULE mod = nullptr;
    fn_inject_frame inject = nullptr;
    fn_set_filter_gating set_gating = nullptr;
    fn_get_write_count get_write_count = nullptr;
    fn_get_recorded_write get_recorded_write = nullptr;
    fn_get_last_filter_start get_last_filter = nullptr;
    fn_get_last_filter_stop get_last_stop = nullptr;

    void SetUp() override
    {
        mod = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
        ASSERT_NE(mod, nullptr);
        inject = reinterpret_cast<fn_inject_frame>(
            GetProcAddress(mod, "InjectFrame"));
        ASSERT_NE(inject, nullptr);
        set_gating = reinterpret_cast<fn_set_filter_gating>(
            GetProcAddress(mod, "SetFilterGating"));
        ASSERT_NE(set_gating, nullptr);
        get_write_count = reinterpret_cast<fn_get_write_count>(
            GetProcAddress(mod, "GetWriteCount"));
        ASSERT_NE(get_write_count, nullptr);
        get_recorded_write = reinterpret_cast<fn_get_recorded_write>(
            GetProcAddress(mod, "GetRecordedWrite"));
        ASSERT_NE(get_recorded_write, nullptr);
        get_last_filter = reinterpret_cast<fn_get_last_filter_start>(
            GetProcAddress(mod, "GetLastFilterStart"));
        ASSERT_NE(get_last_filter, nullptr);
        get_last_stop = reinterpret_cast<fn_get_last_filter_stop>(
            GetProcAddress(mod, "GetLastFilterStop"));
        ASSERT_NE(get_last_stop, nullptr);
    }

    void TearDown() override
    {
        /* Gating is module-global state: never leak it into the next
         * test, even on a failing assertion. */
        if (set_gating != nullptr) {
            set_gating(0);
        }
        if (mod != nullptr) {
            FreeLibrary(mod);
        }
    }

    static void wait_for_count(StreamCapture& cap, int target, int iters)
    {
        for (int i = 0; i < iters && cap.count.load() < target; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* Obd2/scheduler variant: wait for the WORK side instead of the
     * receive side — rotation order is asserted from GetRecordedWrite
     * indices, so writes are the primary observable. */
    static void wait_for_writes(fn_get_write_count get_wc,
                                unsigned long target, int iters)
    {
        unsigned long wc = 0;
        for (int i = 0; i < iters; ++i) {
            get_wc(&wc);
            if (wc >= target) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

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

    wait_for_count(cap, 3, 100);   /* ~1 s budget for 3 x 100 ms cycles */

    const int received = cap.count.load();
    unsigned long wc = 0;
    get_write_count(&wc);

    /* Time-driven producer: assert the floor, not an exact count. */
    EXPECT_GE(received, 3);
    EXPECT_GE(wc, 3u);
    /* Every echo is CAUSED by a write and the worker reads at most one
     * frame per cycle, so writes >= received with at most a couple in
     * flight. A runaway write loop blows this delta up. */
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
    /* FC is the bare tester ID, same 4-byte shape — not exposed by
     * GetLastFilterStart; equal-length passing is the seam under test. */

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* DETACH ORDERING: StopMsgFilter called exactly once with registered ID
 * BEFORE disconnect. The fake's GetLastFilterStop records the invocation. */
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
 * profile declares none, so this is rejected — at ATTACH, not at
 * parse, because the rule consults the profile's channels, which
 * parse_query can't see. Still before any DLL load. */
TEST_F(J2534AdoptionTest, PollWithoutChannelRejectedBeforeDllLoad)
{
    ecuio_session_t* session = nullptr;
    char uri[700];

    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&poll=0x0C", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

/* PASSIVE RECEIVE: ?profile=raw&filter=0x7E8 without ?poll= — filter
 * registers, no writes generated, frames still stripped and stamped. */
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

    /* Wait for read → strip → dispatch */
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

/* FILTER GATING: SetFilterGating(1) makes ReadMsgs drop non-matching
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

    /* Arm gating on the fake (module-global; TearDown disarms) */
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

    /* Exactly the 2 matching frames — the non-matching one is DRAINED
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
    /* SLICE 2 FLIP: j2534:// with no override selects the obd2 set —
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
              ECUIO_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

// ---------------------------------------------------------------------------
// ADR-0013: obd2 scheduler mode — rotation, coupling, drain
// ---------------------------------------------------------------------------

/* SCHEDULER ROTATION: ?profile=obd2&filter=0x7E8 — the worker
 * requests PIDs round-robin in set order. Rotation order is asserted
 * from the fake's recorded WRITES (deterministic, one per cycle),
 * not from received frames. */
TEST_F(J2534AdoptionTest, Obd2SchedulerRotatesPidsInSetOrder)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* 3 cycles = 300 ms minimum; budget 2 s for CI variance. */
    wait_for_writes(get_write_count, 3, 200);

    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;

    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[4], 0x01u);
    EXPECT_EQ(wbuf[5], 0x04u);               /* PID: calculated load */
    get_recorded_write(1, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[5], 0x05u);               /* coolant */
    get_recorded_write(2, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[5], 0x0Cu);              /* RPM */
    wait_for_writes(get_write_count, 11, 250);
    /* The rotation follows kObd2Defs order. With 10 defs, MAF (0x10) and
     * Oil Temp (0x5C), the last two, take writes 8 and 9, and write 10 wraps
     * to PID 0x04. */
    get_recorded_write(8, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[5], 0x10u);               /* MAF */
    get_recorded_write(9, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[5], 0x5Cu);               /* Oil Temp */
    get_recorded_write(10, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[5], 0x04u);              /* wrapped — round-robin */

    /* RX: each cycle writes one request then drains its response in
     * the same cycle, so the first three received frames answer the
     * first three requests in order. */
    wait_for_count(cap, 3, 100);
    ASSERT_GE(cap.count.load(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(cap.arbitration_ids[i], 0x7E8u);
        EXPECT_EQ(cap.payloads[i][0], 0x41u);
    }
    EXPECT_EQ(cap.payloads[0][1], 0x04u);
    EXPECT_EQ(cap.payloads[1][1], 0x05u);
    EXPECT_EQ(cap.payloads[2][1], 0x0Cu);
    EXPECT_EQ(cap.payloads[2][2], 0x1Fu);
    EXPECT_EQ(cap.payloads[2][3], 0x40u);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* FLAGSHIP: a bare j2534:// URI derives its channel from the obd2
 * profile — filter pattern 7E8, requests to 7E0, scheduler running.
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

    /* Derived tx: requests go to 7E0, rotation starts at PID 0x04 */
    wait_for_writes(get_write_count, 3, 200);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xE0u);
    EXPECT_EQ(wbuf[4], 0x01u);
    EXPECT_EQ(wbuf[5], 0x04u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* OVERRIDE SEMANTICS: a lone ?txid= under the obd2 default keeps
 * the derived 7E8 filter but rewrites the request target — the
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

/* DRAIN BUDGET: inject a backlog LARGER than the per-cycle drain
 * budget (10) and assert every frame is eventually delivered —
 * leftover frames survive into the next cycle's drain instead of
 * being dropped or head-of-line-blocking the scheduler responses.
 * Uses a local 32-slot capture: the shared StreamCapture's 8 slots
 * would truncate under scheduler traffic + backlog combined. */
TEST_F(J2534AdoptionTest, SchedulerDrainsBacklogBeyondPerCycleBudget)
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

    /* 12 frames / ~10 per cycle -> ~2 cycles = 200 ms min; budget
     * 4 s for CI variance (responses consume drain slots too). */
    for (int i = 0; i < 400 && !seen_all_markers(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(seen_all_markers());

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ====================================================================
 * Link health (#11 HARNESS): drive ECUIO_FAKE_READ_STATUS through the
 * real worker and assert the error-state contract — canonical codes,
 * one episode per outage, quiet debounce, recovery via the continued
 * request probe. In-proc fake: the env knob re-reads per ReadMsgs
 * call, so arming/clearing is immediate.
 * ==================================================================== */

/* Clears BOTH knobs on scope exit — a leaked ECUIO_FAKE_WRITE_STATUS
 * fails every write of the NEXT test in the same process (a direct
 * binary run; ctest isolates each case), and the write-count pins read
 * it as "probes stopped". ReadStatusArm is the narrower guard for
 * read-knob-only tests. */
struct KnobsArm {
    ~KnobsArm()
    {
        SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", nullptr);
        SetEnvironmentVariableA("ECUIO_FAKE_WRITE_STATUS", nullptr);
    }
};

TEST_F(J2534AdoptionTest, HardFailureSetsCanonicalErrorAndRecovers)
{
    KnobsArm arm;   // restore even on a failing ASSERT
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Dead adapter: STATUS_FAILED (0x07) on every read */
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "7");

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {   /* 5 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "hard failure never reported (was " << le << ")";
    EXPECT_EQ(ec, 1u);   /* one episode per outage, not per read */

    /* Still exactly one episode while the outage persists — episode
     * counting is transition-based; the backoff may be doubling under
     * us but must not re-arm the counter. */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(ec, 1u);

    /* Clear the knob; scheduler writes continue as the recovery probe,
     * so a good frame follows and the link heals. The backoff
     * may have grown toward the cap, so budget generously. */
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", nullptr);
    reached = false;
    for (int i = 0; i < 1000 && !reached; ++i) {   /* 10 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_OK);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "recovery never reported (was " << le << ")";
    EXPECT_EQ(ec, 1u);   /* recovery adds no episode */

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

TEST_F(J2534AdoptionTest, QuietBusDebouncesToVehicleTimeoutWithoutEpisode)
{
    KnobsArm arm;
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Quiet vehicle: ERR_TIMEOUT (0x09) on every read, nothing else */
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "9");

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 800 && !reached; ++i) {   /* 8 s: debounce ~1 s + CI */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_VEHICLE_TIMEOUT);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "quiet debounce never fired (was " << le << ")";
    EXPECT_EQ(ec, 0u);   /* vehicle silence is not an episode */

    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", nullptr);
    reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {   /* 5 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_OK);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Count-trust rule, in-proc half: a dirty count under a hard error must
 * NOT read as health. The loader requests exactly one message; the
 * knob leaves num == 1 over a zero-filled PASSTHRU_MSG, which without
 * ecuio_trusted_count would dispatch as a real frame and report the
 * link healthy. Bare URI: the scheduler keeps writing, so recovery to
 * OK is reachable (passive mode would correctly hold VEHICLE_TIMEOUT
 * via the quiet debounce — recovery and quiet are different states). */
TEST_F(J2534AdoptionTest, UntouchedCountUnderHardErrorReportsUnhealthy)
{
    struct Arm {
        ~Arm() {
            SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS",
                                     nullptr);
        }
    } arm;

    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", "7");

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {   /* 5 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "dead adapter reported healthy (was " << le << ")";
    EXPECT_GE(ec, 1u);
    /* PHANTOM CHECK, taken BEFORE clearing the knob: scheduler writes
     * have parked responses in the fake's queue, but the poisoned
     * reads must dispatch nothing — a positive count would be phantom
     * frames from the dirty count. */
    EXPECT_EQ(cap.count.load(), 0);

    /* Recovery: writes continued as the probe, responses flush once
     * reads return honest counts. */
    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", nullptr);
    reached = false;
    for (int i = 0; i < 1000 && !reached; ++i) {   /* 10 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_OK);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "recovery never reported (was " << le << ")";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?poll= under a scheduler profile overrides the rotation. The worker
 * checks sched.count before poll_pid, so if attach built the set
 * scheduler anyway, ?poll=0x0C under obd2 would rotate the whole set and
 * silently drop the URI's PID. Five consecutive writes pin it:
 * rotation would give 0x04, 0x05, 0x0C, 0x0D, 0x0F. */
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

    wait_for_writes(get_write_count, 5, 200);   /* ~550 ms of 100 ms cycles */

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

/* ====================================================================
 * Health machine: write-result latch, hard-read priority,
 * unmatched-delivered frames, backoff growth. Same fixture and knob
 * idioms as the link-health tests above
 * (env knob re-read per call in-proc; RAII arm restores even on a
 * failing ASSERT).
 * ==================================================================== */

/* Findings 15 + 6: a failed write latches REQUEST_FAILED with ONE
 * episode; a subsequent successful write clears it immediately,
 * without needing a matched read (pinned by forcing reads quiet
 * during recovery: only the write can produce OK). The knob leaves
 * the write count untouched — the VX's "status 7, written 1" shape. */
TEST_F(J2534AdoptionTest, WriteFailureLatchesRequestFailedAndClears)
{
    KnobsArm arm;
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    SetEnvironmentVariableA("ECUIO_FAKE_WRITE_STATUS", "7");    /* reads clean */

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {   /* 5 s budget */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_REQUEST_FAILED);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "failed write never latched (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "write failure flapped episodes";

    /* Latch discipline: > 1.5 s in (past the ~1.2 s quiet debounce),
     * clean-but-empty reads must NOT have flipped the state to
     * VEHICLE_TIMEOUT — only a write result moves it. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_REQUEST_FAILED)
        << "quiet reads overrode the latched write failure";
    EXPECT_EQ(ec, 1u);

    /* Recovery with reads held QUIET (0x09): a successful write is
     * itself proof requests flow, so OK can only come from the write
     * success — no frame can be delivered to produce a matched read.
     * The backoff may sit near its 5 s cap; budget 12 s. */
    SetEnvironmentVariableA("ECUIO_FAKE_WRITE_STATUS", nullptr);
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "9");
    reached = false;
    for (int i = 0; i < 1200 && !reached; ++i) {
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_OK);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "successful write never cleared (was " << le << ")";
    EXPECT_EQ(ec, 1u) << "recovery added an episode";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Ordering: with writes AND reads both failing, the hard
 * READ failure outranks requests-failing. Both transitions are
 * sequential within the first cycle (write, then drain read), so
 * exactly two episodes: one REQUEST_FAILED, one HARD. While both
 * persist, the write path must not downgrade back. */
TEST_F(J2534AdoptionTest, HardReadFailureOutranksRequestFailed)
{
    KnobsArm arm;
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    SetEnvironmentVariableA("ECUIO_FAKE_WRITE_STATUS", "7");
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "7");

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "hard read failure never reported (was " << le << ")";
    EXPECT_EQ(ec, 2u) << "expected write-then-read episodes, got " << ec;

    /* Hold: the continued failing writes must not downgrade the
     * hard-failure state or add episodes. */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    (void)ecuio_session_get_error_state(session, &le, &ec);
    EXPECT_EQ(le, ECUIO_ERR_TRANSPORT_HARD_FAILURE)
        << "write path downgraded a hard read failure";
    EXPECT_EQ(ec, 2u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A delivered-but-unmatched frame (header matches no channel) clears
 * hard failure and the backoff — the adapter answered — but
 * transitions to VEHICLE_TIMEOUT, not OK, and adds no episode.
 *
 * The 900 ms budget is the discriminator, so it is NOT generous by
 * accident: code that fully clears reaches OK (never TIMEOUT, fails);
 * code that treats the frame as plain quiet needs the ~1.2 s debounce
 * and misses the window. The passive URI keeps writes out of the picture entirely. */
TEST_F(J2534AdoptionTest, UnmatchedDeliveredFrameGoesQuietNotHealthy)
{
    KnobsArm arm;
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw&filter=0x7E8",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* Force hard failure, then deliver something the adapter DID read */
    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "7");

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached);
    EXPECT_EQ(ec, 1u);

    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", nullptr);
    /* Header 0x7D0 — no channel's rx_id (filter registered 0x7E8).
     * Gating is off, so the fake delivers it; the transport decides. */
    const unsigned char unmatched[12] = {
        0x00, 0x00, 0x07, 0xD0,
        0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0
    };
    inject(unmatched, sizeof(unmatched), 1000, 0, 1);

    reached = false;
    for (int i = 0; i < 90 && !reached; ++i) {   /* 900 ms: < debounce */
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_VEHICLE_TIMEOUT);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(reached) << "unmatched frame did not degrade to quiet (was "
                        << le << ")";
    EXPECT_EQ(ec, 1u) << "unmatched frame added an episode";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Backoff GROWS under sustained hard failure. Writes continue as the
 * recovery probe but the doubling backoff spaces them: ~5 probes in
 * 1.5 s (100+200+400+800 ms doublings), against ~15 if the backoff
 * reset to its base on every failure. Wide margins on purpose — the two
 * behaviors differ by 3x, so CI timing noise can't confuse them. */
TEST_F(J2534AdoptionTest, BackoffGrowsUnderSustainedHardFailure)
{
    KnobsArm arm;
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    SetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS", "7");    /* writes clean */

    int32_t le = 0;
    uint32_t ec = 0;
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {
        (void)ecuio_session_get_error_state(session, &le, &ec);
        reached = (le == ECUIO_ERR_TRANSPORT_HARD_FAILURE);
        if (!reached) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_TRUE(reached) << "hard failure never reported";

    unsigned long wc0 = 0;
    get_write_count(&wc0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    unsigned long wc1 = 0;
    get_write_count(&wc1);

    EXPECT_GE(wc1 - wc0, 1u) << "probe writes stopped entirely";
    EXPECT_LE(wc1 - wc0, 8u) << "backoff never grew (reset-to-base shape)";

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* The backoff step, without a session: from the base every wait grows
 * and none exceeds the cap, including the step that lands on it, and
 * at the cap it stays. The worker test above can't see this; reaching
 * the cap takes over 6 s of sustained failure. */
TEST(J2534StreamTest, BackoffStepDoublesToCapWithoutOvershoot)
{
    uint32_t ms = J2534_BACKOFF_BASE_MS;
    uint32_t steps = 0;
    while (ms < J2534_BACKOFF_CAP_MS) {
        const uint32_t next = j2534_backoff_next(ms);
        EXPECT_GT(next, ms);
        EXPECT_LE(next, J2534_BACKOFF_CAP_MS) << "overshoot after " << ms;
        ms = next;
        ASSERT_LT(++steps, 32u) << "never reached the cap";
    }
    EXPECT_EQ(ms, J2534_BACKOFF_CAP_MS);
    EXPECT_EQ(j2534_backoff_next(J2534_BACKOFF_CAP_MS), J2534_BACKOFF_CAP_MS);
    EXPECT_EQ(j2534_backoff_next(J2534_BACKOFF_CAP_MS - 1), J2534_BACKOFF_CAP_MS);
    EXPECT_EQ(j2534_backoff_next(UINT32_MAX), J2534_BACKOFF_CAP_MS)
        << "doubling must not wrap";
}

/* ECUIO_ERR_REQUEST_FAILED has a distinct, mapped string. */
TEST(J2534StreamTest, RequestFailedStatusHasDistinctString)
{
    const char* s = ecuio_status_string(ECUIO_ERR_REQUEST_FAILED);
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "unknown status");
    EXPECT_STRNE(s, ecuio_status_string(ECUIO_ERR_TRANSPORT_HARD_FAILURE));
}

} // namespace

#endif // _WIN32
