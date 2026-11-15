// File/layer: tests/test_bridge_health.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.Health*, HeldValues*, WindowTheme* (test_bridge)
//
// Cases for the health report and the window theme.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <vector>

#include "healthreport.h"
#include "fake_theme.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "windowtheme.h"
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

// ---- The health report, Diagnostics' Save report (ADR-0054's appendix
// "the health report as built") ----------------------------------------

/* The temporary folder core writes the report in: TMP, TEMP and TMPDIR
 * pointed at a folder of the test's own while it lives, so a case can
 * see the report's own folder go */
struct TempDirScope {
    EnvRestore tmp{"TMP"};
    EnvRestore temp{"TEMP"};
    EnvRestore tmpdir{"TMPDIR"};
    QTemporaryDir dir;
    TempDirScope()
    {
        const QByteArray path = QDir::toNativeSeparators(dir.path()).toUtf8();
        qputenv("TMP", path);
        qputenv("TEMP", path);
        qputenv("TMPDIR", path);
    }
    QStringList left() const
    {
        return QDir(dir.path())
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    }
};

TEST(BridgeTest, HealthFormatIsTheFiltersOrTheNames)
{
    const QUrl txt = QUrl::fromLocalFile(QStringLiteral("/x/report.txt"));
    const QUrl json = QUrl::fromLocalFile(QStringLiteral("/x/report.JSON"));
    const QUrl content(QStringLiteral("content://provider/document/1234"));
    EXPECT_EQ(healthFormatFor(txt, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(txt, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(json, 0), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, 0), ECUIO_HEALTH_TEXT);
    EXPECT_EQ(healthFormatFor(content, 1), ECUIO_HEALTH_JSON);
    EXPECT_EQ(healthFormatFor(content, -1), ECUIO_HEALTH_TEXT);
}

/* Each count's noun as the count wants it: one, none and many */
TEST(BridgeTest, HealthReportWordsEachCount)
{
    ecuio_health_report_t one{};
    one.ecus = 1;
    one.codes = 1;
    one.values = 1;
    one.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 1;
    EXPECT_EQ(healthSummary(one).toStdString(),
              "1 ECU, 1 trouble code, 1 key value");
    EXPECT_EQ(healthSavedLine(one).toStdString(),
              "Health report saved: 1 ECU, 1 trouble code, 1 key value (1 "
              "identification item left out)");
    ecuio_health_report_t many{};
    many.ecus = 2;
    many.codes = 0;
    many.values = 14;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION] = 13;
    many.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES] = 1;
    EXPECT_EQ(healthSummary(many).toStdString(),
              "2 ECUs, 0 trouble codes, 14 key values");
    EXPECT_EQ(healthSavedLine(many).toStdString(),
              "Health report saved: 2 ECUs, 0 trouble codes, 14 key values (13 "
              "identification items left out)");
}

/* A live session's report names the picked vehicle's baseline it's
 * given, by its start and never its folder: read, gone (the report still
 * saved, baselineRead false) and none ("") */
TEST(BridgeTest, HealthReportNamesTheBaselineItIsGiven)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));
    const struct {
        QString folder;
        bool read;
    } cases[] = {
        {QStringLiteral(ECUIO_RECORDING_FIXTURES "/findings"), true},
        {out.path() + QStringLiteral("/vanished-folder"), false},
        {QString(), false},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder.toStdString());
        const QString target =
            out.path() + QStringLiteral("/report-%1.json").arg(++n);
        const QVariantMap r =
            bridge.saveHealthReport(QUrl::fromLocalFile(target), 1, c.folder);
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("baselineRead")).toBool(), c.read);
        const QByteArray bytes = readFile(target);
        EXPECT_FALSE(bytes.contains("vanished"));
        EXPECT_FALSE(bytes.contains(ECUIO_RECORDING_FIXTURES));
        const QJsonValue b = QJsonDocument::fromJson(bytes).object().value(
            QStringLiteral("baseline"));
        if (c.folder.isEmpty()) {
            EXPECT_TRUE(b.isNull()) << bytes.toStdString();
            continue;
        }
        EXPECT_EQ(b.toObject().value(QStringLiteral("read")).toBool(), c.read);
        if (c.read) {
            EXPECT_EQ(b.toObject().value(QStringLiteral("started")).toString(),
                      QStringLiteral("2026-11-03T16:00:00Z"));
            EXPECT_FALSE(
                b.toObject().value(QStringLiteral("is_source")).toBool());
        }
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* The mock's report written where the user chose: text, then JSON by
 * the filter or the name, over a file that exists; its label never in
 * the file or the lines; core's temporary folder gone each time */
TEST(BridgeTest, HealthReportOfTheMockIsWrittenWhereChosen)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList lines;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&](const QString& line) { lines << line; });

    TempDirScope temp;
    ASSERT_TRUE(temp.dir.isValid());
    const QString target = out.path() + QStringLiteral("/report.txt");
    const QVariantMap none =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    EXPECT_FALSE(none.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(none.value(QStringLiteral("words")).toString().toStdString(),
              "No session");
    EXPECT_FALSE(QFile::exists(target));

    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://Daily driver")).isEmpty());
    ASSERT_TRUE(
        spinUntil([&]() { return !bridge.diagnostics().isEmpty(); }, 2000));

    {
        QFile old(target);
        ASSERT_TRUE(old.open(QIODevice::WriteOnly));
        old.write("the old report");
    }
    lines.clear();
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 0);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 1);
    const int codes = r.value(QStringLiteral("codes")).toInt();
    EXPECT_GT(codes, 0);
    EXPECT_FALSE(r.value(QStringLiteral("vinDecoded")).toBool());
    const QByteArray text = readFile(target);
    EXPECT_FALSE(text.contains("the old report"));
    EXPECT_TRUE(text.contains("mock://<label>")) << text.toStdString();
    EXPECT_FALSE(text.contains("Daily"));
    EXPECT_FALSE(text.startsWith("{"));
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0],
              QStringLiteral("Health report saved: 1 ECU, %1 trouble codes, "
                             "0 key values (0 identification items left out)")
                  .arg(codes));
    EXPECT_EQ(
        r.value(QStringLiteral("summary")).toString(),
        QStringLiteral("1 ECU, %1 trouble codes, 0 key values").arg(codes));

    // JSON by the dialog's filter, a .txt name notwithstanding, and by the
    // name alone
    const QVariantMap byFilter =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(byFilter.value(QStringLiteral("ok")).toBool());
    const QJsonDocument doc = QJsonDocument::fromJson(readFile(target));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value(QStringLiteral("havne_health")).toInt(), 1);
    EXPECT_EQ(doc.object().value(QStringLiteral("ecus")).toArray().size(), 1);
    const QString named = out.path() + QStringLiteral("/report.json");
    ASSERT_TRUE(bridge.saveHealthReport(QUrl::fromLocalFile(named), 0)
                    .value(QStringLiteral("ok"))
                    .toBool());
    EXPECT_TRUE(QJsonDocument::fromJson(readFile(named)).isObject());
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();

    // A folder isn't a file it can write: nothing there, the words plain
    lines.clear();
    const QVariantMap folder =
        bridge.saveHealthReport(QUrl::fromLocalFile(out.path()), 0);
    EXPECT_FALSE(folder.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(folder.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].toStdString(),
              "Health report not saved: Couldn't write the file");
    EXPECT_TRUE(temp.left().isEmpty()) << temp.left().join(' ').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    for (const QString& line : lines) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

/* Live's rows hold each value as core decoded it, in the def's unit:
 * the display's conversion never reaches the report, which converts for
 * itself; each value aged from the window that brought it */
TEST(BridgeTest, HeldValuesAreCoresInTheDefsUnit)
{
    SignalListModel model;
    ecuio_signal_def_t defs[2];
    ecuio_signal_set_t set;
    buildTestSignalSet(defs, set);
    std::memset(defs[0].unit, 0, sizeof defs[0].unit);
    std::memcpy(defs[0].unit, "km/h", 4);
    model.setSignalSet(&set);
    EXPECT_TRUE(model.heldValues().empty()) << "no value yet";
    model.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    model.updateValues({{uint16_t(1001), 100.0}});
    spinLoop(300);
    const std::vector<ecuio_health_value_t> held = model.heldValues();
    ASSERT_EQ(held.size(), 1u) << "only the row with a value";
    EXPECT_EQ(held[0].def_id, 1001u);
    EXPECT_EQ(held[0].value, 100.0) << "km/h as decoded, not mph";
    EXPECT_GE(held[0].age_us, 250000u);
    EXPECT_LT(held[0].age_us, 10000000u);
    EXPECT_EQ(held[0].flags, 0u);
    EXPECT_EQ(held[0].reserved, 0u);
}

/* A replay under obd2: Live's ranked values are the report's key values,
 * shown in the app's units (core converts them for the file) */
TEST(BridgeTest, HealthReportCarriesLivesValuesInTheAppsUnits)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    bridge.setUnitSystem(ECUIO_UNITS_IMPERIAL);
    const QString uri =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=10")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_EQ(bridge.connectSession(uri), QString());
    ASSERT_TRUE(spinUntil(
        [&]() { return bridge.signalModel()->heldValues().size() >= 5; },
        10000));
    const QString target = out.path() + QStringLiteral("/report.json");
    const QVariantMap r =
        bridge.saveHealthReport(QUrl::fromLocalFile(target), 1);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(r.value(QStringLiteral("values")).toInt(), 0);
    const QJsonObject doc = QJsonDocument::fromJson(readFile(target)).object();
    const QJsonArray values = doc.value(QStringLiteral("key_values")).toArray();
    ASSERT_FALSE(values.isEmpty());
    QStringList units;
    for (const QJsonValue& v : values) {
        units << v.toObject().value(QStringLiteral("unit")).toString();
    }
    EXPECT_TRUE(units.contains(QStringLiteral("mph")) ||
                units.contains(QString::fromUtf8("\xC2\xB0"
                                                 "F")))
        << units.join(' ').toStdString();
    EXPECT_FALSE(units.contains(QStringLiteral("km/h")));
}

namespace {

QStringList g_themeWarnings;

void collectWarnings(QtMsgType type, const QMessageLogContext&,
                     const QString& message)
{
    if (type == QtWarningMsg) {
        g_themeWarnings << message;
    }
}

} // namespace

// Each of bg, surface and text, changed, applies the theme once, with no
// window to color
TEST(BridgeTest, WindowThemeAppliesOncePerChangeOfEachColor)
{
    FakeTheme theme;
    WindowTheme follower(&theme, nullptr);
    int applied = 0;
    QObject::connect(&follower, &WindowTheme::applied, &follower,
                     [&] { ++applied; });
    theme.setBg(Qt::red);
    EXPECT_EQ(applied, 1);
    theme.setSurface(Qt::green);
    EXPECT_EQ(applied, 2);
    theme.setText(Qt::blue);
    EXPECT_EQ(applied, 3);
}

// The scheme apply() asks for follows the look's lightness each way, and
// the first apply (at construction) asks too. The offscreen platform
// reports Unknown whatever is asked, so the QML case that reads the
// platform's scheme skips there: this is what pins the choice
TEST(BridgeTest, WindowThemeAsksTheSchemeOfTheLookEachWay)
{
    FakeTheme theme; // black: a dark look
    WindowTheme follower(&theme, nullptr);
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Dark);
    theme.setBg(QColor(QLatin1String("#f3f3f1")));
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Light);
    theme.setBg(QColor(QLatin1String("#0d0a07"))); // Night amber
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Dark);
    theme.setBg(QColor(QLatin1String("#ffffff")));
    EXPECT_EQ(follower.askedScheme(), Qt::ColorScheme::Light);
}

// A light look's page gets dark bar icons and the Light scheme; Dark and
// the night palettes keep light ones. The line is HSL lightness 0.5
TEST(BridgeTest, WindowThemeLightBarsFollowTheBackground)
{
    for (const char* dark : {"#141416", "#0d0a07", "#080404", "#7f7f7f"}) {
        EXPECT_FALSE(WindowTheme::lightBars(QColor(QLatin1String(dark))))
            << dark;
    }
    for (const char* light : {"#f3f3f1", "#ffffff", "#808080"}) {
        EXPECT_TRUE(WindowTheme::lightBars(QColor(QLatin1String(light))))
            << light;
    }
}

// A property the theme lacks, or does not notify, is a warning: a rename
// would otherwise leave the window not following the theme
TEST(BridgeTest, WindowThemeWarnsOfAColorItCannotFollow)
{
    g_themeWarnings.clear();
    const QtMessageHandler previous = qInstallMessageHandler(collectWarnings);
    {
        FakeTheme whole;
        WindowTheme follower(&whole, nullptr);
    }
    EXPECT_TRUE(g_themeWarnings.isEmpty()) << "a whole theme says nothing";
    {
        FakeThemeWithoutSurface partial;
        WindowTheme follower(&partial, nullptr);
        int applied = 0;
        QObject::connect(&follower, &WindowTheme::applied, &follower,
                         [&] { ++applied; });
        partial.setBg(Qt::red);
        EXPECT_EQ(applied, 1) << "the colors it has still follow";
    }
    {
        FakeThemeWithConstantSurface constant;
        WindowTheme follower(&constant, nullptr);
    }
    qInstallMessageHandler(previous);
    ASSERT_EQ(g_themeWarnings.size(), 2)
        << g_themeWarnings.join(u'\n').toStdString();
    EXPECT_TRUE(g_themeWarnings[0].contains(QStringLiteral("'surface'")))
        << g_themeWarnings[0].toStdString();
    EXPECT_TRUE(g_themeWarnings[1].contains(
        QStringLiteral("no notifying property 'surface'")))
        << "a property that never notifies: "
        << g_themeWarnings[1].toStdString();
}
