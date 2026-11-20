// File/layer: tests/test_recording_view_stats.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

// The answer to one statistics ask, waited for on the GUI thread
QVariantList askedStats(RecordingView& v, const QVariantList& ids, double t0,
                        double t1)
{
    QVariantList got;
    bool answered = false;
    const auto c = QObject::connect(&v, &RecordingView::statsReady,
                                    [&](int, const QVariantList& s) {
                                        got = s;
                                        answered = true;
                                    });
    const int request = v.stats(ids, t0, t1);
    EXPECT_GT(request, 0);
    EXPECT_TRUE(waitFor([&] { return answered; }));
    QObject::disconnect(c);
    return got;
}

double askedStep(RecordingView& v, int signal, double t, bool forward)
{
    double got = -2;
    const auto c = QObject::connect(&v, &RecordingView::stepReady,
                                    [&](int, double at) { got = at; });
    EXPECT_GT(v.step(signal, t, forward), 0);
    EXPECT_TRUE(waitFor([&] { return got != -2; }));
    QObject::disconnect(c);
    return got;
}

// A span's mean as a statistics ask gives it: [t0, t1) in ms
double meanOver(RecordingView& v, int signal, double t0, double t1)
{
    const QVariantList got = askedStats(v, {signal}, t0, t1 - 0.001);
    return got.value(0).toMap().value(QStringLiteral("mean")).toDouble();
}

// RPM over 1 s to 3 s of pull-early, worked out from signals.csv by hand;
// a sample at the span's end counts
TEST(RecordingViewTest, AStatsAskReadsTheSpan)
{
    RecordingView v;
    EXPECT_EQ(v.stats({0}, 0, 1000), 0) << "not open";
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const int vin =
        signalNamed(v, "VIN").value(QStringLiteral("index")).toInt();

    const QVariantList got = askedStats(v, {rpm, vin}, 1000, 3000);
    ASSERT_EQ(got.size(), 2);
    const QVariantMap s = got[0].toMap();
    EXPECT_EQ(s.value(QStringLiteral("index")).toInt(), rpm);
    EXPECT_EQ(s.value(QStringLiteral("samples")).toDouble(), 21.0);
    EXPECT_EQ(s.value(QStringLiteral("first")).toDouble(), 2001.0);
    EXPECT_EQ(s.value(QStringLiteral("last")).toDouble(), 1982.0);
    EXPECT_EQ(s.value(QStringLiteral("lowest")).toDouble(), 1982.0);
    EXPECT_EQ(s.value(QStringLiteral("lowestMs")).toDouble(), 3000.0);
    EXPECT_EQ(s.value(QStringLiteral("highest")).toDouble(), 2012.0);
    EXPECT_EQ(s.value(QStringLiteral("highestMs")).toDouble(), 1300.0);
    EXPECT_NEAR(s.value(QStringLiteral("mean")).toDouble(), 2003.476190476,
                1e-6);
    EXPECT_EQ(s.value(QStringLiteral("change")).toDouble(), -19.0);
    EXPECT_EQ(s.value(QStringLiteral("rate")).toDouble(), -9.5);
    EXPECT_EQ(s.value(QStringLiteral("unit")).toString(),
              QStringLiteral("rpm"));
    EXPECT_EQ(s.value(QStringLiteral("decimals")).toInt(), 0);

    const QVariantMap text = got[1].toMap();
    EXPECT_EQ(text.value(QStringLiteral("index")).toInt(), vin);
    EXPECT_TRUE(std::isnan(text.value(QStringLiteral("mean")).toDouble()));
    EXPECT_TRUE(std::isnan(text.value(QStringLiteral("change")).toDouble()));
    EXPECT_EQ(text.value(QStringLiteral("lowestMs")).toDouble(), -1.0);

    EXPECT_EQ(v.stats({}, 0, 1000), 0) << "no signals";
    EXPECT_EQ(v.stats({99}, 0, 1000), 0) << "no such signal";
    EXPECT_EQ(v.stats({rpm}, 1000, 1000), 0) << "an empty span";
    EXPECT_EQ(v.stats({rpm}, 2000, 1000), 0) << "backward";
    EXPECT_EQ(v.stats({rpm}, -2000, -1000), 0) << "all before the start";
    EXPECT_FALSE(v.stale());
}

// A span from before the start reads from the start, whose time is the
// length the rate divides by
TEST(RecordingViewTest, AStatsSpanFromBeforeTheStartReadsFromIt)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const QVariantMap early =
        askedStats(v, {rpm}, -5000, 1000).value(0).toMap();
    const QVariantMap own = askedStats(v, {rpm}, 0, 1000).value(0).toMap();
    EXPECT_EQ(early.value(QStringLiteral("samples")).toDouble(),
              own.value(QStringLiteral("samples")).toDouble());
    EXPECT_EQ(early.value(QStringLiteral("highest")).toDouble(),
              own.value(QStringLiteral("highest")).toDouble());
    EXPECT_TRUE(std::isnan(early.value(QStringLiteral("first")).toDouble()))
        << "nothing in force at the start";
}

// The statistics convert as the cursor's values do: a temperature's
// change and rate scale without the offset
TEST(RecordingViewTest, TheStatisticsAreInDisplayUnits)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const int coolant =
        signalNamed(v, "Coolant").value(QStringLiteral("index")).toInt();
    // 88 deg C through 28 s, 90 by 29 s
    const QVariantMap s =
        askedStats(v, {coolant}, 1500, 29500).value(0).toMap();
    EXPECT_EQ(s.value(QStringLiteral("unit")).toString(), QStringLiteral("°F"));
    EXPECT_NEAR(s.value(QStringLiteral("first")).toDouble(), 190.4, 1e-9);
    EXPECT_NEAR(s.value(QStringLiteral("last")).toDouble(), 194.0, 1e-9);
    EXPECT_NEAR(s.value(QStringLiteral("change")).toDouble(), 3.6, 1e-9);
    EXPECT_NEAR(s.value(QStringLiteral("rate")).toDouble(), 3.6 / 28.0, 1e-9);
    EXPECT_NEAR(s.value(QStringLiteral("lowest")).toDouble(), 190.4, 1e-9);
    EXPECT_NEAR(s.value(QStringLiteral("highest")).toDouble(), 194.0, 1e-9);
    const double mean = s.value(QStringLiteral("mean")).toDouble();
    EXPECT_GE(mean, 190.4);
    EXPECT_LE(mean, 194.0);
}

TEST(RecordingViewTest, AStepAskFindsTheNextSample)
{
    RecordingView v;
    EXPECT_EQ(v.step(0, 100, true), 0) << "not open";
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const QVariantMap rpm = signalNamed(v, "RPM");
    const int i = rpm.value(QStringLiteral("index")).toInt();
    const double last = rpm.value(QStringLiteral("lastMs")).toDouble();
    // RPM is sampled every 100 ms from 100 ms
    EXPECT_EQ(askedStep(v, i, 1150, true), 1200.0);
    EXPECT_EQ(askedStep(v, i, 1150, false), 1100.0);
    EXPECT_EQ(askedStep(v, i, 1200, true), 1300.0) << "strictly after";
    EXPECT_EQ(askedStep(v, i, 1200, false), 1100.0) << "strictly before";
    EXPECT_EQ(askedStep(v, i, last, true), -1.0) << "past the last";
    EXPECT_EQ(askedStep(v, i, 100, false), -1.0) << "before the first";
    EXPECT_EQ(v.step(-1, 100, true), 0) << "no such signal";
    EXPECT_EQ(v.step(99, 100, true), 0) << "no such signal";
}

// A column's mean is over the column's own span of the recording's time,
// wherever its ends fall in a second: what a difference between two
// recordings a part of a second apart is taken from. A query's columns of
// a second or more hold the whole seconds that start in them
TEST(RecordingViewTest, ColumnMeansAreOverEachColumnsOwnSpan)
{
    RecordingView v;
    EXPECT_EQ(v.columnMeans({0}, 0, 1000, 1), 0) << "not open";
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const int vin =
        signalNamed(v, "VIN").value(QStringLiteral("index")).toInt();

    // Ten columns of 2 s that start half a second into a second
    const std::vector<QList<double>> got =
        askedMeans(v, {rpm, vin}, 500, 20500, 10);
    ASSERT_EQ(got.size(), 2u);
    ASSERT_EQ(got[0].size(), 10);
    int apart = 0;
    for (int c = 0; c < 10; ++c) {
        const double from = 500 + 2000.0 * c;
        EXPECT_NEAR(got[0][c], meanOver(v, rpm, from, from + 2000), 1e-9)
            << "column " << c;
        // The whole seconds a query places there are another span
        if (std::abs(got[0][c] - meanOver(v, rpm, from + 500, from + 2500)) >
            0.5) {
            ++apart;
        }
    }
    EXPECT_GT(apart, 0) << "no column tells its own span from whole seconds";
    ASSERT_EQ(got[1].size(), 10);
    for (int c = 0; c < 10; ++c) {
        EXPECT_TRUE(std::isnan(got[1][c])) << "a text signal has no mean";
    }

    // Columns under a second, and one with no sample in it (RPM is sampled
    // ten times a second)
    const std::vector<QList<double>> narrow =
        askedMeans(v, {rpm}, 1050, 1350, 6);
    ASSERT_EQ(narrow.size(), 1u);
    ASSERT_EQ(narrow[0].size(), 6);
    EXPECT_EQ(narrow[0][1], meanOver(v, rpm, 1100, 1150));
    EXPECT_TRUE(std::isnan(narrow[0][0])) << "1050 to 1100 holds no sample";

    EXPECT_EQ(v.columnMeans({}, 0, 1000, 4), 0) << "no signals";
    EXPECT_EQ(v.columnMeans({99}, 0, 1000, 4), 0) << "no such signal";
    EXPECT_EQ(v.columnMeans({rpm}, 1000, 1000, 4), 0) << "an empty span";
    EXPECT_EQ(v.columnMeans({rpm}, 2000, 1000, 4), 0) << "backward";
    EXPECT_EQ(v.columnMeans({rpm}, 0, 1000, 0), 0) << "no columns";
    EXPECT_EQ(v.columnMeans({rpm}, 0, 1000, 65537), 0) << "too many columns";
    EXPECT_FALSE(v.stale());
}

// A window from before the start: the columns before it are empty, and
// the one across it reads from the start
TEST(RecordingViewTest, ColumnMeansFromBeforeTheStartAreEmptyUntilIt)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const std::vector<QList<double>> got = askedMeans(v, {rpm}, -2500, 3500, 6);
    ASSERT_EQ(got.size(), 1u);
    ASSERT_EQ(got[0].size(), 6);
    EXPECT_TRUE(std::isnan(got[0][0])) << "-2.5 to -1.5 s";
    EXPECT_TRUE(std::isnan(got[0][1])) << "-1.5 to -0.5 s";
    EXPECT_NEAR(got[0][2], meanOver(v, rpm, 0, 500), 1e-9)
        << "across the start";
    EXPECT_NEAR(got[0][3], meanOver(v, rpm, 500, 1500), 1e-9);
    EXPECT_NEAR(got[0][5], meanOver(v, rpm, 2500, 3500), 1e-9);
}

// The means are in the units the view displays, as its statistics are
TEST(RecordingViewTest, ColumnMeansAreInDisplayUnits)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int coolant =
        signalNamed(v, "Coolant").value(QStringLiteral("index")).toInt();
    const double metric = askedMeans(v, {coolant}, 0, 30000, 3).at(0).at(1);
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const double imperial = askedMeans(v, {coolant}, 0, 30000, 3).at(0).at(1);
    EXPECT_NEAR(imperial, metric * 9.0 / 5.0 + 32.0, 1e-9);
    EXPECT_NEAR(imperial, meanOver(v, coolant, 10000, 20000), 1e-9);
}

// A later means ask replaces an earlier one, a pass under way included,
// and a close drops one; the other kinds still answer
TEST(RecordingViewTest, ALaterColumnMeansAskReplacesAnEarlierOne)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    std::vector<int> answered;
    const auto c = QObject::connect(
        &v, &RecordingView::columnMeansReady,
        [&](int request, const QVariantList&) { answered.push_back(request); });
    int last = 0;
    for (int k = 1; k <= 20; ++k) {
        last = v.columnMeans({rpm}, 0, 1000.0 * k, 500);
        ASSERT_GT(last, 0);
    }
    ASSERT_TRUE(
        waitFor([&] { return !answered.empty() && answered.back() == last; }));
    EXPECT_EQ(answered.size(), 1u) << "an earlier ask was answered too";
    EXPECT_EQ(askedStats(v, {rpm}, 1000, 3000).size(), 1);

    // An answer posted and waiting when a newer ask is put in is dropped:
    // no events run in the wait
    answered.clear();
    ASSERT_GT(v.columnMeans({rpm}, 0, 30000, 500), 0);
    QThread::msleep(300);
    last = v.columnMeans({rpm}, 0, 20000, 500);
    ASSERT_GT(last, 0);
    ASSERT_TRUE(
        waitFor([&] { return !answered.empty() && answered.back() == last; }));
    EXPECT_EQ(answered.size(), 1u) << "the answer before was delivered too";

    // A pass the task has taken stops when a newer ask arrives
    answered.clear();
    const int stopped = v.meansStoppedForTesting();
    int held = v.asksHeldForTesting();
    v.setAsksHoldForTesting(200);
    ASSERT_GT(v.columnMeans({rpm}, 0, 30000, 500), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > held; }));
    last = v.columnMeans({rpm}, 0, 20000, 500);
    ASSERT_GT(last, 0);
    ASSERT_TRUE(
        waitFor([&] { return !answered.empty() && answered.back() == last; }));
    EXPECT_EQ(answered.size(), 1u);
    EXPECT_EQ(v.meansStoppedForTesting(), stopped + 1)
        << "the pass under way read on to its end";

    answered.clear();
    held = v.asksHeldForTesting();
    v.setAsksHoldForTesting(300);
    ASSERT_GT(v.columnMeans({rpm}, 0, 30000, 500), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > held; }));
    v.close();
    EXPECT_FALSE(waitFor([&] { return !answered.empty(); }, 800))
        << "a closed recording's means were answered";
    QObject::disconnect(c);
}

// A means ask the task has no memory for is answered with no rows, so a
// page pairing two views' answers isn't left waiting; the next one answers
TEST(RecordingViewTest, AColumnMeansAskTheTaskCantAnswerSaysSo)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    v.setLoseMeansForTesting(true);
    EXPECT_TRUE(askedMeans(v, {rpm}, 0, 30000, 50).empty());
    EXPECT_EQ(askedStats(v, {rpm}, 1000, 3000).size(), 1)
        << "the other kinds answer";
    v.setLoseMeansForTesting(false);
    EXPECT_EQ(askedMeans(v, {rpm}, 0, 30000, 50).size(), 1u);
}

// Only the newest ask of a kind is answered, so a dragged cursor's span
// never shows an older one's numbers; each kind answers for itself
TEST(RecordingViewTest, ALaterAskReplacesAnEarlierOne)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    std::vector<int> statsAnswered;
    std::vector<int> stepsAnswered;
    QObject::connect(&v, &RecordingView::statsReady,
                     [&](int request, const QVariantList&) {
                         statsAnswered.push_back(request);
                     });
    QObject::connect(&v, &RecordingView::stepReady, [&](int request, double) {
        stepsAnswered.push_back(request);
    });
    // No events run in the wait, so the first answers are posted and waiting
    ASSERT_GT(v.stats({rpm}, 0, 1000), 0);
    ASSERT_GT(v.step(rpm, 100, true), 0);
    QThread::msleep(300);
    int lastStats = 0;
    int lastStep = 0;
    for (int k = 2; k <= 20; ++k) {
        lastStats = v.stats({rpm}, 0, 1000.0 * k);
        lastStep = v.step(rpm, 100.0 * k, true);
    }
    ASSERT_TRUE(waitFor([&] {
        return !statsAnswered.empty() && statsAnswered.back() == lastStats &&
               !stepsAnswered.empty() && stepsAnswered.back() == lastStep;
    }));
    EXPECT_EQ(statsAnswered.size(), 1u);
    EXPECT_EQ(stepsAnswered.size(), 1u);
}

TEST(RecordingViewTest, ACloseDropsAnAskAndANewOpenAnswers)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    int answers = 0;
    int stepAnswers = 0;
    const auto c =
        QObject::connect(&v, &RecordingView::statsReady,
                         [&](int, const QVariantList&) { ++answers; });
    const auto cs = QObject::connect(&v, &RecordingView::stepReady,
                                     [&](int, double) { ++stepAnswers; });
    ASSERT_GT(v.stats({rpm}, 0, 30000), 0);
    ASSERT_GT(v.step(rpm, 100, true), 0);
    QThread::msleep(300); // answered and posted: no events run in the wait
    v.close();
    EXPECT_EQ(v.stats({rpm}, 0, 30000), 0) << "closed";
    EXPECT_EQ(v.step(rpm, 100, true), 0) << "closed";
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(answers, 0) << "the close's own generation has gone";
    EXPECT_EQ(stepAnswers, 0) << "and the step's";
    QObject::disconnect(c);
    QObject::disconnect(cs);

    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(askedStats(v, {rpm}, 1000, 3000).size(), 1);
}

// A statistics ask over a long recording with a NaN in most seconds reads
// its raw rows, and core's call can't be stopped: a close leaves the ask
// to end on its own, the reader open under it until then, and never waits
TEST(RecordingViewTest, ACloseNeverWaitsForAStatsOrStepAsk)
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
    const int hold = 2000;
    v.setAsksHoldForTesting(hold);
    QElapsedTimer timer;
    timer.start();
    ASSERT_GT(v.stats({rpm}, 0, 30000), 0);
    ASSERT_GT(v.step(rpm, 100, true), 0);
    // The task has taken an ask and is at its read
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    v.close();
    EXPECT_LT(timer.elapsed(), hold / 2)
        << "an ask or the close waited for the read";
    EXPECT_EQ(v.state(), RecordingView::Closed);

    // Another recording opens and answers while the first ask still holds
    v.setAsksHoldForTesting(0);
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(askedStats(v, {rpm}, 1000, 3000).size(), 1);
    EXPECT_GE(askedStep(v, rpm, 100, true), 0.0);
    EXPECT_LT(timer.elapsed(), hold)
        << "the new asks waited for the abandoned one";
    answers = 0;
    // The abandoned ask makes no read once its hold ends, and nothing is
    // answered
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, hold + 500));
}

// An asks' task that can't be started: a statistics or step ask says 0,
// leaves nothing queued, and the next ask answers once one can
TEST(RecordingViewTest, AnAsksThreadThatCantStartSaysSoAndTheNextAskAnswers)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    int answers = 0;
    const auto c = QObject::connect(&v, &RecordingView::statsReady,
                                    [&](int, const QVariantList&) { ++answers; });
    const auto cs = QObject::connect(&v, &RecordingView::stepReady,
                                     [&](int, double) { ++answers; });
    v.setFailThreadForTesting(true);
    EXPECT_EQ(v.stats({rpm}, 0, 30000), 0);
    EXPECT_EQ(v.step(rpm, 100, true), 0);
    QThread::msleep(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(answers, 0) << "nothing was left queued";
    v.setFailThreadForTesting(false);
    EXPECT_EQ(askedStats(v, {rpm}, 1000, 3000).size(), 1);
    EXPECT_GE(askedStep(v, rpm, 100, true), 0.0);
    QObject::disconnect(c);
    QObject::disconnect(cs);
}

// The view gone mid-ask: the task ends on its own and posts to no one
TEST(RecordingViewTest, DestroyedWithAnAskUnderWayIsSafe)
{
    QElapsedTimer timer;
    const int hold = 1500;
    int answers = 0;
    {
        RecordingView v;
        v.open(fixture("pull-early"));
        ASSERT_TRUE(waitFor([&] { return settled(v); }));
        const int rpm =
            signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
        QObject::connect(&v, &RecordingView::statsReady,
                         [&](int, const QVariantList&) { ++answers; });
        QObject::connect(&v, &RecordingView::stepReady,
                         [&](int, double) { ++answers; });
        v.setAsksHoldForTesting(hold);
        timer.start();
        ASSERT_GT(v.stats({rpm}, 0, 30000), 0);
        ASSERT_GT(v.step(rpm, 100, true), 0);
        ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    }
    EXPECT_LT(timer.elapsed(), hold / 2) << "an ask or the destructor waited";
    // The task reads on by itself, its hold and the read, and ends
    EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(hold + 10000))
        << "the abandoned task never ended";
    EXPECT_GE(timer.elapsed(), hold - 50)
        << "no read was under way when the view went";
    // Its answers, posted for a view that is gone, run here and reach no one
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    EXPECT_EQ(answers, 0);
}

// The file gone under an open recording: a partial second's raw rows
// can't be read, and the viewer says what's shown is the open's
TEST(RecordingViewTest, AStatsAskOnAChangedFileAnswersEmptyAndGoesStale)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRecording(dir.path(), {{"RPM", 0}});
    QFile rows(dir.filePath(QStringLiteral("signals.csv")));
    ASSERT_TRUE(rows.open(QIODevice::Append));
    rows.write("1100000,RPM,2000,rpm\n1900000,RPM,3000,rpm\n");
    rows.close();
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open);
    const int i = signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("signals.csv"))));
    EXPECT_TRUE(askedStats(v, {i}, 1200, 1800).isEmpty());
    EXPECT_TRUE(v.stale());
}
