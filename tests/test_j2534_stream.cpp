// File/layer: tests/test_j2534_stream.cpp | GTest, HARNESS tier
// Testing: End-to-end j2534:// session: URI routing -> attach ->
//          connect -> worker poll -> InjectFrame -> ReadMsgs ->
//          callback dispatch. Exact-count assertions (not >=) —
//          the fake emits nothing we did not inject.
//          Query-param tests: ?baud=/?flags= values observed at
//          PassThruConnect via the fake's GetLastConnectParams hook;
//          malformed queries rejected at session_create (no DLL load).

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

typedef void (*fn_inject_frame)(const unsigned char*, unsigned long,
                                unsigned long, unsigned long, unsigned long);
typedef void (*fn_get_connect_params)(unsigned long*, unsigned long*,
                                      unsigned long*);

struct StreamCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][8]{};
};

void capture_cb(const qttune_frame_t* frame, void* user_data)
{
    StreamCapture* cap = static_cast<StreamCapture*>(user_data);
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        memcpy(cap->payloads[i], frame->data, 8);
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

    inject(f0, 8, 1000, 0, 1);
    inject(f1, 8, 2000, 0, 1);
    inject(f2, 8, 3000, 0, 1);

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

    EXPECT_EQ(qttune_session_close(session), QT_OK);   /* joins worker */
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
}

TEST(J2534StreamTest, UnknownVendorNameRejected)
{
    qttune_session_t* session = nullptr;
    EXPECT_EQ(qttune_session_create("j2534://no-such-vendor-xyz",
                                    &session),
              QT_ERR_NO_TRANSPORT);
    EXPECT_EQ(session, nullptr);
}

} // namespace

#endif /* _WIN32 */
