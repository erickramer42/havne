// File/layer: tests/bench_bridge.cpp | Qt6 Core, benchmark, C++17
// Testing: none of its own; a measurement, not a test. Not registered
//          with ctest. docs/testing.md ("Cost per frame") records
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
// nothing. The looping run and the 100x pass run again recorded through
// core's recorder (ADR-0049): the looping one at least 0.8 of its
// unrecorded rate, the 100x one writing every frame and value with no
// record dropped. Exit status 1 on any failed check.
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
// Run: build/tests/Release/bench_bridge.exe [frames per run, default 20000]
//      build/tests/Release/bench_bridge.exe --load   (the load test alone,
//      as CI runs it; a markdown table)
//      build/tests/Release/bench_bridge.exe --record (the recording test
//      alone, about 90 s; a markdown table)

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
#include "ecuio/internal.h"   /* ecuio_session_dispatch_callbacks */
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

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
// under obd2 against raw (dev: 0.69 to 0.76), 2026-11-01; a run of the
// decode ratio is kDecodeFrames frames
constexpr double kMaxDecodeRatio = 3.0;
constexpr double kMinDecodingShare = 0.5;
// The looping run recorded through core's recorder against the same run
// not recorded (ADR-0049 section 9's CI gate; the 10% is judged on the
// dev machine by --record's alternated runs)
constexpr double kMinRecordedLoopShare = 0.8;
constexpr int kDecodeFrames = 100000;

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
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=%2&%3")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES),
                 QString::fromLatin1(run.profile),
                 QString::fromLatin1(run.query));
    SessionBridge bridge;
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
constexpr double kMaxRecordedNs = 75.0; // dev: 42 to 47, Windows and Linux,
                                        // 2026-11-02
constexpr quint64 kFixtureValues = 7061;

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
// 40%, on another by about 4%, 2026-11-02). Its processors in *which;
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

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
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
    const LoadRun& recordedRun = runs[4];
    const double recordedShare = recordedRun.workerRate / runs[2].workerRate;
    std::printf(
        "Looping, recorded against not: %.0f against %.0f frames/s (%.2f), "
        "%llu frames written, %llu records dropped. 100x recorded: %llu "
        "frames and %llu values written, %llu records dropped.\n",
        recordedRun.workerRate, runs[2].workerRate, recordedShare,
        static_cast<unsigned long long>(recordedRun.rec.frame_rows),
        static_cast<unsigned long long>(recordedRun.rec.records_dropped),
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--load") == 0) {
        return loadTest(false) ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--record") == 0) {
        return recordTest() ? 0 : 1;
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
