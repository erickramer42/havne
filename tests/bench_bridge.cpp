// File/layer: tests/bench_bridge.cpp | Qt6 Core, benchmark, C++17
// Testing: none of its own; a measurement, not a test. Not registered
//          with ctest. docs/testing.md ("Decode cost per frame") records
//          its results and how to run it.
//
// Each thread's cost per frame through the bridge, by the number of
// defs in the session's profile (ADR-0024: core decodes on the worker).
// Frames are queued as the worker queues them (dispatchFrameForTesting),
// then the GUI thread's drain is timed: the queued call, the frame log
// and the counters. Recording is off. Beside it, the worker's side,
// run here on the bench's thread: onFrame (the frame's copy and the
// queued call); core's match and decode into a batch
// (ecuio_session_dispatch_callbacks on a session of its own, to a
// values callback that does nothing); and the bridge's staging of that
// batch (onValues). Then one coalesce tick's SignalListModel::
// updateValues for a model of N rows.
//
// Then the load test ADR-0023 and ADR-0024 name: the committed
// engine-start recording replayed through the bridge under obd2, once at
// full speed, once at 100x (paced, about 3 s), and looping at full speed
// for 3 s (sustained), each reported from the bridge's delivery figures
// (the activity log's "Delivery:" line): frames through the worker (the
// replay's read, core's decode, the bridge's callbacks), frames handled
// and dropped at the in-flight cap, values, and how long frames took to
// reach the GUI thread and values the model. A one-pass run must account
// for every frame and keep the recording's 1,261.25 rpm peak on the chart
// (ADR-0005's appendix); the floors below are loose enough for a shared
// CI runner, so a failure is a regression, not noise. Exit status 1 on
// any failed check.
//
// The defs sit four to a CAN ID, 16 bits each, as a passive-bus profile
// packs them; each frame carries one ID, so it decodes four values and
// is checked against every def. Plain defs derive no requests, so the
// registry's request cap doesn't bound N.
//
// Run: build/tests/Release/bench_bridge.exe [frames per run, default 20000]
//      build/tests/Release/bench_bridge.exe --load   (the load test alone,
//      as CI runs it; a markdown table)

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "qttunebridge.h"
#include "ecuio/internal.h"   /* ecuio_session_dispatch_callbacks */
#include "ecuio/profile.h"
#include "ecuio/signals.h"

namespace {

constexpr int kDefsPerId = 4;
constexpr uint32_t kFirstId = 0x100;
constexpr int kRuns = 3;

std::vector<ecuio_signal_def_t> makeDefs(int n)
{
    std::vector<ecuio_signal_def_t> defs(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        ecuio_signal_def_t& d = defs[static_cast<size_t>(k)];
        d = {};
        d.id = static_cast<uint16_t>(k + 1);
        d.message_id = kFirstId + static_cast<uint32_t>(k / kDefsPerId);
        d.kind = ECUIO_VALUE_UINT;
        d.byte_order = ECUIO_BYTE_ORDER_LE;
        d.start_byte = static_cast<uint16_t>(2 * (k % kDefsPerId));
        d.bit_length = 16;
        d.scale = 1.0;
        std::snprintf(d.name, sizeof(d.name), "S%d", k + 1);
    }
    return defs;
}

// Microseconds per frame for `frames` frames cycling over `ids` CAN IDs:
// the GUI thread's drain, and the worker's side (onFrame: the frame's
// copy and the queued call), here run on this thread
struct PerFrame {
    double drain = 0.0;
    double worker = 0.0;
};

PerFrame microsPerFrame(QtTuneBridge& bridge, int frames, int ids)
{
    ecuio_frame_t f{};
    f.data_length = 8;
    const int burst = QtTuneBridge::maxInFlightForTesting() / 2;
    std::chrono::steady_clock::duration drained{};
    std::chrono::steady_clock::duration posted{};
    int sent = 0;
    while (sent < frames) {
        const int n = std::min(burst, frames - sent);
        const int expected = bridge.frameCount() + n;
        const auto p0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i, ++sent) {
            f.arbitration_id =
                kFirstId + static_cast<uint32_t>(ids > 0 ? sent % ids : 0);
            f.timestamp_us = static_cast<uint64_t>(sent);
            for (int b = 0; b < 8; ++b)
                f.data[b] = static_cast<uint8_t>(sent + b);
            bridge.dispatchFrameForTesting(f);
        }
        posted += std::chrono::steady_clock::now() - p0;
        const auto t0 = std::chrono::steady_clock::now();
        while (bridge.frameCount() < expected) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
        }
        drained += std::chrono::steady_clock::now() - t0;
    }
    PerFrame out;
    out.drain =
        std::chrono::duration<double, std::micro>(drained).count() / frames;
    out.worker =
        std::chrono::duration<double, std::micro>(posted).count() / frames;
    return out;
}

// The median of kRuns runs, each on a fresh connect
PerFrame measure(const QString& uri, int frames, int ids)
{
    std::vector<PerFrame> runs;
    for (int r = 0; r < kRuns; ++r) {
        QtTuneBridge bridge;
        const QString err = bridge.connectSession(uri);
        if (!err.isEmpty()) {
            std::fprintf(stderr, "connect %s failed: %s\n", qPrintable(uri),
                         qPrintable(err));
            std::exit(1);
        }
        runs.push_back(microsPerFrame(bridge, frames, ids));
        if (bridge.droppedFrames() != 0) {
            std::fprintf(stderr, "%s dropped %d frames\n", qPrintable(uri),
                         bridge.droppedFrames());
            std::exit(1);
        }
        bridge.disconnectSession();
        while (bridge.disconnecting())
            QCoreApplication::processEvents();
    }
    std::sort(
        runs.begin(), runs.end(),
        [](const PerFrame& a, const PerFrame& b) { return a.drain < b.drain; });
    PerFrame median = runs[runs.size() / 2];
    std::vector<double> workers;
    for (const PerFrame& p : runs)
        workers.push_back(p.worker);
    std::sort(workers.begin(), workers.end());
    median.worker = workers[workers.size() / 2];
    return median;
}

// Microseconds per frame on the worker for the values path: core's match
// and decode into a batch, and the bridge's staging of it (onValues),
// told apart by dispatching once to a values callback that does nothing
// and once to one that hands the batch to a connected bridge
struct ValuesPath {
    double decode = 0.0;
    double stage = 0.0;
};

void ignoreValues(const ecuio_value_batch_t*, void*) {}

void forwardValues(const ecuio_value_batch_t* batch, void* bridge)
{
    static_cast<QtTuneBridge*>(bridge)->dispatchValuesForTesting(*batch);
}

double dispatchMicros(ecuio_session_t* session, int frames, int ids)
{
    ecuio_frame_t f{};
    f.data_length = 8;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        f.arbitration_id = kFirstId + static_cast<uint32_t>(ids > 0 ? i % ids : 0);
        f.timestamp_us = static_cast<uint64_t>(i);
        for (int b = 0; b < 8; ++b)
            f.data[b] = static_cast<uint8_t>(i + b);
        ecuio_session_dispatch_callbacks(session, &f);
    }
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - t0)
               .count() /
           frames;
}

ValuesPath valuesMicros(const QString& uri, int frames, int ids)
{
    std::vector<double> decode, both;
    for (int r = 0; r < kRuns; ++r) {
        const QByteArray uri8 = uri.toUtf8();
        ecuio_session_t* session = nullptr;
        if (ecuio_session_create(uri8.constData(), &session) != ECUIO_OK) {
            std::fprintf(stderr, "create %s failed\n", qPrintable(uri));
            std::exit(1);
        }
        ecuio_register_values_callback(session, ignoreValues, nullptr);
        decode.push_back(dispatchMicros(session, frames, ids));
        ecuio_unregister_values_callback(session, ignoreValues, nullptr);

        // The bridge's own session runs the same profile, so it knows the
        // ids; its mock frames match none of them
        QtTuneBridge bridge;
        bridge.setValueSampleLimitForTesting(frames * kDefsPerId);
        if (!bridge.connectSession(uri).isEmpty()) {
            std::fprintf(stderr, "connect %s failed\n", qPrintable(uri));
            std::exit(1);
        }
        ecuio_register_values_callback(session, forwardValues, &bridge);
        both.push_back(dispatchMicros(session, frames, ids));
        ecuio_session_destroy(session);
        bridge.disconnectSession();
        while (bridge.disconnecting())
            QCoreApplication::processEvents();
    }
    std::sort(decode.begin(), decode.end());
    std::sort(both.begin(), both.end());
    ValuesPath out;
    out.decode = decode[decode.size() / 2];
    out.stage = both[both.size() / 2] - out.decode;
    return out;
}

// Microseconds for one coalesce tick's update of a model of n rows with
// a value for every row: SignalListModel::updateValues, as the tick
// calls it, with no view attached
double tickMicros(const ecuio_signal_set_t& set)
{
    SignalListModel model;
    model.setSignalSet(&set);
    std::vector<std::pair<uint16_t, double>> values;
    for (uint32_t k = 0; k < set.count; ++k)
        values.emplace_back(set.defs[k].id, static_cast<double>(k));
    constexpr int kTicks = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < kTicks; ++t) {
        for (auto& v : values)
            v.second += 1.0;
        model.updateValues(values, t * 100);
    }
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - t0)
               .count() /
           kTicks;
}

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

struct LoadRun {
    const char* label = "";
    const char* query = "";
    bool sustained = false;
    double seconds = 0.0;        // one pass: until every frame is handled or dropped
    double workerRate = 0.0;     // sustained: frames a second through the worker
    double handledRate = 0.0;    // sustained: frames a second the GUI thread handled
    int handled = 0;
    double peakRpm = 0.0;
    QtTuneBridge::DeliveryStats stats;
};

void spin(double seconds)
{
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(seconds));
    while (std::chrono::steady_clock::now() < until)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void replayFixture(LoadRun& run)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&%2")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES), QString::fromLatin1(run.query));
    QtTuneBridge bridge;
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
        std::exit(1);
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
    } else {
        while (static_cast<quint64>(bridge.frameCount() + bridge.droppedFrames()) <
               kFixtureFrames)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        run.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        spin(0.25);   // two more ticks: the last window's values reach the model
        run.stats = bridge.deliveryStats();
    }
    run.handled = bridge.frameCount();
    bridge.disconnectSession();
    while (bridge.disconnecting())
        QCoreApplication::processEvents();
}

double ms(uint64_t us) { return static_cast<double>(us) / 1000.0; }

std::string quantiles(const LatencyStats& l)
{
    if (l.count() == 0) return "-";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f", ms(l.quantileUs(0.5)),
                  ms(l.quantileUs(0.99)), ms(l.maxUs()));
    return buf;
}

// The load test: a markdown table, then each failed check; false if any
bool loadTest(bool withSlowRun)
{
    std::vector<LoadRun> runs(3);
    runs[0].label = "full speed, one pass";
    runs[0].query = "speed=max";
    runs[1].label = "100x, one pass";
    runs[1].query = "speed=100";
    runs[2].label = "full speed, looping 3 s";
    runs[2].query = "speed=max&loop=1";
    runs[2].sustained = true;
    if (withSlowRun) {
        LoadRun slow;
        slow.label = "10x, one pass";
        slow.query = "speed=10";
        runs.push_back(slow);
    }
    for (LoadRun& run : runs)
        replayFixture(run);

    std::printf("\n### Load test: the engine-start fixture under obd2\n\n"
                "7,074 frames over 297 s recorded, RPM peaking at 1,261.25. "
                "Latencies in ms (median / p99 / max), each at most 12.5 %% "
                "above the true value.\n\n");
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
    }
    const LoadRun& paced = runs[1];
    check(ms(paced.stats.valueToModel.quantileUs(0.99)) <= kMaxPacedValueP99Ms,
          "100x: values' p99 to the model over " + std::to_string(static_cast<long long>(kMaxPacedValueP99Ms)) + " ms");
    check(ms(paced.stats.frameToGui.quantileUs(0.99)) <= kMaxPacedFrameP99Ms,
          "100x: frames' p99 to the GUI thread over " + std::to_string(static_cast<long long>(kMaxPacedFrameP99Ms)) + " ms");

    if (failed.empty()) {
        std::printf("\nEvery check passed: each one-pass run accounted for every frame "
                    "and kept the peak; at least %.0f frames/s through the worker; "
                    "at 100x, p99 under %.0f ms to the model and %.0f ms to the GUI "
                    "thread.\n",
                    kMinWorkerFramesPerSecond, kMaxPacedValueP99Ms, kMaxPacedFrameP99Ms);
        return true;
    }
    std::printf("\n");
    for (const std::string& f : failed)
        std::printf("**FAILED:** %s\n\n", f.c_str());
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--load") == 0) {
        return loadTest(false) ? 0 : 1;
    }
    const int frames = argc > 1 ? std::max(1000, std::atoi(argv[1])) : 20000;

    std::printf("%d frames a run, median of %d runs. us a frame: the GUI "
                "thread's drain; on the worker, onFrame, core's decode and "
                "the bridge's staging; us a tick for the model's update\n",
                frames, kRuns);
    std::printf("%6s  %10s  %10s  %10s  %10s  %10s\n", "defs", "GUI us",
                "onFrame us", "decode us", "stage us", "tick us");

    const PerFrame rawRun = measure("mock://bench?profile=raw", frames, 0);
    std::printf("%6s  %10.2f  %10.2f  %10s  %10s  %10s\n", "raw", rawRun.drain,
                rawRun.worker, "-", "-", "-");

    for (int n : {9, 50, 100, 250, 500, 1000}) {
        std::vector<ecuio_signal_def_t> defs = makeDefs(n);
        ecuio_signal_set_t set{};
        set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        set.count = static_cast<uint32_t>(n);
        set.defs = defs.data();
        const std::string name = "bench" + std::to_string(n);
        ecuio_validation_t why{};
        if (ecuio_profile_register(name.c_str(), &set, &why) != ECUIO_OK) {
            std::fprintf(stderr, "%s refused: code %d at def %u\n",
                         name.c_str(), why.code, why.index);
            return 1;
        }
        const int ids = (n + kDefsPerId - 1) / kDefsPerId;
        const QString uri = QString("mock://bench?profile=%1").arg(name.c_str());
        const PerFrame run = measure(uri, frames, ids);
        const ValuesPath values = valuesMicros(uri, frames, ids);
        std::printf("%6d  %10.2f  %10.2f  %10.2f  %10.2f  %10.1f\n", n,
                    run.drain, run.worker, values.decode, values.stage,
                    tickMicros(set));
        ecuio_profile_unregister(name.c_str());
    }

    return loadTest(true) ? 0 : 1;
}
