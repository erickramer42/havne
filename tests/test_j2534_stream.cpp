// File/layer: tests/test_j2534_stream.cpp | GTest, HARNESS tier
// Testing: End-to-end j2534:// session: URI routing -> attach ->
//          connect -> worker poll -> InjectFrame -> ReadMsgs ->
//          callback dispatch. Exact-count assertions (not >=) —
//          the fake emits nothing we did not inject.

#include <gtest/gtest.h>

#include "qttune/j2534.h"
#include "qttune/core.h"

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
