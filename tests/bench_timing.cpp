// File/layer: tests/bench_timing.cpp | test-only, C++17, Qt-free, benchmark
// Testing: none of its own; a measurement, not a test. Not registered
//          with ctest. docs/testing.md ("Reply timing") records its
//          results and how to run it.
//
// Reply timing (ADR-0042 section 4) against the fakes' ECU through each
// transport this platform has: an obd2 session (no identification, no
// diagnostics: the rotation alone) for N seconds through
//   - J2534 (Windows): the fake DLL as the VX (ECUIO_FAKE_VX), through
//     the surrogate, each reply 4 ms after its write;
//   - ELM327 (every platform): the fake ELM327 behind the port opener,
//     each reply 4 ms after its request;
//   - SocketCAN (Linux, vcan0 up with can-isotp): the fakes' ECU through
//     the kernel's ISO-TP, answering at once.
// Each prints its session's timing report: what the stack adds to the
// fake's delay, per transport. The fakes answer from canned bytes at a
// fixed delay, so the spread is the stack's, not an ECU's. On Windows
// the bench holds a 1 ms timer, as the surrogate does: the fake ELM327
// waits out its delay in this process, and at the default 15.6 ms step
// its 4 ms read 16.4; at 1 ms it reads 4.6 to 5.1, a timed wait still
// running up to a millisecond long. The worker adds none of it: with a
// fake spinning to its reply time, at the default step, ELM327 read 4.0
// to 4.1 ms (2026-11-01).
//
// Run: build/tests/Release/bench_timing.exe [seconds per transport, default 60]

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "fake_elm327.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include "vcan_bus.h"
#endif
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

const char* clock_name(uint32_t clock)
{
    switch (clock) {
    case ECUIO_TIMING_CLOCK_HOST:
        return "host";
    case ECUIO_TIMING_CLOCK_ADAPTER:
        return "adapter";
    case ECUIO_TIMING_CLOCK_KERNEL:
        return "kernel";
    default:
        return "none";
    }
}

// A quantile in ms, or "n/a" with too few samples
std::string ms(uint32_t us, bool has)
{
    if (!has) {
        return "n/a";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(us) / 1000.0);
    return buf;
}

// One session for seconds, then its report
bool run(const char* label, const std::string& uri, int seconds,
         const char* delay)
{
    ecuio_session_t* s = nullptr;
    if (ecuio_session_create(uri.c_str(), &s) != ECUIO_OK ||
        ecuio_session_start(s) != ECUIO_OK) {
        std::printf("%s: didn't connect: %s\n", label,
                    ecuio_last_error_detail());
        ecuio_session_destroy(s);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    ecuio_session_close(s);
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    const bool ok = ecuio_session_get_timing(s, &t, rows.data(),
                                             static_cast<uint32_t>(rows.size()),
                                             &count) == ECUIO_OK;
    ecuio_session_destroy(s);
    if (!ok) {
        std::printf("%s: no timing report\n", label);
        return false;
    }
    rows.resize(std::min<size_t>(count, rows.size()));

    uint64_t answered = 0, timeouts = 0, packed = 0;
    uint32_t asked = 0;
    uint32_t p50_low = UINT32_MAX, p50_high = 0, p99_high = 0, max_high = 0;
    bool p99 = false;
    const ecuio_request_timing_t* rpm = nullptr;
    for (const ecuio_request_timing_t& r : rows) {
        if (r.answered + r.refused + r.timeouts == 0) {
            continue;
        }
        ++asked;
        answered += r.answered;
        timeouts += r.timeouts;
        packed += r.packed;
        if (r.answered + r.refused > 0) {
            p50_low = std::min(p50_low, r.latency_p50_us);
            p50_high = std::max(p50_high, r.latency_p50_us);
            max_high = std::max(max_high, r.latency_max_us);
        }
        if ((r.flags & ECUIO_TIMING_LATENCY_P99) != 0) {
            p99 = true;
            p99_high = std::max(p99_high, r.latency_p99_us);
        }
        if (r.length == 2 && r.request[0] == 0x01 && r.request[1] == 0x0C) {
            rpm = &r;
        }
    }
    std::printf("| %s | %s | %d s | %u | %llu (%llu packed) | %llu | %s to %s "
                "| %s | %s |",
                label, delay, seconds, asked,
                static_cast<unsigned long long>(answered),
                static_cast<unsigned long long>(packed),
                static_cast<unsigned long long>(timeouts),
                ms(p50_low, asked > 0).c_str(), ms(p50_high, asked > 0).c_str(),
                ms(p99_high, p99).c_str(), ms(max_high, asked > 0).c_str());
    if (rpm != nullptr) {
        std::printf(" %u: %s / %s / %s, max %s (%s, %u left out) |",
                    rpm->intervals,
                    ms(rpm->interval_p50_us, rpm->intervals > 0).c_str(),
                    ms(rpm->interval_p99_us,
                       (rpm->flags & ECUIO_TIMING_INTERVAL_P99) != 0)
                        .c_str(),
                    ms(rpm->interval_p999_us,
                       (rpm->flags & ECUIO_TIMING_INTERVAL_P999) != 0)
                        .c_str(),
                    ms(rpm->interval_max_us, rpm->intervals > 0).c_str(),
                    clock_name(t.interval_clock), rpm->intervals_mixed);
    } else {
        std::printf(" n/a |");
    }
    if ((t.flags & ECUIO_TIMING_ISOTP) != 0) {
        std::printf(" %u messages, %u segmented, %u dropped, %u unusable |\n",
                    t.isotp_completed, t.isotp_first_frames, t.isotp_dropped,
                    t.isotp_malformed);
    } else {
        std::printf(" the adapter's |\n");
    }
    return true;
}

void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 60;
#ifdef _WIN32
    /* The fakes' delays at their length (the header); released at exit */
    timeBeginPeriod(1);
#endif
    const std::string query = "?profile=obd2&identify=0&diagnose=0";
    std::printf(
        "| Transport | Fake's delay | Run | Requests | Answers | Timeouts "
        "| Latency P50, ms (lowest to highest request) "
        "| Latency P99, ms (highest) | Latency max, ms "
        "| PID 0C's intervals: P50 / P99 / P99.9, ms (clock) | ISO-TP |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
    bool ok = true;

#ifdef _WIN32
    set_env("ECUIO_FAKE_VX", "1");
    ok = run("J2534 (the fake as the VX)",
             std::string("j2534://") + FAKE_J2534_DLL_PATH + query, seconds,
             "4 ms") &&
         ok;
    set_env("ECUIO_FAKE_VX", "");
#endif

    /* The wire logs go to a folder of the run's own, so the user's in
     * TEMP aren't pruned */
    std::error_code ec;
    const std::filesystem::path logs =
        std::filesystem::temp_directory_path(ec) / "havne-bench-timing";
    std::filesystem::create_directories(logs, ec);
    ecuio_elm327_set_log_dir(logs.string().c_str());
    {
        fake_elm327::installed fake;
        fake_elm327::with(
            [](fake_elm327::adapter& a) { a.k.reply_delay_ms = 4; });
#ifdef _WIN32
        const char* port = "COM7";
#else
        const char* port = "/dev/fake-elm327";
#endif
        ok = run("ELM327 (the fake)", std::string("elm327://") + port + query,
                 seconds, "4 ms") &&
             ok;
    }
    ecuio_elm327_set_log_dir(nullptr);
    std::filesystem::remove_all(logs, ec);

#if defined(__linux__) && !defined(__ANDROID__)
    std::string why;
    if (vcan_bus::available(&why)) {
        vcan_bus::Ecu ecu;
        ok = run("SocketCAN (vcan0, the kernel's ISO-TP)",
                 std::string("socketcan://") + vcan_bus::interface_name() +
                     query,
                 seconds, "none") &&
             ok;
    } else {
        std::printf("SocketCAN: skipped, %s\n", why.c_str());
    }
#endif
    return ok ? 0 : 1;
}
