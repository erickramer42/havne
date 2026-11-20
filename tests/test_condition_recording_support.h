// File/layer: tests/test_condition_recording_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_condition_recording*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/recording.h>
#include <ecuio/signal_sets.h>
#include "session.h" /* the record link, for a link transition */
#include "sha256.h"
#include "temp_name.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/* test_core.cpp's: operator new's calls on this thread */
unsigned long news_on_this_thread();

namespace {

namespace fs = std::filesystem;

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

/* A set file of the conditions' JSON */
inline std::string set_of(const std::string& conditions)
{
    return R"({"havne_conditions":1,"conditions":[)" + conditions + "]}";
}

/* One condition on engine speed: its key, its test and anything after */
inline std::string on_rpm(const std::string& key, const std::string& test,
                   const std::string& rest = "")
{
    return R"({"key":")" + key + R"(","name":"N )" + key +
           R"(","when":{"signal":["obd2.rpm"],"unit":"RPM",)" + test + "}" +
           rest + "}";
}

inline ecuio_status_t attach_json(ecuio_session_t* s, const std::string& json)
{
    ecuio_condition_set_t* set = nullptr;
    EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_status_t st = ecuio_session_set_conditions(s, set, nullptr);
    ecuio_condition_set_free(set);
    return st;
}

/* Engine speed (PID 0C, a quarter of a turn per count) on 0x7E8 at a session
 * time, dispatched as the worker would */
inline void engine_speed(ecuio_session_t* s, uint64_t t_us, unsigned turns)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    const unsigned raw = turns * 4;
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = t_us;
    f.data[0] = 0x41;
    f.data[1] = 0x0C;
    f.data[2] = static_cast<uint8_t>(raw >> 8);
    f.data[3] = static_cast<uint8_t>(raw & 0xFF);
    f.data_length = 4;
    ecuio_session_dispatch_callbacks(s, &f);
}

/* The session's events, as its conditions callback gave them */
struct Events {
    std::mutex m;
    std::vector<ecuio_condition_event_t> all;
    static void take(const ecuio_condition_event_t* events, uint32_t count,
                     void* user)
    {
        Events* self = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(self->m);
        self->all.insert(self->all.end(), events, events + count);
    }
};

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
               temp_name("ecuio-condition-recording");
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

inline ecuio_recorder_t* open_recorder(const Root& root, uint32_t ring_bytes = 0)
{
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    o.ring_bytes = ring_bytes;
    ecuio_recorder_t* recorder = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
        << ecuio_last_error_detail();
    return recorder;
}

inline fs::path folder_of(const ecuio_recorder_t* recorder)
{
    char buf[1024];
    uint32_t len = 0;
    EXPECT_EQ(ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len),
              ECUIO_OK);
    return fs::u8path(buf);
}

/* A file's bytes as they are */
inline std::string read_bytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* A text file's lines, carriage returns dropped */
inline std::vector<std::string> lines_of(const fs::path& p)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(read_bytes(p));
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

/* events.log's lines of a kind ("cond", "drop"): what follows the kind */
inline std::vector<std::string> kind_lines(const fs::path& folder, const char* kind)
{
    const std::string key = std::string("  ") + kind + "  ";
    std::vector<std::string> out;
    for (const std::string& l : lines_of(folder / "events.log")) {
        const size_t at = l.find(key);
        if (at != std::string::npos && at < 24) {
            out.push_back(l.substr(at + key.size()));
        }
    }
    return out;
}

/* A cond line's seven fields */
struct Cond {
    std::string key, state, flags, value;
    uint64_t term = 0, host_us = 0, t_us = 0;
};

inline std::vector<Cond> cond_lines(const fs::path& folder)
{
    std::vector<Cond> out;
    for (const std::string& l : kind_lines(folder, "cond")) {
        std::vector<std::string> f;
        std::stringstream in(l);
        std::string field;
        while (std::getline(in, field, ',')) {
            f.push_back(field);
        }
        EXPECT_EQ(f.size(), 7u) << l;
        if (f.size() != 7) {
            continue;
        }
        out.push_back(Cond{f[0], f[1], f[2], f[3], std::stoull(f[4]),
                           std::stoull(f[5]), std::stoull(f[6])});
    }
    return out;
}

/* A recording made by hand: set attached, recorder attached, closed */
struct Recorded {
    Root root;
    Session session{"mock://x?profile=obd2"};
    Events events;
    ecuio_recorder_t* recorder = nullptr;
    fs::path folder;

    explicit Recorded(const std::string& json, uint32_t ring_bytes = 0)
    {
        EXPECT_EQ(ecuio_register_conditions_callback(session.s, Events::take,
                                                     &events),
                  ECUIO_OK);
        if (!json.empty()) {
            EXPECT_EQ(attach_json(session.s, json), ECUIO_OK)
                << ecuio_last_error_detail();
        }
        recorder = open_recorder(root, ring_bytes);
        if (recorder != nullptr) {
            folder = folder_of(recorder);
        }
    }
    void record()
    {
        ASSERT_EQ(ecuio_session_record(session.s, recorder), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    /* Until the writer drained what the ring held: a wake counts itself
     * before it reads the hold, so the second wake follows a drain */
    void drained() const
    {
        const uint64_t from = ecuio_recorder_test_wakes(recorder);
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ecuio_recorder_test_wakes(recorder) < from + 2) {
            if (std::chrono::steady_clock::now() >= give_up) {
                ADD_FAILURE() << "the writer never woke twice";
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ecuio_recorder_state_t state() const
    {
        ecuio_recorder_state_t st{};
        EXPECT_EQ(ecuio_recorder_get_state(recorder, &st), ECUIO_OK);
        return st;
    }
    void close()
    {
        if (recorder != nullptr) {
            EXPECT_EQ(ecuio_recorder_close(recorder, "done"), ECUIO_OK);
            recorder = nullptr;
        }
    }
    ~Recorded() { close(); }
};

} // namespace
