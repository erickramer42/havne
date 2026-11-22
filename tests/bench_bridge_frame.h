// File/layer: tests/bench_bridge_frame.h | Qt6 Core, benchmark, C++17
// Testing: none of its own; part of bench_bridge (tests/bench_bridge.cpp)
// The default run: each thread's cost per frame by def count.

#pragma once

#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>
#include "sessionbridge.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "bench_bridge_common.h"

namespace {

// Microseconds per frame for `frames` frames cycling over `ids` CAN IDs:
// the GUI thread's drain, and the worker's side (onFrame: the frame's
// copy and the queued call), here run on this thread
struct PerFrame {
    double drain = 0.0;
    double worker = 0.0;
};

PerFrame microsPerFrame(SessionBridge& bridge, int frames, int ids)
{
    ecuio_frame_t f{};
    f.data_length = 8;
    const int burst = SessionBridge::maxInFlightForTesting() / 2;
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
        SessionBridge bridge;
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
    static_cast<SessionBridge*>(bridge)->dispatchValuesForTesting(*batch);
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
        SessionBridge bridge;
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

} // namespace
