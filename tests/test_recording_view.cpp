// File/layer: tests/test_recording_view.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

namespace {

QList<double> doubles(const QVariantMap& m, const char* key)
{
    return m.value(QLatin1String(key)).value<QList<double>>();
}

} // namespace

// The findings fixture: the reader's view and the check's findings, in the
// order the check settled them, with nothing done on the GUI thread but
// taking the result
TEST(RecordingViewTest, OpensAndChecksARecording)
{
    RecordingView v;
    v.open(fixture("findings"));
    EXPECT_EQ(v.state(), RecordingView::Opening)
        << "the open runs on its own thread";
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.error().isEmpty());
    EXPECT_EQ(v.progress(), 1.0);
    EXPECT_EQ(v.durationMs(), 20000.0);
    EXPECT_EQ(v.info().value(QStringLiteral("format")).toInt(),
              ECUIO_RECORDING_FORMAT_CORE);
    EXPECT_EQ(v.info().value(QStringLiteral("utcStartMs")).toDouble(),
              1793721600000.0);

    const QVariantList sigs = v.signalList();
    ASSERT_EQ(sigs.size(), 1);
    const QVariantMap rpm = sigs[0].toMap();
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("RPM"));
    EXPECT_TRUE(rpm.value(QStringLiteral("plotted")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("lowest")).toDouble(), 804.0);
    EXPECT_EQ(rpm.value(QStringLiteral("highest")).toDouble(), 1610.0);
    EXPECT_EQ(rpm.value(QStringLiteral("firstMs")).toDouble(), 100.3);

    ASSERT_EQ(v.markers().size(), 1);
    EXPECT_EQ(v.markers()[0].toMap().value(QStringLiteral("timeMs")).toDouble(),
              2000.0);
    EXPECT_EQ(v.markers()[0].toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("lap"));

    const QVariantMap check = v.check();
    EXPECT_TRUE(check.value(QStringLiteral("ran")).toBool());
    EXPECT_EQ(check.value(QStringLiteral("frames")).toDouble(), 509.0);
    EXPECT_EQ(check.value(QStringLiteral("ids")).toInt(), 3);
    EXPECT_FALSE(check.value(QStringLiteral("isotpSkipped")).toBool());

    // havne-cli info's golden for the same folder (info-findings.jsonl)
    const int kinds[] = {ECUIO_CHECK_GAP,      ECUIO_CHECK_RATE_DROP,
                         ECUIO_CHECK_ASLEEP,   ECUIO_CHECK_LATE_ID,
                         ECUIO_CHECK_OVERFLOW, ECUIO_CHECK_ISOTP,
                         ECUIO_CHECK_LINK,     ECUIO_CHECK_LINK};
    const QVariantList findings = v.findings();
    ASSERT_EQ(findings.size(), 8);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(findings[i].toMap().value(QStringLiteral("kind")).toInt(),
                  kinds[i])
            << i;
    }
    const QVariantMap gap = findings[0].toMap();
    EXPECT_EQ(gap.value(QStringLiteral("id")).toDouble(), -1.0)
        << "the whole bus's";
    EXPECT_EQ(gap.value(QStringLiteral("atMs")).toDouble(), 4950.0);
    EXPECT_EQ(gap.value(QStringLiteral("lengthMs")).toDouble(), 1550.0);
    EXPECT_EQ(findings[1].toMap().value(QStringLiteral("id")).toDouble(),
              0x3E9);
    EXPECT_EQ(findings[6].toMap().value(QStringLiteral("code")).toInt(),
              ECUIO_ERR_VEHICLE_TIMEOUT);
}

// A folder with no frames.csv is still a recording to view: its check
// didn't run and says the recording has no frames, and a decode through
// a profile is refused in the same words
TEST(RecordingViewTest, AFolderWithNoFramesOpensUnchecked)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.hasFrames());
    EXPECT_FALSE(v.check().value(QStringLiteral("ran")).toBool());
    EXPECT_EQ(v.check().value(QStringLiteral("error")).toString(),
              QStringLiteral("the recording has no frames"));
    EXPECT_TRUE(v.findings().isEmpty());
    EXPECT_EQ(v.signalList().size(), 4);

    /* Its frames through a profile: refused at once, in the same words,
     * never a pass that fails or waits */
    v.openFrames(fixture("pull-early"), QStringLiteral("obd2"));
    EXPECT_EQ(v.state(), RecordingView::Failed);
    EXPECT_EQ(v.error(), QStringLiteral("the recording has no frames"));
    v.open(fixture("findings"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_TRUE(v.hasFrames());
}

// A folder the reader refuses fails in core's words, never with the path
TEST(RecordingViewTest, AFolderWithNoSignalsFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(v.state(), RecordingView::Failed);
    EXPECT_TRUE(v.error().contains(QStringLiteral("signals.csv")))
        << v.error().toStdString();
    EXPECT_FALSE(v.error().contains(dir.path()));
    EXPECT_TRUE(v.signalList().isEmpty());
    EXPECT_TRUE(v.columns({0}, 0, 1000, 1).isEmpty());
}

// The ranked recording's default: the numbers of the lowest ranks; a text
// signal is never plotted
TEST(RecordingViewTest, TheRankedNumbersArePlottedByDefault)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_TRUE(
        signalNamed(v, "RPM").value(QStringLiteral("plotted")).toBool());
    EXPECT_TRUE(
        signalNamed(v, "Throttle").value(QStringLiteral("plotted")).toBool());
    EXPECT_TRUE(
        signalNamed(v, "Coolant").value(QStringLiteral("plotted")).toBool());
    const QVariantMap vin = signalNamed(v, "VIN");
    EXPECT_TRUE(vin.value(QStringLiteral("isText")).toBool());
    EXPECT_FALSE(vin.value(QStringLiteral("plotted")).toBool());
    EXPECT_TRUE(std::isnan(vin.value(QStringLiteral("lowest")).toDouble()));
}

namespace {

QStringList plottedNames(const RecordingView& v)
{
    QStringList names;
    for (const QVariant& s : v.signalList()) {
        const QVariantMap m = s.toMap();
        if (m.value(QStringLiteral("plotted")).toBool())
            names << m.value(QStringLiteral("name")).toString();
    }
    return names;
}

} // namespace

// At most ViewerPlotted (4) plot on open: in a ranked recording the numbers
// of the lowest ranks, a signal with no samples giving its place up and an
// unranked one never in; with no rank anywhere, the first numbers in order
TEST(RecordingViewTest, AtMostFourPlotOnOpen)
{
    QTemporaryDir ranked;
    ASSERT_TRUE(ranked.isValid());
    writeRecording(ranked.path(),
                   {{"S1", 8},
                    {"S2", 7},
                    {"S3", 6},
                    {"S4", 5},
                    {"Quiet", 1},
                    {"S5", 4},
                    {"S6", 3},
                    {"S7", 2},
                    {"S8", 1},
                    {"Free", 0}},
                   {QStringLiteral("Quiet")});
    RecordingView v;
    v.open(ranked.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(plottedNames(v),
              QStringList({"S5", "S6", "S7", "S8"}));

    QTemporaryDir unranked;
    ASSERT_TRUE(unranked.isValid());
    writeRecording(
        unranked.path(),
        {{"A", 0}, {"B", 0}, {"C", 0}, {"D", 0}, {"E", 0}, {"F", 0}, {"G", 0}},
        {QStringLiteral("G")});
    v.open(unranked.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(plottedNames(v), QStringList({"A", "B", "C", "D"}));
}

// Each column's lowest and highest in display units, a column with no
// sample NaN
TEST(RecordingViewTest, ColumnsHoldEachColumnsRange)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const QVariantList cols = v.columns({rpm}, 0, 60000, 2);
    ASSERT_EQ(cols.size(), 1);
    const QVariantMap c = cols[0].toMap();
    EXPECT_EQ(c.value(QStringLiteral("index")).toInt(), rpm);
    const QList<double> lows = doubles(c, "lows");
    const QList<double> highs = doubles(c, "highs");
    const QList<double> samples = doubles(c, "samples");
    ASSERT_EQ(lows.size(), 2);
    EXPECT_EQ(lows[0], 1981.0);
    EXPECT_EQ(highs[0], 6806.0);
    EXPECT_EQ(samples[0], 299.0);
    EXPECT_TRUE(std::isnan(lows[1])) << "nothing after 30 s";
    EXPECT_EQ(samples[1], 0.0);

    EXPECT_TRUE(v.columns({rpm}, 1000, 1000, 1).isEmpty()) << "an empty range";
    EXPECT_TRUE(v.columns({99}, 0, 1000, 1).isEmpty()) << "no such signal";
    EXPECT_TRUE(v.columns({rpm}, 0, 1000, 0).isEmpty()) << "no columns";
    EXPECT_FALSE(v.stale());
}

// A logger's CSV imported by its column map (ADR-0054's appendix "the CSV
// import as built") opens as any recording: no frames, so unchecked; its
// signals ranked in the map's order, so plotted; its columns its samples
TEST(RecordingViewTest, AnImportedCsvOpensAndPlots)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = importedCsv(dir);
    ASSERT_FALSE(folder.isEmpty());

    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.hasFrames());
    EXPECT_FALSE(v.check().value(QStringLiteral("ran")).toBool());
    EXPECT_EQ(v.check().value(QStringLiteral("error")).toString(),
              QStringLiteral("the recording has no frames"));
    EXPECT_EQ(v.signalList().size(), 3);
    for (const char* name : {"RPM", "Engine Load", "Lambda"}) {
        EXPECT_TRUE(
            signalNamed(v, name).value(QStringLiteral("plotted")).toBool())
            << name;
    }
    const int rpm =
        signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    const QVariantList cols = v.columns({rpm}, 0, 2000, 2);
    ASSERT_EQ(cols.size(), 1);
    const QVariantMap c = cols[0].toMap();
    const QList<double> lows = doubles(c, "lows");
    const QList<double> highs = doubles(c, "highs");
    const QList<double> samples = doubles(c, "samples");
    ASSERT_EQ(lows.size(), 2);
    EXPECT_EQ(lows[0], 780.5);
    EXPECT_EQ(highs[0], 2100.0);
    EXPECT_EQ(samples[0], 4.0) << "0 to 0.75 s";
    EXPECT_EQ(lows[1], 900.0);
    EXPECT_EQ(highs[1], 3420.25);
    EXPECT_EQ(samples[1], 4.0) << "1 to 1.75 s";
}

// A window from before the start (a view aligned on another recording):
// the columns wholly before it empty, and every sample counted once
// wherever the start falls in a column, narrow or a second and wider
TEST(RecordingViewTest, AWindowFromBeforeTheStartCountsEverySampleOnce)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const QVariantMap rpmSignal = signalNamed(v, "RPM");
    const int rpm = rpmSignal.value(QStringLiteral("index")).toInt();
    const double total = rpmSignal.value(QStringLiteral("samples")).toDouble();
    ASSERT_GT(total, 0);
    for (const double t0 :
         {-0.5, -250.0, -1000.0, -1234.567, -10000.0, -29999.0}) {
        for (const int n : {1, 3, 7, 30, 31, 299, 3000}) {
            const QVariantList cols = v.columns({rpm}, t0, 31000, n);
            ASSERT_EQ(cols.size(), 1) << t0 << " ms, " << n;
            const QList<double> samples = doubles(cols[0].toMap(), "samples");
            const QList<double> lows = doubles(cols[0].toMap(), "lows");
            ASSERT_EQ(samples.size(), n);
            double sum = 0;
            for (int c = 0; c < n; ++c) {
                sum += samples[c];
                if (t0 + (31000 - t0) * (c + 1) / n < -1) {
                    EXPECT_EQ(samples[c], 0) << t0 << " ms, " << n << ": " << c;
                    EXPECT_TRUE(std::isnan(lows[c]));
                }
            }
            EXPECT_EQ(sum, total) << t0 << " ms, " << n;
        }
    }

    // Ten seconds before: ten empty columns, then the recording's own
    const QList<double> early =
        doubles(v.columns({rpm}, -10000, 30000, 40)[0].toMap(), "highs");
    const QList<double> own =
        doubles(v.columns({rpm}, 0, 30000, 30)[0].toMap(), "highs");
    ASSERT_EQ(early.size(), 40);
    for (int c = 0; c < 10; ++c) {
        EXPECT_TRUE(std::isnan(early[c])) << c;
    }
    for (int c = 0; c < 30; ++c) {
        EXPECT_EQ(early[10 + c], own[c]) << c;
    }
    const QList<double> before =
        doubles(v.columns({rpm}, -5000, -1000, 4)[0].toMap(), "samples");
    EXPECT_EQ(before, QList<double>(4, 0.0)) << "a window wholly before it";
    EXPECT_FALSE(v.stale());
}

// Where two recordings line up, as havne-cli view --align-on finds it
// (tests/golden/cli/view-rise.jsonl, view-marker.jsonl,
// view-unaligned.jsonl), and a level in display units
TEST(RecordingViewTest, TheAlignmentFindsAreHavneClisOwn)
{
    RecordingView early;
    RecordingView late;
    early.open(fixture("pull-early"));
    late.open(fixture("pull-late"));
    ASSERT_TRUE(waitFor([&] { return settled(early) && settled(late); }));
    const int rpm =
        signalNamed(early, "RPM").value(QStringLiteral("index")).toInt();
    const int vin =
        signalNamed(early, "VIN").value(QStringLiteral("index")).toInt();
    EXPECT_EQ(early.findThreshold(rpm, 4000, 500, true), 14600);
    EXPECT_EQ(late.findThreshold(rpm, 4000, 500, true), 24600);
    EXPECT_EQ(early.findMarker(QStringLiteral("pull")), 11500);
    EXPECT_EQ(late.findMarker(QStringLiteral("pull")), 21500);
    EXPECT_EQ(early.findThreshold(rpm, 2010, 30, true), -1)
        << "its hysteresis keeps it from being found";
    const double fall = early.findThreshold(rpm, 4000, 500, false);
    EXPECT_GT(fall, 14600) << "after the pull";
    EXPECT_LT(fall, 30000);
    EXPECT_EQ(early.findMarker(QStringLiteral("pul")), -1) << "matched whole";
    EXPECT_EQ(early.findThreshold(vin, 1, 0, true), -1) << "a text signal";
    EXPECT_EQ(early.findThreshold(99, 1, 0, true), -1) << "no such signal";
    EXPECT_EQ(early.findThreshold(rpm, 4000, -1, true), -1)
        << "a negative hysteresis";

    // Coolant reaching 89.5 deg C, or the same level in deg F
    const int coolant =
        signalNamed(early, "Coolant").value(QStringLiteral("index")).toInt();
    const double metric = early.findThreshold(coolant, 89.5, 1.0, true);
    ASSERT_GT(metric, 0);
    early.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(early.findThreshold(coolant, 89.5 * 9 / 5 + 32, 1.8, true),
              metric);
    EXPECT_EQ(early.findThreshold(coolant, 89.5, 0.5, true), -1)
        << "89.5 deg F: the coolant never falls below it";

    RecordingView closed;
    EXPECT_EQ(closed.findMarker(QStringLiteral("pull")), -1);
}

// The engine starts, as havne-cli view --align-on engine-start finds them
// (tests/golden/cli/view-engine-start.jsonl): units of either case, in
// whatever units the page shows; none where the engine runs from the
// first sample
TEST(RecordingViewTest, TheEngineStartIsHavneClisOwn)
{
    RecordingView early;
    RecordingView late;
    RecordingView pull;
    early.open(fixture("start-early"));
    late.open(fixture("start-late"));
    pull.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor(
        [&] { return settled(early) && settled(late) && settled(pull); }));
    EXPECT_EQ(early.findEngineStart(), 5600);
    EXPECT_EQ(late.findEngineStart(), 12600);
    EXPECT_EQ(pull.findEngineStart(), -1) << "running from its first sample";
    late.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(late.findEngineStart(), 12600) << "rpm doesn't convert";

    RecordingView closed;
    EXPECT_EQ(closed.findEngineStart(), -1);
}

// The cursor's readout: the value in force at a time, its decimals, and
// nothing before a signal's first sample
TEST(RecordingViewTest, ValuesAtAreTheValuesInForce)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const QVariant rpm = signalNamed(v, "RPM").value(QStringLiteral("index"));
    const QVariant throttle =
        signalNamed(v, "Throttle").value(QStringLiteral("index"));
    const QVariant vin = signalNamed(v, "VIN").value(QStringLiteral("index"));

    const QVariantList at = v.valuesAt({rpm, throttle}, 1150);
    ASSERT_EQ(at.size(), 2);
    EXPECT_EQ(at[0].toMap().value(QStringLiteral("text")).toString(),
              QStringLiteral("2006"));
    EXPECT_EQ(at[0].toMap().value(QStringLiteral("unit")).toString(),
              QStringLiteral("rpm"));
    EXPECT_EQ(at[0].toMap().value(QStringLiteral("atMs")).toDouble(), 1100.0);
    EXPECT_EQ(at[1].toMap().value(QStringLiteral("text")).toString(),
              QStringLiteral("13.5"));

    const QVariantMap before = v.valuesAt({vin}, 400).value(0).toMap();
    EXPECT_EQ(before.value(QStringLiteral("text")).toString(), QString());
    EXPECT_EQ(before.value(QStringLiteral("atMs")).toDouble(), -1.0);
    EXPECT_EQ(v.valuesAt({vin}, 600)
                  .value(0)
                  .toMap()
                  .value(QStringLiteral("text"))
                  .toString(),
              QStringLiteral("ZZZFAKE11M0000001"));
}

// An enum signal's value in force carries its label from the
// recording's events.log (format 3), beside the raw number it plots; a
// number signal's carries none
TEST(RecordingViewTest, AnEnumsValueCarriesItsLabel)
{
    RecordingView v;
    v.open(fixture("fuel-status"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const QVariant fuel =
        signalNamed(v, "Fuel System 1").value(QStringLiteral("index"));
    const QVariant rpm = signalNamed(v, "RPM").value(QStringLiteral("index"));

    const QVariantMap cold = v.valuesAt({fuel}, 1000).value(0).toMap();
    EXPECT_EQ(cold.value(QStringLiteral("text")).toString(),
              QStringLiteral("1"));
    EXPECT_EQ(cold.value(QStringLiteral("label")).toString(),
              QStringLiteral("Open loop, cold"));
    EXPECT_EQ(v.valuesAt({fuel}, 4200)
                  .value(0)
                  .toMap()
                  .value(QStringLiteral("label"))
                  .toString(),
              QStringLiteral("Closed loop"));
    EXPECT_EQ(v.valuesAt({rpm}, 1000)
                  .value(0)
                  .toMap()
                  .value(QStringLiteral("label"))
                  .toString(),
              QString());
}

// A tapped cursor lands on the sample nearest it, before or after, the
// earlier of two as near; before the first sample on the first, past the
// last on the last; a signal with no samples or a bad index gives -1
TEST(RecordingViewTest, TheNearestSampleIsEitherSide)
{
    RecordingView v;
    EXPECT_EQ(v.nearestSampleMs(0, 100), -1.0); // not open
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    const QVariantMap rpm = signalNamed(v, "RPM");
    const int i = rpm.value(QStringLiteral("index")).toInt();

    // RPM is sampled every 100 ms from 100 ms
    EXPECT_EQ(v.nearestSampleMs(i, 1140), 1100.0);
    EXPECT_EQ(v.nearestSampleMs(i, 1160), 1200.0);
    EXPECT_EQ(v.nearestSampleMs(i, 1199.9), 1200.0);
    EXPECT_EQ(v.nearestSampleMs(i, 1150), 1100.0);
    EXPECT_EQ(v.nearestSampleMs(i, 1200), 1200.0);
    EXPECT_EQ(v.nearestSampleMs(i, 0), 100.0);
    const double last = rpm.value(QStringLiteral("lastMs")).toDouble();
    EXPECT_EQ(v.nearestSampleMs(i, last + 5000), last);
    EXPECT_EQ(v.nearestSampleMs(-1, 1140), -1.0);
    EXPECT_EQ(v.nearestSampleMs(v.signalList().size(), 1140), -1.0);
}

// With samples bunched after the cursor, the first one after it is the
// one weighed, not the last in reach
TEST(RecordingViewTest, TheNearestSampleAfterIsTheFirstAfter)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRecording(dir.path(), {{"RPM", 0}});
    QFile rows(dir.filePath(QStringLiteral("signals.csv")));
    ASSERT_TRUE(rows.open(QIODevice::Append));
    // 100 ms (written above), then 300, 310, 320 ms
    rows.write(
        "300000,RPM,2000,rpm\n310000,RPM,3000,rpm\n320000,RPM,4000,rpm\n");
    rows.close();
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open);
    const int i = signalNamed(v, "RPM").value(QStringLiteral("index")).toInt();
    // 100 ms back, 20 ms on: the bunch's first, not its last in reach
    EXPECT_EQ(v.nearestSampleMs(i, 280), 300.0);
    EXPECT_EQ(v.nearestSampleMs(i, 305), 300.0);
    EXPECT_EQ(v.nearestSampleMs(i, 306), 310.0);
    EXPECT_EQ(v.nearestSampleMs(i, 199), 100.0);
}

// Imperial units convert the ranges, the columns and the readout, and
// the list says so
TEST(RecordingViewTest, ImperialUnitsConvertEverything)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    int listChanges = 0;
    QObject::connect(&v, &RecordingView::signalListChanged,
                     [&] { ++listChanges; });
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    EXPECT_EQ(listChanges, 1);

    const QVariantMap coolant = signalNamed(v, "Coolant");
    EXPECT_EQ(coolant.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°F"));
    EXPECT_NEAR(coolant.value(QStringLiteral("lowest")).toDouble(), 190.4,
                1e-9);
    const int index = coolant.value(QStringLiteral("index")).toInt();

    const QVariantMap c = v.columns({index}, 1000, 2000, 1).value(0).toMap();
    EXPECT_NEAR(doubles(c, "lows").value(0), 190.4, 1e-9);
    EXPECT_NEAR(doubles(c, "highs").value(0), 190.4, 1e-9);

    const QVariantMap at = v.valuesAt({index}, 1500).value(0).toMap();
    EXPECT_EQ(at.value(QStringLiteral("text")).toString(),
              QStringLiteral("190"));
    EXPECT_EQ(at.value(QStringLiteral("unit")).toString(),
              QStringLiteral("°F"));
    EXPECT_EQ(at.value(QStringLiteral("atMs")).toDouble(), 1002.0);
}

// The pulls and cruises are core's, as havne-cli view --stretches lists
// them (tests/golden/cli/view-align-pull.jsonl and view-stretches-cruise),
// in display units
TEST(RecordingViewTest, AStretchesAskFindsThePullsAndCruises)
{
    RecordingView v;
    EXPECT_EQ(v.stretches(true, true), 0) << "not open";
    v.open(fixture("pull-twice"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(v.stretches(false, false), 0) << "neither kind";
    const int speed =
        signalNamed(v, "Vehicle Speed").value(QStringLiteral("index")).toInt();

    const QVariantList pulls = askedStretches(v, true, false);
    ASSERT_EQ(pulls.size(), 2) << "neither the stab nor the part throttle";
    const QVariantMap first = pulls[0].toMap();
    EXPECT_EQ(first.value(QStringLiteral("kind")).toString(),
              QStringLiteral("pull"));
    EXPECT_EQ(first.value(QStringLiteral("fromMs")).toDouble(), 18002.0);
    EXPECT_EQ(first.value(QStringLiteral("toMs")).toDouble(), 25002.0)
        << "the throttle's lift, its sample 2 ms after speed's";
    EXPECT_EQ(first.value(QStringLiteral("start")).toDouble(), 70.0);
    EXPECT_EQ(first.value(QStringLiteral("end")).toDouble(), 128.0);
    EXPECT_EQ(first.value(QStringLiteral("unit")).toString(),
              QStringLiteral("km/h"));
    EXPECT_EQ(first.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_EQ(first.value(QStringLiteral("signal")).toInt(), speed);
    const QVariantMap second = pulls[1].toMap();
    EXPECT_EQ(second.value(QStringLiteral("fromMs")).toDouble(), 31002.0);
    EXPECT_EQ(second.value(QStringLiteral("toMs")).toDouble(), 35002.0);
    // Asked to end at top speed: speed's first sample at its highest
    const QVariantList atTop = askedStretches(
        v, true, false,
        {{QStringLiteral("pullEnd"), QStringLiteral("top-speed")}});
    ASSERT_EQ(atTop.size(), 2);
    EXPECT_EQ(atTop[0].toMap().value(QStringLiteral("toMs")).toDouble(),
              25000.0);
    EXPECT_EQ(atTop[1].toMap().value(QStringLiteral("toMs")).toDouble(),
              35000.0);
    EXPECT_TRUE(askedStretches(v, false, true).isEmpty())
        << "cruises alone: the log has none";

    // Its ends in mph under imperial
    v.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QVariantMap mph = askedStretches(v, true, false)[1].toMap();
    EXPECT_EQ(mph.value(QStringLiteral("unit")).toString(),
              QStringLiteral("mph"));
    EXPECT_NEAR(mph.value(QStringLiteral("start")).toDouble(), 140 / 1.609344,
                1e-9);
    v.setUnitSystem(ECUIO_UNITS_METRIC);

    // Cruises alone, and both kinds in time order
    v.open(fixture("cruise"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_TRUE(askedStretches(v, true, false).isEmpty());
    const QVariantList cruises = askedStretches(v, false, true);
    ASSERT_EQ(cruises.size(), 2);
    EXPECT_EQ(cruises[0].toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("cruise"));
    EXPECT_EQ(cruises[0].toMap().value(QStringLiteral("fromMs")).toDouble(),
              0.0);
    EXPECT_EQ(cruises[1].toMap().value(QStringLiteral("fromMs")).toDouble(),
              12000.0);
    EXPECT_EQ(askedStretches(v, true, true).size(), 2);
}
