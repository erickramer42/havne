// File/layer: tests/test_recording_view.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "recordingview.h"

#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

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

// A recording of the car's replies under obd2, frames.csv alone, in a
// folder named as a vehicle's recording is (its label in the name):
// readiness (the light on), one stored code, an engine speed
QString carRecording(const QTemporaryDir& root)
{
    const QString folder =
        root.path() + QStringLiteral("/20261107-100000 Daily driver");
    QDir().mkpath(folder);
    QFile events(folder + QStringLiteral("/events.log"));
    if (!events.open(QIODevice::WriteOnly)) {
        return {};
    }
    events.write("ecuio session recording, format 2 (ADR-0049)\n"
                 "started  2026-11-07 10:00:00.000 (local time)\n"
                 "utc  2026-11-07T16:00:00.000Z\n"
                 "core  0.5.0\n"
                 "uri  elm327://COM3?profile=obd2\n"
                 "profile  obd2\n"
                 "frame flags  complete\n"
                 "caller  havne-cli 0.5.0\n"
                 "\n"
                 "10:00:01.000  +1.000  end  havne-cli: seconds: 3 frames, 0 "
                 "values, 0 records dropped, 0 notes dropped\n");
    events.close();
    QFile frames(folder + QStringLiteral("/frames.csv"));
    if (!frames.open(QIODevice::WriteOnly)) {
        return {};
    }
    frames.write("host_us,timestamp_us,id,flags,data\n"
                 "100000,100000,0x7E8,0x0,06 41 01 83 07 E5 00 00\n"
                 "200000,200000,0x7E8,0x0,04 43 01 03 01 00 00 00\n"
                 "300000,300000,0x7E8,0x0,04 41 0C 0C 90 00 00 00\n");
    return folder;
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

// logger-a's CSV imported by its column map into dir's "rec": the folder,
// or "" (the case failed) when the fixture or the import fails
QString importedCsv(const QTemporaryDir& dir)
{
    const QString base = QStringLiteral(ECUIO_IMPORT_FIXTURES "/csv/logger-a");
    QFile mapFile(base + QStringLiteral("/map.json"));
    if (!mapFile.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "no map.json";
        return QString();
    }
    const QByteArray map = mapFile.readAll();
    const QByteArray in = (base + QStringLiteral("/log.csv")).toUtf8();
    const QByteArray out = (dir.path() + QStringLiteral("/rec")).toUtf8();
    ecuio_import_options_t o{};
    o.format = ECUIO_IMPORT_CSV;
    o.in_path_utf8 = in.constData();
    o.out_folder_utf8 = out.constData();
    o.column_map_utf8 = map.constData();
    ecuio_import_report_t r{};
    if (ecuio_import(&o, &r) != ECUIO_OK) {
        ADD_FAILURE() << ecuio_last_error_detail();
        return QString();
    }
    return QString::fromUtf8(out);
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

// The answer to one column means ask, waited for on the GUI thread: each
// signal's means, in the ask's order
std::vector<QList<double>> askedMeans(RecordingView& v, const QVariantList& ids,
                                      double t0, double t1, int columns)
{
    QVariantList got;
    bool answered = false;
    const auto c = QObject::connect(&v, &RecordingView::columnMeansReady,
                                    [&](int, const QVariantList& m) {
                                        got = m;
                                        answered = true;
                                    });
    EXPECT_GT(v.columnMeans(ids, t0, t1, columns), 0);
    EXPECT_TRUE(waitFor([&] { return answered; }));
    QObject::disconnect(c);
    std::vector<QList<double>> out;
    for (qsizetype k = 0; k < got.size(); ++k) {
        const QVariantMap m = got[k].toMap();
        EXPECT_EQ(m.value(QStringLiteral("index")).toInt(), ids[k].toInt());
        out.push_back(m.value(QStringLiteral("means")).value<QList<double>>());
    }
    return out;
}

// A span's mean as a statistics ask gives it: [t0, t1) in ms
double meanOver(RecordingView& v, int signal, double t0, double t1)
{
    const QVariantList got = askedStats(v, {signal}, t0, t1 - 0.001);
    return got.value(0).toMap().value(QStringLiteral("mean")).toDouble();
}

// The answer to one stretches ask, waited for on the GUI thread
QVariantList askedStretches(RecordingView& v, bool pulls, bool cruises,
                            const QVariantMap& rules = {})
{
    QVariantList got;
    bool answered = false;
    const auto c = QObject::connect(&v, &RecordingView::stretchesReady,
                                    [&](int, const QVariantList& s, bool ok) {
                                        got = s;
                                        answered = true;
                                        EXPECT_TRUE(ok);
                                    });
    EXPECT_GT(v.stretches(pulls, cruises, rules), 0)
        << v.stretchesProblem().toStdString();
    EXPECT_TRUE(waitFor([&] { return answered; }));
    QObject::disconnect(c);
    return got;
}

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

// The pulls and cruises one ask finds under rules
qsizetype found(RecordingView& v, bool pulls, bool cruises,
                const QVariantMap& rules)
{
    return askedStretches(v, pulls, cruises, rules).size();
}

QVariantMap rule(const char* key, const QVariant& value)
{
    return {{QLatin1String(key), value}};
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
    EXPECT_EQ(first.value(QStringLiteral("toMs")).toDouble(), 25000.0);
    EXPECT_EQ(first.value(QStringLiteral("start")).toDouble(), 70.0);
    EXPECT_EQ(first.value(QStringLiteral("end")).toDouble(), 128.0);
    EXPECT_EQ(first.value(QStringLiteral("unit")).toString(),
              QStringLiteral("km/h"));
    EXPECT_EQ(first.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_EQ(first.value(QStringLiteral("signal")).toInt(), speed);
    const QVariantMap second = pulls[1].toMap();
    EXPECT_EQ(second.value(QStringLiteral("fromMs")).toDouble(), 31002.0);
    EXPECT_EQ(second.value(QStringLiteral("toMs")).toDouble(), 35000.0);
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

namespace {

// pull-twice's events.log over a long signals.csv: speed, engine speed
// and throttle every stepMs, a ten-second pull every periodS seconds for
// 6000 s. Every 120 s at 10 ms (the default) is 1.8 million rows and 50
// pulls, fewer than the viewer's first ask holds, so one call is a whole
// pass; every 60 s is 100, which takes a second call
QString denseRecording(const QTemporaryDir& dir, int periodS = 120,
                       int stepMs = 10)
{
    const QString folder =
        dir.filePath(QStringLiteral("dense-%1-%2").arg(periodS).arg(stepMs));
    QDir().mkpath(folder);
    QFile::copy(fixture("pull-twice") + QStringLiteral("/events.log"),
                folder + QStringLiteral("/events.log"));
    QFile f(folder + QStringLiteral("/signals.csv"));
    if (!f.open(QIODevice::WriteOnly)) {
        return {};
    }
    f.write("host_us,signal,value,unit\n");
    QByteArray chunk;
    const qint64 period = periodS * qint64{1000000};
    for (qint64 us = 0; us < 6000 * qint64{1000000}; us += stepMs * 1000) {
        const bool pull = us % period >= period - 10000000;
        const qint64 into = us % period - (period - 10000000);
        chunk += QByteArray::number(us) + ",Vehicle Speed," +
                 QByteArray::number(pull ? 60 + into / 500000 : 60) +
                 ",km/h\n" + QByteArray::number(us + 1) + ",RPM," +
                 QByteArray::number(pull ? 2500 + into / 2500 : 2500) +
                 ",rpm\n" + QByteArray::number(us + 2) + ",Throttle," +
                 (pull ? "100.0" : "12.0") + ",%\n";
        if (chunk.size() > (1 << 20)) {
            f.write(chunk);
            chunk.clear();
        }
    }
    f.write(chunk);
    return folder;
}

} // namespace

// A close stops a stretches pass under way at its next second: timed
// against a whole pass over the same recording, the pass after the close
// ends well before a whole one would
TEST(RecordingViewTest, ACloseStopsAStretchesPassUnderWay)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 60000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QElapsedTimer timer;
    timer.start();
    ASSERT_GE(askedStretches(v, true, false).size(), 20);
    const qint64 whole = timer.elapsed();

    // Held, then closed at a third of a whole pass into its read
    const int hold = 200;
    v.setStretchesHoldForTesting(hold);
    timer.restart();
    ASSERT_GT(v.stretches(true, false), 0);
    QThread::msleep(static_cast<unsigned long>(hold + whole / 3));
    v.close();
    ASSERT_TRUE(waitFor([&] { return v.readsStoppedForTesting() > 0; },
                        hold + 2 * static_cast<int>(whole) + 5000))
        << "the pass read on to its end";
    EXPECT_LT(timer.elapsed(), hold + whole / 3 + whole / 2 + 100)
        << "a whole pass took " << whole << " ms";
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
    ASSERT_EQ(askedStretches(v, true, false).size(), 100)
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
// before it are never made, and none is answered
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
    const int hold = 300;
    v.setAsksHoldForTesting(hold);
    QElapsedTimer timer;
    timer.start();
    ASSERT_GT(v.stats({rpm}, 0, 30000), 0);
    ASSERT_GT(v.step(rpm, 100, true), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    v.close();
    EXPECT_EQ(v.readsStoppedForTesting(), 0) << "nothing stopped yet";
    ASSERT_TRUE(
        waitFor([&] { return v.readsStoppedForTesting() == 2; }, hold + 2000));
    EXPECT_LT(timer.elapsed(), hold + 1000);
    EXPECT_FALSE(waitFor([&] { return answers > 0; }, 300));
    EXPECT_EQ(v.readsStoppedForTesting(), 2);
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
              QStringLiteral("Pull level must be a number from 0 to 100"));
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
        {"pullLevel", 100.5, "Pull level must be a number from 0 to 100"},
        {"pullLevel", -1, "Pull level must be a number from 0 to 100"},
        {"pullLoadLevel", nan,
         "Pull load level must be a number from 0 to 100"},
        {"cruiseThrottleBand", 101,
         "Cruise throttle band must be a number from 0 to 100"},
        {"pullHoldS", 3600.5, "Pull hold must be a number from 0 to 3600 s"},
        {"pullGapS", inf, "Pull gap must be a number from 0 to 3600 s"},
        {"cruiseHoldS", -0.1, "Cruise hold must be a number from 0 to 3600 s"},
        {"pullRise", -1, "Pull rise must be a number, 0 or more"},
        {"pullRise", inf, "Pull rise must be a number, 0 or more"},
        {"pullGain", 1000.5, "Pull gain must be a number from 0 to 1000 km/h"},
        {"cruiseSpeedBand", QStringLiteral("wide"),
         "Cruise speed band must be a number from 0 to 1000 km/h"},
        {"cruiseFloor", true,
         "Cruise floor must be a number from 0 to 1000 km/h"},
        {"pullSpeed", 1, "'pullSpeed' isn't a stretch rule"},
    };
    changes = 0;
    for (const auto& r : refused) {
        EXPECT_EQ(v.stretches(true, true, rule(r.key, r.value)), 0) << r.key;
        EXPECT_EQ(v.stretchesProblem(), QLatin1String(r.words)) << r.key;
    }
    EXPECT_EQ(changes, 11) << "a change for each new wording, none for the "
                              "two refusals that repeat the one before";
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
        QStringLiteral("Cruise floor must be a number from 0 to 621 mph"));
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
    EXPECT_EQ(v.stretchDefaults().size(), 10);
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

// ---- The values from the frames (ADR-0049's appendix of 2026-11-06) ----

namespace {

const QString kReplayFixtures = QStringLiteral(ECUIO_REPLAY_FIXTURES);

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

// A signal file of one DID on 0x7E8, its profile user.frames-view
const QByteArray kOneDid = R"({
  "havne_signals": 1,
  "name": "Frames view",
  "source": { "name": "Written for the test" },
  "ecus": [ { "id": "pcm", "rx": "7E8" } ],
  "signals": [
    { "key": "a", "name": "Input A", "ecu": "pcm",
      "read": { "service": "22", "number": "1111" },
      "field": { "byte": 0, "bits": 8 } }
  ]
})";

// Three of that DID's replies as a listener saw them, Single Frames
const QByteArray kOneDidFrames = "host_us,timestamp_us,id,flags,data\n"
                                 "100000,100000,0x7E8,0x0,04 62 11 11 07\n"
                                 "200000,200000,0x7E8,0x0,04 62 11 11 08\n"
                                 "300000,300000,0x7E8,0x0,04 62 11 11 09\n";

} // namespace

// A listener's wire frames through the profile its header names, as
// havne-cli view --frames reads them
TEST(RecordingViewTest, AListenersFramesDecodeThroughItsRecordedProfile)
{
    RecordingView v;
    v.openFrames(fixture("findings"), QString());
    EXPECT_EQ(v.state(), RecordingView::Opening);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.decodedWith(), QStringLiteral("obd2"));
    EXPECT_EQ(v.recordedProfile(), QStringLiteral("obd2"));
    EXPECT_FALSE(v.ownSignalFile());
    EXPECT_TRUE(v.check().value(QStringLiteral("ran")).toBool());
    const QVariantMap rpm = signalNamed(v, "RPM");
    EXPECT_EQ(rpm.value(QStringLiteral("samples")).toDouble(), 307.0);
    EXPECT_EQ(rpm.value(QStringLiteral("lowest")).toDouble(), 804.0);
    EXPECT_EQ(rpm.value(QStringLiteral("firstMs")).toDouble(), 100.0)
        << "the frame's time, not signals.csv's";
    EXPECT_TRUE(rpm.value(QStringLiteral("plotted")).toBool());

    v.open(fixture("findings"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_TRUE(v.decodedWith().isEmpty()) << "signals.csv again";
}

// The replay fixture holds frames.csv alone: open goes to the frames, and
// with no header to name a profile it says to pick one
TEST(RecordingViewTest, AFolderOfFramesAloneOpensThroughAProfile)
{
    const QString folder =
        kReplayFixtures + QStringLiteral("/2026-10-25-engine-start");
    RecordingView v;
    v.open(folder);
    EXPECT_EQ(v.state(), RecordingView::Failed) << "nothing names a profile";
    EXPECT_EQ(v.error(),
              QStringLiteral("it names no profile: pick one to decode its "
                             "frames with"));
    v.openFrames(folder, QStringLiteral("obd2"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.decodedWith(), QStringLiteral("obd2"));
    EXPECT_EQ(signalNamed(v, "RPM").value(QStringLiteral("highest")).toDouble(),
              1261.25);
}

// What can't decode fails at once, in words that never name the path
TEST(RecordingViewTest, AProfileThatCantDecodeFailsAtOnce)
{
    RecordingView v;
    const struct {
        const char* profile;
        const char* words;
    } cases[] = {
        {"raw", "the profile raw has no signals to decode with"},
        {"user.not-loaded", "the profile user.not-loaded isn't loaded"},
        {"signals.json",
         "its signals.json isn't the file it was recorded with"},
    };
    for (const auto& c : cases) {
        v.openFrames(fixture("findings"), QString::fromLatin1(c.profile));
        EXPECT_EQ(v.state(), RecordingView::Failed) << c.profile;
        EXPECT_EQ(v.error(), QString::fromLatin1(c.words));
        EXPECT_TRUE(v.decodedWith().isEmpty());
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 2 (ADR-0049)\n"
              "profile  raw\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    v.openFrames(dir.path(), QString());
    EXPECT_EQ(v.state(), RecordingView::Failed);
    EXPECT_EQ(v.error(),
              QStringLiteral("it was recorded under raw, which decodes "
                             "nothing: pick a profile to decode its frames "
                             "with"));
    EXPECT_FALSE(v.error().contains(dir.path()));
}

// The set is copied before the open's thread starts: a profile gone the
// moment after still decodes the open under way
TEST(RecordingViewTest, TheSetIsCopiedBeforeTheOpenRuns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(kOneDid.constData(),
                                     static_cast<size_t>(kOneDid.size()),
                                     &file),
              ECUIO_OK);
    const char* profile = ecuio_signal_file_profile(file);
    ASSERT_STREQ(profile, "user.frames-view");
    ASSERT_EQ(
        ecuio_profile_register(profile, ecuio_signal_file_set(file), nullptr),
        ECUIO_OK);
    RecordingView v;
    v.openFrames(dir.path(), QStringLiteral("user.frames-view"));
    ASSERT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    ecuio_signal_file_free(file);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantMap a = signalNamed(v, "Input A");
    EXPECT_EQ(a.value(QStringLiteral("samples")).toDouble(), 3.0);
    EXPECT_EQ(a.value(QStringLiteral("highest")).toDouble(), 9.0);
}

// The recording's own signals.json, while it hashes to the one its header
// names: offered, and what the default picks; changed, it isn't
TEST(RecordingViewTest, ItsOwnSignalFileDecodesItWhileItMatches)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray sha =
        QCryptographicHash::hash(kOneDid, QCryptographicHash::Sha256).toHex();
    writeFile(dir.filePath(QStringLiteral("signals.json")), kOneDid);
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 3 (ADR-0049)\n"
              "profile  user.frames-view\n"
              "signal file  signals.json," +
                  sha + "\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.ownSignalFile());
    EXPECT_EQ(v.decodedWith(), QStringLiteral("signals.json"));
    EXPECT_EQ(
        signalNamed(v, "Input A").value(QStringLiteral("samples")).toDouble(),
        3.0);

    writeFile(dir.filePath(QStringLiteral("signals.json")),
              kOneDid + QByteArray("\n"));
    v.openFrames(dir.path(), QString());
    EXPECT_FALSE(v.ownSignalFile());
    EXPECT_EQ(v.state(), RecordingView::Failed)
        << "the recorded profile, which isn't loaded";
    EXPECT_EQ(v.error(),
              QStringLiteral("the profile user.frames-view isn't loaded"));
}

// ---- The health report, the viewer's Save report (ADR-0054's appendix
// "the health report as built") ----------------------------------------

// The whole recording's report, on a thread of its own: one at a time,
// its result posted back, the folder's name (the vehicle's label) in
// neither the words nor the line
TEST(RecordingViewTest, HealthReportIsTheWholeRecordingsOnItsOwnThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    const QUrl url =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/report.json"));
    EXPECT_FALSE(v.saveHealthReport(url, 1)) << "no recording open";
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    QList<QVariantMap> saved;
    int reportingChanges = 0;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    QObject::connect(&v, &RecordingView::reportingChanged,
                     [&] { ++reportingChanges; });
    ASSERT_TRUE(v.saveHealthReport(url, 1));
    EXPECT_TRUE(v.reporting());
    EXPECT_FALSE(v.saveHealthReport(url, 1)) << "one at a time";
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(v.reporting());
    EXPECT_EQ(reportingChanges, 2);
    ASSERT_EQ(saved.size(), 1);
    const QVariantMap r = saved[0];
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 1);
    EXPECT_EQ(r.value(QStringLiteral("codes")).toInt(), 1);
    EXPECT_EQ(r.value(QStringLiteral("values")).toInt(), 1) << "RPM's";
    EXPECT_EQ(r.value(QStringLiteral("line")).toString().toStdString(),
              "Health report saved: 1 ECU, 1 trouble code, 1 key value (0 "
              "identification items left out)");

    QFile f(url.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    EXPECT_FALSE(bytes.contains("Daily"));
    const QJsonObject doc = QJsonDocument::fromJson(bytes).object();
    const QJsonObject source = doc.value(QStringLiteral("source")).toObject();
    EXPECT_EQ(source.value(QStringLiteral("kind")).toString(),
              QStringLiteral("recording"));
    EXPECT_EQ(source.value(QStringLiteral("from_s")).toDouble(-1), 0.0)
        << "the whole recording";

    // A folder isn't a file it can write: core's report made, the copy
    // refused, in words that name neither
    saved.clear();
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(root.path()), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved[0].value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    for (const QVariantMap& m : saved) {
        EXPECT_FALSE(m.value(QStringLiteral("line"))
                         .toString()
                         .contains(QStringLiteral("Daily")));
    }
}

// The baseline given goes to core on the report's thread: another
// recording read by its start, the open one said to be the report's own,
// a folder that's gone said so with the report still saved, none ("")
// null; never a folder's name (the vehicle's label) in the file
TEST(RecordingViewTest, HealthReportNamesTheBaselineItIsGiven)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const struct {
        QString folder;
        bool read;
        bool own;
    } cases[] = {
        {fixture("findings"), true, false},
        {folder, true, true},
        {root.path() + QStringLiteral("/vanished-folder"), false, false},
        {QString(), false, false},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder.toStdString());
        const QUrl url = QUrl::fromLocalFile(
            root.path() + QStringLiteral("/report-%1.json").arg(++n));
        saved.clear();
        ASSERT_TRUE(v.saveHealthReport(url, 1, c.folder));
        ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
        ASSERT_TRUE(saved[0].value(QStringLiteral("ok")).toBool())
            << saved[0].value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(saved[0].value(QStringLiteral("baselineRead")).toBool(),
                  c.read);
        QFile f(url.toLocalFile());
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        EXPECT_FALSE(bytes.contains("Daily"));
        EXPECT_FALSE(bytes.contains("vanished"));
        const QJsonValue b = QJsonDocument::fromJson(bytes).object().value(
            QStringLiteral("baseline"));
        if (c.folder.isEmpty()) {
            EXPECT_TRUE(b.isNull()) << bytes.toStdString();
            continue;
        }
        EXPECT_EQ(b.toObject().value(QStringLiteral("read")).toBool(), c.read);
        EXPECT_EQ(b.toObject().value(QStringLiteral("is_source")).toBool(),
                  c.own);
    }
}

// A report under way when the viewer closes or opens another recording
// ends unannounced: its thread runs out (only the viewer's destruction
// stops it), and the result is dropped
TEST(RecordingViewTest, AReportEndingAfterACloseIsDropped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    int saved = 0;
    QObject::connect(&v, &RecordingView::reportSaved, [&] { ++saved; });
    const QUrl url =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/r.txt"));
    ASSERT_TRUE(v.saveHealthReport(url, 0));
    v.close();
    ASSERT_TRUE(waitFor([&] { return !v.reporting(); }));
    EXPECT_EQ(saved, 0);

    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_TRUE(v.saveHealthReport(url, 0));
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return !v.reporting() && settled(v); }));
    EXPECT_EQ(saved, 0) << "another open drops it too";
}

// A report under way when the viewer goes (the app quitting) stops at
// core's next ask: the destructor's wait ends with no file written, where
// the same report run to its end writes one
TEST(RecordingViewTest, AReportStopsWhenTheViewerGoes)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::Append));
        QByteArray rows;
        for (int i = 0; i < 200000; ++i) {
            const QByteArray us = QByteArray::number(400000 + i * 100);
            rows += us + ',' + us + ",0x7E8,0x0,04 41 0C 0C 90 00 00 00\n";
        }
        ASSERT_EQ(frames.write(rows), rows.size());
    }
    auto v = std::make_unique<RecordingView>();
    v->open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(*v); }, 30000));
    ASSERT_EQ(v->state(), RecordingView::Open) << v->error().toStdString();
    QList<QVariantMap> saved;
    QObject::connect(v.get(), &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const QUrl whole =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/whole.txt"));
    ASSERT_TRUE(v->saveHealthReport(whole, 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }, 30000));
    ASSERT_TRUE(saved[0].value(QStringLiteral("ok")).toBool())
        << saved[0].value(QStringLiteral("words")).toString().toStdString();
    ASSERT_TRUE(QFile::exists(whole.toLocalFile()));

    const QUrl stopped =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/stopped.txt"));
    ASSERT_TRUE(v->saveHealthReport(stopped, 0));
    v.reset();
    EXPECT_FALSE(QFile::exists(stopped.toLocalFile()));
}

// A report the picker's document (a provider's, which the picker has made
// already) doesn't end up holding removes the document, as a failed export
// does: one core refuses, and one stopped under way
TEST(RecordingViewTest, AFailedReportLeavesAPickedDocumentGone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly | QIODevice::Truncate));
        frames.write("not a frames file\n");
    }
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFile::exists(picked)) << "the empty document goes";

    // A local file's picker is the file dialog's, which asked about
    // replacing: nothing is removed there
    v.setThroughProviderForTesting(false);
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    saved.clear();
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(QFile::exists(picked));
}

TEST(RecordingViewTest, AStoppedReportLeavesAPickedDocumentGone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::Append));
        QByteArray rows;
        for (int i = 0; i < 200000; ++i) {
            const QByteArray us = QByteArray::number(400000 + i * 100);
            rows += us + ',' + us + ",0x7E8,0x0,04 41 0C 0C 90 00 00 00\n";
        }
        ASSERT_EQ(frames.write(rows), rows.size());
    }
    auto v = std::make_unique<RecordingView>();
    v->setThroughProviderForTesting(true);
    v->open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(*v); }, 30000));
    ASSERT_EQ(v->state(), RecordingView::Open) << v->error().toStdString();
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    ASSERT_TRUE(v->saveHealthReport(QUrl::fromLocalFile(picked), 0));
    v.reset();
    EXPECT_FALSE(QFile::exists(picked)) << "the empty document goes";
}

// A report thread the system can't start says no, with nothing reporting,
// and the picker's document goes
TEST(RecordingViewTest, AReportWhoseThreadCantStartSaysSo)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    int changes = 0;
    QObject::connect(&v, &RecordingView::reportingChanged, [&] { ++changes; });
    v.setFailThreadForTesting(true);
    EXPECT_FALSE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    EXPECT_FALSE(v.reporting());
    EXPECT_EQ(changes, 0);
    EXPECT_FALSE(QFile::exists(picked));

    v.setFailThreadForTesting(false);
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_TRUE(saved[0].value(QStringLiteral("ok")).toBool());
}

// A CSV import holds values alone, and Save report stays offered on it:
// core's report takes a folder with no frames.csv, which has no ECU and
// says so, so the save ends and succeeds rather than failing on frames
TEST(RecordingViewTest, AnImportedCsvsReportEndsWithNoEcu)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = importedCsv(dir);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    ASSERT_FALSE(v.hasFrames());

    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const QUrl url =
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/report.json"));
    ASSERT_TRUE(v.saveHealthReport(url, 1));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(v.reporting());
    const QVariantMap r = saved[0];
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 0);
    EXPECT_EQ(r.value(QStringLiteral("line")).toString().toStdString(),
              "Health report saved: 0 ECUs, 0 trouble codes, 0 key values (0 "
              "identification items left out)");

    // The report says why it has no ECU, in the viewer's words, and is
    // the whole import's: its raw profile ranks nothing, so no key values
    QFile f(url.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_TRUE(doc.value(QStringLiteral("ecus")).toArray().isEmpty());
    EXPECT_EQ(doc.value(QStringLiteral("ecus_note")).toString(),
              QStringLiteral("the recording has no frames"));
    EXPECT_TRUE(doc.value(QStringLiteral("key_values")).toArray().isEmpty());
    const QJsonObject source = doc.value(QStringLiteral("source")).toObject();
    EXPECT_EQ(source.value(QStringLiteral("kind")).toString(),
              QStringLiteral("recording"));
    EXPECT_FALSE(source.value(QStringLiteral("frames")).toBool(true));
    EXPECT_EQ(source.value(QStringLiteral("from_s")).toDouble(-1), 0.0);
    EXPECT_EQ(source.value(QStringLiteral("to_s")).toDouble(-1), 1.75);
}
