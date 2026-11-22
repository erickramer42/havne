// File/layer: tests/test_bridge_replay.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for replays, the recordings' list and the earlier name's carry-over.

#include "test_bridge_replay_support.h"

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
