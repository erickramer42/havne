// File/layer: tests/test_bridge.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include <cmath>
#include <gtest/gtest.h>
#include <functional>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

#include "carryover.h"
#include "healthreport.h"
#include "fake_theme.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "urlwrite.h"
#include "windowtheme.h"
#include "new_counter.h" // operator new counted per thread: the conditions callback
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES

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
#include "bridge_support.h"

namespace {

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

} // namespace

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

/* One folder per connect, which core writes (ADR-0049): every frame
 * the session dispatched, the log's Pause notwithstanding, each decoded
 * value, core's header with the app's caller lines, and the session's
 * activity lines from Connected through Disconnected as notes, then
 * core's summary. */
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
    // The worker may dispatch a frame or two more after the bridge
    // stops taking them, as the session closes
    EXPECT_GE(frameRows.size() - 1, frames) << "every frame, paused or not";
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

    // CRLF on Windows (ADR-0017 takes either)
    const QByteArray events =
        readFile(dir + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const char* expected :
         {"ecuio session recording, format 3 (ADR-0049)\n",
          "\nuri  mock://rec\n", "\nsignal  RPM,", "\ncaller  Havne ",
          "\ncaller  surrogate frame logging  ",
          "  note  Connected: mock://rec\n",
          "  note  Disconnecting: mock://rec\n", "  note  Delivery: ",
          "  note  Disconnected: mock://rec\n", "  end  disconnected: "}) {
        EXPECT_TRUE(events.contains(expected)) << expected << "\n" << events.toStdString();
    }
}

/* The app's Mark (ADR-0049 section 4): a marker in markers.csv at the
 * time it's tapped, "Mark N" unless labeled, counted per recording, said
 * in the activity log; nothing to mark without a recording. */
TEST(BridgeTest, AMarkGoesIntoTheRecording)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_FALSE(bridge.mark()) << "no recording";
    EXPECT_EQ(bridge.markCount(), 0);

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://marks").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    EXPECT_TRUE(bridge.mark());
    EXPECT_TRUE(bridge.mark(QStringLiteral("  launch, \"go\"  ")));
    EXPECT_TRUE(bridge.mark(QStringLiteral(" ")));
    EXPECT_EQ(bridge.markCount(), 3);
    EXPECT_TRUE(log.contains(QStringLiteral("Marked: Mark 1")));
    EXPECT_TRUE(log.contains(QStringLiteral("Marked: launch, \"go\"")));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.mark()) << "the recording ended";
    EXPECT_EQ(bridge.markCount(), 3) << "the last recording's";

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    const QList<QByteArray> rows =
        fileLines(dir + QStringLiteral("/markers.csv"));
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0], QByteArray("host_us,label"));
    EXPECT_TRUE(rows[1].endsWith(",Mark 1")) << rows[1].toStdString();
    EXPECT_TRUE(rows[2].endsWith(",\"launch, \"\"go\"\"\""))
        << rows[2].toStdString();
    EXPECT_TRUE(rows[3].endsWith(",Mark 3")) << rows[3].toStdString();
    EXPECT_LE(rows[1].split(',')[0].toLongLong(),
              rows[3].split(',')[0].toLongLong());
    const QByteArray events = readFile(dir + QStringLiteral("/events.log"));
    EXPECT_FALSE(events.contains("Marked:"))
        << "markers.csv holds them, not notes";

    // A new recording counts from one
    ASSERT_TRUE(bridge.connectSession("mock://marks-again").isEmpty());
    EXPECT_EQ(bridge.markCount(), 0);
    EXPECT_TRUE(bridge.mark());
    EXPECT_EQ(log.count(QStringLiteral("Marked: Mark 1")), 2);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* A folder's files' sizes summed */
qint64 folderBytes(const QString& folder)
{
    qint64 bytes = 0;
    for (const QFileInfo& f :
         QDir(folder).entryInfoList(QDir::Files | QDir::Hidden)) {
        bytes += f.size();
    }
    return bytes;
}

} // namespace

/* Start log is refused, with its reason in the activity log and no
 * folder made: while not connected, while Record sessions' recorder is
 * live (one recorder a session), with no row plotted, and on a replay,
 * which plays a recording already. */
TEST(BridgeTest, StartLogIsRefusedWithItsReason)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(QStringLiteral("Log not started: not connected")))
        << log.join('\n').toStdString();

    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://recorded").isEmpty());
    ASSERT_TRUE(bridge.recordingActive());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: Recording this session already")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    EXPECT_EQ(recordingFolders(root.path()).size(), 1)
        << "Record sessions' alone";

    ASSERT_TRUE(bridge.connectSession("mock://unticked").isEmpty());
    plotOnly(bridge.signalModel(), {});
    ASSERT_EQ(bridge.signalModel()->subscribedCount(), 0);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: no signal is ticked on Live")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);

    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("replay://%1/2026-10-25-engine-start"
                                           "?profile=obd2&speed=100")
                                .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES)))
            .isEmpty());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: a replay plays a recording already")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);
}

/* An attach the session refuses, with a folder core can't give whole:
 * the bridge removes nothing, so the working folder (QDir("") is it) is
 * left alone. The working folder is a scratch one of the case's own, so
 * a mutant without the guard empties only that. */
TEST(BridgeTest, ARecorderFolderCoreCantGiveRemovesNothing)
{
    QTemporaryDir root;
    QTemporaryDir own;
    QTemporaryDir work;
    ASSERT_TRUE(root.isValid() && own.isValid() && work.isValid());
    {
        QFile kept(work.filePath(QStringLiteral("kept.txt")));
        ASSERT_TRUE(kept.open(QIODevice::WriteOnly));
        kept.write("the user's");
    }
    struct Cwd {
        QString was = QDir::currentPath();
        ~Cwd() { QDir::setCurrent(was); }
    } cwd;
    ASSERT_TRUE(QDir::setCurrent(work.path()));

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    ASSERT_TRUE(bridge.connectSession("mock://cut").isEmpty());
    const QByteArray own8 = own.path().toUtf8();
    ecuio_record_options_t o{};
    o.root_utf8 = own8.constData();
    ecuio_recorder_t* holder = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &holder), ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(bridge.sessionForTesting(), holder),
              ECUIO_OK);

    bridge.failRecorderFolderForTesting(true);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(QFileInfo::exists(work.filePath(QStringLiteral("kept.txt"))))
        << "the working folder's file is still there";
    EXPECT_EQ(recordingFolders(root.path()).size(), 1)
        << "the refused log's folder stays: its name couldn't be read";
    (void)ecuio_recorder_close(holder, "the test's");
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A start the session refuses after its focus took (a recorder of the
 * test's own holds the session's one slot): the whole set is asked
 * again, and the folder the start made, holding no frame, is gone. */
TEST(BridgeTest, AStartLogTheSessionRefusesLeavesNoFolderAndNoFocus)
{
    QTemporaryDir root;
    QTemporaryDir own;
    ASSERT_TRUE(root.isValid() && own.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession("mock://refused").isEmpty());
    ASSERT_GT(bridge.signalModel()->subscribedCount(), 0);

    const QByteArray own8 = own.path().toUtf8();
    const auto attach = [&bridge, &own8]() {
        ecuio_record_options_t o{};
        o.root_utf8 = own8.constData();
        ecuio_recorder_t* r = nullptr;
        EXPECT_EQ(ecuio_recorder_open(&o, &r), ECUIO_OK);
        EXPECT_EQ(ecuio_session_record(bridge.sessionForTesting(), r),
                  ECUIO_OK);
        return r;
    };
    ecuio_recorder_t* holder = attach();
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(QStringLiteral("Log not started: %1")
                                 .arg(QString::fromUtf8(ecuio_status_string(
                                     ECUIO_ERR_INVALID_ARGUMENT)))))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty())
        << recordingFolders(root.path()).join(',').toStdString();
    (void)ecuio_recorder_close(holder, "the test's");

    // A recorder attached now writes the focus it finds: none
    ecuio_recorder_t* probe = attach();
    char folder[4096];
    uint32_t length = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(probe, folder, sizeof folder, &length),
              ECUIO_OK);
    (void)ecuio_recorder_close(probe, "the test's");
    const QByteArray events =
        readFile(QString::fromUtf8(folder) + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.contains("\nuri  mock://refused"))
        << events.toStdString();
    EXPECT_FALSE(events.contains("\nfocus  ")) << events.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A write that fails during a log ends it as it ends a Record sessions
 * recording: "Recording stopped: ..." in the activity log, logStopped
 * not the user's, the session running on. */
TEST(BridgeTest, AFailedWriteEndsALogNotAsTheUsers)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QList<bool> byUser;
    QObject::connect(&bridge, &SessionBridge::logStopped,
                     [&byUser](const QString&, bool user) { byUser << user; });
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_EQ(byUser, QList<bool>{false});
    EXPECT_TRUE(
        log.contains(QStringLiteral("Recording stopped: the disk is full")))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(bridge.connected());
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(byUser, QList<bool>{false}) << "once";
}

/* The Dashboard header's notice: a refusal's "Log not started: <why>"
 * (startLog's own reasons and the recorder's), an end the user didn't
 * ask for as "Log ended: <why>", the user's Disconnect as "Log ended"
 * (the recording's end note keeps "disconnected"), nothing for Stop
 * log's, a log that starts clearing it, Dismiss clearing it, and a
 * change signal for each change alone. */
TEST(BridgeTest, TheLogNoticeTellsRefusalsAndUnaskedEnds)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::logNoticeChanged,
                     [&changes]() { ++changes; });

    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_EQ(bridge.logNotice(),
              QStringLiteral("Log not started: not connected"));
    EXPECT_EQ(changes, 1);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_EQ(changes, 1) << "the same text again is no change";
    bridge.clearLogNotice();
    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_EQ(changes, 2);
    bridge.clearLogNotice();
    EXPECT_EQ(changes, 2) << "clearing nothing is no change";

    // The recorder's refusal, the root a file: openRecorder's reason
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    bridge.setRecordingsRootForTesting(file);
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(bridge.logNotice().startsWith(
        QStringLiteral("Log not started: can't create ")))
        << bridge.logNotice().toStdString();
    EXPECT_TRUE(log.contains(bridge.logNotice()))
        << "the activity log's own line: " << log.join('\n').toStdString();
    EXPECT_EQ(
        log.filter(QStringLiteral("Log not started: can't create ")).size(), 1)
        << "once, not twice";
    bridge.setRecordingsRootForTesting(root.path());

    // A log that starts clears it; a failed write ends it, told
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_EQ(bridge.logNotice(), QString());
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_EQ(bridge.logNotice(), QStringLiteral("Log ended: the disk is full"))
        << "the recorder's own words, not the end note's";

    // Stop log's end says nothing: the viewer opens on it
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_EQ(bridge.logNotice(), QString());
    const int before = changes;
    bridge.stopLog();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_EQ(bridge.logNotice(), QString());
    EXPECT_EQ(changes, before);

    // The user's Disconnect closes a running log: "Log ended" on the
    // Dashboard, "disconnected" in the recording's end note
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    const QString folder = bridge.recordingFolder();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.logNotice(), QStringLiteral("Log ended"));
    EXPECT_TRUE(readFile(folder + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
}

/* A connect started from disconnectingChanged's end (as a QML handler
 * may) finds the last session's recording closed: that one ends with
 * the teardown, and the new session's runs on. */
TEST(BridgeTest, AConnectAtTheTeardownsEndFindsTheRecordingClosed)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    const QString first = bridge.recordingFolderUrl().toLocalFile();
    ASSERT_FALSE(first.isEmpty());
    bool again = false;
    QObject::connect(
        &bridge, &SessionBridge::disconnectingChanged, &bridge,
        [&bridge, &again]() {
            if (!bridge.disconnecting() && !again) {
                again = true;
                EXPECT_TRUE(bridge.connectSession("mock://second").isEmpty());
            }
        });
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(again);
    EXPECT_TRUE(bridge.connected());
    EXPECT_TRUE(bridge.recordingActive());
    const QString second = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_NE(second, first);
    EXPECT_TRUE(readFile(first + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
    EXPECT_FALSE(
        readFile(second + QStringLiteral("/events.log")).contains("  end  "));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(readFile(second + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
}

/* ADR-0056 section 4: Settings' minimum time takes only its choices,
 * and on a session that asks nothing (the mock), where it changes
 * nothing, neither the log's line nor its header name it; a session
 * that asks is ALogOnARequestingSessionNamesItsMinimumTime */
TEST(BridgeTest, ALogTakesItsMinimumTimeBetweenSweeps)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    EXPECT_EQ(bridge.minSweepMs(), 0);
    EXPECT_EQ(bridge.minSweepChoices(), (QVariantList{0, 100, 250, 500, 1000}));
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::minSweepMsChanged,
                     [&changes]() { ++changes; });
    bridge.setMinSweepMs(250);
    EXPECT_EQ(bridge.minSweepMs(), 250);
    EXPECT_EQ(changes, 1);
    bridge.setMinSweepMs(251);
    bridge.setMinSweepMs(-100);
    bridge.setMinSweepMs(250);
    EXPECT_EQ(bridge.minSweepMs(), 250) << "anything but a choice is ignored";
    EXPECT_EQ(changes, 1);

    ASSERT_TRUE(bridge.connectSession("mock://paced").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);

    const auto header = [](const QString& folder) {
        QFile f(folder + QStringLiteral("/events.log"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                           : QString();
    };
    const auto stoppedFolder = [&bridge]() {
        return bridge.recordingFolderUrl().toLocalFile();
    };

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    QString folder = stoppedFolder();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_FALSE(header(folder).contains(QLatin1String("min sweep")))
        << header(folder).toStdString();

    bridge.setMinSweepMs(0);
    log.clear();
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    folder = stoppedFolder();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    EXPECT_FALSE(header(folder).contains(QLatin1String("min sweep")))
        << header(folder).toStdString();
    disconnectAndWait(bridge);
}

/* A log on a running session (ADR-0056 section 1): its own folder, the
 * plotted rows its focus line, a readout that counts the folder's bytes
 * and its time, Stop log's folder in logStopped as the user's; a second
 * log in the same session, which the session's end closes and reports
 * with its folder, not as the user's. */
TEST(BridgeTest, ALogRecordsTheRunningSessionUntilStopped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QStringList stopped;
    QList<bool> byUser;
    QObject::connect(&bridge, &SessionBridge::logStopped,
                     [&stopped, &byUser](const QString& folder, bool user) {
                         stopped << folder;
                         byUser << user;
                     });

    ASSERT_TRUE(bridge.connectSession("mock://logged").isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);
    ASSERT_EQ(model->subscribedCount(), 2);

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(bridge.logActive());
    EXPECT_TRUE(bridge.recordingActive());
    const QString first = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_TRUE(log.contains(QStringLiteral("Log started: %1 (2 signals)")
                                 .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    EXPECT_FALSE(bridge.startLog());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Log not started: a log is running already")));

    // The readout's second: the folder's bytes and the time so far
    ASSERT_TRUE(spinUntil(
        [&bridge]() {
            return bridge.logBytes() > 0 && bridge.logElapsedMs() >= 1000;
        },
        10000))
        << bridge.logBytes() << " bytes at " << bridge.logElapsedMs();
    EXPECT_LE(bridge.logBytes(), folderBytes(first));

    bridge.stopLog();
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    ASSERT_EQ(stopped, QStringList{first});
    EXPECT_EQ(byUser, QList<bool>{true}) << "Stop log's own";
    EXPECT_EQ(bridge.logBytes(), folderBytes(first))
        << "the closed folder's size";
    const qint64 elapsed = bridge.logElapsedMs();
    EXPECT_GE(elapsed, 1000);
    spinLoop(50);
    EXPECT_EQ(bridge.logElapsedMs(), elapsed) << "the last log's, held";
    const QByteArray events =
        readFile(first + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const QString& expected :
         {QStringLiteral("\nfocus  ") + namesOf(model, picks).join(',') +
              QStringLiteral("\n"),
          QStringLiteral("  note  Log stopped: "),
          QStringLiteral("  end  Stop log: ")}) {
        EXPECT_TRUE(events.contains(expected.toUtf8()))
            << expected.toStdString() << "\n"
            << events.toStdString();
    }

    // The slot is free again, and the session's end closes the next
    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    const QString second = bridge.recordingFolderUrl().toLocalFile();
    EXPECT_NE(second, first);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.logActive());
    ASSERT_EQ(stopped, (QStringList{first, second}));
    EXPECT_EQ(byUser, (QList<bool>{true, false}))
        << "the session's end isn't the user's";
    EXPECT_TRUE(readFile(second + QStringLiteral("/events.log"))
                    .contains("  end  disconnected: "));
    EXPECT_EQ(recordingFolders(root.path()).size(), 2);
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
    EXPECT_TRUE(events.contains("  end  the app closed: "))
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

/* A write that fails (a full disk) stops the recording, never the
 * session: the activity log says why, once, with no path, the folder is
 * no longer active, and frames keep coming. */
TEST(BridgeTest, AFailedWriteStopsTheRecordingAndTheSessionRuns)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession("mock://failing").isEmpty());
    ASSERT_TRUE(bridge.recordingActive());
    ASSERT_NE(bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(bridge.recorderForTesting());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return !bridge.recordingActive(); }, 5000));
    const QStringList stopped =
        log.filter(QStringLiteral("Recording stopped: "));
    ASSERT_EQ(stopped.size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(stopped.first(),
              QStringLiteral("Recording stopped: the disk is full"));
    EXPECT_TRUE(bridge.connected());
    const int before = bridge.frameCount();
    ASSERT_TRUE(spinUntil(
        [&bridge, before]() { return bridge.frameCount() > before + 2; }, 5000))
        << "the session runs on";
    EXPECT_EQ(log.filter(QStringLiteral("Recording stopped: ")).size(), 1)
        << "once";
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
// Strip chart: per-signal rolling history ring in SignalListModel
// ---------------------------------------------------------------------------

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
    /* A text row's tick asks it (the default pick and a log take it) and
     * never plots: the count and the chart leave it out */
    int ticks = 0;
    QObject::connect(&model, &SignalListModel::ticksChanged,
                     [&ticks]() { ++ticks; });
    model.toggleSubscribed(1);
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.subscribedCount(), 1);
    EXPECT_EQ(model.tickedCount(), 2) << "a log counts the ticked text row";
    const QString textName =
        model.data(row1, SignalListModel::NameRole).toString();
    EXPECT_EQ(model.plottedNames(), QStringList{textName});
    EXPECT_EQ(model.tickedIds(), std::vector<uint16_t>{2001});
    EXPECT_EQ(model.plottedIds(), (std::vector<uint16_t>{1001, 2001}));
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

    /* A vehicle's choices tick it again; an untick is never kept for it,
     * its default being unticked */
    model.toggleSubscribed(1);
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_TRUE(model.tickedIds().empty());
    model.setPlotChoices({textName}, {});
    EXPECT_EQ(ticks, 3);
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    model.setPlotChoices({}, {textName});
    EXPECT_FALSE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_TRUE(model.unplottedNames().isEmpty());
    // A name in both lists is unplotted, a text row's never: it stays ticked
    model.setPlotChoices({textName}, {textName});
    EXPECT_TRUE(model.data(row1, SignalListModel::SubscribedRole).toBool());
    EXPECT_EQ(model.tickedCount(), 2);
}

/* A set with no rows (raw, or none) ticks nothing: Start log goes by it */
TEST(BridgeTest, AnEmptySetLeavesNoRowTicked)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    ASSERT_GT(model.tickedCount(), 0);
    int changes = 0;
    QObject::connect(&model, &SignalListModel::tickedCountChanged,
                     [&changes]() { ++changes; });
    model.setSignalSet(nullptr);
    EXPECT_EQ(model.tickedCount(), 0);
    EXPECT_EQ(changes, 1);

    model.setSignalSet(&set);
    ASSERT_GT(model.tickedCount(), 0);
    ecuio_signal_set_t none = set;
    none.count = 0;
    model.setSignalSet(&none);
    EXPECT_EQ(model.tickedCount(), 0);
}

/* The rows the focus leaves unasked (ecuio_session_get_unasked_signals)
 * say so, and Live counts the ones it shows; a new set asks every row */
TEST(BridgeTest, AnUnaskedRowSaysSoAndLiveCountsTheOnesItShows)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    int counted = 0;
    QObject::connect(&model, &SignalListModel::unaskedCountChanged,
                     [&counted]() { ++counted; });
    const auto rowOf = [&model](uint16_t id) {
        for (int r = 0; r < model.rowCount(); ++r) {
            if (model.data(model.index(r), SignalListModel::IdRole).toUInt() ==
                id) {
                return model.index(r);
            }
        }
        return QModelIndex();
    };
    const QModelIndex throttle = rowOf(ECUIO_OBD2_SIG_THROTTLE_ID);
    const QModelIndex fuel = rowOf(ECUIO_OBD2_SIG_FUEL_LEVEL_ID);
    const QModelIndex rpm = rowOf(ECUIO_OBD2_SIG_RPM_ID);
    ASSERT_TRUE(throttle.isValid() && fuel.isValid() && rpm.isValid());
    EXPECT_FALSE(model.data(throttle, SignalListModel::UnaskedRole).toBool());

    model.applyUnasked(
        {ECUIO_OBD2_SIG_THROTTLE_ID, ECUIO_OBD2_SIG_FUEL_LEVEL_ID});
    EXPECT_TRUE(model.data(throttle, SignalListModel::UnaskedRole).toBool());
    EXPECT_TRUE(model.data(fuel, SignalListModel::UnaskedRole).toBool());
    EXPECT_FALSE(model.data(rpm, SignalListModel::UnaskedRole).toBool());
    EXPECT_EQ(model.unaskedCount(), 2);
    EXPECT_EQ(counted, 1);

    /* A hidden row isn't Live's to count */
    model.applyUnsupported({ECUIO_OBD2_SIG_FUEL_LEVEL_ID});
    EXPECT_EQ(model.unaskedCount(), 1);

    /* Each call judges afresh */
    model.applyUnasked({ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_FALSE(model.data(throttle, SignalListModel::UnaskedRole).toBool());
    EXPECT_TRUE(model.data(rpm, SignalListModel::UnaskedRole).toBool());
    EXPECT_EQ(model.unaskedCount(), 1);

    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.unaskedCount(), 0);
    EXPECT_FALSE(
        model.data(rowOf(ECUIO_OBD2_SIG_RPM_ID), SignalListModel::UnaskedRole)
            .toBool());
}

/* The gauges the default pick adds: each cell's first ref the set has,
 * found as SignalHub finds it (a key def that took a base def's place
 * answers to the base's id: too), each once */
TEST(BridgeTest, AGaugesPickIsItsCellsFirstRefTheSetHas)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const auto cells = [](std::initializer_list<QStringList> lists) {
        QVariantList out;
        for (const QStringList& refs : lists) {
            out.append(refs);
        }
        return out;
    };
    EXPECT_EQ(SessionBridge::gaugeIds(
                  obd2, nullptr,
                  cells({{QStringLiteral("key:none"), QStringLiteral("id:7006"),
                          QStringLiteral("id:7003")},
                         {},
                         {QStringLiteral("id:99")},
                         {QStringLiteral("id:7003")},
                         {QStringLiteral("id:7006")}})),
              (std::vector<uint16_t>{7006, 7003}));
    EXPECT_TRUE(SessionBridge::gaugeIds(nullptr, nullptr,
                                        cells({{QStringLiteral("id:7003")}}))
                    .empty());

    /* A file's RPM replaces obd2's and keeps its id; its own signal has
     * a key alone */
    const QByteArray text =
        R"({"havne_signals":1,"name":"Pick","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"rpm","name":"RPM","read":{"service":"22","number":"1E1C"},)"
        R"("field":{"byte":0,"bits":16}},)"
        R"({"key":"own","name":"Own","read":{"service":"22","number":"1E1D"},)"
        R"("field":{"byte":0,"bits":8}}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    const ecuio_signal_def_t* own = ecuio_signal_file_find(file, "own");
    ASSERT_NE(own, nullptr);
    EXPECT_EQ(SessionBridge::gaugeIds(set, file,
                                      cells({{QStringLiteral("id:7003")},
                                             {QStringLiteral("key:own")}})),
              (std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID, own->id}));
    EXPECT_EQ(SessionBridge::gaugeIds(set, file,
                                      cells({{QStringLiteral("key:rpm")}})),
              std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID});
    EXPECT_TRUE(SessionBridge::gaugeIds(
                    set, file, cells({{QStringLiteral("id:%1").arg(own->id)}}))
                    .empty())
        << "a file's own signal answers to its key alone";
    ecuio_signal_file_free(file);
}

/* A session that asks nothing (the mock, under a profile that ranks its
 * signals too) takes no default pick: no line, no row unasked, and its
 * recording names no pick */
TEST(BridgeTest, ASessionThatAsksNothingTakesNoPick)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.setPickGauges({QVariant(QStringList{QStringLiteral("id:7006")})});
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=obd2"))
            .isEmpty());
    bridge.signalModel()->toggleSubscribed(0);
    spinLoop(400);
    EXPECT_TRUE(
        log.filter(QRegularExpression(QStringLiteral("^Asking "))).isEmpty())
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.signalModel()->unaskedCount(), 0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QLatin1Char('/') + folders[0] +
                 QStringLiteral("/events.log"));
    EXPECT_FALSE(events.contains("\npick  ")) << events.toStdString();
}

/* An enum row (ADR-0051) shows its label for the newest raw value, and
 * a raw value with no label its number; the label is notified with the
 * value, and a row of another kind has none */
TEST(BridgeTest, AnEnumRowShowsItsLabel)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    static const ecuio_enum_entry_t kGears[] = {
        {0, "Park"}, {3, "Drive"}, {7, "Low"}};
    defs[1].kind = ECUIO_VALUE_ENUM; /* row 1, id 2001 */
    defs[1].table_first = 1;         /* Drive and Low */
    defs[1].table_count = 2;
    set.enums = kGears;
    set.enum_count = 3;
    model.setSignalSet(&set);

    const QModelIndex row0 = model.index(0, 0);
    const QModelIndex row1 = model.index(1, 0);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a label before any value";
    QList<int> notified;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&notified](const QModelIndex& topLeft, const QModelIndex&,
                                 const QList<int>& roles) {
                         if (topLeft.row() <= 1)
                             notified = roles;
                     });
    model.updateValues({{uint16_t(2001), 3.0}, {uint16_t(1001), 3.0}}, 10);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Drive"));
    EXPECT_TRUE(notified.contains(SignalListModel::LabelRole));
    EXPECT_EQ(model.data(row0, SignalListModel::LabelRole).toString(),
              QString())
        << "a number row has no label";
    model.updateValues({{uint16_t(2001), 7.0}}, 11);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QStringLiteral("Low"));
    /* Park is the set's, but not the def's: past its table_first */
    model.updateValues({{uint16_t(2001), 0.0}}, 12);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString());
    model.updateValues({{uint16_t(2001), 5.0}}, 13);
    EXPECT_EQ(model.data(row1, SignalListModel::LabelRole).toString(),
              QString())
        << "a raw value with no label shows its number";
    EXPECT_DOUBLE_EQ(model.data(row1, SignalListModel::ValueRole).toDouble(),
                     5.0);
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
    EXPECT_EQ(roles.value(SignalListModel::RefusedRole), QByteArray("refused"));
    EXPECT_EQ(roles.value(SignalListModel::RefusedCodeRole),
              QByteArray("refusedCode"));
    EXPECT_EQ(roles.value(SignalListModel::RefRole), QByteArray("ref"));
    EXPECT_EQ(roles.value(SignalListModel::RangeLowRole),
              QByteArray("rangeLow"));
    EXPECT_EQ(roles.value(SignalListModel::RangeHighRole),
              QByteArray("rangeHigh"));
    EXPECT_EQ(roles.value(SignalListModel::IsEnumRole), QByteArray("isEnum"));

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

/* Live's count of the refusals past a report's list: each ECU's, one
 * request said as one */
TEST(BridgeTest, TheRefusalsLiveCantNameAreCounted)
{
    EXPECT_EQ(SessionBridge::refusedUnnamedText({}), QString());
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 3}}),
              QStringLiteral("ECU 0x7E8 refused 3 more requests"));
    EXPECT_EQ(SessionBridge::refusedUnnamedText({{0x7E8, 1}, {0x7E9, 2}}),
              QStringLiteral("ECU 0x7E8 refused 1 more request; "
                             "ECU 0x7E9 refused 2 more requests"));
}

/* ADR-0051 section 3: a refused signal's words name the code, with the
 * reply in frames.csv's form; a code with no name gives the reply alone */
TEST(BridgeTest, ARefusalNamesItsCode)
{
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x31),
              QStringLiteral("Refused: requestOutOfRange (7F 22 31)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x33),
              QStringLiteral("Refused: securityAccessDenied (7F 22 33)"));
    EXPECT_EQ(SessionBridge::refusalText(0x01, 0x12),
              QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"));
    EXPECT_EQ(SessionBridge::refusalText(0x22, 0x99),
              QStringLiteral("Refused: 7F 22 99"));
    EXPECT_EQ(SessionBridge::refusalCode(0x22, 0x31),
              QStringLiteral("7F 22 31"));
}

/* The model takes each refusal by id, judges every row afresh on each
 * call, and a new set refuses none */
TEST(BridgeTest, ARefusedRowSaysSoUntilItIsntRefused)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto refused = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedRole)
            .toString();
    };
    QList<int> changed;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&changed](const QModelIndex& first, const QModelIndex&,
                                const QList<int>& roles) {
                         if (roles.contains(SignalListModel::RefusedRole)) {
                             changed << first.row();
                         }
                     });
    const uint16_t second = static_cast<uint16_t>(defs[1].id);
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()},
         {9999, QStringLiteral("x"), QString()}});
    EXPECT_EQ(refused(0), QString());
    EXPECT_EQ(refused(1), QStringLiteral("Refused: 7F 22 31"));
    EXPECT_EQ(changed, QList<int>{1}) << "only the row that changed";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    EXPECT_EQ(changed, QList<int>{1}) << "the same words change nothing";
    model.applyRefused({});
    EXPECT_EQ(refused(1), QString());
    // The reply's bytes come with the words, for a row too narrow for them
    const auto code = [&model](int row) {
        return model.data(model.index(row, 0), SignalListModel::RefusedCodeRole)
            .toString();
    };
    model.applyRefused({{second, QStringLiteral("Refused: 7F 22 31"),
                         QStringLiteral("7F 22 31")}});
    EXPECT_EQ(code(1), QStringLiteral("7F 22 31"));
    EXPECT_EQ(code(0), QString());
    model.setSignalSet(&set);
    EXPECT_EQ(code(1), QString()) << "a new set's rows carry no code";
    model.applyRefused(
        {{second, QStringLiteral("Refused: 7F 22 31"), QString()}});
    model.setSignalSet(&set);
    EXPECT_EQ(refused(1), QString()) << "a new set refuses none";
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
 * every linear PID J1979 defines, with 01, 41 and 7F, across its six
 * bitmaps, so nothing the set asks is dropped. The fake answers packed
 * requests, so a second line says so, and nothing more follows. */
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
    ASSERT_TRUE(spinUntil([&]() { return ecu_lines().size() >= 2; }, 5000))
        << ecu_lines().join('\n').toStdString();
    spinLoop(300);
    ASSERT_EQ(ecu_lines().size(), 2) << ecu_lines().join('\n').toStdString();
    EXPECT_EQ(ecu_lines().at(0).toStdString(),
              "ECU 0x7E8: supports 95 PIDs; 92 requests, none dropped");
    EXPECT_EQ(ecu_lines().at(1).toStdString(),
              "ECU 0x7E8: supports 95 PIDs; 92 requests, none dropped; "
              "several PIDs per request");

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

/* ADR-0060 in the app: with ?session=03 each module's session has a line
 * of its own, once; the module with none is named with its refusal and
 * read in the default session; the support lines end with the session in
 * force; Disconnect names the module returning to the default session; a
 * recording keeps every one of them as a note */
TEST(BridgeTest, AnExtendedSessionNamesEachModulesSessionOnce)
{
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    /* Paced by its replies, the range's scan spans two ticks or more
     * only when each reply takes a while (well under the 100 ms wait) */
    FakeEnvArm slow("ECUIO_FAKE_REPLY_DELAY_MS", "40");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C,D800-D803,"
                       "7E9:D801&session=03&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString entered =
        QStringLiteral("ECU 0x7E8: reading in the extended session (50 03)");
    const QString refused = QStringLiteral(
        "ECU 0x7E9: refused the extended session (subFunctionNotSupported, "
        "7F 10 12): reading in the default session");
    ASSERT_TRUE(spinUntil(
        [&]() { return log.contains(entered) && log.contains(refused); }, 5000))
        << log.join('\n').toStdString();
    spinLoop(300);
    EXPECT_EQ(log.count(entered), 1) << log.join('\n').toStdString();
    EXPECT_EQ(log.count(refused), 1) << log.join('\n').toStdString();
    const auto support = [&log](const QString& ecu) {
        QStringList out;
        for (const QString& l : log.filter(ecu)) {
            if (l.contains(QStringLiteral(" requests")) ||
                l.contains(QStringLiteral(" request,"))) {
                out << l;
            }
        }
        return out;
    };
    ASSERT_FALSE(support(QStringLiteral("ECU 0x7E8: ")).isEmpty());
    ASSERT_FALSE(support(QStringLiteral("ECU 0x7E9: ")).isEmpty());
    /* the range's steps report after the entry, so a repeat would show */
    EXPECT_GE(log.mid(log.indexOf(entered))
                  .filter(QStringLiteral("ECU 0x7E8: "))
                  .size(),
              3)
        << log.join('\n').toStdString();
    EXPECT_TRUE(support(QStringLiteral("ECU 0x7E8: "))
                    .last()
                    .endsWith(QStringLiteral("; extended session")))
        << support(QStringLiteral("ECU 0x7E8: ")).last().toStdString();
    for (const QString& l : support(QStringLiteral("ECU 0x7E9: "))) {
        EXPECT_TRUE(l.endsWith(QStringLiteral("; default session")))
            << l.toStdString();
    }
    const QString folder = bridge.recordingFolder();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QString returned =
        QStringLiteral("ECU 0x7E8: returning to the default session (10 01)");
    EXPECT_EQ(log.count(returned), 1) << log.join('\n').toStdString();
    EXPECT_FALSE(
        log.join('\n').contains(QStringLiteral("ECU 0x7E9: returning")));
    const QByteArray events =
        readFile(folder + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const QString& line : {entered, refused, returned}) {
        EXPECT_TRUE(events.contains(("  note  " + line + "\n").toUtf8()))
            << line.toStdString() << "\n"
            << events.toStdString();
    }
}

/* Without ?session=03 the same modules give no session line and no
 * session part in their support lines (the module scans two DIDs, so it
 * reports something) */
TEST(BridgeTest, WithoutASessionNoSessionIsNamed)
{
    FakeEnvArm module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C,7E9:D800-D801"
                       "&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(spinUntil(
        [&]() { return !log.filter(QStringLiteral("ECU 0x7E9: ")).isEmpty(); },
        5000))
        << log.join('\n').toStdString();
    spinLoop(300);
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QString all = log.join('\n');
    EXPECT_FALSE(all.contains(QStringLiteral("session (")));
    EXPECT_FALSE(all.contains(QStringLiteral("; extended session")));
    EXPECT_FALSE(all.contains(QStringLiteral("; default session")));
    EXPECT_FALSE(all.contains(QStringLiteral("returning to the default")));
}

/* A second connect from the same bridge names the session again: what was
 * written is per connect */
TEST(BridgeTest, AReconnectNamesTheSessionAgain)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1?profile=uds-generic&did=1E1C"
                                       "&session=03&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    const QString entered =
        QStringLiteral("ECU 0x7E8: reading in the extended session (50 03)");
    for (int n = 1; n <= 2; ++n) {
        ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
        ASSERT_TRUE(spinUntil([&]() { return log.count(entered) == n; }, 5000))
            << "connect " << n << "\n"
            << log.join('\n').toStdString();
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
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

/* ADR-0027 section 3: the fake's VIN reaches QML only as a salted SHA-256, once
 * a session, and nothing is left of it after the disconnect. A bare URI
 * runs obd2, whose rotation asks the identity. */
TEST(BridgeTest, AConnectsVinReachesQmlOnlyAsASaltedHash)
{
    SessionBridge bridge;
    int identified = 0;
    QObject::connect(&bridge, &SessionBridge::vehicleIdentified,
                     [&identified]() { ++identified; });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
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
    // ADR-0043: its decode is logged once too; the fake's has an I
    EXPECT_EQ(log.filter(QRegularExpression(QStringLiteral("^VIN: "))),
              QStringList{QStringLiteral(
                  "VIN: not a VIN: 49 CFR 565 takes 17 digits and capital "
                  "letters, never I, O or Q")});
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty());
}

/* ADR-0027 section 3: the vehicle is known once 09 02 has answered, while
 * the rest of the identification is still out. The fake holds every
 * Service 0x22 request (7F 22 78, no answer), so the list's DIDs stay out
 * about 5 s each and no identity line comes within the wait. */
TEST(BridgeTest, TheVinIsTakenWhileTheRestOfTheIdentificationIsOut)
{
    FakeEnvArm hold("ECUIO_FAKE_HELD_SERVICE", "22");
    SessionBridge bridge;
    int identified = 0;
    QObject::connect(&bridge, &SessionBridge::vehicleIdentified,
                     [&identified]() { ++identified; });
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    // A bare URI runs obd2, which identifies
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(spinUntil([&identified]() { return identified > 0; }, 4000))
        << "vehicleIdentified waited for the whole list:\n"
        << log.join('\n').toStdString();
    EXPECT_EQ(
        bridge.vinKey(QStringLiteral("salt")),
        QString::fromLatin1(
            QCryptographicHash::hash(QByteArrayLiteral("saltFAKEJ2534VIN00001"),
                                     QCryptographicHash::Sha256)
                .toHex()));
    EXPECT_EQ(log.filter(QRegularExpression(QStringLiteral("^VIN: "))).size(),
              1);
    EXPECT_TRUE(
        log.filter(QRegularExpression(QStringLiteral("^ECU 0x7E8 identity: ")))
            .isEmpty())
        << "the list isn't complete, so no identity line yet";
    spinLoop(300);
    EXPECT_EQ(identified, 1) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.vinKey(QStringLiteral("salt")).isEmpty());
}

/* ADR-0043: the "VIN:" line and Settings' lookup, one summary. The VINs
 * are made up around FAKE, their check digits by 49 CFR 565.15 */
TEST(BridgeTest, AVinSummarySaysItsYearMakeAndCheckDigit)
{
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("1FAFAKE10M0000001"), 2027),
        QStringLiteral("model year 2021; Ford (manufacturer code 1FA); "
                       "check digit matches"));
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("1FAFAKE13Y0000001"), 2027),
        QStringLiteral("model year 2000 (not 2030, which is still ahead); "
                       "Ford (manufacturer code 1FA); check digit matches"));
    EXPECT_EQ(
        SessionBridge::vinSummary(QStringLiteral("ZZZFAKE10U0000001"), 2027),
        QStringLiteral("no model year (position 10 is U); manufacturer "
                       "code ZZZ isn't in NHTSA's list; check digit 0 "
                       "isn't the 1 it works out to (North America "
                       "requires it; other markets may not)"));
    EXPECT_EQ(SessionBridge::vinSummary(
                  QStringLiteral("1FAFAKE10M000000") + QChar(0xC9), 2027)
                  .left(10),
              QStringLiteral("not a VIN:"));
    SessionBridge bridge;
    EXPECT_EQ(
        bridge.describeVin(QStringLiteral(" 1fa fake10m 0000001 ")).left(14),
        QStringLiteral("model year 202"))
        << "spaces dropped, upper-cased";
    EXPECT_TRUE(bridge.describeVin(QStringLiteral("  ")).isEmpty());
    EXPECT_TRUE(
        bridge.vinSource().startsWith(QStringLiteral("vPICList_lite_")));
}

/* ADR-0027 section 2: an adapter session's recording folder names the picked
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

/* ADR-0038 through the fake J2534 DLL, whose engine turns (2000 rpm)
 * and refuses to clear running (ECUIO_FAKE_CLEAR_NRC 0x22, J1979's
 * conditionsNotCorrect): the refusal is one line that says why, and the
 * generations the clear moved log no second diagnostics line; a Read
 * again after it logs its read */
TEST(BridgeTest, AClearTheEngineRefusesLogsOneLineAndNoRead)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm refuses("ECUIO_FAKE_CLEAR_NRC", "0x22");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    const auto clear_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(spinUntil([&]() { return !clear_lines().isEmpty(); }, 5000));
    spinLoop(500);
    ASSERT_EQ(clear_lines().size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().first().toStdString(),
              "ECU 0x7E8 clear: refused (7F 04 22): this ECU won't clear with "
              "the engine running: stop and turn it off, then try again");
    EXPECT_EQ(diagnostics_lines().size(), 1) << log.join('\n').toStdString();

    bridge.readDiagnostics();
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << "a read after the clear logs";
    EXPECT_EQ(diagnostics_lines().at(1).toStdString(), kFakeEcuDiagnosticsLine);
    spinLoop(300);
    EXPECT_EQ(clear_lines().size(), 1) << "one line per clear asked";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The user's word on the engine (ADR-0038's appendix of 2026-11-01): the
 * fake's engine turns (2000 rpm) and its car stands, so the clear goes
 * through, its line naming both speeds */
TEST(BridgeTest, AClearAtTheFakesRunningEngineGoesThrough)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << log.join('\n').toStdString();
    EXPECT_EQ(log.filter(QStringLiteral("ECU 0x7E8 clear: ")),
              QStringList{QStringLiteral(
                  "ECU 0x7E8 clear: cleared (2000 rpm, 0 km/h)")});
    EXPECT_TRUE(bridge.clearMoving().isEmpty()) << "nothing waits";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Moving (ECUIO_FAKE_SPEED_KMH 48), the word on the engine isn't enough:
 * the clear ends unsent and clearMoving names the ECU and the speed, in
 * the unit system, the log's lines too; the word on a moving vehicle
 * clears it, with no second file. Canceled instead, nothing more is sent */
TEST(BridgeTest, AClearWhileMovingAsksOnceMore)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm moving("ECUIO_FAKE_SPEED_KMH", "48");
    for (const bool word : {true, false}) {
        SCOPED_TRACE(word ? "cleared moving" : "canceled");
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        SessionBridge bridge;
        bridge.setRecordingsRootForTesting(root.path());
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        const auto clear_lines = [&log]() {
            return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
        };
        int moving_signals = 0;
        QObject::connect(&bridge, &SessionBridge::clearMovingChanged,
                         [&moving_signals]() { ++moving_signals; });
        ASSERT_TRUE(bridge
                        .connectSession(
                            QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
                        .isEmpty());
        ASSERT_TRUE(spinUntil(
            [&]() {
                return !log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "))
                            .isEmpty();
            },
            5000));
        EXPECT_FALSE(bridge.clearDiagnostics().isEmpty());
        ASSERT_TRUE(
            spinUntil([&]() { return !bridge.clearMoving().isEmpty(); }, 5000))
            << log.join('\n').toStdString();
        EXPECT_EQ(clear_lines(),
                  QStringList{QStringLiteral(
                      "ECU 0x7E8 clear: not sent: moving (48 km/h)")});
        EXPECT_EQ(bridge.clearMoving(), QStringList{QStringLiteral("0x7E8")});
        EXPECT_EQ(bridge.clearMovingSpeed(), QStringLiteral("48 km/h"));
        bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
        EXPECT_EQ(bridge.clearMovingSpeed(), QStringLiteral("30 mph"));
        EXPECT_GT(moving_signals, 0);

        if (word) {
            bridge.clearDiagnosticsMoving();
            EXPECT_TRUE(bridge.clearMoving().isEmpty());
            ASSERT_TRUE(
                spinUntil([&]() { return clear_lines().size() == 2; }, 5000))
                << log.join('\n').toStdString();
            EXPECT_EQ(clear_lines().at(1).toStdString(),
                      "ECU 0x7E8 clear: cleared (2000 rpm, 30 mph)")
                << "the log in the unit system the clear ended in";
            EXPECT_EQ(
                log.filter(QStringLiteral("Clearing while moving")).size(), 1);
        } else {
            bridge.cancelClearMoving();
            EXPECT_TRUE(bridge.clearMoving().isEmpty());
            EXPECT_EQ(log.filter(QStringLiteral("Clear while moving not sent: "
                                                "0x7E8"))
                          .size(),
                      1);
            spinLoop(500);
            EXPECT_EQ(clear_lines().size(), 1) << "nothing more sent";
            bridge.clearDiagnosticsMoving();
            spinLoop(300);
            EXPECT_EQ(clear_lines().size(), 1) << "nothing waits to send";
        }
        EXPECT_EQ(QDir(root.path())
                      .entryList({QStringLiteral("*.jsonl")}, QDir::Files)
                      .size(),
                  1)
            << "one saved file";
        ASSERT_TRUE(disconnectAndWait(bridge));
    }
}

/* With the fake's engine stopped the clear goes through: its line, then
 * the read after it, once, though the report passes through an
 * incomplete read on the way; an adapter's file names the vehicle */
TEST(BridgeTest, AClearAtTheFakesStoppedEngineLogsTheReadAfter)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm off("ECUIO_FAKE_ENGINE_OFF", "1");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setVehicleLabel(QStringLiteral("Daily driver"));
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("j2534://%1?probe=0&identify=0")
                                .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH)))
            .isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 5000));
    const QString shown = bridge.clearDiagnostics();
    EXPECT_TRUE(QRegularExpression(
                    QStringLiteral(
                        "diagnostics-[0-9]{8}-[0-9]{6} Daily driver\\.jsonl$"))
                    .match(shown)
                    .hasMatch())
        << shown.toStdString();
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 5000))
        << log.join('\n').toStdString();
    spinLoop(500);
    EXPECT_EQ(log.filter(QStringLiteral("ECU 0x7E8 clear: ")),
              QStringList{
                  QStringLiteral("ECU 0x7E8 clear: cleared (0 rpm, 0 km/h)")});
    ASSERT_EQ(diagnostics_lines().size(), 2) << log.join('\n').toStdString();
    EXPECT_TRUE(diagnostics_lines().at(1).contains(
        QStringLiteral("MIL off; stored none; pending none; permanent P0420")))
        << diagnostics_lines().at(1).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Live leaves out what the fake ECU withholds from its bitmaps, and
 * only that: the bridge reads the session's support report (ADR-0039
 * section 6) */
TEST(BridgeTest, LiveHidesThePidsTheFakeEcuWithholds)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    FakeEnvArm withheld("ECUIO_FAKE_WITHHELD_PIDS", "0A,5C");
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&]() { return model->hiddenCount() > 0; }, 5000))
        << "nothing hidden";
    spinLoop(300);
    QStringList hidden;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r);
        if (model->data(i, SignalListModel::HiddenRole).toBool()) {
            hidden << model->data(i, SignalListModel::NameRole).toString();
        }
    }
    /* In set order: the ten first, then the table's */
    EXPECT_EQ(hidden, (QStringList{QStringLiteral("Engine Oil Temp"),
                                   QStringLiteral("Fuel Pressure")}));
    EXPECT_EQ(bridge.liveSignalModel()->rowCount(), model->rowCount() - 2);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* The obd2 set's id for Service 01 PID pid */
uint16_t obd2IdOf(uint8_t pid)
{
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            d.mux_key == (0x4100u | pid)) {
            return d.id;
        }
    }
    ADD_FAILURE() << "no def reads PID " << int(pid);
    return 0;
}

/* The next 16 writes after a reset of the fake's page, as the Service 01
 * PIDs they ask; empty when they don't come or one isn't a Service 01
 * request */
std::vector<uint8_t> nextPids()
{
    fake_control::reset(fakePage());
    if (!spinUntil([]() { return fake_control::writes(fakePage()) >= 16; },
                   5000, 10)) {
        return {};
    }
    std::vector<uint8_t> pids;
    for (uint32_t i = 0; i < 16; ++i) {
        fake_control::Write w{};
        if (!fake_control::recorded_write(fakePage(), i, &w) || w.size < 6 ||
            w.data[4] != 0x01) {
            return {};
        }
        pids.push_back(w.data[5]);
    }
    return pids;
}

bool onlyThese(const std::vector<uint8_t>& pids,
               std::initializer_list<uint8_t> picks)
{
    return !pids.empty() &&
           std::all_of(pids.begin(), pids.end(), [&picks](uint8_t pid) {
               return std::find(picks.begin(), picks.end(), pid) != picks.end();
           });
}

} // namespace

namespace {

/* obd2's ranked PIDs (kPlotOrder): its default pick with nothing added */
const std::vector<uint8_t> kRankedPids = {0x0C, 0x05, 0x0F, 0x10, 0x5C,
                                          0x0D, 0x04, 0x0E, 0x06, 0x07,
                                          0x0B, 0x42, 0x49, 0x44};

std::vector<uint8_t> rankedAnd(std::initializer_list<uint8_t> more)
{
    std::vector<uint8_t> pids = kRankedPids;
    pids.insert(pids.end(), more.begin(), more.end());
    return pids;
}

bool onlyOf(const std::vector<uint8_t>& pids, const std::vector<uint8_t>& picks)
{
    return !pids.empty() &&
           std::all_of(pids.begin(), pids.end(), [&picks](uint8_t pid) {
               return std::find(picks.begin(), picks.end(), pid) != picks.end();
           });
}

int modelRowOf(SignalListModel* model, uint16_t id)
{
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->data(model->index(r), SignalListModel::IdRole).toUInt() ==
            id) {
            return r;
        }
    }
    return -1;
}

bool unaskedRow(SignalListModel* model, uint16_t id)
{
    const int row = modelRowOf(model, id);
    return row >= 0 &&
           model->data(model->index(row), SignalListModel::UnaskedRole)
               .toBool();
}

/* The first line starting with prefix, or -1 */
int lineStarting(const QStringList& log, const QString& prefix)
{
    for (int i = 0; i < log.size(); ++i) {
        if (log[i].startsWith(prefix)) {
            return i;
        }
    }
    return -1;
}

QString fakeObd2PickUri()
{
    return QStringLiteral(
               "j2534://%1?profile=obd2&probe=0&identify=0&diagnose=0")
        .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
}

} // namespace

/* Start log over the fake ECU (ADR-0056 sections 2 and 3): the session
 * asks the plotted PIDs alone, ticks on Live meanwhile leave that so,
 * the recording's time starts at Start (not at the connect), and Stop
 * asks the default pick again, saying so. */
TEST(BridgeTest, ALogAsksThePlottedPidsAlone)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList stopped;
    QObject::connect(
        &bridge, &SessionBridge::logStopped,
        [&stopped](const QString& folder, bool) { stopped << folder; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=obd2&filter=0x7E8&probe=0&"
                       "identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    QElapsedTimer age;
    age.start();
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    const std::vector<uint16_t> picks = {obd2IdOf(0x0C), obd2IdOf(0x0D)};
    plotOnly(model, picks);
    ASSERT_EQ(model->subscribedCount(), 2);
    // A session old enough that a recording timed from its start shows it
    ASSERT_TRUE(spinUntil([&]() { return age.elapsed() >= 2000; }, 10000));
    const std::vector<uint8_t> before = nextPids();
    ASSERT_FALSE(before.empty());
    ASSERT_FALSE(onlyThese(before, {0x0C, 0x0D}))
        << "the whole set is asked before the log";

    const qint64 ageAtStartMs = age.elapsed();
    ASSERT_TRUE(bridge.startLog());
    const QString folder = bridge.recordingFolderUrl().toLocalFile();
    ASSERT_TRUE(spinUntil([]() { return onlyThese(nextPids(), {0x0C, 0x0D}); },
                          15000, 10))
        << "the rotation never narrowed to 0C and 0D";

    // A tick meanwhile plots the row and leaves the focus as it was
    plotOnly(model, {picks[0], picks[1], obd2IdOf(0x05)});
    ASSERT_EQ(model->subscribedCount(), 3);
    for (int round = 0; round < 2; ++round) {
        const std::vector<uint8_t> pids = nextPids();
        EXPECT_TRUE(onlyThese(pids, {0x0C, 0x0D})) << "round " << round;
    }

    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    bridge.stopLog();
    ASSERT_EQ(stopped, QStringList{folder});
    // The ticked rows are ranked ones: the pick is obd2's ranked PIDs
    EXPECT_GT(lineStarting(log, QStringLiteral("Asking 14 signals: ")),
              lineStarting(log, QStringLiteral("Log stopped: ")))
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        []() {
            const std::vector<uint8_t> pids = nextPids();
            return onlyOf(pids, kRankedPids) &&
                   !onlyThese(pids, {0x0C, 0x0D, 0x05});
        },
        15000, 10))
        << "the default pick never came back";
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QList<QByteArray> rows =
        fileLines(folder + QStringLiteral("/frames.csv"));
    ASSERT_GE(rows.size(), 2) << "no frame in the log";
    const qint64 firstHostUs = rows[1].split(',')[0].toLongLong();
    EXPECT_LT(firstHostUs, ageAtStartMs * 1000 / 2)
        << "timed from the connect, " << ageAtStartMs << " ms before Start";
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains(
        (QStringLiteral("focus  ") + namesOf(model, picks).join(',')).toUtf8()))
        << events.toStdString();
}

/* A focus core refuses (under ?poll=0x0C, 0D alone leaves nothing to
 * ask) refuses Start log with core's words and opens no folder; the
 * polled PID's is taken. */
TEST(BridgeTest, AFocusCoreRefusesStartsNoLog)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=obd2&poll=0x0C&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    plotOnly(bridge.signalModel(), {obd2IdOf(0x0D)});
    ASSERT_EQ(bridge.signalModel()->subscribedCount(), 1);
    EXPECT_FALSE(bridge.startLog());
    EXPECT_FALSE(bridge.logActive());
    EXPECT_FALSE(bridge.recordingActive());
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Log not started: the focus leaves nothing to request: none of the "
        "picked signals is asked on this session")))
        << log.join('\n').toStdString();
    EXPECT_TRUE(recordingFolders(root.path()).isEmpty());

    plotOnly(bridge.signalModel(), {obd2IdOf(0x0C)});
    EXPECT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    bridge.stopLog();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(recordingFolders(root.path()).size(), 1);
}

/* The default pick (ADR-0056 section 2's second use): from the start the
 * session asks obd2's ranked PIDs and the gauges' signals, its line after
 * the session's; the rest read "not asked"; a tick asks its row within a
 * tick, and a gauge taken away stops asking its signal */
TEST(BridgeTest, TheDefaultPickAsksTheRankedRowsTheGaugesAndTheTicks)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    // Set while disconnected: Throttle Position, which obd2 doesn't rank
    bridge.setPickGauges({QVariant(
        QStringList{QStringLiteral("id:9"), QStringLiteral("id:7006")})});
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    const int session = lineStarting(log, QStringLiteral("Session: "));
    const int asking = lineStarting(log, QStringLiteral("Asking "));
    ASSERT_GE(asking, 0) << log.join('\n').toStdString();
    EXPECT_GT(asking, session);
    EXPECT_TRUE(log[asking].startsWith(QStringLiteral(
        "Asking 15 signals: Engine Load, Coolant Temp, RPM, Vehicle Speed, "
        "Intake Air Temp, Throttle Position, Battery Voltage")))
        << log[asking].toStdString();
    EXPECT_TRUE(log[asking].endsWith(QStringLiteral(" and 3 more")))
        << log[asking].toStdString();
    ASSERT_TRUE(spinUntil(
        []() { return onlyOf(nextPids(), rankedAnd({0x11})); }, 15000, 10))
        << "the rotation never kept to the pick";

    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil(
        [&]() { return unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID); },
        5000))
        << "Fuel Level never read not asked";
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_THROTTLE_ID));
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_RPM_ID));
    EXPECT_GT(model->unaskedCount(), 0);

    // A tick asks the row
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() { return !unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID); },
        5000))
        << "the tick never asked Fuel Level";
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 16 signals: ")), 0)
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        []() {
            const std::vector<uint8_t> pids = nextPids();
            return onlyOf(pids, rankedAnd({0x11, 0x2F})) &&
                   std::find(pids.begin(), pids.end(), 0x2F) != pids.end();
        },
        15000, 10))
        << "Fuel Level's PID never went out";

    // A gauge taken away stops asking its signal
    bridge.setPickGauges({});
    ASSERT_TRUE(spinUntil(
        [&]() { return unaskedRow(model, ECUIO_OBD2_SIG_THROTTLE_ID); }, 5000))
        << "Throttle Position stayed asked";
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 15 signals: ")), 0);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A Record sessions recording of a session that asks names its default
 * pick on a pick line, and the Recordings list calls it a session, not
 * a log; a pick changed by a tick is core's pick line in the activity,
 * and the Log page's "Asking" line is no note beside it */
TEST(BridgeTest, ASessionRecordingNamesItsPickAndIsNoLog)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    spinLoop(500);
    SignalListModel* model = bridge.signalModel();
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(log, QStringLiteral("Asking 15 signals: ")) >=
                   0;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QLatin1Char('/') + folders[0];
    const QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains("\npick  Engine Load,Coolant Temp,RPM,"))
        << events.toStdString();
    EXPECT_FALSE(events.contains("\nfocus  ")) << events.toStdString();
    const int change = events.indexOf("  pick  15 signals: Engine Load,");
    ASSERT_GE(change, 0) << events.toStdString();
    EXPECT_GT(events.indexOf("Fuel Level", change), change);
    EXPECT_FALSE(events.contains("note  Asking ")) << events.toStdString();
    EXPECT_FALSE(
        bridge.recordingRow(folder).value(QStringLiteral("log")).toBool());
}

/* A new minimum time between sweeps reaches the running session's pick
 * (decided choice 4), though the picked signals stay the same, and Off
 * takes it away again */
TEST(BridgeTest, ANewMinimumTimeReachesTheRunningPick)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeObd2PickUri()).isEmpty());
    ASSERT_GE(lineStarting(log, QStringLiteral("Asking 14 signals: ")), 0)
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(250);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(
                       log, QStringLiteral("Asking 14 signals (at least 250 ms "
                                           "between sweeps): ")) >= 0;
        },
        5000))
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(0);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.filter(QRegularExpression(
                                  QStringLiteral("^Asking 14 signals: ")))
                       .size() == 2;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Core paces only a focus, so a set that ranks none, asked whole, says a
 * minimum doesn't apply to it, and says nothing of one while it's Off;
 * the line says why, which core's own focus line doesn't, so a recording
 * keeps it as a note */
TEST(BridgeTest, TheWholeSetSaysAMinimumTimeDoesntApply)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://"
                       "%1?profile=uds-generic&did=1E1C:u16&identify=0&"
                       "diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString ranksNone =
        QStringLiteral("Asking every signal: the profile ranks none");
    EXPECT_TRUE(log.contains(ranksNone)) << log.join('\n').toStdString();
    bridge.setMinSweepMs(250);
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(
                ranksNone +
                QStringLiteral(
                    "; no minimum time between sweeps applies to it"));
        },
        5000))
        << log.join('\n').toStdString();
    bridge.setMinSweepMs(0);
    ASSERT_TRUE(spinUntil([&]() { return log.count(ranksNone) == 2; }, 5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QByteArray events =
        readFile(root.path() + QLatin1Char('/') + folders[0] +
                 QStringLiteral("/events.log"));
    const QByteArray note = ("  note  " + ranksNone).toUtf8();
    EXPECT_TRUE(events.contains(note + "\n") || events.contains(note + "\r\n"))
        << events.toStdString();
}

/* A pick core refuses (under ?poll=0x2F none of the ranked PIDs is asked)
 * leaves the session asking its whole set, and says why; a tick that
 * gives the pick something to ask is taken. The lines say a polled
 * session asks its polled signals, never the set */
TEST(BridgeTest, APickCoreRefusesAsksTheWholeSet)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=obd2&poll=0x2F&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Asking every polled signal: the focus leaves nothing to request: "
        "none of the picked signals is asked on this session")))
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([]() { return onlyThese(nextPids(), {0x2F}); }, 15000, 10));

    SignalListModel* model = bridge.signalModel();
    model->toggleSubscribed(modelRowOf(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(spinUntil(
        [&]() {
            return lineStarting(log, QStringLiteral(
                                         "Picked 15 signals; ?poll= asks only "
                                         "the polled ones among them: ")) >= 0;
        },
        5000))
        << log.join('\n').toStdString();
    ASSERT_TRUE(
        spinUntil([]() { return onlyThese(nextPids(), {0x2F}); }, 15000, 10));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A pick the walk leaves with nothing to ask (the ECU lacks every ranked
 * PID) gives way to the whole set, with core's words, and leaves no row
 * unasked */
TEST(BridgeTest, APickLeftWithNothingToAskAsksTheWholeSet)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    // Past the page's 31 characters: the environment, which the surrogate
    // child takes at its spawn
    EnvRestore restore("ECUIO_FAKE_WITHHELD_PIDS");
    qputenv("ECUIO_FAKE_WITHHELD_PIDS",
            "0C,05,0F,10,5C,0D,04,0E,06,07,0B,42,49,44");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("j2534://%1?profile=obd2&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_GE(lineStarting(log, QStringLiteral("Asking 14 signals: ")), 0)
        << log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return log.contains(QStringLiteral(
                "Asking every signal: the focus leaves nothing to request: "
                "none of the picked signals is asked on this session"));
        },
        15000))
        << log.join('\n').toStdString();
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&]() { return model->unaskedCount() == 0; }, 5000));
    EXPECT_FALSE(unaskedRow(model, ECUIO_OBD2_SIG_FUEL_LEVEL_ID));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* ADR-0051 section 3: a DID the fake ECU holds no reply for (it answers
 * 7F 22 31) shows on Live as refused, with the code by name, and the one
 * it answers takes its value; neither is hidden */
TEST(BridgeTest, LiveSaysWhichDidsTheFakeEcuRefuses)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://"
                                       "%1?profile=uds-generic&did=1E1C:u16,"
                                       "F1FE:u16&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    SignalListModel* model = bridge.signalModel();
    const auto rowNamed = [model](const QString& name) {
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->data(model->index(r), SignalListModel::NameRole)
                    .toString() == name) {
                return r;
            }
        }
        return -1;
    };
    const int answered = rowNamed(QStringLiteral("DID 1E1C"));
    const int refused = rowNamed(QStringLiteral("DID F1FE"));
    ASSERT_GE(answered, 0);
    ASSERT_GE(refused, 0);
    const auto words = [model](int row) {
        return model->data(model->index(row), SignalListModel::RefusedRole)
            .toString();
    };
    ASSERT_TRUE(spinUntil([&]() { return !words(refused).isEmpty(); }, 5000))
        << "DID F1FE never refused";
    ASSERT_TRUE(spinUntil(
        [&]() {
            return model
                ->data(model->index(answered), SignalListModel::HasValueRole)
                .toBool();
        },
        5000))
        << "DID 1E1C never answered";
    EXPECT_EQ(words(refused),
              QStringLiteral("Refused: requestOutOfRange (7F 22 31)"));
    EXPECT_EQ(words(answered), QString());
    EXPECT_EQ(model->hiddenCount(), 0) << "a refused row stays on Live";
    EXPECT_EQ(bridge.refusedUnnamed(), QString()) << "every refusal named";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* Ten DIDs the fake refuses: the report lists eight, whose rows say so,
 * and Live's status says the two more it can't name; a new connect
 * starts with none */
TEST(BridgeTest, LiveCountsTheRefusalsItCantName)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    QStringList dids;
    for (int n = 0xF0; n < 0xFA; ++n) {
        dids << QStringLiteral("F1%1")
                        .arg(n, 2, 16, QLatin1Char('0'))
                        .toUpper() +
                    QStringLiteral(":u16");
    }
    const QString uri =
        QStringLiteral(
            "j2534://%1?profile=uds-generic&did=%2&identify=0&diagnose=0")
            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH),
                 dids.join(QLatin1Char(',')));
    int changed = 0;
    QObject::connect(&bridge, &SessionBridge::refusedUnnamedChanged,
                     [&changed]() { ++changed; });
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    // The two refusals past the list can land on either side of a 100 ms
    // tick, so the text may read "1 more request" first: wait for both
    const QString both = QStringLiteral("ECU 0x7E8 refused 2 more requests");
    ASSERT_TRUE(
        spinUntil([&]() { return bridge.refusedUnnamed() == both; }, 5000))
        << "refusals counted past the list: "
        << bridge.refusedUnnamed().toStdString();
    EXPECT_GE(changed, 1);
    SignalListModel* model = bridge.signalModel();
    int named = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        named += model->data(model->index(r), SignalListModel::RefusedRole)
                         .toString()
                         .isEmpty()
                     ? 0
                     : 1;
    }
    EXPECT_EQ(named, ECUIO_SUPPORT_MAX_DROPPED);
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_EQ(bridge.refusedUnnamed(), QString())
        << "a new session starts with none";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A refusal is the session's: a module that refused a DID in the last
 * session refuses nothing in the next until it answers. Channel 0 (a
 * one-DID scan, found at once) reports first in both; in the second the
 * fake answers refusals with nothing, so the module on 0x7E9 never
 * reports, and its row must never read refused */
TEST(BridgeTest, ALastSessionsRefusalIsNotTheNextOnes)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    SessionBridge bridge;
    const QString uri = QStringLiteral("j2534://"
                                       "%1?profile=uds-generic&did=1E1C-1E1D,"
                                       "7E9:F1FE&identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    SignalListModel* model = bridge.signalModel();
    const auto rowNamed = [model](const QString& name) {
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->data(model->index(r), SignalListModel::NameRole)
                    .toString() == name) {
                return r;
            }
        }
        return -1;
    };
    const auto words = [model](int row) {
        return row < 0
                   ? QStringLiteral("no row")
                   : model
                         ->data(model->index(row), SignalListModel::RefusedRole)
                         .toString();
    };
    const auto answered = [model](int row) {
        return row >= 0 &&
               model->data(model->index(row), SignalListModel::HasValueRole)
                   .toBool();
    };

    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    int found = rowNamed(QStringLiteral("DID 1E1C"));
    int other = rowNamed(QStringLiteral("0x7E9 DID F1FE"));
    ASSERT_GE(other, 0);
    ASSERT_TRUE(spinUntil([&]() { return !words(other).isEmpty(); }, 5000))
        << "the module on 0x7E9 never refused its DID F1FE";
    ASSERT_TRUE(disconnectAndWait(bridge));

    FakeEnvArm silent("ECUIO_FAKE_UNSUPPORTED_NRC", "0");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    found = rowNamed(QStringLiteral("DID 1E1C"));
    other = rowNamed(QStringLiteral("0x7E9 DID F1FE"));
    ASSERT_GE(other, 0);
    bool everRefused = false;
    const auto watch = [&]() {
        everRefused = everRefused || !words(other).isEmpty();
        return false;
    };
    EXPECT_TRUE(spinUntil(
        [&]() {
            (void)watch();
            return answered(found);
        },
        5000))
        << "channel 0's scan never found DID 1E1C";
    (void)spinUntil(watch, 1000);
    EXPECT_FALSE(everRefused)
        << "the last session's refusal: " << words(other).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
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

/* The "ECU 0x…:" line's wording, from hand-built reports: the first
 * vehicle's three bitmaps (docs/bench-sessions/2026-10-22-vehicle.md:
 * 44 data PIDs to 0x60, whose bit says a fourth exists), a chain
 * answered to its end, one unanswered, and requests dropped for each
 * reason, past the eight a report lists. */
/* Finding a signal among many (ADR-0050): every word in the name or the
 * unit, ignoring case; plotted rows only; the chosen groups, a code
 * core doesn't name counted as Other; the choices in code order */
TEST(BridgeTest, SignalFilterMatchesWordsPlottedRowsAndGroups)
{
    SignalFilter f;
    int changes = 0;
    QObject::connect(&f, &SignalFilter::changed, [&changes]() { ++changes; });
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));

    f.setText(QStringLiteral("  COOL   temp "));
    EXPECT_TRUE(f.active());
    EXPECT_TRUE(f.matches(QStringLiteral("Coolant Temp"), QStringLiteral("°C"),
                          1, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Engine Oil Temp"),
                           QStringLiteral("°C"), 1, false))
        << "every word";
    f.setText(QStringLiteral("temp °c"));
    EXPECT_TRUE(f.matches(QStringLiteral("Engine Oil Temp"),
                          QStringLiteral("°C"), 1, false))
        << "a word in the unit";
    f.setText(QStringLiteral("psi"));
    EXPECT_TRUE(f.matches(QStringLiteral("Fuel Pressure"),
                          QStringLiteral("psi"), 3, false));
    EXPECT_FALSE(f.matches(QStringLiteral("Fuel Pressure"),
                           QStringLiteral("kPa"), 3, false));
    f.setText(QString());
    EXPECT_FALSE(f.active());

    f.setPlottedOnly(true);
    EXPECT_FALSE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("RPM"), QStringLiteral("RPM"), 1, true));
    f.setPlottedOnly(false);

    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_TRUE(f.matches(QStringLiteral("RPM"), QString(),
                          ECUIO_SIGNAL_GROUP_ENGINE, false));
    EXPECT_FALSE(f.matches(QStringLiteral("MAF"), QString(),
                           ECUIO_SIGNAL_GROUP_AIR, false));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_TRUE(f.matches(QStringLiteral("MAF"), QString(),
                          ECUIO_SIGNAL_GROUP_AIR, false))
        << "either chosen group";
    f.toggleGroup(200);
    EXPECT_TRUE(f.chosen(ECUIO_SIGNAL_GROUP_OTHER))
        << "an unknown code is Other's";
    EXPECT_TRUE(f.matches(QStringLiteral("DID 1E1C"), QString(), 0, false));
    EXPECT_TRUE(
        f.matches(QStringLiteral("Next year's"), QString(), 250, false));
    EXPECT_EQ(f.groups(), (QVariantList{0, 1, 2}));
    f.toggleGroup(ECUIO_SIGNAL_GROUP_ENGINE);
    EXPECT_FALSE(f.chosen(ECUIO_SIGNAL_GROUP_ENGINE))
        << "a second tap lets it go";

    const QVariantList choices = f.groupChoices(QVariantList{3, 1, 1, 200, 0});
    ASSERT_EQ(choices.size(), 3);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("code")).toInt(), 0);
    EXPECT_EQ(choices[0].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Other"));
    EXPECT_EQ(choices[1].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine"));
    EXPECT_EQ(choices[2].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Fuel"));

    f.clear();
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(f.groups().isEmpty());
    EXPECT_EQ(changes, 11);
}

/* Live's list through its filter (ADR-0050): the mock's rows by word,
 * group and plotted; a tick under "Plotted only" re-filters; the groups
 * and the count before the filter leave out the rows core hides */
TEST(BridgeTest, LiveListFollowsItsFilter)
{
    SignalListModel model;
    model.setSignalSet(ecuio_mock_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_EQ(live.rowCount(), rows);
    EXPECT_EQ(live.shownCount(), rows);
    EXPECT_EQ(live.groups(), (QVariantList{1, 2, 4, 6, 7}))
        << "Engine, Air, Oxygen sensors, Vehicle, Electrical";
    const auto names = [&live]() {
        QStringList n;
        for (int r = 0; r < live.rowCount(); ++r) {
            n << live.data(live.index(r, 0), SignalListModel::NameRole)
                     .toString();
        }
        return n;
    };

    live.filter()->setText(QStringLiteral("temp"));
    EXPECT_EQ(names(), (QStringList{QStringLiteral("Coolant Temp"),
                                    QStringLiteral("Intake Air Temp"),
                                    QStringLiteral("Engine Oil Temp")}));
    live.filter()->toggleGroup(ECUIO_SIGNAL_GROUP_AIR);
    EXPECT_EQ(names(), QStringList{QStringLiteral("Intake Air Temp")});
    live.filter()->clear();
    EXPECT_EQ(live.rowCount(), rows);

    live.filter()->setPlottedOnly(true);
    EXPECT_EQ(live.rowCount(), rows) << "the mock plots every row";
    model.toggleSubscribed(0); /* RPM */
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_FALSE(names().contains(QStringLiteral("RPM")));
    EXPECT_EQ(live.shownCount(), rows) << "the count before the filter";
    live.filter()->clear();

    /* obd2's Diagnostics rows hidden by core: no choice, not counted */
    model.setSignalSet(ecuio_obd2_signal_set());
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    std::vector<uint16_t> diag;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].group == ECUIO_SIGNAL_GROUP_DIAGNOSTICS) {
            diag.push_back(set->defs[i].id);
        }
    }
    ASSERT_FALSE(diag.empty());
    EXPECT_TRUE(live.groups().contains(QVariant(8)));
    model.applyAbsent(diag);
    EXPECT_FALSE(live.groups().contains(QVariant(8)));
    EXPECT_EQ(live.shownCount(),
              model.rowCount() - static_cast<int>(diag.size()));
}

/* Live leaves out the rows core judges unsupported (ADR-0039 section 6;
 * the rule is PidSupportTest's): on the first vehicle's bitmaps (00, 20
 * and 40 answered), 0x5C among them; a PID past an answered range, and a
 * row no report names, stay. Each list is applied afresh, so a rescan
 * that finds a PID shows its row again. */
TEST(BridgeTest, LiveHidesTheRowsTheirEcuMarksUnsupported)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    ASSERT_GT(rows, 0);
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
    int hiddenSignals = 0;
    QObject::connect(&model, &SignalListModel::hiddenCountChanged,
                     [&hiddenSignals]() { ++hiddenSignals; });

    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    model.applyUnsupported(unsupportedIds(&car, 1));

    /* The rule, from the bitmaps' bytes: PID p is supported when bit
     * 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8 is set */
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    int expectHidden = 0;
    int pastTheRanges = 0;
    int row5C = -1;
    int row0C = -1;
    for (int r = 0; r < rows; ++r) {
        const unsigned pid =
            (set->defs[r].flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                ? 0x100u /* a computed def has no PID: past every range */
                : set->defs[r].mux_key & 0xFFu;
        const bool clear = pid <= 0x60 && (bitmaps[(pid - 1) / 8] &
                                           (0x80u >> ((pid - 1) % 8))) == 0;
        const QModelIndex i = model.index(r);
        EXPECT_EQ(model.data(i, SignalListModel::HiddenRole).toBool(), clear)
            << "PID " << std::hex << pid;
        EXPECT_EQ(model.data(i, SignalListModel::RowRole).toInt(), r);
        expectHidden += clear ? 1 : 0;
        pastTheRanges += pid > 0x60 ? 1 : 0;
        if (pid == 0x5C)
            row5C = r;
        if (pid == 0x0C)
            row0C = r;
    }
    ASSERT_GE(row5C, 0);
    ASSERT_GE(row0C, 0);
    EXPECT_TRUE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_GT(expectHidden, 1);
    EXPECT_GT(pastTheRanges, 0);
    EXPECT_EQ(model.hiddenCount(), expectHidden);
    EXPECT_EQ(hiddenSignals, 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "a ranked set plots its top ranked rows";

    /* Live's rows are the shown ones in order, each naming its model row */
    ASSERT_EQ(live.rowCount(), rows - expectHidden);
    int last = -1;
    for (int r = 0; r < live.rowCount(); ++r) {
        const QModelIndex i = live.index(r, 0);
        const int source = live.data(i, SignalListModel::RowRole).toInt();
        EXPECT_GT(source, last);
        last = source;
        EXPECT_FALSE(live.data(i, SignalListModel::HiddenRole).toBool());
        EXPECT_EQ(live.data(i, SignalListModel::NameRole),
                  model.data(model.index(source), SignalListModel::NameRole));
    }

    /* A hidden row is never charted, and its plot box counts nothing */
    model.updateValues(
        {{set->defs[row5C].id, 80.0}, {set->defs[row0C].id, 2000.0}}, 100);
    QList<int> charted;
    for (const QVariant& v : model.chartData()) {
        charted << v.toMap().value(QStringLiteral("row")).toInt();
    }
    EXPECT_TRUE(charted.contains(row0C));
    EXPECT_FALSE(charted.contains(row5C));
    model.toggleSubscribed(row5C);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.toggleSubscribed(row5C);

    /* A rescan that finds 0x5C shows it again */
    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_FALSE(
        model.data(model.index(row5C), SignalListModel::HiddenRole).toBool());
    EXPECT_EQ(model.hiddenCount(), expectHidden - 1);
    EXPECT_EQ(live.rowCount(), rows - expectHidden + 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Another ECU's report, or one with nothing learned, hides nothing */
    ecuio_channel_support_t other = car;
    other.rx_id = 0x7E9;
    ecuio_channel_support_t nothing = car;
    nothing.generation = 0;
    const ecuio_channel_support_t neither[2] = {other, nothing};
    model.applyUnsupported(unsupportedIds(neither, 2));
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);

    /* A new set shows every row */
    model.applyUnsupported(unsupportedIds(&car, 1));
    ASSERT_GT(model.hiddenCount(), 0);
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0);
    EXPECT_EQ(live.rowCount(), rows);
}

/* ADR-0039 section 6: a row core reports absent (its field past every reply)
 * is hidden beside the ones the bitmaps rule out, and shown again when
 * core no longer reports it; a PID the bitmaps list keeps its other
 * rows. A new set shows all. */
TEST(BridgeTest, LiveHidesTheFieldsCoreReportsAbsent)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    LiveSignalModel live(&model);
    const int rows = model.rowCount();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_GE(b1, 0);
    ASSERT_GE(b3, 0);
    const auto hidden = [&model](int row) {
        return model.data(model.index(row), SignalListModel::HiddenRole)
            .toBool();
    };

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    EXPECT_TRUE(hidden(b3));
    EXPECT_FALSE(hidden(b1));
    EXPECT_EQ(model.hiddenCount(), 1);
    EXPECT_EQ(live.rowCount(), rows - 1);
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted)
        << "an unranked row's hiding moves no default";

    /* The bitmaps hide theirs beside it; neither undoes the other */
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x1; /* 0x01-0x20: all clear but 0x0C */
    car.pid_bitmap[(0x0C - 1) / 8] = 0x80u >> ((0x0C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&car, 1));
    int unsupported = 0;
    for (int r = 0; r < rows; ++r) {
        const ecuio_signal_def_t& d = ecuio_obd2_signal_set()->defs[r];
        /* Boost, the set's computed def, reads MAP (0B), which this car
         * lacks: it is unsupported with it */
        const unsigned pid = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0
                                 ? 0x0Bu
                                 : d.mux_key & 0xFFu;
        unsupported += (pid <= 0x20 && pid != 0x0C) ? 1 : 0;
    }
    ASSERT_GT(unsupported, 0);
    EXPECT_TRUE(hidden(obd2RowOf(ECUIO_OBD2_SIG_BOOST_ID)));
    EXPECT_TRUE(hidden(b3)) << "still absent after the support's judgment";
    EXPECT_EQ(model.hiddenCount(), unsupported + 1);

    model.applyAbsent({});
    EXPECT_FALSE(hidden(b3)) << "core no longer reports it";
    EXPECT_EQ(model.hiddenCount(), unsupported);

    model.applyAbsent({ECUIO_OBD2_PID_SIG_ID(0x55, 1)});
    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_EQ(model.hiddenCount(), 0) << "a new set shows every row";
}

/* The bridge reads core's absent signals on its tick: a replay of the
 * test car's one-byte PID 55 replies hides bank 3's row, and only it */
TEST(BridgeTest, LiveHidesAFieldTheRepliesComeWithout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray text = "host_us,timestamp_us,id,flags,data\n";
    for (int i = 0; i < 5; ++i) {
        const QByteArray t = QByteArray::number(10000 * i);
        text += t + ',' + t + ",0x7E8,0x0,41 55 80\n";
    }
    QFile f(dir.filePath(QStringLiteral("frames.csv")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(text), text.size());
    f.close();

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("replay://%1?profile=obd2&speed=max")
                                .arg(dir.path()))
            .isEmpty())
        << log.join('\n').toStdString();
    SignalListModel* model = bridge.signalModel();
    const int b1 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 0));
    const int b3 = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x55, 1));
    ASSERT_TRUE(
        spinUntil([model]() { return model->hiddenCount() > 0; }, 5000));
    spinLoop(200);
    EXPECT_EQ(model->hiddenCount(), 1);
    EXPECT_TRUE(
        model->data(model->index(b3), SignalListModel::HiddenRole).toBool());
    EXPECT_FALSE(
        model->data(model->index(b1), SignalListModel::HiddenRole).toBool());
    EXPECT_TRUE(
        model->data(model->index(b1), SignalListModel::HasValueRole).toBool());
    ASSERT_TRUE(disconnectAndWait(bridge));
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

/* ADR-0027 section 1: a vehicle's plotted signals are the rows ticked
 * past the default and unticked from it, read and set by name. The
 * mock's set ranks none, so every numeric row is its default. */
/* The chart's window (LiveViewPage's 10 to 120 s): chartData keeps the
 * samples of the last windowMs before the newest plotted one, and with
 * columns, each slice's lowest and highest in time order and the newest
 * sample, never more than two a slice plus it. Slices are counted from
 * time 0, so the window's first may be partial */
TEST(BridgeTest, ChartDataKeepsTheWindowAndEachSlicesExtremes)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* 1001 rises and falls every second; 2001 tracks the time */
    for (qint64 t = 0; t < 20000; t += 10) {
        const double wave = static_cast<double>((t / 10) % 100 < 50 ? (t / 10) % 50
                                                                    : 50 - (t / 10) % 50);
        model.updateValues({{uint16_t(1001), wave}, {uint16_t(2001), double(t)}}, t);
    }
    const auto samples = [](const QVariantList& data, int s) {
        return data.at(s).toMap().value("samples").toList();
    };

    const QVariantList all = model.chartData();
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(samples(all, 1).size(), 2 * 2000) << "no window: every sample";

    const QVariantList win = model.chartData(5000);
    const QVariantList w = samples(win, 1);
    EXPECT_EQ(w.at(0).toDouble(), 19990.0 - 5000.0) << "from the window's start";
    EXPECT_EQ(w.at(w.size() - 2).toDouble(), 19990.0) << "to the newest";
    EXPECT_EQ(w.size(), 2 * 501);

    const int columns = 10;
    const QVariantList cut = model.chartData(5000, columns);
    for (int s = 0; s < 2; ++s) {
        const QVariantList c = samples(cut, s);
        EXPECT_LE(c.size(), 2 * (2 * (columns + 1) + 1)) << s;
        for (int i = 2; i < c.size(); i += 2) {
            EXPECT_LT(c.at(i - 2).toDouble(), c.at(i).toDouble()) << "in time order";
        }
        EXPECT_EQ(c.at(c.size() - 2).toDouble(), 19990.0) << "the newest kept";
    }
    /* Each slice of the wave keeps its own lowest and highest, as the
     * whole window's samples show them */
    const QVariantList full = samples(model.chartData(5000), 0);
    const QVariantList wave = samples(cut, 0);
    const double slice = 5000.0 / columns;
    std::map<int, std::pair<double, double>> want, got;
    const auto note = [&](std::map<int, std::pair<double, double>>& m,
                          const QVariantList& v) {
        for (int i = 0; i < v.size(); i += 2) {
            const int k =
                static_cast<int>(std::floor(v.at(i).toDouble() / slice));
            const double x = v.at(i + 1).toDouble();
            auto it = m.find(k);
            if (it == m.end()) {
                m[k] = {x, x};
            } else {
                it->second.first = std::min(it->second.first, x);
                it->second.second = std::max(it->second.second, x);
            }
        }
    };
    note(want, full);
    note(got, wave);
    EXPECT_EQ(got, want) << "each slice's lowest and highest";
}

/* A window that slides keeps its points: a sample stays in its slice
 * however far the window has moved, so between two updates every slice
 * away from the window's ends keeps the same lowest and highest, and a
 * noisy trace scrolls instead of shimmering */
TEST(BridgeTest, ChartDataKeepsEachSlicesPointsAsTheWindowSlides)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    /* A noisy trace: a value that jumps sample to sample */
    const auto noise = [](qint64 t) {
        return static_cast<double>((t * 7919) % 97);
    };
    qint64 t = 0;
    const auto feed = [&](qint64 until) {
        for (; t < until; t += 10)
            model.updateValues({{uint16_t(1001), noise(t)}}, t);
    };
    const qint64 windowMs = 10000;
    const int columns = 300; /* a phone's chart: 33.3 ms a slice */
    const auto points = [&]() {
        std::map<double, double> out;
        const QVariantList v = model.chartData(windowMs, columns)
                                   .at(0)
                                   .toMap()
                                   .value("samples")
                                   .toList();
        for (int i = 0; i < v.size(); i += 2)
            out[v.at(i).toDouble()] = v.at(i + 1).toDouble();
        return out;
    };
    feed(20000);
    const std::map<double, double> before = points();
    feed(20130); /* one 100 ms update and a bit: not a slice's width */
    const std::map<double, double> after = points();

    /* The stretch both windows hold whole, a slice clear of either end */
    const double slice = static_cast<double>(windowMs) / columns;
    const double lo = 20120.0 - windowMs + slice;
    const double hi = 19990.0 - slice;
    const auto inside = [&](const std::map<double, double>& m) {
        std::map<double, double> out;
        for (const auto& p : m)
            if (p.first >= lo && p.first <= hi)
                out.insert(p);
        return out;
    };
    ASSERT_GT(inside(before).size(), 100u);
    EXPECT_EQ(inside(after), inside(before))
        << "the same points, slice for slice";
}

/* The Dashboard's trace (ADR-0057's note "a trace reads the model's
 * history"): one row's samples sliced as chartData slices them, for any row
 * whether Live plots it or not, over the window before its own newest sample */
TEST(BridgeTest, ATraceIsTheChartsSlicingForAnyRow)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    EXPECT_TRUE(model.trace(1001, 5000, 10).isEmpty()) << "no history yet";
    for (qint64 t = 0; t < 20000; t += 10) {
        const double wave = static_cast<double>(
            (t / 10) % 100 < 50 ? (t / 10) % 50 : 50 - (t / 10) % 50);
        model.updateValues(
            {{uint16_t(1001), wave}, {uint16_t(2001), double(t)}}, t);
    }
    const auto chartOf = [&model](int s, qint64 windowMs, int columns) {
        QList<double> out;
        for (const QVariant& v : model.chartData(windowMs, columns)
                                     .at(s)
                                     .toMap()
                                     .value("samples")
                                     .toList())
            out << v.toDouble();
        return out;
    };
    const std::pair<qint64, int> shapes[] = {
        {0, 0}, {5000, 0}, {5000, 10}, {5000, 300}, {120000, 40}};
    for (const auto& shape : shapes) {
        EXPECT_EQ(model.trace(1001, shape.first, shape.second),
                  chartOf(0, shape.first, shape.second))
            << shape.first << " ms, " << shape.second << " columns";
        EXPECT_EQ(model.trace(2001, shape.first, shape.second),
                  chartOf(1, shape.first, shape.second))
            << shape.first << " ms, " << shape.second << " columns";
    }

    const QList<double> plotted = model.trace(1001, 5000, 10);
    model.toggleSubscribed(0);
    ASSERT_EQ(model.chartData().size(), 1) << "Live no longer plots it";
    EXPECT_EQ(model.trace(1001, 5000, 10), plotted)
        << "the Dashboard still traces it";

    model.updateValues({{uint16_t(2001), 1.0}}, 30000);
    const QList<double> own = model.trace(1001, 5000, 0);
    ASSERT_FALSE(own.isEmpty());
    EXPECT_EQ(own.first(), 19990.0 - 5000.0)
        << "the window ends at the row's own newest";

    EXPECT_TRUE(model.trace(4242, 5000, 10).isEmpty()) << "no such def";
    EXPECT_TRUE(model.trace(-1, 5000, 10).isEmpty());
    EXPECT_TRUE(model.trace(1001 + 65536, 5000, 10).isEmpty())
        << "an id past 16 bits is no def's";
}

TEST(BridgeTest, ATextRowHasNoTrace)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    defs[1].kind = ECUIO_VALUE_HEX; /* id 2001, a text row */
    model.setSignalSet(&set);
    model.updateTexts({{uint16_t(2001), QStringLiteral("62 F1 90")}}, 10);
    model.updateValues({{uint16_t(1001), 5.0}}, 10);
    EXPECT_TRUE(model.trace(2001, 0, 0).isEmpty());
    EXPECT_EQ(model.trace(1001, 0, 0), (QList<double>{10, 5}));
}

/* A one-window spike (a fuel cut's lambda) keeps its height and its place
 * in every read while it is in the window, however the window has moved */
TEST(BridgeTest, ATraceKeepsAOneWindowSpikeInPlaceAsItScrolls)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto window = [](qint64 endMs, double high) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = 1.0;
        w.lastMs = endMs;
        w.min = 0.98;
        w.minMs = endMs - 70;
        w.max = high;
        w.maxMs = endMs - 40;
        return w;
    };
    const qint64 windowMs = 30000;
    const int columns = 100; /* slices of 300 ms, three windows in each */
    const qint64 spikeAt = 5100 - 40;
    int reads = 0;
    for (qint64 end = 100; end <= windowMs; end += 100) {
        model.updateWindow({window(end, end == 5100 ? 1.99 : 1.0)});
        if (end < 5100)
            continue;
        const QList<double> t = model.trace(1001, windowMs, columns);
        bool found = false;
        for (int i = 0; i + 1 < t.size(); i += 2)
            found =
                found || (t.at(i) == double(spikeAt) && t.at(i + 1) == 1.99);
        ASSERT_TRUE(found) << "the spike at its own time and height, read at "
                           << end;
        ++reads;
    }
    EXPECT_GT(reads, 200);
}

TEST(BridgeTest, ATraceIsInTheUnitSystem)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    model.setSignalSet(&set);
    model.updateValues({{uint16_t(1001), 100.0}}, 0);
    model.updateValues({{uint16_t(1001), 110.0}}, 100);
    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QList<double> mph = model.trace(1001, 0, 0);
    ASSERT_EQ(mph.size(), 4);
    EXPECT_NEAR(mph.at(1), 100.0 * 0.621371192237334, 1e-9);
    EXPECT_NEAR(mph.at(3), 110.0 * 0.621371192237334, 1e-9);
    model.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(model.trace(1001, 0, 0), (QList<double>{0, 100, 100, 110}));
}

/* Two minutes of windows, each with its lowest, highest and newest apart
 * (three points for each window), fit the ring: the longest trace is whole */
TEST(BridgeTest, TheRingHoldsTheLongestTracesWindow)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const qint64 longestMs = 120000;
    for (qint64 end = 0; end <= longestMs; end += 100) {
        SignalListModel::WindowValue w;
        w.id = 1001;
        w.last = static_cast<double>(end % 7);
        w.lastMs = end;
        w.min = -1.0;
        w.minMs = end - 60;
        w.max = 50.0;
        w.maxMs = end - 30;
        model.updateWindow({w});
    }
    const QList<double> t = model.trace(1001, longestMs, 0);
    ASSERT_FALSE(t.isEmpty());
    EXPECT_LE(t.first(), 100.0)
        << "the trace reaches back to its window's start";
    EXPECT_EQ(t.at(t.size() - 2), double(longestMs));
}

/* A trace breaks only where the ring stalls past the stale time, never on
 * its slices' spacing: a phone tile's 120 s over 40 columns (3 s slices)
 * draws a signal every 100 ms as one piece, and a lone sample between two
 * stalls keeps its value */
TEST(BridgeTest, ATraceBreaksOnlyWhereTheRingStalls)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    const auto breaks = [](const QList<double>& t) {
        int n = 0;
        for (int i = 1; i < t.size(); i += 2) {
            n += std::isnan(t.at(i)) ? 1 : 0;
        }
        return n;
    };
    qint64 t = 0;
    for (; t < 120000; t += 100) {
        model.updateValues({{uint16_t(1001), static_cast<double>(t % 900)}}, t);
    }
    const QList<double> narrow = model.trace(1001, 120000, 40);
    ASSERT_GT(narrow.size(), 40);
    EXPECT_EQ(breaks(narrow), 0) << "slices 3 s wide are no stall";

    const qint64 stall = SessionBridge::valueStaleMs() + 500;
    t += stall;
    model.updateValues({{uint16_t(1001), 1.99}}, t);
    t += stall;
    for (const qint64 end = t + 2000; t < end; t += 100) {
        model.updateValues({{uint16_t(1001), 1.0}}, t);
    }
    const QList<double> after = model.trace(1001, 120000, 40);
    EXPECT_EQ(breaks(after), 2) << "one break for each stall";
    bool lone = false;
    for (int i = 3; i + 2 < after.size(); i += 2) {
        lone = lone || (std::isnan(after.at(i - 2)) && after.at(i) == 1.99 &&
                        std::isnan(after.at(i + 2)));
    }
    EXPECT_TRUE(lone) << "the lone sample between the stalls, at its value";
    EXPECT_EQ(breaks(model.trace(1001, 120000, 0)), 2) << "unsliced too";
}

/* A value that isn't a number is a point of its own: the slice it falls in
 * still shows its lowest and highest number */
TEST(BridgeTest, AValueThatIsntANumberHidesNoSlicesExtremes)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    model.setSignalSet(&set);
    model.updateValues(
        {{uint16_t(1001), std::numeric_limits<double>::quiet_NaN()}}, 1000);
    model.updateValues({{uint16_t(1001), 1.0}}, 1100);
    model.updateValues({{uint16_t(1001), 9.0}}, 1200);
    model.updateValues({{uint16_t(1001), 5.0}}, 1300);
    model.updateValues({{uint16_t(1001), 4.0}}, 1400);
    /* one slice of 5 s holds them all */
    const QList<double> t = model.trace(1001, 5000, 1);
    QList<double> numbers;
    int breaks = 0;
    for (int i = 0; i + 1 < t.size(); i += 2) {
        if (std::isnan(t.at(i + 1))) {
            ++breaks;
            EXPECT_TRUE(numbers.isEmpty())
                << "the break comes first, at its time";
        } else {
            numbers << t.at(i) << t.at(i + 1);
        }
    }
    EXPECT_GE(breaks, 1);
    EXPECT_EQ(numbers, (QList<double>{1100, 1, 1200, 9, 1400, 4}))
        << "the slice's low, its high and the newest";
}

TEST(BridgeTest, UnplottedNamesReadAndSetTheChartsRows)
{
    SessionBridge bridge;
    SignalListModel* m = bridge.signalModel();
    int countNotified = 0;
    QObject::connect(m, &SignalListModel::subscribedCountChanged,
                     [&countNotified]() { ++countNotified; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    EXPECT_GE(countNotified, 1) << "a new set's count is notified";
    ASSERT_GE(m->rowCount(), 4);
    const auto name = [m](int row) {
        return m->data(m->index(row), SignalListModel::NameRole).toString();
    };
    const auto plotted = [m](int row) {
        return m->data(m->index(row), SignalListModel::SubscribedRole).toBool();
    };
    const int all = m->subscribedCount();
    EXPECT_TRUE(m->unplottedNames().isEmpty())
        << "every numeric row starts plotted";
    EXPECT_TRUE(m->plottedNames().isEmpty()) << "and none past its default";

    m->setPlotChoices({name(1)},
                      {name(0), name(2), name(1), QStringLiteral("Not a signal")});
    EXPECT_EQ(m->unplottedNames(), (QStringList{name(0), name(1), name(2)}))
        << "a name in both lists is unplotted";
    EXPECT_TRUE(m->plottedNames().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all - 3);
    EXPECT_FALSE(plotted(0));
    EXPECT_TRUE(plotted(3));

    m->setPlotChoices({}, {});
    EXPECT_TRUE(m->unplottedNames().isEmpty());
    EXPECT_EQ(m->subscribedCount(), all);

    m->toggleSubscribed(1);
    EXPECT_EQ(m->unplottedNames(), QStringList{name(1)})
        << "a click is read back";
    m->toggleSubscribed(1);
    EXPECT_TRUE(m->unplottedNames().isEmpty())
        << "a click back to the default clears the choice";
    EXPECT_TRUE(m->plottedNames().isEmpty()) << "rather than turning it over";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

namespace {

/* The names of the rows the chart plots, in set order */
QStringList plottedRows(const SignalListModel& model)
{
    QStringList names;
    for (int r = 0; r < model.rowCount(); ++r) {
        const QModelIndex i = model.index(r);
        if (model.data(i, SignalListModel::SubscribedRole).toBool()
                && !model.data(i, SignalListModel::HiddenRole).toBool()) {
            names << model.data(i, SignalListModel::NameRole).toString();
        }
    }
    return names;
}

/* The test car's first two ranges with PID 0x5C clear, as its
 * bitmaps read (docs/bench-sessions/2026-10-31-elm327-vehicle.md) */
ecuio_channel_support_t carWithoutOilTemp()
{
    ecuio_channel_support_t car{};
    car.flags = ECUIO_SUPPORT_PROBED;
    car.rx_id = 0x7E8;
    car.generation = 1;
    car.ranges = 0x7;
    const uint8_t bitmaps[12] = {0xFE, 0x7F, 0xA8, 0x13, 0xA0, 0x17,
                                 0xF0, 0x11, 0xFE, 0xD0, 0x8C, 0x81};
    memcpy(car.pid_bitmap, bitmaps, sizeof(bitmaps));
    return car;
}

} // namespace

/* ADR-0039 section 8: a ranked set plots the DefaultPlotted shown rows
 * of the lowest plot_rank; a row Live hides gives its place to the
 * next, and a rescan that shows it takes the place back */
TEST(BridgeTest, ARankedSetPlotsTheTopRankedRowsItShows)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    ASSERT_EQ(SignalListModel::DefaultPlotted, 6);
    const QStringList topSix = {
        QStringLiteral("Coolant Temp"),    QStringLiteral("RPM"),
        QStringLiteral("Vehicle Speed"),   QStringLiteral("Intake Air Temp"),
        QStringLiteral("MAF"),             QStringLiteral("Engine Oil Temp")};
    EXPECT_EQ(plottedRows(model), topSix) << "ranks 1 to 6, in set order";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad) << "rank 7 takes the hidden row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    ecuio_channel_support_t rescan = car;
    rescan.generation = 2;
    rescan.pid_bitmap[(0x5C - 1) / 8] |= 0x80u >> ((0x5C - 1) % 8);
    model.applyUnsupported(unsupportedIds(&rescan, 1));
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    /* A car with fewer ranked rows than that plots all it has */
    ecuio_channel_support_t sparse{};
    sparse.flags = ECUIO_SUPPORT_PROBED;
    sparse.rx_id = 0x7E8;
    sparse.generation = 3;
    sparse.ranges = 0x7; /* 0x01-0x60: only RPM and speed */
    sparse.pid_bitmap[(0x0C - 1) / 8] |= 0x80u >> ((0x0C - 1) % 8);
    sparse.pid_bitmap[(0x0D - 1) / 8] |= 0x80u >> ((0x0D - 1) % 8);
    model.applyUnsupported(unsupportedIds(&sparse, 1));
    EXPECT_EQ(plottedRows(model),
              (QStringList{QStringLiteral("RPM"), QStringLiteral("Vehicle Speed")}));
}

/* A refused row never has a value to draw: it gives its place among the
 * default plotted rows to the next, as a hidden one does, and takes it
 * back once it isn't refused; a tick still plots it. In a set that ranks
 * none, it is no default either */
TEST(BridgeTest, ARefusedRowGivesItsDefaultPlotToTheNext)
{
    SignalListModel model;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    model.setSignalSet(set);
    const QStringList topSix = plottedRows(model);
    int oilRow = -1;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (std::strcmp(set->defs[i].name, "Engine Oil Temp") == 0) {
            oilRow = static_cast<int>(i);
        }
    }
    ASSERT_GE(oilRow, 0);
    ASSERT_TRUE(topSix.contains(QStringLiteral("Engine Oil Temp")));
    const SignalListModel::Refusal oil{
        static_cast<uint16_t>(set->defs[oilRow].id),
        QStringLiteral("Refused: subFunctionNotSupported (7F 01 12)"),
        QStringLiteral("7F 01 12")};

    model.applyRefused({oil});
    QStringList withLoad = topSix;
    withLoad.removeOne(QStringLiteral("Engine Oil Temp"));
    withLoad.prepend(QStringLiteral("Engine Load"));
    EXPECT_EQ(plottedRows(model), withLoad)
        << "rank 7 takes the refused row's place";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);
    model.applyRefused({});
    EXPECT_EQ(plottedRows(model), topSix) << "and gives it back";

    model.applyRefused({oil});
    model.toggleSubscribed(oilRow);
    EXPECT_TRUE(plottedRows(model).contains(QStringLiteral("Engine Oil Temp")))
        << "a tick plots it all the same";

    SignalListModel plain;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t two;
    buildTestSignalSet(defs, two);
    plain.setSignalSet(&two);
    ASSERT_EQ(plottedRows(plain).size(), 2)
        << "an unranked set plots every row";
    plain.applyRefused(
        {{static_cast<uint16_t>(defs[1].id),
          QStringLiteral("Refused: requestOutOfRange (7F 22 31)"),
          QStringLiteral("7F 22 31")}});
    EXPECT_EQ(plottedRows(plain), QStringList{QStringLiteral("Engine_RPM")});
    EXPECT_EQ(plain.subscribedCount(), 1);
}

/* The user's ticks and unticks outlive the defaults moving, and a click
 * back to the default clears one */
TEST(BridgeTest, APlotChoiceOutlivesTheDefaultsMoving)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    const int pedal = obd2RowOf(ECUIO_OBD2_PID_SIG_ID(0x49, 0));
    ASSERT_GE(rpm, 0);
    ASSERT_GE(load, 0);
    ASSERT_GE(pedal, 0);
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };

    model.toggleSubscribed(rpm);
    model.toggleSubscribed(load);
    EXPECT_FALSE(plotted(rpm));
    EXPECT_TRUE(plotted(load));
    EXPECT_EQ(model.unplottedNames(), QStringList{QStringLiteral("RPM")});
    EXPECT_EQ(model.plottedNames(), QStringList{QStringLiteral("Engine Load")});
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted);

    /* Load's default turns on and RPM's stays on: neither choice moves */
    const ecuio_channel_support_t car = carWithoutOilTemp();
    model.applyUnsupported(unsupportedIds(&car, 1));
    EXPECT_FALSE(plotted(rpm)) << "an untick holds against its default";
    EXPECT_TRUE(plotted(load));
    EXPECT_FALSE(plotted(pedal)) << "an untick leaves its place empty";
    EXPECT_EQ(model.subscribedCount(), SignalListModel::DefaultPlotted - 1);

    model.toggleSubscribed(rpm);
    EXPECT_TRUE(model.unplottedNames().isEmpty()) << "back to its default";
    EXPECT_EQ(model.plottedNames(), QStringList{QStringLiteral("Engine Load")})
        << "with no choice of its own";

    /* A vehicle's lists, applied whole: every other row to its default */
    model.setPlotChoices({QStringLiteral("Accelerator Pedal D")}, {});
    EXPECT_TRUE(plotted(pedal));
    EXPECT_TRUE(plotted(load)) << "its default now, with no choice";
    EXPECT_TRUE(model.unplottedNames().isEmpty());
    EXPECT_EQ(model.plottedNames(),
              QStringList{QStringLiteral("Accelerator Pedal D")});

    model.setSignalSet(ecuio_obd2_signal_set());
    EXPECT_TRUE(model.plottedNames().isEmpty()) << "a new set starts with no choices";
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

/* The page's entry for an ECU (ADR-0037): the mock's report, named and
 * converted as the Diagnostics page shows it, in either unit system */
TEST(BridgeTest, DiagnosticsEntryIsWhatThePageShows)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ecuio_session_destroy(s);

    const QVariantMap e =
        SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("ecu").toString(), QStringLiteral("0x7E8"));
    EXPECT_TRUE(e.value("asked").toBool());
    EXPECT_TRUE(e.value("complete").toBool());
    EXPECT_EQ(e.value("readiness").toString(), QStringLiteral("answered"));
    EXPECT_TRUE(e.value("mil").toBool());
    EXPECT_EQ(e.value("dtcCount").toInt(), 3);
    EXPECT_FALSE(e.value("compression").toBool());
    EXPECT_EQ(e.value("cycle").toString(), QStringLiteral("answered"));

    const QVariantList lists = e.value("lists").toList();
    ASSERT_EQ(lists.size(), 3);
    const QStringList keys = {"stored", "pending", "permanent"};
    const QList<QStringList> codes = {
        {"P0301", "P0420", "U0100"}, {"P0171", "C0035", "B1001"}, {"P0420"}};
    for (int l = 0; l < 3; ++l) {
        const QVariantMap list = lists.at(l).toMap();
        EXPECT_EQ(list.value("key").toString(), keys.at(l));
        EXPECT_EQ(list.value("outcome").toString(), QStringLiteral("answered"));
        EXPECT_EQ(list.value("codes").toStringList(), codes.at(l));
        // Each code's title beside it, "" with none (ADR-0044)
        const QStringList listed = list.value("titles").toStringList();
        ASSERT_EQ(listed.size(), codes.at(l).size());
        for (int i = 0; i < listed.size(); ++i) {
            uint16_t raw = 0;
            ASSERT_EQ(
                ecuio_dtc_parse(codes.at(l).at(i).toLatin1().constData(), &raw),
                ECUIO_OK);
            EXPECT_EQ(listed.at(i), QString::fromLatin1(ecuio_dtc_title(raw)));
        }
        EXPECT_EQ(list.value("more").toInt(), 0);
    }

    const QVariantList monitors = e.value("monitors").toList();
    QStringList names;
    for (const QVariant& m : monitors) {
        names << m.toMap().value("name").toString();
        EXPECT_TRUE(m.toMap().value("complete").toBool());
        EXPECT_TRUE(m.toMap().value("cycleEnabled").toBool());
        EXPECT_TRUE(m.toMap().value("cycleComplete").toBool());
    }
    EXPECT_EQ(names,
              (QStringList{"Misfire", "Fuel system", "Comprehensive components",
                           "Catalyst", "Evaporative system", "Oxygen sensor",
                           "Oxygen sensor heater", "EGR and/or VVT system"}));

    const QVariantMap freeze = e.value("freeze").toMap();
    EXPECT_EQ(freeze.value("outcome").toString(), QStringLiteral("answered"));
    EXPECT_TRUE(freeze.value("stored").toBool());
    EXPECT_EQ(freeze.value("code").toString(), QStringLiteral("P0301"));
    const auto value = [](const QVariantMap& f, int i) {
        return f.value("values").toList().at(i).toMap();
    };
    ASSERT_EQ(freeze.value("values").toList().size(), 4);
    EXPECT_EQ(value(freeze, 0).value("name").toString(),
              QStringLiteral("Engine Load"));
    EXPECT_NEAR(value(freeze, 0).value("value").toDouble(), 0xB3 * 100.0 / 255,
                1e-9);
    EXPECT_EQ(value(freeze, 0).value("decimals").toInt(), 1);
    EXPECT_EQ(value(freeze, 1).value("name").toString(),
              QStringLiteral("Coolant Temp"));
    EXPECT_EQ(value(freeze, 1).value("value").toDouble(), 90.0);
    EXPECT_EQ(value(freeze, 1).value("unit").toString(),
              QString::fromUtf8("\xC2\xB0"
                                "C"));
    EXPECT_EQ(value(freeze, 2).value("name").toString(), QStringLiteral("RPM"));
    EXPECT_EQ(value(freeze, 2).value("value").toDouble(), 3000.0);
    EXPECT_EQ(value(freeze, 3).value("name").toString(),
              QStringLiteral("Vehicle Speed"));
    EXPECT_EQ(value(freeze, 3).value("unit").toString(),
              QStringLiteral("km/h"));

    /* The same report in imperial units: the values convert, as Live's do */
    const QVariantMap us =
        SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_IMPERIAL)
            .value("freeze")
            .toMap();
    EXPECT_EQ(value(us, 1).value("unit").toString(),
              QString::fromUtf8("\xC2\xB0"
                                "F"));
    EXPECT_NEAR(value(us, 1).value("value").toDouble(), 194.0, 1e-9);
    EXPECT_EQ(value(us, 3).value("unit").toString(), QStringLiteral("mph"));
    EXPECT_NEAR(value(us, 3).value("value").toDouble(), 80.0 / 1.609344, 1e-9);

    /* Refusals carry their reply; an unasked report says so */
    ecuio_channel_diagnostics_t old{};
    old.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE;
    old.rx_id = 0x7E9;
    old.readiness = ECUIO_DIAG_REFUSED;
    old.readiness_nrc = 0x12;
    old.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_REFUSED;
    old.lists[ECUIO_DIAG_PERMANENT].nrc = 0x11;
    old.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    old.lists[ECUIO_DIAG_STORED].reported = 40;
    old.lists[ECUIO_DIAG_STORED].held = 1;
    old.lists[ECUIO_DIAG_STORED].codes[0] = 0xC100;
    old.freeze = ECUIO_DIAG_UNANSWERED;
    const QVariantMap o =
        SessionBridge::diagnosticsEntry(old, ECUIO_UNITS_METRIC);
    EXPECT_EQ(o.value("ecu").toString(), QStringLiteral("0x7E9"));
    EXPECT_EQ(o.value("readiness").toString(), QStringLiteral("refused"));
    EXPECT_EQ(o.value("readinessNrc").toString(), QStringLiteral("7F 01 12"));
    EXPECT_EQ(o.value("lists").toList().at(0).toMap().value("more").toInt(),
              39);
    EXPECT_EQ(
        o.value("lists").toList().at(1).toMap().value("outcome").toString(),
        QStringLiteral("unasked"));
    EXPECT_EQ(o.value("lists").toList().at(2).toMap().value("nrc").toString(),
              QStringLiteral("7F 0A 11"));
    EXPECT_EQ(o.value("freeze").toMap().value("outcome").toString(),
              QStringLiteral("unanswered"));
    EXPECT_FALSE(SessionBridge::diagnosticsEntry(ecuio_channel_diagnostics_t{},
                                                 ECUIO_UNITS_METRIC)
                     .value("asked")
                     .toBool());
}

/* Bridge.diagnostics follows the session: the mock's report after the
 * first tick, converted again when the unit system changes, kept after
 * disconnect, emptied at the next connect, and Read again moves it on */
TEST(BridgeTest, DiagnosticsFollowTheSession)
{
    SessionBridge bridge;
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_TRUE(bridge.diagnostics().isEmpty());
    bridge.readDiagnostics(); // no session: nothing happens
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    const auto speedUnit = [&bridge]() {
        return bridge.diagnostics()
            .at(0)
            .toMap()
            .value("freeze")
            .toMap()
            .value("values")
            .toList()
            .at(3)
            .toMap()
            .value("unit")
            .toString();
    };
    EXPECT_EQ(speedUnit(), QStringLiteral("km/h"));
    // the mock's light is on: the car's verdict (ADR-0045)
    EXPECT_EQ(bridge.inspection().value("verdict").toString(),
              QStringLiteral("mil_on"));
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(speedUnit(), QStringLiteral("mph"));
    bridge.setUnitSystem(ECUIO_UNITS_METRIC);

    const auto lines = [&log]() {
        return log.filter(QStringLiteral(" diagnostics: "));
    };
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 1; }, 2000));
    bridge.readDiagnostics();
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 2; }, 2000))
        << "Read again logs a second read";

    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.diagnostics().size(), 1) << "the last read, kept";
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo?diagnose=0"))
                    .isEmpty());
    EXPECT_TRUE(bridge.diagnostics().isEmpty()) << "emptied at connect";
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    EXPECT_FALSE(bridge.diagnostics().at(0).toMap().value("asked").toBool());
    EXPECT_TRUE(bridge.inspection().value("verdict").toString().isEmpty())
        << "nothing read, nothing judged";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A clear's line for each outcome (ADR-0038), as the activity log
 * and a recording's events.log keep it, a speed in the unit system */
TEST(BridgeTest, ChannelClearTextNamesEachOutcome)
{
    ecuio_channel_diagnostics_t d{};
    d.rx_id = 0x7E8;
    const auto text = [&d](uint8_t clear, uint8_t nrc, uint32_t rpm) {
        d.clear = clear;
        d.clear_nrc = nrc;
        d.clear_rpm = rpm;
        return SessionBridge::channelClearText(d, ECUIO_UNITS_METRIC)
            .toStdString();
    };
    const auto imperial = [&d](uint8_t clear, uint32_t rpm) {
        d.clear = clear;
        d.clear_nrc = 0;
        d.clear_rpm = rpm;
        return SessionBridge::channelClearText(d, ECUIO_UNITS_IMPERIAL)
            .toStdString();
    };
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0), "ECU 0x7E8 clear: cleared");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 0),
              "ECU 0x7E8 clear: refused (7F 04 22)");
    EXPECT_EQ(text(ECUIO_CLEAR_UNANSWERED, 0, 0), "ECU 0x7E8 clear: no answer");
    EXPECT_EQ(text(ECUIO_CLEAR_ENGINE_RUNNING, 0, 812),
              "ECU 0x7E8 clear: not sent: engine running (812 rpm)");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_ENGINE_SPEED, 0, 0),
              "ECU 0x7E8 clear: not sent: no engine speed");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_ENGINE_SPEED, 0x12, 0),
              "ECU 0x7E8 clear: not sent: no engine speed (7F 01 12)");
    EXPECT_EQ(text(ECUIO_CLEAR_REPORT_CHANGED, 0, 0),
              "ECU 0x7E8 clear: not sent: the report changed");
    EXPECT_EQ(text(ECUIO_CLEAR_NONE, 0, 0), "ECU 0x7E8 clear: not asked");

    /* Under the user's word (ADR-0038's appendix of 2026-11-01): what was
     * read, the vehicle's speed, and why an ECU refused a running engine */
    d.clear_kmh = 48;
    EXPECT_EQ(text(ECUIO_CLEAR_MOVING, 0, 2000),
              "ECU 0x7E8 clear: not sent: moving (48 km/h)");
    EXPECT_EQ(imperial(ECUIO_CLEAR_MOVING, 2000),
              "ECU 0x7E8 clear: not sent: moving (30 mph)");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_VEHICLE_SPEED, 0, 0),
              "ECU 0x7E8 clear: not sent: no vehicle speed");
    EXPECT_EQ(text(ECUIO_CLEAR_NO_VEHICLE_SPEED, 0x12, 0),
              "ECU 0x7E8 clear: not sent: no vehicle speed (7F 01 12)");
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 812),
              "ECU 0x7E8 clear: cleared (812 rpm, 48 km/h)");
    EXPECT_EQ(imperial(ECUIO_CLEAR_CLEARED, 812),
              "ECU 0x7E8 clear: cleared (812 rpm, 30 mph)");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 812),
              "ECU 0x7E8 clear: refused (7F 04 22): this ECU won't clear with "
              "the engine running: stop and turn it off, then try again");
    EXPECT_EQ(text(ECUIO_CLEAR_REFUSED, 0x22, 0),
              "ECU 0x7E8 clear: refused (7F 04 22)")
        << "at 0 rpm, not the engine";
    d.flags = ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0),
              "ECU 0x7E8 clear: cleared (engine speed unknown, 48 km/h)");
    d.flags = ECUIO_DIAG_CLEAR_RPM;
    EXPECT_EQ(text(ECUIO_CLEAR_CLEARED, 0, 0), "ECU 0x7E8 clear: cleared")
        << "no vehicle speed asked: the stopped engine's clear";
    d.flags = 0;
    d.clear_kmh = 0;

    /* The page's entry carries the same */
    d.clear = ECUIO_CLEAR_REFUSED;
    d.clear_nrc = 0x22;
    QVariantMap e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("refused"));
    EXPECT_EQ(e.value("clearNrc").toString(), QStringLiteral("7F 04 22"));
    EXPECT_FALSE(e.value("clearing").toBool());
    d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
    d.clear_nrc = 0;
    d.clear_rpm = 812;
    d.flags = ECUIO_DIAG_CLEARING;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("engineRunning"));
    EXPECT_EQ(e.value("clearRpm").toInt(), 812);
    EXPECT_TRUE(e.value("clearing").toBool());
    EXPECT_FALSE(e.value("clearRunning").toBool()) << "no reading flagged";
    EXPECT_EQ(e.value("clearSpeed").toString(), QString());
    d.clear = ECUIO_CLEAR_MOVING;
    d.clear_kmh = 48;
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("moving"));
    EXPECT_TRUE(e.value("clearRunning").toBool());
    EXPECT_EQ(e.value("clearSpeed").toString(), QStringLiteral("30 mph"));
    d.clear = ECUIO_CLEAR_NO_VEHICLE_SPEED;
    d.clear_nrc = 0x12;
    e = SessionBridge::diagnosticsEntry(d, ECUIO_UNITS_METRIC);
    EXPECT_EQ(e.value("clear").toString(), QStringLiteral("noVehicleSpeed"));
    EXPECT_EQ(e.value("clearNrc").toString(), QStringLiteral("7F 01 12"));
}

/* The saved file is havne-cli's: a start line, then each report as its
 * diagnostics line, here the mock's before any clear */
TEST(BridgeTest, DiagnosticsFileTextIsTheCliLines)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://file", &s), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(s, 0, &d), ECUIO_OK);
    ecuio_session_destroy(s);

    const QList<QByteArray> lines =
        SessionBridge::diagnosticsFileText(&d, 1).split('\n');
    ASSERT_EQ(lines.size(), 3) << "two lines, each ended";
    EXPECT_TRUE(lines.at(2).isEmpty());
    const QJsonObject start = QJsonDocument::fromJson(lines.at(0)).object();
    EXPECT_EQ(start.value("type").toString(), QStringLiteral("start"));
    EXPECT_EQ(start.value("schema").toInt(), 1);
    EXPECT_EQ(start.value("core").toString(),
              QString::fromUtf8(ecuio_version_string()));
    const QJsonObject line = QJsonDocument::fromJson(lines.at(1)).object();
    EXPECT_EQ(line.value("type").toString(), QStringLiteral("diagnostics"));
    EXPECT_EQ(line.value("rx").toString(), QStringLiteral("0x7E8"));
    EXPECT_EQ(line.value("generation").toInt(), static_cast<int>(d.generation));
    EXPECT_TRUE(line.value("readiness").toObject().value("mil").toBool());
    const QJsonArray stored =
        line.value("stored").toObject().value("codes").toArray();
    ASSERT_EQ(stored.size(), 3);
    EXPECT_EQ(stored.at(0).toString(), QStringLiteral("P0301"));
    EXPECT_EQ(line.value("freeze").toObject().value("code").toString(),
              QStringLiteral("P0301"));
}

/* ADR-0038 on the mock, which starts each loop with its engine stopped:
 * the report goes to a file of its own first (no vehicle in a mock's
 * name), then the clear, its line, and the read after it, the only
 * other diagnostics line */
/* ADR-0038: the one rule for a clear's ECUs, which the confirmation
 * names (clearTargets) and clearDiagnostics sends to: a completed read,
 * no clear under way */
TEST(BridgeTest, AClearGoesToCompletedReportsNotClearing)
{
    ecuio_channel_diagnostics_t r{};
    EXPECT_FALSE(SessionBridge::clearTarget(r)) << "a read under way";
    r.flags = ECUIO_DIAG_COMPLETE;
    EXPECT_TRUE(SessionBridge::clearTarget(r));
    r.flags = ECUIO_DIAG_COMPLETE | ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(SessionBridge::clearTarget(r)) << "a clear under way";
    r.flags = ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(SessionBridge::clearTarget(r));

    SessionBridge bridge;
    EXPECT_TRUE(bridge.clearTargets().isEmpty()) << "no session, no report";
}

/* A clear with nothing to erase still goes out (a reset after a repair
 * is a use), and the confirmation says so: only what the ECU answered
 * counts as empty */
TEST(BridgeTest, NothingStoredIsWhatTheEcuAnsweredEmpty)
{
    ecuio_channel_diagnostics_t r{};
    r.flags = ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS;
    r.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PERMANENT].outcome = ECUIO_DIAG_ANSWERED;
    r.lists[ECUIO_DIAG_PERMANENT].reported = 1;
    r.lists[ECUIO_DIAG_PERMANENT].held = 1;
    EXPECT_TRUE(SessionBridge::nothingStored(r))
        << "a permanent code isn't a clear's to erase";

    const auto with = [&r](auto change) {
        ecuio_channel_diagnostics_t c = r;
        change(c);
        return SessionBridge::nothingStored(c);
    };
    EXPECT_FALSE(with([](auto& c) { c.flags |= ECUIO_DIAG_MIL_ON; }));
    EXPECT_FALSE(with([](auto& c) { c.dtc_count = 1; }));
    EXPECT_FALSE(with([](auto& c) { c.flags |= ECUIO_DIAG_FREEZE_FRAME; }));
    EXPECT_FALSE(
        with([](auto& c) { c.lists[ECUIO_DIAG_STORED].reported = 2; }));
    EXPECT_FALSE(with([](auto& c) { c.lists[ECUIO_DIAG_PENDING].held = 1; }));
    EXPECT_FALSE(with([](auto& c) { c.flags &= ~ECUIO_DIAG_READINESS; }))
        << "PID 01 unanswered: the light unknown";
    EXPECT_FALSE(with([](auto& c) {
        c.lists[ECUIO_DIAG_STORED].outcome = ECUIO_DIAG_REFUSED;
    })) << "a refused list isn't known to be empty";
    EXPECT_FALSE(with([](auto& c) {
        c.lists[ECUIO_DIAG_PENDING].outcome = ECUIO_DIAG_UNANSWERED;
    }));

    SessionBridge bridge;
    EXPECT_FALSE(bridge.clearFindsNothing()) << "no report: nothing known";
}

TEST(BridgeTest, AClearSavesTheReportFirst)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setVehicleLabel(QStringLiteral("Daily driver"));
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto diagnostics_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 diagnostics: "));
    };
    const auto clear_lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 clear: "));
    };
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 1; }, 2000));

    const QString shown = bridge.clearDiagnostics();
    const QStringList files =
        QDir(root.path()).entryList({QStringLiteral("*.jsonl")}, QDir::Files);
    ASSERT_EQ(files.size(), 1) << files.join(',').toStdString();
    EXPECT_TRUE(QRegularExpression(
                    QStringLiteral("^diagnostics-[0-9]{8}-[0-9]{6}\\.jsonl$"))
                    .match(files.first())
                    .hasMatch())
        << files.first().toStdString();
    EXPECT_TRUE(shown.endsWith(files.first())) << shown.toStdString();
    QFile saved(root.path() + QLatin1Char('/') + files.first());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = saved.readAll().split('\n');
    ASSERT_GE(lines.size(), 2);
    EXPECT_TRUE(
        lines.at(1).contains("\"codes\":[\"P0301\",\"P0420\",\"U0100\"]"))
        << "the codes before the clear: " << lines.at(1).toStdString();

    ASSERT_TRUE(
        spinUntil([&]() { return diagnostics_lines().size() == 2; }, 2000))
        << log.join('\n').toStdString();
    spinLoop(300);
    ASSERT_EQ(clear_lines().size(), 1) << log.join('\n').toStdString();
    EXPECT_EQ(clear_lines().first().toStdString(),
              "ECU 0x7E8 clear: cleared (0 rpm, 0 km/h)")
        << "the mock's key-on: engine stopped, car standing";
    EXPECT_EQ(diagnostics_lines().size(), 2);
    const QString after = diagnostics_lines().at(1);
    EXPECT_TRUE(after.contains(
        QStringLiteral("MIL off; stored none; pending none; permanent P0420")))
        << after.toStdString();
    const QStringList saved_lines =
        log.filter(QStringLiteral("Diagnostics saved before the clear: "));
    ASSERT_EQ(saved_lines.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(saved_lines.first().endsWith(shown));
    EXPECT_LT(log.indexOf(saved_lines.first()),
              log.indexOf(clear_lines().first()));
    EXPECT_LT(log.indexOf(clear_lines().first()), log.indexOf(after));
    EXPECT_EQ(bridge.diagnostics().at(0).toMap().value("clear").toString(),
              QStringLiteral("cleared"));
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty()) << "no session";
}

/* A report that can't be saved sends no clear: the root is under a
 * file, so no folder can be made */
TEST(BridgeTest, AFailedSaveSendsNoClear)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString file = root.path() + QStringLiteral("/not-a-folder");
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(file + QStringLiteral("/recordings"));
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty());
    EXPECT_TRUE(log.isEmpty()) << "no session: nothing to say";
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    EXPECT_TRUE(bridge.clearDiagnostics().isEmpty());
    spinLoop(300);
    EXPECT_EQ(log.filter(QStringLiteral(
                             "Clear not sent: the report couldn't be saved "
                             "(can't create "))
                  .size(),
              1)
        << log.join('\n').toStdString();
    EXPECT_TRUE(log.filter(QStringLiteral(" clear: ")).isEmpty());
    EXPECT_EQ(log.filter(QStringLiteral(" diagnostics: ")).size(), 1);
    EXPECT_EQ(bridge.diagnostics().at(0).toMap().value("clear").toString(),
              QStringLiteral("none"));
    ASSERT_TRUE(disconnectAndWait(bridge));
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
/* A sleep while connected (ADR-0033 section 6) is one activity line saying how
 * long; a rise under a second, one before the connect, or one after the
 * disconnect says nothing. The line is never a note in the recording:
 * core writes its own asleep line (ADR-0049 section 2,
 * RecorderTest.ASleepIsAnAsleepLineWithItsLength). No runner can sleep,
 * so the seam adds to the count the bridge's tick reads */
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
    ASSERT_FALSE(events.isEmpty());
    EXPECT_TRUE(std::none_of(
        events.cbegin(), events.cend(),
        [](const QByteArray& e) { return e.contains("Asleep for"); }))
        << "the activity line is no note";
}

/* A replay with faults injected (ADR-0041) still finishes, and says what
 * the faults did; the Session line says they were injected; the
 * recording holds the frames delivered, row for row, and the overflows
 * the bridge counts are the frames flagged in it. Seed 3 over the engine
 * start: the CLI's golden, replay-faults.jsonl, has the same counts.
 * Paced at 100x: at full speed the worker can outrun the GUI thread,
 * and frames dropped at the bridge's gate would leave rows out. */
TEST(BridgeTest, AFaultedReplayFinishesAndRecordsWhatItDelivered)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecording(true);
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri =
        QStringLiteral("replay://") + QString::fromUtf8(ECUIO_REPLAY_FIXTURES) +
        QStringLiteral("/2026-10-25-engine-start?profile=obd2&speed=100&"
                       "faults=drop:10,overflow:5,corrupt:2,truncate:2,"
                       "stall:1:5&seed=3");
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QString finished = QStringLiteral(
        "Replay finished: 7074 frames, faults injected: 735 dropped, 296 lost "
        "to an overflow, 119 corrupted, 109 cut short, 70 held back; the "
        "session stays open");
    ASSERT_TRUE(spinUntil(
        [&log, &finished]() { return log.contains(finished); }, 20000))
        << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(log.filter(QStringLiteral("frames over 297.4 s at 100x, with "
                                           "faults injected"))
                     .isEmpty())
        << log.join(QLatin1Char('\n')).toStdString();
    spinLoop(300);
    ASSERT_EQ(bridge.droppedFrames(), 0)
        << "the rows below are whole only without drops";
    const int overflows = bridge.overflowFrames();
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList recorded = recordingFolders(root.path());
    ASSERT_EQ(recorded.size(), 1);
    const QList<QByteArray> rows =
        fileLines(root.path() + QStringLiteral("/") + recorded.first() +
                  QStringLiteral("/frames.csv"));
    ASSERT_EQ(rows.size(), 1 + 6043) << "the header, then each frame delivered";
    int flagged = 0;
    for (qsizetype i = 1; i < rows.size(); ++i) {
        const QList<QByteArray> cols = rows[i].split(',');
        ASSERT_GE(cols.size(), 4) << i;
        bool ok = false;
        const uint32_t flags = cols[3].toUInt(&ok, 0); /* 0x80 */
        ASSERT_TRUE(ok) << cols[3].toStdString();
        flagged += (flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0 ? 1 : 0;
    }
    EXPECT_GT(flagged, 0);
    EXPECT_LE(flagged, 296) << "losses in a row flag one frame";
    EXPECT_EQ(overflows, flagged);
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

/* A row says how long it is, whether it is a log of chosen signals or a
 * whole session, its signal count and its marks */
TEST(BridgeTest, ARecordingsRowSaysItsLengthKindSignalsAndMarks)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto make = [&root](const char* name, bool log, int marks) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
        QFile events(dir + QStringLiteral("/events.log"));
        ASSERT_TRUE(events.open(QIODevice::WriteOnly));
        events.write("ecuio session recording, format 2\n"
                     "uri  mock://demo\n"
                     "signal  A,number,km/h,1,1,1\n"
                     "signal  B,number,%,1,2,2\n");
        if (log) {
            events.write("focus  A,B\n");
        }
        events.write("columns  local time, seconds\n\n"
                     "09:00:25.000  +25.500  end  x\n");
        if (marks > 0) {
            QFile m(dir + QStringLiteral("/markers.csv"));
            ASSERT_TRUE(m.open(QIODevice::WriteOnly));
            m.write("host_us,label\n");
            for (int i = 0; i < marks; ++i) {
                m.write("1000,m\n");
            }
        }
    };
    make("20261110-090000", true, 0);
    make("20261111-090000", false, 2);
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 2);
    const QVariantMap session = rows.at(0).toMap();
    const QVariantMap log = rows.at(1).toMap();
    EXPECT_EQ(session.value(QStringLiteral("durationMs")).toLongLong(), 25500);
    EXPECT_EQ(session.value(QStringLiteral("signals")).toInt(), 2);
    EXPECT_FALSE(session.value(QStringLiteral("log")).toBool());
    EXPECT_EQ(session.value(QStringLiteral("marks")).toInt(), 2);
    EXPECT_TRUE(log.value(QStringLiteral("log")).toBool());
    EXPECT_EQ(log.value(QStringLiteral("marks")).toInt(), 0);
}

/* A requesting session's recording with a set of conditions and a default
 * pick: its header holds conditions, pick, min sweep and link lines among
 * those the row reads (core's begins "co" too). The row is a session's,
 * not a log's, with every signal counted and the uri read */
TEST(BridgeTest, ARowReadsPastTheConditionsPickAndMinSweepLines)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir = root.path() + QStringLiteral("/20261114-090000");
    ASSERT_TRUE(QDir().mkpath(dir));
    {
        QFile frames(dir + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
        QFile events(dir + QStringLiteral("/events.log"));
        ASSERT_TRUE(events.open(QIODevice::WriteOnly));
        events.write("ecuio session recording, format 3 (ADR-0049)\n"
                     "started  2026-11-14 09:00:00.000 (local time)\n"
                     "core  0.8.0\n"
                     "uri  elm327://COM5?profile=obd2\n"
                     "profile  obd2\n"
                     "conditions  conditions.json,");
        events.write(QByteArray(64, '0'));
        events.write("\n"
                     "frame flags  complete\n"
                     "pick  \"A\",\"B\"\n"
                     "min sweep  100 ms\n"
                     "link  healthy (state 0, 0 episodes)\n"
                     "signal  A,number,km/h,1,1,1\n"
                     "signal  B,number,%,1,2,2\n"
                     "signal  C,number,C,0,0,3\n"
                     "columns  local time, seconds\n\n"
                     "09:00:25.000  +25.500  end  x\n");
    }
    SessionBridge bridge;
    const QVariantMap row = bridge.recordingRow(dir);
    ASSERT_FALSE(row.isEmpty());
    EXPECT_FALSE(row.value(QStringLiteral("log")).toBool())
        << "a default pick is no log's focus";
    EXPECT_EQ(row.value(QStringLiteral("signals")).toInt(), 3);
    const QString uri = row.value(QStringLiteral("uri")).toString();
    EXPECT_TRUE(uri.startsWith(QStringLiteral("replay://")) &&
                uri.endsWith(QStringLiteral("/20261114-090000?profile=obd2")))
        << uri.toStdString();
    EXPECT_EQ(row.value(QStringLiteral("source")).toString(),
              QStringLiteral("elm327://COM5?profile=obd2"));
    EXPECT_EQ(row.value(QStringLiteral("durationMs")).toLongLong(), 25500);
}

// A format 3 header's "signal file" line is no signal, and a header holds a
// line for each of its signals, past 400 lines
TEST(BridgeTest, ARecordingsRowCountsASignalFilesSignalsOnly)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir = root.path() + QStringLiteral("/20261112-090000");
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile frames(dir + QStringLiteral("/frames.csv"));
    ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
    QFile events(dir + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    events.write(
        "ecuio session recording, format 3 (ADR-0049)\n"
        "uri  mock://demo\n"
        "signal file  signals.json,"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "\n");
    for (int i = 0; i < 500; ++i) {
        events.write("signal  S" + QByteArray::number(i) +
                     ",number,km/h,1,1,1\n");
    }
    events.write("columns  local time, seconds\n\n"
                 "09:00:25.000  +25.500  end  x\n");
    events.close();
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).toMap().value(QStringLiteral("signals")).toInt(), 500);
}

// A focus line far longer than one read names picks whose quoted names hold
// "columns" and "signal  " after spaces: a later part of it, trimmed, starts
// with one of them wherever the parts fall, and still counts as no line
TEST(BridgeTest, ARecordingsRowJudgesOnlyALinesStart)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir = root.path() + QStringLiteral("/20261113-090000");
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile frames(dir + QStringLiteral("/frames.csv"));
    ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
    QByteArray focus = "focus  ";
    for (int i = 0; focus.size() < 40000; ++i) {
        const QByteArray pad(1 + i % 18, ' ');
        focus += (i == 0 ? "\"" : ",\"") + pad +
                 (i % 2 == 0 ? "columns, x" : "signal  y, z") + "\"";
    }
    // The case's premise: parts of 1099 or 1100 bytes both start with them
    for (const int part : {1099, 1100}) {
        int words = 0;
        for (int at = part; at < focus.size(); at += part) {
            const QByteArray rest = focus.mid(at).trimmed();
            words += rest.startsWith("columns") || rest.startsWith("signal  ");
        }
        EXPECT_GE(words, 2) << part;
    }
    QFile events(dir + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    events.write("ecuio session recording, format 3 (ADR-0049)\n"
                 "uri  mock://demo\n" +
                 focus + "\n");
    for (int i = 0; i < 5; ++i) {
        events.write("signal  S" + QByteArray::number(i) +
                     ",number,km/h,1,1,1\n");
    }
    events.write("columns  local time, seconds\n\n"
                 "09:00:25.000  +25.500  end  x\n");
    events.close();
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 1);
    const QVariantMap row = rows.at(0).toMap();
    EXPECT_EQ(row.value(QStringLiteral("signals")).toInt(), 5);
    EXPECT_TRUE(row.value(QStringLiteral("log")).toBool());
}

#ifdef _WIN32

// A read that fails before the file's end (its bytes locked by another
// handle) ends the header's read: the row comes back, no signal counted
TEST(BridgeTest, ARecordingsRowEndsAtAReadThatFails)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir = root.path() + QStringLiteral("/20261113-100000");
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile frames(dir + QStringLiteral("/frames.csv"));
    ASSERT_TRUE(frames.open(QIODevice::WriteOnly));
    frames.close();
    const QString path = dir + QStringLiteral("/events.log");
    {
        QFile events(path);
        ASSERT_TRUE(events.open(QIODevice::WriteOnly));
        events.write("ecuio session recording, format 3 (ADR-0049)\n"
                     "uri  mock://demo\n");
        for (int i = 0; i < 50; ++i) {
            events.write("signal  S" + QByteArray::number(i) +
                         ",number,km/h,1,1,1\n");
        }
        events.write("columns  local time, seconds\n\n");
    }
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE locker = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locker, INVALID_HANDLE_VALUE);
    OVERLAPPED whole{};
    ASSERT_TRUE(LockFileEx(locker,
                           LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                           0, MAXDWORD, MAXDWORD, &whole));
    {
        // What the row's read meets: nothing read, and not at the end
        QFile probe(path);
        ASSERT_TRUE(probe.open(QIODevice::ReadOnly | QIODevice::Text));
        EXPECT_TRUE(probe.readLine(1100).isEmpty());
        EXPECT_FALSE(probe.atEnd());
    }
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    std::atomic<bool> done{false};
    std::atomic<int> counted{-1};
    std::thread reader([&] {
        counted = bridge.recordingRow(dir)
                      .value(QStringLiteral("signals"), -2)
                      .toInt();
        done = true;
    });
    QElapsedTimer waited;
    waited.start();
    while (!done && waited.elapsed() < 20000) {
        QThread::msleep(10);
    }
    if (!done) {
        // The read spins on: nothing can stop it, so the process ends here
        ADD_FAILURE() << "the header's read never ended";
        std::fflush(stdout);
        std::_Exit(1);
    }
    reader.join();
    UnlockFileEx(locker, 0, MAXDWORD, MAXDWORD, &whole);
    CloseHandle(locker);
    EXPECT_EQ(counted.load(), 0);
}

#endif

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
    EXPECT_EQ(field(0, "folder"),
              root.path().toStdString() + "/20261028-143012-2 Daily driver");
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

/* The list takes what the viewer opens, the rule recordingRow keeps: a
 * folder holding a signals.csv (a CSV import's values alone, ADR-0054's
 * appendix "the CSV import as built") or a frames.csv; each row says
 * whether it has frames to replay */
TEST(BridgeTest, TheRecordingsListTakesAFolderOfValuesAlone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto make = [&root](const char* name,
                              std::initializer_list<const char*> files) {
        const QString dir =
            root.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        for (const char* f : files) {
            QFile file(dir + QLatin1Char('/') + QString::fromUtf8(f));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        }
    };
    make("20261107-090000", {"signals.csv", "events.log"});
    make("20261106-090000", {"frames.csv"});
    make("20261105-090000", {"frames.csv", "signals.csv"});
    make("20261104-090000", {"events.log"});
    make("20261103-090000", {});

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantList rows = bridge.recentRecordings(10);
    ASSERT_EQ(rows.size(), 3) << "an events.log alone or nothing isn't one";
    const auto row = [&rows](int i) { return rows.at(i).toMap(); };
    EXPECT_EQ(row(0).value(QStringLiteral("title")).toString(),
              QStringLiteral("2026-11-07 09:00:00"));
    EXPECT_FALSE(row(0).value(QStringLiteral("frames")).toBool())
        << "values alone: nothing to replay";
    EXPECT_TRUE(row(1).value(QStringLiteral("frames")).toBool());
    EXPECT_TRUE(row(2).value(QStringLiteral("frames")).toBool());
    for (int i = 0; i < rows.size(); ++i) {
        const QString folder =
            row(i).value(QStringLiteral("folder")).toString();
        EXPECT_EQ(bridge.recordingRow(folder), row(i))
            << "the list's rule is recordingRow's: " << folder.toStdString();
    }
    EXPECT_TRUE(
        bridge.recordingRow(root.path() + QStringLiteral("/20261104-090000"))
            .isEmpty());
}

/* A vehicle's baseline (ADR-0049's appendix of 2026-11-06): one folder's
 * row, its title and URI as the list gives them, its folder as stored,
 * while it holds a signals.csv or a frames.csv; nothing once it's gone */
TEST(BridgeTest, ARecordingRowIsTheListsRowWhileItsFolderHoldsItsValues)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir =
        root.path() + QStringLiteral("/20261106-091500 Weekend car");
    ASSERT_TRUE(QDir().mkpath(dir));
    for (const char* name : {"/frames.csv", "/signals.csv"}) {
        QFile f(dir + QString::fromUtf8(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    QFile events(dir + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    events.write("ecuio session recording, format 3 (ADR-0049)\n"
                 "uri  mock://demo\n");
    events.close();

    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    const QVariantMap listed = bridge.recentRecordings(1).value(0).toMap();
    const QVariantMap row = bridge.recordingRow(dir);
    EXPECT_EQ(row, listed);
    EXPECT_EQ(row.value(QStringLiteral("title")).toString().toStdString(),
              "2026-11-06 09:15:00 \xC2\xB7 Weekend car");
    EXPECT_EQ(row.value(QStringLiteral("profile")).toString(),
              QStringLiteral("mock"));

    EXPECT_TRUE(bridge.recordingRow(QString()).isEmpty());
    ASSERT_TRUE(QFile::remove(dir + QStringLiteral("/signals.csv")));
    EXPECT_EQ(bridge.recordingRow(dir), row) << "its frames still open";
    ASSERT_TRUE(QFile::remove(dir + QStringLiteral("/frames.csv")));
    EXPECT_TRUE(bridge.recordingRow(dir).isEmpty()) << "nothing to open";
    ASSERT_TRUE(QDir(dir).removeRecursively());
    EXPECT_TRUE(bridge.recordingRow(dir).isEmpty()) << "the folder is gone";
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
 * value, formatted as core's recorder writes them */
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

#ifdef _WIN32

/* A log on a session that asks takes the minimum, and its line and its
 * header say so (ADR-0056 section 4) */
TEST(BridgeTest, ALogOnARequestingSessionNamesItsMinimumTime)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setMinSweepMs(250);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 2; }, 5000));
    SignalListModel* model = bridge.signalModel();
    std::vector<uint16_t> picks;
    for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
        const QModelIndex i = model->index(r);
        if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
            picks.push_back(static_cast<uint16_t>(
                model->data(i, SignalListModel::IdRole).toUInt()));
        }
    }
    ASSERT_EQ(picks.size(), 2u);
    plotOnly(model, picks);

    ASSERT_TRUE(bridge.startLog()) << log.join('\n').toStdString();
    EXPECT_TRUE(log.contains(
        QStringLiteral(
            "Log started: %1 (2 signals, at least 250 ms between sweeps)")
            .arg(bridge.recordingFolder())))
        << log.join('\n').toStdString();
    const QString folder = bridge.recordingFolderUrl().toLocalFile();
    bridge.stopLog();
    ASSERT_TRUE(spinUntil([&bridge]() { return !bridge.logActive(); }, 5000));
    QFile f(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString header = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(header.contains(QLatin1String("min sweep  250 ms")))
        << header.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A recording of a requesting session holds latency.csv (ADR-0042
 * section 3), which core writes (ADR-0049): its header is
 * ECUIO_TIMING_COLUMNS, then the worker's final snapshot at the
 * teardown, a row per request on the engine's ECU, the intervals on the
 * fake's own clock (core's 10 s snapshots, RecorderTest); the activity
 * log names the timing at the disconnect */
TEST(BridgeTest, ARecordingKeepsReplyTimingsSnapshots)
{
    FakeHooks hooks;
    ASSERT_TRUE(hooks.load());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString uri = QStringLiteral("j2534://%1?identify=0&diagnose=0")
                            .arg(QString::fromUtf8(FAKE_J2534_DLL_PATH));
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    spinLoop(3500);
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(spinUntil([&]() { return !bridge.recordingActive(); }, 5000));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QList<QByteArray> lines =
        fileLines(root.path() + QStringLiteral("/") + folders.first() +
                  QStringLiteral("/latency.csv"));
    ASSERT_GT(lines.size(), 2);
    EXPECT_EQ(lines.first(), QByteArray(ECUIO_TIMING_COLUMNS));
    const int columns =
        static_cast<int>(QByteArray(ECUIO_TIMING_COLUMNS).count(',')) + 1;
    QSet<QByteArray> snapshots;
    QHash<QByteArray, qint64> answered;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> cols = lines[i].split(',');
        ASSERT_EQ(cols.size(), columns) << lines[i].toStdString();
        snapshots.insert(cols[0]);
        EXPECT_EQ(cols[1], QByteArray("0x7E8"));
        EXPECT_EQ(cols.last(), QByteArray("adapter"));
        answered[cols[2]] = cols[3].toLongLong();
    }
    EXPECT_EQ(snapshots.size(), 1) << "the last, at the teardown";
    EXPECT_GT(answered.size(), 10);
    bool named = false;
    for (const QString& line : log) {
        named = named || (line.startsWith(QStringLiteral("Reply timing: ")) &&
                          line.contains(QStringLiteral("the adapter's clock")));
    }
    EXPECT_TRUE(named) << log.join(QLatin1Char('\n')).toStdString();
}

#endif // _WIN32

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
 * (ADR-0024 section 7): with the event loop held while a replay floods in,
 * thousands of frames drop, yet the recording holds every row's RPM in
 * order and the row shows the last one. Core's recording (ADR-0049)
 * holds the dropped frames too, and its last note counts them */
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
    const int dropped = bridge.droppedFrames();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const std::vector<double> rpm = recordedRpm(root.path());
    ASSERT_EQ(rpm.size(), static_cast<size_t>(kRows)) << "every row's value recorded";
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(rpm[static_cast<size_t>(i)], 1000.0 + i) << "row " << i;
    }
    // Core records the frames the bridge dropped too (ADR-0049), and the
    // recording's last note says how many the Log page skipped
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString dir = root.path() + QStringLiteral("/") + folders.first();
    EXPECT_EQ(fileLines(dir + QStringLiteral("/frames.csv")).size() - 1, kRows)
        << "every frame, dropped by the bridge or not";
    const QByteArray events = readFile(dir + QStringLiteral("/events.log"));
    EXPECT_TRUE(events.contains(
        QStringLiteral(
            "  note  the Log page skipped %1 frames; the recording has them")
            .arg(dropped)
            .toUtf8()))
        << events.toStdString();
}

/* One window holds a bounded number of samples, so the worker never
 * allocates: past the limit a value goes untimed and the Delivery line
 * counts it, while the row still shows the newest value and the
 * recording, which core writes (ADR-0049), holds every one */
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
    const SessionBridge::DeliveryStats s = bridge.deliveryStats();
    const std::vector<double> rpm = recordedRpm(root.path());
    ASSERT_EQ(rpm.size(), static_cast<size_t>(kRows)) << "every value recorded";
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(rpm[static_cast<size_t>(i)], 1000.0 + i) << "row " << i;
    }
    EXPECT_GE(s.valuesMissed, static_cast<quint64>(kRows - 2 * kLimit));
    const QStringList delivery = log.filter(QStringLiteral("Delivery: "));
    ASSERT_EQ(delivery.size(), 1) << log.join('\n').toStdString();
    EXPECT_TRUE(delivery.first().contains(
        QStringLiteral("(%1 past the staging's limit").arg(s.valuesMissed)))
        << delivery.first().toStdString();
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
    info.replay_rows_torn = 1;
    EXPECT_EQ(SessionBridge::sessionInfoText(info).toStdString(),
              "Session: replay of a recording, 7194 frames over 275.2 s at "
              "full speed, its last row cut short and left out; "
              "profile obd2");
}

/* ADR-0044: Diagnostics' lookup of a typed code: spaces and lower case
 * taken, a title, a code with none, not a code */
namespace {

// A read report whose PID 01 answered, the first `incomplete` of the
// monitors incomplete
ecuio_channel_diagnostics_t readinessReport(std::initializer_list<uint8_t> ids,
                                            uint32_t incomplete,
                                            bool mil = false)
{
    ecuio_channel_diagnostics_t d{};
    d.flags = ECUIO_DIAG_ASKED | ECUIO_DIAG_COMPLETE | ECUIO_DIAG_READINESS |
              (mil ? ECUIO_DIAG_MIL_ON : 0u);
    d.readiness = ECUIO_DIAG_ANSWERED;
    for (uint8_t id : ids) {
        d.monitors[d.monitor_count].id = id;
        d.monitors[d.monitor_count].flags =
            d.monitor_count < incomplete ? 0 : ECUIO_MONITOR_COMPLETE;
        ++d.monitor_count;
    }
    return d;
}

} // namespace

/* The inspection line as Diagnostics shows it (ADR-0045): each verdict's
 * word, as havne-cli writes it, and its sentence */
TEST(BridgeTest, InspectionEntryIsWhatThePageShows)
{
    const auto entry = [](const ecuio_channel_diagnostics_t& d, uint32_t year) {
        return SessionBridge::inspectionEntry(&d, 1, year);
    };
    QVariantMap m = entry(readinessReport({8, 10, 13}, 0), 2015);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("ready"));
    EXPECT_EQ(m.value("text").toString(),
              QStringLiteral("Ready for an emissions inspection: every monitor "
                             "complete, the check-engine light off."));
    EXPECT_TRUE(
        m.value("rule").toString().contains(QStringLiteral("40 CFR 85.2222")));
    m = entry(readinessReport({10, 8, 13}, 1), 2015);
    EXPECT_EQ(m.value("text").toString(),
              QStringLiteral("Ready for an emissions inspection: 1 monitor "
                             "incomplete (EVAP), which a model year 2015 car "
                             "may have."));
    m = entry(readinessReport({8, 10, 13}, 2), 2015);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("not_ready"));
    EXPECT_EQ(
        m.value("text").toString(),
        QStringLiteral("Not ready for an emissions inspection: 2 monitors "
                       "incomplete (catalyst, EVAP), and a model year "
                       "2015 car may have 1 at most. A station would "
                       "send it away to be driven until they complete."));
    EXPECT_EQ(entry(readinessReport({8, 10, 13}, 2), 1999)
                  .value("verdict")
                  .toString(),
              QStringLiteral("ready"));
    m = entry(readinessReport({8, 10, 13}, 2), 0);
    EXPECT_EQ(m.value("verdict").toString(), QStringLiteral("year_needed"));
    EXPECT_EQ(
        m.value("text").toString(),
        QStringLiteral("2 monitors incomplete (catalyst, EVAP): ready for "
                       "an emissions inspection if the car is model year "
                       "1996-2000, not if it's 2001 or newer, and no VIN "
                       "gave its year."));
    EXPECT_EQ(
        entry(readinessReport({8}, 0, true), 2015).value("verdict").toString(),
        QStringLiteral("mil_on"));
    EXPECT_EQ(entry(readinessReport({8}, 0), 1995).value("verdict").toString(),
              QStringLiteral("not_covered"));
    ecuio_channel_diagnostics_t reading = readinessReport({8}, 0);
    reading.flags &= ~ECUIO_DIAG_COMPLETE;
    EXPECT_TRUE(entry(reading, 2015).value("verdict").toString().isEmpty());
    EXPECT_TRUE(SessionBridge::inspectionEntry(nullptr, 0, 2015)
                    .value("text")
                    .toString()
                    .isEmpty());
}

TEST(BridgeTest, ATypedCodeIsDescribed)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.describeDtc(QStringLiteral(" p0 301 ")),
              QStringLiteral("P0301: Cylinder 1 Misfire Detected"));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("C0035"))
                    .startsWith(QStringLiteral("C0035: no title here")));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("P0301X"))
                    .startsWith(QStringLiteral("not a trouble code")));
    EXPECT_TRUE(bridge.describeDtc(QStringLiteral("   ")).isEmpty());
}

TEST(BridgeTest, TheStaleAgeIsCores)
{
    EXPECT_EQ(SessionBridge::valueStaleMs(),
              static_cast<int>(ECUIO_VALUE_STALE_US / 1000u));
}

// ---------------------------------------------------------------------------
// The vehicles' signal files (ADR-0051, app/signalstore.h): each case's
// store in a HAVNE_SETTINGS_DIR of its own.
// ---------------------------------------------------------------------------

/* A file that loads is kept as <sha256>.json, its bytes as given; the
 * same bytes from elsewhere reuse that copy */
TEST(BridgeTest, SignalFileImportKeepsACopyByItsHash)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);

    QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Extras");
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.extras");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 1);
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha + QStringLiteral(".json")});
    QFile copy(dir.store() + QLatin1Char('/') + sha + QStringLiteral(".json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file: Extras (user.extras, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(registered("user.extras")) << "import registers nothing";

    // The same bytes under another name, from a fresh bridge too
    SessionBridge again;
    r = again.importSignalFile(writeScratch(from.path(), "b.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
}

/* A format 2 file (ADR-0062) imports and is used as any: its chain counts
 * among its signals, its profile registers, and a format 1 file using a
 * call or a format 3 file is refused in the loader's words */
TEST(BridgeTest, AFormat2SignalFileImportsAndIsUsedAsAny)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const auto chain = [](const char* format, const char* expr) {
        return QStringLiteral(
                   R"({"havne_signals":%1,"name":"Chained","source":{"name":"t"},)"
                   R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
                   R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
                   R"("field":{"byte":0,"bits":8}},)"
                   R"({"key":"b","name":"B","kind":"computed","expr":"%2"},)"
                   R"({"key":"c","name":"C","kind":"computed","expr":"b * 2"}]})")
            .arg(QLatin1String(format), QLatin1String(expr))
            .toUtf8();
    };
    const QByteArray good = chain("2", "max(a, 1)");
    QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "chain.json", good));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 3);
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.chained");
    EXPECT_EQ(bridge.useSignalFiles({sha256Of(good)}).size(), 1);
    EXPECT_TRUE(registered("user.chained"));

    r = bridge.importSignalFile(
        writeScratch(from.path(), "one.json", chain("1", "max(a, 1)")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("\"havne_signals\": 2")))
        << r.value(QStringLiteral("words")).toString().toStdString();

    r = bridge.importSignalFile(
        writeScratch(from.path(), "three.json", chain("3", "a")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("format 3")))
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
}

/* A file says the built-in set it extends and how many of its signals
 * are its own, at import and in use: the page offers one extending obd2
 * in obd2's place (ADR-0052) */
TEST(BridgeTest, SignalFileSaysWhatItExtendsAndHowManyAreItsOwn)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Plus"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QByteArray alone = signalFile(QStringLiteral("Alone"));
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const int base = static_cast<int>(obd2->count);

    QVariantMap r = bridge.importSignalFile(
        writeScratch(from.path(), "plus.json", extending));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(),
              "obd2");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), base + 1);
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);
    r = bridge.importSignalFile(writeScratch(from.path(), "alone.json", alone));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(), "");
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);

    const QVariantList used =
        bridge.useSignalFiles({sha256Of(extending), sha256Of(alone)});
    ASSERT_EQ(used.size(), 2);
    for (const QVariant& v : used) {
        const QVariantMap e = v.toMap();
        ASSERT_TRUE(e.value(QStringLiteral("ok")).toBool());
        const bool plus =
            e.value(QStringLiteral("sha256")).toString() == sha256Of(extending);
        EXPECT_EQ(e.value(QStringLiteral("extends")).toString().toStdString(),
                  plus ? "obd2" : "");
        EXPECT_EQ(e.value(QStringLiteral("own")).toInt(), 1);
        EXPECT_EQ(e.value(QStringLiteral("count")).toInt(),
                  plus ? base + 1 : 1);
    }
    bridge.useSignalFiles({});
}

/* A refusal is the loader's words, never the path it came from; nothing
 * is kept */
TEST(BridgeTest, SignalFileRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    // On Windows the scratch folder is under the account's home, so its
    // name is in the path too
    const QString folder = from.path() + QStringLiteral("/jdoe-private");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QByteArray bad = signalFile(QStringLiteral("Extras"));
    bad.replace("\"bits\"", "\"bitz\"");
    QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "extras.json", bad));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("bitz"))) << words.toStdString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();

    r = bridge.importSignalFile(
        QUrl::fromLocalFile(folder + QStringLiteral("/gone.json")));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't read the file");

    for (const QString& line : log + QStringList{words}) {
        EXPECT_FALSE(line.contains(QStringLiteral("jdoe")))
            << line.toStdString();
        EXPECT_FALSE(line.contains(from.path())) << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("extras.json")))
            << line.toStdString();
    }
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty());
}

/* use() registers the files it's given and unregisters the rest */
TEST(BridgeTest, SignalFileUseRegistersItsProfile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    QVariantList used = bridge.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo?profile=user.extras", &s),
              ECUIO_OK)
        << "a URI can choose it";
    ecuio_session_destroy(s);

    EXPECT_TRUE(bridge.useSignalFiles({}).isEmpty());
    EXPECT_FALSE(registered("user.extras"));
}

/* One profile, one file: two files of one name don't both register */
TEST(BridgeTest, SignalFileOneProfileHasOneFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b =
        signalFile(QStringLiteral("Extras"), QStringLiteral("D101"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList used = bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_EQ(used.size(), 2);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(used[1].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "user.extras is made by another file already (Extras)");
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("user.extras", &set), ECUIO_OK);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->defs[0].mux_key, 0x62D100u)
        << "the first file's, never replaced";
}

/* At start-up each copy is read again: a missing, changed or no longer
 * loading copy says so and isn't offered; a good one registers */
TEST(BridgeTest, SignalFileThatNoLongerLoadsAtStartUpSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    QByteArray stale = signalFile(QStringLiteral("Stale"));
    stale.replace("\"havne_signals\":1", "\"havne_signals\":9");
    writeScratch(dir.store(), sha256Of(good) + QStringLiteral(".json"), good);
    writeScratch(dir.store(), sha256Of(stale) + QStringLiteral(".json"), stale);
    const QString changed = sha256Of("other bytes");
    writeScratch(dir.store(), changed + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    const QString missing = QString(64, QLatin1Char('e'));

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QVariantList used = bridge.useSignalFiles(
        {sha256Of(stale), missing, changed, sha256Of(good)});
    ASSERT_EQ(used.size(), 4);
    const QString staleWords =
        used[0].toMap().value(QStringLiteral("words")).toString();
    EXPECT_TRUE(staleWords.startsWith(QStringLiteral("It no longer loads: ")))
        << staleWords.toStdString();
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy is missing: import the file again");
    EXPECT_EQ(
        used[2].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy has changed: import the file again");
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(used[i].toMap().value(QStringLiteral("ok")).toBool()) << i;
    }
    EXPECT_FALSE(registered("user.stale"));
    EXPECT_TRUE(used[3].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    EXPECT_EQ(log.filter(QStringLiteral(" not offered: ")).size(), 3)
        << log.join(QLatin1Char('\n')).toStdString();
}

/* A copy no vehicle keeps goes, its profile first; the folder with the
 * last one */
TEST(BridgeTest, SignalFileIsDeletedOnlyOnceItsProfileIsUnregistered)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b = signalFile(QStringLiteral("Body"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    (void)bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_TRUE(registered("user.extras"));

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({sha256Of(b)}), 1);
    EXPECT_FALSE(registered("user.extras"));
    EXPECT_TRUE(registered("user.body"));
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha256Of(b) + QStringLiteral(".json")});

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({}), 1);
    EXPECT_FALSE(registered("user.body"));
    EXPECT_FALSE(QDir(dir.store()).exists()) << "the folder with the last copy";
}

/* The form's file goes in as text and comes back as text; a refusal is
 * the loader's, nothing kept */
TEST(BridgeTest, SignalFileTextGoesInAndComesBack)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString text = QString::fromUtf8(signalFile(QStringLiteral("Mine")));
    const QVariantMap r = bridge.importSignalText(text);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();
    EXPECT_EQ(sha, sha256Of(text.toUtf8()));
    EXPECT_EQ(bridge.signalFileText(sha), text);
    EXPECT_TRUE(bridge.signalFileText(QString(64, QLatin1Char('e'))).isEmpty());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Mine (user.mine, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    /* Two signals: the count's plural */
    QString two = QString::fromUtf8(signalFile(QStringLiteral("Pair")));
    two.replace(QStringLiteral("}}]}"),
                QStringLiteral("}},{\"key\":\"b\",\"name\":\"B\",\"read\":"
                               "{\"service\":\"22\",\"number\":\"D101\"},"
                               "\"field\":{\"byte\":0,\"bits\":8}}]}"));
    ASSERT_TRUE(
        bridge.importSignalText(two).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Pair (user.pair, 2 signals)")))
        << log.join(QLatin1Char('\n')).toStdString();

    QString bad = text;
    bad.replace(QStringLiteral("\"bits\""), QStringLiteral("\"bitz\""));
    const QVariantMap refused = bridge.importSignalText(bad);
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(refused.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("bitz")));
    EXPECT_EQ(storeFiles(dir.store()).size(), 2);
}

/* A recording of a session a vehicle's file decodes holds the store's
 * copy as signals.json, named by the copy's hash on events.log's header;
 * a session no file decodes has none (ADR-0051 section 4) */
TEST(BridgeTest, SignalFileRecordingHoldsTheStoresCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Recorded"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    ASSERT_EQ(bridge.useSignalFiles({sha}).size(), 1);
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);

    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("mock://rec?profile=user.recorded"))
            .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2);
    folders.sort();
    const QString first = root.path() + QStringLiteral("/") + folders[0];
    const QString second = root.path() + QStringLiteral("/") + folders[1];
    QFile copy(first + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    QFile events(first + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_TRUE(events.readAll().contains(
        ("\nsignal file  signals.json," + sha + "\n").toUtf8()));
    EXPECT_FALSE(QFile::exists(second + QStringLiteral("/signals.json")));
}

/* A replay decodes with the recording's own signals.json (ADR-0051
 * section 4): under the recorded profile while the store registers the
 * same bytes there, and as rec.<16 hex> once the vehicle's file has been
 * removed or changed, so the replay still decodes what was recorded */
TEST(BridgeTest, ReplayDecodesWithTheRecordingsOwnSignalFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    QTemporaryDir from;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString sha1 = sha256Of(v1);
    const QString own = QStringLiteral("rec.") + sha1.left(16);
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());

    /* The same bytes in the store: its profile, as recorded */
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?profile=user.recorded&speed=max")))
        << bridge.currentUri().toStdString();
    EXPECT_FALSE(registered(own.toUtf8().constData()));
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* The file changed under the same name (another DID, which the
     * recording's frames don't hold): the copy, under its own name */
    const QByteArray v2 =
        signalFile(QStringLiteral("Recorded"), QStringLiteral("D101"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", v2))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.useSignalFiles({sha256Of(v2)}).size(), 1);
    ASSERT_TRUE(registered("user.recorded"));
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?speed=max&profile=") + own))
        << bridge.currentUri().toStdString();
    EXPECT_TRUE(registered(own.toUtf8().constData()));
    EXPECT_TRUE(
        log.contains(QStringLiteral("Replay: decoding with the recording's own "
                                    "signal file, Recorded (") +
                     own + QLatin1Char(')')))
        << log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    /* The file removed: the copy again, and a replay of the replay holds
     * the copy as its own signals.json */
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    ASSERT_FALSE(registered("user.recorded"));
    QTemporaryDir again;
    ASSERT_TRUE(again.isValid());
    bridge.setRecordingsRootForTesting(again.path());
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecording(true);
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList replayed = recordingFolders(again.path());
    ASSERT_EQ(replayed.size(), 1);
    EXPECT_EQ(readFile(again.path() + QLatin1Char('/') + replayed.first() +
                       QStringLiteral("/signals.json")),
              v1);

    /* Its own name, as the URI the last replay left in the field */
    bridge.setRecording(false);
    const QString err = bridge.connectSession(
        QStringLiteral("replay://") + QDir::fromNativeSeparators(folder) +
        QStringLiteral("?profile=") + own + QStringLiteral("&speed=max"));
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A copy that no longer matches the hash its recording names isn't
 * loaded: the replay decodes with the profile as loaded now, and says so */
TEST(BridgeTest, ReplayOfAChangedSignalFileCopyDecodesAsLoaded)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile copy(folder + QStringLiteral("/signals.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    const QString err = replayFolder(bridge, folder, false);
    EXPECT_FALSE(err.isEmpty()) << "user.recorded is registered by nothing";
    EXPECT_FALSE(
        registered(("rec." + sha256Of(v1).left(16)).toUtf8().constData()));
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Replay: the recording's signals.json doesn't match its hash; "
        "decoding with user.recorded as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();

    QFile::remove(folder + QStringLiteral("/signals.json"));
    (void)replayFolder(bridge, folder, false);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Replay: the recording's signals.json is missing; "
                       "decoding with user.recorded as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();
}

// ---- ADR-0057's refs and ranges: the model's rows and profileSignals ----
namespace {

// The rows of the bridge's signal model, each a map of its role names
QVariantList modelRows(SessionBridge& bridge)
{
    SignalListModel* model = bridge.signalModel();
    const QHash<int, QByteArray> roles = model->roleNames();
    QVariantList rows;
    for (int i = 0; i < model->rowCount(); ++i) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromUtf8(it.value()),
                       model->data(model->index(i, 0), it.key()));
        }
        rows.append(row);
    }
    return rows;
}

// The row named name, an empty map if none
QVariantMap rowNamed(const QVariantList& rows, const QString& name)
{
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("name")).toString() == name) {
            return r.toMap();
        }
    }
    return {};
}

QStringList refsOf(const QVariantList& rows)
{
    QStringList refs;
    for (const QVariant& r : rows) {
        refs << r.toMap().value(QStringLiteral("ref")).toString();
    }
    return refs;
}

// Imports bytes into the store and uses it alone; its profile
QString useFile(SessionBridge& bridge, const QString& folder,
                const QByteArray& bytes)
{
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "f.json", bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(bridge.useSignalFiles({sha256Of(bytes)}).size(), 1);
    return r.value(QStringLiteral("profile")).toString();
}

// A signal file extending obd2: an 8-bit temperature at -40 and one at
// -50 (whose low end differs between the unit systems), an enum, a
// text, and a computed signal over the temperature
QByteArray rangesFile()
{
    return QByteArray(
        R"({"havne_signals":1,"name":"Ranges","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"temp","name":"Temp","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8},"offset":-40,"unit":"\u00b0C"},)"
        R"({"key":"gear","name":"Gear","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"kind":"enum","values":{"0":"Park"}},)"
        R"({"key":"part","name":"Part","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0},"kind":"text"},)"
        R"({"key":"cold","name":"Cold","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":8},"offset":-50,"unit":"\u00b0C"},)"
        R"({"key":"twice","name":"Twice","kind":"computed","expr":"temp * 2"}]})");
}

} // namespace

/* The mock's and obd2's rows are theirs by core's def ID; a row's ref
 * is the same in the model and in profileSignals */
TEST(BridgeTest, ABuiltInSetsRowsAreReferencedByTheirIds)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QVariant& r : rows) {
        const QVariantMap m = r.toMap();
        EXPECT_EQ(
            m.value(QStringLiteral("ref")).toString(),
            QStringLiteral("id:%1").arg(m.value(QStringLiteral("id")).toInt()))
            << m.value(QStringLiteral("name")).toString().toStdString();
    }
    EXPECT_EQ(
        rowNamed(rows, QStringLiteral("RPM")).value(QStringLiteral("ref")),
        QStringLiteral("id:1001"));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("mock://demo"))),
              refsOf(rows));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString obd2 =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_TRUE(bridge.connectSession(obd2).isEmpty());
    const QVariantList obd2Rows = modelRows(bridge);
    EXPECT_TRUE(refsOf(obd2Rows).contains(QStringLiteral("id:7003")));
    for (const QString& ref : refsOf(obd2Rows)) {
        EXPECT_TRUE(ref.startsWith(QStringLiteral("id:"))) << ref.toStdString();
    }
    EXPECT_EQ(refsOf(bridge.profileSignals(obd2)), refsOf(obd2Rows));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("elm327://COM9"))),
              refsOf(obd2Rows))
        << "an adapter's default, obd2";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* uds-generic's rows and a set registered with no signal file (the
 * mock's, copied) have no ref, though they carry IDs */
TEST(BridgeTest, ASetWithNoFileHasNoRefs)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=uds-generic&did=1E1C:u16"))
                    .isEmpty());
    QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The registry keeps the set's pointer until it's unregistered
    static std::vector<ecuio_signal_def_t> defs;
    static ecuio_signal_set_t copy{};
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    defs.assign(mock->defs, mock->defs + mock->count);
    copy = *mock;
    copy.defs = defs.data();
    ASSERT_EQ(ecuio_profile_register("copied-mock", &copy, nullptr), ECUIO_OK);
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=copied-mock"))
            .isEmpty());
    rows = modelRows(bridge);
    ASSERT_EQ(rows.size(), static_cast<int>(mock->count));
    EXPECT_EQ(rowNamed(rows, QStringLiteral("RPM"))
                  .value(QStringLiteral("id"))
                  .toInt(),
              1001);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    const QVariantList copied = bridge.profileSignals(
        QStringLiteral("mock://demo?profile=copied-mock"));
    ASSERT_EQ(copied.size(), static_cast<int>(mock->count));
    for (const QString& ref : refsOf(copied)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("copied-mock"), ECUIO_OK);
}

/* The Dashboard starts a dial from the file's gauge: profileSignals' row
 * carries its range and levels in the decoded unit, and {} for a signal
 * with none or a level the file leaves out */
TEST(BridgeTest, AProfileRowCarriesTheFilesGauge)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray gauged = signalFile(QStringLiteral("Gauged"));
    gauged.replace(
        R"("field":{"byte":0,"bits":8}})",
        R"("field":{"byte":0,"bits":8},)"
        R"("gauge":{"min":10,"max":16,"warn_low":11.5,"warn_high":15.25}})");
    const QString profile = useFile(bridge, from.path(), gauged);
    ASSERT_FALSE(profile.isEmpty());
    const QVariantList rows =
        bridge.profileSignals(QStringLiteral("mock://demo?profile=") + profile);
    const QVariantMap gauge = rowNamed(rows, QStringLiteral("A"))
                                  .value(QStringLiteral("gauge"))
                                  .toMap();
    EXPECT_EQ(gauge.value(QStringLiteral("min")).toDouble(), 10.0);
    EXPECT_EQ(gauge.value(QStringLiteral("max")).toDouble(), 16.0);
    EXPECT_EQ(gauge.value(QStringLiteral("warnLow")).toDouble(), 11.5);
    EXPECT_EQ(gauge.value(QStringLiteral("warnHigh")).toDouble(), 15.25);

    QByteArray lowOnly =
        signalFile(QStringLiteral("LowOnly"), QStringLiteral("D101"));
    lowOnly.replace(R"("field":{"byte":0,"bits":8}})",
                    R"("field":{"byte":0,"bits":8},)"
                    R"("gauge":{"min":0,"max":100,"warn_low":20}})");
    const QString lowProfile = useFile(bridge, from.path(), lowOnly);
    ASSERT_FALSE(lowProfile.isEmpty());
    const QVariantMap low =
        rowNamed(bridge.profileSignals(QStringLiteral("mock://demo?profile=") +
                                       lowProfile),
                 QStringLiteral("A"))
            .value(QStringLiteral("gauge"))
            .toMap();
    EXPECT_TRUE(low.contains(QStringLiteral("warnLow")));
    EXPECT_FALSE(low.contains(QStringLiteral("warnHigh")));

    const QVariantList plain =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_FALSE(plain.isEmpty());
    for (const QVariant& row : plain) {
        EXPECT_TRUE(
            row.toMap().value(QStringLiteral("gauge")).toMap().isEmpty());
    }
}

/* A signal file's own defs are theirs by key, its extends base's by
 * the base's ID; a file extending nothing has keys alone */
TEST(BridgeTest, ASignalFilesRowsAreReferencedByTheirKeys)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Extending"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QString profile = useFile(bridge, from.path(), extending);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QVariantList rows = modelRows(bridge);
    EXPECT_EQ(rowNamed(rows, QStringLiteral("A")).value(QStringLiteral("ref")),
              QStringLiteral("key:a"));
    const QStringList refs = refsOf(rows);
    EXPECT_TRUE(refs.contains(QStringLiteral("id:7003"))) << "obd2's RPM";
    EXPECT_EQ(refs.filter(QStringLiteral("key:")).size(), 1);
    EXPECT_FALSE(refs.contains(QString()));
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)), refs);

    // A store change under a session leaves its rows' refs; the file's
    // profile is gone from the registry, so profileSignals gives []
    bridge.useSignalFiles({});
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString alone =
        useFile(bridge, from.path(), signalFile(QStringLiteral("Alone")));
    ASSERT_FALSE(alone.isEmpty());
    EXPECT_EQ(refsOf(bridge.profileSignals(
                  QStringLiteral("mock://demo?profile=") + alone)),
              QStringList{QStringLiteral("key:a")});
    bridge.useSignalFiles({});
}

/* The mock answers Service 01 from its drive cycle and says what it
 * doesn't send: under a signal file that extends obd2, the PIDs its ECU
 * lacks (the file's own among them) and what is computed from one are
 * hidden as not sent by the ECU (ADR-0039 section 6), so a Dashboard cell
 * reads "not sent by this ECU", never "waiting for data" for good; obd2's
 * engine speed and Boost, the file's DID and what is computed from it
 * have values. The ECU's line is logged once. */
TEST(BridgeTest, TheMockSendsTheDemoCarsPidsAndHidesTheRest)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray file(
        R"({"havne_signals":1,"name":"Disclaimed","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"did","name":"Own DID","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"pid","name":"Own PID","read":{"service":"01","number":"06"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"of_did","name":"Of DID","kind":"computed","expr":"did * 2"},)"
        R"({"key":"of_pid","name":"Of PID","kind":"computed","expr":"pid * 2"}]})");
    const QString profile = useFile(bridge, from.path(), file);
    ASSERT_FALSE(profile.isEmpty());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=") + profile)
            .isEmpty());
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&] { return model->hiddenCount() > 0; }, 3000));
    ASSERT_TRUE(spinUntil(
        [&] {
            return rowNamed(modelRows(bridge), QStringLiteral("Of DID"))
                .value(QStringLiteral("hasValue"))
                .toBool();
        },
        3000));

    const QVariantList rows = modelRows(bridge);
    const auto hidden = [&rows](const char* name) {
        const QVariantMap row = rowNamed(rows, QString::fromLatin1(name));
        EXPECT_FALSE(row.isEmpty()) << name;
        return row.value(QStringLiteral("hidden")).toBool();
    };
    const auto hasValue = [&rows](const char* name) {
        return rowNamed(rows, QString::fromLatin1(name))
            .value(QStringLiteral("hasValue"))
            .toBool();
    };
    EXPECT_FALSE(hidden("Own DID"));
    EXPECT_TRUE(hasValue("Own DID"));
    EXPECT_FALSE(hidden("Of DID"));
    EXPECT_TRUE(hidden("Own PID"));
    EXPECT_FALSE(hasValue("Own PID"));
    EXPECT_TRUE(hidden("Of PID")) << "computed from a PID the ECU lacks";
    EXPECT_FALSE(hasValue("Of PID"));
    EXPECT_FALSE(hidden("RPM")) << "obd2's, which the demo car sends";
    EXPECT_FALSE(hidden("Boost"))
        << "obd2's computed signal, from MAP and baro";
    ASSERT_TRUE(spinUntil(
        [&] {
            const QVariantList now = modelRows(bridge);
            return rowNamed(now, QStringLiteral("RPM"))
                       .value(QStringLiteral("hasValue"))
                       .toBool() &&
                   rowNamed(now, QStringLiteral("Boost"))
                       .value(QStringLiteral("hasValue"))
                       .toBool();
        },
        3000));

    // obd2's rows the ECU doesn't send are hidden, the rest shown in Live:
    // each field of a PID the demo car doesn't answer (sim_ecu.h's drive
    // PIDs and its diagnostics' readiness and counts since a clear)
    static const uint8_t kDemoPids[] = {
        0x01, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x21,
        0x24, 0x30, 0x31, 0x33, 0x41, 0x42, 0x44, 0x46, 0x4D, 0x4E, 0x5C};
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    int notSent = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue; /* Boost: MAP and baro are sent */
        }
        const auto pid = static_cast<uint8_t>(d.mux_key);
        notSent += std::find(std::begin(kDemoPids), std::end(kDemoPids), pid) ==
                           std::end(kDemoPids)
                       ? 1
                       : 0;
    }
    uint32_t unsupported = 0;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(bridge.sessionForTesting(),
                                                    nullptr, 0, &unsupported),
              ECUIO_OK);
    int byId = 0;
    int byIdHidden = 0;
    for (const QVariant& r : rows) {
        const QVariantMap row = r.toMap();
        if (row.value(QStringLiteral("ref"))
                .toString()
                .startsWith(QStringLiteral("id:"))) {
            ++byId;
            byIdHidden += row.value(QStringLiteral("hidden")).toBool() ? 1 : 0;
        }
    }
    ASSERT_GT(byId, 100);
    EXPECT_EQ(byIdHidden, notSent);
    EXPECT_EQ(model->hiddenCount(), byIdHidden + 2) << "Own PID and Of PID";
    EXPECT_EQ(model->hiddenCount(), static_cast<int>(unsupported));
    EXPECT_EQ(bridge.liveSignalModel()->rowCount(),
              model->rowCount() - model->hiddenCount());

    spinLoop(300);
    const QStringList ecu = log.filter(QStringLiteral("ECU 0x7E8: supports "));
    ASSERT_EQ(ecu.size(), 1) << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(
        ecu.first().startsWith(QStringLiteral("ECU 0x7E8: supports 22 PIDs")))
        << ecu.first().toStdString();
    EXPECT_TRUE(
        ecu.first().contains(QStringLiteral("(not in its supported PIDs)")))
        << ecu.first().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The mock's own profile: nothing hidden, no ECU line
    log.clear();
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    spinLoop(400);
    EXPECT_EQ(model->hiddenCount(), 0);
    EXPECT_TRUE(log.filter(QStringLiteral("ECU 0x7E8: supports ")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* A plugin variant's rows have refs, its file being the plugin
 * store's, in profileSignals and in a session's model alike */
TEST(BridgeTest, APluginVariantsRowsHaveRefs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty());
    ASSERT_TRUE(bridge
                    .importPlugin(
                        writeScratch(from.path(), "p.havne-plugin.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString uri =
        QStringLiteral("mock://demo?profile=plugin.example-sim.base");
    const QVariantList rows = bridge.profileSignals(uri);
    ASSERT_GT(rows.size(), 0);
    const QStringList refs = refsOf(rows);
    EXPECT_FALSE(refs.contains(QString())) << refs.join(' ').toStdString();
    EXPECT_FALSE(refs.filter(QStringLiteral("key:")).isEmpty());
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A def's range follows the unit system in the model and in
 * profileSignals; an enum's is scaled as the decoder scales it, and a
 * text or computed def has none (NaN) */
TEST(BridgeTest, ARowsRangeIsShownInTheUnitSystem)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QString profile = useFile(bridge, from.path(), rangesFile());
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        const bool metric = units == 0;
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
            ASSERT_FALSE(temp.isEmpty());
            EXPECT_EQ(temp.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("°C") : QStringLiteral("°F"));
            EXPECT_NEAR(temp.value(QStringLiteral("rangeLow")).toDouble(),
                        -40.0, 1e-9);
            EXPECT_NEAR(temp.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 215.0 : 419.0, 1e-9);
            EXPECT_FALSE(temp.value(QStringLiteral("isEnum")).toBool());
            const QVariantMap cold = rowNamed(rows, QStringLiteral("Cold"));
            ASSERT_FALSE(cold.isEmpty());
            EXPECT_NEAR(cold.value(QStringLiteral("rangeLow")).toDouble(),
                        metric ? -50.0 : -58.0, 1e-9);
            EXPECT_NEAR(cold.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 205.0 : 401.0, 1e-9);

            const QVariantMap gear = rowNamed(rows, QStringLiteral("Gear"));
            EXPECT_TRUE(gear.value(QStringLiteral("isEnum")).toBool());
            EXPECT_EQ(gear.value(QStringLiteral("rangeLow")).toDouble(), 0.0);
            EXPECT_EQ(gear.value(QStringLiteral("rangeHigh")).toDouble(),
                      255.0);

            for (const char* none : {"Part", "Twice"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(none));
                ASSERT_FALSE(row.isEmpty()) << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << none;
                EXPECT_FALSE(row.value(QStringLiteral("isEnum")).toBool())
                    << none;
            }
            const QVariantMap speed =
                rowNamed(rows, QStringLiteral("Vehicle Speed"));
            ASSERT_FALSE(speed.isEmpty()) << "obd2's, the base";
            EXPECT_EQ(speed.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("km/h") : QStringLiteral("mph"));
            EXPECT_NEAR(speed.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 255.0 : 255.0 / 1.609344, 1e-3);
        }

        // profileSignals also gives the def's own unit and range, the
        // same in every unit system
        const QVariantList rows = bridge.profileSignals(uri);
        const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
        EXPECT_EQ(temp.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("°C"));
        EXPECT_NEAR(temp.value(QStringLiteral("decodedLow")).toDouble(), -40.0,
                    1e-9);
        EXPECT_NEAR(temp.value(QStringLiteral("decodedHigh")).toDouble(), 215.0,
                    1e-9);
        const QVariantMap speed =
            rowNamed(rows, QStringLiteral("Vehicle Speed"));
        EXPECT_EQ(speed.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("km/h"));
        EXPECT_NEAR(speed.value(QStringLiteral("decodedHigh")).toDouble(),
                    255.0, 1e-9);
        const QVariantMap part = rowNamed(rows, QStringLiteral("Part"));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedLow")).toDouble()));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedHigh")).toDouble()));
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* A range whose end isn't a finite number (a 32-bit field at scale
 * 1e300), whose ends are one number (a byte at offset 1e300, past the
 * double's precision), whose end converts past a double (deg C near
 * DBL_MAX, times 1.8 in deg F), or whose ends meet once converted (a bit
 * at 1.75 km/h and one ulp more, one number in mph) is no range, NaN, in
 * the model and in profileSignals */
TEST(BridgeTest, ARangeOfAnEndPastADoubleIsNoRange)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes(
        R"({"havne_signals":1,"name":"Huge","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"huge","name":"Huge","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":32},"scale":1e300},)"
        R"({"key":"flat","name":"Flat","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"offset":1e300},)"
        R"({"key":"edge","name":"Edge","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0,"bits":32},"scale":3.49e298,"unit":"\u00b0C"},)"
        R"({"key":"meet","name":"Meet","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":1,"order":"le"},"offset":1.75,)"
        R"("scale":2.220446049250313e-16,"unit":"km/h"}]})");
    const QString profile = useFile(bridge, from.path(), bytes);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            for (const char* name : {"Huge", "Flat"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(name));
                ASSERT_FALSE(row.isEmpty()) << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << name;
            }
            // About 1.499e308 in deg C; deg F's 2.698e308 is inf
            const QVariantMap edge = rowNamed(rows, QStringLiteral("Edge"));
            ASSERT_FALSE(edge.isEmpty());
            const double lo = edge.value(QStringLiteral("rangeLow")).toDouble();
            const double hi =
                edge.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(lo, 0.0);
                EXPECT_GT(hi, 1.4e308);
                EXPECT_TRUE(std::isfinite(hi));
            } else {
                EXPECT_TRUE(std::isnan(lo)) << lo;
                EXPECT_TRUE(std::isnan(hi)) << hi;
            }
            // 1.75 and 1.75 + 2^-52 km/h are both 1.0873995864153345 mph
            const QVariantMap meet = rowNamed(rows, QStringLiteral("Meet"));
            ASSERT_FALSE(meet.isEmpty());
            const double mlo =
                meet.value(QStringLiteral("rangeLow")).toDouble();
            const double mhi =
                meet.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(mlo, 1.75);
                EXPECT_EQ(mhi, std::nextafter(1.75, 2.0));
            } else {
                EXPECT_TRUE(std::isnan(mlo)) << mlo;
                EXPECT_TRUE(std::isnan(mhi)) << mhi;
            }
        }
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* profileSignals gives the rows of the profile a URI selects, and []
 * for one with no rows to bind or none registered */
TEST(BridgeTest, ProfileSignalsGivesTheProfilesRows)
{
    SessionBridge bridge;
    const QVariantList mock =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_EQ(mock.size(), static_cast<int>(ecuio_mock_signal_set()->count));
    const QVariantMap rpm = rowNamed(mock, QStringLiteral("RPM"));
    EXPECT_EQ(rpm.value(QStringLiteral("id")).toInt(), 1001);
    EXPECT_EQ(rpm.value(QStringLiteral("ref")).toString(),
              QStringLiteral("id:1001"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.value(QStringLiteral("isText")).toBool());
    EXPECT_FALSE(rpm.value(QStringLiteral("isEnum")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("group")).toInt(),
              static_cast<int>(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_NEAR(rpm.value(QStringLiteral("rangeHigh")).toDouble(), 65535 * 0.25,
                1e-9);

    for (const char* none :
         {"mock://demo?profile=uds-generic", "mock://demo?profile=no-such",
          "mock://demo?profile=raw", "replay://somewhere", "bogus://x",
          "mock://demo?profile="}) {
        EXPECT_TRUE(bridge.profileSignals(QString::fromLatin1(none)).isEmpty())
            << none;
    }
}

/* A replay's own signals.json is registered when it connects, so its
 * profile has no rows before and the file's after */
TEST(BridgeTest, ProfileSignalsOfAReplaysOwnFileFollowTheConnect)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString own = QStringLiteral("rec.") + sha256Of(v1).left(16);
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    const QString uri = QStringLiteral("replay://") +
                        QDir::fromNativeSeparators(folder) +
                        QStringLiteral("?profile=") + own;
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty()) << "before the connect";
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(own))
        << bridge.currentUri().toStdString();
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)),
              QStringList{QStringLiteral("key:a")});
    EXPECT_EQ(refsOf(modelRows(bridge)), QStringList{QStringLiteral("key:a")});
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* profileOfUri is core's choice of profile: each URI core takes gives
 * the profile its session reports, and one core refuses for its scheme
 * or its query gives "" */
TEST(BridgeTest, ProfileOfUriIsTheProfileCoreChooses)
{
    const QString fixture =
        QStringLiteral("replay://%1/2026-10-25-engine-start")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    const QStringList taken = {
        QStringLiteral("mock://demo"),
        QStringLiteral("mock://demo?profile=obd2"),
        QStringLiteral("mock://demo?profile=raw&profile=mock"),
        QStringLiteral("mock://demo?profile=uds-generic&did=1E1C:u16"),
        fixture,
        fixture + QStringLiteral("?profile=obd2&speed=max"),
    };
    for (const QString& uri : taken) {
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
        EXPECT_EQ(SessionBridge::profileOfUri(uri).toStdString(),
                  std::string(info.profile))
            << uri.toStdString();
        ecuio_session_destroy(s);
    }

    const QString tooLong =
        QStringLiteral("mock://demo?profile=") +
        QString(ECUIO_PROFILE_NAME_MAX + 1, QLatin1Char('x'));
    const QStringList refused = {
        QStringLiteral("Mock://demo"),
        QStringLiteral("bogus://x"),
        QStringLiteral("mock://demo?profile"),
        QStringLiteral("mock://demo?profile="),
        QStringLiteral("mock://demo?&profile=obd2"),
        tooLong,
    };
    for (const QString& uri : refused) {
        ecuio_session_t* s = nullptr;
        EXPECT_NE(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_destroy(s);
        EXPECT_EQ(SessionBridge::profileOfUri(uri), QString())
            << uri.toStdString();
    }

    // The longest name core takes, and the transports this test can't open
    const QString longest(ECUIO_PROFILE_NAME_MAX, QLatin1Char('x'));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("mock://demo?profile=") + longest),
              longest);
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("elm327://COM9")),
              QStringLiteral("obd2"));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("elm327://COM9?profile=mock")),
              QStringLiteral("mock"));
#ifdef _WIN32
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QStringLiteral("obd2"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QString());
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QStringLiteral("raw"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QString());
#endif
}

// ---- The app's plugins (ADR-0052, PluginStore): the example plugin
// packed, as a phone imports it ----
namespace {

QStringList pluginCopies()
{
    return QDir(PluginStore::root()).entryList(QDir::Files, QDir::Name);
}

} // namespace

/* An import keeps the package's bytes by their hash and registers every
 * variant as plugin.<id>.<variant>; a later bridge loads it from its copy
 * at its first list */
TEST(BridgeTest, PluginImportKeepsACopyAndRegistersEveryVariant)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty()) << "the example plugin is readable";
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        EXPECT_TRUE(bridge.plugins().isEmpty());
        const QVariantMap r = bridge.importPlugin(
            writeScratch(from.path(), "example-sim.havne-plugin.json", bytes));
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
        EXPECT_EQ(r.value(QStringLiteral("id")).toString().toStdString(),
                  "example-sim");
        EXPECT_EQ(r.value(QStringLiteral("version")).toString().toStdString(),
                  "1.0.0");
        EXPECT_EQ(
            r.value(QStringLiteral("variants")).toStringList(),
            (QStringList{QStringLiteral("base"), QStringLiteral("my2019")}));
        EXPECT_EQ(r.value(QStringLiteral("profiles")).toStringList(),
                  (QStringList{QStringLiteral("plugin.example-sim.base"),
                               QStringLiteral("plugin.example-sim.my2019")}));
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("replaced")).toString(), QString());
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_TRUE(registered("plugin.example-sim.my2019"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha + QStringLiteral(".havne-plugin.json")});
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
        EXPECT_EQ(copy.readAll(), bytes);
        EXPECT_TRUE(log.contains(QStringLiteral(
            "Plugin: Example: the simulated car 1.0.0 "
            "(plugin.example-sim.base, plugin.example-sim.my2019)")))
            << log.join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(bridge.plugins().size(), 1);
    }
    EXPECT_FALSE(registered("plugin.example-sim.base")) << "freed at close";

    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.my2019"));
}

/* A refusal is core's words, never the path it came from; nothing is
 * kept */
TEST(BridgeTest, PluginRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "broken.havne-plugin.json",
                     packed_plugin::example().replace("\"havne_plugin\": 1",
                                                      "\"havne_plugin\": 2")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();
    EXPECT_FALSE(words.contains(QStringLiteral("broken")))
        << words.toStdString();
    EXPECT_FALSE(words.contains(from.path())) << words.toStdString();
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* A kept copy is loaded at start only while its bytes are the ones its
 * name hashes: one changed since says so, registers nothing, and goes
 * with Remove */
TEST(BridgeTest, PluginCopyThatChangedSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
    }
    QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
               QStringLiteral(".havne-plugin.json"));
    ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    copy.close();

    SessionBridge bridge;
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_FALSE(e.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(e.value(QStringLiteral("words")).toString().toStdString(),
              "Its copy has changed: import the plugin again");
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_TRUE(bridge.removePlugin(sha).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* One version per id: an import of an id loaded replaces it, its copy
 * going with it */
TEST(BridgeTest, PluginOfAnIdLoadedReplacesIt)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QByteArray newer = packed_plugin::example("1.1.0");
    const QVariantMap r =
        bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("replaced")).toString().toStdString(),
              "1.0.0");
    EXPECT_EQ(
        pluginCopies(),
        QStringList{sha256Of(newer) + QStringLiteral(".havne-plugin.json")});
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all.at(0)
                  .toMap()
                  .value(QStringLiteral("version"))
                  .toString()
                  .toStdString(),
              "1.1.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose copy can't be kept (a folder where its file goes)
 * is refused with the loaded version still loaded, registered and kept,
 * so a later bridge loads it too */
TEST(BridgeTest, PluginReplaceWhoseCopyCantBeKeptKeepsTheLoadedOne)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        ASSERT_TRUE(QDir().mkpath(PluginStore::root() + QLatin1Char('/') +
                                  sha256Of(newer) +
                                  QStringLiteral(".havne-plugin.json")));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "Couldn't keep a copy of the plugin");
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_EQ(all.at(0)
                      .toMap()
                      .value(QStringLiteral("version"))
                      .toString()
                      .toStdString(),
                  "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose new version doesn't load (the store's seam, as a
 * registration refused would) gives way to the version it replaced:
 * loaded again from its copy, registered and kept, so a later bridge
 * loads it too; the new version's copy goes */
TEST(BridgeTest, PluginReplaceThatDoesntLoadLoadsTheReplacedOneAgain)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    const auto onlyVersion = [](const QVariantList& all) {
        return all.size() == 1 &&
                       all.at(0).toMap().value(QStringLiteral("ok")).toBool()
                   ? all.at(0)
                         .toMap()
                         .value(QStringLiteral("version"))
                         .toString()
                         .toStdString()
                   : std::string("not one loaded plugin");
    };
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        bridge.failPluginLoadForTesting(sha256Of(newer));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "a load failed for the test");
        EXPECT_EQ(onlyVersion(bridge.plugins()), "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    EXPECT_EQ(onlyVersion(again.plugins()), "1.0.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session on a variant decodes its signals; while it does, the plugin
 * is neither replaced nor removed, and once it has ended both go ahead */
TEST(BridgeTest, PluginInUseIsNeitherReplacedNorRemoved)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_TRUE(bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://plugin?profile=plugin.example-sim.base"))
                    .isEmpty());
    SignalListModel* m = bridge.signalModel();
    QStringList names;
    for (int row = 0; row < m->rowCount(); ++row) {
        names << m->data(m->index(row), SignalListModel::NameRole).toString();
    }
    EXPECT_TRUE(names.contains(QStringLiteral("Oil Temp")))
        << names.join(',').toStdString();
    // The mock answers each of the variant's DIDs with its frame count,
    // so Oil Temp reads a whole raw number less the variant's offset, 40,
    // and moves on frame by frame
    const int oil = static_cast<int>(names.indexOf(QStringLiteral("Oil Temp")));
    ASSERT_GE(oil, 0);
    const auto oilValue = [m, oil]() {
        return m->data(m->index(oil), SignalListModel::ValueRole);
    };
    // ValueRole reads 0 before any value: wait for HasValueRole
    ASSERT_TRUE(spinUntil(
        [m, oil]() {
            return m->data(m->index(oil), SignalListModel::HasValueRole)
                .toBool();
        },
        5000, 10));
    const double first = oilValue().toDouble();
    EXPECT_GE(first, -40.0);
    EXPECT_LE(first, 215.0);
    EXPECT_EQ(first, std::floor(first)) << first;
    EXPECT_TRUE(
        spinUntil([&]() { return oilValue().toDouble() != first; }, 5000, 10))
        << first;

    QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "b.json", packed_plugin::example("1.1.0")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "A session decodes with Example: the simulated car 1.0.0: "
              "disconnect, then import it again");
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pluginCopies().size(), 1) << "its copy kept";
    EXPECT_TRUE(registered("plugin.example-sim.base"));

    ASSERT_TRUE(disconnectAndWait(bridge));
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_FALSE(QDir(PluginStore::root()).exists())
        << "the empty folder goes too";
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* The VIN proposes variants (ADR-0052 section 5): the example matches
 * ZZZ from 2015 to 2030, its my2019 variant from 2019; a typed VIN or,
 * with none typed, the session's; no VIN, no match */
TEST(BridgeTest, PluginMatchesProposeVariantsByTheVin)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const auto variants = [&bridge](const QString& vin) {
        QStringList out;
        for (const QVariant& v : bridge.pluginMatches(vin)) {
            out << v.toMap().value(QStringLiteral("profile")).toString();
        }
        return out;
    };
    /* Model year 2021: M, on the later cycle (a letter at position 7) */
    EXPECT_EQ(variants(QStringLiteral("ZZZFAKE11M0000001")),
              (QStringList{QStringLiteral("plugin.example-sim.base"),
                           QStringLiteral("plugin.example-sim.my2019")}));
    EXPECT_EQ(variants(QStringLiteral("zzz fake11g0000001")),
              QStringList{QStringLiteral("plugin.example-sim.base")})
        << "2016: before my2019's years; spaces and lower case taken";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE11A0000001")).isEmpty())
        << "2010: before the plugin's years";
    EXPECT_TRUE(variants(QStringLiteral("1FAFAKE11M0000001")).isEmpty())
        << "another maker";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE")).isEmpty()) << "not a VIN";
    EXPECT_TRUE(variants(QString()).isEmpty()) << "no session, no VIN";
    const QVariantMap first =
        bridge.pluginMatches(QStringLiteral("ZZZFAKE11M0000001")).at(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("name")).toString().toStdString(),
              "Example: the simulated car");
    EXPECT_EQ(first.value(QStringLiteral("variant")).toString().toStdString(),
              "base");
}

/* A copy whose bytes changed since it was kept is written again by an
 * import of the right bytes, so the next launch loads it: the words that
 * refused it say to import it again */
TEST(BridgeTest, SignalFileImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(good);
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    QTemporaryDir from;
    {
        SessionBridge bridge;
        const QVariantList before = bridge.useSignalFiles({sha});
        ASSERT_EQ(before.size(), 1);
        EXPECT_EQ(before[0]
                      .toMap()
                      .value(QStringLiteral("words"))
                      .toString()
                      .toStdString(),
                  "Its copy has changed: import the file again");
        const QVariantMap r =
            bridge.importSignalFile(writeScratch(from.path(), "e.json", good));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool())
            << "a copy that held other bytes is written, not kept";
    }
    SessionBridge later;
    const QVariantList used = later.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool())
        << used[0]
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("user.extras"));
}

TEST(BridgeTest, PluginImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    {
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
        copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    }
    {
        SessionBridge bridge;
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_FALSE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    }
    SessionBridge later;
    const QVariantList all = later.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool())
        << all.at(0)
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session a plugin variant decodes records the variant's files merged
 * as signals.json, with the plugin's line (ADR-0052 section 4), as
 * havne-cli stream --plugin --record does; the mock answers the variant's
 * DIDs */
TEST(BridgeTest, PluginRecordingHoldsTheVariantsFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://rec?profile=plugin.example-sim.base"))
                    .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QStringLiteral("/") + folders[0];
    QFile copy(folder + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly)) << "no signals.json";
    const QByteArray text = copy.readAll();
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(ecuio_signal_file_find(file, "oil_temp"), nullptr);
    ecuio_signal_file_free(file);
    QFile events(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray log = events.readAll();
    EXPECT_TRUE(log.contains("\nplugin  example-sim,1.0.0,base\n"))
        << log.left(600).toStdString();
    EXPECT_TRUE(log.contains(
        ("\nsignal file  signals.json," + sha256Of(text) + "\n").toUtf8()));
}

/* The bridge's own refusal of a file it can't read names no path: the
 * folder can hold the account's name */
TEST(BridgeTest, AFileThatCantBeReadIsRefusedWithoutItsPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QUrl missing = QUrl::fromLocalFile(
        from.path() + QStringLiteral("/missing-extras.json"));
    SessionBridge bridge;
    for (const QVariantMap& r :
         {bridge.importSignalFile(missing), bridge.importPlugin(missing)}) {
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), "Couldn't read the file");
        EXPECT_FALSE(words.contains(QStringLiteral("missing")));
    }
}

/* A Save out of the app writes the store's copy byte for byte, so
 * havne-cli --signals and another vehicle read the file the vehicle has;
 * a copy changed since it was kept, a sha that isn't one and a target
 * that can't be written are each refused in words naming no path, and a
 * file already at the target is replaced whole */
TEST(BridgeTest, SignalFileSavedOutIsItsCopyByteForByte)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    // Its own spacing and order, which a writer of its own would change
    const QByteArray bytes =
        signalFile(QStringLiteral("Extras")).replace(",", ",\r\n  ");
    SessionBridge bridge;
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "e.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();

    const QString target = out.path() + QStringLiteral("/extras.json");
    writeScratch(out.path(), "extras.json", QByteArray(5000, 'x'));
    const QVariantMap saved =
        bridge.saveSignalFile(sha, QUrl::fromLocalFile(target));
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), bytes) << "the copy, byte for byte, and only it";
    f.close();

    const auto refused = [&](const QString& which, const QUrl& to,
                             const char* expected) {
        const QVariantMap no = bridge.saveSignalFile(which, to);
        EXPECT_FALSE(no.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = no.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(out.path()) || words.contains(dir.store()));
    };
    refused(QStringLiteral("../signals/x"), QUrl::fromLocalFile(target),
            "No such signal file");
    refused(QString(64, QLatin1Char('a')), QUrl::fromLocalFile(target),
            "Its copy is missing: import the file again");
    refused(sha, QUrl::fromLocalFile(out.path()), "Couldn't write the file");
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    refused(sha, QUrl::fromLocalFile(out.path() + QStringLiteral("/b.json")),
            "Its copy has changed since it was kept: import the file again");
    EXPECT_FALSE(QFile::exists(out.path() + QStringLiteral("/b.json")));
}

/* A layout file (ADR-0057's appendix on the layout file): the text goes out
 * and comes back as it was, a file already at the target is replaced whole,
 * and a write that can't be made leaves what was there. */
TEST(BridgeTest, ALayoutFileRoundTripsItsText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QString text =
        QStringLiteral("{\"format\":2,\"label\":\"caf\u00e9 \U0001F600\",\"pages\":[]}");
    const QUrl target = QUrl::fromLocalFile(dir.path() + QStringLiteral("/l.json"));
    writeScratch(dir.path(), "l.json", QByteArray(5000, 'x'));
    const QVariantMap saved = bridge.saveLayoutFile(text, target);
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), text.toUtf8()) << "the text as UTF-8, replacing the file";
    f.close();
    const QVariantMap read = bridge.layoutFileText(target);
    EXPECT_TRUE(read.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(read.value(QStringLiteral("text")).toString(), text);
}

TEST(BridgeTest, ALayoutFileIsReadWithinItsBoundAndAsUtf8)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const auto refused = [&](const QUrl& url, const char* expected) {
        const QVariantMap r = bridge.layoutFileText(url);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(dir.path()));
        EXPECT_TRUE(r.value(QStringLiteral("text")).toString().isEmpty());
    };
    refused(QUrl::fromLocalFile(dir.path() + QStringLiteral("/none.json")),
            "Couldn't read the file");
    const qint64 most = SessionBridge::layoutFileMaxBytes;
    const QUrl at = writeScratch(dir.path(), "at.json", QByteArray(most, ' '));
    EXPECT_TRUE(bridge.layoutFileText(at).value(QStringLiteral("ok")).toBool());
    const QUrl over =
        writeScratch(dir.path(), "over.json", QByteArray(most + 1, ' '));
    refused(over, "The file is over 82400 bytes, a layout file's most");
    refused(writeScratch(dir.path(), "latin1.json", QByteArray("{\"a\":\"\xE9\"}")),
            "The file isn't UTF-8 text");
    refused(writeScratch(dir.path(), "cut.json", QByteArray("{\"a\":\"\xE2\x82\"}")),
            "The file isn't UTF-8 text");
    const QVariantMap bom = bridge.layoutFileText(
        writeScratch(dir.path(), "bom.json", QByteArray("\xEF\xBB\xBF{}")));
    EXPECT_TRUE(bom.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bom.value(QStringLiteral("text")).toString().toStdString(), "{}")
        << "a leading BOM is skipped, once";
}

TEST(BridgeTest, ALayoutThatCantBeSavedLeavesTheFileAsItWas)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QByteArray before(3000, 'y');
    const QUrl target = writeScratch(dir.path(), "keep.json", before);
    const auto unchanged = [&] {
        QFile f(target.toLocalFile());
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_EQ(f.readAll(), before) << "an existing file is never touched";
    };
    // Past the bound: refused before anything is opened
    QVariantMap r = bridge.saveLayoutFile(
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x')), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "The layout is over 82400 bytes, a layout file's most");
    unchanged();
    // Nothing to save
    r = bridge.saveLayoutFile(QString(), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "There is no layout to save");
    unchanged();
    // A target that can't take a file: a folder stays a folder, nothing
    // is removed, and the words name no path
    r = bridge.saveLayoutFile(QStringLiteral("{}"),
                              QUrl::fromLocalFile(dir.path()));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    EXPECT_TRUE(QFileInfo(dir.path()).isDir());
    unchanged();
    r = bridge.saveLayoutFile(
        QStringLiteral("{}"),
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/no/such/folder/l.json")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFileInfo(dir.path() + QStringLiteral("/no")).exists());
    unchanged();
}

// A provider's document is opened by its URI as Android's Uri.parse wants it:
// encoded, so a name the picker makes for a name taken ("report (1).csv")
// keeps its space and parentheses apart from the path's own separators
TEST(BridgeTest, APickedUrlIsOpenedByItsEncodedName)
{
    EXPECT_EQ(
        pathOfUrl(
            QUrl(QStringLiteral(
                "content://com.android.externalstorage.documents/document/"
                "primary%3ADocuments%2Freport (1).csv")))
            .toStdString(),
        "content://com.android.externalstorage.documents/document/"
        "primary%3ADocuments%2Freport%20(1).csv");
    EXPECT_EQ(pathOfUrl(QUrl::fromLocalFile(QStringLiteral("/tmp/a b (1).csv")))
                  .toStdString(),
              "/tmp/a b (1).csv");
}

// Android's document picker hands back a URI's text, encoded: the URL made of
// it opens by that same text, so the content resolver is asked for the
// document picked, its escapes and parentheses as they came
TEST(BridgeTest, APickersUriOpensByTheTextItCameAs)
{
    for (const char* uri : {
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20(1).json",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20%281%29.json",
             "content://com.android.providers.downloads.documents/document/"
             "msf%3A1000000034",
             "content://com.android.providers.media.documents/document/"
             "document%3A42",
             "content://com.google.android.apps.docs.storage/document/"
             "acc%3D1%3Bdoc%3Dencoded%3DAbC-_d%2Be%3D%3D",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADocuments%2F%C3%B8l%20%26%20%23%20%25.json",
         }) {
        const QString text = QString::fromLatin1(uri);
        const QUrl url = pickedUrl(text);
        EXPECT_TRUE(url.isValid()) << uri;
        EXPECT_FALSE(url.isLocalFile()) << uri;
        EXPECT_EQ(pathOfUrl(url).toStdString(), uri);
    }
}

// A picker through a provider makes the document before the app writes. A
// QUrl that isn't a file URL stands in for a content:// one: QFile opens its
// text as a path. A save that refuses removes only a document that was empty
// when the save began
TEST(BridgeTest, ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // pathOfUrl encodes a URL: a folder whose path an encoding changes (a
    // space or a non-ASCII name in it; a drive letter's case is no change)
    // can't stand in for a provider's
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly))
            << url.toString().toStdString();
        f.write(bytes);
    };
    const auto tooBig =
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x'));
    const QByteArray theirs("the user's layout");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"over the bound",
         [&](const QUrl& u) { return bridge.saveLayoutFile(tooBig, u); }},
        {"a signal file that isn't kept",
         [&](const QUrl& u) {
             return bridge.saveSignalFile(QStringLiteral("0123456789abcdef"),
                                          u);
         }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made(1).json"));
        make(made, QByteArray());
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the picker's empty document goes";
        const QUrl chosen = provider(QStringLiteral("chosen.json"));
        make(chosen, theirs);
        EXPECT_FALSE(save.run(chosen).value(QStringLiteral("ok")).toBool());
        QFile after(chosen.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a replaced document stays";
        after.close();
        QFile::remove(chosen.toString());
    }
    // A local target is never removed by a refusal
    const QUrl local =
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/local.json"));
    make(local, QByteArray());
    EXPECT_FALSE(bridge.saveLayoutFile(QString(), local)
                     .value(QStringLiteral("ok"))
                     .toBool());
    EXPECT_TRUE(QFile::exists(local.toLocalFile()));
}

// A save pick's verdict, from its document's last_modified against the
// picker's launch and answer: the window allows 2 s of skew each way, and a
// time past the answer says nothing (a replaced document's upload ending
// meanwhile)
TEST(BridgeTest, ASavePicksVerdictIsItsDocumentsTimeAgainstTheLaunch)
{
    const qint64 launched = 1'700'000'000'000;
    const qint64 answered = launched + 60'000;
    EXPECT_EQ(kPickMadeWindowMs, 2000);
    const auto judged = [&](qint64 modified) {
        return judgePick(launched, answered, modified);
    };
    EXPECT_EQ(judged(launched), PickMade::Made);
    EXPECT_EQ(judged(answered), PickMade::Made);
    EXPECT_EQ(judged(launched - kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s behind";
    EXPECT_EQ(judged(launched - kPickMadeWindowMs - 1), PickMade::Replaced);
    EXPECT_EQ(judged(launched - 86'400'000), PickMade::Replaced);
    EXPECT_EQ(judged(answered + kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s ahead";
    EXPECT_EQ(judged(answered + kPickMadeWindowMs + 1), PickMade::Unknown)
        << "past the answer";
    EXPECT_EQ(judged(-1), PickMade::Unknown) << "no time";
    EXPECT_EQ(judged(0), PickMade::Unknown) << "a provider's none";
}

// A pick's verdict is taken once, a later pick of the same document replaces
// it, and one with no time clears it
TEST(BridgeTest, ASavePicksVerdictIsTakenOnce)
{
    const QString where = QStringLiteral("content://test/document/verdict");
    notePick(where, PickMade::Made);
    EXPECT_EQ(peekPick(where), PickMade::Made);
    notePick(where, PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Unknown) << "taken once";
    notePick(where, PickMade::Made);
    notePick(where, PickMade::Unknown);
    EXPECT_EQ(takePick(where), PickMade::Unknown);
}

// Each branch of the rule a failed save removes its document by, judged at
// the save's entry; a missing document stands in for one a provider won't
// open (Proton Drive's, still uploading)
TEST(BridgeTest, APickedDocumentIsRemovableByItsVerdictAndItsBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray none;
    const QByteArray some("the user's bytes");
    struct Case {
        const char* name;
        PickMade verdict;
        const QByteArray* bytes; // null: unreadable
        bool removable;
    };
    const Case cases[] = {
        {"made, empty", PickMade::Made, &none, true},
        {"made, unreadable", PickMade::Made, nullptr, true},
        {"made, holding bytes", PickMade::Made, &some, false},
        {"replaced, empty", PickMade::Replaced, &none, false},
        {"replaced, unreadable", PickMade::Replaced, nullptr, false},
        {"no verdict, empty", PickMade::Unknown, &none, true},
        {"no verdict, unreadable", PickMade::Unknown, nullptr, false},
        {"no verdict, holding bytes", PickMade::Unknown, &some, false},
    };
    int n = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const QString path = dir.filePath(QStringLiteral("doc-%1").arg(++n));
        if (c.bytes != nullptr) {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write(*c.bytes);
        }
        notePick(path, c.verdict);
        EXPECT_EQ(pickedDocumentRemovable(path), c.removable);
        EXPECT_EQ(peekPick(path), PickMade::Unknown) << "the save took it";
    }
}

// The verdict through the bridge's saves (a non-file QUrl stands in for a
// provider's, as in ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument):
// a refused layout save and the live report's refusal with no session
// remove the pick's own empty document and leave one the user chose to
// replace, empty or not
TEST(BridgeTest, ARefusedSaveFollowsItsSavePicksVerdict)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    const QByteArray theirs("the user's earlier save");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"a layout with nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"a report with no session",
         [&](const QUrl& u) { return bridge.saveHealthReport(u, 0); }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made.txt"));
        make(made, QByteArray());
        notePick(pathOfUrl(made), PickMade::Made);
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the pick's own empty document goes";

        const QUrl replaced = provider(QStringLiteral("replaced.txt"));
        make(replaced, QByteArray());
        notePick(pathOfUrl(replaced), PickMade::Replaced);
        EXPECT_FALSE(save.run(replaced).value(QStringLiteral("ok")).toBool());
        EXPECT_TRUE(QFile::exists(replaced.toString()))
            << "an empty document the user chose to replace stays";
        EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown)
            << "the save took the verdict";

        const QUrl full = provider(QStringLiteral("full.txt"));
        make(full, theirs);
        notePick(pathOfUrl(full), PickMade::Made);
        EXPECT_FALSE(save.run(full).value(QStringLiteral("ok")).toBool());
        QFile after(full.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a document with bytes stays";
        after.close();
        QFile::remove(replaced.toString());
        QFile::remove(full.toString());
    }
}

// A save a page refuses before it calls the bridge (a layout that can't be
// written as a file) drops the pick's own empty document by the same rule
TEST(BridgeTest, ASaveThePageRefusesDropsOnlyThePicksOwnDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto empty = [&](const QString& name) {
        const QUrl url(dir.path() + QLatin1Char('/') + name,
                       QUrl::TolerantMode);
        QFile f(url.toString());
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        return url;
    };
    const QUrl made = empty(QStringLiteral("made.json"));
    notePick(pathOfUrl(made), PickMade::Made);
    bridge.dropPicked(made);
    EXPECT_FALSE(QFile::exists(made.toString()));
    const QUrl replaced = empty(QStringLiteral("replaced.json"));
    notePick(pathOfUrl(replaced), PickMade::Replaced);
    bridge.dropPicked(replaced);
    EXPECT_TRUE(QFile::exists(replaced.toString()));
    EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown);
}

// ---- The health report, Diagnostics' Save report (ADR-0054's appendix
// "the health report as built") ----------------------------------------

/* The temporary folder core writes the report in: TMP, TEMP and TMPDIR
 * pointed at a folder of the test's own while it lives, so a case can
 * see the report's own folder go */
struct TempDirScope {
    EnvRestore tmp{"TMP"};
    EnvRestore temp{"TEMP"};
    EnvRestore tmpdir{"TMPDIR"};
    QTemporaryDir dir;
    TempDirScope()
    {
        const QByteArray path = QDir::toNativeSeparators(dir.path()).toUtf8();
        qputenv("TMP", path);
        qputenv("TEMP", path);
        qputenv("TMPDIR", path);
    }
    QStringList left() const
    {
        return QDir(dir.path())
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    }
};

TEST(BridgeTest, HealthFormatIsTheFiltersOrTheNames)
{
    const QUrl txt = QUrl::fromLocalFile(QStringLiteral("/x/report.txt"));
    const QUrl json = QUrl::fromLocalFile(QStringLiteral("/x/report.JSON"));
    const QUrl content(QStringLiteral("content://provider/document/1234"));
    EXPECT_EQ(healthFormatFor(txt, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(txt, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(json, 0), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(content, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, -1), ECUIO_HEALTH_TEXT);
}

/* Each count's noun as the count wants it: one, none and many */
TEST(BridgeTest, HealthReportWordsEachCount)
{
    ecuio_health_report_t one{};
    one.ecus = 1;
    one.codes = 1;
    one.values = 1;
    one.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 1;
    EXPECT_EQ(healthSummary(one).toStdString(),
              "1 ECU, 1 trouble code, 1 key value");
    EXPECT_EQ(healthSavedLine(one).toStdString(),
              "Health report saved: 1 ECU, 1 trouble code, 1 key value (1 "
              "identification item left out)");
    ecuio_health_report_t many{};
    many.ecus = 2;
    many.codes = 0;
    many.values = 14;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 13;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES] = 1;
    EXPECT_EQ(healthSummary(many).toStdString(),
              "2 ECUs, 0 trouble codes, 14 key values");
    EXPECT_EQ(healthSavedLine(many).toStdString(),
              "Health report saved: 2 ECUs, 0 trouble codes, 14 key values (13 "
              "identification items left out)");
}

/* A live session's report names the picked vehicle's baseline it's
 * given, by its start and never its folder: read, gone (the report still
 * saved, baselineRead false) and none ("") */
TEST(BridgeTest, HealthReportNamesTheBaselineItIsGiven)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    const struct {
        QString folder;
        bool read;
    } cases[] = {
        {QStringLiteral(ECUIO_RECORDING_FIXTURES "/findings"), true},
        {out.path() + QStringLiteral("/vanished-folder"), false},
        {QString(), false},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder.toStdString());
        const QString target =
            out.path() + QStringLiteral("/report-%1.json").arg(++n);
        const QVariantMap r =
            bridge.saveHealthReport(QUrl::fromLocalFile(target), 1, c.folder);
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("baselineRead")).toBool(), c.read);
        const QByteArray bytes = readFile(target);
        EXPECT_FALSE(bytes.contains("vanished"));
        EXPECT_FALSE(bytes.contains(ECUIO_RECORDING_FIXTURES));
        const QJsonValue b = QJsonDocument::fromJson(bytes).object().value(
            QStringLiteral("baseline"));
        if (c.folder.isEmpty()) {
            EXPECT_TRUE(b.isNull()) << bytes.toStdString();
            continue;
        }
        EXPECT_EQ(b.toObject().value(QStringLiteral("read")).toBool(), c.read);
        if (c.read) {
            EXPECT_EQ(b.toObject().value(QStringLiteral("started")).toString(),
                      QStringLiteral("2026-11-03T16:00:00Z"));
            EXPECT_FALSE(
                b.toObject().value(QStringLiteral("is_source")).toBool());
        }
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The mock's report written where the user chose: text, then JSON by
 * the filter or the name, over a file that exists; its label never in
 * the file or the lines; core's temporary folder gone each time */
TEST(BridgeTest, HealthReportOfTheMockIsWrittenWhereChosen)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList lines;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&](const QString& line) { lines << line; });

    TempDirScope temp;
    ASSERT_TRUE(temp.dir.isValid());
    const QString target = out.path() + QStringLiteral("/report.txt");
    const QVariantMap none =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    EXPECT_FALSE(none.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(none.value(QStringLiteral("words")).toString().toStdString(),
              "No session");
    EXPECT_FALSE(QFile::exists(target));

    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));

    {
        QFile old(target);
        ASSERT_TRUE(old.open(QIODevice::WriteOnly));
        old.write("the old report");
    }
    lines.clear();
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 1);
    const int codes = r.value(QStringLiteral("codes")).toInt();
    EXPECT_GT(codes, 0);
    EXPECT_FALSE(r.value(QStringLiteral("vinDecoded")).toBool());
    const QByteArray text = readFile(target);
    EXPECT_FALSE(text.contains("the old report"));
    EXPECT_TRUE(text.contains("mock://<label>")) << text.toStdString();
    EXPECT_FALSE(text.contains("Daily"));
    EXPECT_FALSE(text.startsWith("{"));
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0],
              QStringLiteral("Health report saved: 1 ECU, %1 trouble codes, "
                             "0 key values (0 identification items left out)")
                  .arg(codes));
    EXPECT_EQ(
        r.value(QStringLiteral("summary")).toString(),
        QStringLiteral("1 ECU, %1 trouble codes, 0 key values").arg(codes));

    // JSON by the dialog's filter, a .txt name notwithstanding, and by the
    // name alone
    const QVariantMap byFilter =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(byFilter.value(QStringLiteral("ok")).toBool());
    const QJsonDocument doc = QJsonDocument::fromJson(readFile(target));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value(QStringLiteral("havne_health")).toInt(), 1);
    EXPECT_EQ(doc.object().value(QStringLiteral("ecus")).toArray().size(), 1);
    const QString named = out.path() + QStringLiteral("/report.json");
    ASSERT_TRUE(bridge.saveHealthReport(QUrl::fromLocalFile(named), 0)
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_TRUE(QJsonDocument::fromJson(readFile(named)).isObject());
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();

    // A folder isn't a file it can write: nothing there, the words plain
    lines.clear();
    const QVariantMap folder =
        bridge.saveHealthReport(QUrl::fromLocalFile(out.path()), 0);
    EXPECT_FALSE(folder.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(folder.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].toStdString(),
              "Health report not saved: Couldn't write the file");
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    for (const QString& line : lines) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

/* Live's rows hold each value as core decoded it, in the def's unit:
 * the display's conversion never reaches the report, which converts for
 * itself; each value aged from the window that brought it */
TEST(BridgeTest, HeldValuesAreCoresInTheDefsUnit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    model.setSignalSet(&set);
    EXPECT_TRUE(model.heldValues().empty()) << "no value yet";
    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    model.updateValues({{uint16_t(1001), 100.0}});
    spinLoop(300);
    const std::vector<ecuio_health_value_t> held = model.heldValues();
    ASSERT_EQ(held.size(), 1u) << "only the row with a value";
    EXPECT_EQ(held[0].def_id, 1001u);
    EXPECT_EQ(held[0].value, 100.0) << "km/h as decoded, not mph";
    EXPECT_GE(held[0].age_us, 250000u);
    EXPECT_LT(held[0].age_us, 10000000u);
    EXPECT_EQ(held[0].flags, 0u);
    EXPECT_EQ(held[0].reserved, 0u);
}

/* A replay under obd2: Live's ranked values are the report's key values,
 * shown in the app's units (core converts them for the file) */
TEST(BridgeTest, HealthReportCarriesLivesValuesInTheAppsUnits)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=10")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_EQ(bridge.connectSession(uri), QString());
    ASSERT_TRUE(spinUntil(
        [&]() { return bridge.signalModel()->heldValues().size() >= 5; },
        10000));
    const QString target = out.path() + QStringLiteral("/report.json");
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(r.value(QStringLiteral("values")).toInt(), 0);
    const QJsonObject doc = QJsonDocument::fromJson(readFile(target)).object();
    const QJsonArray values = doc.value(QStringLiteral("key_values")).toArray();
    ASSERT_FALSE(values.isEmpty());
    QStringList units;
    for (const QJsonValue& v : values) {
        units << v.toObject().value(QStringLiteral("unit")).toString();
    }
    EXPECT_TRUE(units.contains(QStringLiteral("mph")) ||
                units.contains(QString::fromUtf8("\xC2\xB0"
                                                 "F")))
        << units.join(' ').toStdString();
    EXPECT_FALSE(units.contains(QStringLiteral("km/h")));
}

namespace {

QStringList g_themeWarnings;

void collectWarnings(QtMsgType type, const QMessageLogContext&,
                     const QString& message)
{
    if (type == QtWarningMsg) {
        g_themeWarnings << message;
    }
}

} // namespace

// Each of bg, surface and text, changed, applies the theme once, with no
// window to color
TEST(BridgeTest, WindowThemeAppliesOncePerChangeOfEachColor)
{
    FakeTheme theme;
    WindowTheme follower(&theme, nullptr);
    int applied = 0;
    QObject::connect(&follower, &WindowTheme::applied, &follower,
                     [&] { ++applied; });
    theme.setBg(Qt::red);
    EXPECT_EQ(applied, 1);
    theme.setSurface(Qt::green);
    EXPECT_EQ(applied, 2);
    theme.setText(Qt::blue);
    EXPECT_EQ(applied, 3);
}

// The scheme apply() asks for follows the look's lightness each way, and
// the first apply (at construction) asks too. The offscreen platform
// reports Unknown whatever is asked, so the QML case that reads the
// platform's scheme skips there: this is what pins the choice
TEST(BridgeTest, WindowThemeAsksTheSchemeOfTheLookEachWay)
{
    FakeTheme theme; // black: a dark look
    WindowTheme follower(&theme, nullptr);
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Dark);
    theme.setBg(QColor(QLatin1String("#f3f3f1")));
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Light);
    theme.setBg(QColor(QLatin1String("#0d0a07"))); // Night amber
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Dark);
    theme.setBg(QColor(QLatin1String("#ffffff")));
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Light);
}

// A light look's page gets dark bar icons and the Light scheme; Dark and
// the night palettes keep light ones. The line is HSL lightness 0.5
TEST(BridgeTest, WindowThemeLightBarsFollowTheBackground)
{
    for (const char* dark : {"#141416", "#0d0a07", "#080404", "#7f7f7f"}) {
        EXPECT_FALSE(WindowTheme::lightBars(QColor(QLatin1String(dark))))
            << dark;
    }
    for (const char* light : {"#f3f3f1", "#ffffff", "#808080"}) {
        EXPECT_TRUE(WindowTheme::lightBars(QColor(QLatin1String(light))))
            << light;
    }
}

// A property the theme lacks, or does not notify, is a warning: a rename
// would otherwise leave the window not following the theme
TEST(BridgeTest, WindowThemeWarnsOfAColorItCannotFollow)
{
    g_themeWarnings.clear();
    const QtMessageHandler previous = qInstallMessageHandler(collectWarnings);
    {
        FakeTheme whole;
        WindowTheme follower(&whole, nullptr);
    }
    EXPECT_TRUE(g_themeWarnings.isEmpty()) << "a whole theme says nothing";
    {
        FakeThemeWithoutSurface partial;
        WindowTheme follower(&partial, nullptr);
        int applied = 0;
        QObject::connect(&follower, &WindowTheme::applied, &follower,
                         [&] { ++applied; });
        partial.setBg(Qt::red);
        EXPECT_EQ(applied, 1) << "the colors it has still follow";
    }
    {
        FakeThemeWithConstantSurface constant;
        WindowTheme follower(&constant, nullptr);
    }
    qInstallMessageHandler(previous);
    ASSERT_EQ(g_themeWarnings.size(), 2)
        << g_themeWarnings.join(u'\n').toStdString();
    EXPECT_TRUE(g_themeWarnings[0].contains(QStringLiteral("'surface'")))
        << g_themeWarnings[0].toStdString();
    EXPECT_TRUE(g_themeWarnings[1].contains(
        QStringLiteral("no notifying property 'surface'")))
        << "a property that never notifies: "
        << g_themeWarnings[1].toStdString();
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

// ---------------------------------------------------------------------------
// Armed (ADR-0063 section 7, step 7e): the app's armed recorder, opened
// through the log's openRecorder with ECUIO_RECORD_ARMED. The mock's speed
// reads 0 until its first pull, well past a slow runner's first seconds, so a
// record condition on speed below 1 km/h is ON from the first frame (a window
// opens at once) and one above 90000 never fires.
// ---------------------------------------------------------------------------
namespace {

QByteArray armedConditions(bool fires)
{
    // Speed below 1 km/h holds until the mock's first pull, well past a
    // slow runner's first seconds (rpm would hold for the key-on's 15 s)
    const QByteArray bound = fires ? R"("below":1)" : R"("above":90000)";
    return QByteArray(
               R"({"havne_conditions":1,"name":"Armed test","conditions":[)"
               R"({"key":"rec","name":"Vehicle stopped","when":{"signal":)"
               R"(["id:3001"],"unit":"km/h",)") +
           bound + R"(},"record":{"before":0.2,"after":0.2}}]})";
}

QStringList windowFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
}

// A bridge on the mock with an armed-test set saved and its log kept
struct ArmedRig {
    SettingsDirScope dir;
    QTemporaryDir root;
    SessionBridge bridge;
    QStringList log;

    explicit ArmedRig(const QByteArray& conditions)
    {
        bridge.setRecordingsRootForTesting(root.path());
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!conditions.isEmpty()) {
            bridge.saveConditions(QString::fromUtf8(conditions));
        }
    }
    bool connectMock()
    {
        return bridge.connectSession("mock://armed").isEmpty() &&
               spinUntil([this]() { return bridge.frameCount() > 2; }, 5000);
    }
    bool logHas(const QString& prefix) const
    {
        for (const QString& l : log) {
            if (l.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST(BridgeTest, ArmedWritesNothingUntilAFiring)
{
    ArmedRig rig(armedConditions(false));
    ASSERT_TRUE(rig.dir.valid() && rig.root.isValid());
    ASSERT_TRUE(rig.connectMock());
    EXPECT_EQ(rig.bridge.armedWhy(), QString());
    ASSERT_TRUE(rig.bridge.startArmed()) << rig.log.join('\n').toStdString();
    EXPECT_TRUE(rig.bridge.armedActive());
    EXPECT_TRUE(rig.bridge.recordingActive()) << "the one recorder slot";
    EXPECT_FALSE(rig.bridge.logActive());
    spinLoop(600);
    EXPECT_EQ(rig.bridge.armedWindows(), 0);
    EXPECT_FALSE(rig.bridge.armedRecording());
    EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty());
    EXPECT_FALSE(rig.bridge.mark(QStringLiteral("x")))
        << "between windows a mark goes nowhere";
    EXPECT_EQ(rig.bridge.markCount(), 0);

    rig.bridge.stopArmed();
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_FALSE(rig.bridge.recordingActive());
    EXPECT_TRUE(rig.logHas(QStringLiteral("Armed ended: no window opened")))
        << rig.log.join('\n').toStdString();
    EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty());
}

TEST(BridgeTest, ArmedIsRefusedBesideALogOrRecordSessions)
{
    {
        ArmedRig rig(armedConditions(false));
        rig.bridge.setRecording(true);
        ASSERT_TRUE(rig.connectMock());
        ASSERT_TRUE(rig.bridge.recordingActive());
        EXPECT_EQ(rig.bridge.armedWhy(),
                  QStringLiteral("Recording this session already"));
        EXPECT_FALSE(rig.bridge.startArmed());
        EXPECT_TRUE(rig.logHas(QStringLiteral(
            "Armed not started: Recording this session already")))
            << rig.log.join('\n').toStdString();
        EXPECT_FALSE(rig.bridge.armedActive());
    }
    {
        ArmedRig rig(armedConditions(false));
        ASSERT_TRUE(rig.connectMock());
        SignalListModel* model = rig.bridge.signalModel();
        std::vector<uint16_t> picks;
        for (int r = 0; r < model->rowCount() && picks.size() < 2; ++r) {
            const QModelIndex i = model->index(r);
            if (!model->data(i, SignalListModel::IsTextRole).toBool()) {
                picks.push_back(static_cast<uint16_t>(
                    model->data(i, SignalListModel::IdRole).toUInt()));
            }
        }
        plotOnly(model, picks);
        ASSERT_TRUE(rig.bridge.startLog());
        EXPECT_EQ(rig.bridge.armedWhy(), QStringLiteral("a log is running"));
        EXPECT_FALSE(rig.bridge.startArmed());
        EXPECT_TRUE(
            rig.logHas(QStringLiteral("Armed not started: a log is running")));
        EXPECT_TRUE(rig.bridge.logActive());
        rig.bridge.stopLog();
    }
}

TEST(BridgeTest, ALogIsRefusedWhileArmed)
{
    ArmedRig rig(armedConditions(false));
    ASSERT_TRUE(rig.connectMock());
    ASSERT_TRUE(rig.bridge.startArmed());
    EXPECT_FALSE(rig.bridge.startLog());
    EXPECT_TRUE(rig.logHas(
        QStringLiteral("Log not started: Armed is recording this session")))
        << rig.log.join('\n').toStdString();
    EXPECT_FALSE(rig.bridge.logActive());
    EXPECT_TRUE(rig.bridge.armedActive()) << "the refusal leaves Armed be";
    rig.bridge.stopArmed();
}

TEST(BridgeTest, ArmedIsRefusedWithNoConditionThatRecords)
{
    // A set with no record use, then none at all
    for (const QByteArray& set : {mockConditions(), QByteArray()}) {
        ArmedRig rig(set);
        ASSERT_TRUE(rig.connectMock());
        EXPECT_EQ(rig.bridge.armedWhy(),
                  QStringLiteral("no condition records"));
        EXPECT_FALSE(rig.bridge.startArmed());
        EXPECT_TRUE(rig.logHas(
            QStringLiteral("Armed not started: no condition records")))
            << rig.log.join('\n').toStdString();
        EXPECT_EQ(rig.bridge.logNotice(),
                  QStringLiteral("Armed not started: no condition records"));
        EXPECT_FALSE(rig.bridge.armedActive());
        EXPECT_FALSE(rig.bridge.recordingActive());
        EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty());
    }
    // Offline, nothing to start
    ArmedRig offline(armedConditions(true));
    EXPECT_EQ(offline.bridge.armedWhy(), QStringLiteral("not connected"));
    EXPECT_FALSE(offline.bridge.startArmed());
}

TEST(BridgeTest, AFiringOpensAWindowAndLogsIt)
{
    ArmedRig rig(armedConditions(true));
    ASSERT_TRUE(rig.connectMock());
    ASSERT_TRUE(rig.bridge.startArmed()) << rig.log.join('\n').toStdString();
    ASSERT_TRUE(spinUntil(
        [&rig]() {
            return rig.bridge.armedWindows() == 1 &&
                   rig.bridge.armedRecording();
        },
        10000))
        << rig.log.join('\n').toStdString();
    const QStringList folders = windowFolders(rig.root.path());
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(rig.bridge.armedFolder(), folders.first());
    EXPECT_TRUE(rig.logHas(
        QStringLiteral("Armed window opened: %1").arg(folders.first())))
        << rig.log.join('\n').toStdString();
    EXPECT_FALSE(rig.log.join(u'\n').contains(rig.root.path()))
        << "a folder name, never a path";
    EXPECT_TRUE(rig.bridge.mark(QStringLiteral("in the window")))
        << "a mark inside a window goes into it";
    EXPECT_EQ(rig.bridge.markCount(), 1);

    rig.bridge.stopArmed();
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_FALSE(rig.bridge.armedRecording());
    EXPECT_TRUE(rig.logHas(
        QStringLiteral("Armed window closed: %1").arg(folders.first())));
    EXPECT_TRUE(rig.logHas(QStringLiteral("Armed ended: 1 window")));
    EXPECT_TRUE(QFileInfo::exists(
        QDir(rig.root.path()).filePath(folders.first() + "/frames.csv")));
}

TEST(BridgeTest, ADisconnectClosesTheArmedRecorder)
{
    ArmedRig rig(armedConditions(true));
    ASSERT_TRUE(rig.connectMock());
    ASSERT_TRUE(rig.bridge.startArmed());
    ASSERT_TRUE(
        spinUntil([&rig]() { return rig.bridge.armedRecording(); }, 10000));
    const QString folder = rig.bridge.armedFolder();
    ASSERT_TRUE(disconnectAndWait(rig.bridge));
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_FALSE(rig.bridge.armedRecording());
    EXPECT_FALSE(rig.bridge.recordingActive());
    EXPECT_TRUE(
        rig.logHas(QStringLiteral("Armed window closed: %1").arg(folder)))
        << rig.log.join('\n').toStdString();
    EXPECT_TRUE(rig.logHas(QStringLiteral("Armed ended: 1 window")));
    EXPECT_EQ(rig.bridge.armedWhy(), QStringLiteral("not connected"));
}

TEST(BridgeTest, AnArmedRecorderThatNeverFiredLeavesNoFolder)
{
    ArmedRig rig(armedConditions(false));
    ASSERT_TRUE(rig.connectMock());
    ASSERT_TRUE(rig.bridge.startArmed());
    spinLoop(500);
    ASSERT_TRUE(disconnectAndWait(rig.bridge));
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty())
        << "nothing on disk but the root";
    EXPECT_TRUE(rig.logHas(QStringLiteral("Armed ended: no window opened")))
        << rig.log.join('\n').toStdString();
}

// The app judged "no condition records" itself; with that judgment off,
// core's own refusal at the attach reaches the user in core's words, not
// the status string the close after a failed attach leaves
TEST(BridgeTest, ArmedShowsCoresWordsWhenCoreRefusesTheAttach)
{
    ArmedRig rig(mockConditions());
    ASSERT_TRUE(rig.connectMock());
    rig.bridge.skipArmedJudgmentForTesting(true);
    EXPECT_FALSE(rig.bridge.startArmed());
    EXPECT_TRUE(rig.bridge.logNotice().startsWith(
        QStringLiteral("Armed not started: no condition records")))
        << rig.bridge.logNotice().toStdString();
    EXPECT_TRUE(
        rig.logHas(QStringLiteral("Armed not started: no condition records")))
        << rig.log.join('\n').toStdString();
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_FALSE(rig.bridge.recordingActive());
    EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty());
}

TEST(BridgeTest, ArmedIsRefusedOnAReplay)
{
    ArmedRig rig(armedConditions(true));
    ASSERT_TRUE(
        rig.bridge
            .connectSession(QStringLiteral("replay://%1/2026-10-25-engine-start"
                                           "?profile=obd2&speed=100")
                                .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES)))
            .isEmpty());
    EXPECT_EQ(rig.bridge.armedWhy(),
              QStringLiteral("a replay plays a recording already"));
    EXPECT_FALSE(rig.bridge.startArmed());
    EXPECT_TRUE(rig.logHas(QStringLiteral(
        "Armed not started: a replay plays a recording already")))
        << rig.log.join('\n').toStdString();
    EXPECT_FALSE(rig.bridge.armedActive());
    EXPECT_TRUE(windowFolders(rig.root.path()).isEmpty());
}

// A failed write ends Armed with the recorder's own words in the log and
// the Dashboard's notice, as a log's does
TEST(BridgeTest, AFailedWriteWhileArmedEndsItWithTheReason)
{
    ArmedRig rig(armedConditions(true));
    ASSERT_TRUE(rig.connectMock());
    ASSERT_TRUE(rig.bridge.startArmed());
    ASSERT_TRUE(
        spinUntil([&rig]() { return rig.bridge.armedRecording(); }, 10000))
        << rig.log.join('\n').toStdString();
    ASSERT_NE(rig.bridge.recorderForTesting(), nullptr);
    ecuio_recorder_test_fail_writes(rig.bridge.recorderForTesting());
    ASSERT_TRUE(
        spinUntil([&rig]() { return !rig.bridge.armedActive(); }, 5000));
    EXPECT_EQ(rig.bridge.logNotice(),
              QStringLiteral("Armed ended: the disk is full"));
    EXPECT_TRUE(rig.logHas(QStringLiteral("Armed ended: the disk is full")))
        << rig.log.join('\n').toStdString();
    EXPECT_FALSE(rig.logHas(QStringLiteral("Armed ended: no window")));
    EXPECT_TRUE(rig.bridge.connected()) << "the session runs on";
    ASSERT_TRUE(disconnectAndWait(rig.bridge));
}
