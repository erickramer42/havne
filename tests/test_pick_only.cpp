// File/layer: tests/test_pick_only.cpp | test_core, Qt-free, C++17
// Testing: UNIT: PickOnlyTest (a session whose set derives more requests
//          than a rotation holds, ADR-0065 sections 2, 3 and 8: the table
//          attach builds, the focus and the conditions through it, the
//          rotation's refusals, the timing rows published); ReplyTimingTest
//          (a scan-only session's timing, ADR-0042)
//
// Each case creates a mock session on car_sized_set.h's set (two modules
// asked their most), builds its requests as a requesting attach does
// (ecuio_build_requests) and is the worker itself (ecuio_take_focus,
// ecuio_publish_timing), so nothing waits on a thread.

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "car_sized_set.h"
#include "request_engine.h" /* core/src */
#include "request_setup.h"  /* core/src */
#include "session.h"        /* core/src */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/* test_core.cpp's: operator new's calls on this thread, and the nth
 * from now failed once (0: none) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace {

using car_sized::id_of;
using car_sized::kMost;
constexpr uint32_t kPool = ECUIO_PROFILE_MAX_REQUESTS;
constexpr char kProfile[] = "pick-only-car";

const ecuio_signal_set_t* car_set()
{
    return car_sized::set();
}

/* A mock session on the car's set with its requests built, the case its
 * worker */
struct Car {
    ecuio_session_t* s = nullptr;
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    ecuio_focus_seen seen;

    Car()
    {
        ecuio_validation_t v{};
        EXPECT_EQ(ecuio_profile_register(kProfile, car_set(), &v), ECUIO_OK)
            << v.code << " at " << v.index;
        const std::string uri = std::string("mock://car?profile=") + kProfile;
        EXPECT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
            << ecuio_last_error_detail();
        if (s == nullptr) {
            return;
        }
        EXPECT_EQ(s->info.module_count, 2u);
        request_params params;
        params.probe = false;
        params.identify = false;
        params.diagnose = false;
        bool probe = false;
        bool identify = false;
        EXPECT_EQ(ecuio_build_requests(s, car_set(), params,
                                       s->module_rx.data(), s->module_tx.data(),
                                       s->info.module_count, e.get(), &probe,
                                       &identify),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Car()
    {
        ecuio_session_destroy(s);
        EXPECT_EQ(ecuio_profile_unregister(kProfile), ECUIO_OK);
    }
    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;

    void take() { ecuio_take_focus(s, e.get(), seen); }

    /* The table rows in the rotation */
    std::vector<uint32_t> asked() const
    {
        std::vector<uint32_t> rows;
        for (uint32_t i = 0; i < e->count; ++i) {
            if (ecuio_engine_in_rotation(e->requests[i])) {
                rows.push_back(i);
            }
        }
        return rows;
    }

    std::vector<uint16_t> unasked() const
    {
        uint32_t n = 0;
        EXPECT_EQ(ecuio_session_get_unasked_signals(s, nullptr, 0, &n),
                  ECUIO_OK);
        std::vector<uint16_t> ids(n);
        uint32_t again = 0;
        EXPECT_EQ(ecuio_session_get_unasked_signals(s, ids.data(), n, &again),
                  ECUIO_OK);
        EXPECT_EQ(again, n);
        return ids;
    }

    ecuio_status_t focus(const std::vector<uint16_t>& ids)
    {
        return ecuio_session_set_focus(s, ids.data(),
                                       static_cast<uint32_t>(ids.size()));
    }
};

/* The ids of defs first to first + n - 1 */
std::vector<uint16_t> ids_from(uint32_t first, uint32_t n)
{
    std::vector<uint16_t> ids;
    for (uint32_t i = 0; i < n; ++i) {
        ids.push_back(id_of(first + i));
    }
    return ids;
}

/* A condition on the def's signal, by its id */
std::string on_def(const char* key, uint32_t def)
{
    return std::string(R"({"key":")") + key + R"(","name":")" + key +
           R"(","when":{"signal":["id:)" + std::to_string(id_of(def)) +
           R"("],"unit":"","above":1}})";
}

ecuio_status_t attach(ecuio_session_t* s, const std::string& list)
{
    const std::string json =
        R"({"havne_conditions":1,"conditions":[)" + list + "]}";
    ecuio_condition_set_t* set = nullptr;
    EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_status_t st = ecuio_session_set_conditions(s, set, nullptr);
    ecuio_condition_set_free(set);
    return st;
}

} // namespace

/* Attach sizes the table from the set and, past a rotation's most,
 * starts it with every request parked: nothing is asked until a focus
 * picks, the unasked list names every signal from the start, and a count
 * of 0 is taken as on any session and parks them all again */
TEST(PickOnlyTest, ASetPastARotationIsAskedOnlyWhatIsPicked)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    const ecuio_request_engine& e = *car.e;
    EXPECT_EQ(e.count, 2 * kMost);
    EXPECT_EQ(e.requests.size(), size_t{2 * kMost})
        << "the table is the set's requests, no more";
    EXPECT_TRUE(e.pick_only);
    EXPECT_TRUE(car.s->focus_pick_only);
    EXPECT_TRUE(car.s->focus_requests);
    EXPECT_EQ(car.s->focus_table, 2 * kMost);
    EXPECT_EQ(car.s->focus_rows[5], 5u);
    EXPECT_EQ(car.s->focus_rows[kMost + 5], kMost + 5);
    EXPECT_EQ(car.s->focus_rows[2 * kMost], 0u) << "it reads def 0's reply";

    /* Before the worker's first pass: every signal, the sharing one too */
    const std::vector<uint16_t> every = ids_from(0, 2 * kMost + 1);
    EXPECT_EQ(car.unasked(), every);
    car.take();
    EXPECT_TRUE(car.asked().empty());
    EXPECT_EQ(car.unasked(), every);
    EXPECT_EQ(e.timing_taken, 0u);

    /* Two of module A's and one of B's */
    ASSERT_EQ(car.focus({id_of(7), id_of(kMost + 3), id_of(2)}), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(car.unasked(), every) << "the worker hasn't taken it yet";
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{2, 7, kMost + 3}));
    std::vector<uint16_t> rest;
    for (uint32_t i = 0; i < 2 * kMost + 1; ++i) {
        if (i != 2 && i != 7 && i != kMost + 3) {
            rest.push_back(id_of(i));
        }
    }
    EXPECT_EQ(car.unasked(), rest);
    EXPECT_EQ(e.chans[0].requests, 2u);
    EXPECT_EQ(e.chans[1].requests, 1u);

    /* The def that shares def 0's reply picks def 0's row */
    ASSERT_EQ(car.focus({id_of(2 * kMost)}), ECUIO_OK);
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{0}));
    const std::vector<uint16_t> now = car.unasked();
    EXPECT_EQ(now.size(), size_t{2 * kMost - 1})
        << "def 0 shares the reply, so it is asked too";
    EXPECT_EQ(now.front(), id_of(1));

    /* A count of 0: nothing, not the whole set */
    ASSERT_EQ(car.focus({}), ECUIO_OK) << ecuio_last_error_detail();
    car.take();
    EXPECT_TRUE(car.asked().empty());
    EXPECT_EQ(car.unasked(), every);
    EXPECT_EQ(e.timing_taken, 4u) << "a request asked once keeps its row";

    /* The mock builds no requests of its own, so its session doesn't
     * carry the flag: only a session that asks does */
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(car.s, &info), ECUIO_OK);
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u);
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_PICK_ONLY, 0u);
}

/* A set that fits a rotation is asked whole from attach, each request
 * with its timing row, and no pick of it is refused for its size */
TEST(PickOnlyTest, ASetThatFitsARotationIsAskedWholeAndRefusesNoPickBySize)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://x?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    auto e = std::make_unique<ecuio_request_engine>();
    request_params params;
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->module_rx.data(),
                                   s->module_tx.data(), s->info.module_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    EXPECT_FALSE(e->pick_only);
    EXPECT_FALSE(s->focus_pick_only);
    EXPECT_EQ(e->requests.size(), size_t{e->count})
        << "the table is the set's requests, no more";
    EXPECT_EQ(e->timing_taken, e->count);
    for (uint32_t i = 0; i < e->count; ++i) {
        EXPECT_EQ(e->timing_of[i], i);
        EXPECT_EQ(e->requests[i].parked, 0u);
    }
    std::vector<uint16_t> all;
    for (uint32_t i = 0; i < set->count; ++i) {
        all.push_back(set->defs[i].id);
    }
    EXPECT_EQ(ecuio_session_set_focus(s, all.data(),
                                      static_cast<uint32_t>(all.size())),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_session_destroy(s);

    /* Exactly a rotation's most, one module's: asked whole as well */
    static ecuio_signal_set_t full = *car_set();
    full.count = kMost;
    full.channel_count = 1;
    ASSERT_EQ(ecuio_profile_register("pick-only-full", &full, nullptr),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_create("mock://x?profile=pick-only-full", &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    auto f = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, &full, params, s->module_rx.data(),
                                   s->module_tx.data(), s->info.module_count,
                                   f.get(), &probe, &identify),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_FALSE(f->pick_only);
    EXPECT_FALSE(s->focus_pick_only);
    EXPECT_EQ(f->count, kMost);
    EXPECT_EQ(f->timing_taken, kMost);
    EXPECT_EQ(f->chans[0].requests, kMost) << "every request in the rotation";
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_profile_unregister("pick-only-full"), ECUIO_OK);
}

/* A focus is refused, with nothing changed, only when its own requests
 * number more than a rotation holds: defs that share a reply count once,
 * and what was asked before counts for nothing */
TEST(PickOnlyTest, AFocusOfMoreRequestsThanARotationIsRefused)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    ASSERT_EQ(car.focus({id_of(1), id_of(2)}), ECUIO_OK);
    car.take();
    const uint32_t generation = car.s->focus_generation.load();

    std::vector<uint16_t> over = ids_from(0, kPool + 1); /* spans both ECUs */
    EXPECT_EQ(car.focus(over), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the picked signals, with the attached conditions', ask 1025 "
                 "requests: a session asks 1024 at a time");
    EXPECT_EQ(car.s->focus_generation.load(), generation) << "nothing changed";
    EXPECT_EQ(car.s->focus_ids, (std::vector<uint16_t>{id_of(1), id_of(2)}));
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{1, 2}));

    /* Exactly a rotation, and the def sharing def 0's reply costs none */
    std::vector<uint16_t> full = ids_from(0, kPool);
    full.push_back(id_of(2 * kMost));
    EXPECT_EQ(car.focus(full), ECUIO_OK) << ecuio_last_error_detail();
    car.take();
    EXPECT_EQ(car.asked().size(), size_t{kPool});
    EXPECT_EQ(car.e->timing_taken, kPool);

    /* Another rotation's worth, none of it asked before: taken, since the
     * count is this focus's alone, and asked with no timing row */
    EXPECT_EQ(car.focus(ids_from(kMost, kPool)), ECUIO_OK)
        << ecuio_last_error_detail();
    car.take();
    EXPECT_EQ(car.asked().size(), size_t{kPool});
    EXPECT_EQ(car.asked().front(), kMost);
    EXPECT_EQ(car.e->untimed, kPool);
}

/* On a session that asks only its picks an attached condition's signals
 * are asked with no focus (ADR-0065 section 8), count in a focus's
 * rotation, and are refused, with nothing changed, when they would take
 * the rotation past its most */
TEST(PickOnlyTest, AConditionsSignalsAreAskedWithNoFocusAndCountInTheRotation)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    car.take();
    ASSERT_TRUE(car.asked().empty());

    /* With no focus: the condition's signal alone */
    const uint32_t before = car.s->focus_generation.load();
    ASSERT_EQ(attach(car.s, on_def("a", 40)), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(car.s->focus_generation.load(), before)
        << "the worker is told, with no focus on";
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{40}));
    const std::vector<uint16_t> unasked = car.unasked();
    EXPECT_EQ(unasked.size(), size_t{2 * kMost});
    EXPECT_EQ(std::count(unasked.begin(), unasked.end(), id_of(40)), 0);

    /* Under a focus: the picks and the condition's */
    ASSERT_EQ(car.focus({id_of(3)}), ECUIO_OK);
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{3, 40}));

    /* The focus's end leaves the condition's signal asked */
    ASSERT_EQ(car.focus({}), ECUIO_OK);
    car.take();
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{40}));

    /* The conditions gone: nothing */
    ASSERT_EQ(ecuio_session_set_conditions(car.s, nullptr, nullptr), ECUIO_OK);
    car.take();
    EXPECT_TRUE(car.asked().empty());

    /* A focus of a whole rotation, then a condition on a signal outside
     * it: one request too many, refused with the focus and the
     * conditions as they were */
    ASSERT_EQ(attach(car.s, on_def("in", 5)), ECUIO_OK);
    ASSERT_EQ(car.focus(ids_from(0, kPool)), ECUIO_OK)
        << "the condition's signal is one of the picks";
    car.take();
    ecuio_condition_state_t states[2] = {};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_condition_states(car.s, states, 2, &count),
              ECUIO_OK);
    ASSERT_EQ(count, 1u);
    const uint32_t held = states[0].generation;
    const uint32_t focus_generation = car.s->focus_generation.load();
    EXPECT_EQ(attach(car.s, on_def("in", 5) + "," + on_def("out", kMost + 9)),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the conditions' signals, with the focus's, ask 1025 "
                 "requests: a session asks 1024 at a time");
    uint32_t after = 0;
    ASSERT_EQ(ecuio_session_get_condition_states(car.s, states, 2, &after),
              ECUIO_OK);
    EXPECT_EQ(after, 1u) << "the set attached before stays";
    EXPECT_EQ(states[0].generation, held) << "no new attach";
    EXPECT_EQ(car.s->focus_generation.load(), focus_generation);
    /* Two conditions on signals the focus already asks: a rotation of
     * exactly its most, taken */
    EXPECT_EQ(attach(car.s, on_def("in", 5) + "," + on_def("too", 6)), ECUIO_OK)
        << ecuio_last_error_detail();

    /* And the other way: with that condition attached first, the focus
     * of a whole rotation is the one refused */
    ASSERT_EQ(car.focus({id_of(1)}), ECUIO_OK);
    ASSERT_EQ(attach(car.s, on_def("out", kMost + 9)), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(car.focus(ids_from(0, kPool)), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the picked signals, with the attached conditions', ask 1025 "
                 "requests: a session asks 1024 at a time");
    EXPECT_EQ(car.focus(ids_from(0, kPool - 1)), ECUIO_OK)
        << ecuio_last_error_detail();
    car.take();
    EXPECT_EQ(car.asked().size(), size_t{kPool});
}

/* The timing report's rows are the pool's, in the order taken, each
 * naming its request; a request asked with the pool spent is counted in
 * untimed and has none */
TEST(PickOnlyTest, TheTimingRowsAreTheRequestsAskedInTheOrderTaken)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    std::unique_ptr<ecuio_timing_table> timing;
    ecuio_attach_timing(*car.e, timing, ECUIO_TIMING_CLOCK_HOST);
    ASSERT_NE(timing, nullptr);
    car.take();
    int64_t published = 0;
    ecuio_publish_timing(car.s, *car.e, nullptr, 1000000, published, true);
    std::vector<ecuio_request_timing_t> rows(kPool);
    ecuio_session_timing_t t{};
    uint32_t count = 77;
    ASSERT_EQ(ecuio_session_get_timing(car.s, &t, rows.data(), kPool, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "nothing asked yet: no row, of 2048 requests";
    EXPECT_EQ(t.rows, 0u);
    EXPECT_EQ(t.untimed, 0u);

    /* Module B's def first in the pick, A's after: the rows follow the
     * table, the order the worker takes them in */
    ASSERT_EQ(car.focus({id_of(kMost + 2), id_of(9)}), ECUIO_OK);
    car.take();
    ASSERT_EQ(car.focus({id_of(4)}), ECUIO_OK);
    car.take();
    /* Row 9's request answered once, a millisecond after it went */
    car.e->chans[0].sent_us = 5000;
    ecuio_engine_time_reply(car.e.get(), 0, 9, true, false, 6000);
    ecuio_publish_timing(car.s, *car.e, nullptr, 2000000, published, true);
    ASSERT_EQ(ecuio_session_get_timing(car.s, &t, rows.data(), kPool, &count),
              ECUIO_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(t.rows, 3u);
    const auto did_of = [&rows](uint32_t i) {
        return (rows[i].request[1] << 8) | rows[i].request[2];
    };
    EXPECT_EQ(rows[0].channel, 0u);
    EXPECT_EQ(did_of(0), 0x1009);
    EXPECT_EQ(rows[0].length, 3u);
    EXPECT_EQ(rows[0].request[0], 0x22);
    EXPECT_EQ(rows[0].answered, 1u);
    EXPECT_EQ(rows[0].latency_max_us, 1000u);
    EXPECT_EQ(rows[1].channel, 1u);
    EXPECT_EQ(did_of(1), 0x1002);
    EXPECT_EQ(rows[1].answered, 0u);
    EXPECT_EQ(rows[2].channel, 0u);
    EXPECT_EQ(did_of(2), 0x1004) << "the later focus's request, after";

    /* The pool spent to its last row (module B's request holds one that
     * this focus doesn't ask), then ten more */
    ASSERT_EQ(car.focus(ids_from(0, kPool - 1)), ECUIO_OK);
    car.take();
    EXPECT_EQ(car.e->timing_taken, kPool);
    EXPECT_EQ(car.e->untimed, 0u);
    ASSERT_EQ(car.focus(ids_from(kMost + 100, 10)), ECUIO_OK);
    car.take();
    ecuio_publish_timing(car.s, *car.e, nullptr, 3000000, published, true);
    ASSERT_EQ(ecuio_session_get_timing(car.s, &t, rows.data(), kPool, &count),
              ECUIO_OK);
    EXPECT_EQ(count, kPool);
    EXPECT_EQ(t.rows, kPool);
    EXPECT_EQ(t.untimed, 10u);
    EXPECT_EQ(did_of(0), 0x1009) << "a row keeps its index";
    EXPECT_EQ(rows[0].answered, 1u);
}

/* The worker's part allocates nothing however large the table: a focus
 * taken, the conditions' signals kept, a publish */
TEST(PickOnlyTest, TheWorkerAllocatesNothingOnALargeTable)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    std::unique_ptr<ecuio_timing_table> timing;
    ecuio_attach_timing(*car.e, timing, ECUIO_TIMING_CLOCK_HOST);
    ASSERT_EQ(attach(car.s, on_def("a", 600)), ECUIO_OK);
    ASSERT_EQ(car.focus(ids_from(3, 500)), ECUIO_OK);
    int64_t published = 0;
    const unsigned long before = news_on_this_thread();
    car.take();
    ecuio_publish_timing(car.s, *car.e, nullptr, 1000000, published, true);
    const unsigned long news = news_on_this_thread() - before;
    EXPECT_EQ(news, 0u);
    EXPECT_EQ(car.asked().size(), 501u);

    ASSERT_EQ(car.focus({}), ECUIO_OK);
    const unsigned long again = news_on_this_thread();
    car.take();
    EXPECT_EQ(news_on_this_thread() - again, 0u);
    EXPECT_EQ(car.asked(), (std::vector<uint32_t>{600}));
}

/* Out of memory anywhere in a focus call or an attach on a large table
 * is answered, never thrown, and leaves the focus and the conditions as
 * they were */
TEST(PickOnlyTest, OutOfMemoryInAFocusOrAnAttachChangesNothing)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    const std::vector<uint16_t> first = {id_of(1), id_of(2)};
    ASSERT_EQ(car.focus(first), ECUIO_OK);
    const std::vector<uint16_t> next = ids_from(10, 300);
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(car.focus(next), ECUIO_OK);
    const unsigned long news = news_on_this_thread() - before;
    ASSERT_GT(news, 2u) << "the picks, their marks and the table's";
    ASSERT_EQ(car.focus(first), ECUIO_OK);
    for (unsigned long n = 1; n <= news; ++n) {
        const uint32_t generation = car.s->focus_generation.load();
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = car.focus(next);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "the focus threw at allocation " << n;
        if (rc == ECUIO_OK) {
            EXPECT_EQ(car.s->focus_ids, next) << "at " << n;
            ASSERT_EQ(car.focus(first), ECUIO_OK);
        } else {
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(car.s->focus_ids, first) << "at " << n;
            EXPECT_EQ(car.s->focus_generation.load(), generation) << "at " << n;
        }
    }

    const std::string json = R"({"havne_conditions":1,"conditions":[)" +
                             on_def("a", 40) + "," + on_def("b", kMost + 4) +
                             "]}";
    ecuio_condition_set_t* set = nullptr;
    ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const unsigned long attach_before = news_on_this_thread();
    ASSERT_EQ(ecuio_session_set_conditions(car.s, set, nullptr), ECUIO_OK);
    const unsigned long attach_news = news_on_this_thread() - attach_before;
    ASSERT_GT(attach_news, 2u);
    ASSERT_EQ(ecuio_session_set_conditions(car.s, nullptr, nullptr), ECUIO_OK);
    for (unsigned long n = 1; n <= attach_news; ++n) {
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_session_set_conditions(car.s, set, nullptr);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "the attach threw at allocation " << n;
        uint32_t held = 77;
        ASSERT_EQ(ecuio_session_get_condition_states(car.s, nullptr, 0, &held),
                  ECUIO_OK);
        if (rc == ECUIO_OK) {
            EXPECT_EQ(held, 2u) << "at " << n;
            ASSERT_EQ(ecuio_session_set_conditions(car.s, nullptr, nullptr),
                      ECUIO_OK);
        } else {
            EXPECT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            EXPECT_EQ(held, 0u) << "at " << n << ": nothing attached";
            EXPECT_TRUE(
                car.s->condition_defs.empty() ||
                std::count(car.s->condition_defs.begin(),
                           car.s->condition_defs.end(), 0) ==
                    static_cast<std::ptrdiff_t>(car.s->condition_defs.size()))
                << "at " << n;
        }
    }
    ecuio_condition_set_free(set);
}

/* With nothing picked and the link down, a worker's idle passes ask one
 * request every ECUIO_UNPICKED_CHECK_US, a module in turn, the first a
 * whole interval after the link was found down; with the link OK they
 * ask nothing. A wait that runs out counts toward a timeout only while
 * no module answers. */
TEST(PickOnlyTest, AnUnpickedSessionWithItsLinkDownChecksAModuleEachInterval)
{
    Car car;
    ASSERT_NE(car.s, nullptr);
    ecuio_request_engine* e = car.e.get();
    car.take();
    ASSERT_TRUE(ecuio_engine_unpicked(e));
    std::atomic<int32_t> published{ECUIO_OK};
    std::atomic<uint32_t> episodes{0};
    link_health h(published, episodes);
    ecuio_unpicked_check check;
    const int64_t every = ECUIO_UNPICKED_CHECK_US;
    EXPECT_EQ(every, 5000000) << "the failure backoff's cap";
    ASSERT_EQ(ecuio_unpicked_check_us().load(), every)
        << "a case before this one left its own interval set";
    const auto due = [&] {
        return (e->chans[0].check_due ? 1u : 0u) |
               (e->chans[1].check_due ? 2u : 0u);
    };

    /* The link OK: pass after pass, nothing is asked */
    int64_t now = 1000;
    for (int i = 0; i < 100; ++i) {
        ecuio_unpicked_pass(e, h, check, now);
        now += every;
    }
    EXPECT_EQ(due(), 0u);
    EXPECT_EQ(check.next_us, 0);
    EXPECT_EQ(published.load(), ECUIO_OK);

    /* Ten waits run out with no module answering: not responding */
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        ecuio_request_timed_out(*e, h);
    }
    ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    ecuio_unpicked_pass(e, h, check, now);
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_EQ(due(), 0u) << "the first check waits a whole interval";
    EXPECT_EQ(check.next_us, now + every);
    ecuio_unpicked_pass(e, h, check, now + every - 1);
    EXPECT_EQ(due(), 0u);
    ecuio_unpicked_pass(e, h, check, now + every);
    EXPECT_EQ(due(), 1u) << "module A's";
    ecuio_unpicked_pass(e, h, check, now + every + 1);
    EXPECT_EQ(due(), 1u) << "one an interval";
    e->chans[0].check_due = false; /* sent, and unanswered */
    ecuio_unpicked_pass(e, h, check, now + 2 * every);
    EXPECT_EQ(due(), 2u) << "then module B's";
    e->chans[1].check_due = false;
    ecuio_unpicked_pass(e, h, check, now + 3 * every);
    EXPECT_EQ(due(), 1u) << "and around";
    e->chans[0].check_due = false;

    /* A module whose requests are all dropped is passed by */
    for (uint32_t i = 0; i < kMost; ++i) {
        ecuio_engine_drop(e, kMost + i, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    }
    ecuio_unpicked_pass(e, h, check, now + 4 * every);
    EXPECT_EQ(due(), 1u) << "module B has nothing to ask";
    e->chans[0].check_due = false;

    /* A reply: the link is OK, and the checks end */
    link_on_read(h, false, true, false);
    ecuio_unpicked_pass(e, h, check, now + 5 * every);
    EXPECT_EQ(due(), 0u);
    EXPECT_EQ(check.next_us, 0);

    /* A test's own interval (internal.h) holds until it is given back */
    {
        struct Short {
            Short() { ecuio_set_unpicked_check_ms(250); }
            ~Short() { ecuio_set_unpicked_check_ms(0); }
        } own;
        for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
            ecuio_request_timed_out(*e, h);
        }
        ASSERT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
        const int64_t at = now + 6 * every;
        ecuio_unpicked_pass(e, h, check, at);
        EXPECT_EQ(check.next_us, at + 250000);
        ecuio_unpicked_pass(e, h, check, at + 249999);
        EXPECT_EQ(due(), 0u);
        ecuio_unpicked_pass(e, h, check, at + 250000);
        EXPECT_EQ(due(), 1u);
        EXPECT_EQ(check.next_us, at + 500000);
        e->chans[0].check_due = false;
        link_on_read(h, false, true, false);
        ecuio_unpicked_pass(e, h, check, at + 250001);
        EXPECT_EQ(check.next_us, 0);
    }
    EXPECT_EQ(ecuio_unpicked_check_us().load(), every) << "0 gives it back";

    /* While a module answers, another's silence is its own */
    const uint8_t reply[4] = {0x62, 0x10, 0x00, 0x2A};
    (void)ecuio_engine_on_frame(e, 0, reply, sizeof reply, now, nullptr);
    ASSERT_TRUE(ecuio_engine_unpicked_answering(e));
    for (uint32_t i = 0; i < 3 * LINK_HEALTH_QUIET_RUN; ++i) {
        ecuio_request_timed_out(*e, h);
    }
    EXPECT_EQ(published.load(), ECUIO_OK);
    EXPECT_FALSE(h.unanswered);

    /* With a pick every wait that runs out counts, as on any session */
    ASSERT_EQ(car.focus({id_of(7)}), ECUIO_OK) << ecuio_last_error_detail();
    car.take();
    ASSERT_FALSE(ecuio_engine_unpicked(e));
    for (uint32_t i = 0; i < LINK_HEALTH_QUIET_RUN; ++i) {
        ecuio_request_timed_out(*e, h);
    }
    EXPECT_EQ(published.load(), ECUIO_ERR_VEHICLE_TIMEOUT);
}

/* A session whose DIDs are all scanned has an empty rotation at attach:
 * it gets its timing all the same, so a DID the scan finds is timed and
 * published once it joins (a scan=first log's latency.csv) */
TEST(ReplyTimingTest, AScanAloneIsTimedOnceItFindsADid)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(
                  "mock://scan?profile=uds-generic&did=7E8:1E20-1E21", &s),
              ECUIO_OK)
        << ecuio_last_error_detail();
    request_params params;
    params.identify = false;
    params.diagnose = false;
    params.scan_first = true;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, ecuio_session_signal_set(s), params,
                                   s->module_rx.data(), s->module_tx.data(),
                                   s->info.module_count, e.get(), &probe,
                                   &identify),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(e->count, 0u) << "nothing to rotate until the scan finds";
    std::unique_ptr<ecuio_timing_table> timing;
    ecuio_attach_timing(*e, timing, ECUIO_TIMING_CLOCK_HOST);
    ASSERT_NE(timing, nullptr);

    /* The ECU answers 1E20 and refuses 1E21, each 4 ms after its request */
    int64_t now = 1000000;
    for (int k = 0; k < 8; ++k, now += ECUIO_ENGINE_WAIT_US) {
        const int32_t r = ecuio_engine_due(e.get(), 0, now);
        if (r == ECUIO_ENGINE_NONE) {
            continue;
        }
        ecuio_engine_sent(e.get(), 0, r, true, now);
        const ecuio_request& q = ecuio_engine_request_of(e.get(), 0, r);
        const bool found =
            q.len == 3 && q.bytes[1] == 0x1E && q.bytes[2] == 0x20;
        const uint8_t positive[] = {0x62, 0x1E, 0x20, 0x05};
        const uint8_t refused[] = {0x7F, 0x22, 0x31};
        (void)ecuio_engine_on_frame(e.get(), 0, found ? positive : refused,
                                    found ? 4 : 3, now + 4000, nullptr);
    }
    ASSERT_EQ(e->count, 1u) << "1E20 found and polled";

    int64_t published = 0;
    ecuio_publish_timing(s, *e, nullptr, now, published, true);
    std::vector<ecuio_request_timing_t> rows(4);
    ecuio_session_timing_t t{};
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_timing(s, &t, rows.data(), 4, &count),
              ECUIO_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(rows[0].request[1], 0x1E);
    EXPECT_EQ(rows[0].request[2], 0x20);
    EXPECT_GE(rows[0].answered, 1u) << "the found DID's own turns are timed";
    EXPECT_EQ(t.untimed, 0u);
    ecuio_session_destroy(s);
}
