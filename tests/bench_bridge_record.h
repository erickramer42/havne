// File/layer: tests/bench_bridge_record.h | Qt6 Core, benchmark, C++17
// Testing: none of its own; part of bench_bridge (tests/bench_bridge.cpp)
// --record: the recorder's cost to the worker, on pinned cores.

#pragma once

#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "sessionbridge.h"
#include "ecuio/internal.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "bench_bridge_common.h"
#include "bench_bridge_load.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace {

// The recorder's cost to the worker (ADR-0049 section 9), measured on
// core's own session, since the bridge keeps its own recorder: the
// fixture replayed under obd2 by core's worker to a frame callback that
// counts and a values callback that does nothing, as the bridge's.
// Recorded runs (a recorder with its default ring attached before start)
// alternate with unrecorded ones, kRecordRounds of each, the process held
// to two physical cores (pinToCores), and their medians are compared.
// A frame costs this worker so little that the recorder's fixed cost per
// frame is a large share of it here, and that share follows the rest of
// the worker's speed; section 9's 10% is judged on the bridge's run,
// where the worker does a frame's real work. This check bounds the
// recorder's own cost, the nanoseconds a frame it adds, against
// regressions. Then one recorded pass at full speed and one at 100x must
// put every frame and value in the files, none dropped
constexpr int kRecordRounds = 5;
constexpr double kMinBridgeRecordedShare = 0.9; // ADR-0049 section 9's 10%
constexpr double kMaxRecordedNs = 75.0; // dev: 42 to 47, Windows and Linux

struct CoreRun {
    double workerRate = 0.0;     // looping: frames a second through the worker
    double seconds = 0.0;        // one pass: until the last frame's callback
    double writtenSeconds = 0.0; // one pass: until the writer had every row
    bool passed = true;          // one pass: every frame came, in time
    ecuio_recorder_state_t rec{};
};

void countFrame(const ecuio_frame_t*, void* n)
{
    static_cast<std::atomic<quint64>*>(n)->fetch_add(1,
                                                     std::memory_order_relaxed);
}

double since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
        .count();
}

void sleepFor(double seconds)
{
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

#if defined(__linux__)
int readSysInt(const char* format, int cpu)
{
    char path[128];
    std::snprintf(path, sizeof(path), format, cpu);
    FILE* f = std::fopen(path, "r");
    int v = -1;
    if (f) {
        if (std::fscanf(f, "%d", &v) != 1)
            v = -1;
        std::fclose(f);
    }
    return v;
}
#endif

// The process held to one logical processor on each of n physical
// cores, so its busy threads (the worker, the writer, the bridge's GUI
// thread) never share a core: otherwise where the OS puts them decides
// the figures (a busy thread on the worker's own core slowed it by about
// 40%, on another by about 4%). Its processors in *which;
// false where it can't be done. Threads started later inherit it, the
// sessions' among them
bool pinToCores(int n, std::string* which)
{
#if defined(_WIN32)
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
        bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (info.empty() || !GetLogicalProcessorInformation(info.data(), &bytes))
        return false;
    DWORD_PTR mask = 0;
    int cores = 0;
    for (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION& i : info) {
        if (i.Relationship != RelationProcessorCore || i.ProcessorMask == 0)
            continue;
        mask |= i.ProcessorMask & (~i.ProcessorMask + 1); // its first
        if (++cores == n)
            break;
    }
    if (cores < n || !SetProcessAffinityMask(GetCurrentProcess(), mask))
        return false;
    char text[64];
    std::snprintf(text, sizeof(text), "processor mask 0x%llx",
                  static_cast<unsigned long long>(mask));
    *which = text;
    return true;
#elif defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return false;
    std::vector<std::pair<int, int>> seen; // (package, core)
    cpu_set_t pick;
    CPU_ZERO(&pick);
    std::string names;
    for (int c = 0; c < CPU_SETSIZE && static_cast<int>(seen.size()) < n; ++c) {
        if (!CPU_ISSET(c, &allowed))
            continue;
        const std::pair<int, int> id{
            readSysInt(
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                c),
            readSysInt("/sys/devices/system/cpu/cpu%d/topology/core_id", c)};
        if (id.first < 0 || id.second < 0)
            return false;
        if (std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        seen.push_back(id);
        CPU_SET(c, &pick);
        names += (names.empty() ? "processors " : ", ") + std::to_string(c);
    }
    if (static_cast<int>(seen.size()) < n ||
        sched_setaffinity(0, sizeof(pick), &pick) != 0)
        return false;
    *which = names;
    return true;
#else
    (void)n;
    (void)which;
    return false;
#endif
}

// One replay of the fixture by core's worker; a recording under root when
// root isn't null
CoreRun coreReplay(const char* query, bool sustained, const char* root)
{
    const std::string uri = std::string("replay://") + ECUIO_REPLAY_FIXTURES +
                            "/2026-10-25-engine-start?profile=obd2&" + query;
    ecuio_session_t* session = nullptr;
    if (ecuio_session_create(uri.c_str(), &session) != ECUIO_OK) {
        std::fprintf(stderr, "create %s failed: %s\n", uri.c_str(),
                     ecuio_last_error_detail());
        std::exit(1);
    }
    std::atomic<quint64> frames{0};
    ecuio_register_frame_callback(session, countFrame, &frames);
    ecuio_register_values_callback(session, ignoreValues, nullptr);
    ecuio_recorder_t* recorder = nullptr;
    if (root) {
        ecuio_record_options_t o{};
        o.root_utf8 = root;
        if (ecuio_recorder_open(&o, &recorder) != ECUIO_OK ||
            ecuio_session_record(session, recorder) != ECUIO_OK) {
            std::fprintf(stderr, "recording %s failed: %s\n", uri.c_str(),
                         ecuio_last_error_detail());
            std::exit(1);
        }
    }
    CoreRun run;
    const auto t0 = std::chrono::steady_clock::now();
    ecuio_session_start(session);
    if (sustained) {
        sleepFor(0.5);
        const quint64 frames0 = frames.load();
        const auto t1 = std::chrono::steady_clock::now();
        sleepFor(kSustainSeconds);
        run.workerRate =
            static_cast<double>(frames.load() - frames0) / since(t1);
    } else {
        while (frames.load() < kFixtureFrames && since(t0) < 30.0)
            sleepFor(0.001);
        run.seconds = since(t0);
        run.passed = frames.load() == kFixtureFrames;
        while (recorder && since(t0) < 30.0) {
            ecuio_recorder_get_state(recorder, &run.rec);
            if (run.rec.frame_rows + run.rec.records_dropped >=
                    kFixtureFrames &&
                run.rec.value_rows >= kFixtureValues)
                break;
            sleepFor(0.001);
        }
        run.writtenSeconds = since(t0);
    }
    if (recorder)
        ecuio_recorder_get_state(recorder, &run.rec);
    ecuio_session_close(session);
    ecuio_session_destroy(session);
    if (recorder)
        ecuio_recorder_close(recorder, "the bench's run ended");
    return run;
}

// The recording test: a markdown table, then each failed check; false if
// any
bool recordTest()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "no temporary folder for the recordings\n");
        std::exit(1);
    }
    const char* loop = "speed=max&loop=1";
    // The bridge's looping run first, recorded and not, alternated, on
    // three cores (ADR-0049 section 9's 10%: the app's worker path)
    std::string pinnedBridge;
    const bool heldBridge = pinToCores(3, &pinnedBridge);
    std::vector<double> bridgePlain, bridgeRecorded;
    quint64 bridgeDropped = 0;
    for (int i = 0; i < kRecordRounds; ++i) {
        LoadRun run;
        run.query = loop;
        run.sustained = true;
        replayFixture(run);
        bridgePlain.push_back(run.workerRate);
        LoadRun recordedRun;
        recordedRun.query = loop;
        recordedRun.sustained = true;
        recordedRun.recorded = true;
        replayFixture(recordedRun);
        bridgeRecorded.push_back(recordedRun.workerRate);
        bridgeDropped += recordedRun.rec.records_dropped;
    }
    const double bridgeShare = median(bridgeRecorded) / median(bridgePlain);

    std::string pinned;
    const bool held = pinToCores(2, &pinned);
    const QByteArray root = dir.path().toUtf8();
    std::vector<double> plain, recorded;
    quint64 loopWritten = 0, loopDropped = 0;
    for (int i = 0; i < kRecordRounds; ++i) {
        plain.push_back(coreReplay(loop, true, nullptr).workerRate);
        const CoreRun r = coreReplay(loop, true, root.constData());
        recorded.push_back(r.workerRate);
        loopWritten += r.rec.frame_rows;
        loopDropped += r.rec.records_dropped;
    }
    const CoreRun fast = coreReplay("speed=max", false, root.constData());
    const CoreRun paced = coreReplay("speed=100", false, root.constData());

    const double share = median(recorded) / median(plain);
    const double costNs = 1e9 / median(recorded) - 1e9 / median(plain);
    std::printf("\n### Recording: the engine-start fixture under obd2, the "
                "bridge's worker\n\n"
                "%d looping runs of %.0f s each way through a SessionBridge, "
                "alternated, %s; frames a second through the worker, median "
                "(lowest to highest).\n\n",
                kRecordRounds, kSustainSeconds,
                heldBridge ? ("held to " + pinnedBridge).c_str()
                           : "NOT held to three cores, so where the OS put the "
                             "threads moves the figures");
    std::printf("| Run | Worker frames/s |\n|---|---|\n");
    auto rate = [](const char* label, const std::vector<double>& v) {
        std::printf("| %s | %.0f (%.0f to %.0f) |\n", label, median(v),
                    *std::min_element(v.begin(), v.end()),
                    *std::max_element(v.begin(), v.end()));
    };
    rate("not recorded", bridgePlain);
    rate("recorded", bridgeRecorded);
    std::printf("\nRecorded at %.2f of the unrecorded rate (%llu records "
                "dropped).\n",
                bridgeShare, static_cast<unsigned long long>(bridgeDropped));

    std::printf("\n### Recording: the engine-start fixture under obd2, core's "
                "worker\n\n"
                "%d looping runs of %.0f s each way, alternated, %s; frames a "
                "second through the worker, median (lowest to highest).\n\n",
                kRecordRounds, kSustainSeconds,
                held ? ("held to " + pinned).c_str()
                     : "NOT held to two cores, so where the OS put the "
                       "threads moves the figures");
    std::printf("| Run | Worker frames/s | Rows written | Records dropped |\n"
                "|---|---|---|---|\n");
    auto row = [](const char* label, const std::vector<double>& v,
                  const std::string& written, const std::string& dropped) {
        std::printf("| %s | %.0f (%.0f to %.0f) | %s | %s |\n", label,
                    median(v), *std::min_element(v.begin(), v.end()),
                    *std::max_element(v.begin(), v.end()), written.c_str(),
                    dropped.c_str());
    };
    row("not recorded", plain, "-", "-");
    row("recorded", recorded, std::to_string(loopWritten),
        std::to_string(loopDropped));
    std::printf("\nRecorded at %.2f of the unrecorded rate: %.0f ns a frame "
                "more.\n\n",
                share, costNs);
    std::printf(
        "| Recorded pass | Last frame, s | Files whole, s | frames.csv rows | "
        "signals.csv rows | Records dropped |\n|---|---|---|---|---|---|\n");
    const std::pair<const char*, const CoreRun*> passes[] = {
        {"full speed", &fast}, {"100x", &paced}};
    for (const auto& p : passes) {
        std::printf(
            "| %s | %.3f | %.3f | %llu | %llu | %llu |\n", p.first,
            p.second->seconds, p.second->writtenSeconds,
            static_cast<unsigned long long>(p.second->rec.frame_rows),
            static_cast<unsigned long long>(p.second->rec.value_rows),
            static_cast<unsigned long long>(p.second->rec.records_dropped));
    }

    std::vector<std::string> failed;
    char bound[160];
    std::snprintf(bound, sizeof(bound),
                  "the bridge's run recorded at %.2f of its unrecorded rate, "
                  "under %.2f",
                  bridgeShare, kMinBridgeRecordedShare);
    if (bridgeShare < kMinBridgeRecordedShare)
        failed.push_back(bound);
    std::snprintf(bound, sizeof(bound),
                  "recording added %.0f ns a frame, over %.0f", costNs,
                  kMaxRecordedNs);
    if (costNs > kMaxRecordedNs)
        failed.push_back(bound);
    for (const auto& p : passes) {
        const CoreRun& r = *p.second;
        const std::string at = std::string(p.first) + ": ";
        if (!r.passed)
            failed.push_back(at + "not every frame reached the callback");
        if (r.rec.records_dropped != 0)
            failed.push_back(at + "records dropped");
        if (r.rec.frame_rows != kFixtureFrames ||
            r.rec.value_rows != kFixtureValues)
            failed.push_back(at + "the files missed rows");
        if (r.rec.state != ECUIO_RECORDING_ACTIVE)
            failed.push_back(at + "the recording failed: " + r.rec.reason);
    }
    if (failed.empty()) {
        std::printf("\nEvery check passed: the bridge's run recorded at least "
                    "%.2f of its unrecorded rate; recording added at most %.0f "
                    "ns a frame to core's worker; each recorded pass wrote "
                    "every frame and value, none dropped.\n",
                    kMinBridgeRecordedShare, kMaxRecordedNs);
        return true;
    }
    std::printf("\n");
    for (const std::string& f : failed)
        std::printf("**FAILED:** %s\n\n", f.c_str());
    return false;
}

} // namespace
