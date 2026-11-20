// File/layer: tests/test_bridge_recorder_armed.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_recorder_support.h"

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
