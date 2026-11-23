// File/layer: tests/test_bridge_fake_awake.cpp | GTest + QtCore/Network,
//             app-layer tests
// Testing: BRIDGE: BridgeTest's keep-awake cases (test_bridge)
//
// Keep-awake (ADR-0033's appendix "the host kept awake while a session
// reads"), through core's seam: a live connect holds only with the switch
// on, the switch off releases at once (also when it goes off during the
// connect), a refusal is one log line, and the mock holds nothing. The
// live cases run against the fake J2534 DLL, so on Windows only.

#include "test_bridge_fake_support.h"
#include "awake_seam.h"

#include <chrono>
#include <thread>

TEST(BridgeTest, AMockConnectHoldsNothing)
{
    AwakeSeam seam;
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://")).isEmpty());
    EXPECT_EQ(AwakeSeam::takes.load(), 0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(AwakeSeam::releases.load(), 0);
}

#ifdef _WIN32

TEST(BridgeTest, AConnectHoldsTheHostOnlyWhenTicked)
{
    AwakeSeam seam;
    SessionBridge bridge;
    EXPECT_TRUE(bridge.keepAwake()) << "on by default";
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::why(), "Havne: reading a vehicle");
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(AwakeSeam::releases.load(), 1) << "the close released it";

    bridge.setKeepAwake(false);
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    EXPECT_EQ(AwakeSeam::takes.load(), 1) << "unticked: nothing taken";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, UntickingReleasesAtOnce)
{
    AwakeSeam seam;
    SessionBridge bridge;
    int changed = 0;
    QObject::connect(&bridge, &SessionBridge::keepAwakeChanged,
                     [&changed]() { ++changed; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty());
    ASSERT_EQ(AwakeSeam::takes.load(), 1);
    bridge.setKeepAwake(false);
    EXPECT_EQ(AwakeSeam::releases.load(), 1) << "released before the set returned";
    EXPECT_EQ(changed, 1);
    bridge.setKeepAwake(true);
    EXPECT_EQ(AwakeSeam::takes.load(), 1) << "ticked again: from the next connect";
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
}

/* The switch off after the connect thread took the hold and before the
 * queued apply ran (no event is processed while the case waits for the
 * take): released at the apply */
TEST(BridgeTest, UntickedWhileConnectingReleasesAtTheApply)
{
    AwakeSeam seam;
    SessionBridge bridge;
    std::atomic<bool> done{false};
    QObject::connect(&bridge, &SessionBridge::connectResult,
                     [&done](const QString&, const QString&) { done.store(true); });
    bridge.connectSessionAsync(fakeUri());
    QElapsedTimer clock;
    clock.start();
    while (AwakeSeam::takes.load() == 0 && clock.elapsed() < 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(AwakeSeam::takes.load(), 1);
    ASSERT_TRUE(bridge.connecting()) << "the apply hasn't run";
    bridge.setKeepAwake(false);
    EXPECT_EQ(AwakeSeam::releases.load(), 0) << "no session to release yet";
    ASSERT_TRUE(spinUntil([&done]() { return done.load(); }, 10000, 10));
    ASSERT_TRUE(bridge.connected());
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, ARefusedHoldLogsOneLine)
{
    AwakeSeam seam;
    AwakeSeam::refuse_with("no hold here");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(fakeUri()).isEmpty()) << "the session runs";
    const QString line =
        QStringLiteral("Couldn't keep the computer awake: no hold here");
    EXPECT_EQ(log.count(line), 1) << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(log.count(line), 1);
}

#endif // _WIN32
