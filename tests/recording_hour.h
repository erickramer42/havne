// File/layer: tests/recording_hour.h | test-only, C++17, Qt-free,
//             header-only, no gtest
// Testing: HARNESS: RecordingTest's hour (tests/test_recording.cpp), which
//          checks every query against these samples; bench_recording times
//          the reader's open on the folder it writes
//
// An hour of the replay fixture's own values at their own times, written as
// the recorder writes a folder: events.log in format 2 with a summary, and
// signals.csv. Shared so the bench opens the same hour the tests check.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "recording_rows.h" /* core/src */

namespace recording_hour {

const char* const kBom = "\xEF\xBB\xBF";

/* events.log in format 2 with these signal lines, and a summary */
inline std::string
events_v2(const std::vector<std::string>& signal_lines,
          const char* end = "12:00:10.000  +10.000  end  the test ended: 0 "
                            "frames, 0 values, 0 records dropped, 0 notes "
                            "dropped")
{
    std::string s = std::string(ecuio_recording_rows::kFormat2) +
                    "\nstarted  2026-11-03 12:00:00.000 (local time)\n"
                    "utc  2026-11-03T18:00:00.000Z\ncore  0.3.6\n"
                    "uri  mock://x\nprofile  test\nframe flags  complete\n";
    for (const std::string& l : signal_lines) {
        s += "signal  " + l + "\n";
    }
    s += "columns  local time, seconds since the recording started\n\n";
    if (end != nullptr) {
        s += std::string("\n") + end + "\n";
    }
    return s;
}

inline void write_file(const std::filesystem::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

/* A sample as the test wrote it: its row's value, read back from the text
 * it wrote, as the reader reads it */
struct Sample {
    uint64_t us;
    uint32_t signal;
    double value;
};

/* The replay fixture's session replayed under obd2, each batch at its
 * frame's session time, the 297 s pass laid down twelve times 300 s apart */
struct Hour {
    struct Def {
        std::string name;
        std::string unit;
        bool text;
        unsigned decimals;
        unsigned rank;
    };
    std::vector<Def> defs;
    std::vector<Sample> samples;
    size_t values_per_pass = 0;

    /* fixture: the folder holding frames.csv. False with why on a failure */
    static bool make(const std::string& fixture, Hour& h, std::string& why)
    {
        const std::string uri =
            "replay://" + fixture + "?speed=max&profile=obd2";
        ecuio_session_t* session = nullptr;
        if (ecuio_session_create(uri.c_str(), &session) != ECUIO_OK) {
            why = std::string("the replay: ") + ecuio_last_error_detail();
            return false;
        }
        const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
        std::map<uint16_t, uint32_t> by_id;
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            by_id[d.id] = static_cast<uint32_t>(h.defs.size());
            h.defs.push_back(
                Def{std::string(d.name, strnlen(d.name, sizeof d.name)),
                    std::string(d.unit, strnlen(d.unit, sizeof d.unit)),
                    d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX,
                    d.decimal_places, d.plot_rank});
        }
        struct Pass {
            std::map<uint16_t, uint32_t>* by_id;
            std::vector<Sample> got;
            std::atomic<size_t> frames{0};
        } pass{&by_id, {}, {}};
        auto values = [](const ecuio_value_batch_t* b, void* user) {
            auto* p = static_cast<Pass*>(user);
            for (uint32_t i = 0; i < b->count; ++i) {
                if (b->values[i].text == nullptr) {
                    p->got.push_back(Sample{b->timestamp_us,
                                            (*p->by_id)[b->values[i].signal_id],
                                            b->values[i].value});
                }
            }
        };
        auto frames = [](const ecuio_frame_t*, void* user) {
            static_cast<Pass*>(user)->frames.fetch_add(1);
        };
        ecuio_session_info_t info{};
        bool ok =
            ecuio_register_values_callback(session, values, &pass) ==
                ECUIO_OK &&
            ecuio_register_frame_callback(session, frames, &pass) == ECUIO_OK &&
            ecuio_session_start(session) == ECUIO_OK &&
            ecuio_session_get_info(session, &info) == ECUIO_OK;
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (ok && pass.frames.load() < info.replay_frames &&
               std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok = ok && pass.frames.load() >= info.replay_frames;
        ecuio_session_destroy(session); /* joins the worker */
        if (!ok) {
            why = "the replay didn't run to its last frame";
            return false;
        }
        h.values_per_pass = pass.got.size();
        for (uint64_t loop = 0; loop < 12; ++loop) {
            for (const Sample& s : pass.got) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.12g", s.value);
                h.samples.push_back(Sample{s.us + loop * 300000000ull, s.signal,
                                           std::strtod(buf, nullptr)});
            }
        }
        return true;
    }

    /* The folder: events.log's header and summary, signals.csv */
    void write(const std::filesystem::path& dir) const
    {
        std::vector<std::string> lines;
        for (const Def& d : defs) {
            lines.push_back(d.name + (d.text ? ",text," : ",number,") + d.unit +
                            "," + std::to_string(d.decimals) + "," +
                            std::to_string(d.rank));
        }
        write_file(dir / "events.log",
                   events_v2(lines, "13:00:00.000  +3600.000  end  the test "
                                    "ended: 0 frames, 0 values, 0 records "
                                    "dropped, 0 notes dropped"));
        std::string csv =
            std::string(kBom) + ecuio_recording_rows::kSignalsHeader + "\n";
        for (const Sample& s : samples) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%llu,%s,%.12g,%s\n",
                          static_cast<unsigned long long>(s.us),
                          defs[s.signal].name.c_str(), s.value,
                          defs[s.signal].unit.c_str());
            csv += buf;
        }
        write_file(dir / "signals.csv", csv);
    }
};

} // namespace recording_hour
