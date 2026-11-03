// File/layer: tests/bench_recording.cpp | bench_recording, Qt-free, C++17
// Testing: none (a measurement, not in ctest): the reader's open (ADR-0049
//          section 9), on the laptop and, built with the NDK, on the test
//          phone; docs/testing.md "Performance" records its figures
//
// bench_recording [--fixture <dir>] [--scratch <dir>] [--runs N] [--bound S]
// bench_recording --open <recording> [--runs N] [--bound S]
//
// Writes two hours into a folder of its own under scratch (TEMP or TMPDIR
// by default), opens each N times (5 by default) and prints each open's
// median, fastest and slowest, the folder's rows and size, and the
// reader's table against section 5's bound:
//   - the fixture's hour: the replay fixture (--fixture, the folder holding
//     frames.csv) looped to an hour under obd2, as RecordingTest checks it
//     (recording_hour.h), the hour section 9's bound is on;
//   - a dense hour: 50 number signals at 10 Hz each, for the margin.
// --open times a recording already on disk instead. Each open after the
// first finds the files in the OS's cache, as an open right after a
// session does. Exits 1 when a median passes the bound (2 s by default:
// the laptop's; the phone's is 10).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/recording.h"
#include "recording_hour.h"

namespace fs = std::filesystem;
using recording_hour::Hour;
using recording_hour::Sample;

namespace {

/* 50 number signals, each sampled every 100 ms, staggered 1 ms apart */
Hour dense_hour()
{
    Hour h;
    const unsigned kSignals = 50;
    for (unsigned k = 0; k < kSignals; ++k) {
        char name[16];
        std::snprintf(name, sizeof name, "Dense%02u", k);
        h.defs.push_back(Hour::Def{name, "u", false, 2, k < 4 ? k + 1 : 0});
    }
    h.samples.reserve(36000u * kSignals);
    for (uint64_t tick = 0; tick < 36000; ++tick) {
        for (unsigned k = 0; k < kSignals; ++k) {
            const double v =
                1000.0 +
                1000.0 * std::sin(static_cast<double>(tick) / (50.0 + k));
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.2f", v);
            h.samples.push_back(
                Sample{tick * 100000 + k * 1000, k, std::strtod(buf, nullptr)});
        }
    }
    h.values_per_pass = h.samples.size();
    return h;
}

/* Section 5's bound: 32 bytes per number signal per second it has samples
 * in, 16 per text signal's, 16 per second for the index */
uint64_t bound_of(const Hour& h)
{
    std::set<std::pair<uint32_t, uint64_t>> signal_seconds;
    std::set<uint64_t> seconds;
    uint64_t bytes = 0;
    for (const Sample& s : h.samples) {
        if (signal_seconds.emplace(s.signal, s.us / 1000000).second) {
            bytes += h.defs[s.signal].text ? 16 : 32;
        }
        if (seconds.insert(s.us / 1000000).second) {
            bytes += 16;
        }
    }
    return bytes;
}

double mb(uint64_t bytes)
{
    return static_cast<double>(bytes) / 1e6;
}

/* Opens the folder runs times; false when one fails or the median passes
 * bound_s. bound is section 5's table bound, 0 when unknown */
bool time_open(const char* label, const fs::path& folder, unsigned runs,
               double bound_s, uint64_t bound)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(folder / "signals.csv", ec);
    std::vector<double> ms;
    uint64_t rows = 0;
    uint64_t table = 0;
    uint32_t signals = 0;
    for (unsigned i = 0; i < runs; ++i) {
        ecuio_recording_t* r = nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        const ecuio_status_t rc = ecuio_recording_open(
            folder.generic_u8string().c_str(), nullptr, nullptr, &r);
        const auto t1 = std::chrono::steady_clock::now();
        if (rc != ECUIO_OK) {
            std::printf("%s: the open failed: %s (%s)\n", label,
                        ecuio_status_string(rc), ecuio_last_error_detail());
            return false;
        }
        ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
        ecuio_recording_info_t info{};
        ecuio_recording_get_info(r, &info);
        signals = info.signal_count;
        rows = 0;
        for (uint32_t k = 0; k < info.signal_count; ++k) {
            ecuio_recording_signal_t s{};
            ecuio_recording_get_signal(r, k, &s);
            rows += s.samples;
        }
        table = ecuio_recording_test_table_bytes(r);
        ecuio_recording_close(r);
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    std::printf("%s: %llu rows, %u signals, signals.csv %.1f MB\n", label,
                static_cast<unsigned long long>(rows), signals, mb(size));
    std::printf("  open: median %.1f ms of %u (fastest %.1f, slowest %.1f; "
                "the first %.1f)\n",
                median, runs, sorted.front(), sorted.back(), ms.front());
    if (bound != 0) {
        std::printf("  table: %.2f MB, bound %.2f MB%s\n", mb(table), mb(bound),
                    table <= bound ? "" : "  OVER THE BOUND");
    } else {
        std::printf("  table: %.2f MB\n", mb(table));
    }
    const bool ok =
        median <= bound_s * 1000.0 && (bound == 0 || table <= bound);
    if (median > bound_s * 1000.0) {
        std::printf("  OVER THE %.0f s BOUND\n", bound_s);
    }
    return ok;
}

fs::path default_scratch()
{
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr) {
        tmp = std::getenv("TMPDIR");
    }
    return fs::path(tmp != nullptr ? tmp : "/tmp");
}

int usage()
{
    std::fprintf(stderr,
                 "usage: bench_recording [--fixture <dir>] [--scratch <dir>] "
                 "[--runs N] [--bound S]\n"
                 "       bench_recording --open <recording> [--runs N] "
                 "[--bound S]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::string fixture;
#ifdef ECUIO_REPLAY_FIXTURES
    fixture = std::string(ECUIO_REPLAY_FIXTURES) + "/2026-10-25-engine-start";
#endif
    fs::path scratch = default_scratch();
    std::string open;
    unsigned runs = 5;
    double bound_s = 2.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) {
            return usage();
        }
        const char* v = argv[++i];
        if (a == "--fixture") {
            fixture = v;
        } else if (a == "--scratch") {
            scratch = fs::u8path(v);
        } else if (a == "--open") {
            open = v;
        } else if (a == "--runs") {
            runs = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--bound") {
            bound_s = std::strtod(v, nullptr);
        } else {
            return usage();
        }
    }
    if (runs == 0 || bound_s <= 0) {
        return usage();
    }
    if (!open.empty()) {
        return time_open("the recording", fs::u8path(open), runs, bound_s, 0)
                   ? 0
                   : 1;
    }

    const fs::path root = scratch / "ecuio-bench-recording";
    std::error_code ec;
    fs::remove_all(root, ec);
    bool ok = true;
    struct Case {
        const char* label;
        const char* folder;
        Hour hour;
    };
    std::vector<Case> cases;
    Hour fixture_hour;
    std::string why;
    if (!Hour::make(fixture, fixture_hour, why)) {
        std::fprintf(stderr, "the fixture's hour: %s\n", why.c_str());
        return 2;
    }
    cases.push_back(
        Case{"the fixture's hour", "fixture", std::move(fixture_hour)});
    cases.push_back(Case{"a dense hour", "dense", dense_hour()});
    for (const Case& c : cases) {
        const fs::path folder = root / c.folder;
        fs::create_directories(folder, ec);
        if (ec) {
            std::fprintf(stderr, "can't make %s: %s\n",
                         folder.generic_u8string().c_str(),
                         ec.message().c_str());
            return 2;
        }
        c.hour.write(folder);
        ok = time_open(c.label, folder, runs, bound_s, bound_of(c.hour)) && ok;
    }
    fs::remove_all(root, ec);
    return ok ? 0 : 1;
}
