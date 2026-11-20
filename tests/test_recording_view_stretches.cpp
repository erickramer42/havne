// File/layer: tests/test_recording_view_stretches.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

// A copy of a recording fixture's events.log and signals.csv in a folder
// of dir, each line as edit leaves it, or left out when edit says false
QString copiedFixture(const QTemporaryDir& dir, const char* name,
                      const std::function<bool(QByteArray&)>& edit)
{
    const QString folder = dir.filePath(QLatin1String(name));
    QDir().mkpath(folder);
    for (const char* file : {"events.log", "signals.csv"}) {
        QFile in(fixture(name) + QLatin1Char('/') + QLatin1String(file));
        EXPECT_TRUE(in.open(QIODevice::ReadOnly));
        QByteArray out;
        for (QByteArray line : in.readAll().split('\n')) {
            if (edit(line)) {
                out += line + '\n';
            }
        }
        out.chop(1); // the split's last piece had no line end
        QFile to(folder + QLatin1Char('/') + QLatin1String(file));
        EXPECT_TRUE(to.open(QIODevice::WriteOnly));
        to.write(out);
    }
    return folder;
}

// pull-twice with no engine speed: a pull there is judged by its gain
QString pullsWithoutRpm(const QTemporaryDir& dir)
{
    return copiedFixture(dir, "pull-twice", [](QByteArray& line) {
        return !line.contains(",RPM,") && !line.startsWith("signal  RPM,");
    });
}

QVariantMap rule(const char* key, const QVariant& value)
{
    return {{QLatin1String(key), value}};
}

// A later ask replaces one waiting, and a close drops one
TEST(RecordingViewTest, OnlyTheNewestStretchesAskIsAnswered)
{
    RecordingView v;
    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    std::vector<int> answered;
    std::vector<qsizetype> sizes;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int request, const QVariantList& s, bool) {
                         answered.push_back(request);
                         sizes.push_back(s.size());
                     });
    // No events run meanwhile, so an earlier answer is posted and waiting
    ASSERT_GT(v.stretches(true, true), 0);
    QThread::msleep(300);
    const int last = v.stretches(true, false);
    ASSERT_TRUE(
        waitFor([&] { return !answered.empty() && answered.back() == last; }));
    EXPECT_EQ(answered.size(), 1u);
    EXPECT_EQ(sizes.back(), 2);

    ASSERT_GT(v.stretches(true, false), 0);
    v.close();
    EXPECT_FALSE(waitFor([&] { return answered.size() > 1; }, 300))
        << "a closed recording's answer is dropped";
}

// A pass over a long recording takes a while: a close leaves it to stop
// at its next second, the reader open under it until then, and never
// waits
TEST(RecordingViewTest, ACloseNeverWaitsForAStretchesPass)
{
    RecordingView v;
    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    int answers = 0;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int, const QVariantList&, bool) { ++answers; });
    const int hold = 2000;
    v.setStretchesHoldForTesting(hold);
    QElapsedTimer timer;
    timer.start();
    ASSERT_GT(v.stretches(true, true), 0);
    v.close();
    EXPECT_LT(timer.elapsed(), hold / 2)
        << "the ask or the close waited for the pass";
    EXPECT_EQ(v.state(), RecordingView::Closed);

    // Another recording opens and answers while the first pass still holds
    v.setStretchesHoldForTesting(0);
    v.open(fixture("cruise"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(askedStretches(v, false, true).size(), 2);
    answers = 0;
    // The abandoned pass stops once its hold ends, and its answer goes
    // nowhere
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, hold + 500));
}

// The view gone mid-pass: the pass stops on its own and posts to no one
TEST(RecordingViewTest, DestroyedWithAStretchesPassUnderWayIsSafe)
{
    QElapsedTimer timer;
    const int hold = 1500;
    {
        RecordingView v;
        v.open(fixture("pull-twice"));
        ASSERT_TRUE(waitFor([&] { return settled(v); }));
        v.setStretchesHoldForTesting(hold);
        timer.start();
        ASSERT_GT(v.stretches(true, false), 0);
    }
    EXPECT_LT(timer.elapsed(), hold / 2) << "the ask or the destructor waited";
    // Its answer arrives for a view that is gone, and is dropped
    EXPECT_FALSE(waitFor([] { return false; }, hold + 500));
}

// A close stops a stretches pass under way at its next second: a pass held
// at an ask halfway through its read and closed there ends at that ask
TEST(RecordingViewTest, ACloseStopsAStretchesPassUnderWay)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 120, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 60000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    // An unoptimized build on CI's macOS runner reads it in about 10 s
    ASSERT_GE(askedStretches(v, true, false, {}, 120000).size(), 20);
    const int whole = v.stretchesAsksForTesting();
    ASSERT_GT(whole, 100);

    const int half = whole / 2;
    v.setStretchesHoldAtAskForTesting(half);
    ASSERT_GT(v.stretches(true, false), 0);
    ASSERT_TRUE(
        waitFor([&] { return v.stretchesHeldForTesting() > 0; }, 120000));
    v.close();
    ASSERT_TRUE(waitFor([&] { return v.readsStoppedForTesting() > 0; }))
        << "the pass read on to its end";
    EXPECT_EQ(v.stretchesAsksForTesting(), half)
        << "a whole pass asks " << whole << " times";
}

// More stretches than a pass's first call holds: the pass asks again for
// them all, and a stop landing in that second call ends it there, CANCELED
// with no answer, rather than reading on to the recording's end
TEST(RecordingViewTest, AStopInAPassesSecondCallEndsIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir, 60, 100);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 60000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    ASSERT_EQ(askedStretches(v, true, false, {}, 120000).size(), 100)
        << "past the first call's 64";
    const int both = v.stretchesAsksForTesting();
    ASSERT_GT(both, 4);
    ASSERT_EQ(both % 2, 0) << "two calls over the same range ask alike";

    int answers = 0;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int, const QVariantList&, bool) { ++answers; });
    v.setStretchesStopAtAskForTesting(both / 2 + 2);
    ASSERT_GT(v.stretches(true, false), 0);
    ASSERT_TRUE(waitFor([&] { return v.readsStoppedForTesting() > 0; }))
        << "the second call read on to its end";
    EXPECT_EQ(v.stretchesAsksForTesting(), both / 2 + 2)
        << "ended at the stop, its second ask";
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, 300));
}

// A close during the asks' hold: the statistics and step reads taken
// before it are never made, the means pass under way ends, and none is
// answered
TEST(RecordingViewTest, AClosedOpensStatsAndStepAreNotRead)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    int answers = 0;
    QObject::connect(&v, &RecordingView::statsReady,
                     [&](int, const QVariantList&) { ++answers; });
    QObject::connect(&v, &RecordingView::stepReady,
                     [&](int, double) { ++answers; });
    QObject::connect(&v, &RecordingView::columnMeansReady,
                     [&](int, const QVariantList&) { ++answers; });
    const int hold = 300;
    v.setAsksHoldForTesting(hold);
    QElapsedTimer timer;
    timer.start();
    // The task takes the means pass alone and holds, so the two asks queue
    // behind it and its next turn takes them together
    ASSERT_GT(v.columnMeans({rpm}, 0, 30000, 64), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    ASSERT_GT(v.stats({rpm}, 0, 30000), 0);
    ASSERT_GT(v.step(rpm, 100, true), 0);
    ASSERT_EQ(v.meansYieldsForTesting(), 0)
        << "the pass gave way before both asks were queued";
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() >= 2; }));
    v.close();
    EXPECT_EQ(v.readsStoppedForTesting(), 0) << "nothing stopped yet";
    // The step and statistics calls not made, and the means pass ended
    ASSERT_TRUE(
        waitFor([&] { return v.readsStoppedForTesting() == 3; }, hold + 2000));
    EXPECT_LT(timer.elapsed(), 2 * hold + 1000);
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, 300));
    EXPECT_EQ(v.readsStoppedForTesting(), 3);
}

// A file changed since the open: the ask says it failed, with no stretch,
// so a page never reads it as a recording with no pull
TEST(RecordingViewTest, AStretchesAskOnAChangedFileSaysItFailed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QDir().mkpath(dir.filePath(QStringLiteral("copy")));
    for (const char* name : {"events.log", "signals.csv"}) {
        ASSERT_TRUE(QFile::copy(
            fixture("pull-twice") + QLatin1Char('/') + QLatin1String(name),
            dir.filePath(QStringLiteral("copy/") + QLatin1String(name))));
    }
    RecordingView v;
    v.open(dir.filePath(QStringLiteral("copy")));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open);
    QFile rows(dir.filePath(QStringLiteral("copy/signals.csv")));
    ASSERT_TRUE(rows.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
    ASSERT_TRUE(rows.remove());
    bool answered = false;
    bool ok = true;
    qsizetype count = -1;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int, const QVariantList& s, bool good) {
                         answered = true;
                         ok = good;
                         count = s.size();
                     });
    ASSERT_GT(v.stretches(true, false), 0);
    ASSERT_TRUE(waitFor([&] { return answered; }));
    EXPECT_FALSE(ok);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(v.stale());
}

// A recording holding neither a throttle nor an engine-load signal has no
// pull to find whatever the levels: the page says so in the Jump row
TEST(RecordingViewTest, PullsLackSignalsWhenNeitherThrottleNorLoadIsRecorded)
{
    RecordingView v;
    EXPECT_FALSE(v.pullsLackSignals()) << "not open";
    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.pullsLackSignals()) << "a throttle";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    v.open(copiedFixture(dir, "pull-twice", [](QByteArray& line) {
        line.replace("Throttle", "Engine Load");
        return true;
    }));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.pullsLackSignals()) << "an engine load alone is enough";

    QTemporaryDir bare;
    ASSERT_TRUE(bare.isValid());
    v.open(copiedFixture(bare, "pull-twice", [](QByteArray& line) {
        return !line.contains(",Throttle,") &&
               !line.startsWith("signal  Throttle,");
    }));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.pullsLackSignals());
    EXPECT_EQ(found(v, true, false, {}), 0) << "and so it finds no pull";
    EXPECT_EQ(found(v, false, true, {}), 0);

    v.close();
    EXPECT_FALSE(v.pullsLackSignals()) << "closed";
}

// Each level a page sets reaches core's rule of that name: pull-twice
// (two pulls, a stab that climbs 180 rpm, a part-throttle rise at 45 %,
// pulls 7 s and 4 s long at 96 % and 97 %, a 925 ms lift), copies of it
// with engine load for the throttle and with no engine speed, the cruise
// fixture (two cruises 3 km/h apart, at 80 and 83 km/h, 12 s and 13 s
// long), and a cruise whose throttle steps 4 %
TEST(RecordingViewTest, AStretchesAskTakesRules)
{
    RecordingView v;
    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(found(v, true, false, {}), 2);
    EXPECT_EQ(found(v, true, false, rule("pullRise", 5000)), 0);
    EXPECT_EQ(found(v, true, false, rule("pullRise", 100)), 3) << "the stab";
    EXPECT_EQ(found(v, true, false, rule("pullLevel", 97)), 1);
    EXPECT_EQ(found(v, true, false, rule("pullLevel", 40)), 3)
        << "the part throttle";
    EXPECT_EQ(found(v, true, false, rule("pullHoldS", 5)), 1);
    EXPECT_EQ(found(v, true, false, rule("pullGapS", 0.9)), 3)
        << "split at the lift";
    // A level of another signal's, and the gain with engine speed in the
    // log, change nothing; 0 is core's default
    EXPECT_EQ(found(v, true, false, rule("pullLoadLevel", 97)), 2);
    EXPECT_EQ(found(v, true, false, rule("pullGain", 100)), 2);
    EXPECT_EQ(found(v, true, false,
                    {{QStringLiteral("pullRise"), 0},
                     {QStringLiteral("pullGapS"), 0.0}}),
              2);
    // Several at once: the stab let in, then every pull under 5 s out
    EXPECT_EQ(found(v, true, false,
                    {{QStringLiteral("pullRise"), 100},
                     {QStringLiteral("pullHoldS"), 5}}),
              1);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    v.open(copiedFixture(dir, "pull-twice", [](QByteArray& line) {
        line.replace("Throttle", "Engine Load");
        return true;
    }));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(found(v, true, false, {}), 2);
    EXPECT_EQ(found(v, true, false, rule("pullLoadLevel", 97)), 1);
    EXPECT_EQ(found(v, true, false, rule("pullLevel", 97)), 2)
        << "the throttle's level, with no throttle";

    QTemporaryDir bare;
    ASSERT_TRUE(bare.isValid());
    v.open(pullsWithoutRpm(bare));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(found(v, true, false, {}), 2);
    EXPECT_EQ(found(v, true, false, rule("pullGain", 5)), 3) << "the stab";
    EXPECT_EQ(found(v, true, false, rule("pullGain", 16)), 1)
        << "the pull in top gear gains 15";
    EXPECT_EQ(found(v, true, false, rule("pullRise", 5000)), 2)
        << "the rise is engine speed's";

    v.open(fixture("cruise"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(found(v, false, true, {}), 2);
    EXPECT_EQ(found(v, false, true, rule("cruiseFloor", 82)), 1);
    EXPECT_EQ(found(v, false, true, rule("cruiseHoldS", 15)), 0);
    EXPECT_EQ(found(v, false, true, rule("cruiseSpeedBand", 4)), 1)
        << "the two as one";

    // 30 s at 100 km/h, the throttle stepping between 20 % and 24 %
    QTemporaryDir stepped;
    ASSERT_TRUE(stepped.isValid());
    {
        QFile events(stepped.filePath(QStringLiteral("events.log")));
        ASSERT_TRUE(events.open(QIODevice::WriteOnly));
        events.write("ecuio session recording, format 2 (ADR-0049)\n"
                     "started  2026-11-03 09:00:00.000 (local time)\n"
                     "profile  test\n"
                     "signal  Vehicle Speed,number,km/h,0,1\n"
                     "signal  Throttle,number,%,1,2\n"
                     "\n00:00:30.000  +30.000  end  test: 0 frames, 122 "
                     "values, 0 records dropped, 0 notes dropped\n");
        QFile rows(stepped.filePath(QStringLiteral("signals.csv")));
        ASSERT_TRUE(rows.open(QIODevice::WriteOnly));
        rows.write("host_us,signal,value,unit\n");
        for (int k = 0; k <= 60; ++k) {
            const QByteArray t = QByteArray::number(k * 500000);
            rows.write(t + ",Vehicle Speed,100,km/h\n");
            rows.write(QByteArray::number(k * 500000 + 1000) + ",Throttle," +
                       (k % 2 == 0 ? "20" : "24") + ",%\n");
        }
    }
    v.open(stepped.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(found(v, false, true, {}), 0) << "a 4 % step, a 3 % band";
    EXPECT_EQ(found(v, false, true, rule("cruiseThrottleBand", 5)), 1);
}

// A speed is in the viewer's unit: mph under imperial units, km/h to core
TEST(RecordingViewTest, ASpeedRuleIsConvertedFromTheViewersUnit)
{
    RecordingView v;
    v.open(fixture("cruise"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    // 51 km/h is under both cruises; 51 mph (82.1 km/h) only under one
    EXPECT_EQ(found(v, false, true, rule("cruiseFloor", 51)), 2);
    EXPECT_EQ(found(v, false, true, rule("cruiseSpeedBand", 2.5)), 2);
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(found(v, false, true, rule("cruiseFloor", 51)), 1);
    // 2.5 mph is 4.02 km/h: the two cruises, 3 km/h apart, as one
    EXPECT_EQ(found(v, false, true, rule("cruiseSpeedBand", 2.5)), 1);

    // The stab gains 8 km/h: 5 mph (8.05 km/h) leaves it out, 4.9 doesn't
    QTemporaryDir bare;
    ASSERT_TRUE(bare.isValid());
    v.open(pullsWithoutRpm(bare));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(found(v, true, false, rule("pullGain", 5)), 2);
    EXPECT_EQ(found(v, true, false, rule("pullGain", 4.9)), 3);
    v.setUnitSystem(ECUIO_UNITS_METRIC);
    EXPECT_EQ(found(v, true, false, rule("pullGain", 5)), 3);
}

// A rule core can't take is refused where it is asked, with words that
// name it, and no pass starts
TEST(RecordingViewTest, AStretchesRuleOutOfRangeIsRefusedWithWords)
{
    RecordingView v;
    int changes = 0;
    QObject::connect(&v, &RecordingView::stretchesProblemChanged,
                     [&] { ++changes; });
    // Not open: bad rules are still named, good ones leave no words
    EXPECT_EQ(v.stretches(true, false, rule("pullLevel", 101)), 0);
    EXPECT_EQ(v.stretchesProblem(),
              QStringLiteral("Pull throttle must be a number from 0 to 100"));
    EXPECT_EQ(v.stretches(true, false, rule("pullLevel", 90)), 0);
    EXPECT_EQ(v.stretchesProblem(), QString());
    EXPECT_EQ(changes, 2);

    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    int answers = 0;
    QObject::connect(&v, &RecordingView::stretchesReady,
                     [&](int, const QVariantList&, bool) { ++answers; });
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const struct {
        const char* key;
        QVariant value;
        const char* words;
    } refused[] = {
        {"pullLevel", 100.5, "Pull throttle must be a number from 0 to 100"},
        {"pullLevel", -1, "Pull throttle must be a number from 0 to 100"},
        {"pullLoadLevel", nan,
         "Pull load, with no throttle must be a number from 0 to 100"},
        {"cruiseThrottleBand", 101,
         "Throttle within must be a number from 0 to 100"},
        {"pullHoldS", 3600.5, "Pull at least must be a number from 0 to 3600 s"},
        {"pullGapS", inf, "A dip shorter than must be a number from 0 to 3600 s"},
        {"cruiseHoldS", -0.1, "Cruise at least must be a number from 0 to 3600 s"},
        {"pullRise", -1, "Engine speed rises must be a number, 0 or more"},
        {"pullRise", inf, "Engine speed rises must be a number, 0 or more"},
        {"pullGain", 1000.5, "Speed gains, no rpm must be a number from 0 to 1000 km/h"},
        {"cruiseSpeedBand", QStringLiteral("wide"),
         "Speed within must be a number from 0 to 1000 km/h"},
        {"cruiseFloor", true,
         "Cruise above must be a number from 0 to 1000 km/h"},
        {"pullEnd", QStringLiteral("top"),
         "Where a pull ends must be lift or top-speed"},
        {"pullEnd", 1, "Where a pull ends must be lift or top-speed"},
        {"pullSpeed", 1, "'pullSpeed' isn't a stretch rule"},
    };
    changes = 0;
    for (const auto& r : refused) {
        EXPECT_EQ(v.stretches(true, true, rule(r.key, r.value)), 0) << r.key;
        EXPECT_EQ(v.stretchesProblem(), QLatin1String(r.words)) << r.key;
    }
    EXPECT_EQ(changes, 12) << "a change for each new wording, none for the "
                              "three refusals that repeat the one before";
    // One bad rule among good ones refuses the ask
    EXPECT_EQ(v.stretches(true, true,
                          {{QStringLiteral("pullLevel"), 90},
                           {QStringLiteral("pullRise"), -5}}),
              0);
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, 300))
        << "a refused ask starts no pass";

    // The most of each is taken, and an ask taken clears the words
    changes = 0;
    EXPECT_EQ(found(v, true, false, rule("pullLevel", 100)), 0);
    EXPECT_EQ(v.stretchesProblem(), QString());
    EXPECT_EQ(changes, 1);
    EXPECT_GT(v.stretches(true, false, rule("pullHoldS", 3600)), 0);
    EXPECT_GT(v.stretches(true, false, rule("pullGain", 1000)), 0);
    EXPECT_GT(v.stretches(true, false, rule("pullRise", 1e9)), 0);
    // In mph the most is 1000 km/h still: 621 mph, and its words say so
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_GT(v.stretches(true, false, rule("cruiseFloor", 621)), 0);
    EXPECT_EQ(v.stretches(true, false, rule("cruiseFloor", 622)), 0);
    EXPECT_EQ(
        v.stretchesProblem(),
        QStringLiteral("Cruise above must be a number from 0 to 621 mph"));
    v.setUnitSystem(ECUIO_UNITS_METRIC);

    // A refused ask leaves the one before it to answer
    ASSERT_TRUE(waitFor([&] { return answers >= 1; }));
    QThread::msleep(100);
    QCoreApplication::processEvents();
    answers = 0;
    const int asked = v.stretches(true, false);
    ASSERT_GT(asked, 0);
    EXPECT_EQ(v.stretches(true, false, rule("pullLevel", -3)), 0);
    int last = 0;
    const auto c = QObject::connect(
        &v, &RecordingView::stretchesReady,
        [&](int request, const QVariantList&, bool) { last = request; });
    EXPECT_TRUE(waitFor([&] { return last == asked; }));
    QObject::disconnect(c);
}

// The levels a page shows as defaults are core's macros, a speed in the
// viewer's unit, and asking with them finds what asking with none does
TEST(RecordingViewTest, TheDefaultsAreCoresOwn)
{
    RecordingView v;
    const auto value = [&](const char* key) {
        const QVariantMap d = v.stretchDefaults();
        EXPECT_TRUE(d.contains(QLatin1String(key))) << key;
        return d.value(QLatin1String(key)).toDouble();
    };
    EXPECT_EQ(v.stretchDefaults().size(), 11);
    EXPECT_EQ(v.stretchDefaults().value(QStringLiteral("pullEnd")),
              QVariant(QStringLiteral("lift")))
        << "a pull ends at the lift unless asked";
    // Every number rule has the name a refusal and the panel's field use;
    // pullEnd, a word with its own switch, has none
    const QVariantMap names = v.stretchRuleNames();
    const QVariantMap defaults = v.stretchDefaults();
    EXPECT_EQ(names.size(), defaults.size() - 1);
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (it.key() == QLatin1String("pullEnd"))
            continue;
        EXPECT_TRUE(names.contains(it.key())) << it.key().toStdString();
        EXPECT_FALSE(names.value(it.key()).toString().isEmpty());
    }
    EXPECT_EQ(value("pullLevel"), ECUIO_STRETCH_DEFAULT_PULL_LEVEL);
    EXPECT_EQ(value("pullLoadLevel"), ECUIO_STRETCH_DEFAULT_PULL_LOAD_LEVEL);
    EXPECT_EQ(value("pullHoldS"), ECUIO_STRETCH_DEFAULT_PULL_HOLD_US / 1e6);
    EXPECT_EQ(value("pullGapS"), ECUIO_STRETCH_DEFAULT_PULL_GAP_US / 1e6);
    EXPECT_EQ(value("pullRise"), ECUIO_STRETCH_DEFAULT_PULL_RISE);
    EXPECT_EQ(value("pullGain"), ECUIO_STRETCH_DEFAULT_PULL_GAIN);
    EXPECT_EQ(value("cruiseHoldS"), ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US / 1e6);
    EXPECT_EQ(value("cruiseSpeedBand"),
              ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND);
    EXPECT_EQ(value("cruiseThrottleBand"),
              ECUIO_STRETCH_DEFAULT_CRUISE_THROTTLE_BAND);
    EXPECT_EQ(value("cruiseFloor"), ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR);
    // Each its own: no two alike but the two levels and the two holds'
    // neighbors, which the macros make differ where they should
    EXPECT_NE(value("pullHoldS"), value("pullGapS"));
    EXPECT_NE(value("pullHoldS"), value("cruiseHoldS"));
    EXPECT_NE(value("pullGain"), value("cruiseSpeedBand"));
    EXPECT_NE(value("cruiseSpeedBand"), value("cruiseThrottleBand"));

    int changed = 0;
    QObject::connect(&v, &RecordingView::unitSystemChanged, [&] { ++changed; });
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(changed, 1) << "the defaults' notify";
    EXPECT_DOUBLE_EQ(value("cruiseFloor"), 35.0) << "35 mph";
    EXPECT_DOUBLE_EQ(value("pullGain"),
                     ECUIO_STRETCH_DEFAULT_PULL_GAIN / 1.609344);
    EXPECT_DOUBLE_EQ(value("cruiseSpeedBand"),
                     ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND / 1.609344);
    EXPECT_EQ(value("pullLevel"), ECUIO_STRETCH_DEFAULT_PULL_LEVEL);
    EXPECT_EQ(value("pullRise"), ECUIO_STRETCH_DEFAULT_PULL_RISE);
    EXPECT_EQ(value("cruiseHoldS"), ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US / 1e6);

    // Handed back as rules, in either unit system, they find what core's
    // own defaults find
    for (const char* name : {"pull-twice", "cruise"}) {
        v.open(fixture(name));
        ASSERT_TRUE(waitFor([&] { return settled(v); }));
        for (uint32_t units : {ECUIO_UNITS_IMPERIAL, ECUIO_UNITS_METRIC}) {
            v.setUnitSystem(static_cast<int>(units));
            const QVariantList plain = askedStretches(v, true, true);
            const QVariantList ruled =
                askedStretches(v, true, true, v.stretchDefaults());
            EXPECT_EQ(plain.size(), 2) << name;
            EXPECT_EQ(ruled, plain) << name;
        }
    }
}
