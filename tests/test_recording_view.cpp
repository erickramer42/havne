// File/layer: tests/test_recording_view.cpp | GTest + QtCore, app-layer tests
// Testing: UNIT: RecordingView (app/recordingview.h, ADR-0049 step 5) on
//          tests/fixtures/recording/: the open and the check off the GUI
//          thread, the refusals, a later open winning, a close canceling a
//          pass under way, the signals in display units and the default
//          plotted ones, columns, the values at a time, markers, findings.
//          Part of test_bridge, whose main() makes the QCoreApplication.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "recordingview.h"

namespace {

const QString kFixtures = QStringLiteral(ECUIO_RECORDING_FIXTURES);

QString fixture(const char* name)
{
    return kFixtures + QLatin1Char('/') + QLatin1String(name);
}

// Runs the GUI thread's events until done() or ms pass; done()'s answer
bool waitFor(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

bool settled(const RecordingView& v)
{
    return v.state() != RecordingView::Opening;
}

QVariantMap signalNamed(const RecordingView& v, const char* name)
{
    for (const QVariant& s : v.signalList()) {
        const QVariantMap m = s.toMap();
        if (m.value(QStringLiteral("name")).toString() == QLatin1String(name))
            return m;
    }
    return {};
}

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
// didn't run, and says why
TEST(RecordingViewTest, AFolderWithNoFramesOpensUnchecked)
{
    RecordingView v;
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_FALSE(v.check().value(QStringLiteral("ran")).toBool());
    EXPECT_TRUE(v.check()
                    .value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("frames.csv")))
        << v.check().value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(v.findings().isEmpty());
    EXPECT_EQ(v.signalList().size(), 4);
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

// A recording of the signals given as (name, rank), each a number with one
// sample at 0.1 s but those named in quiet, which have none
void writeRecording(const QString& folder,
                    const std::vector<std::pair<const char*, int>>& sigs,
                    const QStringList& quiet = {})
{
    QFile events(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    QByteArray head = "ecuio session recording, format 2 (ADR-0049)\n"
                      "started  2026-11-03 09:00:00.000 (local time)\n"
                      "profile  test\n";
    for (const auto& s : sigs) {
        head += "signal  " + QByteArray(s.first) + ",number,rpm,0," +
                QByteArray::number(s.second) + "\n";
    }
    head += "\n00:00:01.000  +1.000  end  test: 0 frames, 1 values, 0 records "
            "dropped, 0 notes dropped\n";
    events.write(head);
    QFile rows(folder + QStringLiteral("/signals.csv"));
    ASSERT_TRUE(rows.open(QIODevice::WriteOnly));
    rows.write("host_us,signal,value,unit\n");
    for (const auto& s : sigs) {
        if (quiet.contains(QLatin1String(s.first)))
            continue;
        rows.write("100000," + QByteArray(s.first) + ",1000,rpm\n");
    }
}

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

// Live's rule: in a ranked recording the DefaultPlotted numbers of the
// lowest ranks, a signal with no samples giving its place up and an
// unranked one never in; with no rank anywhere, every number
TEST(RecordingViewTest, TheDefaultIsLivesRule)
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
              QStringList({"S3", "S4", "S5", "S6", "S7", "S8"}));

    QTemporaryDir unranked;
    ASSERT_TRUE(unranked.isValid());
    writeRecording(
        unranked.path(),
        {{"A", 0}, {"B", 0}, {"C", 0}, {"D", 0}, {"E", 0}, {"F", 0}, {"G", 0}},
        {QStringLiteral("G")});
    v.open(unranked.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(plottedNames(v), QStringList({"A", "B", "C", "D", "E", "F"}));
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

// An open started over another drops the first one's result
TEST(RecordingViewTest, ALaterOpenWins)
{
    RecordingView v;
    v.open(fixture("findings"));
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(v.state(), RecordingView::Open);
    EXPECT_EQ(v.folder(), fixture("pull-early"));
    EXPECT_EQ(v.signalList().size(), 4);
    // Whatever the first pass posted is dropped, now and later
    waitFor([] { return false; }, 200);
    EXPECT_EQ(v.signalList().size(), 4);
    EXPECT_TRUE(v.findings().isEmpty());
}

// A long open reports its progress, and a close stops it where it is:
// nothing it posts afterwards lands
TEST(RecordingViewTest, AnOpenReportsProgressAndACloseStopsIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        // About 30 MB of rows: the reader reports every 4 MiB
        QFile f(dir.filePath(QStringLiteral("signals.csv")));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("host_us,signal,value,unit\n");
        QByteArray chunk;
        for (qint64 t = 0; t < 1500000; ++t) {
            chunk += QByteArray::number(100000 + t * 1000) + ",RPM,1234,rpm\n";
            if (chunk.size() > (1 << 20)) {
                f.write(chunk);
                chunk.clear();
            }
        }
        f.write(chunk);
    }
    RecordingView v;
    double seen = 0;
    QObject::connect(&v, &RecordingView::progressChanged, [&] {
        if (v.state() == RecordingView::Opening)
            seen = std::max(seen, v.progress());
    });
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return seen > 0 || settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Opening)
        << "the open ended before any progress";
    EXPECT_GT(seen, 0.0);
    EXPECT_LT(seen, 1.0);
    v.close();
    EXPECT_EQ(v.state(), RecordingView::Closed);
    EXPECT_EQ(v.progress(), 0.0);
    waitFor([] { return false; }, 200);
    EXPECT_EQ(v.state(), RecordingView::Closed);
    EXPECT_TRUE(v.signalList().isEmpty());
}

// A view destroyed mid-open stops the pass and leaves nothing behind
TEST(RecordingViewTest, DestroyedWhileOpeningIsSafe)
{
    {
        RecordingView v;
        v.open(fixture("findings"));
    }
    waitFor([] { return false; }, 100);
    SUCCEED();
}
