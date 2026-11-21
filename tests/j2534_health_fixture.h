// File/layer: tests/j2534_health_fixture.h | GTest, HARNESS tier
// Testing: the fixture of J2534HealthTest (test_j2534_health)
//
// A named namespace, so every file of the suite shares one fixture type
// (gtest refuses two for one suite).

#ifndef ECUIO_TESTS_J2534_HEALTH_FIXTURE_H
#define ECUIO_TESTS_J2534_HEALTH_FIXTURE_H

#include <gtest/gtest.h>

#include "ecuio/core.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "j2534_backend_fixture.h"

namespace j2534_health {

inline void count_cb(const ecuio_frame_t* /*frame*/, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}

/* What the session delivered: each frame's payload and ID, how many
 * writes the fake had taken when it arrived, and how many came after
 * the fake's next write. In a request mode where every write is
 * answered, frame k belongs while the fake has taken exactly k writes;
 * one that arrives later came back after the next request. Read the
 * payloads only after the session is closed. */
struct Delivery {
    fake_control::Page* page = nullptr;
    std::atomic<int> count{0};
    std::atomic<int> after_next_write{0};
    uint16_t lengths[16] = {};
    uint8_t payloads[16][8] = {};
    uint64_t stamps[16] = {};
    uint32_t ids[16] = {};
    uint32_t flags[16] = {};
    uint32_t writes_at[16] = {};
};

inline void delivery_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* d = static_cast<Delivery*>(user_data);
    const int i = d->count.fetch_add(1);
    const uint32_t writes = fake_control::writes(d->page);
    if (writes > static_cast<uint32_t>(i + 1)) {
        d->after_next_write.fetch_add(1);
    }
    if (i < 16) {
        d->lengths[i] = frame->data_length;
        memcpy(d->payloads[i], frame->data, sizeof(d->payloads[i]));
        d->stamps[i] = frame->timestamp_us;
        d->ids[i] = frame->arbitration_id;
        d->flags[i] = frame->flags;
        d->writes_at[i] = writes;
    }
}

/* The session and its frame count are members, and TearDown closes the
 * session first, so a failed ASSERT can't leave the worker counting
 * into a destroyed local. */
class J2534HealthTest : public j2534_backends::BackendTest {
protected:
    ecuio_session_t* session = nullptr;
    std::atomic<int> frames{0};
    Delivery delivered;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(BackendTest::SetUp());
        delivered.page = page;
    }

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }

    /* Creates and starts the session on the fake with the given
     * query, counting its frames, and checks the backend it runs on. */
    void start_session(const char* query)
    {
        const std::string u = uri(query);
        ASSERT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_OK) << u;
        ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
        ASSERT_EQ(ecuio_register_frame_callback(session, count_cb, &frames),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_register_frame_callback(session, delivery_cb,
                                                &delivered),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    }

    /* Waits up to budget_ms for at least n frames. */
    bool wait_for_frames(int n, int budget_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        while (frames.load() < n &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return frames.load() >= n;
    }

    /* Waits up to budget_ms for the worker to make n more reads of the
     * fake than it has made now: the first of them may have begun before
     * what the caller just changed, so n = 2 is one that began after. */
    bool after_reads(uint32_t n, int budget_ms)
    {
        const uint32_t base = fake_control::reads(page);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(budget_ms);
        while (fake_control::reads(page) - base < n &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return fake_control::reads(page) - base >= n;
    }

    /* Closes the session, so what it delivered can be read. */
    void close_session()
    {
        EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
        ecuio_session_destroy(session);
        session = nullptr;
    }
};

} // namespace j2534_health

#endif
