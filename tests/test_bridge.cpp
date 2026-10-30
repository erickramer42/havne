// File/layer: tests/test_bridge.cpp | GTest + QtCore, app-layer tests
// Testing: UNIT/HARNESS: the bridge and its models against the real mock
//          worker (and, on Windows, the fake J2534 DLL): marshaling,
//          coalescing, backpressure, the frame log's ring and order, the
//          signal model's roles and freshness, link states, the activity
//          log's lines, connect and disconnect threads, both decode
//          paths over the vehicle fixture, recording, the stream
//          preparer's refusal (ADR-0028), the BLE service picker

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

#include "blepicker.h"
#include "btconnect.h"
#include "carryover.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/serial.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fake_control.h"   // the fake J2534 DLL's control page
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
 * 50ms cadence is a LOCAL expectation, not a CI-runner guarantee:
 * a starved macOS VM can deliver 2 frames in 200+ ms of wall clock.
 * Polling keeps the asserted CONTRACT (the
 * condition eventually holds on a healthy stream) and drops the
 * coincidence (any particular wall-clock duration). The event loop
 * must keep turning while polling: worker lambdas drain through it. */
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
bool disconnectAndWait(SessionBridge& bridge, int timeout_ms = 5000)
{
    bridge.disconnectSession();
    return spinUntil([&bridge]() { return !bridge.disconnecting(); }, timeout_ms, 10);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* Lines without their line ends (the recorder writes text mode, CRLF
 * on Windows). */
QList<QByteArray> fileLines(const QString& path)
{
    QList<QByteArray> lines;
    for (QByteArray line : readFile(path).split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) lines << line;
    }
    return lines;
}

QStringList recordingFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/* Restores an environment variable as it was, set or not. */
struct EnvRestore {
    const char* name;
    bool had;
    QByteArray value;
    explicit EnvRestore(const char* n)
        : name(n), had(qEnvironmentVariableIsSet(n)), value(qgetenv(n)) {}
    ~EnvRestore() { had ? qputenv(name, value) : qunsetenv(name); }
    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;
};

/* What the surrogate child would inherit: the OS environment block, not
 * just the C runtime's copy. */
QByteArray osEnv(const char* name)
{
#ifdef _WIN32
    char buf[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? QByteArray(buf, static_cast<qsizetype>(n)) : QByteArray();
#else
    return qgetenv(name);
#endif
}

/* The fake ECU's diagnostics as the activity log words them (ADR-0037):
 * the fake J2534 DLL's and the mock's alike (core/src/mock_ecu.h) */
const char* const kFakeEcuDiagnosticsLine =
    "ECU 0x7E8 diagnostics: MIL on; stored P0301 P0420 U0100; pending P0171 "
    "C0035 B1001; permanent P0420; readiness 8 of 8 complete; freeze frame "
    "for P0301";

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // ONE instance, lives for the process
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

namespace {

BleCharacteristic bleChar(const QString& uuid, bool write, bool noResponse,
                          bool notify, bool indicate = false)
{
    BleCharacteristic c;
    c.uuid = uuid;
    c.write = write;
    c.writeNoResponse = noResponse;
    c.notify = notify;
    c.indicate = indicate;
    return c;
}

BleService bleService(const QString& uuid, std::vector<BleCharacteristic> chars)
{
    BleService s;
    s.uuid = uuid;
    s.characteristics = std::move(chars);
    return s;
}

// What the generic access service and a device-information service look
// like: never the adapter's text
std::vector<BleService> bleStandardServices()
{
    return {bleService("1800", {bleChar("2a00", false, false, false)}),
            bleService("180a", {bleChar("2a29", false, false, false)})};
}

} // namespace

TEST(BridgeTest, BlePickerExpandsShortUuids)
{
    EXPECT_EQ(bleUuid("FFF0"),
              QStringLiteral("0000fff0-0000-1000-8000-00805f9b34fb"));
    EXPECT_EQ(bleUuid("{6E400001-B5A3-F393-E0A9-E50E24DCCA9E}"),
              QStringLiteral("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
}

// ADR-0028 §2's order: the Nordic UART service first, then FFF0, then
// FFE0, whatever order the adapter lists them in
TEST(BridgeTest, BlePickerTakesTheListedServicesInOrder)
{
    std::vector<BleService> services = bleStandardServices();
    services.push_back(
        bleService("ffe0", {bleChar("ffe1", false, true, true)}));
    services.push_back(
        bleService("fff0", {bleChar("fff1", false, false, true),
                            bleChar("fff2", true, false, false)}));
    services.push_back(bleService(
        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
        {bleChar("6e400003-b5a3-f393-e0a9-e50e24dcca9e", false, false, true),
         bleChar("6e400002-b5a3-f393-e0a9-e50e24dcca9e", true, true, false)}));

    BlePick pick = pickBleService(services);
    EXPECT_EQ(pick.service, 4) << "the Nordic UART service";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);

    services.pop_back();
    pick = pickBleService(services);
    EXPECT_EQ(pick.service, 3) << "FFF0";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);

    services.pop_back();
    pick = pickBleService(services);
    EXPECT_EQ(pick.service, 2) << "FFE0, one characteristic both ways";
    EXPECT_EQ(pick.write, 0);
    EXPECT_EQ(pick.notify, 0);
}

// A listed service that can't carry the text is passed over for the next
// rule; the last rule takes the first service that can write and notify,
// indications counting as notifications
TEST(BridgeTest, BlePickerPassesOverAServiceMissingItsParts)
{
    std::vector<BleService> services = bleStandardServices();
    services.push_back(
        bleService("fff0", {bleChar("fff1", false, false, true)})); // no FFF2
    services.push_back(bleService(
        "ffe0", {bleChar("ffe1", false, false, true)})); // can't write
    services.push_back(bleService(
        "0000abcd-0000-1000-8000-00805f9b34fb",
        {bleChar("0000abce-0000-1000-8000-00805f9b34fb", false, false, false,
                 true),
         bleChar("0000abcf-0000-1000-8000-00805f9b34fb", false, true, false)}));
    const BlePick pick = pickBleService(services);
    EXPECT_EQ(pick.service, 4) << "the first that writes and notifies";
    EXPECT_EQ(pick.write, 1);
    EXPECT_EQ(pick.notify, 0);
}

TEST(BridgeTest, BlePickerFindsNothingInAnAdapterWithoutAWay)
{
    EXPECT_FALSE(pickBleService({}).found());
    EXPECT_FALSE(pickBleService(bleStandardServices()).found());
}

using BtAction = BtConnectRules::Action;

// The plain path: open, connect, the link up; close takes it down
TEST(BridgeTest, BtConnectOpensAndClosesALink)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.connected(), BtAction::ReportOpened);
    EXPECT_TRUE(r.up());
    EXPECT_EQ(r.close(), BtAction::CloseSocket);
    EXPECT_FALSE(r.up());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.connectFailed(), BtAction::ReportFailed) << "core hears why";
    EXPECT_FALSE(r.connecting());
    EXPECT_FALSE(r.wanted());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.connected(), BtAction::ReportOpened);
    EXPECT_EQ(r.lost(), BtAction::ReportFailed) << "a dropped link";
    EXPECT_EQ(r.lost(), BtAction::None) << "told once";
    EXPECT_EQ(r.close(), BtAction::None) << "core's close after the failure";
}

// The car run (docs/bench-sessions/2026-10-28-android-8.md): core gave up
// on an open while its connect still ran, and the next open started a
// second connect beside it. Now the next open adopts the running one
TEST(BridgeTest, BtConnectAnOpenAdoptsTheRunningConnect)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None) << "the connect is left running";
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.open(), BtAction::None) << "no second connect";
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::None) << "still one connect";
    EXPECT_EQ(r.connected(), BtAction::ReportOpened) << "the adopted one opens";
    EXPECT_TRUE(r.up());
}

// An abandoned connect that succeeds with no open waiting is closed, and
// one that fails is dropped quietly: core gave up on it already
TEST(BridgeTest, BtConnectAnAbandonedConnectEndsQuietly)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.connected(), BtAction::CloseSocket);
    EXPECT_FALSE(r.up());
    EXPECT_FALSE(r.connecting());

    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::Connect) << "nothing running: a fresh one";
}

// An adopted connect that fails began for an earlier open: the waiting
// open gets a connect of its own, and only its failure reaches core
TEST(BridgeTest, BtConnectAFailedAdoptedConnectStartsAFreshOne)
{
    BtConnectRules r;
    EXPECT_EQ(r.open(), BtAction::Connect);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_EQ(r.open(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::Connect);
    EXPECT_TRUE(r.connecting());
    EXPECT_EQ(r.connectFailed(), BtAction::ReportFailed);
    EXPECT_FALSE(r.wanted());
}

// Results with no connect running are stale: nothing happens
TEST(BridgeTest, BtConnectIgnoresResultsWithNoConnectRunning)
{
    BtConnectRules r;
    EXPECT_EQ(r.connected(), BtAction::None);
    EXPECT_EQ(r.connectFailed(), BtAction::None);
    EXPECT_EQ(r.lost(), BtAction::None);
    EXPECT_EQ(r.close(), BtAction::None);
    EXPECT_FALSE(r.up());
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

/* Recording is off by default: a session writes nothing. */
TEST(BridgeTest, RecordingIsOffByDefault)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    EXPECT_FALSE(bridge.recording());

    ASSERT_TRUE(bridge.connectSession("mock://off").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());
    EXPECT_TRUE(bridge.recordingFolder().isEmpty());
}

/* One folder per connect: every frame the bridge received, the log's
 * Pause notwithstanding, each decoded value, and the session's activity
 * lines from Connected through Disconnected. */
TEST(BridgeTest, RecordingWritesFramesValuesAndEvents)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.recording());

    ASSERT_TRUE(bridge.connectSession("mock://rec").isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(spinUntil([&bridge]() {
        return bridge.frameCount() > 3 && bridge.signalBatchCount() > 0;
    }, 5000));
    bridge.setLogPaused(true);
    const int before = bridge.frameCount();
    ASSERT_TRUE(spinUntil([&bridge, before]() { return bridge.frameCount() > before + 2; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.recordingActive());
    const int frames = bridge.frameCount();

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    EXPECT_TRUE(bridge.recordingFolder().endsWith(folders.first())) << bridge.recordingFolder().toStdString();
    EXPECT_EQ(bridge.recordingFolderUrl(), QUrl::fromLocalFile(dir));

    // frames.csv: the header row, then one row per frame, host time
    // never going backwards
    const QList<QByteArray> frameRows = fileLines(dir + QStringLiteral("/frames.csv"));
    ASSERT_FALSE(frameRows.isEmpty());
    EXPECT_EQ(frameRows.first(), QByteArray("host_us,timestamp_us,id,flags,data"));
    EXPECT_EQ(frameRows.size() - 1, frames) << "every frame, paused or not";
    qint64 lastHost = -1;
    bool sawTelemetry = false;
    for (qsizetype i = 1; i < frameRows.size(); ++i) {
        const QList<QByteArray> cols = frameRows[i].split(',');
        ASSERT_EQ(cols.size(), 5) << frameRows[i].toStdString();
        const qint64 host = cols[0].toLongLong();
        EXPECT_GE(host, lastHost) << frameRows[i].toStdString();
        lastHost = host;
        sawTelemetry = sawTelemetry || cols[2] == "0xC0";
    }
    EXPECT_TRUE(sawTelemetry) << "no 0x0C0 telemetry frame recorded";

    // signals.csv: a byte-order mark, the header row, decoded values
    const QByteArray signalsCsv = readFile(dir + QStringLiteral("/signals.csv"));
    EXPECT_TRUE(signalsCsv.startsWith("\xEF\xBB\xBFhost_us,signal,value,unit"));
    EXPECT_TRUE(signalsCsv.contains(",RPM,")) << "no RPM value recorded";

    const QByteArray events = readFile(dir + QStringLiteral("/events.log"));
    for (const char* expected : { "uri      mock://rec", "signals  ", "Connected: mock://rec",
                                  "Disconnecting: mock://rec", "Delivery: ",
                                  "Disconnected: mock://rec",
                                  "end      disconnected: " }) {
        EXPECT_TRUE(events.contains(expected)) << expected << "\n" << events.toStdString();
    }
}

/* An app closed while connected: the bridge's destructor destroys the
 * session itself and ends the recording, with no disconnect first. */
TEST(BridgeTest, DestroyedWhileRecordingClosesSessionAndRecording)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    {
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(root.path());
        bridge.setRecording(true);
        ASSERT_TRUE(bridge.connectSession("mock://closing").isEmpty());
        ASSERT_TRUE(
            spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
        ASSERT_TRUE(bridge.recordingActive());
    }

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QStringLiteral("/") + folders.first() +
                 QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("end      the app closed: "))
        << events.toStdString();
    EXPECT_FALSE(events.contains("Disconnecting:")) << events.toStdString();
}

/* A recording that can't start (the root is a file, so no folder can
 * be made under it) says why in the activity log and leaves the
 * session running unrecorded. */
TEST(BridgeTest, RecordingThatCantStartIsLoggedAndTheSessionRuns)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(file);
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    ASSERT_TRUE(bridge.connectSession("mock://unrecorded").isEmpty());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(
        log.filter(QStringLiteral("Recording failed: can't create ")).size() ==
        1)
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The switch holds while a session is up, so a recording covers a whole
 * session; each connect gets its own folder. */
TEST(BridgeTest, RecordingSwitchHoldsDuringASession)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());

    ASSERT_TRUE(bridge.connectSession("mock://late").isEmpty());
    bridge.setRecording(true);
    EXPECT_FALSE(bridge.recording());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.recording());
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(bridge.connectSession("mock://twice").isEmpty());
        bridge.setRecording(false);
        EXPECT_TRUE(bridge.recording());
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
    EXPECT_EQ(recordingFolders(root.path()).size(), 2);
}

/* Recording turns on the surrogate's frame logging for the spawns it
 * records, in the OS environment the child inherits, and takes it off
 * again at the next connect without recording. A value set at launch
 * stays. */
TEST(BridgeTest, RecordingTurnsOnSurrogateFrameLogging)
{
    const EnvRestore restore("ECUIO_SUR_LOG_FRAMES");
    qunsetenv("ECUIO_SUR_LOG_FRAMES");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_EQ(osEnv("ECUIO_SUR_LOG_FRAMES"), QByteArray("1"));
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_TRUE(readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/events.log"))
                    .contains("surrogate frame logging  on, set by this recording"));

    bridge.setRecording(false);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_TRUE(osEnv("ECUIO_SUR_LOG_FRAMES").isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    qputenv("ECUIO_SUR_LOG_FRAMES", "1");   // set at launch
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    ASSERT_TRUE(bridge.connectSession("mock://env").isEmpty());
    EXPECT_EQ(osEnv("ECUIO_SUR_LOG_FRAMES"), QByteArray("1"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The frame log belongs to a session: rows stay readable after a
 * disconnect, and the next connect starts an empty table. Checked as
 * the synchronous connect returns, before any queued frame can land. */
TEST(BridgeTest, ConnectStartsAnEmptyFrameLog)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    FrameListModel* proxy = bridge.model();
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
    SessionBridge bridge;
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

    FrameListModel* proxy = bridge.model();
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

    // A row answers only its roles, and only rows that exist answer
    EXPECT_FALSE(proxy->data(proxy->index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(proxy
                     ->data(proxy->index(proxy->rowCount(), 0),
                            FrameListModel::ArbitrationIdRole)
                     .isValid());
    EXPECT_EQ(proxy->count(), proxy->rowCount());
    EXPECT_EQ(bridge.maxFrameCount(), FrameListModel::MaxFrames);

    bridge.disconnectSession();
}

TEST(BridgeTest, FrameLogOrdersNewestFirstByDefault)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-default").isEmpty());

    FrameListModel* proxy = bridge.model();
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

TEST(BridgeTest, FrameLogTogglesToOldestFirst)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-toggle").isEmpty());

    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000));

    proxy->setNewestFirst(false);
    EXPECT_FALSE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_LT(first, last);

    // Toggle back: must re-sort to descending without reconnect
    proxy->setNewestFirst(true);

    // Capture BOTH ends fresh
    const quint64 firstAgain = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 lastAgain = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();

    // Top is the newest we've ever seen (>= because the stream kept ticking)
    EXPECT_GE(firstAgain, last);
    // Bottom is the oldest: at most the original ascending-order top,
    // and strictly older than the top of the list
    EXPECT_LE(lastAgain, first);      // 'first' = oldest captured earlier
    EXPECT_GT(firstAgain, lastAgain); // reversal actually happened

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

TEST(BridgeTest, AWindowShowsItsNewestAndChartsItsEnvelope)
{
    /* Three frames' values inside one coalesce window, as core's worker
     * hands them to onValues, at 1, 2 and 3 ms of session time: 20, then
     * a peak of 30, then 10. The row shows the newest, numeric and text
     * alike, and the window's highest and lowest beside it; the chart
     * gets those at their own times, the peak included (ADR-0005's
     * appendix). The mock's own
     * frames (0x0C0) match neither def. */
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].message_id = 0x123u;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Byte", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_HEX;
    defs[1].message_id = 0x123u;
    defs[1].byte_length = 1;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "Byte as hex", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-newest", &set, nullptr), ECUIO_OK);

    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://newest?profile=test-newest").isEmpty());
    spinLoop(150);   // a tick or more, so the window below starts clean
    const int batches = bridge.signalBatchCount();
    uint64_t at = 0;
    for (const char* hex : {"14", "1E", "0A"}) {
        ecuio_value_t values[2] = {};
        values[0].signal_id = 1;
        values[0].kind = ECUIO_VALUE_UINT;
        values[0].value = static_cast<double>(std::strtol(hex, nullptr, 16));
        values[1].signal_id = 2;
        values[1].kind = ECUIO_VALUE_HEX;
        values[1].text = hex;
        values[1].text_len = 2;
        ecuio_value_batch_t batch = {};
        batch.timestamp_us = at += 1000;
        batch.arbitration_id = 0x123;
        batch.count = 2;
        batch.values = values;
        bridge.dispatchValuesForTesting(batch);
    }
    ASSERT_TRUE(spinUntil([&]() { return bridge.signalBatchCount() > batches; }, 2000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::ValueRole).toDouble(), 10.0)
        << "the newest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowHighRole).toDouble(), 30.0)
        << "the Dashboard's peak hold sees the window's highest";
    EXPECT_EQ(model->data(model->index(0, 0), SignalListModel::WindowLowRole).toDouble(), 10.0);
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowHighRole), "windowHigh");
    EXPECT_EQ(model->roleNames().value(SignalListModel::WindowLowRole), "windowLow");
    EXPECT_EQ(model->data(model->index(1, 0), SignalListModel::TextRole).toString(),
              QStringLiteral("0A"));
    const QVariantList chart = model->chartData();
    ASSERT_EQ(chart.size(), 1);
    QList<double> samples;
    for (const QVariant& v : chart.at(0).toMap().value("samples").toList())
        samples << v.toDouble();
    EXPECT_EQ(samples, (QList<double>{2, 30, 3, 10}))
        << "the peak at 2 ms and the low, which is the newest, at 3 ms";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("test-newest"), ECUIO_OK);
}

namespace {

/* Dispatches n frames with ID 0x123 and stamps first, first + 1, …,
 * letting each burst drain so none is dropped at the in-flight gate. */
void dispatchNumbered(SessionBridge& bridge, int first, int n)
{
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;   // not the mock's 0x0C0
    f.data_length = 2;
    const int burst = SessionBridge::maxInFlightForTesting() / 2;
    for (int i = 0; i < n; ++i) {
        f.timestamp_us = static_cast<uint64_t>(first + i);
        f.data[0] = static_cast<uint8_t>((first + i) >> 8);
        f.data[1] = static_cast<uint8_t>(first + i);
        bridge.dispatchFrameForTesting(f);
        if ((i + 1) % burst == 0) {
            spinLoop(5);
        }
    }
    spinLoop(50);
}

/* The 0x123 rows' stamps, top to bottom; mock frames between them are
 * counted in others. */
std::vector<quint64> numberedRows(const FrameListModel* model, int* others)
{
    std::vector<quint64> out;
    *others = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() == 0x123u) {
            out.push_back(model->data(idx, FrameListModel::TimestampRole).toULongLong());
        } else {
            ++*others;
        }
    }
    return out;
}

} // namespace

/* Past the cap the log keeps the newest MaxFrames in arrival order, in
 * either direction, each row still showing its own frame: the ring's
 * slot arithmetic, through the overwrite of the oldest and a toggle.
 * A view sees the oldest row leave from its end and the new one arrive
 * at the other, which is what keeps a ListView's rows right. */
TEST(BridgeTest, FullFrameLogKeepsTheNewestInOrder)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://ring").isEmpty());
    FrameListModel* model = bridge.model();
    int wrongRemovals = 0;
    int wrongInserts = 0;
    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeRemoved,
                     [model, &wrongRemovals](const QModelIndex&, int first, int last) {
        const int oldest = model->newestFirst() ? model->rowCount() - 1 : 0;
        if (first != oldest || last != oldest) ++wrongRemovals;
    });
    QObject::connect(model, &QAbstractItemModel::rowsInserted,
                     [model, &wrongInserts](const QModelIndex&, int first, int last) {
        const int newest = model->newestFirst() ? 0 : model->rowCount() - 1;
        if (first != newest || last != newest) ++wrongInserts;
    });
    const int total = FrameListModel::MaxFrames + 250;
    dispatchNumbered(bridge, 1, total);
    ASSERT_EQ(bridge.droppedFrames(), 0);

    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    int others = 0;
    std::vector<quint64> rows = numberedRows(model, &others);
    ASSERT_EQ(static_cast<int>(rows.size()) + others, FrameListModel::MaxFrames);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front(), static_cast<quint64>(total)) << "the newest on top";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] - 1) << "row " << i << " out of order";
    }
    // Each row's payload is its own frame's, not a stale slot's: every
    // numbered row carries its stamp in its two bytes, and the mock's
    // rows between them keep their own length
    int checked = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() != 0x123u) {
            EXPECT_NE(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u)
                << "row " << r << " has a numbered frame's length";
            continue;
        }
        const quint64 n = model->data(idx, FrameListModel::TimestampRole).toULongLong();
        ASSERT_EQ(model->data(idx, FrameListModel::PayloadRole).toString(),
                  QStringLiteral("%1 %2")
                      .arg((n >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                      .arg(n & 0xFF, 2, 16, QLatin1Char('0')).toUpper())
            << "row " << r;
        ASSERT_EQ(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u) << "row " << r;
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(rows.size()));

    model->setNewestFirst(false);
    rows = numberedRows(model, &others);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back(), static_cast<quint64>(total)) << "the newest at the bottom";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }

    // And it keeps going round while oldest-first
    dispatchNumbered(bridge, total + 1, 300);
    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    rows = numberedRows(model, &others);
    EXPECT_EQ(rows.back(), static_cast<quint64>(total + 300));
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }
    EXPECT_EQ(wrongRemovals, 0) << "a row other than the oldest left";
    EXPECT_EQ(wrongInserts, 0) << "a frame arrived at the wrong end";
    bridge.disconnectSession();
}

/* A payload up to InlinePayload bytes lives in its row; a longer one
 * holds a heap block only while its row is shown, so a run of long
 * replies leaves nothing behind once short frames take the slots. Clear
 * keeps the slots (no reallocation on the next connect) and frees the
 * blocks. */
TEST(BridgeTest, FrameLogHoldsLongPayloadsOnlyWhileShown)
{
    FrameListModel model;
    auto frame = std::make_unique<ecuio_frame_t>();
    *frame = ecuio_frame_t{};
    quint64 stamp = 0;
    auto append = [&](uint16_t len) {
        frame->timestamp_us = ++stamp;
        for (uint16_t i = 0; i < len; ++i) {
            frame->data[i] = static_cast<uint8_t>(stamp + i);
        }
        model.appendFrame(*frame, len);
    };
    // The newest row's bytes, as the Log page shows them
    auto newestHex = [&]() {
        return model.data(model.index(0, 0), FrameListModel::PayloadRole)
            .toString();
    };
    auto expectedHex = [](quint64 s, int len) {
        QByteArray bytes;
        for (int i = 0; i < len; ++i)
            bytes.append(static_cast<char>(s + i));
        return QString::fromLatin1(bytes.toHex(' ').toUpper());
    };
    const int cap = FrameListModel::MaxFrames;

    for (int i = 0; i < cap; ++i)
        append(200);
    EXPECT_GE(model.spilledBytesForTesting(), static_cast<size_t>(cap) * 200);
    EXPECT_EQ(newestHex(), expectedHex(stamp, 200));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              200u);

    // Short frames round the ring: every long block is gone
    for (int i = 0; i < cap; ++i)
        append(27);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(newestHex(), expectedHex(stamp, 27));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              27u);

    // InlinePayload itself stays inline; one byte past it spills
    append(FrameListModel::InlinePayload);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(newestHex(), expectedHex(stamp, FrameListModel::InlinePayload));
    append(FrameListModel::InlinePayload + 1);
    EXPECT_GE(model.spilledBytesForTesting(),
              static_cast<size_t>(FrameListModel::InlinePayload + 1));
    EXPECT_EQ(newestHex(),
              expectedHex(stamp, FrameListModel::InlinePayload + 1));

    // Clear keeps the slots and frees the blocks
    model.clear();
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    for (int i = 0; i < 3; ++i)
        append(8);
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    for (int r = 0; r < 3; ++r) {
        EXPECT_EQ(model.data(model.index(r, 0), FrameListModel::TimestampRole)
                      .toULongLong(),
                  stamp - static_cast<quint64>(r))
            << "row " << r;
    }

    // A ring cleared before it filled grows only as far as it's used
    FrameListModel partial;
    for (int i = 0; i < 5; ++i)
        partial.appendFrame(*frame, 8);
    partial.clear();
    for (int i = 0; i < 7; ++i) {
        frame->timestamp_us = 100 + static_cast<quint64>(i);
        partial.appendFrame(*frame, 8);
    }
    EXPECT_EQ(partial.slotsForTesting(), 7u);
    ASSERT_EQ(partial.rowCount(), 7);
    for (int r = 0; r < 7; ++r) {
        EXPECT_EQ(
            partial.data(partial.index(r, 0), FrameListModel::TimestampRole)
                .toULongLong(),
            106u - static_cast<quint64>(r))
            << "row " << r;
    }
}

/* frameCount and guiNotifyCount reach QML on the 100 ms tick, not once
 * per frame: a burst of 400 frames changes each a handful of times. The
 * disconnect reports the last of them, so what QML last saw is exact. */
TEST(BridgeTest, FrameCountsAreReportedOnTheTick)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://counts").isEmpty());
    int countSignals = 0;
    int notifySignals = 0;
    int seenFrames = -1;
    int seenNotify = -1;
    QObject::connect(&bridge, &SessionBridge::frameCountChanged,
                     [&]() { ++countSignals; seenFrames = bridge.frameCount(); });
    QObject::connect(&bridge, &SessionBridge::notifyCountChanged,
                     [&](int n) { ++notifySignals; seenNotify = n; });

    const int before = bridge.frameCount();
    dispatchNumbered(bridge, 1, 400);
    spinLoop(250);   // two ticks past the burst
    EXPECT_GE(bridge.frameCount(), before + 400);
    EXPECT_GT(countSignals, 0) << "the frame count never reached QML";
    EXPECT_GT(notifySignals, 0) << "the notify count never reached QML";
    EXPECT_LT(countSignals, 40) << "reported per frame, not per tick";
    EXPECT_LT(notifySignals, 40) << "reported per frame, not per tick";

    bridge.disconnectSession();   // no frame is counted after this
    EXPECT_EQ(seenFrames, bridge.frameCount());
    EXPECT_EQ(seenNotify, bridge.guiNotifyCount());
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount())
        << "one GUI-thread delivery per frame";
}

/* Frames flagged ECUIO_FRAME_FLAG_OVERFLOW (the adapter lost frames
 * before them) are counted on the tick, the first with one activity-log
 * line; a new session starts the count again */
TEST(BridgeTest, FramesAfterALossAtTheAdapterAreCountedAndLoggedOnce)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::overflowFramesChanged,
                     [&changes]() { ++changes; });
    ASSERT_TRUE(bridge.connectSession("mock://overflow").isEmpty());
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;
    f.data_length = 1;
    bridge.dispatchFrameForTesting(f);
    f.flags = ECUIO_FRAME_FLAG_OVERFLOW;
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    bridge.dispatchFrameForTesting(f);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 3);
    EXPECT_GE(changes, 2);
    const QString line =
        QStringLiteral("The adapter's buffer overflowed: frames were lost");
    EXPECT_EQ(log.count(line), 1) << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.overflowFrames(), 3) << "readable after the disconnect";
    ASSERT_TRUE(bridge.connectSession("mock://overflow-again").isEmpty());
    EXPECT_EQ(bridge.overflowFrames(), 0);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    EXPECT_EQ(log.count(line), 2) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
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

TEST(BridgeTest, DispatchedShortFrameHonorsDataLength)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://short").isEmpty());

    // Unique timestamp so we can find OUR row among mock frames
    ecuio_frame_t f{};
    f.timestamp_us = 777777;   // steady-clock µs from the mock won't collide
    f.arbitration_id = 0x0C0;  // telemetry ID: decodes (gate-consistent)
    f.channel_id = 0;
    f.data_length = 4;          // the whole point: NOT 8
    f.flags = ECUIO_FRAME_FLAG_EXTENDED;    // was: f.is_extended = 1
    f.data[0] = 0x34;           // arbitrary RPM-looking bytes
    f.data[1] = 0x12;
    f.data[2] = 0xAA;
    f.data[3] = 0x55;

    bridge.dispatchFrameForTesting(f);

    // CONTRACT: our dispatched frame reaches the model. Poll on the
    // finding condition instead of a fixed 200ms settle: the row must
    // appear through the queued-invocation path regardless of runner speed.
    FrameListModel* proxy = bridge.model();
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
 * through Bridge.model, what QML gets. */
TEST(BridgeTest, FrameRoleNamesMatchLogPageBindings)
{
    SessionBridge bridge;
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
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://pause").isEmpty());
    FrameListModel* proxy = bridge.model();
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
// Strip chart: per-signal rolling history ring in SignalListModel
// ---------------------------------------------------------------------------

namespace {

/* Minimal two-signal set, built test-side. Plain assignments only:
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

    // Push 10 MORE one-point batches than the ring holds.
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
        // Exactly capacity survives: ring size is a hard bound, no leaks
        ASSERT_EQ(samples.size(), SignalListModel::HistoryCapacity * 2)
            << "wrong sample count for " << m.value("name").toString().toStdString();

        // Oldest retained = push #10; newest = the last push.
        // Exact ints: injected timestamps make wrap position deterministic.
        const double newest = SignalListModel::HistoryCapacity + 9;
        EXPECT_EQ(samples.at(0).toDouble(), 10.0);
        EXPECT_EQ(samples.at(samples.size() - 2).toDouble(), newest);

        // Values track their timestamps (order preserved through the wrap)
        const double offset = (m.value("id").toInt() == 1001) ? 0.0 : -40.0;
        EXPECT_NEAR(samples.at(1).toDouble(), 10.0 + offset, 0.01);
        EXPECT_NEAR(samples.at(samples.size() - 1).toDouble(), newest + offset, 0.01);

        // Monotonic timestamps oldest->newest (wrap-around correctness)
        double prev = -1.0;
        for (int i = 0; i < samples.size(); i += 2) {
            EXPECT_GE(samples.at(i).toDouble(), prev);
            prev = samples.at(i).toDouble();
        }
    }
}

/* A window's points (ADR-0005's appendix): its lowest, highest and
 * newest value in time order, each once; the row's age counts from the
 * value's arrival, whatever its sample time */
TEST(BridgeTest, AWindowChartsItsLowestHighestAndNewestInTimeOrder)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto window = [](double last, qint64 lastMs, double min, qint64 minMs,
                           double max, qint64 maxMs) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = last;
        w.lastMs = lastMs;
        w.min = min;
        w.minMs = minMs;
        w.max = max;
        w.maxMs = maxMs;
        return w;
    };
    const auto points = [&model]() {
        QList<double> out;
        for (const QVariant& v : model.chartData().at(0).toMap().value("samples").toList())
            out << v.toDouble();
        return out;
    };
    model.updateWindow({window(25, 30, 5, 10, 50, 20)});
    EXPECT_EQ(points(), (QList<double>{10, 5, 20, 50, 30, 25})) << "three apart";
    model.updateWindow({window(7, 40, 7, 40, 7, 40)});
    EXPECT_EQ(points().size(), 8) << "one value: one point";
    model.updateWindow({window(1, 60, 1, 60, 9, 50)});
    EXPECT_EQ(points().mid(8), (QList<double>{50, 9, 60, 1}))
        << "the high first; the low is the newest";
    EXPECT_EQ(model.data(model.index(0, 0), SignalListModel::ValueRole).toDouble(), 1.0);

    /* Sample times far past the model's clock, as a replay's can run */
    model.updateWindow({window(3, 1000000000, 3, 1000000000, 3, 1000000000)});
    const int age = model.data(model.index(0, 0), SignalListModel::AgeMsRole).toInt();
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 5000) << "from arrival, not sample time";
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
    // trace keeps its color when a signal before it is unsubscribed
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

/* A text row (a TEXT or HEX def) carries its latest text, counts as
 * having a value once one arrived, and is never plotted: not
 * subscribed, not toggled, not in chartData. */
TEST(BridgeTest, TextRowsShowTheirTextAndNeverPlot)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[1].kind = ECUIO_VALUE_HEX;   /* row 1, id 2001, a text row */
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_FALSE(model.data(row0, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row1, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model.data(row0, SignalListModel::SubscribedRole).toBool());
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    model.toggleSubscribed(1);   /* nothing to plot: a no-op */
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    EXPECT_FALSE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QString());

    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&notified](const QModelIndex& topLeft, const QModelIndex&, const QList<int>& roles) {
            if (topLeft.row() == 1) notified = roles;
        });
    model.updateTexts({{uint16_t(2001), QStringLiteral("62 F1 90")}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::TextRole).toString(), QStringLiteral("62 F1 90"));
    EXPECT_TRUE(model.data(row1, SignalListModel::HasValueRole).toBool());
    EXPECT_NE(model.data(row1, SignalListModel::AgeMsRole).toInt(), -1);   /* arrived */
    EXPECT_TRUE(notified.contains(SignalListModel::TextRole));
    EXPECT_TRUE(notified.contains(SignalListModel::HasValueRole));
    EXPECT_TRUE(notified.contains(SignalListModel::AgeMsRole));
    EXPECT_TRUE(model.chartData().isEmpty()) << "a text row was plotted";

    /* A numeric row ignores a text batch */
    model.updateTexts({{uint16_t(1001), QStringLiteral("no")}}, 11);
    EXPECT_FALSE(model.data(row0, SignalListModel::HasValueRole).toBool());
    EXPECT_EQ(model.data(row0, SignalListModel::TextRole).toString(), QString());
}

/* A text def takes the bridge's whole path like a number: decoded
 * from the frame (here two bytes of the mock's telemetry as hex),
 * staged, shown on its row, and written to signals.csv. */
TEST(BridgeTest, TextSignalsReachTheRowAndTheRecording)
{
    static ecuio_signal_def_t defs[2] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_HEX;
    defs[0].message_id = 0x0C0u;   /* the mock's telemetry frame */
    defs[0].byte_length = 2;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Raw bytes", sizeof(defs[0].name) - 1);
    defs[1] = {};
    defs[1].id = 2;
    defs[1].kind = ECUIO_VALUE_UINT;
    defs[1].message_id = 0x0C0u;
    defs[1].bit_length = 8;
    defs[1].scale = 1.0;
    strncpy(defs[1].name, "First byte", sizeof(defs[1].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-text", &set, nullptr), ECUIO_OK);

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://text?profile=test-text").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 1; }, 5000));

    SignalListModel* model = bridge.signalModel();
    ASSERT_EQ(model->rowCount(), 2);
    const QModelIndex raw = model->index(0, 0);
    const QModelIndex first = model->index(1, 0);
    EXPECT_TRUE(model->data(raw, SignalListModel::IsTextRole).toBool());
    EXPECT_TRUE(model->data(raw, SignalListModel::HasValueRole).toBool());
    const QString text = model->data(raw, SignalListModel::TextRole).toString();
    const auto hexPair = [](const QString& s) {
        if (s.size() != 5 || s[2] != QLatin1Char(' ')) return false;
        for (int i : {0, 1, 3, 4}) {
            const QChar c = s[i];
            if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) return false;
        }
        return true;
    };
    EXPECT_TRUE(hexPair(text)) << text.toStdString();
    EXPECT_TRUE(model->data(first, SignalListModel::HasValueRole).toBool());
    EXPECT_FALSE(model->data(first, SignalListModel::IsTextRole).toBool());
    EXPECT_EQ(model->subscribedCount(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    bool recorded = false;
    for (const QByteArray& line : signalsCsv.split('\n')) {
        const QList<QByteArray> cols = line.split(',');
        if (cols.size() == 4 && cols[1] == "Raw bytes" && hexPair(QString::fromUtf8(cols[2]))) {
            recorded = true;
            break;
        }
    }
    EXPECT_TRUE(recorded) << "no hex text row in signals.csv:\n" << signalsCsv.toStdString();
    EXPECT_EQ(ecuio_profile_unregister("test-text"), ECUIO_OK);
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
    EXPECT_EQ(roles.value(SignalListModel::TextRole), QByteArray("text"));
    EXPECT_EQ(roles.value(SignalListModel::IsTextRole), QByteArray("isText"));

    // A row answers only its roles, and only rows that exist answer
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    EXPECT_EQ(model.data(model.index(1, 0), SignalListModel::IdRole).toInt(),
              2001);
    EXPECT_FALSE(model.data(model.index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(
        model.data(model.index(2, 0), SignalListModel::NameRole).isValid());
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

TEST(BridgeTest, UnitSystemConvertsRowsAndChartAndKeepsHistory)
{
    /* The model keeps decoded values; imperial converts value, unit,
     * decimals and the chart's whole history on the way out, and metric
     * gives back exactly what was decoded. A unit with no imperial form
     * passes through. */
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    std::memset(defs[1].unit, 0, sizeof defs[1].unit);
    std::memcpy(defs[1].unit, "\xC2\xB0" "C", 3);
    defs[1].decimal_places = 1;
    model.setSignalSet(&set);
    int redraws = 0;
    QObject::connect(&model, &SignalListModel::valuesUpdated, [&redraws]() { ++redraws; });

    for (int i = 0; i < 3; ++i) {
        std::vector<std::pair<uint16_t, double>> batch;
        batch.emplace_back(uint16_t(1001), 100.0 + i);
        batch.emplace_back(uint16_t(2001), 90.0 + i);
        model.updateValues(batch, i * 100);
    }
    const int before = redraws;
    const QModelIndex speed = model.index(0, 0);
    const QModelIndex temp = model.index(1, 0);

    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(redraws, before + 1) << "the chart must redraw in the new units";
    EXPECT_NEAR(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0 * 0.621371192237334, 1e-9);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("mph"));
    EXPECT_NEAR(model.data(temp, SignalListModel::ValueRole).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
    EXPECT_EQ(model.data(temp, SignalListModel::UnitRole).toString(), QString::fromUtf8("\xC2\xB0" "F"));
    EXPECT_EQ(model.data(temp, SignalListModel::DecimalsRole).toInt(), 1);

    const QVariantList chart = model.chartData();
    ASSERT_EQ(chart.size(), 2);
    for (const QVariant& v : chart) {
        const QVariantMap m = v.toMap();
        const QVariantList samples = m.value("samples").toList();
        ASSERT_EQ(samples.size(), 6) << "history lost on a unit change";
        if (m.value("id").toInt() == 1001) {
            EXPECT_EQ(m.value("unit").toString(), QStringLiteral("mph"));
            EXPECT_NEAR(samples.at(1).toDouble(), 100.0 * 0.621371192237334, 1e-9);
        } else {
            EXPECT_NEAR(samples.at(5).toDouble(), 92.0 * 1.8 + 32.0, 1e-9);
        }
    }

    model.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(model.data(speed, SignalListModel::ValueRole).toDouble(), 102.0);
    EXPECT_EQ(model.data(speed, SignalListModel::UnitRole).toString(), QStringLiteral("km/h"));
    EXPECT_EQ(model.chartData().at(1).toMap().value("samples").toList().at(1).toDouble(), 90.0);
}

TEST(BridgeTest, UnitSystemShowsImperialAndRecordsDecodedUnits)
{
    /* The property takes metric or imperial only. The Dashboard's
     * helpers convert through core. A recording keeps what the ECU sent,
     * whatever the screen shows. */
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    EXPECT_EQ(bridge.unitSystem(), 0);
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::unitSystemChanged, [&changes]() { ++changes; });
    bridge.setUnitSystem(7);
    EXPECT_EQ(bridge.unitSystem(), 0);
    bridge.setUnitSystem(1);
    bridge.setUnitSystem(1);
    EXPECT_EQ(bridge.unitSystem(), 1);
    EXPECT_EQ(changes, 1);
    EXPECT_NEAR(bridge.displayValue(QStringLiteral("km/h"), 160.0), 99.4193907579734, 1e-9);
    EXPECT_EQ(bridge.displayUnit(QStringLiteral("km/h")), QStringLiteral("mph"));
    EXPECT_EQ(bridge.displayValue(QStringLiteral("%"), 42.0), 42.0);

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://units").isEmpty());
    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.signalBatchCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    bool sawMph = false;
    for (int r = 0; r < model->rowCount(); ++r) {
        sawMph = sawMph || model->data(model->index(r, 0), SignalListModel::UnitRole).toString() ==
                                QStringLiteral("mph");
    }
    EXPECT_TRUE(sawMph) << "no row shows mph under imperial";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray signalsCsv =
        readFile(root.path() + QStringLiteral("/") + folders.first() + QStringLiteral("/signals.csv"));
    EXPECT_TRUE(signalsCsv.contains(",Vehicle Speed,")) << signalsCsv.left(400).toStdString();
    EXPECT_TRUE(signalsCsv.contains(",km/h")) << "the recording should keep km/h";
    EXPECT_FALSE(signalsCsv.contains("mph")) << "the recording followed the display";
}

TEST(BridgeTest, StripChartDataPopulatedByLiveStream)
{
    /* End-to-end through the coalescer: real mock frames -> decode ->
       staging -> updateValues -> ring. Poll to the condition instead
       of a fixed 750ms window: the CONTRACT is history accumulates,
       not that 7.5 windows elapse. Monotonicity assertions are timing-
       independent once the data exists. */
    SessionBridge bridge;
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

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Connect and disconnect threads, against the fake J2534 DLL: its delay
// knobs stall the vendor call, so a call made on the GUI thread would
// block it, and its control page counts the teardown's calls.
// ---------------------------------------------------------------------------

namespace {

/* The fake's control page; created before any session opens, so a
 * surrogate child takes its name at spawn. */
fake_control::Page* fakePage()
{
    return fake_control::create_for_this_process();
}

/* Sets one of the fake's knobs for one test, on its page (which the
 * fake in the child reads on every call), and clears it on exit,
 * so a failing ASSERT can't leak it into the next case. */
class FakeEnvArm
{
public:
    FakeEnvArm(const char* name, const char* value) : m_name(name)
    {
        EXPECT_TRUE(fake_control::set(fakePage(), name, value)) << name;
    }
    ~FakeEnvArm() { fake_control::set(fakePage(), m_name, nullptr); }
    FakeEnvArm(const FakeEnvArm&) = delete;
    FakeEnvArm& operator=(const FakeEnvArm&) = delete;

private:
    const char* m_name;
};

/* What the fake publishes on its page, reset for one test. */
struct FakeHooks
{
    bool load()
    {
        fake_control::Page* page = fakePage();
        if (page == nullptr) return false;
        fake_control::reset(page);
        return true;
    }
    ~FakeHooks()
    {
        if (fakePage() != nullptr) fake_control::reset(fakePage());
    }
    unsigned long disconnect_count() const
    {
        return fake_control::disconnects(fakePage());
    }
};

/* A passive raw session: no requests, so the vendor calls under test
 * are the open, the engine ECU's filter and the teardown. */
QString fakeUri()
{
    return QStringLiteral("j2534://%1?profile=raw")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

}  // namespace

/* An async connect makes the slow vendor open on its own thread: with
 * PassThruOpen stalled, connectSessionAsync returns at once, so the
 * open didn't run on this (GUI) thread. */
TEST(BridgeTest, AsyncConnectOpensOffGuiThread)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm delay("ECUIO_FAKE_OPEN_DELAY_MS", "600");

    SessionBridge bridge;
    std::atomic<bool> done{false};
    QString gotErr = "unset";
    QObject::connect(&bridge, &SessionBridge::connectResult,
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

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
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

    ASSERT_GE(log.size(), 3);
    EXPECT_TRUE(log.at(log.size() - 3).startsWith(QStringLiteral("Disconnecting: ")));
    EXPECT_TRUE(log.at(log.size() - 2).startsWith(QStringLiteral("Delivery: ")));
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

    auto bridge = std::make_unique<SessionBridge>();
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

/* The adapter's session in the activity log, right after "Connected:",
 * from the fake's own readings. */
TEST(BridgeTest, ConnectLogsTheAdaptersSession)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_GE(log.size(), 2);
    EXPECT_TRUE(log.at(log.size() - 2).startsWith(QStringLiteral("Connected: ")));
    EXPECT_EQ(log.last().toStdString(),
              "Session: x64 DLL, through the surrogate; ISO 15765 at 500000 baud; "
              "profile raw, channel 0x7E8/0x7E0 (rx/tx); battery 13.8 V; "
              "firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04");
    /* What Settings keeps for the device, and none past the session */
    EXPECT_EQ(bridge.adapterVersions().toStdString(),
              "firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04");
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.adapterVersions().isEmpty());
}

/* The probe's answer reaches the activity log once: the fake's ECU lists
 * seventeen PIDs across its three bitmaps (the obd2 set's ten and the
 * diagnostics' seven), so nothing the set asks is dropped. */
TEST(BridgeTest, ConnectLogsWhatTheEcuSupports)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const auto ecu_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8:"));
    };
    ASSERT_TRUE(spinUntil([&]() { return !ecu_lines().isEmpty(); }, 5000))
        << "no ECU line in the log";
    spinLoop(300);
    ASSERT_EQ(ecu_lines().size(), 1) << ecu_lines().join('\n').toStdString();
    EXPECT_EQ(ecu_lines().first().toStdString(),
              "ECU 0x7E8: supports 17 PIDs; 10 requests, none dropped");

    /* Identification (ADR-0021) logs one line once every item has an
     * outcome: the fake's FAKE values, and the two DIDs it refuses. */
    const auto identity_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 identity:"));
    };
    ASSERT_TRUE(spinUntil([&]() { return !identity_lines().isEmpty(); }, 5000))
        << "no identity line in the log";
    spinLoop(300);
    ASSERT_EQ(identity_lines().size(), 1) << identity_lines().join('\n').toStdString();
    EXPECT_EQ(identity_lines().first().toStdString(),
              "ECU 0x7E8 identity: VIN FAKEJ2534VIN00001; CALID FAKECAL-01; "
              "CVN DE AD BE EF; ECU name FAKE-ECM; part number FAKE-PART-0001; "
              "software number FAKE-SW-0001; software version FAKE 1.0; "
              "supplier FAKE SUPPLIER; ECU serial FAKE-SERIAL-000001; "
              "VIN (F190) FAKEJ2534VIN00001; hardware number FAKE-HW-01; "
              "2 refused: 22 F1 94 (7F 22 31), 22 F1 95 (7F 22 31)");

    /* Diagnostics (ADR-0037) log one line once the list has settled:
     * the fake ECU's codes, readiness and freeze frame */
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics:"));
    };
    ASSERT_TRUE(
        spinUntil([&]() { return !diagnostics_lines().isEmpty(); }, 5000))
        << "no diagnostics line in the log";
    spinLoop(300);
    ASSERT_EQ(diagnostics_lines().size(), 1)
        << diagnostics_lines().join('\n').toStdString();
    EXPECT_EQ(diagnostics_lines().first().toStdString(),
              kFakeEcuDiagnosticsLine);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A session that opens but won't start (the adapter refuses the obd2
 * channel's filter) fails the connect with the cause, and the bridge
 * keeps no session: a second connect works once the refusal is gone. */
TEST(BridgeTest, RefusedStartNamesItsCauseAndLeavesNoSession)
{
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    {
        FakeEnvArm refuse("ECUIO_FAKE_FILTER_STATUS", "7");
        EXPECT_EQ(bridge.connectSession(uri).toStdString(),
                  "the adapter's driver refused: PassThruStartMsgFilter "
                  "returned status 7 (0x7, ERR_FAILED): fake_j2534: "
                  "PassThruStartMsgFilter refused (ECUIO_FAKE_FILTER_STATUS)");
        EXPECT_FALSE(bridge.connected());
    }
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(bridge.connected());
    ASSERT_TRUE(disconnectAndWait(bridge));
}
/* ADR-0027 §3: the fake's VIN reaches QML only as a salted SHA-256, once
 * a session, and nothing is left of it after the disconnect. A bare URI
 * runs obd2, whose rotation asks the identity. */
TEST(BridgeTest, AConnectsVinReachesQmlOnlyAsASaltedHash)
{
    SessionBridge bridge;
    int identified = 0;
    QObject::connect(&bridge, &SessionBridge::vehicleIdentified,
                     [&identified]() { ++identified; });
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty())
        << "no session, no key";
    const QString uri = QStringLiteral("j2534://%1")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil([&identified]() { return identified > 0; }, 5000))
        << "vehicleIdentified never came";
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("saltFAKEJ2534VIN00001"),
                                 QCryptographicHash::Sha256)
            .toHex());
    EXPECT_EQ(bridge.vinKey(QStringLiteral("salt")), expected);
    EXPECT_NE(bridge.vinKey(QStringLiteral("pepper")), expected)
        << "the salt is in the key";
    spinLoop(300);
    EXPECT_EQ(identified, 1) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty());
}

/* ADR-0027 §2: an adapter session's recording folder names the picked
 * vehicle, as a folder name may take it; a mock session's doesn't. */
TEST(BridgeTest, RecordingFolderNamesThePickedVehicleForAnAdapterOnly)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    bridge.setVehicleLabel(QStringLiteral("Daily driver/2"));

    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://rec")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2) << folders.join(',').toStdString();
    const QRegularExpression dateOnly(
        QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)?$"));
    const QRegularExpression named(
        QStringLiteral("^[0-9]{8}-[0-9]{6}(-[0-9]+)? Daily driver2$"));
    EXPECT_EQ(folders.filter(dateOnly).size(), 1)
        << folders.join(',').toStdString();
    EXPECT_EQ(folders.filter(named).size(), 1)
        << folders.join(',').toStdString();
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
     * session's rows with no live updates: stale data posing as live.
     * Frames still flow to the Log page; decode alone is silenced. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://profile-then-raw").isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.signalModel()->rowCount() > 0; }, 5000));
    EXPECT_GT(bridge.signalModel()->rowCount(), 0);

    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge.connectSession("mock://profile-raw-stage2?profile=raw")
                    .isEmpty());
    /* Frames keep arriving (mock streams regardless of profile):
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
     * ONLY: the stalled-row dimming path; (c) ageMs is -1 before the
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
    empty.refreshAges();   // no rows: silent, no crash
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

    /* SocketCAN (ADR-0032): a listen-only controller, and vcan */
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

    ecuio_session_info_t later{};
    later.backend = 7; // a backend this build doesn't know
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 7");
    later.backend = ECUIO_BACKEND_J2534_INPROC; // retired, never reported
    EXPECT_EQ(SessionBridge::sessionInfoText(later).toStdString(),
              "Session: backend 2");
}

/* The "ECU 0x…:" line's wording, from hand-built reports: the first
 * vehicle's three bitmaps (docs/bench-sessions/2026-10-22-vehicle.md:
 * 44 data PIDs to 0x60, whose bit says a fourth exists), a chain
 * answered to its end, one unanswered, and requests dropped for each
 * reason, past the eight a report lists. */
TEST(BridgeTest, ChannelSupportTextNamesTheEcuAndWhatItDropped)
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.requests = 10;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
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
    all.pid_bitmap[0] = 0x81;   /* PIDs 01 and 08 */
    EXPECT_EQ(SessionBridge::channelSupportText(all).toStdString(),
              "ECU 0x18DAF110: supports 2 PIDs; 1 request, none dropped");

    ecuio_channel_support_t part = all;
    part.flags = ECUIO_SUPPORT_PROBED;
    part.ranges = 0;
    part.pid_bitmap[0] = 0;
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
/* ADR-0027 §3: the VIN a vehicle is known by, from the first channel's
 * report: 09 02's text, else F190's; letters and digits only, upper
 * case, at 17 characters. */
TEST(BridgeTest, VinFromIdentityTakesService09ThenF190)
{
    const auto item = [](std::initializer_list<uint8_t> request,
                         uint8_t outcome, const std::string& text) {
        ecuio_identity_item_t it{};
        uint8_t k = 0;
        for (uint8_t b : request) {
            it.request[k++] = b;
        }
        it.length = k;
        it.outcome = outcome;
        it.text_length = static_cast<uint8_t>(text.size());
        memcpy(it.text, text.data(), text.size());
        return it;
    };
    ecuio_channel_identity_t id{};
    id.flags = ECUIO_IDENTITY_COMPLETE;
    id.items = 2;
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "f190vin0000000002");
    id.list[1] = item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED,
                      std::string(2, '\0') + " FAKEJ2534VIN00001");
    EXPECT_EQ(SessionBridge::vinFromIdentity(id).toStdString(),
              "FAKEJ2534VIN00001")
        << "09 02 first, its padding dropped";

    id.list[1].outcome = ECUIO_IDENTITY_REFUSED;
    EXPECT_EQ(SessionBridge::vinFromIdentity(id).toStdString(),
              "F190VIN0000000002")
        << "F190 when 09 02 wasn't answered, in upper case";

    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN0001");
    EXPECT_TRUE(SessionBridge::vinFromIdentity(id).isEmpty())
        << "16 characters";
    id.list[0] =
        item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN000012");
    EXPECT_TRUE(SessionBridge::vinFromIdentity(id).isEmpty())
        << "18 characters";

    id.list[0] =
        item({0x09, 0x04}, ECUIO_IDENTITY_ANSWERED, "CALIBRATION00001X");
    EXPECT_TRUE(SessionBridge::vinFromIdentity(id).isEmpty())
        << "a CALID of 17 isn't a VIN";

    id.items = 1;
    id.list[1] =
        item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED, "FAKEJ2534VIN00001");
    EXPECT_TRUE(SessionBridge::vinFromIdentity(id).isEmpty())
        << "past the count isn't read";
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

/* ADR-0027 §2: a label as a recording folder may take it */
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

/* ADR-0027 §1: a vehicle's plotted signals are the numeric rows the
 * chart leaves out, read and set by name */
TEST(BridgeTest, UnplottedNamesReadAndSetTheChartsRows)
{
    SessionBridge bridge;
    SignalListModel* m = bridge.signalModel();
    int countNotified = 0;
    QObject::connect(m, &SignalListModel::subscribedCountChanged,
                     [&countNotified]() { ++countNotified; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_EQ(countNotified, 1) << "a new set's count is notified";
    ASSERT_GE(m->rowCount(), 3);
    const auto name = [m](int row) {
        return m->data(m->index(row), SignalListModel::NameRole).toString();
    };
    const auto plotted = [m](int row) {
        return m->data(m->index(row), SignalListModel::SubscribedRole).toBool();
    };
    const int all = m->subscribedCount();
    EXPECT_TRUE(m->unplottedNames().isEmpty())
        << "every numeric row starts plotted";

    m->setUnplotted({name(0), name(2), QStringLiteral("Not a signal")});
    EXPECT_EQ(m->unplottedNames(), (QStringList{name(0), name(2)}));
    EXPECT_EQ(m->subscribedCount(), all - 2);
    EXPECT_FALSE(plotted(0));
    EXPECT_TRUE(plotted(1));

    m->setUnplotted({});
    EXPECT_TRUE(m->unplottedNames().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all);

    m->toggleSubscribed(1);
    EXPECT_EQ(m->unplottedNames(), QStringList{name(1)})
        << "a click is read back";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The identity line (ADR-0021): each answered item by name, the refused
 * requests with their codes, the unanswered count; a request outside the
 * list is named by its bytes, an empty answer says so. */
TEST(BridgeTest, ChannelIdentityTextNamesWhatTheEcuSaid)
{
    const auto item = [](std::initializer_list<uint8_t> request, uint8_t outcome,
                         uint8_t nrc, const char* text) {
        ecuio_identity_item_t it{};
        uint8_t k = 0;
        for (uint8_t b : request) {
            it.request[k++] = b;
        }
        it.length = k;
        it.outcome = outcome;
        it.nrc = nrc;
        it.text_length = static_cast<uint8_t>(strlen(text));
        memcpy(it.text, text, it.text_length);
        return it;
    };

    ecuio_channel_identity_t car{};
    car.flags = ECUIO_IDENTITY_COMPLETE;
    car.rx_id = 0x7E8;
    car.items = 5;
    car.list[0] = item({0x09, 0x02}, ECUIO_IDENTITY_ANSWERED, 0, "FAKEJ2534VIN00001");
    car.list[1] = item({0x09, 0x04}, ECUIO_IDENTITY_ANSWERED, 0, "CAL-A, CAL-B");
    car.list[2] = item({0x22, 0xF1, 0x94}, ECUIO_IDENTITY_REFUSED, 0x31, "");
    car.list[3] = item({0x09, 0x0A}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    car.list[4] = item({0x22, 0xF1, 0x8C}, ECUIO_IDENTITY_REFUSED, 0x33, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(car).toStdString(),
              "ECU 0x7E8 identity: VIN FAKEJ2534VIN00001; CALID CAL-A, CAL-B; "
              "2 refused: 22 F1 94 (7F 22 31), 22 F1 8C (7F 22 33); 1 unanswered");

    ecuio_channel_identity_t silent{};
    silent.flags = ECUIO_IDENTITY_COMPLETE;
    silent.rx_id = 0x18DAF110;
    silent.items = 2;
    silent.list[0] = item({0x09, 0x02}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    silent.list[1] = item({0x22, 0xF1, 0x90}, ECUIO_IDENTITY_UNANSWERED, 0, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(silent).toStdString(),
              "ECU 0x18DAF110 identity: 2 unanswered");

    ecuio_channel_identity_t odd{};
    odd.rx_id = 0x7E9;
    odd.items = 2;
    odd.list[0] = item({0x22, 0x12, 0x34}, ECUIO_IDENTITY_ANSWERED, 0, "");
    odd.list[1] = item({0x22, 0xF1, 0x91}, ECUIO_IDENTITY_UNASKED, 0, "");
    EXPECT_EQ(SessionBridge::channelIdentityText(odd).toStdString(),
              "ECU 0x7E9 identity: 22 12 34 (empty)");

    ecuio_channel_identity_t none{};
    none.rx_id = 0x7E8;
    EXPECT_EQ(SessionBridge::channelIdentityText(none).toStdString(),
              "ECU 0x7E8 identity: nothing asked");
}

/* The mock's one ECU reports the fake ECU's diagnostics, so the line
 * the activity log and a recording's events.log keep is the same on
 * every platform: logged once, after the Session line */
TEST(BridgeTest, ConnectLogsTheMocksDiagnostics)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const auto lines = [&log]() {
        return log.filter(QStringLiteral(" diagnostics: "));
    };
    ASSERT_TRUE(spinUntil([&]() { return !lines().isEmpty(); }, 2000))
        << "no diagnostics line in the log";
    spinLoop(300);
    ASSERT_EQ(lines().size(), 1) << lines().join('\n').toStdString();
    EXPECT_EQ(lines().first().toStdString(), kFakeEcuDiagnosticsLine);
    EXPECT_GT(
        log.indexOf(lines().first()),
        log.indexOf(QStringLiteral("Session: mock transport; profile mock")));
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* ?diagnose=0: no line */
    log.clear();
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo?diagnose=0"))
                    .isEmpty());
    spinLoop(300);
    EXPECT_TRUE(lines().isEmpty()) << lines().join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The diagnostics line's wording: each list as answered, cut, refused
 * or unanswered; readiness with the incomplete monitors by their short
 * names, a compression engine's by its own; the freeze frame's code or
 * its absence; nothing asked */
TEST(BridgeTest, ChannelDiagnosticsTextNamesWhatTheEcuReported)
{
    const auto monitor = [](uint8_t id, bool complete) {
        ecuio_monitor_t m{};
        m.id = id;
        m.flags = complete ? ECUIO_MONITOR_COMPLETE : 0;
        return m;
    };

    ecuio_channel_diagnostics_t older{};
    older.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS;
    older.rx_id = 0x7E8;
    older.readiness = ECUIO_DIAG_ANSWERED;
    older.monitor_count = 4;
    older.monitors[0] = monitor(0, true);
    older.monitors[1] = monitor(8, false);
    older.monitors[2] = monitor(10, false);
    older.monitors[3] = monitor(14, true);
    older.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    older.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_ANSWERED;
    older.lists[ECUIO_DIAG_PENDING].reported = 5;
    older.lists[ECUIO_DIAG_PENDING].held = 2;
    older.lists[ECUIO_DIAG_PENDING].codes[0] = 0x0171;
    older.lists[ECUIO_DIAG_PENDING].codes[1] = 0x4035;
    older.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_REFUSED;
    older.lists[ECUIO_DIAG_PERMANENT].nrc = 0x11;
    older.freeze = ECUIO_DIAG_ANSWERED;
    EXPECT_EQ(
        SessionBridge::channelDiagnosticsText(older).toStdString(),
        "ECU 0x7E8 diagnostics: MIL off; stored none; pending P0171 C0035 "
        "and 3 more; permanent refused (7F 0A 11); readiness 2 of 4 "
        "complete (catalyst, EVAP incomplete); no freeze frame");

    ecuio_channel_diagnostics_t diesel{};
    diesel.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE |
                   ECUIO_DIAG_READINESS | ECUIO_DIAG_MIL_ON |
                   ECUIO_DIAG_COMPRESSION | ECUIO_DIAG_FREEZE_FRAME;
    diesel.rx_id = 0x7E8;
    diesel.readiness = ECUIO_DIAG_ANSWERED;
    diesel.monitor_count = 2;
    diesel.monitors[0] = monitor(8, false);
    diesel.monitors[1] = monitor(14, false);
    diesel.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    diesel.lists[ECUIO_DIAG_STORED].reported = 1;
    diesel.lists[ECUIO_DIAG_STORED].held = 1;
    diesel.lists[ECUIO_DIAG_STORED].codes[0] = 0x2002;
    diesel.freeze = ECUIO_DIAG_ANSWERED;
    diesel.freeze_dtc = 0x2002;
    EXPECT_EQ(
        SessionBridge::channelDiagnosticsText(diesel).toStdString(),
        "ECU 0x7E8 diagnostics: MIL on; stored P2002; readiness 0 of 2 "
        "complete (NMHC catalyst, PM filter incomplete); freeze frame for "
        "P2002");

    ecuio_channel_diagnostics_t silent{};
    silent.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    silent.rx_id = 0x18DAF110;
    silent.readiness = ECUIO_DIAG_UNANSWERED;
    for (ecuio_dtc_list_t& l : silent.lists) {
        l.outcome = ECUIO_DIAG_UNANSWERED;
    }
    silent.freeze = ECUIO_DIAG_REFUSED;
    silent.freeze_nrc = 0x12;
    EXPECT_EQ(SessionBridge::channelDiagnosticsText(silent).toStdString(),
              "ECU 0x18DAF110 diagnostics: stored unanswered; pending "
              "unanswered; permanent unanswered; readiness unanswered; freeze "
              "frame refused (7F 02 12)");

    ecuio_channel_diagnostics_t none{};
    none.rx_id = 0x7E8;
    EXPECT_EQ(SessionBridge::channelDiagnosticsText(none).toStdString(),
              "ECU 0x7E8 diagnostics: nothing asked");
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

/* A replay whose recording changes under it stops, and the bridge says
 * why: the "stopped" state and one log line with the frames sent, never
 * "Replay finished". The file is rewritten while it loops, so the next
 * pass (or the one under way) meets a row it can't read. */
TEST(BridgeTest, ReplayThatStopsEarlySaysSo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString csv = dir.filePath(QStringLiteral("frames.csv"));
    const auto write = [&csv](const QByteArray& text) {
        QFile f(csv);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               f.write(text) == text.size();
    };
    QByteArray rows = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < 5; ++i) {
        rows += QByteArray::number(10000 * i) + ',' + QByteArray::number(10000 * i) +
                ",0x7E8,0x0,41 0C 1F 40\n";
    }
    ASSERT_TRUE(write(rows));

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("replay://") + csv +
                                      QStringLiteral("?loop=1"))
                    .isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.linkState().toStdString(), "ok");
    ASSERT_TRUE(write("host_us,timestamp_us,id,flags,data\nnot a row\n"));

    ASSERT_TRUE(spinUntil([&bridge]() { return bridge.linkState() == QStringLiteral("stopped"); },
                          5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.linkErrorCode(), static_cast<int>(ECUIO_ERR_RECORDING_CHANGED));
    EXPECT_EQ(bridge.errorEpisodes(), 1);
    spinLoop(300);
    const QStringList stopped = log.filter(QStringLiteral("Replay stopped after "));
    ASSERT_EQ(stopped.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(stopped.first().endsWith(
        QStringLiteral(" frames: the recording changed after it was opened")))
        << stopped.first().toStdString();
    EXPECT_TRUE(log.filter(QStringLiteral("Replay finished")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0023's round trip: a recorded mock session, replayed at full speed
 * under its profile, delivers every frame (the first included, though a
 * replay sends it the moment the session starts) and decodes every value
 * the recording holds; the replay's own recording matches the first one
 * row for row, its times rebased to the first frame. The end of the file
 * is logged once. */
TEST(BridgeTest, ReplayOfAMockRecordingDecodesWhatWasRecorded)
{
    QTemporaryDir first, second;
    ASSERT_TRUE(first.isValid() && second.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecordingsRootForTesting(first.path());
    ASSERT_TRUE(bridge.connectSession("mock://roundtrip").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 30; }, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList recorded = recordingFolders(first.path());
    ASSERT_EQ(recorded.size(), 1);
    const QString a = first.path() + QStringLiteral("/") + recorded.first();
    const QList<QByteArray> framesA =
        fileLines(a + QStringLiteral("/frames.csv"));
    const int n = static_cast<int>(framesA.size()) - 1;
    ASSERT_GT(n, 30);

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(second.path());
    const QString uri = QStringLiteral("replay://") + a +
                        QStringLiteral("?profile=mock&speed=max");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString finished =
        QStringLiteral("Replay finished: %1 frames; the session stays open")
            .arg(n);
    ASSERT_TRUE(spinUntil(
        [&log, &finished]() { return log.contains(finished); }, 10000))
        << log.join(QLatin1Char('\n')).toStdString();
    spinLoop(300); // the last frames' queued calls, and a tick or two
    EXPECT_EQ(bridge.droppedFrames(), 0)
        << "the count below is whole only without drops";
    EXPECT_EQ(bridge.frameCount(), n) << "every frame, the first included";
    EXPECT_EQ(log.count(finished), 1) << "logged once";
    const QString session =
        QStringLiteral("Session: replay of a recording, %1 frames over ")
            .arg(n);
    EXPECT_TRUE(std::any_of(
        log.cbegin(), log.cend(),
        [&session](const QString& l) { return l.startsWith(session); }))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList replayed = recordingFolders(second.path());
    ASSERT_EQ(replayed.size(), 1);
    const QString b = second.path() + QStringLiteral("/") + replayed.first();
    const QList<QByteArray> framesB =
        fileLines(b + QStringLiteral("/frames.csv"));
    ASSERT_EQ(framesB.size(), framesA.size());
    const qulonglong t0 = framesA.at(1).split(',').at(1).toULongLong();
    for (int i = 1; i < framesA.size(); ++i) {
        const QList<QByteArray> ra = framesA.at(i).split(',');
        const QList<QByteArray> rb = framesB.at(i).split(',');
        ASSERT_EQ(ra.size(), 5);
        ASSERT_EQ(rb.size(), 5);
        EXPECT_EQ(rb.at(1).toULongLong(), ra.at(1).toULongLong() - t0)
            << "row " << i;
        EXPECT_EQ(rb.at(2), ra.at(2)) << "row " << i;
        EXPECT_EQ(rb.at(3), ra.at(3)) << "row " << i;
        ASSERT_EQ(rb.at(4), ra.at(4)) << "row " << i;
    }

    // Each decoded value, signal by signal in order (host_us aside)
    const auto values = [](const QString& dir) {
        QList<QByteArray> out;
        const QList<QByteArray> rows =
            fileLines(dir + QStringLiteral("/signals.csv"));
        for (int i = 1; i < rows.size(); ++i) {
            out << rows.at(i).mid(rows.at(i).indexOf(',') + 1);
        }
        return out;
    };
    const QList<QByteArray> valuesA = values(a);
    ASSERT_FALSE(valuesA.isEmpty());
    EXPECT_EQ(values(b), valuesA);
}

/* Settings' recordings list: a recording replays under the profile it
 * was recorded with, so a tap leaves nothing to type */
/* A sleep while connected (ADR-0033 §6) is one activity line saying how
 * long, in the recording's events.log too; a rise under a second, one
 * before the connect, or one after the disconnect says nothing. No runner
 * can sleep, so the seam adds to the count the tick reads */
TEST(BridgeTest, ASleepWhileConnectedIsLoggedWithItsLength)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    // Before the connect: not this session's
    bridge.addSleptForTesting(5000000);
    ASSERT_TRUE(bridge.connectSession("mock://asleep").isEmpty());
    spinLoop(300);
    EXPECT_TRUE(log.filter(QStringLiteral("Asleep")).isEmpty())
        << log.join(QLatin1Char('\n')).toStdString();

    bridge.addSleptForTesting(98400000);
    const QString line =
        QStringLiteral("Asleep for 98.4 s: nothing was read meanwhile");
    ASSERT_TRUE(spinUntil([&log, &line]() { return log.contains(line); }, 5000))
        << log.join(QLatin1Char('\n')).toStdString();
    bridge.addSleptForTesting(900000);
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral("Asleep")).size(), 1)
        << "told once; 0.9 s is no line";

    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.addSleptForTesting(3000000);
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral("Asleep")).size(), 1)
        << "nothing while disconnected";

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QList<QByteArray> events =
        fileLines(root.path() + QStringLiteral("/") + folders.first() +
                  QStringLiteral("/events.log"));
    EXPECT_TRUE(std::any_of(
        events.cbegin(), events.cend(),
        [&line](const QByteArray& e) { return e.endsWith(line.toUtf8()); }));
}

TEST(BridgeTest, ReplayUriForTakesTheRecordedProfile)
{
    // A folder as QFileInfo gives it, '/' on every platform
    const auto r = [](const char* recorded) {
        return SessionBridge::replayUriFor(
                   QStringLiteral("C:/rec/20261028-143012 Daily driver"),
                   QString::fromUtf8(recorded))
            .toStdString();
    };
    const std::string base =
        "replay://C:/rec/20261028-143012 Daily driver?profile=";
    EXPECT_EQ(r("elm327://192.168.4.1:23?profile=raw&poll=0x0C"), base + "raw")
        << "an adapter's keys stay behind";
    EXPECT_EQ(r("elm327://COM5"), base + "obd2");
    EXPECT_EQ(r("j2534://OBDX Pro - VX"), base + "obd2");
    EXPECT_EQ(r("mock://demo"), base + "mock");
    EXPECT_EQ(r("replay://C:/rec/x?speed=10"), base + "raw");
    EXPECT_EQ(r(""), base + "raw")
        << "no source named: what can't decode wrongly";
    EXPECT_EQ(r("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16"),
              base + "uds-generic&did=F190:text,1E1C:u16");
    EXPECT_EQ(r("mock://demo?did=F190&profile=obd2"), base + "obd2")
        << "did= only under uds-generic, which core insists on";
    EXPECT_EQ(r("mock://demo?profile=raw&profile=obd2"), base + "obd2")
        << "the last profile= wins, as in core";
}

TEST(BridgeTest, RecentRecordingsListTheNewestFirst)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto make = [&root](const char* name, const char* uriLine) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
        if (uriLine != nullptr) {
            QFile events(dir + QStringLiteral("/events.log"));
            ASSERT_TRUE(events.open(QIODevice::WriteOnly));
            events.write("Havne session recording (ADR-0017)\n"
                         "started  2026-10-28 14:30:12.000 (local time)\n"
                         "core     0.3.2\n");
            events.write(uriLine);
            events.write("\n");
        }
    };
    make("20261027-090000", "uri      mock://demo");
    make("20261028-143012 Daily driver",
         "uri      elm327://192.168.4.1:23?profile=raw&poll=0x0C");
    make("20261028-143012-2 Daily driver", nullptr);
    ASSERT_TRUE(QDir().mkpath(root.path() + QStringLiteral("/20261029-080000")))
        << "no frames.csv: not a recording";

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3);
    const auto field = [&rows](int i, const char* key) {
        return rows.at(i)
            .toMap()
            .value(QString::fromUtf8(key))
            .toString()
            .toStdString();
    };
    const std::string at = "replay://" + root.path().toStdString() + "/";
    EXPECT_EQ(field(0, "title"), "2026-10-28 14:30:12 \xC2\xB7 Daily driver");
    EXPECT_EQ(field(0, "uri"),
              at + "20261028-143012-2 Daily driver?profile=raw");
    EXPECT_EQ(field(0, "source"), "") << "no events.log";
    EXPECT_EQ(field(1, "title"), "2026-10-28 14:30:12 \xC2\xB7 Daily driver");
    EXPECT_EQ(field(1, "source"),
              "elm327://192.168.4.1:23?profile=raw&poll=0x0C");
    EXPECT_EQ(field(1, "profile"), "raw");
    EXPECT_EQ(field(2, "title"), "2026-10-27 09:00:00");
    EXPECT_EQ(field(2, "uri"), at + "20261027-090000?profile=mock");
    EXPECT_EQ(field(2, "profile"), "mock");

    EXPECT_EQ(bridge.recentRecordings(2).size(), 2);
    EXPECT_TRUE(bridge.recentRecordings(0).isEmpty());
    bridge.setRecordingsRootForTesting(root.path() + QStringLiteral("/none"));
    EXPECT_TRUE(bridge.recentRecordings(10).isEmpty()) << "no folder yet";
}

/* With Record on, a replay isn't recorded: it would copy the recording
 * it plays (the third phone run's Record left on made one) */
TEST(BridgeTest, AReplayIsNotRecorded)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_EQ(bridge.connectSession(uri), QString());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Not recording: a replay plays a recording already")))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());
    EXPECT_TRUE(bridge.recording())
        << "the switch stays on for the next session";
}

/* The list's URI connects as it stands, uds-generic's DIDs and all */
// ---- What the product's earlier name left (ADR-0001's appendix of
// 2026-10-28): its preferences copied once, its recordings read --------

namespace {

// QSettings' default format and the user scope's ini path, pointed at a
// scratch directory for one test, then back to the native format
struct ScratchSettings {
    QTemporaryDir dir;
    ScratchSettings()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           dir.path());
    }
    ~ScratchSettings() { QSettings::setDefaultFormat(QSettings::NativeFormat); }
};

QVariant storedValue(const char* org, const char* app, const char* key)
{
    QSettings store(QSettings::IniFormat, QSettings::UserScope,
                    QString::fromUtf8(org), QString::fromUtf8(app));
    return store.value(QString::fromUtf8(key));
}

void storeValue(const char* org, const char* app, const char* key,
                const QVariant& value)
{
    QSettings store(QSettings::IniFormat, QSettings::UserScope,
                    QString::fromUtf8(org), QString::fromUtf8(app));
    store.setValue(QString::fromUtf8(key), value);
}

} // namespace

TEST(BridgeTest, CarryOverCopiesTheEarlierPreferencesOnce)
{
    ScratchSettings scratch;
    ASSERT_TRUE(scratch.dir.isValid());
    storeValue("Old", "Old", "ui/zoom", 1.25);
    storeValue("Old", "Old", "session/uri", QStringLiteral("mock://demo"));
    storeValue("Old", "Old", "vehicles/document", QStringLiteral("{\"list\":[]}"));

    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              3);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.25);
    EXPECT_EQ(storedValue("New", "New", "session/uri").toString(),
              QStringLiteral("mock://demo"));
    EXPECT_EQ(storedValue("New", "New", "vehicles/document").toString(),
              QStringLiteral("{\"list\":[]}"));

    // Once: a later change to the earlier store isn't copied again
    storeValue("Old", "Old", "ui/zoom", 2.0);
    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              0);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.25);
    // and the earlier store is left as it was, for an older build
    EXPECT_EQ(storedValue("Old", "Old", "session/uri").toString(),
              QStringLiteral("mock://demo"));
}

TEST(BridgeTest, CarryOverNeverWritesOverAStoreThatHasPreferences)
{
    ScratchSettings scratch;
    ASSERT_TRUE(scratch.dir.isValid());
    storeValue("Old", "Old", "ui/zoom", 1.25);
    storeValue("Old", "Old", "session/uri", QStringLiteral("mock://demo"));
    storeValue("New", "New", "ui/zoom", 1.5);

    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("New"), QStringLiteral("New")),
              0);
    EXPECT_EQ(storedValue("New", "New", "ui/zoom").toDouble(), 1.5);
    EXPECT_FALSE(storedValue("New", "New", "session/uri").isValid());

    // The same name, or nothing to carry: nothing happens
    EXPECT_EQ(carryOverSettings(QStringLiteral("Old"), QStringLiteral("Old"),
                                QStringLiteral("Old"), QStringLiteral("Old")),
              0);
    EXPECT_EQ(carryOverSettings(QStringLiteral("None"), QStringLiteral("None"),
                                QStringLiteral("Empty"), QStringLiteral("Empty")),
              0);
}

TEST(BridgeTest, RecentRecordingsReadTheEarlierNamesFolderToo)
{
    QTemporaryDir current;
    QTemporaryDir earlier;
    ASSERT_TRUE(current.isValid());
    ASSERT_TRUE(earlier.isValid());
    const auto make = [](const QTemporaryDir& root, const char* name) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
    };
    make(current, "20261029-090000");
    make(earlier, "20261028-100000");
    make(earlier, "20261030-080000 Weekend car");

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(current.path());
    bridge.setEarlierRecordingsRootForTesting(earlier.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3) << "both folders are read";
    const auto uri = [&rows](int i) {
        return rows.at(i).toMap().value(QStringLiteral("uri")).toString().toStdString();
    };
    // Newest first across the two, each replaying from its own folder
    EXPECT_EQ(uri(0), "replay://" + earlier.path().toStdString() +
                          "/20261030-080000 Weekend car?profile=raw");
    EXPECT_EQ(uri(1), "replay://" + current.path().toStdString() +
                          "/20261029-090000?profile=raw");
    EXPECT_EQ(uri(2), "replay://" + earlier.path().toStdString() +
                          "/20261028-100000?profile=raw");
    EXPECT_EQ(bridge.recentRecordings(2).size(), 2) << "the limit spans both";
}

TEST(BridgeTest, RecentRecordingsReplayUnderTheirOwnProfile)
{
    QTemporaryDir first, second;
    ASSERT_TRUE(first.isValid() && second.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(first.path());
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral(
                "mock://recent?profile=uds-generic&did=F190:text,1E1C:u16"))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 3; }, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QVariantList rows = bridge.recentRecordings(5);
    ASSERT_EQ(rows.size(), 1);
    const QString uri =
        rows.first().toMap().value(QStringLiteral("uri")).toString();
    EXPECT_TRUE(uri.endsWith(
        QStringLiteral("?profile=uds-generic&did=F190:text,1E1C:u16")))
        << uri.toStdString();

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(second.path());
    ASSERT_EQ(bridge.connectSession(uri), QString());
    EXPECT_TRUE(std::any_of(log.cbegin(), log.cend(), [](const QString& l) {
        return l.startsWith(QStringLiteral("Session: replay of a recording")) &&
               l.endsWith(QStringLiteral("; profile uds-generic"));
    })) << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* What core's values callback delivers, as signals.csv rows: name and
 * value, formatted as SessionRecorder writes them */
struct CoreRows {
    QHash<uint16_t, QByteArray> names;
    std::vector<std::pair<QByteArray, QByteArray>> rows;
    std::atomic<int> frames{0};
};

void core_rows_frame_cb(const ecuio_frame_t*, void* user_data)
{
    ++static_cast<CoreRows*>(user_data)->frames;
}

void core_rows_values_cb(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* c = static_cast<CoreRows*>(user_data);
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        c->rows.emplace_back(c->names.value(v.signal_id),
                             v.text != nullptr
                                 ? QByteArray(v.text)
                                 : QByteArray::number(v.value, 'g', 12));
    }
}

} // namespace

/* The chart keeps a peak (ADR-0005's appendix): the vehicle fixture's
 * engine start peaks at 1,261 rpm, and replayed at 100x under obd2,
 * ten seconds of the recording to a window, the chart reaches it. Each
 * sample sits at its frame's time in the recording, in order */
TEST(BridgeTest, TheChartKeepsTheFixturesPeak)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=100")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    double peak = 0.0;
    bool ordered = true;
    qint64 lastMs = -1;
    QObject::connect(model, &SignalListModel::valuesUpdated, model, [&]() {
        for (const QVariant& s : model->chartData()) {
            const QVariantMap m = s.toMap();
            if (m.value("name").toString() != QStringLiteral("RPM"))
                continue;
            const QVariantList samples = m.value("samples").toList();
            qint64 prev = -1;
            for (qsizetype i = 0; i + 1 < samples.size(); i += 2) {
                const qint64 t = samples.at(i).toLongLong();
                ordered = ordered && t >= prev && t <= 297446;
                prev = t;
                peak = std::max(peak, samples.at(i + 1).toDouble());
            }
            lastMs = prev;
        }
    });
    ASSERT_TRUE(spinUntil(
        [&bridge]() { return bridge.frameCount() + bridge.droppedFrames() >= 7074; },
        20000));
    spinLoop(300);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_NEAR(peak, 1261.25, 0.01) << "the recording's peak, the sheet's 1,261";
    EXPECT_TRUE(ordered) << "times in order, inside the recording";
    EXPECT_GT(lastMs, 290000) << "the last sample near the recording's end";
}

/* LatencyStats reads a quantile back at the top of the bucket holding
 * it: exact below 16 µs, at most an eighth above the true value past
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
 * queued, then each path's median, p99 and max (µs below a millisecond,
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

/* The recording holds every value core decodes (ADR-0024): over the
 * vehicle fixture under obd2, the bridge's signals.csv is, row for row,
 * what core's values callback delivers to a session of its own for the
 * same recording */
TEST(BridgeTest, RecordingHoldsEveryValueCoreDecodes)
{
    const QString uri =
        QStringLiteral("replay://%1/2026-10-25-engine-start?profile=obd2&speed=100")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    constexpr int kFrames = 7074;

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kFrames;
        },
        20000));
    spinLoop(300);   // the last frames' values staged, a tick or more
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray csv = readFile(root.path() + QStringLiteral("/") +
                                    folders.first() + QStringLiteral("/signals.csv"));
    std::vector<std::pair<QByteArray, QByteArray>> recorded;
    const QList<QByteArray> lines = csv.split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i) {   // past the header
        const QList<QByteArray> cols = lines[i].split(',');
        if (cols.size() == 4)
            recorded.emplace_back(cols[1], cols[2]);
    }

    ecuio_session_t* session = nullptr;
    const QByteArray utf8 = uri.toUtf8();
    ASSERT_EQ(ecuio_session_create(utf8.constData(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    CoreRows core;
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    ASSERT_NE(set, nullptr);
    for (uint32_t i = 0; i < set->count; ++i) {
        core.names.insert(set->defs[i].id,
                          QByteArray(set->defs[i].name,
                                     qstrnlen(set->defs[i].name,
                                              sizeof set->defs[i].name)));
    }
    EXPECT_EQ(ecuio_register_frame_callback(session, core_rows_frame_cb, &core),
              ECUIO_OK);
    EXPECT_EQ(ecuio_register_values_callback(session, core_rows_values_cb, &core),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_start(session), ECUIO_OK);
    const bool reached =
        spinUntil([&core]() { return core.frames.load() >= kFrames; }, 20000);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    ASSERT_TRUE(reached) << core.frames.load() << " frames";

    ASSERT_GT(recorded.size(), 1000u);
    ASSERT_EQ(core.rows.size(), recorded.size());
    for (size_t i = 0; i < recorded.size(); ++i) {
        ASSERT_EQ(core.rows[i], recorded[i])
            << "row " << i << ": core " << core.rows[i].first.toStdString()
            << " " << core.rows[i].second.toStdString() << ", bridge "
            << recorded[i].first.toStdString() << " "
            << recorded[i].second.toStdString();
    }
}

namespace {

/* n rows of 0x7E8 RPM replies, 10 ms of session time apart; row i reads
 * 1000 + i rpm (41 0C, a quarter rpm a count) */
QString writeRpmRows(const QString& folder, int n)
{
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < n; ++i) {
        const QByteArray t = QByteArray::number(qint64{10000} * i);
        const int raw = (1000 + i) * 4;
        text += t + ',' + t + ",0x7E8,0x0,41 0C " +
                QByteArray::number(raw >> 8, 16).rightJustified(2, '0').toUpper() + ' ' +
                QByteArray::number(raw & 0xFF, 16).rightJustified(2, '0').toUpper() + '\n';
    }
    QFile f(folder + QStringLiteral("/frames.csv"));
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(text);
    return QStringLiteral("replay://%1?profile=obd2&speed=max").arg(folder);
}

/* signals.csv's RPM values, in order */
std::vector<double> recordedRpm(const QString& root)
{
    std::vector<double> out;
    const QStringList folders = recordingFolders(root);
    if (folders.size() != 1)
        return out;
    const QByteArray csv = readFile(root + QStringLiteral("/") + folders.first() +
                                    QStringLiteral("/signals.csv"));
    for (const QByteArray& line : csv.split('\n')) {
        const QList<QByteArray> cols = line.split(',');
        if (cols.size() == 4 && cols[1] == "RPM")
            out.push_back(cols[2].toDouble());
    }
    return out;
}

} // namespace

/* Values come from core's worker, not from the frames the GUI thread
 * handles, so a frame dropped at the in-flight cap keeps its values
 * (ADR-0024 §7): with the event loop held while a replay floods in,
 * thousands of frames drop, yet the recording holds every row's RPM in
 * order and the row shows the last one */
TEST(BridgeTest, ValuesSurviveFramesDroppedAtTheCap)
{
    constexpr int kRows = 5000;
    QTemporaryDir recording, root;
    ASSERT_TRUE(recording.isValid() && root.isValid());
    const QString uri = writeRpmRows(recording.path(), kRows);
    ASSERT_FALSE(uri.isEmpty());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // No event processing: the queued frames pile up to the cap
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kRows;
        },
        10000));
    EXPECT_GT(bridge.droppedFrames(), 1000) << "the cap dropped frames";
    spinLoop(300);
    SignalListModel* model = bridge.signalModel();
    int rpmRow = -1;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r, 0), SignalListModel::NameRole).toString() ==
            QStringLiteral("RPM"))
            rpmRow = r;
    }
    ASSERT_GE(rpmRow, 0);
    EXPECT_EQ(model->data(model->index(rpmRow, 0), SignalListModel::ValueRole).toDouble(),
              1000.0 + kRows - 1)
        << "the last row's value, though its frame may have dropped";
    ASSERT_TRUE(disconnectAndWait(bridge));
    const std::vector<double> rpm = recordedRpm(root.path());
    ASSERT_EQ(rpm.size(), static_cast<size_t>(kRows)) << "every row's value recorded";
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(rpm[static_cast<size_t>(i)], 1000.0 + i) << "row " << i;
    }
}

/* A disconnect drains the window it ends: a value staged just before
 * it, with no tick between, reaches the recording */
TEST(BridgeTest, ValuesStagedAtDisconnectAreRecorded)
{
    static ecuio_signal_def_t defs[1] = {};
    defs[0] = {};
    defs[0].id = 1;
    defs[0].kind = ECUIO_VALUE_UINT;
    defs[0].message_id = 0x123u;   /* not the mock's 0x0C0 */
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    strncpy(defs[0].name, "Last", sizeof(defs[0].name) - 1);
    static ecuio_signal_set_t set = {};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = defs;
    ASSERT_EQ(ecuio_profile_register("test-last-window", &set, nullptr), ECUIO_OK);

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://last?profile=test-last-window").isEmpty());
    spinLoop(150);
    ecuio_value_t value = {};
    value.signal_id = 1;
    value.kind = ECUIO_VALUE_UINT;
    value.value = 42.0;
    ecuio_value_batch_t batch = {};
    batch.arbitration_id = 0x123;
    batch.count = 1;
    batch.values = &value;
    bridge.dispatchValuesForTesting(batch);
    ASSERT_TRUE(disconnectAndWait(bridge));   // no tick since the value

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray csv = readFile(root.path() + QStringLiteral("/") + folders.first() +
                                    QStringLiteral("/signals.csv"));
    EXPECT_TRUE(csv.contains(",Last,42,")) << csv.toStdString();
    EXPECT_EQ(ecuio_profile_unregister("test-last-window"), ECUIO_OK);
}

/* One window holds a bounded number of samples, so the worker never
 * allocates: past the limit, the recording misses them, the activity
 * log says so once and gives the count at disconnect, and the row still
 * shows the newest value */
TEST(BridgeTest, ValuesPastTheWindowsLimitAreCountedAndLogged)
{
    constexpr int kRows = 3000;
    constexpr int kLimit = 100;
    QTemporaryDir recording, root;
    ASSERT_TRUE(recording.isValid() && root.isValid());
    const QString uri = writeRpmRows(recording.path(), kRows);
    ASSERT_FALSE(uri.isEmpty());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    // A replay, compared with its source
    bridge.setRecordReplaysForTesting(true);
    bridge.setValueSampleLimitForTesting(kLimit);
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // Held, so the whole replay lands in one window
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.frameCount() + bridge.droppedFrames() >= kRows;
        },
        10000));
    spinLoop(300);
    SignalListModel* model = bridge.signalModel();
    double shown = 0.0;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r, 0), SignalListModel::NameRole).toString() ==
            QStringLiteral("RPM"))
            shown = model->data(model->index(r, 0), SignalListModel::ValueRole).toDouble();
    }
    EXPECT_EQ(shown, 1000.0 + kRows - 1) << "the newest value, past the limit";
    ASSERT_TRUE(disconnectAndWait(bridge));
    const std::vector<double> rpm = recordedRpm(root.path());
    EXPECT_LT(rpm.size(), static_cast<size_t>(kRows));
    EXPECT_GE(rpm.size(), static_cast<size_t>(kLimit));
    int warned = 0;
    QString total;
    for (const QString& line : log) {
        if (line.startsWith(QStringLiteral("Values are arriving faster than 100 in 100 ms")))
            ++warned;
        if (line.startsWith(QStringLiteral("Values the recording missed: ")))
            total = line;
    }
    EXPECT_EQ(warned, 1);
    const qint64 missed = static_cast<qint64>(kRows) - static_cast<qint64>(rpm.size());
    EXPECT_EQ(total, QStringLiteral("Values the recording missed: %1, past 100 in one "
                                    "100 ms window").arg(missed))
        << total.toStdString();
}

namespace {

/* A recording's signals.csv rows past the header, each without its
 * host_us column: signal, value, unit */
QList<QByteArray> signalRows(const QString& folder)
{
    QList<QByteArray> out;
    const QList<QByteArray> lines = readFile(folder + QStringLiteral("/signals.csv")).split('\n');
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype comma = line.indexOf(',');
        if (comma > 0)
            out << line.mid(comma + 1);
    }
    return out;
}

/* The replay URI that decodes a recording as it was decoded live: the
 * recorded URI's profile and DIDs, or its scheme's default profile */
QString replayUriFor(const QString& folder)
{
    QString uri;
    for (const QByteArray& line : readFile(folder + QStringLiteral("/events.log")).split('\n')) {
        if (line.startsWith("uri ")) {
            uri = QString::fromUtf8(line.mid(4).trimmed());
            break;
        }
    }
    if (uri.isEmpty())
        return {};
    QString profile = uri.startsWith(QStringLiteral("mock://")) ? QStringLiteral("mock")
                                                                : QStringLiteral("obd2");
    QStringList keep;
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        for (const QString& pair : uri.mid(q + 1).split(QLatin1Char('&'))) {
            if (pair.startsWith(QStringLiteral("profile=")))
                profile = pair.mid(8);
            else if (pair.startsWith(QStringLiteral("did=")))
                keep << pair;
        }
    }
    keep.prepend(QStringLiteral("profile=") + profile);
    return QStringLiteral("replay://%1?%2&speed=max").arg(folder, keep.join(QLatin1Char('&')));
}

} // namespace

/* Local only: ECUIO_BENCH_RAW names a folder of real-car recordings
 * (Testing/bench-raw, kept out of the repository by the bench-session
 * rules). Each recording's frames.csv, replayed through the bridge
 * under the profile its URI chose, records the values its own
 * signals.csv holds, row for row: the decode on the worker against the
 * one that ran on the GUI thread in the car. A mismatch names the
 * recording, the row and the signal, never a value, which can be a
 * vehicle's identity. */
TEST(BridgeTest, VehicleRecordingsReplayToTheirOwnValues)
{
    const QString root = qEnvironmentVariable("ECUIO_BENCH_RAW");
    if (root.isEmpty())
        GTEST_SKIP() << "set ECUIO_BENCH_RAW to a folder of recordings";
    QStringList folders;
    QDirIterator it(root, {QStringLiteral("frames.csv")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = QFileInfo(it.next()).absolutePath();
        if (QFile::exists(dir + QStringLiteral("/signals.csv")) &&
            QFile::exists(dir + QStringLiteral("/events.log")))
            folders << dir;
    }
    folders.sort();
    ASSERT_FALSE(folders.isEmpty()) << "no recordings under ECUIO_BENCH_RAW";

    qsizetype compared = 0;
    for (const QString& folder : folders) {
        const QString name = QDir(folder).dirName();
        SCOPED_TRACE(name.toStdString());
        const QString uri = replayUriFor(folder);
        ASSERT_FALSE(uri.isEmpty()) << "no uri line in events.log";
        const QList<QByteArray> frames = readFile(folder + QStringLiteral("/frames.csv")).split('\n');
        int rows = 0;
        for (qsizetype i = 1; i < frames.size(); ++i) {
            if (!frames[i].trimmed().isEmpty())
                ++rows;
        }
        if (rows == 0)
            continue;   // a connect that heard nothing; replay refuses it

        QTemporaryDir out;
        ASSERT_TRUE(out.isValid());
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(out.path());
        bridge.setRecording(true);
        // A replay, compared with its source
        bridge.setRecordReplaysForTesting(true);
        bridge.setValueSampleLimitForTesting(1 << 20);
        const QString err = bridge.connectSession(uri);
        if (!err.isEmpty()) {
            ADD_FAILURE() << "replay refused: " << err.toStdString();
            continue;
        }
        EXPECT_TRUE(spinUntil(
            [&]() { return bridge.frameCount() + bridge.droppedFrames() >= rows; },
            30000))
            << bridge.frameCount() + bridge.droppedFrames() << " of " << rows << " frames";
        spinLoop(300);
        ASSERT_TRUE(disconnectAndWait(bridge));
        const QStringList made = recordingFolders(out.path());
        ASSERT_EQ(made.size(), 1);

        const QList<QByteArray> live = signalRows(folder);
        const QList<QByteArray> replayed = signalRows(out.path() + QStringLiteral("/") + made.first());
        EXPECT_EQ(replayed.size(), live.size()) << "rows";
        const qsizetype n = std::min(live.size(), replayed.size());
        for (qsizetype i = 0; i < n; ++i) {
            if (replayed[i] != live[i]) {
                ADD_FAILURE() << "row " << i << ": " << live[i].left(live[i].indexOf(',')).toStdString()
                              << " decodes differently";
                break;
            }
        }
        compared += n;
    }
    EXPECT_GT(compared, 0);
    std::printf("compared %lld rows over %lld recordings\n", static_cast<long long>(compared),
                static_cast<long long>(folders.size()));
}

TEST(BridgeTest, ReplayUriLabelKeepsTheRecordingsName)
{
    const QString dots = QString(QChar(0x2026));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("replay://C:/Users/me/Documents/Havne/"
                                 "recordings/20261025-165100?profile=obd2")),
              QStringLiteral("replay://") + dots +
                  QStringLiteral("/20261025-165100?profile=obd2"));
    EXPECT_EQ(
        SessionBridge::uriLabelFor(QStringLiteral(
            "replay://C:\\Users\\me\\recordings\\20261025-165100\\frames.csv")),
        QStringLiteral("replay://") + dots +
            QStringLiteral("/20261025-165100\\frames.csv"));
    EXPECT_EQ(SessionBridge::uriLabelFor(
                  QStringLiteral("replay:///home/me/rec/20261025-165100/")),
              QStringLiteral("replay://") + dots +
                  QStringLiteral("/20261025-165100"));
    for (const char* uri :
         {"replay://", "replay://?speed=max", "replay://frames.csv"}) {
        EXPECT_EQ(SessionBridge::uriLabelFor(QString::fromLatin1(uri)),
                  QString::fromLatin1(uri));
    }
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
}
