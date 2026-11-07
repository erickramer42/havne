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
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <functional>
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

// A report under way when the viewer closes or opens another recording
// ends unannounced: its thread runs out (core's export can't stop
// midway), and the result is dropped
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
