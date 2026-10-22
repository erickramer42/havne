#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "qttunebridge.h"   // QtTuneBridge + FrameListModel

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

/* Spin the GUI event loop for `ms`, letting queued invocations drain.
 * Without this, worker-posted lambdas never execute. */
void spinLoop(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* Poll-until-predicate with deadline, event-loop flavored. The mock's
 * 50ms cadence is a LOCAL expectation, not a CI-runner guarantee —
 * a starved macOS VM can deliver 2 frames in 200+ ms of wall clock.
 * Polling keeps the asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). The event loop
 * must keep turning while polling — worker lambdas drain through it. */
template <typename Predicate>
bool spinUntil(Predicate pred, int timeout_ms, int step_ms = 50)
{
    QElapsedTimer clock;
    clock.start();
    while (!pred()) {
        if (clock.elapsed() >= timeout_ms) {
            return false;
        }
        spinLoop(step_ms);
    }
    return true;
}

/* Disconnect is async: connected drops at once and the teardown ends on
 * a worker thread. A test that connects again waits for it, as the UI
 * does (Connect stays disabled while disconnecting). */
bool disconnectAndWait(QtTuneBridge& bridge, int timeout_ms = 5000)
{
    bridge.disconnectSession();
    return spinUntil([&bridge]() { return !bridge.disconnecting(); }, timeout_ms, 10);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // ONE instance, lives for the process
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(BridgeTest, LifecycleIsIdempotent)
{
    QtTuneBridge bridge;

    EXPECT_FALSE(bridge.connected());

    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());

    // Double connect = idempotent success, no new session
    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());

    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());     // drops at once; the teardown
    EXPECT_TRUE(bridge.disconnecting());  // ends on its own thread

    bridge.disconnectSession();   // double + triple disconnect: no-op
    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());

    // No connect until the teardown ends
    EXPECT_FALSE(bridge.connectSession("mock://test").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 5000, 10));

    // Reconnect after disconnect must work (fresh session path)
    EXPECT_TRUE(bridge.connectSession("mock://test").isEmpty());
    EXPECT_TRUE(bridge.connected());
    bridge.disconnectSession();
}

TEST(BridgeTest, InvalidUriReturnsError)
{
    QtTuneBridge bridge;
    EXPECT_FALSE(bridge.connectSession("j2534://dev0").isEmpty());   // no transport
    EXPECT_FALSE(bridge.connected());

    EXPECT_FALSE(bridge.connectSession("").isEmpty());               // bad URI
    EXPECT_FALSE(bridge.connected());
}

TEST(BridgeTest, FramesMarshalFromWorkerToGuiThread)
{
    QtTuneBridge bridge;

    // Connect to SOURCE MODEL's countChanged — proxy forwards changes automatically
    std::atomic<Qt::HANDLE> append_thread{nullptr};
    QObject::connect(
        static_cast<FrameListModel*>(bridge.model()->sourceModel()),
        &FrameListModel::countChanged,
        [&append_thread](int) {
            Qt::HANDLE current = QThread::currentThreadId();
            Qt::HANDLE expected = nullptr;
            append_thread.compare_exchange_strong(expected, current);
        });

    ASSERT_TRUE(bridge.connectSession("mock://test").isEmpty());
    // CONTRACT: frames eventually flow. Poll instead of a fixed 300ms.
    const bool flowing = spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000);
    ASSERT_TRUE(flowing) << "no frames in 5s — stream is dead, not slow";

    // Frames flowed...
    EXPECT_GT(bridge.frameCount(), 0);
    // ...and they landed on the thread running this event loop.
    EXPECT_EQ(append_thread.load(), QThread::currentThreadId());
    EXPECT_NE(append_thread.load(), nullptr);

    const int frames_before = bridge.frameCount();

    // Mid-stream disconnect: late-arriving frames must be dropped,
    // not crash, not append. Equality assertion after a settle window
    // is duration-independent — any arriving frame would break it.
    bridge.disconnectSession();
    spinLoop(300);   // settle: let any in-flight lambdas drain and drop
    EXPECT_EQ(bridge.frameCount(), frames_before);
}

TEST(BridgeTest, ClearFramesResets)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://test").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    ASSERT_GT(bridge.frameCount(), 0);

    bridge.clearFrames();
    EXPECT_EQ(bridge.frameCount(), 0);
    EXPECT_EQ(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

/* The frame log belongs to a session: rows stay readable after a
 * disconnect, and the next connect starts an empty table. Checked as
 * the synchronous connect returns, before any queued frame can land. */
TEST(BridgeTest, ConnectStartsAnEmptyFrameLog)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    FrameSortProxy* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(proxy->rowCount(), 0) << "disconnect cleared the log";

    ASSERT_TRUE(bridge.connectSession("mock://second").isEmpty());
    EXPECT_EQ(proxy->rowCount(), 0);
    EXPECT_EQ(bridge.frameCount(), 0);

    bridge.disconnectSession();
}

/* Rows sort by arrival, not by frame timestamp: a J2534 timestamp is the
 * vendor's raw 32-bit stamp, which can wrap. A frame stamped earlier but
 * arriving later is still the newest row. */
TEST(BridgeTest, FrameLogOrdersByArrivalNotTimestamp)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://arrival").isEmpty());

    ecuio_frame_t first{};
    first.timestamp_us = 900000;
    first.arbitration_id = 0x123;   // not the mock's 0x0C0
    first.data_length = 1;
    ecuio_frame_t second = first;
    second.timestamp_us = 100;      // a wrapped stamp, say
    second.arbitration_id = 0x124;
    bridge.dispatchFrameForTesting(first);
    bridge.dispatchFrameForTesting(second);

    FrameSortProxy* proxy = bridge.model();
    auto row_of = [proxy](quint32 id) {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                            FrameListModel::ArbitrationIdRole).toUInt() == id) {
                return r;
            }
        }
        return -1;
    };
    ASSERT_TRUE(spinUntil([&row_of]() {
        return row_of(0x123) >= 0 && row_of(0x124) >= 0;
    }, 5000));

    EXPECT_LT(row_of(0x124), row_of(0x123)) << "newest first";
    proxy->setNewestFirst(false);
    EXPECT_LT(row_of(0x123), row_of(0x124)) << "oldest first";

    bridge.disconnectSession();
}

TEST(BridgeTest, SortProxyOrdersNewestFirstByDefault)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-default").isEmpty());

    FrameSortProxy* proxy = bridge.model();
    // CONTRACT: more than two frames arrive sorted. Polled, not a fixed
    // sleep: starved CI runners can deliver only 2 in 300 ms.
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000))
        << "only " << proxy->rowCount() << " rows in 5s";
    EXPECT_TRUE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_GT(first, last);

    bridge.disconnectSession();
}

TEST(BridgeTest, SortProxyTogglesToOldestFirst)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-toggle").isEmpty());

    FrameSortProxy* proxy = bridge.model();
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000));

    proxy->setNewestFirst(false);
    EXPECT_FALSE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_LT(first, last);

    // Toggle back — must re-sort to descending without reconnect
    proxy->setNewestFirst(true);

    // Capture BOTH ends fresh
    const quint64 firstAgain = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 lastAgain = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();

    // Top is the newest we've ever seen (>= because the stream kept ticking)
    EXPECT_GE(firstAgain, last);
    // Bottom is the oldest — at most the original ascending-order top,
    // and strictly older than the top of the list
    EXPECT_LE(lastAgain, first);      // 'first' = oldest captured earlier
    EXPECT_GT(firstAgain, lastAgain); // reversal actually happened

    bridge.disconnectSession();
}

TEST(BridgeTest, NotifyCountTracksFrameProcessing)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://notify").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.guiNotifyCount() > 0; }, 5000));
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount());
    EXPECT_GT(bridge.guiNotifyCount(), 0);
    bridge.disconnectSession();
}

TEST(BridgeTest, SignalBatchesAreCoalesced)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://coalesce").isEmpty());

    // CONTRACT: batches accumulate while the stream runs. No upper
    // bound on the count: a wall-clock bound is only a proxy for
    // "coalescing merges frames," which the ratio assertion below
    // states directly.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalBatchCount() > 4; }, 5000))
        << "only " << bridge.signalBatchCount() << " batches in 5s";

    EXPECT_GT(bridge.signalBatchCount(), 4);
    EXPECT_GT(bridge.frameCount(), 0);
    // Ratio contract: at ~20fps
    // frames vs 10Hz batches, coalescing means fewer batches than
    // frames. Per-frame emission (the regression this guards) yields
    // exact equality — one emission per frame.
    EXPECT_LE(bridge.signalBatchCount(), bridge.frameCount());

    // Values actually flowed through the model
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, StreamingContinuesPastTenThousandFrames)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://longrun").isEmpty());

    // CONTRACT: at least one frame flows first. Poll, no fixed 150ms.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    ASSERT_GT(bridge.frameCount(), 0);

    // Simulate the old freeze boundary: cumulative count just below 10k.
    // Under the OLD gate, the next frame would hit >= MaxFrames and
    // every subsequent frame would be dropped at source, forever.
    bridge.forceFrameCountForTesting(9999);

    // CONTRACT: the count advances past the boundary. Was a fixed
    // 600ms wait (needs ~2 frames) — poll until it's demonstrably past.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 10000; }, 5000))
        << "stream stalled at " << bridge.frameCount();
    EXPECT_GT(bridge.frameCount(), 10000);

    // And no drops occurred — nothing was rejected at the boundary
    EXPECT_EQ(bridge.droppedFrames(), 0);

    // Liveness, not coincidence: the log model kept receiving rows too
    EXPECT_GT(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, DispatchedShortFrameHonorsDataLength)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://short").isEmpty());

    // Unique timestamp so we can find OUR row among mock frames
    ecuio_frame_t f{};
    f.timestamp_us = 777777;   // steady-clock µs from the mock won't collide
    f.arbitration_id = 0x0C0;  // telemetry ID — decodes (gate-consistent)
    f.channel_id = 0;
    f.data_length = 4;          // the whole point: NOT 8
    f.flags = ECUIO_FRAME_FLAG_EXTENDED;    // was: f.is_extended = 1
    f.data[0] = 0x34;           // arbitrary RPM-looking bytes
    f.data[1] = 0x12;
    f.data[2] = 0xAA;
    f.data[3] = 0x55;

    bridge.dispatchFrameForTesting(f);

    // CONTRACT: our dispatched frame reaches the model. Poll on the
    // finding condition instead of a fixed 200ms settle — the row must
    // appear through the queued-invocation path regardless of runner speed.
    FrameSortProxy* proxy = bridge.model();
    auto row_finder = [proxy]() {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                    FrameListModel::TimestampRole).toULongLong() == 777777) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(spinUntil(row_finder, 5000))
        << "dispatched frame never reached the model";

    int foundRow = -1;
    for (int r = 0; r < proxy->rowCount(); ++r) {
        if (proxy->data(proxy->index(r, 0),
                FrameListModel::TimestampRole).toULongLong() == 777777) {
            foundRow = r;
            break;
        }
    }
    ASSERT_NE(foundRow, -1) << "row vanished between poll and assert";

    const QModelIndex idx = proxy->index(foundRow, 0);
    // DLC must reflect the ACTUAL length, not an assumed 8
    EXPECT_EQ(proxy->data(idx, FrameListModel::DlcRole).toUInt(), 4u);
    // Exactly the 4 bytes, not 8, as spaced uppercase hex
    EXPECT_EQ(proxy->data(idx, FrameListModel::PayloadRole).toString(),
              QStringLiteral("34 12 AA 55"));
    EXPECT_TRUE(proxy->data(idx, FrameListModel::ExtendedRole).toBool());
    EXPECT_EQ(proxy->data(idx, FrameListModel::ArbitrationIdRole).toUInt(), 0x0C0u);

    bridge.disconnectSession();
}

/* LogPage binds frame columns by role NAME. A name missing here isn't a
 * build error: the column renders empty and every delegate throws. Read
 * through the proxy, which is what QML gets as Bridge.model. */
TEST(BridgeTest, FrameRoleNamesMatchLogPageBindings)
{
    QtTuneBridge bridge;
    const QHash<int, QByteArray> roles = bridge.model()->roleNames();
    EXPECT_EQ(roles.value(FrameListModel::TimestampRole), QByteArray("timestampUs"));
    EXPECT_EQ(roles.value(FrameListModel::DlcRole), QByteArray("dlc"));
    EXPECT_EQ(roles.value(FrameListModel::ExtendedRole), QByteArray("extended"));
    EXPECT_EQ(roles.value(FrameListModel::PayloadRole), QByteArray("payloadHex"));
    EXPECT_EQ(roles.value(FrameListModel::ArbitrationIdRole), QByteArray("arbId"));
}

TEST(BridgeTest, LogPauseStopsFrameLogNotDecode)
{
    /* Pause holds the frame log still to read: nothing is appended while
     * paused, but frames are still counted and decoded, so Live and the
     * Dashboard carry on. Resume appends again, and pause belongs to a
     * session: disconnect clears it and a new session starts unpaused. */
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://pause").isEmpty());
    FrameSortProxy* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.logPaused());
    const int rows = proxy->rowCount();
    const int frames = bridge.frameCount();
    const int batches = bridge.signalBatchCount();

    // CONTRACT: frames and batches keep coming while the log holds. The
    // row count is exact: pause is set on the GUI thread, where every
    // append happens.
    ASSERT_TRUE(spinUntil([&bridge, frames, batches]() {
        return bridge.frameCount() > frames + 2 && bridge.signalBatchCount() > batches;
    }, 5000)) << "stream stopped while paused";
    EXPECT_EQ(proxy->rowCount(), rows);

    bridge.setLogPaused(false);
    ASSERT_TRUE(spinUntil([proxy, rows]() { return proxy->rowCount() > rows; }, 5000))
        << "log did not resume";

    bridge.setLogPaused(true);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.logPaused());

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.connectSession("mock://pause-again").isEmpty());
    EXPECT_FALSE(bridge.logPaused());
    bridge.disconnectSession();
}

TEST(BridgeTest, BackpressureDropsAreCountedAndReported)
{
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://drops").isEmpty());

    // Lambda-counter instead of QSignalSpy: avoids a new Qt6::Test dependency
    int notifyCount = 0;
    int lastReported = -1;
    QObject::connect(&bridge, &QtTuneBridge::droppedFramesChanged,
                     [&](int dropped) { ++notifyCount; lastReported = dropped; });

    ecuio_frame_t f{};
    f.data_length = 8;

    const int inject = QtTuneBridge::maxInFlightForTesting() + 500;

    // Synchronous flood from the test thread: no event processing happens,
    // so in-flight climbs monotonically to the cap and stays there.
    for (int i = 0; i < inject; ++i) {
        f.timestamp_us = 1000000 + static_cast<uint64_t>(i);
        bridge.dispatchFrameForTesting(f);
    }

    // Drops happened during saturation (loop 1001..1500 hit the gate).
    // GE not EQ: the mock worker may contribute a frame or two mid-loop.
    EXPECT_GE(bridge.droppedFrames(), 500);

    // Nothing could have been reported yet — the queue hasn't been processed
    EXPECT_EQ(notifyCount, 0);

    // CONTRACT: the backlog drains. Was a fixed 800ms — poll until the
    // overwhelming majority of the queued invocations processed.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 900; }, 10000))
        << "drain stalled at " << bridge.frameCount() << " frames";

    // Accounting survived the drain
    EXPECT_GE(bridge.droppedFrames(), 500);
    // The consolidated notification arrived with the final value
    EXPECT_GE(notifyCount, 1);
    EXPECT_EQ(lastReported, bridge.droppedFrames());
    // The queued frames were actually processed, not silently lost
    EXPECT_GT(bridge.frameCount(), 900);
    // Drained backlog frees the gate again — no permanent wedge
    // (the running mock resumes normal delivery after this)

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// Strip chart: per-signal rolling history ring in SignalListModel
// ---------------------------------------------------------------------------

namespace {

/* Minimal two-signal set, built test-side. Plain assignments only —
   designated initializers are C++20, and MSVC accepts them where GCC
   rejects them. */
void buildTestSignalSet(ecuio_signal_def_t (&defs)[2], ecuio_signal_set_t& set)
{
    ecuio_signal_def_t& rpm = defs[0];
    rpm = {};
    rpm.id = 1001;
    strncpy(rpm.name, "Engine_RPM", sizeof(rpm.name) - 1);
    rpm.kind = ECUIO_VALUE_UINT;
    strncpy(rpm.unit, "RPM", sizeof(rpm.unit) - 1);

    ecuio_signal_def_t& temp = defs[1];
    temp = {};
    temp.id = 2001;
    strncpy(temp.name, "Coolant_Temp", sizeof(temp.name) - 1);
    temp.kind = ECUIO_VALUE_INT;
    strncpy(temp.unit, "degC", sizeof(temp.unit) - 1);

    set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
}

} // namespace

TEST(BridgeTest, HistoryRingEvictsOldestAtCapacity)
{
    /* Deterministic: injected timestamps + values equal to the timestamp,
       so retention/ordering asserts are exact arithmetic, not timing. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // Push 10 MORE batches than capacity: 0..129 into a 120-slot ring.
    std::vector<std::pair<uint16_t, double>> batch;
    for (qint64 t = 0; t < SignalListModel::HistoryCapacity + 10; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t) - 40.0);
        model.updateValues(batch, t);
    }

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 2);   // both signals subscribed with history

    for (int s = 0; s < 2; ++s) {
        const QVariantMap m = data.at(s).toMap();
        const QVariantList samples = m.value("samples").toList();
        // Exactly capacity survives — ring size is a hard bound, no leaks
        ASSERT_EQ(samples.size(), SignalListModel::HistoryCapacity * 2)
            << "wrong sample count for " << m.value("name").toString().toStdString();

        // Oldest retained = push #(130-120) = timestamp 10; newest = 129.
        // Exact ints: injected timestamps make wrap position deterministic.
        EXPECT_EQ(samples.at(0).toDouble(), 10.0);
        EXPECT_EQ(samples.at(samples.size() - 2).toDouble(), 129.0);

        // Values track their timestamps (order preserved through the wrap)
        const double offset = (m.value("id").toInt() == 1001) ? 0.0 : -40.0;
        EXPECT_NEAR(samples.at(1).toDouble(), 10.0 + offset, 0.01);
        EXPECT_NEAR(samples.at(samples.size() - 1).toDouble(), 129.0 + offset, 0.01);

        // Monotonic timestamps oldest->newest (wrap-around correctness)
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }
}

TEST(BridgeTest, ChartDataFiltersUnsubscribedAndEmptyBatchesDoNotEmit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    int emissions = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated,
                     [&emissions]() { ++emissions; });

    std::vector<std::pair<uint16_t, double>> batch;
    for (int t = 0; t < 5; ++t) {
        batch.clear();
        batch.emplace_back(uint16_t(1001), double(t));
        batch.emplace_back(uint16_t(2001), double(t));
        model.updateValues(batch, t);
    }
    ASSERT_EQ(emissions, 5);   // one per non-empty batch, per contract

    // Empty batch = silent: no dataChanged, no valuesUpdated
    model.updateValues({}, 999);
    EXPECT_EQ(emissions, 5);

    // Unknown IDs = silent too (defensive: staging should never send them)
    batch.clear();
    batch.emplace_back(uint16_t(9999), 1.0);
    model.updateValues(batch, 1000);
    EXPECT_EQ(emissions, 5);
    EXPECT_EQ(model.chartData().size(), 2);

    // Unsubscribe signal 0: excluded from chartData
    model.toggleSubscribed(0);
    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("id").toInt(), 2001);
    // `row` is the model row, not the position in chartData(), so the
    // trace keeps its colour when a signal before it is unsubscribed
    EXPECT_EQ(data.at(0).toMap().value("row").toInt(), 1);
    EXPECT_EQ(data.at(0).toMap().value("samples").toList().size(), 10); // still 5 samples

    // Resubscribed signal keeps its accumulated history (non-destructive)
    model.toggleSubscribed(0);
    EXPECT_EQ(model.chartData().size(), 2);
    EXPECT_EQ(model.chartData().at(0).toMap().value("samples").toList().size(), 10);
}

TEST(BridgeTest, UnsubscribedSignalKeepsUpdating)
{
    /* A subscription chooses what the chart plots, nothing else. An
     * unsubscribed row still takes values, so the Dashboard's gauge for
     * it stays live instead of dimming as stale while its frames arrive,
     * and its history keeps growing, so the trace is whole when it's
     * plotted again. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    model.toggleSubscribed(0);   // row 0 is id 1001

    bool row0Notified = false;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&row0Notified](const QModelIndex& topLeft, const QModelIndex& bottomRight,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::ValueRole)
                && topLeft.row() <= 0 && bottomRight.row() >= 0) {
                row0Notified = true;
            }
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 3000.0);
    batch.emplace_back(uint16_t(2001), 7.0);
    model.updateValues(batch, 10);

    const QModelIndex row0 = model.index(0, 0);
    EXPECT_TRUE(row0Notified);
    EXPECT_FALSE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_DOUBLE_EQ(model.data(row0, SignalListModel::ValueRole).toDouble(), 3000.0);
    EXPECT_TRUE(model.data(row0, SignalListModel::HasValueRole).toBool());

    // Only the subscribed row is plotted
    const QVariantList plotted = model.chartData();
    ASSERT_EQ(plotted.size(), 1);
    EXPECT_EQ(plotted.at(0).toMap().value("id").toInt(), 2001);

    // Plotted again: the sample taken while unsubscribed is in its trace
    model.toggleSubscribed(0);
    const QVariantList both = model.chartData();
    ASSERT_EQ(both.size(), 2);
    EXPECT_EQ(both.at(0).toMap().value("id").toInt(), 1001);
    EXPECT_EQ(both.at(0).toMap().value("samples").toList().size(), 2);   // one [ts, val]
}

/* LiveView's delegate and SignalHub's Entry take these roles as
 * required properties, by NAME. A missing name isn't a build error: the
 * delegate fails at creation and its row never shows. */
TEST(BridgeTest, SignalRoleNamesMatchQmlBindings)
{
    SignalListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    EXPECT_EQ(roles.value(SignalListModel::NameRole), QByteArray("name"));
    EXPECT_EQ(roles.value(SignalListModel::UnitRole), QByteArray("unit"));
    EXPECT_EQ(roles.value(SignalListModel::ValueRole), QByteArray("value"));
    EXPECT_EQ(roles.value(SignalListModel::SubscribedRole), QByteArray("subscribed"));
    EXPECT_EQ(roles.value(SignalListModel::HasValueRole), QByteArray("hasValue"));
    EXPECT_EQ(roles.value(SignalListModel::AgeMsRole), QByteArray("ageMs"));
    EXPECT_EQ(roles.value(SignalListModel::DecimalsRole), QByteArray("decimals"));
}

TEST(BridgeTest, DecimalsComeFromTheDefAndReachTheChart)
{
    /* Display precision has one source, the def's decimal_places:
     * LiveView and the Dashboard format with DecimalsRole, the strip
     * chart's value labels with chartData's "decimals". */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[0].decimal_places = 0;
    defs[1].decimal_places = 2;
    model.setSignalSet(&set);

    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::DecimalsRole).toInt(), 0);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::DecimalsRole).toInt(), 2);
    EXPECT_FALSE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(2001), 21.5);
    model.updateValues(batch, 0);
    EXPECT_TRUE(model.data(model.index(1, 0), SignalListModel::HasValueRole).toBool());

    const QVariantList data = model.chartData();
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.at(0).toMap().value("decimals").toInt(), 2);
}

TEST(BridgeTest, StripChartDataPopulatedByLiveStream)
{
    /* End-to-end through the coalescer: real mock frames -> decode ->
       staging -> updateValues -> ring. Poll to the condition instead
       of a fixed 750ms window: the CONTRACT is history accumulates,
       not that 7.5 windows elapse. Monotonicity assertions are timing-
       independent once the data exists. */
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://strips").isEmpty());

    // CONTRACT: every subscribed signal has at least 4 samples
    auto chart_ready = [&bridge]() {
        const QVariantList data = bridge.signalModel()->chartData();
        if (data.isEmpty()) {
            return false;
        }
        for (const QVariant& v : data) {
            if (v.toMap().value("samples").toList().size() < 4) {
                return false;
            }
        }
        return true;
    };
    ASSERT_TRUE(spinUntil(chart_ready, 10000))
        << "strip chart history did not accumulate in 10s";

    const QVariantList data = bridge.signalModel()->chartData();
    ASSERT_GT(data.size(), 0);

    for (const QVariant& v : data) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();

        // Sample count floor satisfied by the poll; upper bound sanity
        // (poll exits promptly, so a healthy stream sits near 4-8)
        EXPECT_GE(samples.size(), 4)
            << "no history for " << m.value("name").toString().toStdString();

        // Timestamps monotonic non-decreasing across the window
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// J2534 picker surface: discovery marshaling + currentUri contract
// ---------------------------------------------------------------------------

TEST(BridgeTest, DiscoveredDevicesHaveWellFormedEntries)
{
    /* Marshaling only — the discovery itself is covered by
       test_j2534_discovery against the live registry. Count is
       registry-dependent (0..16, empty on non-Windows), so this pins
       SHAPE: every entry carries string vendorName/dllPath/architecture.
       An entry with a missing or non-string field would feed garbage
       into the SettingsPage picker's bindings. */
    QtTuneBridge bridge;
    const QVariantList devices = bridge.discoverJ2534Devices();

    EXPECT_GE(devices.size(), 0);   // documents the legitimate empty case
    for (const QVariant& v : devices) {
        ASSERT_TRUE(v.canConvert<QVariantMap>()) << "entry not a map";
        const QVariantMap m = v.toMap();
        ASSERT_TRUE(m.contains("vendorName")) << "missing vendorName";
        ASSERT_TRUE(m.contains("dllPath")) << "missing dllPath";
        ASSERT_TRUE(m.contains("architecture")) << "missing architecture";
        ASSERT_TRUE(m.value("vendorName").canConvert<QString>());
        ASSERT_TRUE(m.value("dllPath").canConvert<QString>());
        ASSERT_TRUE(m.value("architecture").canConvert<QString>());
    }
}

TEST(BridgeTest, CurrentUriTracksConnectState)
{
    /* currentUri: empty while disconnected, set after a successful
       connect, cleared by disconnect. The QML picker displays it as
       the ground truth of what's live — staleness here lies to the
       user. Failed connects must not leave a residue either. */
    QtTuneBridge bridge;
    EXPECT_TRUE(bridge.currentUri().isEmpty());

    ASSERT_TRUE(bridge.connectSession("mock://uri-track").isEmpty());
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://uri-track"));

    bridge.disconnectSession();
    EXPECT_TRUE(bridge.currentUri().isEmpty());   // cleared at once
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 5000, 10));

    // Failed connect leaves no stale URI behind
    EXPECT_FALSE(bridge.connectSession("j2534://definitely-not-a-vendor").isEmpty());
    EXPECT_TRUE(bridge.currentUri().isEmpty());
    EXPECT_FALSE(bridge.connected());
}

TEST(BridgeTest, AsyncConnectCompletesOffGuiThread)
{
    QtTuneBridge bridge;
    // Run a spinner on the GUI thread DURING the connect: if the
    // blocking core calls ran on this thread, the spinner would be
    // starved past its deadline. The mock's connect is fast, so give
    // a generous-but-finite margin.
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &QtTuneBridge::connectResult,
                     [&](const QString&, const QString& error) {
                         gotErr = error;
                         done.store(true);
                     });

    bridge.connectSessionAsync(QStringLiteral("mock://demo"));

    QElapsedTimer timer; timer.start();
    while (!done.load() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(done.load());
    EXPECT_EQ(gotErr.toStdString(), "");
    EXPECT_TRUE(bridge.connected());
    bridge.disconnectSession();
    EXPECT_FALSE(bridge.connected());
}

namespace {
void countFramesCb(const ecuio_frame_t*, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}
}  // namespace

/* An async connect whose completion never runs, because the bridge is
 * destroyed first, must not leave a running session calling onFrame on
 * the freed bridge. The connect thread creates and registers but doesn't
 * start, and the destructor destroys what it left pending. The event
 * loop isn't spun while the bridge lives, so the completion can't run;
 * a started mock session would reach the probe callback every 50 ms. */
TEST(BridgeTest, DestroyedMidConnectLeavesNoRunningSession)
{
    auto bridge = std::make_unique<QtTuneBridge>();
    bridge->connectSessionAsync(QStringLiteral("mock://demo"));

    ecuio_session_t* pending = nullptr;
    for (int i = 0; i < 500 && pending == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pending = bridge->pendingSessionForTesting();
    }
    ASSERT_NE(pending, nullptr) << "the connect thread produced no session";

    std::atomic<int> frames{0};
    ASSERT_EQ(ecuio_register_frame_callback(pending, countFramesCb, &frames), ECUIO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(frames.load(), 0) << "the session started before the GUI thread took it";
    ASSERT_EQ(ecuio_unregister_frame_callback(pending, countFramesCb, &frames), ECUIO_OK);

    bridge.reset();   // the queued completion is dropped; the destructor destroys `pending`
    spinLoop(50);     // nothing queued may run against the freed bridge
    EXPECT_EQ(frames.load(), 0);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Connect and disconnect threads, against the fake J2534 DLL (in-proc):
// its delay knobs stall the vendor call, and its hooks say which thread
// made it.
// ---------------------------------------------------------------------------

namespace {

/* Sets a fake-DLL env knob for one test and clears it on exit, so a
 * failing ASSERT can't leak it into the next case. The in-proc fake
 * re-reads the knob on every call. */
class FakeEnvArm
{
public:
    FakeEnvArm(const char* name, const char* value) : m_name(name)
    {
        SetEnvironmentVariableA(name, value);
    }
    ~FakeEnvArm() { SetEnvironmentVariableA(m_name, nullptr); }
    FakeEnvArm(const FakeEnvArm&) = delete;
    FakeEnvArm& operator=(const FakeEnvArm&) = delete;

private:
    const char* m_name;
};

using fn_get_last_call_thread_ids = void (*)(unsigned long*, unsigned long*);
using fn_get_disconnect_count = unsigned long (*)(void);

/* The fake's hooks. Holding our own module reference also keeps its
 * counters alive across the session's FreeLibrary. */
struct FakeHooks
{
    HMODULE mod = nullptr;
    fn_get_last_call_thread_ids thread_ids = nullptr;
    fn_get_disconnect_count disconnect_count = nullptr;

    bool load()
    {
        mod = LoadLibraryExA(FAKE_J2534_DLL_PATH, nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod == nullptr) return false;
        thread_ids = reinterpret_cast<fn_get_last_call_thread_ids>(
            GetProcAddress(mod, "GetLastCallThreadIds"));
        disconnect_count = reinterpret_cast<fn_get_disconnect_count>(
            GetProcAddress(mod, "GetDisconnectCount"));
        return thread_ids != nullptr && disconnect_count != nullptr;
    }
    ~FakeHooks()
    {
        if (mod != nullptr) FreeLibrary(mod);
    }
};

/* A passive raw session: no requests, so the vendor calls under test
 * are the open and the teardown. */
QString fakeUri()
{
    return QStringLiteral("j2534://%1?profile=raw")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

unsigned long guiThreadId()
{
    return static_cast<unsigned long>(GetCurrentThreadId());
}

}  // namespace

/* An async connect makes the slow vendor open on its own thread: with
 * PassThruOpen stalled, connectSessionAsync returns at once, and the
 * open ran on a thread other than this (GUI) one. */
TEST(BridgeTest, AsyncConnectOpensOffGuiThread)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm delay("ECUIO_FAKE_OPEN_DELAY_MS", "600");

    QtTuneBridge bridge;
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &QtTuneBridge::connectResult,
                     [&](const QString&, const QString& error) {
                         gotErr = error;
                         done.store(true);
                     });

    QElapsedTimer clock;
    clock.start();
    bridge.connectSessionAsync(fakeUri());
    EXPECT_LT(clock.elapsed(), 300) << "connectSessionAsync blocked on the open";
    EXPECT_TRUE(bridge.connecting());

    ASSERT_TRUE(spinUntil([&done]() { return done.load(); }, 10000, 10));
    EXPECT_EQ(gotErr.toStdString(), "");
    ASSERT_TRUE(bridge.connected());

    unsigned long open_tid = 0;
    hooks.thread_ids(&open_tid, nullptr);
    EXPECT_NE(open_tid, 0ul);
    EXPECT_NE(open_tid, guiThreadId());

    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A slow vendor teardown runs off the GUI thread. With PassThruDisconnect
 * stalled, disconnectSession returns at once with the session already
 * gone as far as the UI can tell; `disconnecting` holds, and refuses a
 * connect, until the teardown ends; the log brackets it. */
TEST(BridgeTest, DisconnectTearsDownOffGuiThread)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());

    QtTuneBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &QtTuneBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    const unsigned long disconnects_before = hooks.disconnect_count();

    FakeEnvArm delay("ECUIO_FAKE_DISCONNECT_DELAY_MS", "800");
    QElapsedTimer clock;
    clock.start();
    bridge.disconnectSession();
    EXPECT_LT(clock.elapsed(), 300) << "disconnectSession blocked on the teardown";
    EXPECT_FALSE(bridge.connected());
    EXPECT_TRUE(bridge.disconnecting());
    EXPECT_EQ(bridge.linkState(), QStringLiteral("disconnected"));
    EXPECT_TRUE(bridge.currentUri().isEmpty());

    // No connect while the adapter is still closing
    EXPECT_FALSE(bridge.connectSession(fakeUri()).isEmpty());
    bridge.connectSessionAsync(fakeUri());
    EXPECT_FALSE(bridge.connecting());

    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.disconnecting(); }, 10000, 10));
    EXPECT_GE(clock.elapsed(), 700) << "the stall never reached the teardown";
    EXPECT_EQ(hooks.disconnect_count(), disconnects_before + 1);

    unsigned long disconnect_tid = 0;
    hooks.thread_ids(nullptr, &disconnect_tid);
    EXPECT_NE(disconnect_tid, 0ul);
    EXPECT_NE(disconnect_tid, guiThreadId());

    ASSERT_GE(log.size(), 2);
    EXPECT_TRUE(log.at(log.size() - 2).startsWith(QStringLiteral("Disconnecting: ")));
    EXPECT_TRUE(log.last().startsWith(QStringLiteral("Disconnected: ")));

    // The adapter is free again once the teardown has ended
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A bridge destroyed mid-teardown waits for it: the destructor joins the
 * teardown thread before shutting core down, and the dropped completion
 * never runs against the freed bridge. */
TEST(BridgeTest, DestroyedMidDisconnectWaitsForTeardown)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());

    auto bridge = std::make_unique<QtTuneBridge>();
    ASSERT_TRUE(bridge->connectSession(fakeUri()).isEmpty());
    const unsigned long disconnects_before = hooks.disconnect_count();

    FakeEnvArm delay("ECUIO_FAKE_DISCONNECT_DELAY_MS", "500");
    bridge->disconnectSession();
    ASSERT_TRUE(bridge->disconnecting());

    QElapsedTimer clock;
    clock.start();
    bridge.reset();
    EXPECT_GE(clock.elapsed(), 300) << "the destructor didn't wait for the teardown";
    EXPECT_EQ(hooks.disconnect_count(), disconnects_before + 1);
    spinLoop(50);   // the dropped completion must not run
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// ADR-0013: the bridge renders the SESSION's profile, not a hardcoded
// set
// ---------------------------------------------------------------------------

TEST(BridgeTest, RawProfileLeavesSignalModelEmpty)
{
    /* A raw session after a mock session must CLEAR the signal model.
     * Failure shape guarded: a setSignalSet that early-returns on NULL
     * without clearing leaves a raw session displaying the PREVIOUS
     * session's rows with no live updates — stale data posing as live.
     * Frames still flow to the Log page; decode alone is silenced. */
    QtTuneBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://profile-then-raw").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalModel()->rowCount() > 0; }, 5000));
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge.connectSession("mock://profile-raw-stage2?profile=raw")
                    .isEmpty());
    /* Frames keep arriving (mock streams regardless of profile) —
     * the Log page stays live while LiveView stays empty. */
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    EXPECT_EQ(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, AgeMsRoleEmittedAndRefreshesWhileStalled)
{
    /* Freshness contract: (a) updateValues emits dataChanged carrying
     * AgeMsRole; (b) refreshAges emits over all rows for AgeMsRole
     * ONLY — the stalled-row dimming path; (c) ageMs is -1 before the
     * first sample and a sane clock delta after. ageMs's wall-clock
     * basis is nondeterministic by nature, so assert ranges, not
     * exact values. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);

    // -1 before any sample
    EXPECT_EQ(model.data(model.index(0, 0),
                         SignalListModel::AgeMsRole).toInt(), -1);

    int ageEmissions = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&ageEmissions](const QModelIndex&, const QModelIndex&,
                        const QList<int>& roles) {
            if (roles.contains(SignalListModel::AgeMsRole)) ++ageEmissions;
        });

    std::vector<std::pair<uint16_t, double>> batch;
    batch.emplace_back(uint16_t(1001), 1500.0);
    model.updateValues(batch, 0);   // deterministic stamp of 0
    EXPECT_GE(ageEmissions, 1);

    const int age = model.data(model.index(0, 0),
                               SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000);   // clock basis sanity, not timing

    const int before = ageEmissions;
    model.refreshAges();   // the stalled-row path
    EXPECT_EQ(ageEmissions, before + 1);

    SignalListModel empty;
    empty.refreshAges();   // no rows — silent, no crash
}

TEST(BridgeTest, LinkStateMapsEveryPublishedCode)
{
    /* The state-string mapping, pinned session-free via the static
     * helper. Covers all four link-health codes plus the forward-
     * compatible fallback; QML's Theme keys colors and labels on
     * these strings. */
    EXPECT_EQ(QtTuneBridge::linkStateForError(ECUIO_OK), QStringLiteral("ok"));
    EXPECT_EQ(QtTuneBridge::linkStateForError(ECUIO_ERR_VEHICLE_TIMEOUT), QStringLiteral("quiet"));
    EXPECT_EQ(QtTuneBridge::linkStateForError(ECUIO_ERR_REQUEST_FAILED), QStringLiteral("requests"));
    EXPECT_EQ(QtTuneBridge::linkStateForError(ECUIO_ERR_TRANSPORT_HARD_FAILURE), QStringLiteral("fault"));
    EXPECT_EQ(QtTuneBridge::linkStateForError(static_cast<ecuio_status_t>(999)), QStringLiteral("degraded"));
}
