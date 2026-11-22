// File/layer: tests/test_bridge_conditions_tones.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_conditions_support.h"

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

// A firing of the set before that arrives after an edit sounds by its
// key's 5 s: the row that keeps the key shares it, and a key the edit
// dropped keeps its own
TEST(BridgeTest, ALateFiringOfTheSetBeforeKeepsItsKeysFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0, 1}));
    ASSERT_EQ(run.danger(), 1u) << "one tick: the danger tone";
    ASSERT_EQ(run.warning(), 0u);
    const uint32_t before = run.generation;

    // Hot alone, behind a new condition: Warm and Quiet are dropped
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"first","name":"First","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":21000},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    run.generation = run.bridge.conditionsGenerationForTesting();
    ASSERT_EQ(run.generation, before + 1);
    // A late firing of the set before's condition c, then its line
    const auto late = [&run, before](uint32_t c, const char* line) {
        const qsizetype had =
            linesStarting(run.log, QLatin1String(line)).size();
        const ecuio_condition_event_t event =
            conditionEvent(c, before, ECUIO_CONDITION_FIRED, 20001);
        run.bridge.dispatchConditionsForTesting(&event, 1);
        return spinUntil(
            [&]() {
                return linesStarting(run.log, QLatin1String(line)).size() > had;
            },
            5000);
    };

    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, within the 5 s of its tone";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 0u)
        << "a dropped key, within the 5 s it began before the edit";
    ASSERT_TRUE(late(2, "Alert: Quiet, 20001 RPM"));
    EXPECT_EQ(run.danger(), 1u) << "an alert with no sound";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"));
    EXPECT_EQ(run.danger(), 2u) << "5 s after its tone";
    ASSERT_TRUE(run.fire({1})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 2u)
        << "Hot's row in the set attached shares the late firing's 5 s";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "the dropped key, 5 s on";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "and within the 5 s that began";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// A late firing of a key the edit keeps sounds as the edit has it: at its
// new level, not at all with its sound or the condition turned off, and
// with a sound turned on
TEST(BridgeTest, ALateFiringOfAKeptKeySoundsAsTheSetAttachedHasIt)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    // A fourth condition that sounds, for the edit to turn off
    QByteArray four = toneConditions();
    four.replace(R"(]})",
                 R"(,{"key":"off","name":"Off","when":{"signal":["id:1001"],)"
                 R"("unit":"RPM","above":17000},)"
                 R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(four))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionModel()->rowCount(), 4);
    const uint32_t before = run.bridge.conditionsGenerationForTesting();
    // Hot a warning, Warm without its sound, Quiet with one, and Off turned
    // off with its alert as it was
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"warning","sound":true}},)"
        R"({"key":"warm","name":"Warm","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":19000},)"
        R"("alert":{"level":"warning","banner":true}},)"
        R"({"key":"quiet","name":"Quiet","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":18000},)"
        R"("alert":{"level":"danger","sound":true}},)"
        R"({"key":"off","name":"Off","enabled":false,)"
        R"("when":{"signal":["id:1001"],"unit":"RPM","above":17000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), before + 1);
    const auto late = [&run, before](uint32_t c, const char* line) {
        const ecuio_condition_event_t event =
            conditionEvent(c, before, ECUIO_CONDITION_FIRED, 20001);
        run.bridge.dispatchConditionsForTesting(&event, 1);
        return spinUntil(
            [&]() { return run.log.contains(QLatin1String(line)); }, 5000);
    };

    ASSERT_TRUE(late(0, "Alert: Hot, 20001 RPM"))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.warning(), 1u) << "the level the edit gave it";
    EXPECT_EQ(run.danger(), 0u) << "not the level it had";
    ASSERT_TRUE(late(1, "Alert: Warm, 20001 RPM"));
    EXPECT_EQ(run.warning(), 1u) << "its sound was turned off";
    ASSERT_TRUE(late(2, "Alert: Quiet, 20001 RPM"));
    EXPECT_EQ(run.danger(), 1u) << "its sound was turned on";
    ASSERT_TRUE(late(3, "Alert: Off, 20001 RPM"));
    EXPECT_EQ(run.danger(), 1u) << "the edit turned the condition off";
    EXPECT_EQ(run.warning(), 1u);
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

// An attach core refuses leaves every tone's 5 s as it was: the attached
// set's and the set before's
TEST(BridgeTest, ARefusedAttachKeepsEachTonesFiveSeconds)
{
    ToneRun run;
    ASSERT_TRUE(run.start()) << run.log.join(u'\n').toStdString();
    ASSERT_TRUE(run.fire({0, 1}));
    ASSERT_EQ(run.danger(), 1u);
    const uint32_t before = run.generation;
    // Hot alone: Warm's 5 s is the set before's from here
    const QByteArray edited(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}}]})");
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(edited))
                    .value(QStringLiteral("ok"))
                    .toBool());
    run.generation = run.bridge.conditionsGenerationForTesting();
    ASSERT_EQ(run.generation, before + 1);

    run.bridge.failConditionsAttachesForTesting(1);
    ASSERT_TRUE(run.bridge.saveConditions(QString::fromUtf8(toneConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(run.bridge.conditionsGenerationForTesting(), run.generation)
        << "refused";

    ASSERT_TRUE(run.fire({0})) << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.danger(), 1u) << "Hot, within the 5 s of its tone";
    const ecuio_condition_event_t warm =
        conditionEvent(1, before, ECUIO_CONDITION_FIRED, 20001);
    run.bridge.dispatchConditionsForTesting(&warm, 1);
    // Its second line: the first is the firing before the edit
    ASSERT_TRUE(spinUntil(
        [&run]() {
            return linesStarting(run.log,
                                 QStringLiteral("Alert: Warm, 20001 RPM"))
                       .size() == 2;
        },
        5000))
        << run.log.join(u'\n').toStdString();
    EXPECT_EQ(run.warning(), 0u) << "the set before's Warm, within its 5 s";

    run.bridge.addToneTimeForTesting(5000000);
    ASSERT_TRUE(run.fire({0}));
    EXPECT_EQ(run.danger(), 2u) << "5 s on, each sounds again";
    ASSERT_TRUE(disconnectAndWait(run.bridge));
}

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

    // A firing the worker gives while the attach is being refused
    const ecuio_condition_event_t during =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 5);
    bridge.failConditionsAttachesForTesting(1, [&bridge, &during]() {
        bridge.dispatchConditionsForTesting(&during, 1);
    });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_FALSE(
        linesStarting(log, QStringLiteral("Alerts not judged: ")).isEmpty())
        << log.join(u'\n').toStdString();
    EXPECT_EQ(bridge.conditionsGenerationForTesting(), 1u);
    EXPECT_EQ(bridge.conditionModel()->rowCount(), 3) << "the set judged";
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Condition: Moving, 5 km/h"));
        },
        5000))
        << "kept through the refusal: " << log.join(u'\n').toStdString();

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

// A refused attach leaves the set before's slots and rows too: its firing
// during the refused call or after it is still its line, and the set
// judged keeps its own
TEST(BridgeTest, ARefusedAttachKeepsTheSlotsOfTheSetBefore)
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
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(otherConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u);
    // The mock's own firing of the second set first: one line, not a count
    ASSERT_TRUE(spinUntil(
        [&log]() {
            return !linesStarting(log, QStringLiteral("Alert: Second set"))
                        .isEmpty();
        },
        5000));

    // The set before's firing, given while the attach is being refused
    const ecuio_condition_event_t during =
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 6, 500000);
    bridge.failConditionsAttachesForTesting(1, [&bridge, &during]() {
        bridge.dispatchConditionsForTesting(&during, 1);
    });
    ASSERT_TRUE(bridge.saveConditions(QString::fromUtf8(toneConditions()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_EQ(bridge.conditionsGenerationForTesting(), 2u) << "refused";
    const ecuio_condition_event_t events[] = {
        conditionEvent(2, 1, ECUIO_CONDITION_FIRED, 7, 1000000),
        conditionEvent(0, 2, ECUIO_CONDITION_FIRED, 8, 2000000, 1),
    };
    bridge.dispatchConditionsForTesting(events, 2);
    EXPECT_TRUE(spinUntil(
        [&log]() {
            return log.contains(QStringLiteral("Alert: Second set, 8 km/h"));
        },
        5000))
        << log.join(u'\n').toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("Condition: Moving fired 2 times")))
        << "the set before the one judged, during the refusal and after: "
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
