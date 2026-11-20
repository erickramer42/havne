// File/layer: tests/test_bridge.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // ONE instance, lives for the process
    // Every bridge's settings folder (its stores) in a scratch folder,
    // never the user's app data; a case that reads one sets its own
    QTemporaryDir settings;
    if (!settings.isValid()) {
        return 1;
    }
    qputenv("HAVNE_SETTINGS_DIR", settings.path().toUtf8());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(BridgeTest, LifecycleIsIdempotent)
{
    SessionBridge bridge;

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

/* The built-in layouts name a signal by a key here, one ID for each set the
 * connection may use: boost is the mock's computed one and obd2's */
TEST(BridgeTest, GaugeSignalsNameBoostOnTheMockAndObd2)
{
    SessionBridge bridge;
    const QVariantList boost = bridge.gaugeSignals().value("boost").toList();
    ASSERT_EQ(boost.size(), 2);
    EXPECT_EQ(boost[0].toString(),
              QString("id:%1").arg(ECUIO_MOCK_SIG_BOOST_ID));
    EXPECT_EQ(boost[1].toString(),
              QString("id:%1").arg(ECUIO_OBD2_SIG_BOOST_ID));
}

/* The built-in Lambda cell reads the demo's lambda or obd2's commanded one
 * (PID 44 field 0): the cell's description says "commanded" */
TEST(BridgeTest, GaugeSignalsNameLambdaOnTheMockAndObd2sCommandedOne)
{
    SessionBridge bridge;
    const QVariantList lambda = bridge.gaugeSignals().value("lambda").toList();
    ASSERT_EQ(lambda.size(), 2);
    EXPECT_EQ(lambda[0].toString(),
              QString("id:%1").arg(ECUIO_MOCK_SIG_LAMBDA_ID));
    EXPECT_EQ(lambda[1].toString(),
              QString("id:%1").arg(ECUIO_OBD2_PID_SIG_ID(0x44, 0)));
}

TEST(BridgeTest, InvalidUriReturnsError)
{
    SessionBridge bridge;
    EXPECT_FALSE(bridge.connectSession("j2534://dev0").isEmpty());   // no transport
    EXPECT_FALSE(bridge.connected());

    EXPECT_FALSE(bridge.connectSession("").isEmpty());               // bad URI
    EXPECT_FALSE(bridge.connected());
}

// The error a failed connect returns, and logs, names the cause: the
// status, then core's detail.
TEST(BridgeTest, FailedConnectNamesItsCause)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.connectSession("mock://demo?interval=10"),
              QStringLiteral("malformed transport URI: mock:// takes no query "
                             "key 'interval', only profile, did and diagnose"));
    EXPECT_EQ(bridge.connectSession("mock://demo?profile=bogus"),
              QStringLiteral("unknown profile: no profile is named 'bogus'"));
    EXPECT_FALSE(bridge.connected());
    EXPECT_EQ(bridge.statusString(ECUIO_ERR_BAD_URI),
              QStringLiteral("malformed transport URI"));
}

// An app-fed stream's link is readied before core is asked (ADR-0028):
// the preparer sees the trimmed URI before each connect, sync or async,
// and its refusal fails the connect with its words, logged as any other
TEST(BridgeTest, AStreamPreparersRefusalFailsTheConnect)
{
    SessionBridge bridge;
    QStringList asked;
    bridge.setStreamPreparer([&asked](const QString& uri) {
        asked << uri;
        return uri.startsWith(QStringLiteral("elm327://bt:"))
                   ? QStringLiteral("bt:AA: not reachable")
                   : QString();
    });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_EQ(bridge.connectSession("  elm327://bt:AA?poll=0x0C "),
              QStringLiteral("bt:AA: not reachable"));
    EXPECT_FALSE(bridge.connected());
    ASSERT_EQ(asked.size(), 1);
    EXPECT_EQ(asked.front(), QStringLiteral("elm327://bt:AA?poll=0x0C"));
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Connect failed: elm327://bt:AA?poll=0x0C (bt:AA: not reachable)")))
        << log.join('\n').toStdString();

    QString result = QStringLiteral("none");
    QObject::connect(
        &bridge, &SessionBridge::connectResult,
        [&result](const QString&, const QString& error) { result = error; });
    bridge.connectSessionAsync("elm327://bt:AA");
    EXPECT_EQ(result, QStringLiteral("bt:AA: not reachable"))
        << "refused at once";
    EXPECT_FALSE(bridge.connecting()) << "no connect started";

    EXPECT_EQ(bridge.connectSession("mock://demo"), QString());
    EXPECT_TRUE(bridge.connected()) << "a URI it passes connects";
    EXPECT_EQ(asked.size(), 3);
    bridge.disconnectSession();
}

TEST(BridgeTest, FramesMarshalFromWorkerToGuiThread)
{
    SessionBridge bridge;

    // The frame log's countChanged fires on the thread that appends
    std::atomic<Qt::HANDLE> append_thread{nullptr};
    QObject::connect(
        bridge.model(),
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
    ASSERT_TRUE(flowing) << "no frames in 5s: stream is dead, not slow";

    // Frames flowed...
    EXPECT_GT(bridge.frameCount(), 0);
    // ...and they landed on the thread running this event loop.
    EXPECT_EQ(append_thread.load(), QThread::currentThreadId());
    EXPECT_NE(append_thread.load(), nullptr);

    const int frames_before = bridge.frameCount();

    // Mid-stream disconnect: late-arriving frames must be dropped,
    // not crash, not append. Equality assertion after a settle window
    // is duration-independent: any arriving frame would break it.
    bridge.disconnectSession();
    spinLoop(300);   // settle: let any in-flight lambdas drain and drop
    EXPECT_EQ(bridge.frameCount(), frames_before);
}

TEST(BridgeTest, ClearFramesResets)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://test").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 0; }, 5000));
    ASSERT_GT(bridge.frameCount(), 0);

    bridge.clearFrames();
    EXPECT_EQ(bridge.frameCount(), 0);
    EXPECT_EQ(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, NotifyCountTracksFrameProcessing)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://notify").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.guiNotifyCount() > 0; }, 5000));
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount());
    EXPECT_GT(bridge.guiNotifyCount(), 0);
    bridge.disconnectSession();
}

TEST(BridgeTest, SignalBatchesAreCoalesced)
{
    SessionBridge bridge;
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
    // exact equality: one emission per frame.
    EXPECT_LE(bridge.signalBatchCount(), bridge.frameCount());

    // Values actually flowed through the model
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, StreamingContinuesPastTenThousandFrames)
{
    SessionBridge bridge;
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
    // 600ms wait (needs ~2 frames): poll until it's demonstrably past.
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() > 10000; }, 5000))
        << "stream stalled at " << bridge.frameCount();
    EXPECT_GT(bridge.frameCount(), 10000);

    // And no drops occurred: nothing was rejected at the boundary
    EXPECT_EQ(bridge.droppedFrames(), 0);

    // Liveness, not coincidence: the log model kept receiving rows too
    EXPECT_GT(bridge.model()->rowCount(), 0);

    bridge.disconnectSession();
}

TEST(BridgeTest, BackpressureDropsAreCountedAndReported)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://drops").isEmpty());

    // Lambda-counter instead of QSignalSpy: avoids a new Qt6::Test dependency
    int notifyCount = 0;
    int lastReported = -1;
    QObject::connect(&bridge, &SessionBridge::droppedFramesChanged,
                     [&](int dropped) { ++notifyCount; lastReported = dropped; });

    ecuio_frame_t f{};
    f.data_length = 8;

    const int inject = SessionBridge::maxInFlightForTesting() + 500;

    // Synchronous flood from the test thread: no event processing happens,
    // so in-flight climbs monotonically to the cap and stays there.
    for (int i = 0; i < inject; ++i) {
        f.timestamp_us = 1000000 + static_cast<uint64_t>(i);
        bridge.dispatchFrameForTesting(f);
    }

    // Drops happened during saturation (loop 1001..1500 hit the gate).
    // GE not EQ: the mock worker may contribute a frame or two mid-loop.
    EXPECT_GE(bridge.droppedFrames(), 500);

    // Nothing could have been reported yet: the queue hasn't been processed
    EXPECT_EQ(notifyCount, 0);

    // CONTRACT: the backlog drains. Was a fixed 800ms; poll until the
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
    // Drained backlog frees the gate again: no permanent wedge
    // (the running mock resumes normal delivery after this)

    bridge.disconnectSession();
}

// ---------------------------------------------------------------------------
// J2534 picker surface: discovery marshaling + currentUri contract
// ---------------------------------------------------------------------------

TEST(BridgeTest, DiscoveredDevicesHaveWellFormedEntries)
{
    /* Marshaling only: the discovery itself is covered by
       test_j2534_discovery against the live registry. Count is
       registry-dependent (0..16, empty on non-Windows), so this pins
       SHAPE: every entry carries string vendorName/dllPath/architecture.
       An entry with a missing or non-string field would feed garbage
       into the SettingsPage picker's bindings. */
    SessionBridge bridge;
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

/* The serial ports as core lists them, name for name and in its order
 * (most machines have none, and the call itself is SerialPortsTest's) */
TEST(BridgeTest, SerialPortsAreListedAsCoreFindsThem)
{
    SessionBridge bridge;
    const QVariantList listed = bridge.discoverSerialPorts();

    uint32_t count = 0;
    ASSERT_EQ(ecuio_serial_ports(nullptr, 0, &count), ECUIO_OK);
    std::vector<ecuio_serial_port_t> ports(count);
    ASSERT_EQ(ecuio_serial_ports(ports.data(), count, &count), ECUIO_OK);
    ASSERT_EQ(listed.size(), static_cast<qsizetype>(count))
        << "a port came or went between the two calls";
    for (uint32_t i = 0; i < count; ++i) {
        const QVariantMap m = listed[static_cast<qsizetype>(i)].toMap();
        EXPECT_EQ(m.value("name").toString().toStdString(), ports[i].name);
        EXPECT_EQ(m.value("device").toString().toStdString(), ports[i].device);
    }
}

TEST(BridgeTest, CurrentUriTracksConnectState)
{
    /* currentUri: empty while disconnected, set after a successful
       connect, cleared by disconnect. The QML picker displays it as
       the ground truth of what's live: staleness here lies to the
       user. Failed connects must not leave a residue either. */
    SessionBridge bridge;
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

TEST(BridgeTest, ConnectTrimsSpacesAroundTheUri)
{
    // A URI pasted with spaces around it connects, as the URI without
    // them; core alone refuses it (the scheme is matched at the first
    // character)
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("  mock://padded \t")).isEmpty());
    EXPECT_TRUE(bridge.connected());
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://padded"));
    ASSERT_TRUE(disconnectAndWait(bridge));

    bridge.connectSessionAsync(QStringLiteral(" mock://padded-async "));
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.connected(); }, 5000, 10));
    EXPECT_EQ(bridge.currentUri(), QStringLiteral("mock://padded-async"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A Bluetooth adapter's address is personal data, and the label reaches
 * the phone's lock screen in the session's notification: the last two
 * octets stay, the query too. A TCP address, a serial port, a short or
 * empty address and another stream kind pass */
TEST(BridgeTest, UriLabelKeepsABluetoothAddresssLastOctets)
{
    const QString dots = QString(QChar(0x2026));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("elm327://bt:00:11:22:33:44:55")),
              QStringLiteral("elm327://bt:") + dots + QStringLiteral(":44:55"));
    EXPECT_EQ(SessionBridge::uriLabelFor(QStringLiteral(
                  "elm327://ble:00:11:22:33:44:55?profile=raw&filter=0x7E8")),
              QStringLiteral("elm327://ble:") + dots +
                  QStringLiteral(":44:55?profile=raw&filter=0x7E8"));
    for (const char* uri :
         {"elm327://192.168.4.1:23", "elm327://COM7?serial=115200",
          "elm327:///dev/rfcomm0", "elm327://bt:44:55",
          "elm327://bt:", "elm327://usb:00:11:22:33:44:55", "elm327://"}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }
}

TEST(BridgeTest, UriLabelCutsADirectLoadPathToItsFileName)
{
    const QString dots = QString(QChar(0x2026));
    // A path selector, either separator, a drive or UNC root, the query kept
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://E:/dev/build/fake_j2534.dll")),
              QStringLiteral("j2534://") + dots +
                  QStringLiteral("/fake_j2534.dll"));
    EXPECT_EQ(SessionBridge::uriLabelFor(QStringLiteral(
                  "j2534://C:\\Program "
                  "Files\\Vendor\\vendor32.dll?profile=raw&filter=0x7E8")),
              QStringLiteral("j2534://") + dots +
                  QStringLiteral("/vendor32.dll?profile=raw&filter=0x7E8"));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://\\\\server\\share\\pt.dll")),
              QStringLiteral("j2534://") + dots + QStringLiteral("/pt.dll"));

    // A '/' in the query isn't a path
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("j2534://OBDX Pro - VX?did=a/b")),
              QStringLiteral("j2534://OBDX Pro - VX?did=a/b"));
    // Vendor names, the first device, the mock and nothing pass unchanged
    for (const char* uri :
         {"j2534://OBDX Pro - VX", "j2534://", "j2534://?probe=0",
          "mock://demo", "mock://a/b", ""}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }

    // The property follows currentUri
    SessionBridge bridge;
    EXPECT_TRUE(bridge.uriLabel().isEmpty());
    int changed = 0;
    QObject::connect(&bridge, &SessionBridge::currentUriChanged,
                     [&changed]() { ++changed; });
    ASSERT_TRUE(bridge.connectSession("mock://label").isEmpty());
    EXPECT_EQ(bridge.uriLabel(), QStringLiteral("mock://label"));
    EXPECT_EQ(changed, 1);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.uriLabel().isEmpty());
    EXPECT_EQ(changed, 2);
}

TEST(BridgeTest, AsyncConnectCompletesOffGuiThread)
{
    SessionBridge bridge;
    // Run a spinner on the GUI thread DURING the connect: if the
    // blocking core calls ran on this thread, the spinner would be
    // starved past its deadline. The mock's connect is fast, so give
    // a generous-but-finite margin.
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &SessionBridge::connectResult,
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

/* Every emit while a connect is applied runs QML handlers synchronously,
 * after the session's worker has started. A connect a handler starts
 * there is refused, so the first session stays the bridge's; let through,
 * it would replace m_session and leave the first worker posting to the
 * bridge with nothing to close it. Sync connect: currentUriChanged, one
 * of the apply's emits. */
TEST(BridgeTest, AConnectFromAHandlerDuringTheApplyIsRefused)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bool tried = false;
    QObject::connect(&bridge, &SessionBridge::currentUriChanged, [&]() {
        if (!tried && !bridge.currentUri().isEmpty()) {
            tried = true;
            (void)bridge.connectSession(QStringLiteral("mock://second"));
        }
    });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://first")).isEmpty());
    ASSERT_TRUE(tried);
    EXPECT_EQ(bridge.currentUri().toStdString(), "mock://first");
    EXPECT_EQ(log.filter(QStringLiteral("Connected:")).size(), 1)
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The async completion: connectingChanged, as connecting clears, is a
 * handler's chance too */
TEST(BridgeTest, AConnectFromConnectingChangedIsRefused)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bool tried = false;
    QObject::connect(&bridge, &SessionBridge::connectingChanged, [&]() {
        if (!tried && !bridge.connecting()) {
            tried = true;
            bridge.connectSessionAsync(QStringLiteral("mock://second"));
        }
    });
    bridge.connectSessionAsync(QStringLiteral("mock://first"));
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.connected(); }, 5000));
    spinLoop(300); // a second connect let through completes in here
    ASSERT_TRUE(tried);
    EXPECT_EQ(bridge.currentUri().toStdString(), "mock://first");
    EXPECT_TRUE(log.filter(QStringLiteral("mock://second")).isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(log.filter(QStringLiteral("Connected:")).size(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

void countFramesCb(const ecuio_frame_t*, void* user_data)
{
    static_cast<std::atomic<int>*>(user_data)->fetch_add(1);
}

} // namespace

/* An async connect whose completion never runs, because the bridge is
 * destroyed first, must not leave a running session calling onFrame on
 * the freed bridge. The connect thread creates and registers but doesn't
 * start, and the destructor destroys what it left pending. The event
 * loop isn't spun while the bridge lives, so the completion can't run;
 * a started mock session would reach the probe callback every 50 ms. */
TEST(BridgeTest, DestroyedMidConnectLeavesNoRunningSession)
{
    auto bridge = std::make_unique<SessionBridge>();
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
