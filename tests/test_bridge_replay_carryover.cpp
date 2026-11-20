// File/layer: tests/test_bridge_replay_carryover.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_replay_support.h"

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

/* A replay decodes with the recording's own signals.json (ADR-0051
 * section 4) under its own name, rec.<16 hex>, whether the store still
 * holds the variant or not, so the replay decodes what was recorded */
TEST(BridgeTest, ReplayDecodesWithTheRecordingsOwnSignalFile)
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
    const QByteArray copy = readFile(folder + QStringLiteral("/signals.json"));
    ASSERT_FALSE(copy.isEmpty());
    const QString own = QStringLiteral("rec.") + sha256Of(copy).left(16);

    /* The same variant in the store: the copy, under its own name */
    ASSERT_TRUE(registered("plugin.recorded.base"));
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

    /* The plugin removed: the copy again, and a replay of the replay holds
     * the copy as its own signals.json */
    EXPECT_EQ(removeEveryPlugin(bridge), 1);
    ASSERT_FALSE(registered("plugin.recorded.base"));
    QTemporaryDir again;
    ASSERT_TRUE(again.isValid());
    bridge.setRecordingsRootForTesting(again.path());
    bridge.setRecordReplaysForTesting(true);
    bridge.setRecording(true);
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?speed=max&profile=") + own))
        << bridge.currentUri().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    const QStringList replayed = recordingFolders(again.path());
    ASSERT_EQ(replayed.size(), 1);
    EXPECT_EQ(readFile(again.path() + QLatin1Char('/') + replayed.first() +
                       QStringLiteral("/signals.json")),
              copy);

    /* Its own name, as the URI the last replay left in the field */
    bridge.setRecording(false);
    const QString err = bridge.connectSession(
        QStringLiteral("replay://") + QDir::fromNativeSeparators(folder) +
        QStringLiteral("?profile=") + own + QStringLiteral("&speed=max"));
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A recording's copy is a session's whole set, which may pass a written
 * file's megabyte (ADR-0065 section 3): the replay reads it to a set's
 * bound and decodes with it under its own name */
TEST(BridgeTest, ReplayDecodesWithACopyPastAWrittenFilesBytes)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());

    /* The copy with a megabyte of spaces before its last brace, and the
     * header naming its hash */
    const QByteArray copy = readFile(folder + QStringLiteral("/signals.json"));
    QByteArray big = copy.trimmed();
    ASSERT_TRUE(big.endsWith('}'));
    big.insert(big.size() - 1, QByteArray(ECUIO_SIGNAL_FILE_MAX_BYTES, ' '));
    QByteArray events = readFile(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.contains(sha256Of(copy).toLatin1()));
    events.replace(sha256Of(copy).toLatin1(), sha256Of(big).toLatin1());
    for (const auto& file :
         {std::make_pair(QStringLiteral("/signals.json"), big),
          std::make_pair(QStringLiteral("/events.log"), events)}) {
        QFile f(folder + file.first);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(f.write(file.second), file.second.size());
    }

    const QString own = QStringLiteral("rec.") + sha256Of(big).left(16);
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(
        QStringLiteral("?speed=max&profile=") + own))
        << bridge.currentUri().toStdString();
    EXPECT_TRUE(registered(own.toUtf8().constData()));
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
    const QByteArray written =
        readFile(folder + QStringLiteral("/signals.json"));
    ASSERT_FALSE(written.isEmpty());
    {
        QFile copy(folder + QStringLiteral("/signals.json"));
        ASSERT_TRUE(copy.open(QIODevice::Append));
        copy.write(" ");
    }
    EXPECT_EQ(removeEveryPlugin(bridge), 1);
    const QString err = replayFolder(bridge, folder, false);
    EXPECT_FALSE(err.isEmpty())
        << "plugin.recorded.base is registered by nothing";
    EXPECT_FALSE(
        registered(("rec." + sha256Of(written).left(16)).toUtf8().constData()));
    EXPECT_TRUE(log.contains(QStringLiteral(
        "Replay: the recording's signals.json doesn't match its hash; "
        "decoding with plugin.recorded.base as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();

    QFile::remove(folder + QStringLiteral("/signals.json"));
    (void)replayFolder(bridge, folder, false);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Replay: the recording's signals.json is missing; "
                       "decoding with plugin.recorded.base as loaded now")))
        << log.join(QLatin1Char('\n')).toStdString();
}
