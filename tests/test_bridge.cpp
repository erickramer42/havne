// File/layer: tests/test_bridge.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include <cmath>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
