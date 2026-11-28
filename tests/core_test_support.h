// File/layer: tests/core_test_support.h | test_core, Qt-free
// Testing: UNIT: the includes and the helpers that more than one tests/test_core*.cpp uses

#ifndef ECUIO_TESTS_CORE_TEST_SUPPORT_H
#define ECUIO_TESTS_CORE_TEST_SUPPORT_H

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h> /* _mkdir, _rmdir */
#else
#include <sys/stat.h>
#include <time.h> /* clock_gettime: the platform's clock, for ecuio_clock_us */
#include <unistd.h>
#endif

extern "C" {
#include "ecuio/version.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "ecuio/profile.h"
#include "ecuio/conditions.h"
#include "ecuio/vin.h"
}

/* C++ headers (namespaced classes and inline functions): must stay
 * outside the extern "C" block above. */
#include "fake_diagnostics.h"
#include "error_detail.h" /* core/src: a refused vendor call's words */
#include "link_health.h"  /* core/src: the workers' link-health rules */
#include "replay_faults.h" /* core/src: ?faults= and its draws */
#include "mock_drive_cycle.h"
#include "mock_ecu.h"      /* core/src: the simulated ECU's answers */
#include "mock_pids.h"     /* core/src: the demo car's Service 01 */
#include "def_index.h"     /* core/src: the defs a frame can match */
#include "precise_wait.h"  /* core/src: a requesting worker's waits */
#include "request_setup.h" /* core/src: the requests and the focus's map */
#include "session.h"       /* core/src: the session's focus fields */
#include "uds_generic.h"   /* core/src: the template set's identity */
#include "surrogate_log_names.h" /* core/src: the surrogate logs pruning keeps */
#include "temp_name.h"     /* tests: a scratch name of this process's own */

/* test_core.cpp defines these two and includes new_counter.h, which replaces
 * operator new, so one source per binary does; the other parts count through them */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);
#ifndef ECUIO_TESTS_NEW_COUNTER_H
namespace new_counter {
inline unsigned long on_this_thread() { return news_on_this_thread(); }
inline void fail_new(unsigned long n) { fail_new_on_this_thread(n); }
} // namespace new_counter
#endif

namespace core_test {


inline std::atomic<int> g_callback_count{0};

inline void counting_callback(const ecuio_frame_t* frame, void* user_data)
{
    (void)frame;
    int* counter = static_cast<int*>(user_data);
    if (counter) {
        (*counter)++;
    }
    g_callback_count++;
}

/* Helper to create test session */
inline ecuio_session_t* make_test_session(void)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://test", &session), ECUIO_OK);
    return session;
}

/* Poll-until-predicate with deadline. The mock's 50ms cadence is a
 * LOCAL expectation, not a CI-runner guarantee: a starved macOS VM
 * can deliver 2 frames in 200+ ms of wall clock. Polling keeps the
 * asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). Deadline is long
 * enough to absorb pathological runner load, short enough that a
 * genuinely dead transport still fails the test promptly. */
template <typename Predicate>
inline bool wait_for(Predicate pred, int timeout_ms, int poll_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return pred();   // final check past the deadline, then give up
}


/* The mock's physical envelope across a whole drive cycle. Shared by
 * the worker test below, which sees only the first second of the
 * cycle, and the deterministic cycle tests, which see all of it. */
constexpr float kMockRpmMin = 0.0f; /* key on, the engine stopped */
constexpr float kMockRpmMax = 7100.0f;
constexpr float kMockCoolantMin = 15.0f;
constexpr float kMockCoolantMax = 110.0f;
/* 4th gear's limiter speed, 233 km/h, under the wire's 255 */
constexpr float kMockSpeedMax = 240.0f;
constexpr float kMockIatMin = 15.0f;
constexpr float kMockIatMax = 80.0f;
constexpr float kMockMafMax =
    500.0f; /* full boost near peak power reads ~445 */
constexpr float kMockOilMin = 15.0f;    /* cold start reads 20 */
constexpr float kMockOilMax = 140.0f;
constexpr float kMockLambdaMin = 0.8f; /* full boost's enrichment reads ~0.82 */
constexpr float kMockLambdaMax = 2.0f;  /* fuel cut pegs the sensor lean */
constexpr float kMockMapMin = 15.0f;    /* a closed throttle's most vacuum */
constexpr float kMockBaroMin = 95.0f;   /* the outside air near sea level */
constexpr float kMockBaroMax = 105.0f;
/* Boost, MAP minus baro: never under a closed throttle's most vacuum,
 * never above what the wastegate holds */
constexpr float kMockBoostMin = kMockMapMin - kMockBaroMax;
constexpr float kMockBoostMax = ecuio_mock::kBoostMaxKpa;
constexpr float kMockMapMax = kMockBaroMax + kMockBoostMax;
/* Battery voltage: the starter's deepest sag to a cold alternator's output */
constexpr float kMockBatteryMin = 9.0f;
constexpr float kMockBatteryMax = 14.8f;

using ecuio_mock::DriveCycle;
using ecuio_mock::Phase;

/* A scratch recording in the temp folder, named for the running test and
 * removed at the end. path is absolute and uses forward slashes, which
 * the replay path accepts on every platform. */
struct TempRecording {
    std::string dir;
    std::string csv;
    bool made_dir = false;

    explicit TempRecording(const std::string& text, bool as_folder = false)
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr)
            tmp = std::getenv("TMPDIR");
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\')
                c = '/';
        }
        while (base.size() > 1 && base.back() == '/')
            base.pop_back();
        const std::string name = temp_name("ecuio-replay");
        if (as_folder) {
            dir = base + "/" + name;
#ifdef _WIN32
            made_dir = _mkdir(dir.c_str()) == 0 || errno == EEXIST;
#else
            made_dir = mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
#endif
            csv = dir + "/frames.csv";
        } else {
            csv = base + "/" + name + ".csv";
        }
        FILE* f = std::fopen(csv.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    ~TempRecording()
    {
        std::remove(csv.c_str());
        if (made_dir) {
#ifdef _WIN32
            _rmdir(dir.c_str());
#else
            rmdir(dir.c_str());
#endif
        }
    }
    std::string uri(const char* query = "") const
    {
        return "replay://" + (dir.empty() ? csv : dir) + query;
    }
};

const char* const kFramesHeader = "host_us,timestamp_us,id,flags,data\n";

/* Every frame a replay delivers, and when it arrived */
struct ReplayCapture {
    std::mutex mutex;
    std::vector<ecuio_frame_t> frames;
    std::vector<std::chrono::steady_clock::time_point> arrived;
    /* The callback holds the worker this long at this frame (a late
     * frame); 0 never */
    size_t stall_at = 0;
    int stall_ms = 0;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }
};

inline void replay_capture_cb(const ecuio_frame_t* frame, void* user_data)
{
    auto* c = static_cast<ReplayCapture*>(user_data);
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(c->mutex);
        c->frames.push_back(*frame);
        c->arrived.push_back(std::chrono::steady_clock::now());
        n = c->frames.size();
    }
    if (c->stall_at != 0 && n == c->stall_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds(c->stall_ms));
    }
}

/* Creates, registers and starts a replay session, or fails the test */
inline ecuio_session_t* start_replay(const std::string& uri, ReplayCapture* capture)
{
    ecuio_session_t* session = nullptr;
    EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    if (session == nullptr)
        return nullptr;
    EXPECT_EQ(
        ecuio_register_frame_callback(session, replay_capture_cb, capture),
        ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    return session;
}

/* Rows at step_us steps of session time, 0x7E8 and 41 0C 1F 40 each */
inline std::string paced_rows(int n, int step_us = 100000)
{
    std::string text = kFramesHeader;
    for (int i = 0; i < n; ++i) {
        const std::string t = std::to_string(5000000 + int64_t{step_us} * i);
        text += t + "," + t + ",0x7E8,0x0,41 0C 1F 40\n";
    }
    return text;
}

/* Reads the worker's own operator new count at two frames past its
 * start-up; the callback itself allocates nothing */
struct WorkerNews {
    static constexpr int kFrom = 3;
    int to = 0;
    std::atomic<int> frames{0};
    std::atomic<unsigned long> at_from{0};
    std::atomic<unsigned long> at_to{0};
    int batches = 0; /* values batches, counted on the worker */
};

inline void worker_news_values_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++static_cast<WorkerNews*>(user_data)->batches;
}

inline void worker_news_cb(const ecuio_frame_t*, void* user_data)
{
    auto* w = static_cast<WorkerNews*>(user_data);
    const int n = w->frames.fetch_add(1) + 1;
    if (n == WorkerNews::kFrom)
        w->at_from.store(new_counter::on_this_thread());
    if (n == w->to)
        w->at_to.store(new_counter::on_this_thread());
}



/* One delivered value, copied out of its batch */
struct SeenValue {
    uint16_t id = 0;
    uint8_t kind = 0;
    double value = 0.0;
    std::string text;
    const char* text_at = nullptr; /* where the batch held it */
    uint32_t text_len = 0;
};

struct SeenBatch {
    uint64_t timestamp_us = 0;
    uint32_t arbitration_id = 0;
    std::vector<SeenValue> values;
};

/* Every batch a values callback received */
struct ValuesCapture {
    std::mutex mutex;
    std::vector<SeenBatch> batches;
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return batches.size();
    }
};

inline void values_capture_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    SeenBatch b;
    b.timestamp_us = batch->timestamp_us;
    b.arbitration_id = batch->arbitration_id;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        SeenValue s;
        s.id = v.signal_id;
        s.kind = v.kind;
        s.value = v.value;
        s.text_at = v.text;
        s.text_len = v.text_len;
        if (v.text != nullptr)
            s.text = v.text;
        b.values.push_back(s);
    }
    auto* c = static_cast<ValuesCapture*>(user_data);
    std::lock_guard<std::mutex> lock(c->mutex);
    c->batches.push_back(std::move(b));
}

inline void values_counting_cb(const ecuio_value_batch_t*, void* user_data)
{
    ++*static_cast<int*>(user_data);
}

inline ecuio_signal_def_t value_def(uint16_t id, uint8_t kind, uint32_t message_id)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.kind = kind;
    d.message_id = message_id;
    d.scale = 1.0;
    std::snprintf(d.name, sizeof(d.name), "V%u", static_cast<unsigned>(id));
    return d;
}

/* A text def reading byte_length bytes from start, keyed when key_len */
inline ecuio_signal_def_t text_value_def(uint16_t id, uint8_t kind, uint8_t key_len,
                                  uint32_t key, uint16_t start,
                                  uint16_t byte_length)
{
    ecuio_signal_def_t d = value_def(id, kind, 0x123);
    d.mux_length = key_len;
    d.mux_key = key;
    d.start_byte = start;
    d.byte_length = byte_length;
    return d;
}

inline ecuio_frame_t value_frame(uint32_t id, std::initializer_list<uint8_t> bytes,
                          uint64_t timestamp_us = 0)
{
    ecuio_frame_t f{};
    f.arbitration_id = id;
    f.timestamp_us = timestamp_us;
    for (uint8_t b : bytes)
        f.data[f.data_length++] = b;
    return f;
}



/* MAP on 0x123's byte 0, a fuel-system enum on its byte 1 (1 and 2
 * labeled), baro on 0x124's byte 0; then boost (MAP - baro, baro aged
 * up to 30 s) and a ratio (MAP / baro, both the stale age) */
struct ComputedSet {
    ecuio_signal_def_t defs[5];
    ecuio_enum_entry_t enums[2];
    ecuio_computed_t computed[2];
    ecuio_signal_set_t set{};

    ComputedSet()
    {
        defs[0] = value_def(1, ECUIO_VALUE_UINT, 0x123);
        defs[0].bit_length = 8;
        defs[1] = value_def(2, ECUIO_VALUE_ENUM, 0x123);
        defs[1].start_byte = 1;
        defs[1].bit_length = 8;
        defs[1].table_count = 2;
        defs[2] = value_def(3, ECUIO_VALUE_UINT, 0x124);
        defs[2].bit_length = 8;
        for (uint16_t i = 3; i < 5; ++i) {
            defs[i] = value_def(static_cast<uint16_t>(i + 1), ECUIO_VALUE_UINT, 0);
            defs[i].flags = ECUIO_DEF_FLAG_COMPUTED;
            defs[i].table_first = static_cast<uint16_t>(i - 3);
        }
        enums[0].raw = 1;
        std::snprintf(enums[0].label, sizeof enums[0].label, "Open loop");
        enums[1].raw = 2;
        std::snprintf(enums[1].label, sizeof enums[1].label, "Closed loop");
        for (uint32_t p = 0; p < 2; ++p) {
            ecuio_computed_t& c = computed[p];
            c = ecuio_computed_t{};
            c.input_ids[0] = 1;
            c.input_ids[1] = 3;
            c.input_count = 2;
            c.step_count = 3;
            c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
            c.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
            c.steps[2] = p == 0 ? ECUIO_COMPUTED_SUBTRACT : ECUIO_COMPUTED_DIVIDE;
        }
        computed[0].max_age_us[1] = 30000000;
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 5;
        set.defs = defs;
        set.enum_count = 2;
        set.enums = enums;
        set.computed_count = 2;
        set.computed = computed;
    }
};

inline const SeenValue* value_of(const SeenBatch& b, uint16_t id)
{
    for (const SeenValue& v : b.values) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}



/* ADR-0062's chain: numbers L1 (id 1, 0x123), L2 (id 2, 0x124), L3 (id
 * 3, 0x125), then computed defs listed top first: C3 (id 13) = C2 + L3,
 * C2 (id 12) = C1 * 2, C1 (id 11) = L1 - L2, L2 bound to 1 s there */
struct ChainSet {
    ecuio_signal_def_t defs[6];
    ecuio_computed_t computed[3];
    ecuio_signal_set_t set{};

    ChainSet()
    {
        for (uint16_t i = 0; i < 3; ++i) {
            defs[i] = value_def(static_cast<uint16_t>(i + 1), ECUIO_VALUE_UINT,
                                0x123u + i);
            defs[i].bit_length = 8;
        }
        const uint16_t ids[3] = {13, 12, 11};
        for (uint16_t k = 0; k < 3; ++k) {
            defs[3 + k] = value_def(ids[k], ECUIO_VALUE_UINT, 0);
            defs[3 + k].flags = ECUIO_DEF_FLAG_COMPUTED;
            defs[3 + k].table_first = k;
            computed[k] = ecuio_computed_t{};
        }
        const auto two = [](ecuio_computed_t& c, uint16_t a, uint16_t b,
                            uint8_t op) {
            c.input_ids[0] = a;
            c.input_ids[1] = b;
            c.input_count = 2;
            c.steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
            c.steps[1] = ECUIO_COMPUTED_PUSH_INPUT | 1u;
            c.steps[2] = op;
            c.step_count = 3;
        };
        two(computed[0], 12, 3, ECUIO_COMPUTED_ADD);
        computed[1].input_ids[0] = 11;
        computed[1].input_count = 1;
        computed[1].constants[0] = 2.0;
        computed[1].constant_count = 1;
        computed[1].steps[0] = ECUIO_COMPUTED_PUSH_INPUT | 0u;
        computed[1].steps[1] = ECUIO_COMPUTED_PUSH_CONSTANT | 0u;
        computed[1].steps[2] = ECUIO_COMPUTED_MULTIPLY;
        computed[1].step_count = 3;
        two(computed[2], 1, 2, ECUIO_COMPUTED_SUBTRACT);
        computed[2].max_age_us[1] = 1000000;
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = 6;
        set.defs = defs;
        set.computed_count = 3;
        set.computed = computed;
    }
};

/* A session under a ChainSet registered as name, its values captured */
struct ChainRun {
    ecuio_session_t* session = nullptr;
    ValuesCapture cap;

    explicit ChainRun(const char* name)
    {
        const std::string uri = std::string("mock://t?profile=") + name;
        EXPECT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK);
        EXPECT_EQ(
            ecuio_register_values_callback(session, values_capture_cb, &cap),
            ECUIO_OK);
    }
    ~ChainRun() { ecuio_session_destroy(session); }
    ChainRun(const ChainRun&) = delete;
    ChainRun& operator=(const ChainRun&) = delete;

    /* One frame of byte on L(n)'s ID at seconds; its batch's ids */
    std::vector<uint16_t> send(uint32_t n, uint8_t byte, double seconds)
    {
        const ecuio_frame_t f =
            value_frame(0x122u + n, {byte},
                        static_cast<uint64_t>(std::llround(seconds * 1e6)));
        const size_t before = cap.batches.size();
        ecuio_session_dispatch_callbacks(session, &f);
        std::vector<uint16_t> ids;
        if (cap.batches.size() > before) {
            for (const SeenValue& v : cap.batches.back().values) {
                ids.push_back(v.id);
            }
        }
        return ids;
    }
    double value(uint16_t id) const
    {
        const SeenValue* v = value_of(cap.batches.back(), id);
        return v != nullptr ? v->value : -1.0;
    }
};



/* The session's unsupported ids, all of them */
inline std::vector<uint16_t> unsupported_ids(const ecuio_session_t* session)
{
    uint32_t count = 0;
    EXPECT_EQ(
        ecuio_session_get_unsupported_signals(session, nullptr, 0, &count),
        ECUIO_OK);
    std::vector<uint16_t> ids(count);
    if (count > 0) {
        EXPECT_EQ(ecuio_session_get_unsupported_signals(session, ids.data(),
                                                        count, &count),
                  ECUIO_OK);
    }
    return ids;
}


/* The PIDs the demo car's ECU answers (sim_ecu.h): the drive cycle's,
 * and the diagnostics' readiness and counts since a clear */
const std::vector<uint8_t> kDemoPids = {
    0x01, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x21,
    0x24, 0x30, 0x31, 0x33, 0x41, 0x42, 0x44, 0x46, 0x4D, 0x4E, 0x5C};


/* The ids and values of a batch, in its order */
inline std::vector<std::pair<uint16_t, double>> id_values(const SeenBatch& b)
{
    std::vector<std::pair<uint16_t, double>> out;
    for (const SeenValue& v : b.values) {
        out.push_back({v.id, v.value});
    }
    return out;
}

inline uint64_t unsplit(const ecuio_session_t* session)
{
    uint64_t n = 99;
    EXPECT_EQ(ecuio_session_get_unsplit_replies(session, &n), ECUIO_OK);
    return n;
}


} // namespace core_test

#endif
