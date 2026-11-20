// File/layer: tests/test_condition_session_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_condition_session*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/profile.h>
#include <ecuio/queue.h>
#include <ecuio/recording.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "link_health.h" /* core/src */
#include "session.h"     /* core/src: judges_link */
#include "temp_name.h"
#include "worker_hold.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* test_core.cpp's: operator new's calls on this thread */
unsigned long news_on_this_thread();

namespace {

namespace fs = std::filesystem;
constexpr uint64_t kSecond = 1000000;

struct Session {
    ecuio_session_t* s = nullptr;
    explicit Session(const char* uri)
    {
        EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Session() { ecuio_session_destroy(s); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

inline std::string rpm(const std::string& test)
{
    return R"({"signal":["obd2.rpm"],"unit":"RPM",)" + test + "}";
}

inline std::string condition(int n, const std::string& when,
                      const std::string& rest = "")
{
    const std::string k = std::to_string(n);
    return R"({"key":"c)" + k + R"(","name":"C)" + k + R"(","when":)" + when +
           rest + "}";
}

/* Attaches the conditions (the list's JSON) to the session; the set is
 * freed at once, since the session keeps nothing of it */
inline ecuio_status_t attach(ecuio_session_t* s, const std::string& conditions,
                      const ecuio_signal_file_t* file = nullptr)
{
    const std::string json =
        R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
    ecuio_condition_set_t* set = nullptr;
    EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_status_t st = ecuio_session_set_conditions(s, set, file);
    ecuio_condition_set_free(set);
    return st;
}

/* One obd2 reply on 0x7E8 at a session time, dispatched as the worker
 * would: a PID and its data bytes */
inline void reply(ecuio_session_t* s, double seconds, uint8_t pid, uint8_t a,
           int b = -1)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = static_cast<uint64_t>(seconds * 1e6 + 0.5);
    f.data[0] = 0x41;
    f.data[1] = pid;
    f.data[2] = a;
    f.data_length = 3;
    if (b >= 0) {
        f.data[3] = static_cast<uint8_t>(b);
        f.data_length = 4;
    }
    ecuio_session_dispatch_callbacks(s, &f);
}

/* Engine speed, PID 0C: a quarter of a turn a count */
inline void engine_speed(ecuio_session_t* s, double seconds, unsigned turns)
{
    const unsigned raw = turns * 4;
    reply(s, seconds, 0x0C, static_cast<uint8_t>(raw >> 8),
          static_cast<int>(raw & 0xFF));
}

/* What the callbacks saw, in the order they were called */
struct Seen {
    std::mutex m;
    std::vector<std::string> lines;
    std::vector<ecuio_condition_event_t> events;

    void add(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(m);
        lines.push_back(line);
    }
    std::string text()
    {
        std::lock_guard<std::mutex> lock(m);
        std::string out;
        for (const std::string& l : lines) {
            out += (out.empty() ? "" : "\n") + l;
        }
        lines.clear();
        return out;
    }
};

/* "<time> g<generation> c<condition> on|off [stale] [fired] [link]
 * t<term> v<value> [k<code>]": link for ECUIO_CONDITION_LINK, the code
 * where it isn't 0 */
inline std::string event_text(const ecuio_condition_event_t& e)
{
    char line[160];
    std::snprintf(line, sizeof line, "%llu g%u c%u %s%s%s%s t%u v%g",
                  static_cast<unsigned long long>(e.timestamp_us), e.generation,
                  e.condition, e.state == ECUIO_CONDITION_ON ? "on" : "off",
                  (e.flags & ECUIO_CONDITION_STALE) != 0 ? " stale" : "",
                  (e.flags & ECUIO_CONDITION_FIRED) != 0 ? " fired" : "",
                  (e.flags & ECUIO_CONDITION_LINK) != 0 ? " link" : "", e.term,
                  e.value);
    return e.code == 0 ? line
                       : line + std::string(" k") + std::to_string(e.code);
}

inline void on_conditions(const ecuio_condition_event_t* events, uint32_t count,
                   void* user)
{
    Seen* seen = static_cast<Seen*>(user);
    std::string line = "events";
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_EQ(events[i].arbitration_id, 0u);
        EXPECT_EQ(events[i].reserved0, 0u);
        EXPECT_EQ(events[i].reserved, 0u);
        line += " [" + event_text(events[i]) + "]";
        std::lock_guard<std::mutex> lock(seen->m);
        seen->events.push_back(events[i]);
    }
    seen->add(line);
}

inline void on_values(const ecuio_value_batch_t* batch, void* user)
{
    static_cast<Seen*>(user)->add("values " +
                                  std::to_string(batch->timestamp_us));
}

inline void on_frame(const ecuio_frame_t* frame, void* user)
{
    static_cast<Seen*>(user)->add("frame " +
                                  std::to_string(frame->timestamp_us));
}

/* const: reading the states needs no session it may change */
inline std::vector<ecuio_condition_state_t> states_of(const ecuio_session_t* s)
{
    uint32_t count = 99;
    EXPECT_EQ(ecuio_session_get_condition_states(s, nullptr, 0, &count),
              ECUIO_OK);
    std::vector<ecuio_condition_state_t> out(count);
    /* Into room that holds something else, so a field core leaves
     * unwritten shows */
    if (count > 0) {
        std::memset(out.data(), 0xA5, count * sizeof out[0]);
    }
    uint32_t again = 0;
    EXPECT_EQ(ecuio_session_get_condition_states(s, out.data(), count, &again),
              ECUIO_OK);
    EXPECT_EQ(again, count);
    for (const ecuio_condition_state_t& state : out) {
        EXPECT_EQ(state.reserved[0], 0u);
        EXPECT_EQ(state.reserved[1], 0u);
    }
    return out;
}

/* A scratch folder of the case's own, removed at the end */
struct Root {
    fs::path path;
    Root()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-conditions");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~Root()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_u8string(); }
};

/* A recorder under root, its folder in *folder */
inline ecuio_recorder_t* open_recorder(const Root& root, std::string* folder)
{
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* recorder = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
        << ecuio_last_error_detail();
    if (recorder != nullptr && folder != nullptr) {
        char buf[1024];
        uint32_t len = 0;
        EXPECT_EQ(ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len),
                  ECUIO_OK);
        *folder = buf;
    }
    return recorder;
}

} // namespace
