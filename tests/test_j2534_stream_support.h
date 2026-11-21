// File/layer: tests/test_j2534_stream_support.h | GTest, HARNESS tier
// Testing: HARNESS: the helpers of test_j2534_stream*.cpp (test_j2534_stream)

#pragma once

#include <gtest/gtest.h>

#include "ecuio/j2534.h"
#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/internal.h" /* ecuio_set_unpicked_check_ms */
#include "ecuio/j2534_ptypes.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signal_sets.h"
#include "j2534_clock.h"     /* core/src: vendor stamps to session time */
#include "request_engine.h"  /* core/src: ECUIO_IDENT_REQUESTS */

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <set>
#include <string>
#include <thread>

#include "car_sized_set.h" /* tests: a set past a rotation's most */
#include "fake_control.h"  /* tests: the fake's control page */
#include "fake_diagnostics.h" /* tests: the fake ECU's report */

#include "temp_name.h"
#endif

#ifdef _WIN32

namespace {

/* ---- the fake, through its control page (fake_control.h) -----------
 * The page reaches the fake whichever backend serves the session, so
 * these read the fake inside the surrogate child. The page must
 * exist before a session opens (a child takes its name at spawn): a
 * fixture creates and resets it in SetUp. */
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

inline fake_control::Page* fake_page()
{
    return fake_control::create_for_this_process();
}

/* A received message for the fake's next read; the 29-bit flag as the
 * RxStatus bit. The channel is the fake's one. */
inline void page_inject(const unsigned char* data, unsigned long size,
                 unsigned long timestamp_us, unsigned long is_extended,
                 unsigned long /*channel*/)
{
    if (!fake_control::post(fake_page(), data, static_cast<uint32_t>(size),
                            static_cast<uint32_t>(timestamp_us),
                            is_extended ? ECUIO_PT_CAN_29BIT_ID : 0u)) {
        ADD_FAILURE() << "the fake's inbox is full";
    }
}

inline void page_set_gating(int enable)
{
    fake_control::set(fake_page(), "ECUIO_FAKE_FILTER_GATING",
                      enable ? "1" : nullptr);
}

inline void page_write_count(unsigned long* count)
{
    *count = fake_control::writes(fake_page());
}

/* Write i since the page's reset (the first kWriteDepth are kept);
 * zeroed past those. */
inline void page_recorded_write(unsigned long i, unsigned char* data,
                         unsigned long* size, unsigned long* tx_flags)
{
    fake_control::Write w;
    fake_control::recorded_write(fake_page(), static_cast<uint32_t>(i), &w);
    memcpy(data, w.data, fake_control::kMsgMax);
    *size = w.size;
    *tx_flags = w.tx_flags;
}

/* The last filter started: mask and pattern into 12-byte buffers. */
inline void page_last_filter(unsigned long* id, unsigned long* type,
                      unsigned char* mask, unsigned char* pattern,
                      unsigned long* len)
{
    const fake_control::Filter f = fake_control::last_filter(fake_page());
    *id = f.id;
    *type = f.type;
    memcpy(mask, f.mask, fake_control::kFilterMax);
    memcpy(pattern, f.pattern, fake_control::kFilterMax);
    *len = f.len;
}

inline void page_last_stop(unsigned long* id)
{
    *id = fake_control::last_stopped(fake_page());
}

struct StreamCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][8]{};
    unsigned short payload_lens[8]{};   /* post-strip length */
    unsigned int frame_flags[8]{};      /* ECUIO_FRAME_FLAG_* */
    uint32_t arbitration_ids[8]{};      /* identity stamped by the transport */
    uint64_t timestamps[8]{};           /* pass-through contract */
};

inline void capture_cb(const ecuio_frame_t* frame, void* user_data)
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

} // namespace

/* ====================================================================
 * Adoption suite (ADR-0012): ?filter= / ?txid= / ?poll=. Every URI
 * opts out of the obd2 default with ?profile=raw, so these pin the
 * raw-profile contract.
 * ==================================================================== */

class J2534AdoptionTest : public ::testing::Test {
protected:
    fn_inject_frame inject = page_inject;
    fn_set_filter_gating set_gating = page_set_gating;
    fn_get_write_count get_write_count = page_write_count;
    fn_get_recorded_write get_recorded_write = page_recorded_write;
    fn_get_last_filter_start get_last_filter = page_last_filter;
    fn_get_last_filter_stop get_last_stop = page_last_stop;

    void SetUp() override
    {
        ASSERT_NE(fake_page(), nullptr);
        fake_control::reset(fake_page());
    }

    void TearDown() override
    {
        /* Knobs (gating among them) and the counts live on the page:
         * never leak them into the next test, even on a failing
         * assertion. */
        if (fake_page() != nullptr) fake_control::reset(fake_page());
    }

    static void wait_for_count(StreamCapture& cap, int target, int iters)
    {
        for (int i = 0; i < iters && cap.count.load() < target; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* Obd2/scheduler variant: wait for the WORK side instead of the
     * receive side: rotation order is asserted from the recorded
     * writes' indices, so writes are the primary observable. */
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

    /* Write i (the page keeps the first 16) asked service for key. */
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

    /* The same for channel 0's identity report (ADR-0021). */
    template <typename Done>
    static bool wait_for_identity(ecuio_session_t* s,
                                  ecuio_channel_identity_t& out, Done done,
                                  int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        for (;;) {
            if (ecuio_session_get_channel_identity(s, 0, &out) == ECUIO_OK &&
                done(out)) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* The same for channel 0's diagnostics report (ADR-0037). */
    template <typename Done>
    static bool wait_for_diagnostics(ecuio_session_t* s,
                                     ecuio_channel_diagnostics_t& out,
                                     Done done, int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        for (;;) {
            if (ecuio_session_get_channel_diagnostics(s, 0, &out) == ECUIO_OK &&
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
            std::snprintf(defs[i].name, sizeof defs[i].name, "PID %02X",
                          pids[i]);
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

namespace {

/* Sets one of the fake's knobs for a scope, on its page, which the fake
 * in the child reads on every call; the destructor clears it, so a
 * failed ASSERT can't leak it into the next case. */
struct KnobGuard {
    const char* name;
    KnobGuard(const char* n, const char* v) : name(n)
    {
        EXPECT_TRUE(fake_control::set(fake_page(), n, v)) << n;
    }
    ~KnobGuard() { fake_control::set(fake_page(), name, nullptr); }
    KnobGuard(const KnobGuard&) = delete;
    KnobGuard& operator=(const KnobGuard&) = delete;
};

} // namespace

#endif // _WIN32
