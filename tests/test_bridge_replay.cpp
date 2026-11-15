// File/layer: tests/test_bridge_replay.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.Replay*, the recordings' rows and list, CarryOver* (test_bridge)
//
// Cases for replays, the recordings' list and the earlier name's carry-over.

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <thread>

#include "carryover.h"
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
