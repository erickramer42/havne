// File/layer: tests/bench_bridge_load.h | Qt6 Core, benchmark, C++17
// Testing: none of its own; part of bench_bridge (tests/bench_bridge.cpp)
// --load: the fixture replayed through the bridge, its bounds and ratios.

#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "sessionbridge.h"
#include "ecuio/conditions.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "bench_bridge_common.h"
#include "bench_bridge_frame.h"

namespace {

// The committed engine-start recording replayed through the bridge under
// obd2. A one-pass run ends when every frame is handled or dropped; a
// sustained one (loop=1) after kSustainSeconds, its rates taken from a
// half-second in
constexpr quint64 kFixtureFrames = 7074;
constexpr double kFixturePeakRpm = 1261.25;
constexpr double kSustainSeconds = 3.0;
// The checks' bounds: far below the dev machine's figures, so a shared
// CI runner passes and a regression of an order of magnitude doesn't
constexpr double kMinWorkerFramesPerSecond = 10000.0;   // dev: about 370,000
constexpr double kMaxPacedValueP99Ms = 1000.0;   // dev: about 112, a tick and a bit
constexpr double kMaxPacedFrameP99Ms = 500.0;    // dev: under 1
// The ratios, which a slow runner moves on both sides alike: decode at
// 1000 defs against 9 (dev: 1.26 to 1.94x), and the looping run's rate
// under obd2 against raw (dev: 0.69 to 0.76); a run of the
// decode ratio is kDecodeFrames frames
constexpr double kMaxDecodeRatio = 3.0;
constexpr double kMinDecodingShare = 0.5;
// The looping run recorded through core's recorder against the same run
// not recorded, the faster of two each (a runner's noise only slows a
// run): ADR-0049 section 9's CI gate for a gross regression, a cost of a
// microsecond or more a frame; the 10%, and the recorder's own
// nanoseconds a frame, are judged on the dev machine by --record's
// pinned runs. On a shared runner the share measures the runner as much
// as the recorder: the writer is a third busy thread on two cores, and
// the bridge's worker rate follows the GUI thread's through the
// in-flight gate, so holding the worker to a core of its own moved the
// share above 1 rather than steadying it (0.94 to 1.13 on two of the dev
// machine's cores). Over 63 CI runs each, Linux read 0.79 to 0.95
// (median 0.89) and Windows 0.68 to 0.95 (median 0.80, 4 under 0.75, one
// under 0.70), most of the spread runner to runner: a job's two recorded
// runs agree within a few percent. A loaded Linux runner has read 0.67.
// The 0.6 floor leaves room for that load, and a 25% loss from a typical
// figure still trips it: about 2 us a frame on Windows' typical runner
// (220,000 frames a second unrecorded), 1 us on its fastest.
constexpr double kMinRecordedLoopShare = 0.6;
constexpr int kDecodeFrames = 100000;

std::string typicalConditions();
std::string largestConditions();

struct LoadRun {
    const char* label = "";
    const char* query = "";
    const char* profile = "obd2";
    bool sustained = false;
    bool recorded = false;       // core's recorder attached (ADR-0049)
    double seconds = 0.0;        // one pass: until every frame is handled or dropped
    double workerRate = 0.0;     // sustained: frames a second through the worker
    double handledRate = 0.0;    // sustained: frames a second the GUI thread handled
    int handled = 0;
    double peakRpm = 0.0;
    SessionBridge::DeliveryStats stats;
    ecuio_recorder_state_t rec{}; // recorded: one pass once every frame is
                                  // written, looping at the run's end
    std::string conditions;       // a condition file the bridge saves and
                                  // judges; "" for none
};

// HAVNE_SETTINGS_DIR at a scratch folder for a run, as it was after
struct ScratchSettings {
    QTemporaryDir dir;
    bool had = qEnvironmentVariableIsSet("HAVNE_SETTINGS_DIR");
    QByteArray before = qgetenv("HAVNE_SETTINGS_DIR");
    ScratchSettings() { qputenv("HAVNE_SETTINGS_DIR", dir.path().toUtf8()); }
    ~ScratchSettings() { restore(); }
    ScratchSettings(const ScratchSettings&) = delete;
    ScratchSettings& operator=(const ScratchSettings&) = delete;
    void restore()
    {
        if (had) {
            qputenv("HAVNE_SETTINGS_DIR", before);
        } else {
            qunsetenv("HAVNE_SETTINGS_DIR");
        }
    }
};

// A run that can't go on: std::exit runs no destructor
[[noreturn]] void failRun(ScratchSettings& settings)
{
    settings.restore();
    settings.dir.remove();
    std::exit(1);
}

void replayFixture(LoadRun& run)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=%2&%3")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES),
                 QString::fromLatin1(run.profile),
                 QString::fromLatin1(run.query));
    // The bridge's saved set (ConditionStore) in a scratch folder
    ScratchSettings settings;
    SessionBridge bridge;
    if (!run.conditions.empty()) {
        const QVariantMap saved =
            bridge.saveConditions(QString::fromStdString(run.conditions));
        if (!saved.value(QStringLiteral("ok")).toBool()) {
            std::fprintf(
                stderr, "the bench's conditions weren't saved: %s\n",
                qPrintable(saved.value(QStringLiteral("words")).toString()));
            failRun(settings);
        }
    }
    QTemporaryDir recordings;
    if (run.recorded) {
        bridge.setRecordingsRootForTesting(recordings.path());
        bridge.setRecording(true);
        bridge.setRecordReplaysForTesting(true);
    }
    SignalListModel* model = bridge.signalModel();
    QObject::connect(model, &SignalListModel::valuesUpdated, model, [&]() {
        for (const QVariant& s : model->chartData()) {
            const QVariantMap m = s.toMap();
            if (m.value(QStringLiteral("name")).toString() != QStringLiteral("RPM"))
                continue;
            const QVariantList samples = m.value(QStringLiteral("samples")).toList();
            for (qsizetype i = 1; i < samples.size(); i += 2)
                run.peakRpm = std::max(run.peakRpm, samples.at(i).toDouble());
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    const QString err = bridge.connectSession(uri);
    if (!err.isEmpty()) {
        std::fprintf(stderr, "connect %s failed: %s\n", qPrintable(uri), qPrintable(err));
        failRun(settings);
    }
    if (!run.conditions.empty() &&
        (bridge.conditionsGenerationForTesting() != 1 ||
         bridge.conditionModel()->rowCount() == 0)) {
        std::fprintf(stderr, "the bench's conditions weren't attached\n");
        failRun(settings);
    }
    if (run.sustained) {
        spin(0.5);
        const quint64 frames0 = bridge.deliveryStats().frames;
        const int handled0 = bridge.frameCount();
        const auto t1 = std::chrono::steady_clock::now();
        spin(kSustainSeconds);
        const double dt =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        run.stats = bridge.deliveryStats();
        run.workerRate = static_cast<double>(run.stats.frames - frames0) / dt;
        run.handledRate = static_cast<double>(bridge.frameCount() - handled0) / dt;
        run.seconds = dt;
        if (bridge.recorderForTesting() != nullptr)
            ecuio_recorder_get_state(bridge.recorderForTesting(), &run.rec);
    } else {
        while (static_cast<quint64>(bridge.frameCount() + bridge.droppedFrames()) <
               kFixtureFrames)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        run.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        spin(0.25);   // two more ticks: the last window's values reach the model
        run.stats = bridge.deliveryStats();
        // Every frame written, or the writer's 100 ms wakes have had ample time
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (bridge.recorderForTesting() != nullptr &&
               std::chrono::steady_clock::now() < until) {
            ecuio_recorder_get_state(bridge.recorderForTesting(), &run.rec);
            if (run.rec.frame_rows + run.rec.records_dropped >= kFixtureFrames)
                break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }
    run.handled = bridge.frameCount();
    bridge.disconnectSession();
    while (bridge.disconnecting())
        QCoreApplication::processEvents();
}

double ms(uint64_t us) { return static_cast<double>(us) / 1000.0; }

// Core's match and decode alone, microseconds a frame (the median of
// kRuns), for a profile of n defs: the decode ratio's two sides
double decodeMicros(int n)
{
    std::vector<ecuio_signal_def_t> defs = makeDefs(n);
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(n);
    set.defs = defs.data();
    const std::string name = "gate" + std::to_string(n);
    if (ecuio_profile_register(name.c_str(), &set, nullptr) != ECUIO_OK) {
        std::fprintf(stderr, "%s refused\n", name.c_str());
        std::exit(1);
    }
    const std::string uri = "mock://bench?profile=" + name;
    const int ids = (n + kDefsPerId - 1) / kDefsPerId;
    std::vector<double> runs;
    for (int r = 0; r < kRuns; ++r) {
        ecuio_session_t* session = nullptr;
        if (ecuio_session_create(uri.c_str(), &session) != ECUIO_OK) {
            std::fprintf(stderr, "create %s failed\n", uri.c_str());
            std::exit(1);
        }
        ecuio_register_values_callback(session, ignoreValues, nullptr);
        runs.push_back(dispatchMicros(session, kDecodeFrames, ids));
        ecuio_session_destroy(session);
    }
    ecuio_profile_unregister(name.c_str());
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

constexpr quint64 kFixtureValues = 7061;
std::string quantiles(const LatencyStats& l)
{
    if (l.count() == 0) return "-";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f", ms(l.quantileUs(0.5)),
                  ms(l.quantileUs(0.99)), ms(l.maxUs()));
    return buf;
}

// The load test: a markdown table, then each failed check; false if any.
// conditions ("" for none) is judged in every run, named as named
bool loadTest(bool withSlowRun, const std::string& conditions = std::string(),
              const char* named = "none")
{
    std::vector<LoadRun> runs(3);
    runs[0].label = "full speed, one pass";
    runs[0].query = "speed=max";
    runs[1].label = "100x, one pass";
    runs[1].query = "speed=100";
    runs[2].label = "full speed, looping 3 s";
    runs[2].query = "speed=max&loop=1";
    runs[2].sustained = true;
    LoadRun raw = runs[2];
    raw.label = "full speed, looping 3 s, raw (no decode)";
    raw.profile = "raw";
    runs.push_back(raw);
    LoadRun recordedLoop = runs[2];
    recordedLoop.label = "full speed, looping 3 s, recorded";
    recordedLoop.recorded = true;
    runs.push_back(recordedLoop); // runs[4]
    LoadRun recordedPaced = runs[1];
    recordedPaced.label = "100x, one pass, recorded";
    recordedPaced.recorded = true;
    runs.push_back(recordedPaced); // runs[5]
    LoadRun loopAgain = runs[2];
    loopAgain.label = "full speed, looping 3 s, again";
    runs.push_back(loopAgain); // runs[6]
    LoadRun recordedAgain = recordedLoop;
    recordedAgain.label = "full speed, looping 3 s, recorded, again";
    runs.push_back(recordedAgain); // runs[7]
    if (withSlowRun) {
        LoadRun slow;
        slow.label = "10x, one pass";
        slow.query = "speed=10";
        runs.push_back(slow);
    }
    for (LoadRun& run : runs) {
        run.conditions = conditions;
        replayFixture(run);
    }

    std::printf("\n### Load test: the engine-start fixture under obd2\n\n"
                "7,074 frames over 297 s recorded, RPM peaking at 1,261.25. "
                "Latencies in ms (median / p99 / max), each at most 12.5 %% "
                "above the true value.\n\nConditions: %s.\n\n",
                named);
    std::printf("| Run | Seconds | Worker frames/s | Handled | Dropped | Values | "
                "Frame to GUI thread | Value to model | Peak rpm |\n"
                "|---|---|---|---|---|---|---|---|---|\n");
    for (const LoadRun& r : runs) {
        char rate[32] = "-";
        if (r.sustained)
            std::snprintf(rate, sizeof(rate), "%.0f", r.workerRate);
        char peak[32] = "-";
        if (!r.sustained)
            std::snprintf(peak, sizeof(peak), "%.2f", r.peakRpm);
        char values[64];
        std::snprintf(values, sizeof(values), "%llu",
                      static_cast<unsigned long long>(r.stats.values));
        if (r.stats.valuesMissed > 0)
            std::snprintf(values, sizeof(values), "%llu (%llu past the staging's limit)",
                          static_cast<unsigned long long>(r.stats.values),
                          static_cast<unsigned long long>(r.stats.valuesMissed));
        std::printf("| %s | %.3f | %s | %d | %llu | %s | %s | %s | %s |\n", r.label,
                    r.seconds, rate, r.handled,
                    static_cast<unsigned long long>(r.stats.dropped), values,
                    quantiles(r.stats.frameToGui).c_str(),
                    quantiles(r.stats.valueToModel).c_str(), peak);
    }

    std::vector<std::string> failed;
    auto check = [&failed](bool ok, const std::string& what) {
        if (!ok) failed.push_back(what);
    };
    for (const LoadRun& r : runs) {
        const std::string at = std::string(r.label) + ": ";
        if (r.sustained) {
            check(r.workerRate >= kMinWorkerFramesPerSecond,
                  at + "under " + std::to_string(static_cast<long long>(kMinWorkerFramesPerSecond)) +
                      " frames/s through the worker");
            continue;
        }
        check(r.stats.frames == kFixtureFrames, at + "not every frame reached the bridge");
        check(r.stats.dropped + static_cast<quint64>(r.handled) == kFixtureFrames &&
                  r.stats.queuedAtEnd == 0,
              at + "a frame neither handled nor dropped");
        check(r.stats.valuesMissed == 0, at + "values past the staging's limit");
        check(r.stats.valueToModel.count() == r.stats.values, at + "a value not timed");
        check(std::abs(r.peakRpm - kFixturePeakRpm) < 0.01,
              at + "the chart lost the recording's peak");
        if (r.recorded) {
            check(r.rec.records_dropped == 0,
                  at + "the recording dropped records");
            check(r.rec.frame_rows == kFixtureFrames &&
                      r.rec.value_rows == kFixtureValues,
                  at + "the recording missed frames or values");
        }
    }
    const double decode9 = decodeMicros(9);
    const double decode1000 = decodeMicros(1000);
    const double decodeRatio = decode1000 / decode9;
    const double decodingShare = runs[2].workerRate / runs[3].workerRate;
    std::printf(
        "\nDecode, 1000 defs against 9: %.3f against %.3f us a frame (%.2fx). "
        "Looping, obd2 against raw: %.0f against %.0f frames/s (%.2f).\n",
        decode1000, decode9, decodeRatio, runs[2].workerRate,
        runs[3].workerRate, decodingShare);
    char bound[160];
    std::snprintf(bound, sizeof(bound),
                  "decode at 1000 defs %.2fx its cost at 9, over %.1fx",
                  decodeRatio, kMaxDecodeRatio);
    check(decodeRatio <= kMaxDecodeRatio, bound);
    std::snprintf(bound, sizeof(bound),
                  "looping under obd2 at %.2f of raw's rate, under %.2f",
                  decodingShare, kMinDecodingShare);
    check(decodingShare >= kMinDecodingShare, bound);
    const double unrecordedRate =
        std::max(runs[2].workerRate, runs[6].workerRate);
    const double recordedRate =
        std::max(runs[4].workerRate, runs[7].workerRate);
    const double recordedShare = recordedRate / unrecordedRate;
    std::printf(
        "Looping, recorded against not, the faster of two each: %.0f against "
        "%.0f frames/s (%.2f), %llu frames written, %llu records dropped. "
        "100x recorded: %llu frames and %llu values written, %llu records "
        "dropped.\n",
        recordedRate, unrecordedRate, recordedShare,
        static_cast<unsigned long long>(runs[4].rec.frame_rows +
                                        runs[7].rec.frame_rows),
        static_cast<unsigned long long>(runs[4].rec.records_dropped +
                                        runs[7].rec.records_dropped),
        static_cast<unsigned long long>(runs[5].rec.frame_rows),
        static_cast<unsigned long long>(runs[5].rec.value_rows),
        static_cast<unsigned long long>(runs[5].rec.records_dropped));
    std::snprintf(bound, sizeof(bound),
                  "looping recorded at %.2f of the unrecorded rate, under %.2f",
                  recordedShare, kMinRecordedLoopShare);
    check(recordedShare >= kMinRecordedLoopShare, bound);

    const LoadRun& paced = runs[1];
    check(ms(paced.stats.valueToModel.quantileUs(0.99)) <= kMaxPacedValueP99Ms,
          "100x: values' p99 to the model over " + std::to_string(static_cast<long long>(kMaxPacedValueP99Ms)) + " ms");
    check(ms(paced.stats.frameToGui.quantileUs(0.99)) <= kMaxPacedFrameP99Ms,
          "100x: frames' p99 to the GUI thread over " + std::to_string(static_cast<long long>(kMaxPacedFrameP99Ms)) + " ms");

    if (failed.empty()) {
        std::printf(
            "\nEvery check passed: each one-pass run accounted for every frame "
            "and kept the peak; at least %.0f frames/s through the worker; "
            "at 100x, p99 under %.0f ms to the model and %.0f ms to the GUI "
            "thread; decode at 1000 defs within %.1fx its cost at 9; looping "
            "under obd2 at least %.2f of raw's rate, and recorded at least "
            "%.2f of its unrecorded rate; the recorded 100x pass wrote every "
            "frame and value.\n",
            kMinWorkerFramesPerSecond, kMaxPacedValueP99Ms, kMaxPacedFrameP99Ms,
            kMaxDecodeRatio, kMinDecodingShare, kMinRecordedLoopShare);
        return true;
    }
    std::printf("\n");
    for (const std::string& f : failed)
        std::printf("**FAILED:** %s\n\n", f.c_str());
    return false;
}

} // namespace
