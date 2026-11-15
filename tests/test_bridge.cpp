// File/layer: tests/test_bridge.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include <cmath>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "new_counter.h" // operator new counted per thread: the conditions callback
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
    // Every bridge's signal store (SignalStore::root) in a scratch folder,
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

}  // namespace

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

/* The "Session:" line's wording, from hand-built infos: every field
 * it names, and what it says when a reading is missing. */
TEST(BridgeTest, SessionInfoTextNamesTheAdapter)
{
    ecuio_session_info_t mock{};
    mock.backend = ECUIO_BACKEND_MOCK;
    qstrncpy(mock.profile, "mock", sizeof(mock.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(mock).toStdString(),
              "Session: mock transport; profile mock");

    ecuio_session_info_t vx{};
    vx.backend = ECUIO_BACKEND_J2534_SURROGATE;
    vx.dll_arch = ECUIO_DLL_ARCH_X86;
    vx.protocol_id = 6;
    vx.baud = 500000;
    vx.channel_count = 2;
    vx.rx_ids[0] = 0x7E8;
    vx.tx_ids[0] = 0x7E0;
    vx.rx_ids[1] = 0x7E9;
    vx.tx_ids[1] = 0x7E1;
    vx.flags = ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS;
    vx.vbatt_mv = 12640;
    qstrncpy(vx.firmware_version, "1.2.3", sizeof(vx.firmware_version));
    qstrncpy(vx.dll_version, "4.5", sizeof(vx.dll_version));
    qstrncpy(vx.api_version, "04.04", sizeof(vx.api_version));
    qstrncpy(vx.profile, "obd2", sizeof(vx.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(vx).toStdString(),
              "Session: x86 DLL, through the surrogate; ISO 15765 at 500000 "
              "baud; profile obd2, channels 0x7E8/0x7E0, 0x7E9/0x7E1 (rx/tx); "
              "battery 12.6 V; firmware 1.2.3, DLL 4.5, API 04.04");
    EXPECT_EQ(SessionBridge::versionsText(vx).toStdString(),
              "firmware 1.2.3, DLL 4.5, API 04.04");
    EXPECT_TRUE(SessionBridge::versionsText(mock).isEmpty());

    ecuio_session_info_t bare{};
    bare.backend = ECUIO_BACKEND_J2534_SURROGATE;
    bare.dll_arch = ECUIO_DLL_ARCH_X64;
    bare.protocol_id = 6;
    bare.baud = 250000;
    bare.connect_flags = 0x100;
    bare.channel_count = 1;
    bare.rx_ids[0] = 0x18DAF110;
    bare.tx_ids[0] = 0x18DA10F1;
    qstrncpy(bare.profile, "raw", sizeof(bare.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(bare).toStdString(),
              "Session: x64 DLL, through the surrogate; ISO 15765 at 250000 baud, flags 0x100; "
              "profile raw, channel 0x18DAF110/0x18DA10F1 (rx/tx); "
              "no battery reading; no version reading");
    EXPECT_TRUE(SessionBridge::versionsText(bare).isEmpty());

    /* An ELM327 on an STN chip, its port in the "Connected:" line */
    ecuio_session_info_t elm{};
    elm.backend = ECUIO_BACKEND_ELM327;
    elm.protocol_id = 6;
    elm.baud = 500000;
    elm.channel_count = 1;
    elm.rx_ids[0] = 0x7E8;
    elm.tx_ids[0] = 0x7E0;
    elm.flags = ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS;
    elm.vbatt_mv = 13800;
    qstrncpy(elm.firmware_version, "ELM327 v1.4b, STN2255 v9.9.9",
             sizeof(elm.firmware_version));
    qstrncpy(elm.profile, "obd2", sizeof(elm.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(elm).toStdString(),
              "Session: ELM327 adapter (ELM327 v1.4b, STN2255 v9.9.9); "
              "ISO 15765 at 500000 baud; profile obd2, channel "
              "0x7E8/0x7E0 (rx/tx); battery 13.8 V");
    EXPECT_EQ(SessionBridge::versionsText(elm).toStdString(),
              "ELM327 v1.4b, STN2255 v9.9.9");

    /* Set not to wait past a pending reply, and no readings */
    elm.flags = ECUIO_SESSION_INFO_PENDING_OFF;
    elm.baud = 250000;
    EXPECT_EQ(SessionBridge::sessionInfoText(elm).toStdString(),
              "Session: ELM327 adapter (no version reading); ISO 15765 "
              "at 250000 baud; profile obd2, channel 0x7E8/0x7E0 "
              "(rx/tx); no battery reading; the adapter is set not to "
              "wait past 7F xx 78 (STN parameter 2A), so a pending "
              "reply's answer is lost");
    EXPECT_TRUE(SessionBridge::versionsText(elm).isEmpty());

    /* SocketCAN (ADR-0032, ADR-0040): a listen-only controller, vcan,
     * and a requesting profile on a controller and on vcan */
    ecuio_session_info_t can{};
    can.backend = ECUIO_BACKEND_SOCKETCAN;
    can.baud = 500000;
    can.flags = ECUIO_SESSION_INFO_LISTEN_ONLY;
    qstrncpy(can.profile, "raw", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a CAN controller in listen-only mode at "
              "500000 bit/s, listening; profile raw");
    can.baud = 0;
    can.flags = 0;
    qstrncpy(can.profile, "mock", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a virtual CAN bus, listening; profile mock");
    EXPECT_TRUE(SessionBridge::versionsText(can).isEmpty());
    can.baud = 500000;
    can.channel_count = 2;
    qstrncpy(can.profile, "obd2", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a CAN controller at 500000 bit/s, "
              "requesting on 2 channels; profile obd2");
    can.baud = 0;
    can.channel_count = 1;
    qstrncpy(can.profile, "user.sim-car-own", sizeof(can.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(can).toStdString(),
              "Session: SocketCAN, a virtual CAN bus, requesting on 1 "
              "channel; profile user.sim-car-own");

    ecuio_session_info_t later{};
    later.backend = 7; // a backend this build doesn't know
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 7");
    later.backend = ECUIO_BACKEND_J2534_INPROC; // retired, never reported
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 2");
}

/* The counts are core's (pids_supported, pids_through: PidSupportTest);
 * the line says them */
/* Each session state's words (ADR-0060); none without a session asked,
 * none while 10 03 is out */
TEST(BridgeTest, ChannelSessionTextSaysEachState)
{
    ecuio_channel_support_t s{};
    s.rx_id = 0x7E8;
    s.session_state = ECUIO_SESSION_STATE_REFUSED; /* but none asked */
    s.session_nrc = 0x12;
    EXPECT_TRUE(SessionBridge::channelSessionText(s).isEmpty());
    s.session_asked = 0x03;
    s.session = 0x01;
    EXPECT_EQ(
        SessionBridge::channelSessionText(s).toStdString(),
        "ECU 0x7E8: refused the extended session (subFunctionNotSupported, "
        "7F 10 12): reading in the default session");
    s.session_nrc = 0x99;
    EXPECT_EQ(SessionBridge::channelSessionText(s).toStdString(),
              "ECU 0x7E8: refused the extended session (7F 10 99): reading in "
              "the default session");
    s.session_state = ECUIO_SESSION_STATE_NOT_ASKED;
    EXPECT_TRUE(SessionBridge::channelSessionText(s).isEmpty());
    s.session_state = ECUIO_SESSION_STATE_ENTERING;
    EXPECT_TRUE(SessionBridge::channelSessionText(s).isEmpty());
    s.session_state = ECUIO_SESSION_STATE_ENTERED;
    s.session = 0x03;
    s.session_nrc = 0;
    s.session_entries = 1;
    EXPECT_EQ(SessionBridge::channelSessionText(s).toStdString(),
              "ECU 0x7E8: reading in the extended session (50 03)");
    s.session_entries = 2;
    s.session_lost = 1;
    EXPECT_EQ(SessionBridge::channelSessionText(s).toStdString(),
              "ECU 0x7E8: left the extended session and entered it again "
              "(1 time so far)");
    s.session_lost = 2;
    EXPECT_EQ(SessionBridge::channelSessionText(s).toStdString(),
              "ECU 0x7E8: left the extended session and entered it again "
              "(2 times so far)");
    s.session_state = ECUIO_SESSION_STATE_UNANSWERED;
    s.session = 0x01;
    EXPECT_EQ(SessionBridge::channelSessionText(s).toStdString(),
              "ECU 0x7E8: no answer to 10 03: reading in the default session, "
              "asked again when the link recovers");
    s.session_state = ECUIO_SESSION_STATE_ABANDONED;
    s.session_nrc = 0x7F;
    EXPECT_EQ(
        SessionBridge::channelSessionText(s).toStdString(),
        "ECU 0x7E8: kept leaving the extended session "
        "(serviceNotSupportedInActiveSession, code 7F, 3 times within 60 s): "
        "reading in the default session");
    s.session_state = 9; /* a state this build doesn't know */
    EXPECT_TRUE(SessionBridge::channelSessionText(s).isEmpty());

    /* Disconnect's line: only for a module that entered */
    ecuio_channel_support_t r{};
    r.rx_id = 0x7E9;
    r.session_asked = 0x03;
    EXPECT_TRUE(SessionBridge::sessionReturnedText(r).isEmpty());
    r.session_entries = 1;
    EXPECT_EQ(SessionBridge::sessionReturnedText(r).toStdString(),
              "ECU 0x7E9: returning to the default session (10 01)");
    r.session_asked = 0;
    EXPECT_TRUE(SessionBridge::sessionReturnedText(r).isEmpty());

    /* The support line's last part, and its access refusal's words in
     * the extended session */
    ecuio_channel_support_t u{};
    u.rx_id = 0x7E8;
    u.requests = 1;
    u.dropped = 1;
    u.dropped_list[0] = {
        {0x22, 0xD8, 0x01}, 3, ECUIO_DROP_ACCESS, 0x33, {0, 0}};
    EXPECT_EQ(
        SessionBridge::channelSupportText(u).toStdString(),
        "ECU 0x7E8: 1 request, 1 dropped: 22 D8 01 (needs security access "
        "or another session: 7F 22 33)");
    u.session_asked = 0x03;
    u.session = 0x03;
    EXPECT_EQ(
        SessionBridge::channelSupportText(u).toStdString(),
        "ECU 0x7E8: 1 request, 1 dropped: 22 D8 01 (needs security access, "
        "or a session Havne doesn't enter: 7F 22 33); extended session");
    u.session = 0x01;
    EXPECT_EQ(
        SessionBridge::channelSupportText(u).toStdString(),
        "ECU 0x7E8: 1 request, 1 dropped: 22 D8 01 (needs security access "
        "or another session: 7F 22 33); default session");
}

TEST(BridgeTest, ChannelSupportTextNamesTheEcuAndWhatItDropped)
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.requests = 10;
    car.ranges = 0x7;
    car.pids_supported = 44;
    car.pids_through = 0x60;
    car.dropped = 1;
    car.dropped_list[0] = {{0x01, 0x5C, 0x00}, 2, ECUIO_DROP_BITMAP, 0, {0, 0}};
    EXPECT_EQ(SessionBridge::channelSupportText(car).toStdString(),
              "ECU 0x7E8: supports 44 PIDs up to 0x60, no answer past that; "
              "10 requests, 1 dropped: 01 5C (not in its supported PIDs)");

    ecuio_channel_support_t later = car; // a reason this build doesn't know
    later.dropped_list[0].reason = 99;
    EXPECT_EQ(SessionBridge::channelSupportText(later).toStdString(),
              "ECU 0x7E8: supports 44 PIDs up to 0x60, no answer past that; "
              "10 requests, 1 dropped: 01 5C (reason 99)");

    ecuio_channel_support_t all{};
    all.flags = ECUIO_SUPPORT_PROBED | ECUIO_SUPPORT_COMPLETE;
    all.rx_id = 0x18DAF110;
    all.requests = 1;
    all.ranges = 0x1;
    all.pids_supported = 2;
    all.pids_through = 0x20;
    EXPECT_EQ(SessionBridge::channelSupportText(all).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped");

    /* Packing (ADR-0039 Appendix A): answered, then given up */
    ecuio_channel_support_t packed = all;
    packed.flags |= ECUIO_SUPPORT_PACKED;
    EXPECT_EQ(SessionBridge::channelSupportText(packed).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped; "
              "several PIDs per request");
    packed.flags |= ECUIO_SUPPORT_UNPACKED;
    EXPECT_EQ(SessionBridge::channelSupportText(packed).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped; "
              "one PID per request, several went unanswered");

    ecuio_channel_support_t part = all;
    part.flags = ECUIO_SUPPORT_PROBED;
    part.ranges = 0;
    part.pids_supported = 0;
    part.pids_through = 0;
    EXPECT_EQ(SessionBridge::channelSupportText(part).toStdString(),
              "ECU 0x18DAF110: no answer to the supported-PID query; "
              "1 request, none dropped");

    ecuio_channel_support_t dids{};
    dids.rx_id = 0x7E9;
    dids.requests = 12;
    dids.dropped = 10;
    dids.dropped_list[0] = {{0x22, 0x1E, 0x1D}, 3, ECUIO_DROP_NOT_SUPPORTED, 0x31, {0, 0}};
    dids.dropped_list[1] = {{0x22, 0x1E, 0x1E}, 3, ECUIO_DROP_ACCESS, 0x33, {0, 0}};
    for (int i = 2; i < ECUIO_SUPPORT_MAX_DROPPED; ++i) {
        dids.dropped_list[i] = {{0x22, 0x40, static_cast<uint8_t>(i)}, 3,
                                ECUIO_DROP_SERVICE, 0x11, {0, 0}};
    }
    EXPECT_EQ(SessionBridge::channelSupportText(dids).toStdString(),
              "ECU 0x7E9: 12 requests, 10 dropped: "
              "22 1E 1D (not supported: 7F 22 31), "
              "22 1E 1E (needs security access or another session: 7F 22 33), "
              "22 40 02 (service not supported: 7F 22 11), "
              "22 40 03 (service not supported: 7F 22 11), "
              "22 40 04 (service not supported: 7F 22 11), "
              "22 40 05 (service not supported: 7F 22 11), "
              "22 40 06 (service not supported: 7F 22 11), "
              "22 40 07 (service not supported: 7F 22 11), and 2 more");

    /* A DID scan (ADR-0020): the fake's 1E18-1E20 range, answered to its
     * end; then one still running whose table filled before every found
     * DID could join the rotation. */
    ecuio_channel_support_t scan{};
    scan.flags = ECUIO_SUPPORT_SCAN_COMPLETE;
    scan.rx_id = 0x7E8;
    scan.requests = 1;
    scan.scan_total = 9;
    scan.scan_done = 9;
    scan.scan_found = 1;
    scan.scan_refused = 8;
    scan.scan_polled = 1;
    EXPECT_EQ(SessionBridge::channelSupportText(scan).toStdString(),
              "ECU 0x7E8: 1 request, none dropped; scan: 9 DIDs, 1 found, 8 refused");

    ecuio_channel_support_t running{};
    running.rx_id = 0x7E9;
    running.requests = 128;
    running.scan_total = 256;
    running.scan_done = 250;
    running.scan_found = 130;
    running.scan_refused = 120;
    running.scan_polled = 128;
    EXPECT_EQ(SessionBridge::channelSupportText(running).toStdString(),
              "ECU 0x7E9: 128 requests, none dropped; "
              "scan: 256 DIDs, 130 found (128 polled), 120 refused, 6 unanswered");
}

TEST(BridgeTest, NewSaltIsSixteenRandomBytes)
{
    SessionBridge bridge;
    const QString a = bridge.newSalt();
    const QString b = bridge.newSalt();
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$"))
                    .match(a)
                    .hasMatch())
        << a.toStdString();
    EXPECT_NE(a, b);
}

/* ADR-0027 section 2: a label as a recording folder may take it */
TEST(BridgeTest, FolderLabelKeepsWhatAFolderNameMay)
{
    const auto f = [](const char* label) {
        return SessionBridge::folderLabel(QString::fromUtf8(label))
            .toStdString();
    };
    EXPECT_EQ(f("Daily driver"), "Daily driver");
    EXPECT_EQ(f("  Weekend   car  "), "Weekend car");
    EXPECT_EQ(f("a/b:c*?<>|d"), "abcd");
    EXPECT_EQ(f("v1.2_track-day"), "v1.2_track-day");
    EXPECT_EQ(f("Car..."), "Car") << "Windows refuses a trailing dot";
    EXPECT_EQ(f("..."), "");
    EXPECT_EQ(f(std::string(60, 'x').c_str()), std::string(40, 'x'));
}

TEST(BridgeTest, ConnectLogsTheMockSession)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_GE(log.size(), 2);
    EXPECT_EQ(log.at(log.size() - 2), QStringLiteral("Connected: mock://demo"));
    EXPECT_EQ(log.last(), QStringLiteral("Session: mock transport; profile mock"));
    EXPECT_TRUE(bridge.adapterVersions().isEmpty()) << "no adapter, nothing to keep";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, LinkStateMapsEveryPublishedCode)
{
    /* The state-string mapping, pinned session-free via the static
     * helper. Covers all four link-health codes plus the forward-
     * compatible fallback; QML's Theme keys colors and labels on
     * these strings. */
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_OK), QStringLiteral("ok"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_VEHICLE_TIMEOUT), QStringLiteral("quiet"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_REQUEST_FAILED), QStringLiteral("requests"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_TRANSPORT_HARD_FAILURE), QStringLiteral("fault"));
    EXPECT_EQ(SessionBridge::linkStateForError(ECUIO_ERR_RECORDING_CHANGED), QStringLiteral("stopped"));
    EXPECT_EQ(SessionBridge::linkStateForError(static_cast<ecuio_status_t>(999)), QStringLiteral("degraded"));
}

/* LatencyStats reads a quantile back at the top of the bucket holding
 * it: exact below 16 us, at most an eighth above the true value past
 * that, never below it, and the maximum exactly */
TEST(BridgeTest, LatencyStatsReadsQuantilesWithinABucket)
{
    LatencyStats empty;
    EXPECT_EQ(empty.count(), 0u);
    EXPECT_EQ(empty.quantileUs(0.5), 0u);

    LatencyStats small;
    for (int us : {0, 3, 7, 15})
        small.add(us);
    small.add(-5);   // a clock step backwards counts as 0
    EXPECT_EQ(small.count(), 5u);
    EXPECT_EQ(small.quantileUs(0.0), 0u);
    EXPECT_EQ(small.quantileUs(0.5), 3u) << "exact below 16 µs";
    EXPECT_EQ(small.quantileUs(1.0), 15u);

    LatencyStats l;
    for (int us = 1; us <= 100000; ++us)
        l.add(us);
    EXPECT_EQ(l.count(), 100000u);
    EXPECT_EQ(l.maxUs(), 100000u);
    for (double q : {0.01, 0.25, 0.5, 0.9, 0.99, 0.999}) {
        const double truth = q * 100000.0;
        const double got = static_cast<double>(l.quantileUs(q));
        EXPECT_GE(got, truth) << q;
        EXPECT_LE(got, truth * 1.125 + 1.0) << q;
    }
    EXPECT_EQ(l.quantileUs(1.0), 100000u) << "the maximum, exactly";

    LatencyStats huge;
    huge.add(int64_t(1) << 50);   // past the last bucket: kept, and the max
    EXPECT_EQ(huge.quantileUs(0.5), uint64_t(1) << 50);
    l.clear();
    EXPECT_EQ(l.count(), 0u);
    EXPECT_EQ(l.maxUs(), 0u);
}

/* The activity log's "Delivery:" line: frames and drops, what was still
 * queued, then each path's median, p99 and max (us below a millisecond,
 * ms with a decimal below 10, whole ms past that); a session that
 * decoded nothing has no values clause */
TEST(BridgeTest, DeliveryTextNamesFramesDropsAndTimes)
{
    SessionBridge::DeliveryStats none;
    EXPECT_EQ(SessionBridge::deliveryText(none).toStdString(), "Delivery: no frames");

    SessionBridge::DeliveryStats raw;
    raw.frames = 3;
    for (int us : {12, 12, 900})
        raw.frameToGui.add(us);
    EXPECT_EQ(SessionBridge::deliveryText(raw).toStdString(),
              "Delivery: 3 frames, none dropped; to the GUI thread "
              "in median 12 µs, p99 900 µs, max 900 µs");

    SessionBridge::DeliveryStats busy;
    busy.frames = 2000;
    busy.dropped = 500;
    busy.queuedAtEnd = 7;
    for (int i = 0; i < 1493; ++i)
        busy.frameToGui.add(2500);
    busy.values = 4000;
    busy.valuesMissed = 10;
    for (int i = 0; i < 3990; ++i)
        busy.valueToModel.add(i < 3900 ? 52000 : 99000);
    /* 52 ms reads 53: its bucket's top is 53.247 ms, an eighth of
     * 32.768 ms past its floor. 99 ms reads exact, capped at the max. */
    EXPECT_EQ(SessionBridge::deliveryText(busy).toStdString(),
              "Delivery: 2000 frames, 500 dropped at the in-flight cap "
              "(25.0 %), 7 still queued at the end; to the GUI thread "
              "in median 2.5 ms, p99 2.5 ms, max 2.5 ms; 4000 values, "
              "to the model in median 53 ms, p99 99 ms, max 99 ms "
              "(10 past the staging's limit, not timed)");
}

/* The activity log's "Reply timing:" line (ADR-0042): the requests asked,
 * their outcomes, the spread of their median latencies, the intervals'
 * clock and ISO-TP's counts where core reassembles; nothing when no
 * request was asked */
TEST(BridgeTest, TimingTextNamesTheRequestsAndTheClock)
{
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(3);
    EXPECT_TRUE(SessionBridge::timingText(t, rows).isEmpty());

    t.interval_clock = ECUIO_TIMING_CLOCK_ADAPTER;
    rows[0].answered = 100;
    rows[0].latency_p50_us = 4200;
    rows[1].answered = 50;
    rows[1].timeouts = 2;
    rows[1].latency_p50_us = 12400;
    EXPECT_EQ(
        SessionBridge::timingText(t, rows).toStdString(),
        "Reply timing: 2 requests, 150 answered, none refused, 2 timed out; "
        "latency P50 4.2 ms to 12 ms; intervals on the adapter's clock");

    t.interval_clock = ECUIO_TIMING_CLOCK_KERNEL;
    t.flags = ECUIO_TIMING_ISOTP;
    t.isotp_completed = 640;
    t.isotp_first_frames = 40;
    t.isotp_dropped = 1;
    rows[1] = ecuio_request_timing_t{};
    rows[2].refused = 1;
    rows[2].latency_p50_us = 4200;
    EXPECT_EQ(
        SessionBridge::timingText(t, rows).toStdString(),
        "Reply timing: 2 requests, 100 answered, 1 refused, none timed out; "
        "latency P50 4.2 ms; intervals on the kernel's receive times; "
        "ISO-TP: 640 messages, 40 segmented, 1 dropped, none unusable");
}

/* At disconnect the activity log says how the session's frames and
 * values were delivered: over the engine-start fixture at full speed,
 * every frame accounted for (handled or dropped at the cap), each
 * handled frame and each staged value timed, and the line is the
 * figures the bridge kept */
TEST(BridgeTest, DisconnectLogsTheDelivery)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    constexpr quint64 kFrames = 7074;
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() + bridge.droppedFrames() >= int(kFrames); },
        20000));
    spinLoop(300);   // past two ticks: every value drained

    const SessionBridge::DeliveryStats s = bridge.deliveryStats();
    EXPECT_EQ(s.frames, kFrames);
    EXPECT_EQ(s.dropped + static_cast<quint64>(bridge.frameCount()), kFrames);
    EXPECT_EQ(s.queuedAtEnd, 0u);
    EXPECT_EQ(s.frameToGui.count(), static_cast<quint64>(bridge.frameCount()))
        << "every handled frame timed";
    EXPECT_GT(s.values, 0u);
    EXPECT_EQ(s.valuesMissed, 0u);
    EXPECT_EQ(s.valueToModel.count(), s.values) << "every staged value timed";

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList delivery = log.filter(QStringLiteral("Delivery: "));
    ASSERT_EQ(delivery.size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(delivery.first(), SessionBridge::deliveryText(s));
    EXPECT_TRUE(delivery.first().startsWith(QStringLiteral("Delivery: 7074 frames, ")));
    EXPECT_TRUE(delivery.first().contains(QStringLiteral("values, to the model in median ")));
}

TEST(BridgeTest, SessionInfoTextDescribesAReplay)
{
    ecuio_session_info_t info{};
    info.backend = ECUIO_BACKEND_REPLAY;
    info.replay_frames = 7194;
    info.replay_span_ms = 275224;
    info.replay_speed_milli = 1000;
    qstrncpy(info.profile, "obd2", sizeof(info.profile));
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at 1x; "
              "profile obd2");
    info.replay_speed_milli = 2500;
    info.flags = ECUIO_SESSION_INFO_REPLAY_LOOP;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "2.5x, looping; "
              "profile obd2");
    info.replay_speed_milli = 0;
    info.flags = 0;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "full speed; "
              "profile obd2");
    info.replay_rows_torn = 1;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "full speed, its last row cut short and left out; "
              "profile obd2");
}

TEST(BridgeTest, TheStaleAgeIsCores)
{
    EXPECT_EQ(SessionBridge::valueStaleMs(),
              static_cast<int>(ECUIO_VALUE_STALE_US / 1000u));
}

// ---------------------------------------------------------------------------
// The conditions (ADR-0063 section 9): the app's saved set
// (app/conditionstore.h), its rows (app/conditionmodel.h) and the bridge's
// attach, callback and tick. The mock's first 15 s are its KeyOn (0 rpm),
// so a condition true there fires at the first frame and stays ON.
// ---------------------------------------------------------------------------
namespace {

// One condition, so a set told from mockConditions by its count; of two
// terms, on two units
QByteArray otherConditions()
{
    return QByteArray(R"({"havne_conditions":1,"conditions":[)"
                      R"({"key":"second","name":"Second set","when":{"all":[)"
                      R"({"signal":["id:1001"],"unit":"RPM","below":300},)"
                      R"({"signal":["id:3001"],"unit":"km/h","below":250}]},)"
                      R"("alert":{"level":"warning"}}]})");
}

// The model's rows, each a map by role name
QVariantList conditionRows(const ConditionModel* model)
{
    QVariantList rows;
    const QHash<int, QByteArray> roles = model->roleNames();
    for (int r = 0; r < model->rowCount(); ++r) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromLatin1(it.value()),
                       model->data(model->index(r), it.key()));
        }
        rows << row;
    }
    return rows;
}

QVariantMap conditionRow(const ConditionModel* model, int r)
{
    return conditionRows(model).value(r).toMap();
}

// Whether row r reads live and in that state, within 5 s
bool rowReads(const SessionBridge& bridge, int r, const char* state)
{
    return spinUntil(
        [&]() {
            const QVariantMap row = conditionRow(bridge.conditionModel(), r);
            return row.value(QStringLiteral("live")).toBool() &&
                   row.value(QStringLiteral("state")).toString() ==
                       QLatin1String(state);
        },
        5000);
}

// An event as core's worker gives it
ecuio_condition_event_t conditionEvent(uint32_t condition, uint32_t generation,
                                       uint32_t flags, double value,
                                       uint64_t at_us = 1000000,
                                       uint32_t term = 0)
{
    ecuio_condition_event_t e{};
    e.timestamp_us = at_us;
    e.term = term;
    e.condition = condition;
    e.state = (flags & ECUIO_CONDITION_FIRED) != 0 ? ECUIO_CONDITION_ON
                                                   : ECUIO_CONDITION_OFF;
    e.flags = flags;
    e.value = value;
    e.generation = generation;
    return e;
}

// The lines of log starting with prefix
QStringList linesStarting(const QStringList& log, const QString& prefix)
{
    QStringList lines;
    for (const QString& line : log) {
        if (line.startsWith(prefix))
            lines << line;
    }
    return lines;
}

} // namespace

// A file is read by core before it's kept: one core refuses, or one too
// long, leaves the kept file as it was; the save replaces it whole, with
// nothing left beside it
TEST(BridgeTest, TheStoreRefusesAFileCoreRefuses)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    ConditionStore store;
    QString words;
    EXPECT_TRUE(store.load(&words)) << "no file is no set";
    EXPECT_EQ(store.set(), nullptr);

    QVariantMap saved = store.save(mockConditions());
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(saved.value(QStringLiteral("conditions")).toInt(), 3);
    EXPECT_EQ(saved.value(QStringLiteral("name")).toString(),
              QStringLiteral("Bridge test"));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(QDir(QFileInfo(ConditionStore::path()).absolutePath())
                  .entryList(QDir::Files),
              QStringList{QStringLiteral("conditions.json")})
        << "the save leaves only conditions.json";

    const QByteArray refused =
        R"({"havne_conditions":1,"conditions":[{"key":"a","name":"A",)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","abov":3}}]})";
    saved = store.save(refused);
    EXPECT_FALSE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(saved.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("abov")))
        << "core's words: "
        << saved.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(ecuio_condition_set_count(store.set()), 3u)
        << "the kept set stays";

    QByteArray tooLong = mockConditions();
    tooLong.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    saved = store.save(tooLong);
    EXPECT_FALSE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("words")).toString(),
              QStringLiteral("a condition file is at most 256 KiB"));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());

    ConditionStore again;
    EXPECT_TRUE(again.load(&words)) << words.toStdString();
    EXPECT_EQ(again.bytes(), mockConditions());
    EXPECT_EQ(ecuio_condition_set_count(again.set()), 3u);

    // A file core refuses, put there by hand: read as no set, with words
    {
        QFile f(ConditionStore::path());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(refused);
    }
    ConditionStore damaged;
    EXPECT_FALSE(damaged.load(&words));
    EXPECT_TRUE(words.contains(QStringLiteral("abov"))) << words.toStdString();
    EXPECT_EQ(damaged.set(), nullptr);
    EXPECT_TRUE(damaged.bytes().isEmpty());
    SessionBridge bridge;
    EXPECT_TRUE(bridge.conditionsProblem().startsWith(
        QStringLiteral("The saved conditions weren't read: ")))
        << bridge.conditionsProblem().toStdString();
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 0);
}

// The Alerts page's Import: a picked file goes through saveConditions, so
// one core refuses, or one that can't be read, leaves the saved file as it
// was; a leading BOM is skipped, and no refusal names the file's path
TEST(BridgeTest, AnImportedConditionFileReplacesTheSavedOneOrLeavesIt)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir picked;
    ASSERT_TRUE(picked.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QVariantMap kept = bridge.importConditions(writeScratch(
        picked.path(), "other.json", QByteArray("\xEF\xBB\xBF") + otherConditions()));
    ASSERT_TRUE(kept.value(QStringLiteral("ok")).toBool())
        << kept.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(kept.value(QStringLiteral("conditions")).toInt(), 1);
    EXPECT_EQ(kept.value(QStringLiteral("pending")).toString(), QString());
    EXPECT_EQ(readFile(ConditionStore::path()), otherConditions())
        << "the file replaces the saved one, its BOM left out";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(otherConditions()));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);

    const auto refused = [&](const QUrl& url, const QString& words) {
        log.clear();
        const QVariantMap r = bridge.importConditions(url);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool()) << words.toStdString();
        const QString said = r.value(QStringLiteral("words")).toString();
        EXPECT_TRUE(said.contains(words)) << said.toStdString();
        EXPECT_FALSE(said.contains(picked.path())) << said.toStdString();
        EXPECT_EQ(readFile(ConditionStore::path()), otherConditions())
            << "the saved file stays: " << words.toStdString();
        EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
        ASSERT_EQ(log.size(), 1) << words.toStdString();
        EXPECT_EQ(log.first(), QStringLiteral("Alerts not imported: ") + said);
    };
    refused(writeScratch(picked.path(), "refused.json",
                         R"({"havne_conditions":1,"conditions":[{"key":"a","name":"A",)"
                         R"("when":{"signal":["id:1001"],"unit":"RPM","abov":3}}]})"),
            QStringLiteral("abov"));
    refused(QUrl::fromLocalFile(picked.path() + QStringLiteral("/none.json")),
            QStringLiteral("Couldn't read the file"));
    refused(writeScratch(picked.path(), "latin1.json", QByteArray("{\xE9}")),
            QStringLiteral("The file isn't UTF-8 text"));
    QByteArray tooLong = otherConditions();
    tooLong.append(QByteArray(ECUIO_CONDITION_FILE_MAX_BYTES, ' '));
    refused(writeScratch(picked.path(), "long.json", tooLong),
            QStringLiteral("The file is over 256 KiB, a condition file's most"));
}

// Export writes the saved file's bytes where the user chose, and says why
// when there are none or the target takes no file; no line names a path
TEST(BridgeTest, AnExportedConditionFileIsTheSavedOne)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QUrl target = QUrl::fromLocalFile(out.path() + QStringLiteral("/c.json"));
    QVariantMap r = bridge.exportConditions(target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("There are no saved alerts to export"));
    EXPECT_FALSE(QFile::exists(target.toLocalFile()));

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    log.clear();
    r = bridge.exportConditions(target);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(target.toLocalFile()), mockConditions());
    ASSERT_EQ(log.size(), 1);
    EXPECT_EQ(log.first(), QStringLiteral("Alerts exported to a file (%1 bytes)")
                               .arg(mockConditions().size()));

    ASSERT_TRUE(QDir(out.path()).mkdir(QStringLiteral("folder")));
    log.clear();
    r = bridge.exportConditions(QUrl::fromLocalFile(out.path() + QStringLiteral("/folder")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("Couldn't write the file"));
    EXPECT_TRUE(QFileInfo(out.path() + QStringLiteral("/folder")).isDir());
    ASSERT_EQ(log.size(), 1);
    EXPECT_FALSE(log.first().contains(out.path())) << log.first().toStdString();
}

// A saved file core refused at the start is exported as it is, so it can
// be mended elsewhere
TEST(BridgeTest, AnExportOfAFileCoreRefusedIsTheFileAsItIs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QByteArray typo =
        R"({"havne_conditions":1,"conditions":[{"key":"x"}]})";
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(typo);
    }
    SessionBridge bridge;
    ASSERT_FALSE(bridge.conditionsProblem().isEmpty());
    const QUrl target = QUrl::fromLocalFile(out.path() + QStringLiteral("/c.json"));
    const QVariantMap r = bridge.exportConditions(target);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(readFile(target.toLocalFile()), typo);

    // Read again as at the start: a file core takes clears the problem
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(mockConditions());
    }
    bridge.reloadSavedConditionsForTesting();
    EXPECT_EQ(bridge.conditionsProblem(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
}

// A level typed in the display unit back in the def's: the inverse of
// displayValue in either unit system, and a unit no system converts as typed
TEST(BridgeTest, ADecodedValueIsTheDisplayValuesInverse)
{
    SessionBridge bridge;
    const QString degC = QString::fromUtf8("\xC2\xB0" "C");
    bridge.setUnitSystem(0);
    EXPECT_EQ(bridge.decodedValue(degC, 90.0), 90.0);
    EXPECT_EQ(bridge.decodedValue(QStringLiteral("kPa"), 101.5), 101.5);
    bridge.setUnitSystem(1);
    EXPECT_NEAR(bridge.decodedValue(degC, 212.0), 100.0, 1e-9);
    EXPECT_NEAR(bridge.decodedValue(degC, -40.0), -40.0, 1e-9);
    EXPECT_NEAR(bridge.decodedValue(QStringLiteral("kPa"), 14.5037737730209), 100.0, 1e-6);
    EXPECT_NEAR(bridge.decodedValue(QStringLiteral("km/h"), 62.1371192237334), 100.0, 1e-6);
    for (const double v : {-40.0, 0.0, 0.5, 98.25, 1200.0}) {
        for (const char* unit : {"\xC2\xB0" "C", "kPa", "bar", "km/h", "g/s", "L/h"}) {
            const QString u = QString::fromUtf8(unit);
            EXPECT_NEAR(bridge.decodedValue(u, bridge.displayValue(u, v)), v, 1e-9 * (1 + std::fabs(v)))
                << unit << " at " << v;
        }
    }
    EXPECT_EQ(bridge.decodedValue(QStringLiteral("RPM"), 3000.0), 3000.0);
    EXPECT_EQ(bridge.decodedValue(QString(), 7.0), 7.0);
}

// The rows come from the file, offline too; connected, the tick gives
// each its state, its value in the display unit and the defs its terms
// bind, and an alert ON names its level for its signal
TEST(BridgeTest, AConditionsEventsReachTheModelOnTheTick)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(mockConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(), QString());
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(mockConditions()));
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_EQ(model->rowCount(), 3);
    QVariantMap row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("key")).toString(),
              QStringLiteral("stopped"));
    EXPECT_FALSE(row.value(QStringLiteral("live")).toBool())
        << "no session judges it";
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(), QVariantList{-1})
        << "bound to no session's signals";

    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://conditions")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(rowReads(bridge, 1, "on"));
    row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine stopped"));
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(),
              QStringLiteral("warning"));
    EXPECT_EQ(row.value(QStringLiteral("uses")).toStringList(),
              QStringList{QStringLiteral("alert")});
    EXPECT_TRUE(row.value(QStringLiteral("banner")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("sound")).toBool());
    EXPECT_TRUE(row.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("stale")).toBool());
    EXPECT_FALSE(row.value(QStringLiteral("unavailable")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(), QString());
    EXPECT_EQ(row.value(QStringLiteral("fired")).toLongLong(), 1);
    EXPECT_EQ(row.value(QStringLiteral("value")).toDouble(), 0.0);
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_GE(row.value(QStringLiteral("sinceMs")).toLongLong(), 0);
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_MOCK_SIG_RPM_ID});

    row = conditionRow(model, 1);
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(),
              QStringLiteral("danger"));
    EXPECT_TRUE(row.value(QStringLiteral("sound")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°C"));
    const double celsius = row.value(QStringLiteral("value")).toDouble();
    EXPECT_TRUE(std::isfinite(celsius));
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    row = conditionRow(model, 1);
    EXPECT_EQ(row.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°F"));
    EXPECT_GT(row.value(QStringLiteral("value")).toDouble(), celsius)
        << "a warm engine's deg F is above its deg C";
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);

    row = conditionRow(model, 2);
    EXPECT_EQ(row.value(QStringLiteral("state")).toString(),
              QStringLiteral("off"));
    EXPECT_EQ(row.value(QStringLiteral("level")).toString(), QString());
    EXPECT_EQ(row.value(QStringLiteral("uses")).toStringList(),
              QStringList{QStringLiteral("mark")});

    const QVariantMap levels{{QString::number(ECUIO_MOCK_SIG_RPM_ID), 1},
                             {QString::number(ECUIO_MOCK_SIG_COOLANT_ID), 2}};
    EXPECT_EQ(bridge.alertLevels(), levels)
        << "a mark-only condition names no level";
    EXPECT_EQ(bridge.alertLevelOf(ECUIO_MOCK_SIG_COOLANT_ID), 2);
    EXPECT_EQ(bridge.alertLevelOf(ECUIO_MOCK_SIG_SPEED_ID), 0);

    const int connected = static_cast<int>(
        log.indexOf(QStringLiteral("Connected: mock://conditions")));
    const int judging = static_cast<int>(
        log.indexOf(QStringLiteral("Alerts: judging 3 conditions")));
    EXPECT_GT(judging, connected) << "after the session's lines";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM")))
        << log.join(u'\n').toStdString();

    ASSERT_TRUE(disconnectAndWait(bridge));
    row = conditionRow(model, 0);
    EXPECT_FALSE(row.value(QStringLiteral("live")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("state")).toString(),
              QStringLiteral("off"));
    EXPECT_TRUE(bridge.alertLevels().isEmpty());
    EXPECT_EQ(model->rowCount(), 3) << "the saved set's rows offline";
}

// A re-attach begins a new generation: an event of the set before is
// passed by, one of the set attached is the tick's
TEST(BridgeTest, AnEventOfTheSetBeforeIsIgnored)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://generations")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    // A firing of the set before, still in the window: written, not lost
    const ecuio_condition_event_t staged =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 777);
    bridge.dispatchConditionsForTesting(&staged, 1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u)
        << "no recorder: at once";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 777 RPM")))
        << "the window's firing before the re-attach: "
        << log.join(u'\n').toStdString();
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts saved: 1 condition")));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));

    const ecuio_condition_event_t old =
        conditionEvent(0, 1, ECUIO_CONDITION_FIRED, 4321);
    bridge.dispatchConditionsForTesting(&old, 1);
    const ecuio_condition_event_t now =
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 1234, 1000000, 1);
    bridge.dispatchConditionsForTesting(&now, 1);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Second set, 1234 km/h"));
        },
        5000))
        << "in the unit of the term that settled it: "
        << log.join(u'\n').toStdString();
    EXPECT_EQ(conditionRow(bridge.conditionModel(), 0)
                  .value(QStringLiteral("unit"))
                  .toString(),
              QStringLiteral("km/h"));
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set, 4321")).isEmpty())
        << "the set before's event";
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set fired")).isEmpty())
        << "the two weren't counted together";

    // An index the set attached doesn't hold
    const ecuio_condition_event_t past =
        conditionEvent(5, 2, ECUIO_CONDITION_FIRED, 99);
    bridge.dispatchConditionsForTesting(&past, 1);
    spinLoop(300);
    EXPECT_TRUE(
        linesStarting(log, QStringLiteral("Alert: Second set, 99 ")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// Each firing is a line of the activity log, and a note in the
// recording; several of one condition's in one window are one line, and
// a change that isn't a firing is none
TEST(BridgeTest, EachFiringIsAnActivityLine)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://firings")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(rowReads(bridge, 1, "on"));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM"));
        },
        5000));
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Engine stopped")).size(), 1)
        << "one firing, one line";
    EXPECT_EQ(
        linesStarting(log, QStringLiteral("Alert: Coolant cold, ")).size(), 1);

    const uint32_t g = bridge.conditionsGenerationForTesting();
    std::vector<ecuio_condition_event_t> window;
    for (int i = 0; i < 1000; ++i) {
        window.push_back(conditionEvent(0, g, ECUIO_CONDITION_FIRED, i));
        window.push_back(conditionEvent(0, g, 0, i)); // back OFF
    }
    window.push_back(conditionEvent(2, g, ECUIO_CONDITION_FIRED, 260));
    window.push_back(
        conditionEvent(1, g, ECUIO_CONDITION_STALE, 20)); // no firing
    bridge.dispatchConditionsForTesting(window.data(),
                                        static_cast<uint32_t>(window.size()));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(
                QStringLiteral("Alert: Engine stopped fired 1000 times"));
        },
        5000))
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Condition: Moving, 260 km/h")))
        << "a condition with no alert";
    EXPECT_EQ(linesStarting(log, QStringLiteral("Alert: Coolant cold")).size(),
              1)
        << "a STALE change isn't a firing";

    // In the order they fired, whatever their order in the set; a
    // condition's place is its window's first firing
    const ecuio_condition_event_t first[] = {
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 1, 2000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 2, 3000000),
    };
    bridge.dispatchConditionsForTesting(first, 2);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Alert: Engine stopped, 2 RPM"));
        },
        5000));
    EXPECT_LT(log.indexOf(QStringLiteral("Condition: Moving, 1 km/h")),
              log.indexOf(QStringLiteral("Alert: Engine stopped, 2 RPM")));
    const ecuio_condition_event_t second[] = {
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 11, 4000000),
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 12, 5000000),
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 13, 6000000),
    };
    bridge.dispatchConditionsForTesting(second, 3);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral("Condition: Moving, 12 km/h"));
        },
        5000));
    EXPECT_LT(
        log.indexOf(QStringLiteral("Alert: Engine stopped fired 2 times")),
        log.indexOf(QStringLiteral("Condition: Moving, 12 km/h")));

    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QLatin1Char('/') + folders.first();
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("Alert: Engine stopped, 0 RPM"))
        << "noted in the recording";
    EXPECT_TRUE(events.contains("Alert: Engine stopped fired 1000 times"));
    EXPECT_EQ(readFile(folder + QStringLiteral("/conditions.json")),
              mockConditions())
        << "the set the session judged";
}

// A recording holds its set (ADR-0063 section 6): an edit saved while one
// runs waits for its end, then is judged; a disconnect ends the wait too
TEST(BridgeTest, AnEditDuringARecordingWaitsForItsEnd)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://edit")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(bridge.startLog());

    QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsPending(),
              QStringLiteral("applies when this recording ends"));
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u)
        << "core would refuse it";
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(otherConditions()))
        << "saved";
    EXPECT_TRUE(
        log.contains(QStringLiteral("Alerts saved: 1 condition; the change "
                                    "applies when this recording ends")));

    bridge.stopLog();
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(readFile(root.path() + QLatin1Char('/') + folders.first() +
                       QStringLiteral("/conditions.json")),
              mockConditions())
        << "the recording kept its set";

    // A disconnect ends a recording's wait: the saved set, offline
    ASSERT_TRUE(bridge.startLog());
    saved = bridge.saveConditions(QString::fromUtf8(mockConditions()));
    EXPECT_FALSE(saved.value(QStringLiteral("pending")).toString().isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_FALSE(conditionRow(bridge.conditionModel(), 0)
                     .value(QStringLiteral("live"))
                     .toBool());
}

// A replay is judged by its recording's own conditions.json while it
// matches the header's hash (ADR-0063 decision 12); an edit then waits for
// the replay's end, and a copy changed since is passed by for the saved set
TEST(BridgeTest, AReplayJudgesByItsRecordingsCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://recorded")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 10; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList rows = bridge.recentRecordings(1);
    ASSERT_EQ(rows.size(), 1);
    const QString uri =
        rows.first().toMap().value(QStringLiteral("uri")).toString();
    const QString folder =
        rows.first().toMap().value(QStringLiteral("folder")).toString();

    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(bridge.conditionsFromRecording());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Alerts: judging the recording's own 3 conditions")))
        << log.join(u'\n').toStdString();
    ASSERT_EQ(bridge.conditionModel()->rowCount(), 3);
    EXPECT_EQ(conditionRow(bridge.conditionModel(), 1)
                  .value(QStringLiteral("key"))
                  .toString(),
              QStringLiteral("cold"));
    ASSERT_TRUE(rowReads(bridge, 0, "on")) << "the replay fires what fired";
    EXPECT_TRUE(log.contains(QStringLiteral("Alert: Engine stopped, 0 RPM")));

    const QVariantMap saved =
        bridge.saveConditions(QString::fromUtf8(otherConditions()));
    EXPECT_EQ(saved.value(QStringLiteral("pending")).toString(),
              QStringLiteral("applies when this replay is disconnected"));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_EQ(bridge.conditionsPending(), QString());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1) << "the saved set's rows";

    // A copy changed since the recording: the saved set, and a line
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    const qsizetype unmatched = log.indexOf(QStringLiteral(
        "Replay: the recording's conditions.json doesn't match its hash; "
        "judging the saved conditions"));
    EXPECT_GE(unmatched, 0) << log.join(u'\n').toStdString();
    const QStringList connected =
        linesStarting(log, QStringLiteral("Connected: "));
    ASSERT_EQ(connected.size(), 1);
    EXPECT_GT(unmatched, log.indexOf(connected.first()))
        << "with the connect's lines";
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    // With no saved set, the line says none is judged
    {
        SettingsDirScope none;
        ASSERT_TRUE(none.valid());
        SessionBridge fresh;
        fresh.setRecordingsRootForTesting(root.path());
        QStringList lines;
        QObject::connect(&fresh, &SessionBridge::logMessage,
                         [&lines](const QString& line) { lines << line; });
        ASSERT_TRUE(fresh.connectSession(uri).isEmpty());
        EXPECT_TRUE(lines.contains(QStringLiteral(
            "Replay: the recording's conditions.json doesn't match its "
            "hash; judging none")))
            << lines.join(u'\n').toStdString();
        EXPECT_TRUE(linesStarting(lines, QStringLiteral("Alerts")).isEmpty());
        EXPECT_EQ(fresh.conditionModel()->rowCount(), 0);
        ASSERT_TRUE(disconnectAndWait(fresh));
    }

    // A copy that matches but isn't judged: the saved set in its place
    {
        QFile copy(folder + QStringLiteral("/conditions.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadWrite));
        copy.resize(copy.size() - 1);
    }
    bridge.failConditionsAttachesForTesting(1);
    log.clear();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_FALSE(bridge.conditionsFromRecording());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Replay: the recording's "
                                          "conditions.json isn't judged ("))
            .isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Alerts: judging 1 condition")));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 1);
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// The level a signal's alerts give it is the highest ON among them,
// whatever their order; one OFF, or a condition with no alert, gives none
TEST(BridgeTest, TheHighestAlertOnASignalIsItsLevel)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"d","name":"D","when":{"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"("alert":{"level":"danger"}},)"
        R"({"key":"w","name":"W","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:4001"],"unit":"%","above":1}]},"alert":{"level":"warning"}},)"
        R"({"key":"off","name":"Off","when":{"signal":["id:3001"],"unit":"km/h","above":1},)"
        R"("alert":{"level":"danger"}},)"
        R"({"key":"m","name":"M","when":{"signal":["id:6001"],"unit":"%","above":1},)"
        R"("mark":true}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    EXPECT_TRUE(model.alertLevels().empty()) << "nothing judged yet";
    ecuio_condition_state_t states[4] = {};
    for (ecuio_condition_state_t& s : states) {
        s.state = ECUIO_CONDITION_ON;
    }
    states[2].state = ECUIO_CONDITION_OFF;
    model.applyStates(states, 4);
    const std::map<uint32_t, int> levels{
        {ECUIO_MOCK_SIG_RPM_ID, ECUIO_ALERT_DANGER},
        {ECUIO_MOCK_SIG_THROTTLE_ID, ECUIO_ALERT_WARNING}};
    EXPECT_EQ(model.alertLevels(), levels);
}

// A row gone stale takes its time from the term it went stale on (the
// event's, through applyTerm) plus the wall offset, judged once while that
// term reads nothing new, and reads -1 again once fresh or unjudged
TEST(BridgeTest, AStaleRowsTimeIsTakenOnceAndDroppedWhenFresh)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:3001"],"unit":"km/h","above":1}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    int notified = 0;
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.contains(ConditionModel::StaleSinceMsRole)) {
                ++notified;
            }
        });
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 200},
                                      {ECUIO_MOCK_SIG_SPEED_ID, 500}};
    int calls = 0;
    const auto read = [&](uint32_t id) {
        ++calls;
        return readAt[id];
    };
    constexpr qint64 offset = 1000000;
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_ON;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.applyTerm(0, 1);
    EXPECT_EQ(notified, 0) << "the states leave the time to the judge";
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 500)
        << "the speed it went stale on, though the RPM read longer ago";
    EXPECT_EQ(notified, 1);
    calls = 0;
    model.judgeStaleSince(read, offset + 5);
    EXPECT_EQ(calls, 1) << "only the judged def is asked again";
    EXPECT_EQ(since(), offset + 500) << "taken once while it reads nothing";
    EXPECT_EQ(notified, 1) << "nothing moved, nothing notified";

    s.flags = 0;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), -1) << "fresh again";
    EXPECT_EQ(notified, 2);
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = 900;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 900) << "a new stretch, its stale term's time";

    model.clearStates();
    EXPECT_EQ(since(), -1) << "unjudged";
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = -1;
    model.applyStates(&s, 1);
    const int unjudged = notified;
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), -1) << "its stale term has no value: no time to name";
    EXPECT_EQ(notified, unjudged) << "-1 still: nothing notified";
    readAt[ECUIO_MOCK_SIG_SPEED_ID] = 950;
    model.judgeStaleSince(read, offset);
    EXPECT_EQ(since(), offset + 200)
        << "it read since: the input read longest ago";
}

// Inputs read at different rates: a fast one stale beside a slow one still
// fresh, whose last read is older. The time is the stale input's, and the
// slow one reading on moves nothing
TEST(BridgeTest, AStaleRowsFirstTimeIsTheInputThatAgedOut)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:11001"],"unit":"kPa","above":1},)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1,"max_age":0.5}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    // At 10000: the RPM (every 100 ms) stopped 550 ms ago, the manifold
    // pressure (once a second) read 900 ms ago
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 9450},
                                      {ECUIO_MOCK_SIG_MAP_ID, 9100}};
    const auto read = [&](uint32_t id) { return readAt[id]; };
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_OFF;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.applyTerm(0, 1);
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 9450) << "the RPM aged out, not the slower pressure";
    readAt[ECUIO_MOCK_SIG_MAP_ID] = 10100;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 9450) << "the pressure reading on moves nothing";
}

// Core gives no event while a row stays stale: with both inputs of an all
// gone quiet, the first back leaves the row stale, and the time it names
// moves to the input still quiet
TEST(BridgeTest, AStaleRowsTimeFollowsTheInputStillQuiet)
{
    const QByteArray file =
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"s","name":"S","when":{"all":[)"
        R"({"signal":["id:1001"],"unit":"RPM","above":1},)"
        R"({"signal":["id:3001"],"unit":"km/h","above":1}]},)"
        R"("alert":{"level":"warning","banner":true}}]})";
    QString words;
    ConditionSetPtr set = loadConditionSet(file, &words);
    ASSERT_NE(set, nullptr) << words.toStdString();
    const ecuio_signal_set_t* mock = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("mock", &mock), ECUIO_OK);
    ConditionModel model;
    model.setConditions(set.get(), mock, nullptr);
    const auto since = [&model]() {
        return model.data(model.index(0), ConditionModel::StaleSinceMsRole)
            .toLongLong();
    };
    std::map<uint32_t, qint64> readAt{{ECUIO_MOCK_SIG_RPM_ID, 1000},
                                      {ECUIO_MOCK_SIG_SPEED_ID, 4000}};
    const auto read = [&](uint32_t id) { return readAt[id]; };
    ecuio_condition_state_t s{};
    s.state = ECUIO_CONDITION_OFF;
    s.flags = ECUIO_CONDITION_STALE;
    model.applyStates(&s, 1);
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 1000) << "the RPM went quiet first";
    readAt[ECUIO_MOCK_SIG_RPM_ID] = 9000;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 4000) << "the RPM is back; the speed is still quiet";
    readAt[ECUIO_MOCK_SIG_RPM_ID] = 9100;
    model.judgeStaleSince(read, 0);
    EXPECT_EQ(since(), 4000) << "the RPM reading on moves nothing";
}

// Through a replay whose RPM stops for two seconds while the speed goes on:
// the condition goes stale on its RPM term and names the wall time the RPM
// row last took a value (the playback's, for a replay), then -1 once the
// RPM comes back
TEST(BridgeTest, AStaleConditionKeepsItsLastReadTime)
{
    SettingsDirScope settings;
    ASSERT_TRUE(settings.valid());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int ms = 0; ms <= 4000; ms += 50) {
        const QByteArray t = QByteArray::number(qint64{ms} * 1000);
        text += t + ',' + t + ",0x7E8,0x0,41 0D 20\n";
        if (ms < 1000 || ms >= 3000) {
            text += t + ',' + t + ",0x7E8,0x0,41 0C 0F A0\n";
        }
    }
    QFile f(dir.filePath(QStringLiteral("frames.csv")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(text), text.size());
    f.close();

    SessionBridge bridge;
    const QString conditions =
        QStringLiteral(
            R"({"havne_conditions":1,"conditions":[{"key":"rpm","name":"Running",)"
            R"("when":{"all":[{"signal":["id:%1"],"unit":"km/h","above":-1},)"
            R"({"signal":["id:%2"],"unit":"RPM","above":500,"max_age":0.5}]},)"
            R"("alert":{"level":"warning","banner":true}}]})")
            .arg(ECUIO_OBD2_SIG_SPEED_ID)
            .arg(ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_TRUE(
        bridge.saveConditions(conditions).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bridge.signalModel()->lastReadMs(ECUIO_OBD2_SIG_RPM_ID), -1)
        << "no row before a session";
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    ASSERT_TRUE(
        bridge
            .connectSession(
                QStringLiteral("replay://%1?profile=obd2").arg(dir.path()))
            .isEmpty());
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    EXPECT_EQ(conditionRow(model, 0).value(QStringLiteral("staleSinceMs")),
              QVariant(qint64{-1}))
        << "fresh";

    ASSERT_TRUE(spinUntil(
        [model]() {
            return conditionRow(model, 0)
                .value(QStringLiteral("stale"))
                .toBool();
        },
        5000));
    const qint64 seen = QDateTime::currentMSecsSinceEpoch();
    const qint64 since = conditionRow(model, 0)
                             .value(QStringLiteral("staleSinceMs"))
                             .toLongLong();
    const qint64 rpmRead =
        bridge.signalModel()->lastReadMs(ECUIO_OBD2_SIG_RPM_ID);
    EXPECT_LE(std::abs(since - rpmRead), 20)
        << "the RPM row's last value, not the speed's";
    EXPECT_GE(since, before + 800) << "the RPM ran for a second";
    EXPECT_LE(since, seen - 250) << "its age bound passed before it was stale";

    ASSERT_TRUE(spinUntil(
        [model]() {
            return !conditionRow(model, 0)
                        .value(QStringLiteral("stale"))
                        .toBool();
        },
        5000));
    EXPECT_EQ(conditionRow(model, 0).value(QStringLiteral("staleSinceMs")),
              QVariant(qint64{-1}))
        << "fresh again once the RPM is back";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A term on a signal the session reports it doesn't get reads
// unavailable, in the app's words; so does one bound to a def in
// another unit
TEST(BridgeTest, AConditionOnASignalTheMockDisclaimsReadsNotReceived)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge
            .saveConditions(QStringLiteral(
                R"({"havne_conditions":1,"conditions":[)"
                R"({"key":"fuel","name":"Fuel pressure",)"
                R"("when":{"signal":["obd2.pid_0a"],"unit":"kPa","above":1}},)"
                R"({"key":"unit","name":"Engine speed in Hz",)"
                R"("when":{"signal":["obd2.rpm"],"unit":"Hz","above":1}}]})"))
            .value(QStringLiteral("ok"))
            .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://disclaims?profile=obd2"))
            .isEmpty());
    const ConditionModel* model = bridge.conditionModel();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return conditionRow(model, 0)
                .value(QStringLiteral("unavailable"))
                .toBool();
        },
        5000));
    QVariantMap row = conditionRow(model, 0);
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(),
              QStringLiteral("the ECU doesn't send its signal"));
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_OBD2_PID_SIG_ID(0x0A, 0)});
    row = conditionRow(model, 1);
    EXPECT_TRUE(row.value(QStringLiteral("unavailable")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("why")).toString(),
              QStringLiteral("its unit isn't its signal's"));
    EXPECT_EQ(row.value(QStringLiteral("defIds")).toList(),
              QVariantList{ECUIO_OBD2_SIG_RPM_ID})
        << "a term bound to a def it can't judge still names it";
    EXPECT_EQ(ConditionModel::whyText(ECUIO_CONDITION_WHY_NONE), QString());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// The worker's callback writes a slot made at the attach: nothing is
// allocated per event, however many come (counted on this thread through
// the same call core's worker makes)
TEST(BridgeTest, TheBridgeAllocatesNothingPerConditionEvent)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://news")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    const uint32_t g = bridge.conditionsGenerationForTesting();
    ecuio_condition_event_t events[3] = {
        conditionEvent(0, g, ECUIO_CONDITION_FIRED, 1),
        conditionEvent(1, g, 0, 2),
        conditionEvent(2, g, ECUIO_CONDITION_FIRED, 3),
    };
    bridge.dispatchConditionsForTesting(events, 3);
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        bridge.dispatchConditionsForTesting(events, 3);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    // Each landed: no tick ran between them, and the mock never moves
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(
                QStringLiteral("Condition: Moving fired 10001 times"));
        },
        5000))
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

// Three alerts on an engine speed the mock never reaches, so only the
// events a case dispatches fire: two that sound, at each level, and one
// that doesn't
QByteArray toneConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}},)"
        R"({"key":"warm","name":"Warm","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":19000},)"
        R"("alert":{"level":"warning","sound":true}},)"
        R"({"key":"quiet","name":"Quiet","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":18000},)"
        R"("alert":{"level":"danger","banner":true}}]})");
}

// A session of mock://demo judging toneConditions, its log kept
struct ToneRun {
    SettingsDirScope dir;
    SessionBridge bridge;
    QStringList log;
    uint32_t generation = 0;

    bool start()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!dir.valid() ||
            !bridge.saveConditions(QString::fromUtf8(toneConditions()))
                 .value(QStringLiteral("ok"))
                 .toBool() ||
            !bridge.connectSession(QStringLiteral("mock://demo")).isEmpty() ||
            !rowReads(bridge, 0, "off")) {
            return false;
        }
        generation = bridge.conditionsGenerationForTesting();
        return true;
    }
    // Each condition fired in one window, then the tick that writes it
    bool fire(std::initializer_list<uint32_t> conditions)
    {
        std::vector<ecuio_condition_event_t> events;
        for (uint32_t c : conditions) {
            events.push_back(
                conditionEvent(c, generation, ECUIO_CONDITION_FIRED, 20001));
        }
        const qsizetype before =
            linesStarting(log, QStringLiteral("Alert: ")).size();
        bridge.dispatchConditionsForTesting(
            events.data(), static_cast<uint32_t>(events.size()));
        const auto want = before + static_cast<qsizetype>(events.size());
        return spinUntil(
            [&]() {
                return linesStarting(log, QStringLiteral("Alert: ")).size() >=
                       want;
            },
            5000);
    }
    uint64_t danger() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_DANGER);
    }
    uint64_t warning() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_WARNING);
    }
};

} // namespace

// A firing whose alert sounds plays one tone; another of the same
// condition within 5 s plays none, and one after it plays again
TEST(BridgeTest, ASoundingFiringPlaysOnceInFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 0u) << "nothing fired yet";

    ASSERT_TRUE(run.fire({0})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    EXPECT_EQ(run.warning(), 0u);

    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 1u) << "a second firing at once";
    // 3 s added leaves the ticks' own time 2 s of margin
    run.bridge.addToneTimeForTesting(3000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 1u) << "within 5 s of the tone";

    run.bridge.addToneTimeForTesting(2000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 2u) << "5 s after the tone";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Each condition keeps its own 5 s, and a level its own tone. Firings in
// one tick are one tone, the loudest, and each of them waits its 5 s
TEST(BridgeTest, EachConditionSoundsItsOwnLevel)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0}));
    ASSERT_TRUE(run.fire({1}));
    EXPECT_EQ(run.danger(), 1u);
    EXPECT_EQ(run.warning(), 1u) << "the second condition's own tone";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(run.fire({1, 0}));
    EXPECT_EQ(run.danger(), 2u) << "one tick: the danger tone";
    EXPECT_EQ(run.warning(), 1u) << "one tick: one tone";
    ASSERT_TRUE(run.fire({1}));
    EXPECT_EQ(run.warning(), 1u) << "the warning sounded with the danger";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An alert without "sound" plays nothing when it fires
TEST(BridgeTest, AnAlertWithoutSoundPlaysNone)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({2})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 0u);
    EXPECT_EQ(run.warning(), 0u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// Many firings of a sounding alert in one window are one tone, and the
// worker's events allocate nothing; the tick's lines are 7a's
TEST(BridgeTest, AWindowsFiringsAreOneToneAndItsEventsAllocateNothing)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    const ecuio_condition_event_t event =
        conditionEvent(0, run.generation, ECUIO_CONDITION_FIRED, 20001);
    run.bridge.dispatchConditionsForTesting(&event, 1);
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 9999; ++i) {
        run.bridge.dispatchConditionsForTesting(&event, 1);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    EXPECT_TRUE(spinUntil(
        [&run]() {
            return run.log.contains(
                QStringLiteral("Alert: Hot fired 10000 times"));
        },
        5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An edit saved mid-session keeps the 5 s of each condition whose key it
// keeps, wherever the condition now sits, and its new ones sound
TEST(BridgeTest, AnEditKeepsEachConditionsFiveSecondsByItsKey)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0}));
    ASSERT_EQ(run.danger(), 1u);

    // A condition before Hot and one past the old set's end, both new
    QByteArray grown = toneConditions();
    grown.replace(
        R"("conditions":[)",
        R"("conditions":[{"key":"first","name":"First","when":{"signal":)"
        R"(["id:1001"],"unit":"RPM","above":21000},"alert":)"
        R"({"level":"warning","sound":true}},)");
    grown.replace(
        R"(]})", R"(,{"key":"last","name":"Last","when":{"signal":["id:1001"],)"
                 R"("unit":"RPM","above":22000},"alert":)"
                 R"({"level":"danger","sound":true}}]})");
    const QVariantMap saved =
        run.bridge.saveConditions(QString::fromUtf8(grown));
    ASSERT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(run.bridge.conditionModel()->rowCount(), 5);
    run.generation = run.bridge.conditionsGenerationForTesting();

    ASSERT_TRUE(run.fire({1})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, now second, within its 5 s";
    ASSERT_TRUE(run.fire({4}));
    EXPECT_EQ(run.danger(), 2u) << "the new last condition sounds";
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.warning(), 1u) << "the new first condition sounds";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

namespace {

// A tone maker standing in for Android's: each call recorded in a state
// the case keeps after the player is gone
struct FakeTones : AlertTone::Backend {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>>
            starts;
        int makes = 0;
        int releases = 0;
        std::vector<std::chrono::steady_clock::time_point> makeTimes;
        std::vector<std::chrono::steady_clock::time_point> releaseTimes;
        int failMakes = 0;    // the next n makes fail
        int failStarts = 0;   // the next n starts fail
        int blockFirstMs = 0; // the first start takes this long
    };
    std::shared_ptr<State> s = std::make_shared<State>();

    bool make() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->makes;
        s->makeTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
        if (s->failMakes > 0) {
            --s->failMakes;
            return false;
        }
        return true;
    }
    bool start(uint32_t level) override
    {
        int block = 0;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            std::swap(block, s->blockFirstMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(block));
        std::lock_guard<std::mutex> lock(s->mutex);
        s->changed.notify_all();
        if (s->failStarts > 0) {
            --s->failStarts;
            return false;
        }
        s->starts.emplace_back(level, std::chrono::steady_clock::now());
        return true;
    }
    void release() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->releases;
        s->releaseTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
    }
};

// Waits up to ms for pred over the state, under its lock
template <typename Pred> bool toneState(FakeTones::State& s, int ms, Pred pred)
{
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.changed.wait_for(lock, std::chrono::milliseconds(ms),
                              [&]() { return pred(s); });
}

} // namespace

// A tone plays out before the next starts, and the asks meanwhile are one
// tone, the loudest: a warning then a danger is a beep, then two beeps
TEST(BridgeTest, ATonePlaysOutAndAsksMeanwhileMergeIntoTheLoudest)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    {
        AlertTone tone(std::move(fake), {1000, 3000, 5000});
        tone.play(ECUIO_ALERT_WARNING);
        ASSERT_TRUE(toneState(*s, 2000, [](FakeTones::State& t) {
            return t.starts.size() == 1;
        }));
        tone.play(ECUIO_ALERT_WARNING);
        tone.play(ECUIO_ALERT_DANGER);
        tone.play(ECUIO_ALERT_WARNING);
        ASSERT_TRUE(toneState(*s, 4000, [](FakeTones::State& t) {
            return t.starts.size() == 2;
        }));
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            EXPECT_EQ(s->starts[0].first, ECUIO_ALERT_WARNING);
            EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
            EXPECT_GE(s->starts[1].second - s->starts[0].second,
                      std::chrono::milliseconds(1000))
                << "the first tone played out";
            EXPECT_EQ(s->makes, 1);
        }
        // Nothing was left waiting: no third tone after the second's room
        EXPECT_FALSE(toneState(*s, 1300, [](FakeTones::State& t) {
            return t.starts.size() > 2;
        })) << "the three asks were one tone";
    }
    EXPECT_EQ(s->releases, 1) << "released as the player ends";
}

// A generator that can't be made, or fails, is made again on a later ask
// once the retry interval has passed, and not before
TEST(BridgeTest, AFailedToneGeneratorIsMadeAgainAfterAPause)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->failMakes = 1;
    AlertTone tone(std::move(fake), {50, 5000, 400});
    // Asks every 50 ms until the backend sees n makes
    const auto askUntilMade = [&](uint32_t level, int n) {
        for (int i = 0; i < 100; ++i) {
            tone.play(level);
            if (toneState(*s, 50,
                          [n](FakeTones::State& t) { return t.makes >= n; })) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_WARNING, 1));
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_WARNING, 2));
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 1; }));
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        EXPECT_GE(s->makeTimes[1] - s->makeTimes[0],
                  std::chrono::milliseconds(400))
            << "made again within the interval, though asked meanwhile";
        s->failStarts = 1;
    }

    // A start that fails releases the generator; a later ask makes another
    ASSERT_TRUE(askUntilMade(ECUIO_ALERT_DANGER, 3));
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 2; }));
    std::lock_guard<std::mutex> lock(s->mutex);
    ASSERT_EQ(s->releaseTimes.size(), 1u);
    EXPECT_GE(s->makeTimes[2] - s->releaseTimes[0],
              std::chrono::milliseconds(400))
        << "made again within the interval after the failed start";
    EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
}

// An ask still waiting past the stale bound, behind a start that blocked,
// plays nothing; a fresh one plays
TEST(BridgeTest, AToneAskedLongBeforeItsTurnPlaysNone)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->blockFirstMs = 700;
    AlertTone tone(std::move(fake), {50, 300, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(
        toneState(*s, 2000, [](FakeTones::State& t) { return t.makes == 1; }));
    tone.play(ECUIO_ALERT_DANGER); // waits behind the blocked start
    ASSERT_TRUE(
        spinUntil([&tone]() { return tone.staleForTesting() == 1; }, 5000))
        << "the late danger wasn't dropped";
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ASSERT_EQ(s->starts.size(), 1u) << "the late danger played";
        EXPECT_EQ(s->starts[0].first, ECUIO_ALERT_WARNING);
    }
    tone.play(ECUIO_ALERT_DANGER);
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 2; }));
}

// An ask of a level already waiting brings its time up to date: behind a
// start that blocked, the newer danger plays though the older is stale
TEST(BridgeTest, AFreshAskBehindABlockedStartPlaysThoughOneOfItsLevelWaited)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    s->blockFirstMs = 1500;
    AlertTone tone(std::move(fake), {50, 1000, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(
        toneState(*s, 2000, [](FakeTones::State& t) { return t.makes == 1; }));
    tone.play(ECUIO_ALERT_DANGER);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    tone.play(ECUIO_ALERT_DANGER); // the older is about to be stale
    ASSERT_TRUE(toneState(*s, 5000, [](FakeTones::State& t) {
        return t.starts.size() == 2;
    })) << "the fresh danger was dropped with the stale one";
    std::lock_guard<std::mutex> lock(s->mutex);
    EXPECT_EQ(s->starts[1].first, ECUIO_ALERT_DANGER);
}

// play hands a tone to a running player with no allocation, and only the
// two levels are tones
TEST(BridgeTest, PlayAllocatesNothingAndOnlyTheTwoLevelsAreTones)
{
    auto fake = std::make_unique<FakeTones>();
    const std::shared_ptr<FakeTones::State> s = fake->s;
    AlertTone tone(std::move(fake), {1, 1000, 5000});
    tone.play(ECUIO_ALERT_WARNING);
    ASSERT_TRUE(toneState(
        *s, 2000, [](FakeTones::State& t) { return t.starts.size() == 1; }));
    const unsigned long before = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        tone.play(i % 2 == 0 ? ECUIO_ALERT_DANGER : ECUIO_ALERT_WARNING);
    }
    EXPECT_EQ(new_counter::on_this_thread() - before, 0ul);
    tone.play(ECUIO_ALERT_NONE);
    tone.play(7);
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_DANGER), 5000u)
        << "a level past danger plays nothing";
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_WARNING), 5001u);
    EXPECT_EQ(tone.playedForTesting(ECUIO_ALERT_NONE), 0u);
}

// An attach core refuses leaves the slots as they were: the set still
// judged keeps its events
TEST(BridgeTest, ARefusedAttachKeepsTheSlotsOfTheSetJudged)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://refused")).isEmpty());
    ASSERT_TRUE(rowReads(bridge, 0, "on"));
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 1u);

    bridge.failConditionsAttachesForTesting(1);
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Alerts not judged: ")).isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";

    // The third slot of generation 1: kept at its size and generation
    const ecuio_condition_event_t third =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 7);
    bridge.dispatchConditionsForTesting(&third, 1);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Condition: Moving, 7 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

// A saved file core refuses at the start is kept for its editor: its text
// is the conditions' text until a save that core takes replaces it
TEST(BridgeTest, AFileCoreRefusedAtTheStartKeepsItsText)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray typo =
        R"({"havne_conditions":1,"conditions":[{"key":"x"}]})";
    {
        QFile file(ConditionStore::path());
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(typo);
    }
    SessionBridge bridge;
    EXPECT_TRUE(bridge.conditionsProblem().startsWith(
        QStringLiteral("The saved conditions weren't read: ")))
        << bridge.conditionsProblem().toStdString();
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(typo));
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 0);
    EXPECT_EQ(readFile(ConditionStore::path()), typo)
        << "the file left as it was";

    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(mockConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_EQ(bridge.conditionsProblem(), QString());
    EXPECT_EQ(bridge.conditionsText(), QString::fromUtf8(mockConditions()));
    EXPECT_EQ(readFile(ConditionStore::path()), mockConditions());
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3);
}
