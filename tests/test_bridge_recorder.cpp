// File/layer: tests/test_bridge_recorder.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for the recorder: Record sessions, the log, marks, the armed recorder.

#include "test_bridge_recorder_support.h"

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
