// File/layer: tests/test_condition_recording.cpp | test_core, Qt-free, C++17
// Testing: HARNESS: ConditionRecordingTest (test_core)
//
// A session's conditions and its recording (ADR-0063 section 8): a set
// never changes under a recorder that is attaching or attached. Sessions
// are never started, so each case is the worker and every time its own.

#include <gtest/gtest.h>
#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>
#include <ecuio/recording.h>
#include "temp_name.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

/* A set of one condition on engine speed, keyed key */
ecuio_status_t attach(ecuio_session_t* s, const std::string& key)
{
    const std::string json =
        R"({"havne_conditions":1,"conditions":[{"key":")" + key +
        R"(","name":"N","when":{"signal":["obd2.rpm"],"unit":"RPM",)"
        R"("above":2000}}]})";
    ecuio_condition_set_t* set = nullptr;
    EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_status_t st = ecuio_session_set_conditions(s, set, nullptr);
    ecuio_condition_set_free(set);
    return st;
}

uint32_t generation_of(ecuio_session_t* s)
{
    ecuio_condition_state_t state{};
    uint32_t count = 0;
    EXPECT_EQ(ecuio_session_get_condition_states(s, &state, 1, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 1u);
    return state.generation;
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

ecuio_recorder_t* open_recorder(const Root& root)
{
    ecuio_record_options_t o{};
    const std::string keep = root.utf8();
    o.root_utf8 = keep.c_str();
    ecuio_recorder_t* recorder = nullptr;
    EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
        << ecuio_last_error_detail();
    return recorder;
}

/* ecuio_session_test_before_record_attach for a scope */
struct BeforeAttach {
    BeforeAttach(void (*hook)(void*), void* user)
    {
        ecuio_session_test_before_record_attach(hook, user);
    }
    ~BeforeAttach()
    {
        ecuio_session_test_before_record_attach(nullptr, nullptr);
    }
    BeforeAttach(const BeforeAttach&) = delete;
    BeforeAttach& operator=(const BeforeAttach&) = delete;
};

/* A set attached from another thread while the recorder attaches */
struct Rival {
    ecuio_session_t* session = nullptr;
    std::thread thread;
    std::atomic<int> rc{-1};
    std::atomic<bool> returned_in_the_hook{false};
};

void attach_from_a_rival(void* user)
{
    Rival* r = static_cast<Rival*>(user);
    r->thread = std::thread([r] { r->rc.store(attach(r->session, "b")); });
    /* Long enough for an attach that doesn't wait to return many times
     * over; one that waits for the link's mutex is still waiting */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r->returned_in_the_hook.store(r->rc.load() != -1);
}

} // namespace

TEST(ConditionRecordingTest, ASetAttachedDuringARecordersAttachIsRefused)
{
    Session s("mock://x?profile=obd2");
    ASSERT_NE(s.s, nullptr);
    ASSERT_EQ(attach(s.s, "a"), ECUIO_OK);
    ASSERT_EQ(generation_of(s.s), 1u);

    Root root;
    ecuio_recorder_t* recorder = open_recorder(root);
    ASSERT_NE(recorder, nullptr);
    Rival rival;
    rival.session = s.s;
    {
        BeforeAttach hook(attach_from_a_rival, &rival);
        EXPECT_EQ(ecuio_session_record(s.s, recorder), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    rival.thread.join();
    EXPECT_FALSE(rival.returned_in_the_hook.load())
        << "the set's attach didn't wait for the recorder's";
    EXPECT_EQ(rival.rc.load(), ECUIO_ERR_IN_USE)
        << "a set changed under an attaching recorder";
    EXPECT_EQ(generation_of(s.s), 1u)
        << "the session judges another set than the recording's";
    EXPECT_EQ(ecuio_recorder_close(recorder, "done"), ECUIO_OK);
}
