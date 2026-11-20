// File/layer: tests/test_bridge_conditions_alloc.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_conditions_support.h"

#include "new_counter.h" // operator new counted per thread: the conditions callback

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

    // The same events once their set is the set before
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), g + 1);
    bridge.dispatchConditionsForTesting(events, 3);
    const unsigned long late = new_counter::on_this_thread();
    for (int i = 0; i < 10000; ++i) {
        bridge.dispatchConditionsForTesting(events, 3);
    }
    EXPECT_EQ(new_counter::on_this_thread() - late, 0ul);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.filter(
                          QStringLiteral("Condition: Moving fired 10001 times"))
                       .size() == 2;
        },
        5000))
        << log.join(u'\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
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
