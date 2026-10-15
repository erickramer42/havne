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
//          Adoption tests (Part 5): ?filter=/?txid=/?poll= — filter
//          registration shape, request framing, header strip + ID
//          stamp, stop-on-detach, gating, passive receive.

#include <gtest/gtest.h>

#include "qttune/j2534.h"
#include "qttune/core.h"
#include "qttune/j2534_ptypes.h"

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
    unsigned short payload_lens[8]{};   /* post-strip length (Part 5) */
    unsigned int frame_flags[8]{};      /* F1: QTTUNE_FRAME_FLAG_* */
    uint32_t arbitration_ids[8]{};      /* F1 identity stamp (Part 5) */
    uint64_t timestamps[8]{};           /* pass-through contract */
};

void capture_cb(const qttune_frame_t* frame, void* user_data)
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

    qttune_status_t start_session_with_uri(const char* uri)
    {
        qttune_session_t* session = nullptr;
        qttune_status_t rc = qttune_session_create(uri, &session);
        if (rc != QT_OK) {
            return rc;
        }
        rc = qttune_session_start(session);
        /* Connect happened at start; observe params before teardown. */
        unsigned long baud = 0, flags = 0, proto = 0;
        get_params(&baud, &flags, &proto);
        last_baud = baud;
        last_flags = flags;
        last_protocol = proto;
        qttune_session_close(session);
        qttune_session_destroy(session);
        return rc;
    }

    unsigned long last_baud = 0;
    unsigned long last_flags = 0;
    unsigned long last_protocol = 0;
};

TEST_F(J2534QueryParamTest, DefaultConnectUsesIso15765At500k)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    /* Unset baud (sentinel 0) resolves to the ISO 15765 500k default;
     * flags default 0. This is the behavior-preservation proof for the
     * existing three stream tests, which do not pass a query. */
    EXPECT_EQ(last_protocol, QTTUNE_PT_PROTOCOL_ISO15765);
    EXPECT_EQ(last_baud, QTTUNE_PT_BAUD_ISO15765_500K);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, BaudParameterReachesPassThruConnect)
{
    char uri[600];
    snprintf(uri, sizeof(uri), "j2534://%s?baud=250000", FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    EXPECT_EQ(last_baud, 250000u);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, FlagsAcceptHexAndDecimalAndCombineWithBaud)
{
    char uri[600];

    snprintf(uri, sizeof(uri), "j2534://%s?baud=125000&flags=0x20",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    EXPECT_EQ(last_baud, 125000u);
    EXPECT_EQ(last_flags, 0x20u);

    /* Decimal flags, reversed order, last-wins duplicate */
    snprintf(uri, sizeof(uri),
             "j2534://%s?flags=10&baud=500000&baud=250000",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    EXPECT_EQ(last_baud, 250000u);
    EXPECT_EQ(last_flags, 10u);
}

TEST_F(J2534QueryParamTest, MalformedQueriesRejectedAtSessionCreate)
{
    qttune_session_t* session = nullptr;

    /* Selector-miss still wins over a good query */
    EXPECT_EQ(qttune_session_create("j2534://nonexistent-vendor?baud=250000",
                                    &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);

    /* Query splits from path; a nonexistent path fails AT LOAD —
     * proves the filename never got '?baud=...' glued onto it. */
    EXPECT_EQ(qttune_session_create("j2534://C:\\does\\not\\exist.dll?baud=250000",
                                    &session),
              QT_ERR_LOAD_FAILED);
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
    };
    for (const char* q : bad_queries) {
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(qttune_session_create(uri, &session), QT_ERR_NO_TRANSPORT)
            << "query: " << q;
        EXPECT_EQ(session, nullptr) << "query: " << q;
    }

    EXPECT_EQ(qttune_session_create("j2534://junk", &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

TEST(J2534StreamTest, DirectLoadUriDeliversInjectedFramesExactly)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    EXPECT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);

    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    /* F1 flag translation: fake's RxStatus bit -> frame flags word.
     * Covered both directions for the first time — previously this
     * mapping had zero coverage because the fake always sent 0. */
    EXPECT_EQ(cap.frame_flags[0], 0u);
    EXPECT_EQ(cap.frame_flags[1], QTTUNE_FRAME_FLAG_EXTENDED);
    EXPECT_EQ(cap.frame_flags[2], 0u);

    /* No filter on this URI: frames dispatch VERBATIM with id 0 —
     * the pre-adoption contract, pinned so the adoption path can't
     * silently change the legacy one. */
    EXPECT_EQ(cap.arbitration_ids[0], 0u);
    EXPECT_EQ(cap.arbitration_ids[1], 0u);
    EXPECT_EQ(cap.arbitration_ids[2], 0u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);

    /* Timestamps pass through UNTRANSLATED — documented v0.3.0
     * decision (vendor-defined units, see j2534_ptypes.h). These
     * asserts pin that decision so a future "helpful" normalization
     * can't land silently. */
    EXPECT_EQ(cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[1], uint64_t{2000});
    EXPECT_EQ(cap.timestamps[2], uint64_t{3000});

    EXPECT_EQ(qttune_session_close(session), QT_OK);   /* joins worker */
    qttune_session_destroy(session);
    FreeLibrary(mod);
}

TEST(J2534StreamTest, StartIsIdempotent)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    EXPECT_EQ(qttune_session_start(session), QT_OK);
    EXPECT_EQ(qttune_session_start(session), QT_OK);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

TEST(J2534StreamTest, UnknownVendorNameRejected)
{
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("j2534://no-such-vendor-xyz",
                                    &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

/* ====================================================================
 * Part 5 adoption suite — ?filter= / ?txid= / ?poll=
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
};

/* REQUEST LOOP: ?filter=0x7E8&poll=0x0C — the worker writes a Mode-01
 * request each 100 ms cycle; the fake echoes it back with the filter
 * pattern prefixed; the worker strips the 4-byte address header and
 * stamps arbitration_id. Asserts BOTH directions of the loop. */
TEST_F(J2534AdoptionTest, RequestLoopWritesMode01AndReceivesStrippedEcho)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    ASSERT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    /* ---- TX framing: 12 wire bytes, recorded by the fake ---- */
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wlen, 12u);
    /* First 4 bytes = tx_id BE (0x7E0) */
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xE0u);
    /* Byte 4 = SF PCI (02), byte 5 = Mode 01, byte 6 = PID (0C) */
    EXPECT_EQ(wbuf[4], 0x02u); EXPECT_EQ(wbuf[5], 0x01u);
    EXPECT_EQ(wbuf[6], 0x0Cu);

    /* ---- RX framing: 4-byte header stripped, 8 bytes payload ---- */
    /* Echo = [7E8][42 01 0C 00 00 00 00 00]; strip removes the 4-byte
     * ID, payload[0] is the +0x40'd first data byte (0x02 -> 0x42).
     * The +0x40 is FAKE-DEVICE semantics, not OBD correctness — a
     * real ECU answers 01 0C with 41 0C (service+0x40 on the service
     * byte); don't port the 0x42 expectation to BENCH. */
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payloads[0][0], 0x42u);   /* SF PCI + 0x40 (device model) */
    EXPECT_EQ(cap.payloads[0][1], 0x01u);   /* Mode 01 passes through */
    EXPECT_EQ(cap.payloads[0][2], 0x0Cu);   /* PID passes through */
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* FILTER REGISTERED ON START: verify StartMsgFilter called with
 * FLOW_CONTROL_FILTER, mask=FF...FF 00...00 (8 bytes), pattern=[rx_id BE]. */
TEST_F(J2534AdoptionTest, FilterRegisteredOnSessionStart)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    ASSERT_EQ(qttune_session_start(session), QT_OK);

    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);

    EXPECT_NE(fid, 0u);
    EXPECT_EQ(ftype, QTTUNE_PT_FLOW_CONTROL_FILTER);
    EXPECT_EQ(len, 8u);   /* 8-byte filter message (FC PCI) */
    /* First 4 mask bytes = FF (ID match), rest = 0 */
    EXPECT_EQ(mask[0], 0xFFu); EXPECT_EQ(mask[1], 0xFFu);
    EXPECT_EQ(mask[2], 0xFFu); EXPECT_EQ(mask[3], 0xFFu);
    EXPECT_EQ(mask[4], 0x00u); EXPECT_EQ(mask[5], 0x00u);
    EXPECT_EQ(mask[6], 0x00u); EXPECT_EQ(mask[7], 0x00u);
    /* Pattern = rx_id BE */
    EXPECT_EQ(pattern[0], 0x00u); EXPECT_EQ(pattern[1], 0x00u);
    EXPECT_EQ(pattern[2], 0x07u); EXPECT_EQ(pattern[3], 0xE8u);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* DETACH ORDERING: StopMsgFilter called exactly once with registered ID
 * BEFORE disconnect. The fake's GetLastFilterStop records the invocation. */
TEST_F(J2534AdoptionTest, DetachStopsFilterBeforeDisconnect)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    ASSERT_EQ(qttune_session_start(session), QT_OK);

    unsigned long registered_fid = 0, dummy_ftype = 0, dummy_len = 0;
    unsigned char dummy_mask[12], dummy_pattern[12];
    get_last_filter(&registered_fid, &dummy_ftype,
                    dummy_mask, dummy_pattern, &dummy_len);

    /* Stop count should be zero before close */
    unsigned long stop_fid = 0;
    get_last_stop(&stop_fid);
    EXPECT_EQ(stop_fid, 0u);

    /* Close invokes detach, which should stop the filter */
    EXPECT_EQ(qttune_session_close(session), QT_OK);

    get_last_stop(&stop_fid);
    EXPECT_EQ(stop_fid, registered_fid);

    qttune_session_destroy(session);
}

/* COUPLING RULE: ?poll= without ?filter= rejected at parse time */
TEST_F(J2534AdoptionTest, PollWithoutFilterRejectedAtParse)
{
    qttune_session_t* session = nullptr;
    char uri[700];

    snprintf(uri, sizeof(uri), "j2534://%s?poll=0x0C", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(qttune_session_create(uri, &session), QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

/* PASSIVE RECEIVE: ?filter=0x7E8 without ?poll= — filter registers,
 * no writes generated, frames still stripped and stamped. */
TEST_F(J2534AdoptionTest, PassiveReceiveWithFilterNoPoll)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    EXPECT_EQ(qttune_register_frame_callback(session, capture_cb, &cap), QT_OK);

    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    /* Header stripped: payload starts at index 0, length = 4 */
    EXPECT_EQ(cap.payloads[0][0], 0x02u);
    EXPECT_EQ(cap.payloads[0][1], 0x01u);
    EXPECT_EQ(cap.payloads[0][2], 0x0Cu);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 8u);
    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* FILTER GATING: SetFilterGating(1) makes ReadMsgs drop non-matching
 * frames. The fake drains them (not head-of-line blocking). */
TEST_F(J2534AdoptionTest, FilterGatingDropsNonMatchingFrames)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    ASSERT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* TXID OVERRIDE: ?txid=0x7DF for broadcast, default is 0x7E0 */
TEST_F(J2534AdoptionTest, TxidOverrideChangesRequestTarget)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?filter=0x7E8&poll=0x0C&txid=0x7DF",
             FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    ASSERT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

    wait_for_count(cap, 3, 100);

    /* Check recorded write uses 0x7DF not 0x7E0 */
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    EXPECT_EQ(wlen, 12u);
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xDFu);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

} // namespace

#endif /* _WIN32 */
