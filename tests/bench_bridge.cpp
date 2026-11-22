// File/layer: tests/bench_bridge.cpp | Qt6 Core, benchmark, C++17
// Testing: none of its own; a measurement, not a test. Not registered
//          with ctest. docs/performance.md ("Cost per frame") records
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
// CI runner, so a failure is a regression, not noise. Two ratios hold
// whatever the runner's speed, since each compares the bench with
// itself: core's decode at 1000 defs against 9 (the def index keeps it
// flat; checking every def made it over 20x), and the looping run's
// worker rate under obd2 against the same run under raw, which decodes
// nothing, the best of three passes each. The looping run and the 100x pass run again recorded through
// core's recorder (ADR-0049): the looping pair three times, unrecorded and
// recorded alternated, the best recorded run at least the platform's
// floor (bench_load_judge.h) of the best unrecorded one, over three more
// pairs as well when the first three miss; the 100x one
// writing every frame and value with no record dropped. Exit status 1 on
// any failed check.
//
// --record measures the recorder (ADR-0049 section 9): the bridge's
// looping run, recorded and unrecorded alternated on three physical
// cores, held to the ADR's 10%; then core's own session on two, its
// cost a frame bounded; then recorded passes checked whole
// (recordTest).
//
// The defs sit four to a CAN ID, 16 bits each, as a passive-bus profile
// packs them; each frame carries one ID, so it decodes four values and
// is checked against every def. Plain defs derive no requests, so the
// registry's request cap doesn't bound N.
//
// With --conditions typical|largest the load test's runs judge that set,
// saved and attached through the bridge as the app's own (ADR-0063
// section 3), so the worker's callback and the tick's are measured too:
// scripts/bench_ab.py's load kind runs one tree with and without it.
// Every run's settings folder is a scratch one, so a saved set of the
// user's is never attached. It holds the plugin store too, so an A/B
// against a tree built before it reads another store.
//
// Run: build/tests/Release/bench_bridge.exe [frames per run, default 20000]
//      build/tests/Release/bench_bridge.exe --load [--conditions
//      typical|largest]   (the load test alone, as CI runs it; a
//      markdown table)
//      build/tests/Release/bench_bridge.exe --record (the recording test
//      alone, about 90 s; a markdown table)
//      build/tests/Release/bench_bridge.exe --decode (core's decode with
//      computed signals and a chain, about 2 s; a markdown table)

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "sessionbridge.h"
#include "ecuio/conditions.h"
#include "ecuio/internal.h"   /* ecuio_session_dispatch_callbacks */
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include "bench_bridge_common.h"
#include "bench_bridge_frame.h"
#include "bench_bridge_conditions.h"
#include "bench_bridge_decode.h"
#include "bench_bridge_load.h"
#include "bench_bridge_record.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--load") == 0) {
        if (argc == 2) {
            return loadTest(false) ? 0 : 1;
        }
        if (argc == 4 && std::strcmp(argv[2], "--conditions") == 0 &&
            (std::strcmp(argv[3], "typical") == 0 ||
             std::strcmp(argv[3], "largest") == 0)) {
            const bool largest = std::strcmp(argv[3], "largest") == 0;
            return loadTest(false,
                            largest ? largestConditions() : typicalConditions(),
                            largest
                                ? "the largest set, 64 conditions of 4 terms, "
                                  "attached through the bridge"
                                : "the typical set, 8 conditions, attached "
                                  "through the bridge")
                       ? 0
                       : 1;
        }
        std::fprintf(stderr,
                     "bench_bridge: --load [--conditions typical|largest]\n");
        return 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--record") == 0) {
        return recordTest() ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--decode") == 0) {
        return decodeTest() ? 0 : 1;
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
