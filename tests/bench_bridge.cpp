// File/layer: tests/bench_bridge.cpp | Qt6 Core, benchmark, C++17
// Testing: none of its own; a measurement, not a test. Not registered
//          with ctest. docs/testing.md ("Decode cost per frame") records
//          its results and how to run it.
//
// The GUI thread's cost per frame through the bridge, by the number of
// defs in the session's profile: what the backlog's decoding-on-the-
// worker item (STATUS.md, Performance) would remove, measured before it
// is built. Frames are queued as the worker queues them
// (dispatchFrameForTesting), then the drain is timed: the queued call,
// the frame log, the match against every def, the decode and staging.
// Recording is off. A raw session (no defs) is the baseline. Beside
// it: the worker's side of each frame (onFrame, the frame's copy and
// the queued call, run here on the bench's thread) and one coalesce
// tick's SignalListModel::updateValues for a model of N rows.
//
// Then the committed engine-start recording replayed under obd2 at
// speed=max and 10 (the load test ADR-0023 and ADR-0024 name): the
// time taken, the frames handled and dropped at the cap, and the
// highest RPM the model held at any tick, against the recording's
// 1,261 (Known Issue #10).
//
// The defs sit four to a CAN ID, 16 bits each, as a passive-bus profile
// packs them; each frame carries one ID, so it decodes four values and
// is checked against every def. Plain defs derive no requests, so the
// registry's request cap doesn't bound N.
//
// Run: build/tests/Release/bench_bridge.exe [frames per run, default 20000]

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "qttunebridge.h"
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
// obd2: wall time, frames handled and dropped at the cap, and the
// highest RPM the model held at any tick (what the Dashboard can show;
// the recording's own peak is 1,261)
struct ReplayRun {
    double seconds = 0.0;
    int handled = 0;
    int dropped = 0;
    double peakRpm = 0.0;
};

ReplayRun replayFixture(const char* speed)
{
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=%2")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES),
                 QString::fromLatin1(speed));
    QtTuneBridge bridge;
    const auto t0 = std::chrono::steady_clock::now();
    const QString err = bridge.connectSession(uri);
    if (!err.isEmpty()) {
        std::fprintf(stderr, "connect %s failed: %s\n", qPrintable(uri),
                     qPrintable(err));
        std::exit(1);
    }
    // Every row after the header is a frame
    int total = -1;
    QFile csv(QStringLiteral(ECUIO_REPLAY_FIXTURES) +
              QStringLiteral("/2026-10-25-engine-start/frames.csv"));
    if (csv.open(QIODevice::ReadOnly)) {
        while (!csv.atEnd()) {
            csv.readLine();
            ++total;
        }
    }
    if (total <= 0) {
        std::fprintf(stderr, "can't read the fixture\n");
        std::exit(1);
    }

    SignalListModel* model = bridge.signalModel();
    int rpmRow = -1;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r), SignalListModel::NameRole)
                .toString() == QStringLiteral("RPM"))
            rpmRow = r;
    }
    ReplayRun run;
    QObject::connect(model, &SignalListModel::valuesUpdated, model, [&]() {
        if (rpmRow < 0)
            return;
        const QModelIndex at = model->index(rpmRow);
        if (model->data(at, SignalListModel::HasValueRole).toBool())
            run.peakRpm = std::max(
                run.peakRpm,
                model->data(at, SignalListModel::ValueRole).toDouble());
    });
    while (bridge.frameCount() + bridge.droppedFrames() < total) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    run.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    // Two more ticks, so the last window's values reach the model
    const auto settle = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - settle <
           std::chrono::milliseconds(250))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    run.handled = bridge.frameCount();
    run.dropped = bridge.droppedFrames();
    bridge.disconnectSession();
    while (bridge.disconnecting())
        QCoreApplication::processEvents();
    return run;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int frames = argc > 1 ? std::max(1000, std::atoi(argv[1])) : 20000;

    std::printf("%d frames a run, median of %d runs; us a frame on the GUI "
                "thread (drain) and the worker's side (onFrame); us a tick "
                "for the model's update\n",
                frames, kRuns);
    std::printf("%6s  %10s  %12s  %14s  %10s  %10s\n", "defs", "us/frame",
                "decode us", "max frames/s", "worker us", "tick us");

    const PerFrame rawRun = measure("mock://bench?profile=raw", frames, 0);
    const double raw = rawRun.drain;
    std::printf("%6s  %10.2f  %12s  %14.0f  %10.2f  %10s\n", "raw", raw, "-",
                1e6 / raw, rawRun.worker, "-");

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
        const PerFrame run = measure(
            QString("mock://bench?profile=%1").arg(name.c_str()), frames, ids);
        const double us = run.drain;
        std::printf("%6d  %10.2f  %12.2f  %14.0f  %10.2f  %10.1f\n", n, us,
                    us - raw, 1e6 / us, run.worker, tickMicros(set));
        ecuio_profile_unregister(name.c_str());
    }

    std::printf("\nThe engine-start fixture under obd2 (7,074 frames, 297 s "
                "recorded; peak 1,261 rpm)\n");
    std::printf("%6s  %10s  %8s  %8s  %10s\n", "speed", "seconds", "handled",
                "dropped", "peak rpm");
    for (const char* speed : {"max", "10"}) {
        const ReplayRun r = replayFixture(speed);
        std::printf("%6s  %10.2f  %8d  %8d  %10.0f\n", speed, r.seconds,
                    r.handled, r.dropped, r.peakRpm);
    }
    return 0;
}
