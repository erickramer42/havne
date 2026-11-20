// File/layer: tests/test_recorder_support.h | test_core, Qt-free, C++17
// Testing: UNIT: the helpers of test_recorder*.cpp (test_core)

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "link_health.h" /* core/src: a worker's link transitions */
#include "recorder.h"    /* core/src: the signal file's bytes */
#include "recording_rows.h" /* core/src: a focus line's fields */
#include "session.h"     /* core/src: the session's record link */
#include "sha256.h"      /* core/src: the file's hash */
#include "temp_name.h"

/* test_core.cpp's, around tests/new_counter.h (included there alone) */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace fs = std::filesystem;

namespace {

template <typename Predicate> bool eventually(Predicate pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/* A recordings root of the case's own, removed at the end */
struct Root {
    fs::path path;
    Root()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr)
            tmp = std::getenv("TMPDIR");
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-recorder");
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

/* A file's text, carriage returns dropped (the files are text mode) */
inline std::string read_text(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::string out;
    for (char c : s) {
        if (c != '\r')
            out += c;
    }
    return out;
}

inline std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        out.push_back(line);
    return out;
}

/* The case's profile: a number at half a count a bit, and a text value */
ecuio_signal_def_t kDefs[2];
ecuio_signal_set_t kSet;
constexpr uint32_t kNumberId = 0x100;
constexpr uint32_t kTextId = 0x101;

inline void register_profile()
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
    std::strcpy(kDefs[0].unit, "\xC2\xB0"
                               "C");
    kDefs[0].decimal_places = 1;
    kDefs[0].plot_rank = 2;
    kDefs[0].group = ECUIO_SIGNAL_GROUP_FUEL;
    kDefs[1].id = 2;
    kDefs[1].message_id = kTextId;
    kDefs[1].kind = ECUIO_VALUE_TEXT;
    std::strcpy(kDefs[1].name, "Words");
    kSet = ecuio_signal_set_t{};
    kSet.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    kSet.count = 2;
    kSet.defs = kDefs;
    ASSERT_EQ(ecuio_profile_register("rec-test", &kSet, nullptr), ECUIO_OK);
    done = true;
}

inline void dispatch(ecuio_session_t* s, uint32_t id, const std::string& bytes,
              uint32_t flags = 0)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = id;
    f.flags = flags;
    f.data_length = static_cast<uint16_t>(bytes.size());
    std::memcpy(f.data, bytes.data(), bytes.size());
    ecuio_session_dispatch_callbacks(s, &f);
}

struct Recording {
    ecuio_session_t* session = nullptr;
    ecuio_recorder_t* recorder = nullptr;
    std::string folder;

    Recording(const Root& root, const char* uri, uint32_t ring_bytes = 0,
              const char* label = nullptr, std::vector<const char*> header = {},
              uint32_t note_lines = 0, bool attach_now = true)
    {
        EXPECT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_record_options_t o{};
        o.ring_bytes = ring_bytes;
        o.note_lines = note_lines;
        const std::string keep = root.utf8();
        o.root_utf8 = keep.c_str();
        o.label_utf8 = label;
        o.header_lines = header.empty() ? nullptr : header.data();
        o.header_line_count = static_cast<uint32_t>(header.size());
        EXPECT_EQ(ecuio_recorder_open(&o, &recorder), ECUIO_OK)
            << ecuio_last_error_detail();
        if (recorder != nullptr) {
            char buf[1024];
            uint32_t len = 0;
            EXPECT_EQ(
                ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len),
                ECUIO_OK);
            folder = buf;
        }
        if (session != nullptr && recorder != nullptr && attach_now) {
            attach();
        }
    }

    void attach()
    {
        EXPECT_EQ(ecuio_session_record(session, recorder), ECUIO_OK)
            << ecuio_last_error_detail();
    }

    /* The session destroyed, then the recorder closed, as the app does */
    ecuio_status_t finish(const char* why = "the test ended")
    {
        ecuio_session_destroy(session);
        session = nullptr;
        const ecuio_status_t st = ecuio_recorder_close(recorder, why);
        recorder = nullptr;
        return st;
    }

    ~Recording()
    {
        ecuio_session_destroy(session);
        ecuio_recorder_close(recorder, nullptr);
    }

    std::string file(const char* name) const
    {
        return read_text(fs::u8path(folder) / name);
    }
};

/* events.log's lines of one kind: their text, and their seconds since
 * the start in ms */
struct EventLine {
    uint64_t ms;
    std::string text;
};

inline std::vector<EventLine> events_of(const std::string& events, const char* kind)
{
    std::vector<EventLine> out;
    const std::string mark = std::string("  ") + kind + "  ";
    for (const std::string& line : lines_of(events)) {
        const size_t plus = line.find("  +");
        const size_t at = line.find(mark);
        if (plus == std::string::npos || at == std::string::npos || at < plus)
            continue;
        const std::string secs = line.substr(plus + 3, at - plus - 3);
        const size_t dot = secs.find('.');
        if (dot == std::string::npos)
            continue;
        out.push_back(EventLine{std::stoull(secs.substr(0, dot)) * 1000 +
                                    std::stoull(secs.substr(dot + 1)),
                                line.substr(at + mark.size())});
    }
    return out;
}

/* The writer held and parked at its check. A wake counts itself, then
 * reads the hold, so a loop that read it before the hold went in can
 * still drain, however long it was held up first: the frames the case
 * puts in next would make room. A second wake after the hold starts a
 * loop that begins wholly after it (two, not one: the hold and the count
 * are separate atomics). A wake comes every 10 ms busy, 100 ms idle */
inline void hold_writer(ecuio_recorder_t* r)
{
    ecuio_recorder_test_hold(r, 1);
    const uint64_t from = ecuio_recorder_test_wakes(r);
    ASSERT_TRUE(eventually(
        [&] { return ecuio_recorder_test_wakes(r) >= from + 2; }, 10000))
        << "the writer woke " << ecuio_recorder_test_wakes(r) - from
        << " times in 10 s after the hold, not twice";
}

} // namespace
