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
//          ADR-0013: j2534 defaults to the obd2 profile (its requests +
//          mux decode). Raw-contract tests opt out with ?profile=raw;
//          the obd2 block covers rotation, routing to each def's
//          channel, Service 0x22 and a backlog beside the replies, plus
//          ?poll= precedence over the profile's rotation. The probe
//          (ADR-0015 §4): the bitmap walk before the rotation, what the
//          bitmaps and a negative reply drop, ?probe=0, the walk resumed
//          after a quiet bus, and each channel's support report. The
//          query's split in create: unknown keys named with the ones
//          j2534:// takes, and ?probe='s malformed forms. The backoff's
//          capped step and the vendor clock's cases are here; the
//          request engine's pacing and the link-health machine run
//          through both backends in test_j2534_health.cpp.

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/core.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "j2534_backoff.h"   /* core/src: the worker's backoff step */
#include "j2534_clock.h"     /* core/src: vendor stamps to session time */

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
        "?probe=",
        "?probe=2",
        "?probe=00",
        "?probe=no",
        "?probe=0x0",
        "?probe=1&probe=",
        "?Probe=0",
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
        /* 17 pairs, one past the most a query holds */
        "?probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1&probe=1"
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
                 "filter, txid, poll, probe, profile and did");
    snprintf(uri, sizeof(uri), "j2534://%s?probe=yes", FAKE_J2534_DLL_PATH);
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
    EXPECT_STREQ(ecuio_last_error_detail(), "?probe= needs 0 or 1, not 'yes'");

    /* Both values are taken, with any profile. */
    for (const char* q : {"?profile=raw&probe=0", "?probe=1", "?probe=0&probe=1"}) {
        snprintf(uri, sizeof(uri), "j2534://%s%s", FAKE_J2534_DLL_PATH, q);
        EXPECT_EQ(start_session_with_uri(uri), ECUIO_OK) << "query: " << q;
    }
}

TEST(J2534StreamTest, DirectLoadUriDeliversInjectedFramesExactly)
{
    char uri[512];
    /* ?profile=raw: legacy verbatim passive mode — id-0 dispatch,
     * no scheduler writes interfering with exact-count assertions. */
    snprintf(uri, sizeof(uri), "j2534://%s?profile=raw",
             FAKE_J2534_DLL_PATH);

    const auto before_create = std::chrono::steady_clock::now();
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

    /* Vendor stamps an hour into the adapter's clock, as a real one's
     * can be at connect. */
    inject(f0, 8, 4000000000ul, 0, 1);   /* standard */
    inject(f1, 8, 4000001000ul, 1, 1);   /* EXTENDED — exercises the 29-bit path */
    inject(f2, 8, 4000002000ul, 0, 1);   /* standard */

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

    /* Session time (j2534_clock.h): the first frame the host's since
     * attach, not the vendor's hour; then the vendor's 1000 µs steps. */
    const uint64_t since_create = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - before_create).count());
    EXPECT_LE(cap.timestamps[0], since_create);
    EXPECT_EQ(cap.timestamps[1] - cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[2] - cap.timestamps[1], uint64_t{1000});

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

    /* Write i (the fake records the first 16) asked service for key. */
    void expect_write(unsigned long i, uint8_t service, uint8_t key)
    {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_GE(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], service) << "write " << i;
        EXPECT_EQ(wbuf[5], key) << "write " << i;
    }

    /* Polls channel 0's support report until done(report) holds or
     * budget_ms passes; out keeps the last reading. */
    template <typename Done>
    static bool wait_for_support(ecuio_session_t* s,
                                 ecuio_channel_support_t& out, Done done,
                                 int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        for (;;) {
            if (ecuio_session_get_channel_support(s, 0, &out) == ECUIO_OK &&
                done(out)) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* A test-only profile asking the obd2 channel (0x7E8, requests to
     * 0x7E0) for each PID in pids, one def each. defs and set must be
     * static: the registry keeps the pointer. */
    static void register_pid_profile(const char* name, const uint8_t* pids,
                                     uint32_t n, ecuio_signal_def_t* defs,
                                     ecuio_signal_set_t* set)
    {
        for (uint32_t i = 0; i < n; ++i) {
            defs[i] = ecuio_signal_def_t{};
            defs[i].id = static_cast<uint16_t>(1 + i);
            defs[i].kind = ECUIO_VALUE_UINT;
            defs[i].start_byte = 2;
            defs[i].bit_length = 8;
            defs[i].scale = 1.0;
            defs[i].message_id = ECUIO_OBD2_ECU_ARB_ID;
            defs[i].mux_length = 2;
            defs[i].mux_key = 0x4100u | pids[i];
        }
        *set = ecuio_signal_set_t{};
        set->layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set->count = n;
        set->defs = defs;
        set->channel_count = ecuio_obd2_signal_set()->channel_count;
        set->channels = ecuio_obd2_signal_set()->channels;
        ASSERT_EQ(ecuio_profile_register(name, set, nullptr), ECUIO_OK);
    }
};

/* Sets one of the fake's knobs for a scope. In-proc the fake reads its
 * environment on every call; the destructor clears it, so a failed
 * ASSERT can't leak it into the next case. */
struct KnobGuard {
    const char* name;
    KnobGuard(const char* n, const char* v) : name(n)
    {
        SetEnvironmentVariableA(n, v);
    }
    ~KnobGuard() { SetEnvironmentVariableA(name, nullptr); }
    KnobGuard(const KnobGuard&) = delete;
    KnobGuard& operator=(const KnobGuard&) = delete;
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
    EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_ERR_BAD_URI);
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
              ECUIO_ERR_UNKNOWN_PROFILE);
    EXPECT_EQ(session, nullptr);
}

// ---------------------------------------------------------------------------
// ADR-0013: obd2 scheduler mode — rotation, coupling, drain
// ---------------------------------------------------------------------------

/* ROTATION: ?profile=obd2&filter=0x7E8 — the worker requests PIDs
 * round-robin in set order. Rotation order is asserted from the fake's
 * recorded WRITES (deterministic, one per reply), not from received
 * frames. ?probe=0 leaves out the bitmaps the probe asks first
 * (ProbeWalksTheChainBeforeTheRotation). */
TEST_F(J2534AdoptionTest, Obd2SchedulerRotatesPidsInSetOrder)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&filter=0x7E8&probe=0",
             FAKE_J2534_DLL_PATH);

    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);

    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    /* 3 requests, at least the floor apart; budget 2 s for CI variance. */
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
    static const ecuio_channel_decl_t kChannels[] = {{0x7E8u, 0x7E0u},
                                                     {0x7E9u, 0x7E1u}};
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
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    set.channel_count = 2;
    set.channels = kChannels;
    ASSERT_EQ(ecuio_profile_register("test-mode01-routing", &set, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-mode01-routing&probe=0",
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

/* SERVICE 0x22 (ADR-0015 §2): a def keyed 62 <DID> enrols the request
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

/* ====================================================================
 * The probe (ADR-0015 §4): the supported-PID walk before the rotation,
 * and dropping only on explicit answers.
 * ==================================================================== */

/* The fake's ECU supports the obd2 set's PIDs, and its bitmaps say so,
 * so the walk asks 0x00, 0x20 and 0x40 (whose last bit is clear) and
 * drops nothing. The expected bitmaps are worked out here from the PIDs
 * by SAE J1979's layout (PID base + k is bit 0x80 >> ((k - 1) % 8) of
 * byte (k - 1) / 8), not taken from the fake:
 *   0x00: 04 05 | 0C 0D 0F 10 | 11 | 20  ->  18 1B 80 01
 *   0x20: 2F, 40                         ->  00 02 00 01
 *   0x40: 42, 5C                         ->  40 00 00 10 */
TEST_F(J2534AdoptionTest, ProbeWalksTheChainBeforeTheRotation)
{
    char uri[512];
    snprintf(uri, sizeof(uri), "j2534://%s", FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 5, 200);
    expect_write(0, 0x01, 0x00);
    expect_write(1, 0x01, 0x20);
    expect_write(2, 0x01, 0x40);
    expect_write(3, 0x01, 0x04);   /* the rotation, in set order */
    expect_write(4, 0x01, 0x05);

    ecuio_channel_support_t s = {};
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_COMPLETE) != 0;
    }, 2000)) << "flags " << s.flags;
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE);
    EXPECT_EQ(s.rx_id, 0x7E8u);
    EXPECT_EQ(s.ranges, 0x7u);
    const uint8_t want[32] = {0x18, 0x1B, 0x80, 0x01, 0x00, 0x02, 0x00, 0x01,
                              0x40, 0x00, 0x00, 0x10};
    EXPECT_EQ(memcmp(s.pid_bitmap, want, sizeof(want)), 0);
    EXPECT_EQ(s.requests, 10u);
    EXPECT_EQ(s.dropped, 0u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* The bitmaps alone drop what they mark unsupported: 0x0B is clear in
 * 0x00's, 0x5D in 0x40's, and 0x61 lies past the chain's end. The ECU
 * answers nothing for an unsupported PID here (as J1979 lets it), so
 * without the walk those three would each wait out 100 ms every pass. */
TEST_F(J2534AdoptionTest, ProbeDropsWhatTheBitmapsMarkUnsupported)
{
    static const uint8_t kPids[] = {0x0B, 0x0C, 0x5D, 0x61};
    static ecuio_signal_def_t defs[4];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-bitmaps", kPids, 4,
                                                 defs, &set));
    KnobGuard silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-probe-bitmaps",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    wait_for_writes(get_write_count, 8, 300);
    for (unsigned long i = 3; i < 8; ++i) {
        expect_write(i, 0x01, 0x0C);
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.requests, 4u);
    ASSERT_EQ(s.dropped, 3u);
    const uint8_t gone[3] = {0x0B, 0x5D, 0x61};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(s.dropped_list[i].request[0], 0x01u) << i;
        EXPECT_EQ(s.dropped_list[i].request[1], gone[i]) << i;
        EXPECT_EQ(s.dropped_list[i].reason, ECUIO_DROP_BITMAP) << i;
    }

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?probe=0 sends the set as written: no walk, and the PIDs the ECU
 * refuses (7F 01 12 for 0x0B, 0x5D and 0x61) stay in the rotation. */
TEST_F(J2534AdoptionTest, ProbeOffSendsTheSetAsWritten)
{
    static const uint8_t kPids[] = {0x0B, 0x0C, 0x5D, 0x61};
    static ecuio_signal_def_t defs[4];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-off", kPids, 4,
                                                 defs, &set));
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-probe-off&probe=0",
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
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    set.channel_count = ecuio_obd2_signal_set()->channel_count;
    set.channels = ecuio_obd2_signal_set()->channels;
    ASSERT_EQ(ecuio_profile_register("test-probe-did", &set, nullptr), ECUIO_OK);

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-probe-did",
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

/* A range is scanned once, in order: of 1E18 to 1E20 the fake's ECU
 * knows only 1E1C, which the scan finds and then polls, taking turns
 * with the rest of the scan, while the eight it refuses (7F 22 31) are
 * counted and never asked again. */
TEST_F(J2534AdoptionTest, UdsGenericScanFindsWhatTheEcuAnswers)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=uds-generic&did=1E18-1E20",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session)->count, 9u);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    ecuio_channel_support_t s = {};
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
    }, 3000)) << "the scan didn't end (done " << s.scan_done << ")";
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE) << "no walk: no Service 01";
    EXPECT_EQ(s.scan_total, 9u);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 8u);
    EXPECT_EQ(s.scan_polled, 1u);
    EXPECT_EQ(s.requests, 1u) << "the found DID, and nothing else, rotates";
    EXPECT_EQ(s.dropped, 0u);

    wait_for_writes(get_write_count, 14, 300);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    /* The scan alone until 1E1C is found at write 4; from then on the
     * rotation (1E1C) and the scan's remaining DIDs take turns, and once
     * the scan is over only 1E1C is asked. */
    const uint8_t want[14] = {0x18, 0x19, 0x1A, 0x1B, 0x1C,
                              0x1C, 0x1D, 0x1C, 0x1E, 0x1C, 0x1F, 0x1C, 0x20,
                              0x1C};
    for (unsigned long i = 0; i < 14; ++i) {
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 7u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x22u) << "write " << i;
        EXPECT_EQ(wbuf[5], 0x1Eu) << "write " << i;
        EXPECT_EQ(wbuf[6], want[i]) << "write " << i;
    }

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* A connect while the ECU answers nothing (reads bring no frame): the
 * walk's first bitmap times out, nothing is judged, and every PID stays
 * while the bus is quiet. Once replies flow the link is ok again, and
 * the walk resumes and drops what the bitmaps mark unsupported. The ECU
 * stays silent for an unsupported PID, so only the walk can drop 0x0B. */
TEST_F(J2534AdoptionTest, ProbeResumesWhenTheLinkRecovers)
{
    static const uint8_t kPids[] = {0x0B, 0x0C};
    static ecuio_signal_def_t defs[2];
    static ecuio_signal_set_t set;
    ASSERT_NO_FATAL_FAILURE(register_pid_profile("test-probe-recovery", kPids,
                                                 2, defs, &set));
    KnobGuard silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    std::unique_ptr<KnobGuard> quiet =
        std::make_unique<KnobGuard>("ECUIO_FAKE_READ_STATUS", "9");

    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=test-probe-recovery",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    int32_t le = 0;
    uint32_t ec = 0;
    for (int i = 0; i < 500 && le != ECUIO_ERR_VEHICLE_TIMEOUT; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)ecuio_session_get_error_state(session, &le, &ec);
    }
    ASSERT_EQ(le, ECUIO_ERR_VEHICLE_TIMEOUT) << "the bus never read as quiet";
    expect_write(0, 0x01, 0x00);
    for (unsigned long i = 1; i < 5; ++i) {
        expect_write(i, 0x01, kPids[(i - 1) % 2]);
    }
    ecuio_channel_support_t s = {};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_PROBED);
    EXPECT_EQ(s.ranges, 0u);
    EXPECT_EQ(s.dropped, 0u) << "silence dropped a request";

    quiet.reset();
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_COMPLETE) != 0;
    }, 3000)) << "the walk didn't resume (flags " << s.flags << ")";
    EXPECT_EQ(s.ranges, 0x7u);
    ASSERT_EQ(s.dropped, 1u);
    EXPECT_EQ(s.dropped_list[0].request[1], 0x0Bu);
    EXPECT_EQ(s.dropped_list[0].reason, ECUIO_DROP_BITMAP);

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

/* The vendor clock (j2534_clock.h): the first frame takes the host's
 * time; later ones the vendor's delta, even when the host saw them
 * arrive together or further apart. */
/* Both read paths copy a vendor's message through ecuio_pt_rx_from_msg:
 * RxStatus and the stamp as they came, and Data up to its DataSize,
 * never past the end of PASSTHRU_MSG.Data however large a size a
 * vendor reports. */
TEST(J2534StreamTest, ReceivedMessageCopyStopsAtTheEndOfData)
{
    static PASSTHRU_MSG m = {};
    static ecuio_pt_rx_t rx = {};
    for (unsigned long i = 0; i < ECUIO_PT_MSG_DATA_SIZE; ++i) {
        m.Data[i] = static_cast<unsigned char>(i * 7);
    }
    m.RxStatus = ECUIO_PT_CAN_29BIT_ID;
    m.Timestamp = 0xFFFFFFF0ul;
    m.DataSize = 7;
    ecuio_pt_rx_from_msg(&m, &rx);
    EXPECT_EQ(rx.rx_status, ECUIO_PT_CAN_29BIT_ID);
    EXPECT_EQ(rx.timestamp, 0xFFFFFFF0u);
    EXPECT_EQ(rx.data_size, 7u);
    EXPECT_EQ(memcmp(rx.data, m.Data, 7), 0);

    m.DataSize = ECUIO_PT_MSG_DATA_SIZE + 1000;
    ecuio_pt_rx_from_msg(&m, &rx);
    EXPECT_EQ(rx.data_size, static_cast<uint32_t>(ECUIO_PT_MSG_DATA_SIZE));
    EXPECT_EQ(memcmp(rx.data, m.Data, ECUIO_PT_MSG_DATA_SIZE), 0);
}

TEST(J2534StreamTest, VendorClockAnchorsAtTheHostAndKeepsVendorDeltas)
{
    j2534_clock c;
    EXPECT_EQ(j2534_clock_step(c, 3000000000u, 1500), 1500u);
    EXPECT_EQ(j2534_clock_step(c, 3000004000u, 1500), 5500u) << "one batch";
    EXPECT_EQ(j2534_clock_step(c, 3000010000u, 20000), 11500u)
        << "the vendor's 6 ms, not the host's 18.5";
}

/* Across the 32-bit wrap, and across a gap of several wraps, which only
 * the host's clock can count. */
TEST(J2534StreamTest, VendorClockCarriesAcrossTheWrap)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 0xFFFFFFF0u, 1000);
    EXPECT_EQ(j2534_clock_step(c, 0x10u, 1100), uint64_t{1000 + 0x20});
    const uint64_t gap = 3 * J2534_CLOCK_WRAP_US + 5000000;
    EXPECT_EQ(j2534_clock_step(c, 0x10u + 5000000u, 1100 + gap),
              uint64_t{1000 + 0x20} + gap);
}

/* A vendor clock that steps, back or far ahead, doesn't take the
 * session's time with it: the host's delta stands in, and the vendor's
 * deltas resume from the new stamp. */
TEST(J2534StreamTest, VendorClockThatStepsFallsBackToTheHost)
{
    j2534_clock c;
    (void)j2534_clock_step(c, 50000000u, 0);
    EXPECT_EQ(j2534_clock_step(c, 10000000u, 2000), 2000u) << "back 40 s";
    EXPECT_EQ(j2534_clock_step(c, 10001000u, 2500), 3000u);
    EXPECT_EQ(j2534_clock_step(c, 10001000u + 120000000u, 3500), 4000u)
        << "two minutes ahead of the host";
    EXPECT_EQ(j2534_clock_step(c, 10001000u + 120000000u, 3400), 4000u)
        << "never decreases";
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
