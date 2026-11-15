// File/layer: tests/test_bridge_obdb.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: ObdbImportTest, BridgeTest.Obdb* (test_bridge)
//
// Cases for an OBDb set imported in the app.

#include <gtest/gtest.h>

#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>

#include "obdbimport.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

// ---- An OBDb set imported in the app (ADR-0051's appendix of 2026-11-06)
namespace {

QByteArray obdbFixture()
{
    return readFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

QUrl obdbUrl()
{
    return QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

// Whether the registered profile reads this DID
bool profileReadsDid(const char* profile, uint32_t did)
{
    const ecuio_signal_set_t* set = nullptr;
    if (ecuio_profile_lookup(profile, &set) != ECUIO_OK || set == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].mux_key == (0x620000u | did)) {
            return true;
        }
    }
    return false;
}

QStringList fieldsOf(const QVariantList& notes, const QString& key)
{
    QStringList out;
    for (const QVariant& v : notes) {
        out << v.toMap().value(key).toString();
    }
    return out;
}

} // namespace

/* The model year picks the commands OBDb gives for some years only, and
 * the notes come sorted: converted counted, each field that differs,
 * each signal left out, the set's own keys */
TEST(ObdbImportTest, TheYearPicksTheCommands)
{
    const QByteArray set = obdbFixture();
    ASSERT_FALSE(set.isEmpty());

    const ObdbImport::Preview none = ObdbImport::read(set, 0, QString());
    ASSERT_TRUE(none.ok) << none.words.toStdString();
    EXPECT_EQ(none.converted, 6);
    EXPECT_EQ(none.changed.size(), 5);
    EXPECT_EQ(none.skipped.size(), 6);
    EXPECT_EQ(fieldsOf(none.file, QStringLiteral("field")),
              (QStringList{QStringLiteral("signalGroups"),
                           QStringLiteral("freq")}));
    EXPECT_FALSE(none.json.contains("D200"));
    EXPECT_FALSE(none.json.contains("D201"));
    const QVariantMap first = none.skipped.value(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("signal")).toString().toStdString(),
              "EXAMPLE_BOOST_TARGET");
    EXPECT_EQ(first.value(QStringLiteral("line")).toInt(), 46);
    EXPECT_EQ(first.value(QStringLiteral("text")).toString().toStdString(),
              "its command is for some model years only: give the "
              "vehicle's model year");
    const QVariantMap changed = none.changed.value(0).toMap();
    EXPECT_EQ(changed.value(QStringLiteral("key")).toString().toStdString(),
              "example_speed");
    EXPECT_EQ(changed.value(QStringLiteral("signal")).toString().toStdString(),
              "EXAMPLE_SPEED");
    EXPECT_EQ(changed.value(QStringLiteral("field")).toString().toStdString(),
              "max");
    EXPECT_EQ(changed.value(QStringLiteral("line")).toInt(), 10);

    const ObdbImport::Preview y2019 = ObdbImport::read(set, 2019, QString());
    ASSERT_TRUE(y2019.ok);
    EXPECT_EQ(y2019.converted, 7);
    EXPECT_EQ(y2019.skipped.size(), 5);
    EXPECT_TRUE(y2019.json.contains("\"D200\""));
    EXPECT_FALSE(y2019.json.contains("\"D201\""));

    const ObdbImport::Preview y2021 =
        ObdbImport::read(set, 2021, QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(y2021.ok);
    EXPECT_EQ(y2021.converted, 7);
    EXPECT_TRUE(y2021.json.contains("\"D201\""));
    EXPECT_FALSE(y2021.json.contains("\"D200\""));
    EXPECT_TRUE(y2021.json.contains("\"name\": \"Daily driver OBDb\""));
    EXPECT_TRUE(y2021.json.contains("\"extends\": \"obd2\""))
        << "the vehicle keeps Service 01's signals";

    const QVariantMap r = ObdbImport::report(
        y2021, QStringLiteral("Daily driver OBDb"), 2021);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Daily driver OBDb");
    EXPECT_EQ(r.value(QStringLiteral("year")).toInt(), 2021);
    EXPECT_EQ(r.value(QStringLiteral("converted")).toInt(), 7);
    EXPECT_EQ(r.value(QStringLiteral("changed")).toList().size(), 6);
    EXPECT_EQ(r.value(QStringLiteral("skipped")).toList().size(), 5);
    EXPECT_EQ(r.value(QStringLiteral("file")).toList().size(), 2);
}

/* Only the importer's own source block marks a file as a conversion */
TEST(ObdbImportTest, AConversionIsKnownByItsSourceBlock)
{
    const ObdbImport::Preview p = ObdbImport::read(obdbFixture(), 0, QString());
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(ObdbImport::isConversion(p.json));
    EXPECT_FALSE(ObdbImport::isConversion(signalFile(QStringLiteral("X"))));
    QByteArray byHand = p.json;
    byHand.replace("Converted to Havne's signal-file format", "By hand");
    EXPECT_FALSE(ObdbImport::isConversion(byHand));
    QByteArray otherSource = p.json;
    otherSource.replace("OBDb editors and contributors", "Someone");
    EXPECT_FALSE(ObdbImport::isConversion(otherSource));
    EXPECT_FALSE(ObdbImport::isConversion(QByteArray()));
}

/* What isn't an OBDb set is refused with core's words; more than core
 * reads is refused before core sees it */
TEST(ObdbImportTest, NotASetIsRefused)
{
    ObdbImport::Preview p = ObdbImport::read("{\"ecu\": 1}", 0, QString());
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(
        p.words.startsWith(QStringLiteral("Not an OBDb set Havne can read: ")))
        << p.words.toStdString();
    EXPECT_TRUE(p.json.isEmpty());
    p = ObdbImport::read(QByteArray(static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES) + 1, ' '), 0,
                         QString());
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.words.toStdString(),
              "The file is over 1 MiB, an OBDb set's most");
    const QVariantMap r = ObdbImport::report(p, QStringLiteral("X"), 0);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(r.contains(QStringLiteral("converted")));
}

/* ADR-0043's year: a typed VIN's, spaces dropped and upper-cased; none
 * for a position 10 that isn't a year code or a text that isn't a VIN */
TEST(ObdbImportTest, AVinGivesItsModelYear)
{
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE10M0000001"), 2027),
              2021);
    EXPECT_EQ(
        ObdbImport::modelYear(QStringLiteral("1fa fake10m 0000001"), 2027),
        2021);
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE13Y0000001"), 2027),
              2000)
        << "2030 is still ahead";
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("ZZZFAKE10U0000001"), 2027),
              0);
    EXPECT_EQ(ObdbImport::modelYear(QStringLiteral("1FAFAKE10M"), 2027), 0);
    EXPECT_EQ(ObdbImport::modelYear(QString(), 2027), 0);
}

/* The bridge's year: the typed VIN's, or with "" the session's */
TEST(BridgeTest, ObdbYearComesFromTheTypedVinOrTheSessions)
{
    SessionBridge bridge;
    EXPECT_EQ(bridge.vinModelYear(QString()), 0);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("1FAFAKE10M0000001")), 2021);
    bridge.identifyForTesting(QStringLiteral("1FAFAKE13Y0000001"));
    EXPECT_EQ(bridge.vinModelYear(QString()), 2000);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("  ")), 2000);
    EXPECT_EQ(bridge.vinModelYear(QStringLiteral("1FAFAKE10M0000001")), 2021)
        << "a typed VIN is the one asked about";
}

/* Preview converts and holds the file; only keep puts it in the store,
 * once, as any signal file's copy */
TEST(BridgeTest, ObdbPreviewKeepsNothingUntilKept)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });

    QVariantMap r = bridge.previewObdb(obdbUrl(), 2021,
                                       QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Daily driver OBDb");
    EXPECT_EQ(r.value(QStringLiteral("year")).toInt(), 2021);
    EXPECT_EQ(r.value(QStringLiteral("converted")).toInt(), 7);
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty()) << "nothing kept yet";
    EXPECT_TRUE(log.contains(QStringLiteral(
        "OBDb set read for Daily driver OBDb: 7 converted, 6 fields differ, "
        "5 left out (model year 2021)")))
        << log.join(QLatin1Char('\n')).toStdString();

    r = bridge.keepObdb(QString());
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.daily-driver-obdb");
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(),
              "obd2");
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 7);
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha + QStringLiteral(".json")});
    EXPECT_TRUE(bridge.isObdbImport(sha));
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file from OBDb: Daily driver OBDb "
                       "(user.daily-driver-obdb, %1 signals)")
            .arg(r.value(QStringLiteral("count")).toInt())))
        << log.join(QLatin1Char('\n')).toStdString();

    // Kept once: a second keep, or one after a drop, has nothing to keep
    r = bridge.keepObdb(QString());
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "No OBDb set is waiting: import it again");
    ASSERT_TRUE(
        bridge.previewObdb(obdbUrl(), 2019, QStringLiteral("Other OBDb"))
            .value(QStringLiteral("ok"))
            .toBool());
    bridge.dropObdb();
    EXPECT_FALSE(
        bridge.keepObdb(QString()).value(QStringLiteral("ok")).toBool());
    // A refused preview leaves none pending either
    ASSERT_TRUE(
        bridge.previewObdb(obdbUrl(), 2019, QStringLiteral("Other OBDb"))
            .value(QStringLiteral("ok"))
            .toBool());
    QTemporaryDir from;
    EXPECT_FALSE(bridge
                     .previewObdb(writeScratch(from.path(), "x.json", "[]"), 0,
                                  QStringLiteral("Other OBDb"))
                     .value(QStringLiteral("ok"))
                     .toBool());
    EXPECT_FALSE(
        bridge.keepObdb(QString()).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
    EXPECT_FALSE(bridge.isObdbImport(QString(64, QLatin1Char('a'))));
}

/* A refusal is core's words or the bridge's, never the path */
TEST(BridgeTest, ObdbRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QString folder = from.path() + QStringLiteral("/jdoe-private");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QVariantMap r = bridge.previewObdb(
        writeScratch(folder, "car.json", "{\"commands\": 3}"), 0,
        QStringLiteral("A OBDb"));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.startsWith(QStringLiteral("Not an OBDb set")))
        << words.toStdString();
    r = bridge.previewObdb(
        QUrl::fromLocalFile(folder + QStringLiteral("/gone.json")), 0,
        QStringLiteral("A OBDb"));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't read the file");
    ASSERT_EQ(log.size(), 2);
    for (const QString& line : log + QStringList{words}) {
        EXPECT_FALSE(line.contains(QStringLiteral("jdoe")))
            << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("car.json")))
            << line.toStdString();
    }
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty());
}

/* The set read again for another year replaces the earlier import: the
 * profile's defs are the new year's */
TEST(BridgeTest, ObdbReimportReplacesTheEarlierImport)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QString name = QStringLiteral("Daily driver OBDb");
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString earlier =
        bridge.keepObdb(QString()).value(QStringLiteral("sha256")).toString();
    ASSERT_FALSE(earlier.isEmpty());
    ASSERT_EQ(bridge.useSignalFiles({earlier}).size(), 1);
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    EXPECT_FALSE(profileReadsDid("user.daily-driver-obdb", 0xD201));

    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2021, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantMap r = bridge.keepObdb(earlier);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString later = r.value(QStringLiteral("sha256")).toString();
    EXPECT_NE(later, earlier);
    // The book swaps them; the store registers the one it's given
    const QVariantList used = bridge.useSignalFiles({later});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD201));
    EXPECT_FALSE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    EXPECT_EQ(bridge.deleteUnusedSignalFiles({later}), 1);
    bridge.useSignalFiles({});
}

/* While a session decodes with the earlier import, keep refuses to
 * replace it and changes nothing; the import waits for the disconnect */
TEST(BridgeTest, ObdbReplaceWhileItsSessionReadsIsRefused)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const QString name = QStringLiteral("Daily driver OBDb");
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString earlier =
        bridge.keepObdb(QString()).value(QStringLiteral("sha256")).toString();
    ASSERT_EQ(bridge.useSignalFiles({earlier}).size(), 1);

    // A session on another profile doesn't hold it
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2019, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    QVariantMap r = bridge.keepObdb(earlier);
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), earlier)
        << "the same year's bytes again";
    ASSERT_TRUE(disconnectAndWait(bridge));

    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=user.daily-driver-obdb"))
                    .isEmpty());
    ASSERT_TRUE(bridge.previewObdb(obdbUrl(), 2021, name)
                    .value(QStringLiteral("ok"))
                    .toBool());
    r = bridge.keepObdb(earlier);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        r.value(QStringLiteral("words")).toString().toStdString(),
        "The session reads the earlier import: disconnect, then keep it");
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{earlier + QStringLiteral(".json")})
        << "nothing changed";
    EXPECT_TRUE(profileReadsDid("user.daily-driver-obdb", 0xD200));
    ASSERT_TRUE(disconnectAndWait(bridge));

    r = bridge.keepObdb(earlier);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_NE(r.value(QStringLiteral("sha256")).toString(), earlier);
    EXPECT_EQ(storeFiles(dir.store()).size(), 2);
    bridge.useSignalFiles({});
}
