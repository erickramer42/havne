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
//          ADR-0013 SLICE 2: j2534 defaults to the obd2 profile
//          (scheduler + mux decode). Legacy-behavior tests opt out
//          with ?profile=raw; the obd2 block covers rotation,
//          coupling, and drain.

#include <gtest/gtest.h>

#include "qttune/j2534.h"
#include "qttune/core.h"
#include "qttune/j2534_ptypes.h"
#include "qttune/profile.h"
#include "qttune/signal_sets.h"

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
    unsigned short payload_lens[8]{};   /* post-strip length (Part 5) */
    unsigned int frame_flags[8]{};      /* QTTUNE_FRAME_FLAG_* */
    uint32_t arbitration_ids[8]{};      /* identity stamped by the transport */
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
    /* ?profile=raw: the slice-2 default is obd2 (scheduler mode);
     * this test pins the CONNECT defaults, so it opts out of the
     * scheduler to keep the legacy posture it has always asserted. */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    /* Unset baud (sentinel 0) resolves to the ISO 15765 500k default;
     * flags default 0. */
    EXPECT_EQ(last_protocol, QTTUNE_PT_PROTOCOL_ISO15765);
    EXPECT_EQ(last_baud, QTTUNE_PT_BAUD_ISO15765_500K);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, BaudParameterReachesPassThruConnect)
{
    char uri[600];
    snprintf(uri, sizeof(uri),
             "j2534://%s?baud=250000&profile=raw", FAKE_J2534_DLL_PATH);

    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    EXPECT_EQ(last_baud, 250000u);
    EXPECT_EQ(last_flags, 0u);
}

TEST_F(J2534QueryParamTest, FlagsAcceptHexAndDecimalAndCombineWithBaud)
{
    char uri[600];

    snprintf(uri, sizeof(uri),
             "j2534://%s?baud=125000&flags=0x20&profile=raw",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(start_session_with_uri(uri), QT_OK);
    EXPECT_EQ(last_baud, 125000u);
    EXPECT_EQ(last_flags, 0x20u);

    /* Decimal flags, reversed order, last-wins duplicate */
    snprintf(uri, sizeof(uri),
             "j2534://%s?flags=10&baud=500000&baud=250000&profile=raw",
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
    /* ?profile=raw: legacy verbatim passive mode — id-0 dispatch,
     * no scheduler writes interfering with exact-count assertions. */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

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

    /* Flag translation: fake's RxStatus bit -> frame flags word. */
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
     * decision (vendor-defined units, see j2534_ptypes.h). */
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
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

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
 * Part 5 adoption suite — ?filter= / ?txid= / ?poll=  (LEGACY paths;
 * every URI opts out of the obd2 default with ?profile=raw so these
 * keep pinning the raw-profile contract)
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
    EXPECT_EQ(wlen, 6u);                     /* adapter-framed, no PCI */
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xE0u);
    EXPECT_EQ(wbuf[4], 0x01u);               /* Mode 01 */
    EXPECT_EQ(wbuf[5], 0x0Cu);               /* PID */
    EXPECT_NE(wflags & QTTUNE_PT_TX_ISO15765_FRAME_PAD, 0u);  /* adapter pads */

    EXPECT_EQ(cap.arbitration_ids[0], 0x7E8u);
    EXPECT_EQ(cap.payloads[0][0], 0x41u);    /* mode echo at offset 0 */
    EXPECT_EQ(cap.payloads[0][1], 0x0Cu);
    EXPECT_EQ(cap.payloads[0][2], 0x1Fu);
    EXPECT_EQ(cap.payloads[0][3], 0x40u);
    EXPECT_EQ(static_cast<unsigned>(cap.payload_lens[0]), 4u);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

TEST_F(J2534AdoptionTest, FilterRegisteredOnSessionStart)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C",
             FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    ASSERT_EQ(qttune_session_start(session), QT_OK);

    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);

    EXPECT_NE(fid, 0u);
    EXPECT_EQ(ftype, QTTUNE_PT_FLOW_CONTROL_FILTER);
    EXPECT_EQ(len, 4u);                       /* N1: bare-ID filter msgs */
    EXPECT_EQ(mask[0], 0xFFu); EXPECT_EQ(mask[1], 0xFFu);
    EXPECT_EQ(mask[2], 0xFFu); EXPECT_EQ(mask[3], 0xFFu);
    EXPECT_EQ(pattern[0], 0x00u); EXPECT_EQ(pattern[1], 0x00u);
    EXPECT_EQ(pattern[2], 0x07u); EXPECT_EQ(pattern[3], 0xE8u);
    /* FC is the bare tester ID, same 4-byte shape — not exposed by
     * GetLastFilterStart; equal-length passing is the seam under test. */

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* DETACH ORDERING: StopMsgFilter called exactly once with registered ID
 * BEFORE disconnect. The fake's GetLastFilterStop records the invocation. */
TEST_F(J2534AdoptionTest, DetachStopsFilterBeforeDisconnect)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C",
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

    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&poll=0x0C", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(qttune_session_create(uri, &session), QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

/* PASSIVE RECEIVE: ?profile=raw&filter=0x7E8 without ?poll= — filter
 * registers, no writes generated, frames still stripped and stamped. */
TEST_F(J2534AdoptionTest, PassiveReceiveWithFilterNoPoll)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    EXPECT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);

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

    /* Header stripped: payload starts at index 0, length = 8 */
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
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8", FAKE_J2534_DLL_PATH);

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
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=raw&filter=0x7E8&poll=0x0C&txid=0x7DF",
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
    EXPECT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[0], 0x00u); EXPECT_EQ(wbuf[1], 0x00u);
    EXPECT_EQ(wbuf[2], 0x07u); EXPECT_EQ(wbuf[3], 0xDFu);
    EXPECT_EQ(wbuf[4], 0x01u);

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

// ---------------------------------------------------------------------------
// ADR-0013 slice 1: j2534 profile selection (slice 2 flips the default
// to obd2; ?profile= override rides the strict grammar through
// parse_query)
// ---------------------------------------------------------------------------

TEST(J2534StreamTest, DefaultProfileIsObd2)
{
    /* SLICE 2 FLIP: j2534:// with no override selects the obd2 set —
     * decode + multi-PID scheduler. Create/destroy without start: the
     * assertion is about selection, not streaming, so no filter
     * coupling fires (coupling lives in session_start). */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH;
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), qttune_obd2_signal_set());
    qttune_session_destroy(session);
}

TEST(J2534StreamTest, RawProfileRemainsSelectableOnJ2534)
{
    /* The escape hatch: log-only sessions on the real transport. */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=raw";
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), nullptr);
    qttune_session_destroy(session);
}

TEST(J2534StreamTest, ProfileOverrideMockSelectsMockSet)
{
    /* The override crosses the transport boundary: the SAME j2534
     * session can carry a named profile's set. Also proves parse_query
     * admits the profile key alongside every other strict key. */
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=mock";
    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri.c_str(), &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), qttune_mock_signal_set());
    qttune_session_destroy(session);
}

TEST(J2534StreamTest, UnknownProfileRejectedAtCreate)
{
    const std::string uri = std::string("j2534://") + FAKE_J2534_DLL_PATH
                          + "?profile=bogus";
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create(uri.c_str(), &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

// ---------------------------------------------------------------------------
// ADR-0013 slice 2: obd2 scheduler mode — rotation, coupling, drain
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

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    StreamCapture cap;
    ASSERT_EQ(qttune_register_frame_callback(session, capture_cb, &cap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

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
    wait_for_writes(get_write_count, 9, 250);
    get_recorded_write(8, wbuf, &wlen, &wflags);
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
    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* N3 FLAGSHIP: a bare j2534:// URI now derives its channel from the
 * obd2 profile — filter pattern 7E8, requests to 7E0, scheduler
 * running. The slice-2 footgun (bare URI -> NO_TRANSPORT at start,
 * after the device was already opened) is gone: ?filter= demotes to
 * an override. */
TEST_F(J2534AdoptionTest, BareJ2534UriDerivesObd2ChannelAndStarts)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);
    EXPECT_EQ(qttune_session_signal_set(session), qttune_obd2_signal_set());
    ASSERT_EQ(qttune_session_start(session), QT_OK);

    /* Derived filter: exact-match mask, pattern 00 00 07 E8 */
    unsigned long fid = 0, ftype = 0, len = 0;
    unsigned char mask[12] = {}, pattern[12] = {};
    get_last_filter(&fid, &ftype, mask, pattern, &len);
    EXPECT_NE(fid, 0u);
    EXPECT_EQ(ftype, QTTUNE_PT_FLOW_CONTROL_FILTER);
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

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

/* N3 OVERRIDE SEMANTICS: a lone ?txid= under the obd2 default keeps
 * the derived 7E8 filter but rewrites the request target — the
 * broadcast-request pattern (?txid=0x7DF) without restating the
 * channel. Mixed override + derived channel. */
TEST_F(J2534AdoptionTest, TxidOverrideRewritesDerivedTransmitTarget)
{
    char uri[512];
    snprintf(uri, sizeof(uri),
             "j2534://%s?txid=0x7DF", FAKE_J2534_DLL_PATH);

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
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

    qttune_session_t* session = nullptr;
    ASSERT_EQ(qttune_session_create(uri, &session), QT_OK);

    struct DrainCapture {
        std::atomic<int> count{0};
        unsigned char payloads[32][8]{};
        uint32_t ids[32]{};
    } dcap;
    auto drain_cb = [](const qttune_frame_t* frame, void* user_data) {
        DrainCapture* c = static_cast<DrainCapture*>(user_data);
        const int i = c->count.fetch_add(1);
        if (i < 32) {
            const unsigned short n = (frame->data_length < 8)
                                     ? frame->data_length : 8;
            memcpy(c->payloads[i], frame->data, n);
            c->ids[i] = frame->arbitration_id;
        }
    };
    ASSERT_EQ(qttune_register_frame_callback(session, drain_cb, &dcap),
              QT_OK);
    ASSERT_EQ(qttune_session_start(session), QT_OK);

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

    EXPECT_EQ(qttune_session_close(session), QT_OK);
    qttune_session_destroy(session);
}

} // namespace

#endif // _WIN32
