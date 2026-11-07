// File/layer: tests/test_queue.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: QueueTest (test_core)
//
// The pull queue (ecuio/queue.h, ADR-0053 section 4 and its appendix
// "the pull queue as built"). Most cases feed a session that never
// starts through ecuio_session_dispatch_callbacks, as RecorderTest does,
// so every entry and drop is exact; the close cases run a mock:// worker
// and readers on threads of their own, for the sanitizers to watch.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/queue.h"
#include "ecuio/signals.h"
}

namespace {

using Clock = std::chrono::steady_clock;

/* The cases' profile: a number on 0x100 and two on 0x102 (one frame,
 * two values), a text on 0x101 */
ecuio_signal_def_t kDefs[4];
ecuio_signal_set_t kSet;
constexpr uint32_t kNumberId = 0x100;
constexpr uint32_t kTextId = 0x101;
constexpr uint32_t kPairId = 0x102;

void register_profile()
{
    static bool done = false;
    if (done)
        return;
    std::memset(kDefs, 0, sizeof kDefs);
    kDefs[0].id = 1;
    kDefs[0].message_id = kNumberId;
    kDefs[0].kind = ECUIO_VALUE_UINT;
    kDefs[0].bit_length = 8;
    kDefs[0].scale = 0.5;
    std::strcpy(kDefs[0].name, "Half");
    kDefs[1].id = 2;
    kDefs[1].message_id = kTextId;
    kDefs[1].kind = ECUIO_VALUE_TEXT;
    std::strcpy(kDefs[1].name, "Words");
    kDefs[2].id = 3;
    kDefs[2].message_id = kPairId;
    kDefs[2].kind = ECUIO_VALUE_UINT;
    kDefs[2].bit_length = 8;
    kDefs[2].scale = 1;
    std::strcpy(kDefs[2].name, "First");
    kDefs[3].id = 4;
    kDefs[3].message_id = kPairId;
    kDefs[3].kind = ECUIO_VALUE_UINT;
    kDefs[3].start_byte = 1;
    kDefs[3].bit_length = 8;
    kDefs[3].scale = 1;
    std::strcpy(kDefs[3].name, "Second");
    kSet = ecuio_signal_set_t{};
    kSet.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    kSet.count = 4;
    kSet.defs = kDefs;
    ecuio_validation_t why{};
    ASSERT_EQ(ecuio_profile_register("queue-test", &kSet, &why), ECUIO_OK)
        << why.code << " at " << why.index;
    done = true;
}

/* A session never started, its frames fed by hand */
struct Fed {
    ecuio_session_t* session = nullptr;
    explicit Fed(const char* uri = "mock://queue?profile=queue-test")
    {
        register_profile();
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Fed() { ecuio_session_destroy(session); }
    void dispatch(uint32_t id, const std::string& bytes, uint64_t t_us = 0,
                  uint32_t flags = 0)
    {
        static ecuio_frame_t f;
        std::memset(&f, 0, sizeof f);
        f.arbitration_id = id;
        f.timestamp_us = t_us;
        f.flags = flags;
        f.data_length = static_cast<uint16_t>(bytes.size());
        std::memcpy(f.data, bytes.data(), bytes.size());
        ecuio_session_dispatch_callbacks(session, &f);
    }
};

/* A queue closed at the end of the scope */
struct Queue {
    ecuio_queue_t* q = nullptr;
    ~Queue() { ecuio_queue_close(q); }
};

struct Frames {
    std::vector<ecuio_frame_t> out;
    uint32_t count = 0;
    uint64_t dropped = 0;
    explicit Frames(uint32_t capacity) : out(capacity) {}
    ecuio_status_t read(ecuio_queue_t* q, uint32_t timeout_ms = 0)
    {
        return ecuio_queue_read_frames(q, out.data(),
                                       static_cast<uint32_t>(out.size()),
                                       &count, &dropped, timeout_ms);
    }
};

struct Values {
    std::vector<ecuio_queue_value_t> out;
    uint32_t count = 0;
    uint64_t dropped = 0;
    explicit Values(uint32_t capacity) : out(capacity) {}
    ecuio_status_t read(ecuio_queue_t* q, uint32_t timeout_ms = 0)
    {
        return ecuio_queue_read_values(q, out.data(),
                                       static_cast<uint32_t>(out.size()),
                                       &count, &dropped, timeout_ms);
    }
};

std::string bytes_of(uint32_t n, uint8_t first)
{
    std::string s(n, '\0');
    for (uint32_t i = 0; i < n; ++i)
        s[i] = static_cast<char>(first + i);
    return s;
}

/* What a values callback saw, entry by entry */
struct Seen {
    uint64_t t;
    uint32_t id;
    uint16_t signal;
    uint8_t kind;
    double value;
    std::string text;
};

void seen_cb(const ecuio_value_batch_t* b, void* user)
{
    auto* seen = static_cast<std::vector<Seen>*>(user);
    for (uint32_t i = 0; i < b->count; ++i) {
        const ecuio_value_t& v = b->values[i];
        seen->push_back(
            {b->timestamp_us, b->arbitration_id, v.signal_id, v.kind, v.value,
             v.text != nullptr ? std::string(v.text, v.text_len) : ""});
    }
}

void count_cb(const ecuio_frame_t*, void* user)
{
    ++*static_cast<std::atomic<int>*>(user);
}

bool wait_for(const std::function<bool()>& done, int ms)
{
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (Clock::now() > until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int64_t ms_since(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                 t)
        .count();
}

} // namespace

TEST(QueueTest, FramesComeInOrderWithTheirBytes)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 8, &q.q),
              ECUIO_OK);
    fed.dispatch(0x7E8, bytes_of(8, 1), 10);
    fed.dispatch(0x18DAF110, bytes_of(64, 2), 20,
                 ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_FD);
    fed.dispatch(0x7E8, bytes_of(4095, 3), 30, ECUIO_FRAME_FLAG_REASSEMBLED);
    fed.dispatch(0x123, "", 40);

    Frames r(16);
    for (ecuio_frame_t& f : r.out)
        std::memset(&f, 0xAB, sizeof f); /* every byte the read leaves shows */
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 4u);
    EXPECT_EQ(r.dropped, 0u);
    const uint32_t ids[] = {0x7E8, 0x18DAF110, 0x7E8, 0x123};
    const uint32_t lengths[] = {8, 64, 4095, 0};
    const uint32_t flags[] = {0,
                              ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_FD,
                              ECUIO_FRAME_FLAG_REASSEMBLED, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        const ecuio_frame_t& f = r.out[i];
        EXPECT_EQ(f.arbitration_id, ids[i]);
        EXPECT_EQ(f.timestamp_us, 10u * (i + 1));
        EXPECT_EQ(f.flags, flags[i]);
        EXPECT_EQ(f.channel_id, 0u);
        EXPECT_EQ(f.reserved, 0u);
        ASSERT_EQ(f.data_length, lengths[i]);
        EXPECT_EQ(
            std::string(reinterpret_cast<const char*>(f.data), f.data_length),
            bytes_of(lengths[i], static_cast<uint8_t>(i + 1)));
        EXPECT_TRUE(std::all_of(f.data + f.data_length,
                                f.data + ECUIO_FRAME_DATA_MAX,
                                [](uint8_t b) { return b == 0; }))
            << "the bytes past data_length are zero";
    }
    EXPECT_EQ(r.out[4].arbitration_id, 0xABABABABu)
        << "nothing past count is written";
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    EXPECT_EQ(r.count, 0u) << "a read takes what it copies";
}

TEST(QueueTest, AFullQueueDropsTheOldestAndCountsEveryDrop)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 4, &q.q),
              ECUIO_OK);
    for (uint32_t i = 0; i < 10; ++i)
        fed.dispatch(0x100 + i, "x", i);
    Frames r(8);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 4u);
    EXPECT_EQ(r.dropped, 6u);
    for (uint32_t i = 0; i < 4; ++i)
        EXPECT_EQ(r.out[i].arbitration_id, 0x106 + i)
            << "the newest four, in order";

    fed.dispatch(0x200, "y", 20);
    fed.dispatch(0x201, "y", 21);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 2u);
    EXPECT_EQ(r.out[0].arbitration_id, 0x200u);
    EXPECT_EQ(r.dropped, 6u) << "the running total, not this read's";
    for (uint32_t i = 0; i < 9; ++i)
        fed.dispatch(0x300 + i, "z", 30 + i);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    EXPECT_EQ(r.dropped, 11u);
    EXPECT_EQ(r.out[0].arbitration_id, 0x305u);
}

TEST(QueueTest, AReadTakesAtMostItsCapacityAcrossChunks)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 200, &q.q),
              ECUIO_OK);
    for (uint32_t i = 0; i < 150; ++i)
        fed.dispatch(0x100, "x", i);
    Frames r(100);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 100u);
    for (uint32_t i = 0; i < 100; ++i)
        ASSERT_EQ(r.out[i].timestamp_us, i);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 50u);
    EXPECT_EQ(r.out[0].timestamp_us, 100u);
    EXPECT_EQ(r.out[49].timestamp_us, 149u);
}

TEST(QueueTest, ValuesAreACallbacksBatchesEntryByEntry)
{
    Fed fed;
    std::vector<Seen> seen;
    ASSERT_EQ(ecuio_register_values_callback(fed.session, seen_cb, &seen),
              ECUIO_OK);
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 64, &q.q),
              ECUIO_OK);
    fed.dispatch(kNumberId, std::string(1, '\x51'), 5);
    fed.dispatch(kTextId, "ECM-1", 6);
    fed.dispatch(kPairId, std::string("\x07\x09", 2), 7);
    fed.dispatch(0x7FF, "nothing decodes this", 8);

    Values r(64);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(seen.size(), 4u);
    ASSERT_EQ(r.count, seen.size());
    for (uint32_t i = 0; i < r.count; ++i) {
        SCOPED_TRACE(i);
        const ecuio_queue_value_t& v = r.out[i];
        EXPECT_EQ(v.timestamp_us, seen[i].t);
        EXPECT_EQ(v.arbitration_id, seen[i].id);
        EXPECT_EQ(v.signal_id, seen[i].signal);
        EXPECT_EQ(v.kind, seen[i].kind);
        EXPECT_EQ(v.value, seen[i].value);
        EXPECT_EQ(std::string(v.text, v.text_len), seen[i].text);
        EXPECT_EQ(v.text[v.text_len], '\0');
        EXPECT_EQ(v.flags, 0u);
        EXPECT_EQ(v.reserved, 0u);
    }
    EXPECT_EQ(r.out[0].value, 40.5);
    EXPECT_EQ(std::string(r.out[1].text), "ECM-1");
    EXPECT_EQ(r.out[2].signal_id, 3u) << "one frame's two values, in set order";
    EXPECT_EQ(r.out[3].signal_id, 4u);
    EXPECT_EQ(r.out[3].timestamp_us, r.out[2].timestamp_us);
}

/* With no values callback, the queue alone makes dispatch decode */
TEST(QueueTest, AValuesQueueAloneDecodes)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 8, &q.q),
              ECUIO_OK);
    fed.dispatch(kNumberId, std::string(1, '\x10'), 1);
    Values r(8);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 1u);
    EXPECT_EQ(r.out[0].value, 8.0);
}

TEST(QueueTest, ALongTextIsCutAndSaysSo)
{
    Fed fed;
    Queue q, one;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 8, &q.q),
              ECUIO_OK);
    /* One slot, so each value lands where the last one was */
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 1, &one.q),
              ECUIO_OK);
    const std::string fits(127, 'A');
    const std::string longer = std::string(200, 'B');
    fed.dispatch(kTextId, longer, 1);
    fed.dispatch(kTextId, fits, 2);
    fed.dispatch(kTextId, "short", 3);
    Values r(8);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 3u);
    EXPECT_EQ(r.out[0].text_len, 127u);
    EXPECT_EQ(r.out[0].flags, ECUIO_QUEUE_VALUE_TEXT_CUT);
    EXPECT_EQ(std::string(r.out[0].text), longer.substr(0, 127));
    EXPECT_EQ(r.out[1].text_len, 127u);
    EXPECT_EQ(r.out[1].flags, 0u) << "127 bytes fit";
    EXPECT_EQ(std::string(r.out[1].text), fits);
    EXPECT_EQ(std::string(r.out[2].text), "short");
    Values last(1);
    ASSERT_EQ(last.read(one.q), ECUIO_OK);
    ASSERT_EQ(last.count, 1u);
    EXPECT_EQ(last.dropped, 2u);
    EXPECT_EQ(std::string(last.out[0].text), "short");
    EXPECT_TRUE(std::all_of(last.out[0].text + 5,
                            last.out[0].text + ECUIO_QUEUE_TEXT_MAX,
                            [](char c) { return c == 0; }))
        << "no older entry's text shows past the NUL";
}

TEST(QueueTest, AFullValuesQueueDropsSingleValues)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 3, &q.q),
              ECUIO_OK);
    fed.dispatch(kPairId, std::string("\x01\x02", 2), 1);
    fed.dispatch(kPairId, std::string("\x03\x04", 2), 2);
    Values r(8);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    ASSERT_EQ(r.count, 3u);
    EXPECT_EQ(r.dropped, 1u) << "a value, not a frame";
    EXPECT_EQ(r.out[0].value, 2.0) << "the first frame's second value kept";
    EXPECT_EQ(r.out[1].value, 3.0);
    EXPECT_EQ(r.out[2].value, 4.0);
}

TEST(QueueTest, AnEmptyReadWaitsItsTimeoutAndAnEntryEndsTheWait)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 8, &q.q),
              ECUIO_OK);
    Frames r(8);
    auto t = Clock::now();
    ASSERT_EQ(r.read(q.q, 0), ECUIO_OK);
    EXPECT_EQ(r.count, 0u);
    EXPECT_LT(ms_since(t), 50) << "0 waits not at all";

    t = Clock::now();
    ASSERT_EQ(r.read(q.q, 150), ECUIO_OK);
    EXPECT_EQ(r.count, 0u);
    EXPECT_GE(ms_since(t), 140);
    EXPECT_LT(ms_since(t), 3000);

    std::thread feeder([&] {
        wait_for([&] { return ecuio_queue_test_waiting(q.q) == 1; }, 5000);
        fed.dispatch(0x100, "x", 1);
    });
    t = Clock::now();
    ASSERT_EQ(r.read(q.q, 10000), ECUIO_OK);
    const int64_t waited = ms_since(t);
    feeder.join();
    EXPECT_EQ(r.count, 1u);
    EXPECT_LT(waited, 5000) << "the entry woke the read";
}

TEST(QueueTest, RefusalsSayWhichArgument)
{
    Fed fed;
    ecuio_queue_t* q = reinterpret_cast<ecuio_queue_t*>(&fed);
    EXPECT_EQ(ecuio_queue_open(nullptr, ECUIO_QUEUE_FRAMES, 8, &q),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(q, nullptr) << "*out NULL first";
    EXPECT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 8, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(fed.session, 0, 8, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(fed.session, 3, 8, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 0, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES,
                               ECUIO_QUEUE_MAX_FRAMES + 1, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES,
                               ECUIO_QUEUE_MAX_VALUES + 1, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(q, nullptr);

    /* The ceilings themselves open, and the session takes four */
    Queue a, b, c, d;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES,
                               ECUIO_QUEUE_MAX_FRAMES, &a.q),
              ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES,
                               ECUIO_QUEUE_MAX_VALUES, &b.q),
              ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 1, &c.q),
              ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_VALUES, 1, &d.q),
              ECUIO_OK);
    EXPECT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 1, &q),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a fifth";

    ecuio_frame_t frame{};
    ecuio_queue_value_t value{};
    uint32_t count = 7;
    uint64_t dropped = 7;
    EXPECT_EQ(ecuio_queue_read_frames(nullptr, &frame, 1, &count, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_frames(a.q, nullptr, 1, &count, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_frames(a.q, &frame, 1, nullptr, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_frames(a.q, &frame, 1, &count, nullptr, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_frames(a.q, &frame, 0, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_frames(b.q, &frame, 1, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a values queue's frames";
    EXPECT_EQ(ecuio_queue_read_values(a.q, &value, 1, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a frames queue's values";
    EXPECT_EQ(ecuio_queue_read_values(nullptr, &value, 1, &count, &dropped, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_queue_read_values(b.q, &value, 0, &count, &dropped, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_queue_close(nullptr);
}

TEST(QueueTest, AStartedOrClosedSessionTakesNoQueue)
{
    register_profile();
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://late", &s), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_queue_t* q = nullptr;
    EXPECT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, 8, &q),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, 8, &q),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(q, nullptr);
    ecuio_session_destroy(s);
}

TEST(QueueTest, WhatWaitsAtCloseIsReadThenTheReadsEnd)
{
    Fed fed;
    Queue q;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 8, &q.q),
              ECUIO_OK);
    fed.dispatch(0x100, "a", 1);
    fed.dispatch(0x101, "b", 2);
    ASSERT_EQ(ecuio_session_close(fed.session), ECUIO_OK);
    fed.dispatch(0x102, "c", 3); /* a closed session dispatches nothing */
    Frames r(1);
    ASSERT_EQ(r.read(q.q), ECUIO_OK);
    EXPECT_EQ(r.out[0].arbitration_id, 0x100u);
    ASSERT_EQ(r.read(q.q, 5000), ECUIO_OK);
    EXPECT_EQ(r.out[0].arbitration_id, 0x101u);
    const auto t = Clock::now();
    EXPECT_EQ(r.read(q.q, 5000), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(r.count, 0u);
    EXPECT_LT(ms_since(t), 1000) << "an ended queue doesn't wait";
}

/* The queue may outlive its session: closed or destroyed first */
TEST(QueueTest, AQueueOutlivesItsSession)
{
    register_profile();
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://outlive?profile=queue-test", &s),
              ECUIO_OK);
    Queue frames, values;
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, 8, &frames.q), ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_VALUES, 8, &values.q), ECUIO_OK);
    ecuio_frame_t f{};
    f.arbitration_id = kNumberId;
    f.data_length = 1;
    f.data[0] = 4;
    ecuio_session_dispatch_callbacks(s, &f);
    ecuio_session_destroy(s); /* closes, then frees the session */

    Frames rf(8);
    ASSERT_EQ(rf.read(frames.q), ECUIO_OK);
    EXPECT_EQ(rf.count, 1u);
    EXPECT_EQ(rf.read(frames.q, 1000), ECUIO_ERR_SESSION_CLOSED);
    Values rv(8);
    ASSERT_EQ(rv.read(values.q), ECUIO_OK);
    ASSERT_EQ(rv.count, 1u);
    EXPECT_EQ(rv.out[0].value, 2.0);
    EXPECT_EQ(rv.read(values.q), ECUIO_ERR_SESSION_CLOSED);
}

/* A running worker: the session closing ends a read that waits on
 * another thread, after it has read what was left */
TEST(QueueTest, ASessionClosingEndsAReadOnAnotherThread)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://running", &s), ECUIO_OK);
    Queue q;
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, 64, &q.q), ECUIO_OK);
    std::atomic<uint32_t> frames{0};
    std::atomic<int> last{ECUIO_OK};
    std::thread reader([&] {
        Frames r(16);
        ecuio_status_t rc;
        while ((rc = r.read(q.q, 10000)) == ECUIO_OK)
            frames += r.count;
        last = rc;
    });
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    EXPECT_TRUE(wait_for([&] { return frames.load() >= 5; }, 5000))
        << frames.load();
    const auto t = Clock::now();
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    reader.join();
    EXPECT_LT(ms_since(t), 5000) << "the close ended the 10 s wait";
    EXPECT_EQ(last.load(), ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}

/* The queue closing on one thread while a read waits on another: the
 * read returns before the queue is freed */
TEST(QueueTest, AQueueClosingEndsAReadOnAnotherThread)
{
    Fed fed;
    ecuio_queue_t* q = nullptr;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES, 8, &q),
              ECUIO_OK);
    std::atomic<int> rc{-1};
    std::atomic<int64_t> waited{0};
    std::thread reader([&] {
        Frames r(8);
        const auto t = Clock::now();
        rc = r.read(q, 10000);
        waited = ms_since(t);
    });
    ASSERT_TRUE(
        wait_for([&] { return ecuio_queue_test_waiting(q) == 1; }, 5000));
    ecuio_queue_close(q); /* returns once the read has */
    reader.join();
    EXPECT_EQ(rc.load(), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_LT(waited.load(), 5000);
    fed.dispatch(0x100, "after", 1); /* the session carries on without it */
}

/* Closed while the worker copies into it: the worker never reaches the
 * freed queue (the sanitizers' case), and the session carries on. The
 * replay fixture at full speed, looping, so the worker copies almost
 * all the time */
TEST(QueueTest, AQueueClosesUnderARunningWorker)
{
    const std::string fixture =
        "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
        "/2026-10-25-engine-start?profile=obd2&speed=max&loop=1";
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(fixture.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_queue_t* frames = nullptr;
    ecuio_queue_t* values = nullptr;
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, 4, &frames), ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_VALUES, 4, &values), ECUIO_OK);
    std::atomic<int> seen{0};
    ASSERT_EQ(ecuio_register_frame_callback(s, count_cb, &seen), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return seen.load() >= 6; }, 5000))
        << seen.load();
    Frames r(4);
    EXPECT_EQ(r.read(frames, 0), ECUIO_OK);
    EXPECT_GT(r.dropped, 0u) << "the worker filled it";
    ecuio_queue_close(frames);
    ecuio_queue_close(values);
    const int at_close = seen.load();
    EXPECT_TRUE(wait_for([&] { return seen.load() >= at_close + 2; }, 5000))
        << "the worker goes on without them";
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_session_destroy(s);
}

/* Two readers split one queue: each entry once, each reader's in order */
TEST(QueueTest, TwoReadersSplitTheEntries)
{
    Fed fed;
    Queue q;
    constexpr uint32_t kFrames = 3000;
    ASSERT_EQ(ecuio_queue_open(fed.session, ECUIO_QUEUE_FRAMES,
                               ECUIO_QUEUE_MAX_FRAMES, &q.q),
              ECUIO_OK);
    std::vector<uint64_t> got[2];
    std::thread readers[2];
    for (int i = 0; i < 2; ++i) {
        readers[i] = std::thread([&, i] {
            Frames r(7);
            while (r.read(q.q, 10000) == ECUIO_OK) {
                for (uint32_t k = 0; k < r.count; ++k)
                    got[i].push_back(r.out[k].timestamp_us);
            }
        });
    }
    for (uint32_t i = 0; i < kFrames; ++i)
        fed.dispatch(0x100, "x", i);
    ASSERT_EQ(ecuio_session_close(fed.session), ECUIO_OK);
    for (std::thread& t : readers)
        t.join();
    std::set<uint64_t> all;
    for (const auto& g : got) {
        EXPECT_TRUE(std::is_sorted(g.begin(), g.end()));
        all.insert(g.begin(), g.end());
    }
    EXPECT_EQ(got[0].size() + got[1].size(), kFrames) << "each once";
    EXPECT_EQ(all.size(), kFrames);
}

/* The stream's end (queue.h): a replay's last pass ends its queues */
namespace {

std::string fixture_uri(const std::string& query)
{
    return "replay://" + std::string(ECUIO_REPLAY_FIXTURES) +
           "/2026-10-25-engine-start?profile=obd2&speed=max" + query;
}

/* Reads a queue until a read isn't ECUIO_OK, each waiting up to 10 s;
 * the entries taken. last_ms: how long that last read took */
constexpr uint32_t kDrainWaitMs = 10000;

template <typename Reader>
uint64_t drain(Reader& r, ecuio_queue_t* q, ecuio_status_t* last,
               int64_t* last_ms = nullptr)
{
    uint64_t total = 0;
    ecuio_status_t rc;
    for (;;) {
        const auto t0 = std::chrono::steady_clock::now();
        rc = r.read(q, kDrainWaitMs);
        if (last_ms != nullptr) {
            *last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        }
        if (rc != ECUIO_OK) {
            break;
        }
        total += r.count;
    }
    *last = rc;
    return total;
}

/* A read waiting when the end comes returns then, not at its timeout. A
 * reader that keeps up with the replay waits for its next frame, so the
 * end finds it waiting; a read that starts after the end returns at once
 * either way */
constexpr int64_t kEndWakeMs = kDrainWaitMs / 2;

} // namespace

TEST(QueueTest, AReplaysLastFrameEndsItsQueues)
{
    const std::string uri = fixture_uri("");
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    Queue frames, values;
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, ECUIO_QUEUE_MAX_FRAMES,
                               &frames.q),
              ECUIO_OK);
    ASSERT_EQ(ecuio_queue_open(s, ECUIO_QUEUE_VALUES, ECUIO_QUEUE_MAX_VALUES,
                               &values.q),
              ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);

    Frames rf(256);
    Values rv(1024);
    ecuio_status_t frames_end = ECUIO_OK;
    ecuio_status_t values_end = ECUIO_OK;
    int64_t frames_ms = 0;
    int64_t values_ms = 0;
    std::thread value_reader(
        [&] { (void)drain(rv, values.q, &values_end, &values_ms); });
    const uint64_t read = drain(rf, frames.q, &frames_end, &frames_ms);
    value_reader.join();
    EXPECT_EQ(frames_end, ECUIO_ERR_STREAM_ENDED);
    EXPECT_EQ(values_end, ECUIO_ERR_STREAM_ENDED);
    EXPECT_LT(frames_ms, kEndWakeMs) << "the end woke the waiting read";
    EXPECT_LT(values_ms, kEndWakeMs) << "the end woke the waiting read";
    EXPECT_EQ(read + rf.dropped, info.replay_frames)
        << "every frame, read or counted as dropped, before the end";
    EXPECT_GT(read, 0u);
    EXPECT_STREQ(ecuio_status_string(ECUIO_ERR_STREAM_ENDED),
                 "the stream has ended");

    /* The end stays the answer once the session closes too */
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(rf.read(frames.q, 1000), ECUIO_ERR_STREAM_ENDED);
    ecuio_session_destroy(s);
}

TEST(QueueTest, AReplayThatDropsFramesStillEnds)
{
    const std::string uri = fixture_uri("&faults=drop:20&seed=7");
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    Queue q;
    ASSERT_EQ(
        ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, ECUIO_QUEUE_MAX_FRAMES, &q.q),
        ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    Frames r(256);
    ecuio_status_t end = ECUIO_OK;
    const uint64_t read = drain(r, q.q, &end);
    EXPECT_EQ(end, ECUIO_ERR_STREAM_ENDED);
    ecuio_replay_faults_t f{};
    ASSERT_EQ(ecuio_session_get_replay_faults(s, &f), ECUIO_OK);
    EXPECT_GT(f.dropped, 0u) << "the faults dropped frames";
    EXPECT_EQ(read + r.dropped + f.dropped + f.overflowed, info.replay_frames)
        << "the frames the faults never sent don't hold the end back";
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_session_destroy(s);
}

/* A looping replay never ends: its queues end only at the close, as a
 * live transport's do */
TEST(QueueTest, ALoopingReplayEndsOnlyAtTheClose)
{
    const std::string uri = fixture_uri("&loop=1");
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    Queue q;
    ASSERT_EQ(
        ecuio_queue_open(s, ECUIO_QUEUE_FRAMES, ECUIO_QUEUE_MAX_FRAMES, &q.q),
        ECUIO_OK);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    Frames r(256);
    uint64_t seen = 0;
    ecuio_status_t rc = ECUIO_OK;
    while (seen < 3u * info.replay_frames &&
           (rc = r.read(q.q, 10000)) == ECUIO_OK)
        seen += r.count;
    EXPECT_EQ(rc, ECUIO_OK) << "past the end of three passes, still going";
    EXPECT_GE(seen + r.dropped, 3u * info.replay_frames);
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_status_t end = ECUIO_OK;
    (void)drain(r, q.q, &end);
    EXPECT_EQ(end, ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}
