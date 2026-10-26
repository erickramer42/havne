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
// Recording is off. A raw session (no defs) is the baseline.
//
// The defs sit four to a CAN ID, 16 bits each, as a passive-bus profile
// packs them; each frame carries one ID, so it decodes four values and
// is checked against every def. Plain defs derive no requests, so the
// registry's request cap doesn't bound N.
//
// Run: build/tests/Release/bench_bridge.exe [frames per run, default 20000]

#include <QCoreApplication>
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

// Microseconds per frame for the GUI thread's drain of `frames` frames
// cycling over `ids` CAN IDs
double drainMicrosPerFrame(QtTuneBridge& bridge, int frames, int ids)
{
    ecuio_frame_t f{};
    f.data_length = 8;
    const int burst = QtTuneBridge::maxInFlightForTesting() / 2;
    std::chrono::steady_clock::duration drained{};
    int sent = 0;
    while (sent < frames) {
        const int n = std::min(burst, frames - sent);
        const int expected = bridge.frameCount() + n;
        for (int i = 0; i < n; ++i, ++sent) {
            f.arbitration_id =
                kFirstId + static_cast<uint32_t>(ids > 0 ? sent % ids : 0);
            f.timestamp_us = static_cast<uint64_t>(sent);
            for (int b = 0; b < 8; ++b)
                f.data[b] = static_cast<uint8_t>(sent + b);
            bridge.dispatchFrameForTesting(f);
        }
        const auto t0 = std::chrono::steady_clock::now();
        while (bridge.frameCount() < expected) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
        }
        drained += std::chrono::steady_clock::now() - t0;
    }
    return std::chrono::duration<double, std::micro>(drained).count() / frames;
}

// The median of kRuns runs, each on a fresh connect
double measure(const QString& uri, int frames, int ids)
{
    std::vector<double> runs;
    for (int r = 0; r < kRuns; ++r) {
        QtTuneBridge bridge;
        const QString err = bridge.connectSession(uri);
        if (!err.isEmpty()) {
            std::fprintf(stderr, "connect %s failed: %s\n", qPrintable(uri),
                         qPrintable(err));
            std::exit(1);
        }
        runs.push_back(drainMicrosPerFrame(bridge, frames, ids));
        if (bridge.droppedFrames() != 0) {
            std::fprintf(stderr, "%s dropped %d frames\n", qPrintable(uri),
                         bridge.droppedFrames());
            std::exit(1);
        }
        bridge.disconnectSession();
        while (bridge.disconnecting())
            QCoreApplication::processEvents();
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int frames = argc > 1 ? std::max(1000, std::atoi(argv[1])) : 20000;

    std::printf("%d frames a run, median of %d runs; GUI-thread drain only\n",
                frames, kRuns);
    std::printf("%6s  %10s  %12s  %14s\n", "defs", "us/frame", "decode us",
                "max frames/s");

    const double raw = measure("mock://bench?profile=raw", frames, 0);
    std::printf("%6s  %10.2f  %12s  %14.0f\n", "raw", raw, "-", 1e6 / raw);

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
        const double us = measure(
            QString("mock://bench?profile=%1").arg(name.c_str()), frames, ids);
        std::printf("%6d  %10.2f  %12.2f  %14.0f\n", n, us, us - raw, 1e6 / us);
        ecuio_profile_unregister(name.c_str());
    }
    return 0;
}
