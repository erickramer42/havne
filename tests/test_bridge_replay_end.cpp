// File/layer: tests/test_bridge_replay_end.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.AReplay*Finished* (test_bridge)
//
// What a replay's end says about the profile's signals: a row with no value
// when the last frame has played is one the recording doesn't hold.

#include <gtest/gtest.h>

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include "bridge_support.h"
#include "sessionbridge.h"

namespace {

// A recording of mock://, played back under `profile` at full speed to its
// end; the bridge is left connected, with its log in `log`
struct Played {
    QTemporaryDir recorded;
    QStringList log;
    bool finished = false;
};

bool playUnder(SessionBridge& bridge, Played& played, const QString& profile,
               bool dropValues = false)
{
    if (!played.recorded.isValid()) {
        return false;
    }
    bridge.setRecording(true);
    bridge.setRecordingsRootForTesting(played.recorded.path());
    if (!bridge.connectSession(QStringLiteral("mock://ends")).isEmpty() ||
        !spinUntil([&bridge]() { return bridge.frameCount() > 30; }, 10000) ||
        !disconnectAndWait(bridge)) {
        return false;
    }
    bridge.setRecording(false);
    const QStringList folders = recordingFolders(played.recorded.path());
    if (folders.size() != 1) {
        return false;
    }
    if (dropValues &&
        !QFile::remove(played.recorded.path() + QStringLiteral("/") +
                       folders.first() + QStringLiteral("/signals.csv"))) {
        return false;
    }
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&played](const QString& line) { played.log << line; });
    const QString uri = QStringLiteral("replay://") + played.recorded.path() +
                        QStringLiteral("/") + folders.first() +
                        QStringLiteral("?profile=") + profile +
                        QStringLiteral("&speed=max");
    if (!bridge.connectSession(uri).isEmpty()) {
        return false;
    }
    played.finished =
        spinUntil([&bridge]() { return bridge.replayFinished(); }, 10000);
    return played.finished;
}

} // namespace

/* A recording of the mock's telemetry replayed under obd2 holds none of
 * obd2's signals: once the last frame has played the bridge says the replay
 * is finished, no row has a value, and the log says why, once. */
TEST(BridgeTest, AReplayFinishedWithNoneOfTheProfilesSignalsSaysSo)
{
    SessionBridge bridge;
    Played played;
    ASSERT_TRUE(playUnder(bridge, played, QStringLiteral("obd2")))
        << played.log.join(QLatin1Char('\n')).toStdString();
    EXPECT_GT(bridge.signalModel()->rowCount(), 0) << "the profile's rows";
    EXPECT_FALSE(bridge.signalModel()->anyValue());
    const QString words = QStringLiteral(
        "Replay: none of the profile's signals is in this recording");
    EXPECT_EQ(played.log.count(words), 1)
        << played.log.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.replayFinished()) << "a new session starts it over";
}

/* Under the profile it was made under the recording's signals decode: the
 * replay still finishes, and nothing is said. */
TEST(BridgeTest, AReplayFinishedWithItsSignalsSaysNothingMore)
{
    SessionBridge bridge;
    Played played;
    ASSERT_TRUE(playUnder(bridge, played, QStringLiteral("mock")))
        << played.log.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(bridge.signalModel()->anyValue());
    EXPECT_TRUE(played.log
                    .filter(QStringLiteral("none of the profile's signals"))
                    .isEmpty())
        << played.log.join(QLatin1Char('\n')).toStdString();
}

/* The first lines name the profile the recording was made under, never its
 * folder, and say when it holds no signal values. */
TEST(BridgeTest, AReplaysFirstLineNamesWhatTheRecordingWasMadeUnder)
{
    SessionBridge bridge;
    Played played;
    ASSERT_TRUE(playUnder(bridge, played, QStringLiteral("mock")))
        << played.log.join(QLatin1Char('\n')).toStdString();
    const QStringList made = played.log.filter(QStringLiteral("recorded under"));
    ASSERT_EQ(made.size(), 1) << played.log.join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(made.first(),
              QStringLiteral("Replay: recorded under profile mock"));
    EXPECT_FALSE(made.first().contains(played.recorded.path()));
}

TEST(BridgeTest, AReplaysFirstLineSaysWhenTheRecordingHoldsNoValues)
{
    SessionBridge bridge;
    Played played;
    ASSERT_TRUE(playUnder(bridge, played, QStringLiteral("obd2"), true))
        << played.log.join(QLatin1Char('\n')).toStdString();
    const QStringList made = played.log.filter(QStringLiteral("recorded under"));
    ASSERT_EQ(made.size(), 1) << played.log.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(made.first().contains(QStringLiteral("holds no signal values")))
        << made.first().toStdString();
    EXPECT_FALSE(made.first().contains(played.recorded.path()));
}

/* A looping replay never finishes: its rows keep waiting for data. */
TEST(BridgeTest, ALoopingReplayIsNeverFinished)
{
    SessionBridge bridge;
    bridge.setRecording(true);
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    bridge.setRecordingsRootForTesting(root.path());
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://ends")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&bridge]() { return bridge.frameCount() > 30; }, 10000));
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.setRecording(false);
    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("replay://") + root.path() +
                              QStringLiteral("/") + folders.first() +
                              QStringLiteral("?profile=obd2&speed=max&loop=1"))
            .isEmpty());
    spinLoop(1500);
    EXPECT_FALSE(bridge.replayFinished());
    ASSERT_TRUE(disconnectAndWait(bridge));
}
