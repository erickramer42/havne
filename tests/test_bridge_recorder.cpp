// File/layer: tests/test_bridge_recorder.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.Recording*, the log, marks, Armed (test_bridge)
//
// Cases for the recorder: Record sessions, the log, marks, the armed recorder.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    // A value is kept under its signal's key: the mock's RPM is 0x0C0's
    // first two bytes
    EXPECT_TRUE(signalsCsv.contains(",0C0::0.0:16,"))
        << "no RPM value recorded";

    // CRLF on Windows (ADR-0017 takes either)
    const QByteArray events =
        readFile(dir + QStringLiteral("/events.log")).replace("\r\n", "\n");
    for (const char* expected :
         {"ecuio session recording, format 4 (ADR-0049)\n",
          "\nuri  mock://rec\n", "\nsignal  0C0::0.0:16,RPM,",
          "\ncaller  Havne ", "\ncaller  surrogate frame logging  ",
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
         {QStringLiteral("\nfocus  ") + keysOf(bridge, model, picks).join(',') +
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

/* What core's values callback delivers, as signals.csv rows: key and
 * value, formatted as core's recorder writes them */
struct CoreRows {
    QHash<uint16_t, QByteArray> keys;
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
        c->rows.emplace_back(c->keys.value(v.signal_id),
                             v.text != nullptr
                                 ? QByteArray(v.text)
                                 : QByteArray::number(v.value, 'g', 12));
    }
}

} // namespace

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
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t keyCount = 0;
    ASSERT_EQ(
        ecuio_signal_set_keys(set, nullptr, keys.data(), set->count, &keyCount),
        ECUIO_OK);
    ASSERT_EQ(keyCount, set->count);
    for (uint32_t i = 0; i < set->count; ++i) {
        core.keys.insert(set->defs[i].id, QByteArray(keys[i].text));
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

/* signals.csv's RPM values, in order: obd2's RPM is kept under its key */
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
        if (cols.size() == 4 && cols[1] == "7E8:410C:2.0:16")
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
