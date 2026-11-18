// File/layer: tests/test_bridge_signalfiles.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest.SignalFile*, Plugin*, refs and ranges, ProfileSignals*, layout files, picks (test_bridge)
//
// Cases for signal files, plugins, refs and ranges, layout files and the pickers.

#include <cmath>
#include <gtest/gtest.h>
#include <functional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <vector>

#include "healthreport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "urlwrite.h"
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fake_control.h"   // the fake J2534 DLL's control page
#endif
#include "bridge_support.h"

// ---------------------------------------------------------------------------
// The vehicles' signal files (ADR-0051, app/signalstore.h): each case's
// store in a HAVNE_SETTINGS_DIR of its own.
// ---------------------------------------------------------------------------

/* A file that loads is kept as <sha256>.json, its bytes as given; the
 * same bytes from elsewhere reuse that copy */
TEST(BridgeTest, SignalFileImportKeepsACopyByItsHash)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);

    QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
    EXPECT_EQ(r.value(QStringLiteral("name")).toString().toStdString(),
              "Extras");
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.extras");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 1);
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha + QStringLiteral(".json")});
    QFile copy(dir.store() + QLatin1Char('/') + sha + QStringLiteral(".json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file: Extras (user.extras, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(registered("user.extras")) << "import registers nothing";

    // The same bytes under another name, from a fresh bridge too
    SessionBridge again;
    r = again.importSignalFile(writeScratch(from.path(), "b.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
}

/* A format 2 file (ADR-0062) imports and is used as any: its chain counts
 * among its signals, its profile registers, and a format 1 file using a
 * call or a format 3 file is refused in the loader's words */
TEST(BridgeTest, AFormat2SignalFileImportsAndIsUsedAsAny)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const auto chain = [](const char* format, const char* expr) {
        return QStringLiteral(
                   R"({"havne_signals":%1,"name":"Chained","source":{"name":"t"},)"
                   R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
                   R"({"key":"a","name":"A","read":{"service":"22","number":"D100"},)"
                   R"("field":{"byte":0,"bits":8}},)"
                   R"({"key":"b","name":"B","kind":"computed","expr":"%2"},)"
                   R"({"key":"c","name":"C","kind":"computed","expr":"b * 2"}]})")
            .arg(QLatin1String(format), QLatin1String(expr))
            .toUtf8();
    };
    const QByteArray good = chain("2", "max(a, 1)");
    QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "chain.json", good));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 3);
    EXPECT_EQ(r.value(QStringLiteral("profile")).toString().toStdString(),
              "user.chained");
    EXPECT_EQ(bridge.useSignalFiles({sha256Of(good)}).size(), 1);
    EXPECT_TRUE(registered("user.chained"));

    r = bridge.importSignalFile(
        writeScratch(from.path(), "one.json", chain("1", "max(a, 1)")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("\"havne_signals\": 2")))
        << r.value(QStringLiteral("words")).toString().toStdString();

    r = bridge.importSignalFile(
        writeScratch(from.path(), "three.json", chain("3", "a")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("format 3")))
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(storeFiles(dir.store()).size(), 1);
}

/* A file says the built-in set it extends and how many of its signals
 * are its own, at import and in use: the page offers one extending obd2
 * in obd2's place (ADR-0052) */
TEST(BridgeTest, SignalFileSaysWhatItExtendsAndHowManyAreItsOwn)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Plus"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QByteArray alone = signalFile(QStringLiteral("Alone"));
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const int base = static_cast<int>(obd2->count);

    QVariantMap r = bridge.importSignalFile(
        writeScratch(from.path(), "plus.json", extending));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(),
              "obd2");
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), base + 1);
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);
    r = bridge.importSignalFile(writeScratch(from.path(), "alone.json", alone));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("extends")).toString().toStdString(), "");
    EXPECT_EQ(r.value(QStringLiteral("own")).toInt(), 1);

    const QVariantList used =
        bridge.useSignalFiles({sha256Of(extending), sha256Of(alone)});
    ASSERT_EQ(used.size(), 2);
    for (const QVariant& v : used) {
        const QVariantMap e = v.toMap();
        ASSERT_TRUE(e.value(QStringLiteral("ok")).toBool());
        const bool plus =
            e.value(QStringLiteral("sha256")).toString() == sha256Of(extending);
        EXPECT_EQ(e.value(QStringLiteral("extends")).toString().toStdString(),
                  plus ? "obd2" : "");
        EXPECT_EQ(e.value(QStringLiteral("own")).toInt(), 1);
        EXPECT_EQ(e.value(QStringLiteral("count")).toInt(),
                  plus ? base + 1 : 1);
    }
    bridge.useSignalFiles({});
}

/* A refusal is the loader's words, never the path it came from; nothing
 * is kept */
TEST(BridgeTest, SignalFileRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    // On Windows the scratch folder is under the account's home, so its
    // name is in the path too
    const QString folder = from.path() + QStringLiteral("/jdoe-private");
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    QByteArray bad = signalFile(QStringLiteral("Extras"));
    bad.replace("\"bits\"", "\"bitz\"");
    QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "extras.json", bad));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("bitz"))) << words.toStdString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();

    r = bridge.importSignalFile(
        QUrl::fromLocalFile(folder + QStringLiteral("/gone.json")));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't read the file");

    for (const QString& line : log + QStringList{words}) {
        EXPECT_FALSE(line.contains(QStringLiteral("jdoe")))
            << line.toStdString();
        EXPECT_FALSE(line.contains(from.path())) << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("extras.json")))
            << line.toStdString();
    }
    EXPECT_TRUE(storeFiles(dir.store()).isEmpty());
}

/* use() registers the files it's given and unregisters the rest */
TEST(BridgeTest, SignalFileUseRegistersItsProfile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    QVariantList used = bridge.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo?profile=user.extras", &s),
              ECUIO_OK)
        << "a URI can choose it";
    ecuio_session_destroy(s);

    EXPECT_TRUE(bridge.useSignalFiles({}).isEmpty());
    EXPECT_FALSE(registered("user.extras"));
}

/* One profile, one file: two files of one name don't both register */
TEST(BridgeTest, SignalFileOneProfileHasOneFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b =
        signalFile(QStringLiteral("Extras"), QStringLiteral("D101"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QVariantList used = bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_EQ(used.size(), 2);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(used[1].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "user.extras is made by another file already (Extras)");
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("user.extras", &set), ECUIO_OK);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->defs[0].mux_key, 0x62D100u)
        << "the first file's, never replaced";
}

/* At start-up each copy is read again: a missing, changed or no longer
 * loading copy says so and isn't offered; a good one registers */
TEST(BridgeTest, SignalFileThatNoLongerLoadsAtStartUpSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    QByteArray stale = signalFile(QStringLiteral("Stale"));
    stale.replace("\"havne_signals\":1", "\"havne_signals\":9");
    writeScratch(dir.store(), sha256Of(good) + QStringLiteral(".json"), good);
    writeScratch(dir.store(), sha256Of(stale) + QStringLiteral(".json"), stale);
    const QString changed = sha256Of("other bytes");
    writeScratch(dir.store(), changed + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    const QString missing = QString(64, QLatin1Char('e'));

    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QVariantList used = bridge.useSignalFiles(
        {sha256Of(stale), missing, changed, sha256Of(good)});
    ASSERT_EQ(used.size(), 4);
    const QString staleWords =
        used[0].toMap().value(QStringLiteral("words")).toString();
    EXPECT_TRUE(staleWords.startsWith(QStringLiteral("It no longer loads: ")))
        << staleWords.toStdString();
    EXPECT_EQ(
        used[1].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy is missing: import the file again");
    EXPECT_EQ(
        used[2].toMap().value(QStringLiteral("words")).toString().toStdString(),
        "Its copy has changed: import the file again");
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(used[i].toMap().value(QStringLiteral("ok")).toBool()) << i;
    }
    EXPECT_FALSE(registered("user.stale"));
    EXPECT_TRUE(used[3].toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("user.extras"));
    EXPECT_EQ(log.filter(QStringLiteral(" not offered: ")).size(), 3)
        << log.join(QLatin1Char('\n')).toStdString();
}

/* A copy no vehicle keeps goes, its profile first; the folder with the
 * last one */
TEST(BridgeTest, SignalFileIsDeletedOnlyOnceItsProfileIsUnregistered)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    const QByteArray b = signalFile(QStringLiteral("Body"));
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "a.json", a))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge.importSignalFile(writeScratch(from.path(), "b.json", b))
                    .value(QStringLiteral("ok"))
                    .toBool());
    (void)bridge.useSignalFiles({sha256Of(a), sha256Of(b)});
    ASSERT_TRUE(registered("user.extras"));

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({sha256Of(b)}), 1);
    EXPECT_FALSE(registered("user.extras"));
    EXPECT_TRUE(registered("user.body"));
    EXPECT_EQ(storeFiles(dir.store()),
              QStringList{sha256Of(b) + QStringLiteral(".json")});

    EXPECT_EQ(bridge.deleteUnusedSignalFiles({}), 1);
    EXPECT_FALSE(registered("user.body"));
    EXPECT_FALSE(QDir(dir.store()).exists()) << "the folder with the last copy";
}

/* The form's file goes in as text and comes back as text; a refusal is
 * the loader's, nothing kept */
TEST(BridgeTest, SignalFileTextGoesInAndComesBack)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QString text = QString::fromUtf8(signalFile(QStringLiteral("Mine")));
    const QVariantMap r = bridge.importSignalText(text);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();
    EXPECT_EQ(sha, sha256Of(text.toUtf8()));
    EXPECT_EQ(bridge.signalFileText(sha), text);
    EXPECT_TRUE(bridge.signalFileText(QString(64, QLatin1Char('e'))).isEmpty());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Mine (user.mine, 1 signal)")))
        << log.join(QLatin1Char('\n')).toStdString();
    /* Two signals: the count's plural */
    QString two = QString::fromUtf8(signalFile(QStringLiteral("Pair")));
    two.replace(QStringLiteral("}}]}"),
                QStringLiteral("}},{\"key\":\"b\",\"name\":\"B\",\"read\":"
                               "{\"service\":\"22\",\"number\":\"D101\"},"
                               "\"field\":{\"byte\":0,\"bits\":8}}]}"));
    ASSERT_TRUE(
        bridge.importSignalText(two).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(log.contains(
        QStringLiteral("Signal file saved: Pair (user.pair, 2 signals)")))
        << log.join(QLatin1Char('\n')).toStdString();

    QString bad = text;
    bad.replace(QStringLiteral("\"bits\""), QStringLiteral("\"bitz\""));
    const QVariantMap refused = bridge.importSignalText(bad);
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(refused.value(QStringLiteral("words"))
                    .toString()
                    .contains(QStringLiteral("bitz")));
    EXPECT_EQ(storeFiles(dir.store()).size(), 2);
}

/* A recording of a session a vehicle's file decodes holds the store's
 * copy as signals.json, named by the copy's hash on events.log's header;
 * a session no file decodes has none (ADR-0051 section 4) */
TEST(BridgeTest, SignalFileRecordingHoldsTheStoresCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray bytes = signalFile(QStringLiteral("Recorded"));
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(
        bridge.importSignalFile(writeScratch(from.path(), "a.json", bytes))
            .value(QStringLiteral("ok"))
            .toBool());
    ASSERT_EQ(bridge.useSignalFiles({sha}).size(), 1);
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);

    ASSERT_TRUE(
        bridge
            .connectSession(QStringLiteral("mock://rec?profile=user.recorded"))
            .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2);
    folders.sort();
    const QString first = root.path() + QStringLiteral("/") + folders[0];
    const QString second = root.path() + QStringLiteral("/") + folders[1];
    QFile copy(first + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), bytes);
    QFile events(first + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_TRUE(events.readAll().contains(
        ("\nsignal file  signals.json," + sha + "\n").toUtf8()));
    EXPECT_FALSE(QFile::exists(second + QStringLiteral("/signals.json")));
}

// ---- ADR-0057's refs and ranges: the model's rows and profileSignals ----
namespace {

// The rows of the bridge's signal model, each a map of its role names
QVariantList modelRows(SessionBridge& bridge)
{
    SignalListModel* model = bridge.signalModel();
    const QHash<int, QByteArray> roles = model->roleNames();
    QVariantList rows;
    for (int i = 0; i < model->rowCount(); ++i) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromUtf8(it.value()),
                       model->data(model->index(i, 0), it.key()));
        }
        rows.append(row);
    }
    return rows;
}

// The row named name, an empty map if none
QVariantMap rowNamed(const QVariantList& rows, const QString& name)
{
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("name")).toString() == name) {
            return r.toMap();
        }
    }
    return {};
}

QStringList refsOf(const QVariantList& rows)
{
    QStringList refs;
    for (const QVariant& r : rows) {
        refs << r.toMap().value(QStringLiteral("ref")).toString();
    }
    return refs;
}

// Imports bytes into the store and uses it alone; its profile
QString useFile(SessionBridge& bridge, const QString& folder,
                const QByteArray& bytes)
{
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(folder, "f.json", bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(bridge.useSignalFiles({sha256Of(bytes)}).size(), 1);
    return r.value(QStringLiteral("profile")).toString();
}

// A signal file extending obd2: an 8-bit temperature at -40 and one at
// -50 (whose low end differs between the unit systems), an enum, a
// text, and a computed signal over the temperature
QByteArray rangesFile()
{
    return QByteArray(
        R"({"havne_signals":1,"name":"Ranges","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"temp","name":"Temp","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8},"offset":-40,"unit":"\u00b0C"},)"
        R"({"key":"gear","name":"Gear","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"kind":"enum","values":{"0":"Park"}},)"
        R"({"key":"part","name":"Part","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0},"kind":"text"},)"
        R"({"key":"cold","name":"Cold","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":8},"offset":-50,"unit":"\u00b0C"},)"
        R"({"key":"twice","name":"Twice","kind":"computed","expr":"temp * 2"}]})");
}

} // namespace

/* The mock's and obd2's rows are theirs by core's def ID; a row's ref
 * is the same in the model and in profileSignals */
TEST(BridgeTest, ABuiltInSetsRowsAreReferencedByTheirIds)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    const QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QVariant& r : rows) {
        const QVariantMap m = r.toMap();
        EXPECT_EQ(
            m.value(QStringLiteral("ref")).toString(),
            QStringLiteral("id:%1").arg(m.value(QStringLiteral("id")).toInt()))
            << m.value(QStringLiteral("name")).toString().toStdString();
    }
    EXPECT_EQ(
        rowNamed(rows, QStringLiteral("RPM")).value(QStringLiteral("ref")),
        QStringLiteral("id:1001"));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("mock://demo"))),
              refsOf(rows));
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString obd2 =
        QStringLiteral(
            "replay://%1/2026-10-25-engine-start?profile=obd2&speed=max")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    ASSERT_TRUE(bridge.connectSession(obd2).isEmpty());
    const QVariantList obd2Rows = modelRows(bridge);
    EXPECT_TRUE(refsOf(obd2Rows).contains(QStringLiteral("id:7003")));
    for (const QString& ref : refsOf(obd2Rows)) {
        EXPECT_TRUE(ref.startsWith(QStringLiteral("id:"))) << ref.toStdString();
    }
    EXPECT_EQ(refsOf(bridge.profileSignals(obd2)), refsOf(obd2Rows));
    EXPECT_EQ(refsOf(bridge.profileSignals(QStringLiteral("elm327://COM9"))),
              refsOf(obd2Rows))
        << "an adapter's default, obd2";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* uds-generic's rows and a set registered with no signal file (the
 * mock's, copied) have no ref, though they carry IDs */
TEST(BridgeTest, ASetWithNoFileHasNoRefs)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://demo?profile=uds-generic&did=1E1C:u16"))
                    .isEmpty());
    QVariantList rows = modelRows(bridge);
    ASSERT_GT(rows.size(), 0);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The registry keeps the set's pointer until it's unregistered
    static std::vector<ecuio_signal_def_t> defs;
    static ecuio_signal_set_t copy{};
    const ecuio_signal_set_t* mock = ecuio_mock_signal_set();
    defs.assign(mock->defs, mock->defs + mock->count);
    copy = *mock;
    copy.defs = defs.data();
    ASSERT_EQ(ecuio_profile_register("copied-mock", &copy, nullptr), ECUIO_OK);
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=copied-mock"))
            .isEmpty());
    rows = modelRows(bridge);
    ASSERT_EQ(rows.size(), static_cast<int>(mock->count));
    EXPECT_EQ(rowNamed(rows, QStringLiteral("RPM"))
                  .value(QStringLiteral("id"))
                  .toInt(),
              1001);
    for (const QString& ref : refsOf(rows)) {
        EXPECT_EQ(ref, QString());
    }
    const QVariantList copied = bridge.profileSignals(
        QStringLiteral("mock://demo?profile=copied-mock"));
    ASSERT_EQ(copied.size(), static_cast<int>(mock->count));
    for (const QString& ref : refsOf(copied)) {
        EXPECT_EQ(ref, QString());
    }
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(ecuio_profile_unregister("copied-mock"), ECUIO_OK);
}

/* The Dashboard starts a dial from the file's gauge: profileSignals' row
 * carries its range and levels in the decoded unit, and {} for a signal
 * with none or a level the file leaves out */
TEST(BridgeTest, AProfileRowCarriesTheFilesGauge)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray gauged = signalFile(QStringLiteral("Gauged"));
    gauged.replace(
        R"("field":{"byte":0,"bits":8}})",
        R"("field":{"byte":0,"bits":8},)"
        R"("gauge":{"min":10,"max":16,"warn_low":11.5,"warn_high":15.25}})");
    const QString profile = useFile(bridge, from.path(), gauged);
    ASSERT_FALSE(profile.isEmpty());
    const QVariantList rows =
        bridge.profileSignals(QStringLiteral("mock://demo?profile=") + profile);
    const QVariantMap gauge = rowNamed(rows, QStringLiteral("A"))
                                  .value(QStringLiteral("gauge"))
                                  .toMap();
    EXPECT_EQ(gauge.value(QStringLiteral("min")).toDouble(), 10.0);
    EXPECT_EQ(gauge.value(QStringLiteral("max")).toDouble(), 16.0);
    EXPECT_EQ(gauge.value(QStringLiteral("warnLow")).toDouble(), 11.5);
    EXPECT_EQ(gauge.value(QStringLiteral("warnHigh")).toDouble(), 15.25);

    QByteArray lowOnly =
        signalFile(QStringLiteral("LowOnly"), QStringLiteral("D101"));
    lowOnly.replace(R"("field":{"byte":0,"bits":8}})",
                    R"("field":{"byte":0,"bits":8},)"
                    R"("gauge":{"min":0,"max":100,"warn_low":20}})");
    const QString lowProfile = useFile(bridge, from.path(), lowOnly);
    ASSERT_FALSE(lowProfile.isEmpty());
    const QVariantMap low =
        rowNamed(bridge.profileSignals(QStringLiteral("mock://demo?profile=") +
                                       lowProfile),
                 QStringLiteral("A"))
            .value(QStringLiteral("gauge"))
            .toMap();
    EXPECT_TRUE(low.contains(QStringLiteral("warnLow")));
    EXPECT_FALSE(low.contains(QStringLiteral("warnHigh")));

    const QVariantList plain =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_FALSE(plain.isEmpty());
    for (const QVariant& row : plain) {
        EXPECT_TRUE(
            row.toMap().value(QStringLiteral("gauge")).toMap().isEmpty());
    }
}

/* A signal file's own defs are theirs by key, its extends base's by
 * the base's ID; a file extending nothing has keys alone */
TEST(BridgeTest, ASignalFilesRowsAreReferencedByTheirKeys)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QByteArray extending = signalFile(QStringLiteral("Extending"));
    extending.replace(R"("source":{"name":"t"},)",
                      R"("source":{"name":"t"},"extends":"obd2",)");
    const QString profile = useFile(bridge, from.path(), extending);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    const QVariantList rows = modelRows(bridge);
    EXPECT_EQ(rowNamed(rows, QStringLiteral("A")).value(QStringLiteral("ref")),
              QStringLiteral("key:a"));
    const QStringList refs = refsOf(rows);
    EXPECT_TRUE(refs.contains(QStringLiteral("id:7003"))) << "obd2's RPM";
    EXPECT_EQ(refs.filter(QStringLiteral("key:")).size(), 1);
    EXPECT_FALSE(refs.contains(QString()));
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)), refs);

    // A store change under a session leaves its rows' refs; the file's
    // profile is gone from the registry, so profileSignals gives []
    bridge.useSignalFiles({});
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QString alone =
        useFile(bridge, from.path(), signalFile(QStringLiteral("Alone")));
    ASSERT_FALSE(alone.isEmpty());
    EXPECT_EQ(refsOf(bridge.profileSignals(
                  QStringLiteral("mock://demo?profile=") + alone)),
              QStringList{QStringLiteral("key:a")});
    bridge.useSignalFiles({});
}

/* The mock answers Service 01 from its drive cycle and says what it
 * doesn't send: under a signal file that extends obd2, the PIDs its ECU
 * lacks (the file's own among them) and what is computed from one are
 * hidden as not sent by the ECU (ADR-0039 section 6), so a Dashboard cell
 * reads "not sent by this ECU", never "waiting for data" for good; obd2's
 * engine speed and Boost, the file's DID and what is computed from it
 * have values. The ECU's line is logged once. */
TEST(BridgeTest, TheMockSendsTheDemoCarsPidsAndHidesTheRest)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const QByteArray file(
        R"({"havne_signals":1,"name":"Disclaimed","source":{"name":"t"},)"
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"did","name":"Own DID","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"pid","name":"Own PID","read":{"service":"01","number":"06"},)"
        R"("field":{"byte":0,"bits":8}},)"
        R"({"key":"of_did","name":"Of DID","kind":"computed","expr":"did * 2"},)"
        R"({"key":"of_pid","name":"Of PID","kind":"computed","expr":"pid * 2"}]})");
    const QString profile = useFile(bridge, from.path(), file);
    ASSERT_FALSE(profile.isEmpty());
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://demo?profile=") + profile)
            .isEmpty());
    SignalListModel* model = bridge.signalModel();
    ASSERT_TRUE(spinUntil([&] { return model->hiddenCount() > 0; }, 3000));
    ASSERT_TRUE(spinUntil(
        [&] {
            return rowNamed(modelRows(bridge), QStringLiteral("Of DID"))
                .value(QStringLiteral("hasValue"))
                .toBool();
        },
        3000));

    const QVariantList rows = modelRows(bridge);
    const auto hidden = [&rows](const char* name) {
        const QVariantMap row = rowNamed(rows, QString::fromLatin1(name));
        EXPECT_FALSE(row.isEmpty()) << name;
        return row.value(QStringLiteral("hidden")).toBool();
    };
    const auto hasValue = [&rows](const char* name) {
        return rowNamed(rows, QString::fromLatin1(name))
            .value(QStringLiteral("hasValue"))
            .toBool();
    };
    EXPECT_FALSE(hidden("Own DID"));
    EXPECT_TRUE(hasValue("Own DID"));
    EXPECT_FALSE(hidden("Of DID"));
    EXPECT_TRUE(hidden("Own PID"));
    EXPECT_FALSE(hasValue("Own PID"));
    EXPECT_TRUE(hidden("Of PID")) << "computed from a PID the ECU lacks";
    EXPECT_FALSE(hasValue("Of PID"));
    EXPECT_FALSE(hidden("RPM")) << "obd2's, which the demo car sends";
    EXPECT_FALSE(hidden("Boost"))
        << "obd2's computed signal, from MAP and baro";
    ASSERT_TRUE(spinUntil(
        [&] {
            const QVariantList now = modelRows(bridge);
            return rowNamed(now, QStringLiteral("RPM"))
                       .value(QStringLiteral("hasValue"))
                       .toBool() &&
                   rowNamed(now, QStringLiteral("Boost"))
                       .value(QStringLiteral("hasValue"))
                       .toBool();
        },
        3000));

    // obd2's rows the ECU doesn't send are hidden, the rest shown in Live:
    // each field of a PID the demo car doesn't answer (sim_ecu.h's drive
    // PIDs and its diagnostics' readiness and counts since a clear)
    static const uint8_t kDemoPids[] = {
        0x01, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x21,
        0x24, 0x30, 0x31, 0x33, 0x41, 0x42, 0x44, 0x46, 0x4D, 0x4E, 0x5C};
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    int notSent = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            continue; /* Boost: MAP and baro are sent */
        }
        const auto pid = static_cast<uint8_t>(d.mux_key);
        notSent += std::find(std::begin(kDemoPids), std::end(kDemoPids), pid) ==
                           std::end(kDemoPids)
                       ? 1
                       : 0;
    }
    uint32_t unsupported = 0;
    ASSERT_EQ(ecuio_session_get_unsupported_signals(bridge.sessionForTesting(),
                                                    nullptr, 0, &unsupported),
              ECUIO_OK);
    int byId = 0;
    int byIdHidden = 0;
    for (const QVariant& r : rows) {
        const QVariantMap row = r.toMap();
        if (row.value(QStringLiteral("ref"))
                .toString()
                .startsWith(QStringLiteral("id:"))) {
            ++byId;
            byIdHidden += row.value(QStringLiteral("hidden")).toBool() ? 1 : 0;
        }
    }
    ASSERT_GT(byId, 100);
    EXPECT_EQ(byIdHidden, notSent);
    EXPECT_EQ(model->hiddenCount(), byIdHidden + 2) << "Own PID and Of PID";
    EXPECT_EQ(model->hiddenCount(), static_cast<int>(unsupported));
    EXPECT_EQ(bridge.liveSignalModel()->rowCount(),
              model->rowCount() - model->hiddenCount());

    spinLoop(300);
    const QStringList ecu = log.filter(QStringLiteral("ECU 0x7E8: supports "));
    ASSERT_EQ(ecu.size(), 1) << log.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(
        ecu.first().startsWith(QStringLiteral("ECU 0x7E8: supports 22 PIDs")))
        << ecu.first().toStdString();
    EXPECT_TRUE(
        ecu.first().contains(QStringLiteral("(not in its supported PIDs)")))
        << ecu.first().toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));

    // The mock's own profile: nothing hidden, no ECU line
    log.clear();
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    spinLoop(400);
    EXPECT_EQ(model->hiddenCount(), 0);
    EXPECT_TRUE(log.filter(QStringLiteral("ECU 0x7E8: supports ")).isEmpty());
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* A plugin variant's rows have refs, its file being the plugin
 * store's, in profileSignals and in a session's model alike */
TEST(BridgeTest, APluginVariantsRowsHaveRefs)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty());
    ASSERT_TRUE(bridge
                    .importPlugin(
                        writeScratch(from.path(), "p.havne-plugin.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QString uri =
        QStringLiteral("mock://demo?profile=plugin.example-sim.base");
    const QVariantList rows = bridge.profileSignals(uri);
    ASSERT_GT(rows.size(), 0);
    const QStringList refs = refsOf(rows);
    EXPECT_FALSE(refs.contains(QString())) << refs.join(' ').toStdString();
    EXPECT_FALSE(refs.filter(QStringLiteral("key:")).isEmpty());
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    EXPECT_EQ(refsOf(modelRows(bridge)), refs);
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* A def's range follows the unit system in the model and in
 * profileSignals; an enum's is scaled as the decoder scales it, and a
 * text or computed def has none (NaN) */
TEST(BridgeTest, ARowsRangeIsShownInTheUnitSystem)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QString profile = useFile(bridge, from.path(), rangesFile());
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());

    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        const bool metric = units == 0;
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
            ASSERT_FALSE(temp.isEmpty());
            EXPECT_EQ(temp.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("°C") : QStringLiteral("°F"));
            EXPECT_NEAR(temp.value(QStringLiteral("rangeLow")).toDouble(),
                        -40.0, 1e-9);
            EXPECT_NEAR(temp.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 215.0 : 419.0, 1e-9);
            EXPECT_FALSE(temp.value(QStringLiteral("isEnum")).toBool());
            const QVariantMap cold = rowNamed(rows, QStringLiteral("Cold"));
            ASSERT_FALSE(cold.isEmpty());
            EXPECT_NEAR(cold.value(QStringLiteral("rangeLow")).toDouble(),
                        metric ? -50.0 : -58.0, 1e-9);
            EXPECT_NEAR(cold.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 205.0 : 401.0, 1e-9);

            const QVariantMap gear = rowNamed(rows, QStringLiteral("Gear"));
            EXPECT_TRUE(gear.value(QStringLiteral("isEnum")).toBool());
            EXPECT_EQ(gear.value(QStringLiteral("rangeLow")).toDouble(), 0.0);
            EXPECT_EQ(gear.value(QStringLiteral("rangeHigh")).toDouble(),
                      255.0);

            for (const char* none : {"Part", "Twice"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(none));
                ASSERT_FALSE(row.isEmpty()) << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << none;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << none;
                EXPECT_FALSE(row.value(QStringLiteral("isEnum")).toBool())
                    << none;
            }
            const QVariantMap speed =
                rowNamed(rows, QStringLiteral("Vehicle Speed"));
            ASSERT_FALSE(speed.isEmpty()) << "obd2's, the base";
            EXPECT_EQ(speed.value(QStringLiteral("unit")).toString(),
                      metric ? QStringLiteral("km/h") : QStringLiteral("mph"));
            EXPECT_NEAR(speed.value(QStringLiteral("rangeHigh")).toDouble(),
                        metric ? 255.0 : 255.0 / 1.609344, 1e-3);
        }

        // profileSignals also gives the def's own unit and range, the
        // same in every unit system
        const QVariantList rows = bridge.profileSignals(uri);
        const QVariantMap temp = rowNamed(rows, QStringLiteral("Temp"));
        EXPECT_EQ(temp.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("°C"));
        EXPECT_NEAR(temp.value(QStringLiteral("decodedLow")).toDouble(), -40.0,
                    1e-9);
        EXPECT_NEAR(temp.value(QStringLiteral("decodedHigh")).toDouble(), 215.0,
                    1e-9);
        const QVariantMap speed =
            rowNamed(rows, QStringLiteral("Vehicle Speed"));
        EXPECT_EQ(speed.value(QStringLiteral("decodedUnit")).toString(),
                  QStringLiteral("km/h"));
        EXPECT_NEAR(speed.value(QStringLiteral("decodedHigh")).toDouble(),
                    255.0, 1e-9);
        const QVariantMap part = rowNamed(rows, QStringLiteral("Part"));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedLow")).toDouble()));
        EXPECT_TRUE(
            std::isnan(part.value(QStringLiteral("decodedHigh")).toDouble()));
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* A range whose end isn't a finite number (a 32-bit field at scale
 * 1e300), whose ends are one number (a byte at offset 1e300, past the
 * double's precision), whose end converts past a double (deg C near
 * DBL_MAX, times 1.8 in deg F), or whose ends meet once converted (a bit
 * at 1.75 km/h and one ulp more, one number in mph) is no range, NaN, in
 * the model and in profileSignals */
TEST(BridgeTest, ARangeOfAnEndPastADoubleIsNoRange)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes(
        R"({"havne_signals":1,"name":"Huge","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"huge","name":"Huge","read":{"service":"22","number":"D100"},)"
        R"("field":{"byte":0,"bits":32},"scale":1e300},)"
        R"({"key":"flat","name":"Flat","read":{"service":"22","number":"D101"},)"
        R"("field":{"byte":0,"bits":8},"offset":1e300},)"
        R"({"key":"edge","name":"Edge","read":{"service":"22","number":"D102"},)"
        R"("field":{"byte":0,"bits":32},"scale":3.49e298,"unit":"\u00b0C"},)"
        R"({"key":"meet","name":"Meet","read":{"service":"22","number":"D103"},)"
        R"("field":{"byte":0,"bits":1,"order":"le"},"offset":1.75,)"
        R"("scale":2.220446049250313e-16,"unit":"km/h"}]})");
    const QString profile = useFile(bridge, from.path(), bytes);
    ASSERT_FALSE(profile.isEmpty());
    const QString uri = QStringLiteral("mock://demo?profile=") + profile;
    ASSERT_TRUE(bridge.connectSession(uri).isEmpty());
    for (int units : {0, 1}) {
        bridge.setUnitSystem(units);
        for (const QVariantList& rows :
             {modelRows(bridge), bridge.profileSignals(uri)}) {
            for (const char* name : {"Huge", "Flat"}) {
                const QVariantMap row =
                    rowNamed(rows, QString::fromLatin1(name));
                ASSERT_FALSE(row.isEmpty()) << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeLow")).toDouble()))
                    << name;
                EXPECT_TRUE(std::isnan(
                    row.value(QStringLiteral("rangeHigh")).toDouble()))
                    << name;
            }
            // About 1.499e308 in deg C; deg F's 2.698e308 is inf
            const QVariantMap edge = rowNamed(rows, QStringLiteral("Edge"));
            ASSERT_FALSE(edge.isEmpty());
            const double lo = edge.value(QStringLiteral("rangeLow")).toDouble();
            const double hi =
                edge.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(lo, 0.0);
                EXPECT_GT(hi, 1.4e308);
                EXPECT_TRUE(std::isfinite(hi));
            } else {
                EXPECT_TRUE(std::isnan(lo)) << lo;
                EXPECT_TRUE(std::isnan(hi)) << hi;
            }
            // 1.75 and 1.75 + 2^-52 km/h are both 1.0873995864153345 mph
            const QVariantMap meet = rowNamed(rows, QStringLiteral("Meet"));
            ASSERT_FALSE(meet.isEmpty());
            const double mlo =
                meet.value(QStringLiteral("rangeLow")).toDouble();
            const double mhi =
                meet.value(QStringLiteral("rangeHigh")).toDouble();
            if (units == 0) {
                EXPECT_EQ(mlo, 1.75);
                EXPECT_EQ(mhi, std::nextafter(1.75, 2.0));
            } else {
                EXPECT_TRUE(std::isnan(mlo)) << mlo;
                EXPECT_TRUE(std::isnan(mhi)) << mhi;
            }
        }
    }
    bridge.setUnitSystem(0);
    ASSERT_TRUE(disconnectAndWait(bridge));
    bridge.useSignalFiles({});
}

/* profileSignals gives the rows of the profile a URI selects, and []
 * for one with no rows to bind or none registered */
TEST(BridgeTest, ProfileSignalsGivesTheProfilesRows)
{
    SessionBridge bridge;
    const QVariantList mock =
        bridge.profileSignals(QStringLiteral("mock://demo"));
    ASSERT_EQ(mock.size(), static_cast<int>(ecuio_mock_signal_set()->count));
    const QVariantMap rpm = rowNamed(mock, QStringLiteral("RPM"));
    EXPECT_EQ(rpm.value(QStringLiteral("id")).toInt(), 1001);
    EXPECT_EQ(rpm.value(QStringLiteral("ref")).toString(),
              QStringLiteral("id:1001"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.value(QStringLiteral("isText")).toBool());
    EXPECT_FALSE(rpm.value(QStringLiteral("isEnum")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("group")).toInt(),
              static_cast<int>(ECUIO_SIGNAL_GROUP_ENGINE));
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 0);
    EXPECT_NEAR(rpm.value(QStringLiteral("rangeHigh")).toDouble(), 65535 * 0.25,
                1e-9);

    for (const char* none :
         {"mock://demo?profile=uds-generic", "mock://demo?profile=no-such",
          "mock://demo?profile=raw", "replay://somewhere", "bogus://x",
          "mock://demo?profile="}) {
        EXPECT_TRUE(bridge.profileSignals(QString::fromLatin1(none)).isEmpty())
            << none;
    }
}

/* A replay's own signals.json is registered when it connects, so its
 * profile has no rows before and the file's after */
TEST(BridgeTest, ProfileSignalsOfAReplaysOwnFileFollowTheConnect)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    const QByteArray v1 = signalFile(QStringLiteral("Recorded"));
    const QString own = QStringLiteral("rec.") + sha256Of(v1).left(16);
    const QString folder = recordWithSignalFile(bridge, root.path(), v1);
    ASSERT_FALSE(folder.isEmpty());
    EXPECT_GE(bridge.deleteUnusedSignalFiles({}), 1);
    const QString uri = QStringLiteral("replay://") +
                        QDir::fromNativeSeparators(folder) +
                        QStringLiteral("?profile=") + own;
    EXPECT_TRUE(bridge.profileSignals(uri).isEmpty()) << "before the connect";
    ASSERT_TRUE(replayFolder(bridge, folder).isEmpty());
    EXPECT_TRUE(bridge.currentUri().endsWith(own))
        << bridge.currentUri().toStdString();
    EXPECT_EQ(refsOf(bridge.profileSignals(uri)),
              QStringList{QStringLiteral("key:a")});
    EXPECT_EQ(refsOf(modelRows(bridge)), QStringList{QStringLiteral("key:a")});
    ASSERT_TRUE(disconnectAndWait(bridge));
}

/* profileOfUri is core's choice of profile: each URI core takes gives
 * the profile its session reports, and one core refuses for its scheme
 * or its query gives "" */
TEST(BridgeTest, ProfileOfUriIsTheProfileCoreChooses)
{
    const QString fixture =
        QStringLiteral("replay://%1/2026-10-25-engine-start")
            .arg(QStringLiteral(ECUIO_REPLAY_FIXTURES));
    const QStringList taken = {
        QStringLiteral("mock://demo"),
        QStringLiteral("mock://demo?profile=obd2"),
        QStringLiteral("mock://demo?profile=raw&profile=mock"),
        QStringLiteral("mock://demo?profile=uds-generic&did=1E1C:u16"),
        fixture,
        fixture + QStringLiteral("?profile=obd2&speed=max"),
    };
    for (const QString& uri : taken) {
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_info_t info{};
        ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
        EXPECT_EQ(SessionBridge::profileOfUri(uri).toStdString(),
                  std::string(info.profile))
            << uri.toStdString();
        ecuio_session_destroy(s);
    }

    const QString tooLong =
        QStringLiteral("mock://demo?profile=") +
        QString(ECUIO_PROFILE_NAME_MAX + 1, QLatin1Char('x'));
    const QStringList refused = {
        QStringLiteral("Mock://demo"),
        QStringLiteral("bogus://x"),
        QStringLiteral("mock://demo?profile"),
        QStringLiteral("mock://demo?profile="),
        QStringLiteral("mock://demo?&profile=obd2"),
        tooLong,
    };
    for (const QString& uri : refused) {
        ecuio_session_t* s = nullptr;
        EXPECT_NE(ecuio_session_create(uri.toUtf8().constData(), &s), ECUIO_OK)
            << uri.toStdString();
        ecuio_session_destroy(s);
        EXPECT_EQ(SessionBridge::profileOfUri(uri), QString())
            << uri.toStdString();
    }

    // The longest name core takes, and the transports this test can't open
    const QString longest(ECUIO_PROFILE_NAME_MAX, QLatin1Char('x'));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("mock://demo?profile=") + longest),
              longest);
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("elm327://COM9")),
              QStringLiteral("obd2"));
    EXPECT_EQ(SessionBridge::profileOfUri(
                  QStringLiteral("elm327://COM9?profile=mock")),
              QStringLiteral("mock"));
#ifdef _WIN32
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QStringLiteral("obd2"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("j2534://dev0")),
              QString());
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QStringLiteral("raw"));
#else
    EXPECT_EQ(SessionBridge::profileOfUri(QStringLiteral("socketcan://vcan0")),
              QString());
#endif
}

// ---- The app's plugins (ADR-0052, PluginStore): the example plugin
// packed, as a phone imports it ----
namespace {

QStringList pluginCopies()
{
    return QDir(PluginStore::root()).entryList(QDir::Files, QDir::Name);
}

} // namespace

/* An import keeps the package's bytes by their hash and registers every
 * variant as plugin.<id>.<variant>; a later bridge loads it from its copy
 * at its first list */
TEST(BridgeTest, PluginImportKeepsACopyAndRegistersEveryVariant)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_FALSE(bytes.isEmpty()) << "the example plugin is readable";
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [&log](const QString& line) { log << line; });
        EXPECT_TRUE(bridge.plugins().isEmpty());
        const QVariantMap r = bridge.importPlugin(
            writeScratch(from.path(), "example-sim.havne-plugin.json", bytes));
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(r.value(QStringLiteral("sha256")).toString(), sha);
        EXPECT_EQ(r.value(QStringLiteral("id")).toString().toStdString(),
                  "example-sim");
        EXPECT_EQ(r.value(QStringLiteral("version")).toString().toStdString(),
                  "1.0.0");
        EXPECT_EQ(
            r.value(QStringLiteral("variants")).toStringList(),
            (QStringList{QStringLiteral("base"), QStringLiteral("my2019")}));
        EXPECT_EQ(r.value(QStringLiteral("profiles")).toStringList(),
                  (QStringList{QStringLiteral("plugin.example-sim.base"),
                               QStringLiteral("plugin.example-sim.my2019")}));
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("replaced")).toString(), QString());
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_TRUE(registered("plugin.example-sim.my2019"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha + QStringLiteral(".havne-plugin.json")});
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
        EXPECT_EQ(copy.readAll(), bytes);
        EXPECT_TRUE(log.contains(QStringLiteral(
            "Plugin: Example: the simulated car 1.0.0 "
            "(plugin.example-sim.base, plugin.example-sim.my2019)")))
            << log.join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(bridge.plugins().size(), 1);
    }
    EXPECT_FALSE(registered("plugin.example-sim.base")) << "freed at close";

    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.my2019"));
}

/* A refusal is core's words, never the path it came from; nothing is
 * kept */
TEST(BridgeTest, PluginRefusalNamesNoPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "broken.havne-plugin.json",
                     packed_plugin::example().replace("\"havne_plugin\": 1",
                                                      "\"havne_plugin\": 3")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    const QString words = r.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.contains(QStringLiteral("line"))) << words.toStdString();
    EXPECT_FALSE(words.contains(QStringLiteral("broken")))
        << words.toStdString();
    EXPECT_FALSE(words.contains(from.path())) << words.toStdString();
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* A kept copy is loaded at start only while its bytes are the ones its
 * name hashes: one changed since says so, registers nothing, and goes
 * with Remove */
TEST(BridgeTest, PluginCopyThatChangedSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
    }
    QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
               QStringLiteral(".havne-plugin.json"));
    ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    copy.close();

    SessionBridge bridge;
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_FALSE(e.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(e.value(QStringLiteral("words")).toString().toStdString(),
              "Its copy has changed: import the plugin again");
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_TRUE(bridge.removePlugin(sha).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(pluginCopies().isEmpty());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* One version per id: an import of an id loaded replaces it, its copy
 * going with it */
TEST(BridgeTest, PluginOfAnIdLoadedReplacesIt)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const QByteArray newer = packed_plugin::example("1.1.0");
    const QVariantMap r =
        bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("replaced")).toString().toStdString(),
              "1.0.0");
    EXPECT_EQ(
        pluginCopies(),
        QStringList{sha256Of(newer) + QStringLiteral(".havne-plugin.json")});
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all.at(0)
                  .toMap()
                  .value(QStringLiteral("version"))
                  .toString()
                  .toStdString(),
              "1.1.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose copy can't be kept (a folder where its file goes)
 * is refused with the loaded version still loaded, registered and kept,
 * so a later bridge loads it too */
TEST(BridgeTest, PluginReplaceWhoseCopyCantBeKeptKeepsTheLoadedOne)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        ASSERT_TRUE(QDir().mkpath(PluginStore::root() + QLatin1Char('/') +
                                  sha256Of(newer) +
                                  QStringLiteral(".havne-plugin.json")));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "Couldn't keep a copy of the plugin");
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_EQ(all.at(0)
                      .toMap()
                      .value(QStringLiteral("version"))
                      .toString()
                      .toStdString(),
                  "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A replace whose new version doesn't load (the store's seam, as a
 * registration refused would) gives way to the version it replaced:
 * loaded again from its copy, registered and kept, so a later bridge
 * loads it too; the new version's copy goes */
TEST(BridgeTest, PluginReplaceThatDoesntLoadLoadsTheReplacedOneAgain)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QByteArray newer = packed_plugin::example("1.1.0");
    const auto onlyVersion = [](const QVariantList& all) {
        return all.size() == 1 &&
                       all.at(0).toMap().value(QStringLiteral("ok")).toBool()
                   ? all.at(0)
                         .toMap()
                         .value(QStringLiteral("version"))
                         .toString()
                         .toStdString()
                   : std::string("not one loaded plugin");
    };
    {
        SessionBridge bridge;
        ASSERT_TRUE(
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                .value(QStringLiteral("ok"))
                .toBool());
        bridge.failPluginLoadForTesting(sha256Of(newer));
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "b.json", newer));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
                  "a load failed for the test");
        EXPECT_EQ(onlyVersion(bridge.plugins()), "1.0.0");
        EXPECT_TRUE(registered("plugin.example-sim.base"));
        EXPECT_EQ(pluginCopies(),
                  QStringList{sha256Of(bytes) +
                              QStringLiteral(".havne-plugin.json")});
    }
    SessionBridge again;
    EXPECT_EQ(onlyVersion(again.plugins()), "1.0.0");
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session on a variant decodes its signals; while it does, the plugin
 * is neither replaced nor removed, and once it has ended both go ahead */
TEST(BridgeTest, PluginInUseIsNeitherReplacedNorRemoved)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    const QByteArray bytes = packed_plugin::example();
    ASSERT_TRUE(bridge.importPlugin(writeScratch(from.path(), "a.json", bytes))
                    .value(QStringLiteral("ok"))
                    .toBool());
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://plugin?profile=plugin.example-sim.base"))
                    .isEmpty());
    SignalListModel* m = bridge.signalModel();
    QStringList names;
    for (int row = 0; row < m->rowCount(); ++row) {
        names << m->data(m->index(row), SignalListModel::NameRole).toString();
    }
    EXPECT_TRUE(names.contains(QStringLiteral("Oil Temp")))
        << names.join(',').toStdString();
    // The mock answers each of the variant's DIDs with its frame count,
    // so Oil Temp reads a whole raw number less the variant's offset, 40,
    // and moves on frame by frame
    const int oil = static_cast<int>(names.indexOf(QStringLiteral("Oil Temp")));
    ASSERT_GE(oil, 0);
    const auto oilValue = [m, oil]() {
        return m->data(m->index(oil), SignalListModel::ValueRole);
    };
    // ValueRole reads 0 before any value: wait for HasValueRole
    ASSERT_TRUE(spinUntil(
        [m, oil]() {
            return m->data(m->index(oil), SignalListModel::HasValueRole)
                .toBool();
        },
        5000, 10));
    const double first = oilValue().toDouble();
    EXPECT_GE(first, -40.0);
    EXPECT_LE(first, 215.0);
    EXPECT_EQ(first, std::floor(first)) << first;
    EXPECT_TRUE(
        spinUntil([&]() { return oilValue().toDouble() != first; }, 5000, 10))
        << first;

    QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), "b.json", packed_plugin::example("1.1.0")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "A session decodes with Example: the simulated car 1.0.0: "
              "disconnect, then import it again");
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(pluginCopies().size(), 1) << "its copy kept";
    EXPECT_TRUE(registered("plugin.example-sim.base"));

    ASSERT_TRUE(disconnectAndWait(bridge));
    r = bridge.removePlugin(sha256Of(bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_FALSE(QDir(PluginStore::root()).exists())
        << "the empty folder goes too";
    EXPECT_TRUE(bridge.plugins().isEmpty());
}

/* The VIN proposes variants (ADR-0052 section 5): the example matches
 * ZZZ from 2015 to 2030, its my2019 variant from 2019; a typed VIN or,
 * with none typed, the session's; no VIN, no match */
TEST(BridgeTest, PluginMatchesProposeVariantsByTheVin)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    const auto variants = [&bridge](const QString& vin) {
        QStringList out;
        for (const QVariant& v : bridge.pluginMatches(vin)) {
            out << v.toMap().value(QStringLiteral("profile")).toString();
        }
        return out;
    };
    /* Model year 2021: M, on the later cycle (a letter at position 7) */
    EXPECT_EQ(variants(QStringLiteral("ZZZFAKE11M0000001")),
              (QStringList{QStringLiteral("plugin.example-sim.base"),
                           QStringLiteral("plugin.example-sim.my2019")}));
    EXPECT_EQ(variants(QStringLiteral("zzz fake11g0000001")),
              QStringList{QStringLiteral("plugin.example-sim.base")})
        << "2016: before my2019's years; spaces and lower case taken";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE11A0000001")).isEmpty())
        << "2010: before the plugin's years";
    EXPECT_TRUE(variants(QStringLiteral("1FAFAKE11M0000001")).isEmpty())
        << "another maker";
    EXPECT_TRUE(variants(QStringLiteral("ZZZFAKE")).isEmpty()) << "not a VIN";
    EXPECT_TRUE(variants(QString()).isEmpty()) << "no session, no VIN";
    const QVariantMap first =
        bridge.pluginMatches(QStringLiteral("ZZZFAKE11M0000001")).at(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("name")).toString().toStdString(),
              "Example: the simulated car");
    EXPECT_EQ(first.value(QStringLiteral("variant")).toString().toStdString(),
              "base");
}

/* A copy whose bytes changed since it was kept is written again by an
 * import of the right bytes, so the next launch loads it: the words that
 * refused it say to import it again */
TEST(BridgeTest, SignalFileImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray good = signalFile(QStringLiteral("Extras"));
    const QString sha = sha256Of(good);
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    QTemporaryDir from;
    {
        SessionBridge bridge;
        const QVariantList before = bridge.useSignalFiles({sha});
        ASSERT_EQ(before.size(), 1);
        EXPECT_EQ(before[0]
                      .toMap()
                      .value(QStringLiteral("words"))
                      .toString()
                      .toStdString(),
                  "Its copy has changed: import the file again");
        const QVariantMap r =
            bridge.importSignalFile(writeScratch(from.path(), "e.json", good));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool())
            << "a copy that held other bytes is written, not kept";
    }
    SessionBridge later;
    const QVariantList used = later.useSignalFiles({sha});
    ASSERT_EQ(used.size(), 1);
    EXPECT_TRUE(used[0].toMap().value(QStringLiteral("ok")).toBool())
        << used[0]
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("user.extras"));
}

TEST(BridgeTest, PluginImportRepairsAChangedCopy)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray bytes = packed_plugin::example();
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    {
        QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
                   QStringLiteral(".havne-plugin.json"));
        ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
        copy.write(QByteArray(bytes).replace("Oil Temp", "Oil Tamp"));
    }
    {
        SessionBridge bridge;
        const QVariantList all = bridge.plugins();
        ASSERT_EQ(all.size(), 1);
        EXPECT_FALSE(all.at(0).toMap().value(QStringLiteral("ok")).toBool());
        const QVariantMap r =
            bridge.importPlugin(writeScratch(from.path(), "a.json", bytes));
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    }
    SessionBridge later;
    const QVariantList all = later.plugins();
    ASSERT_EQ(all.size(), 1);
    EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("ok")).toBool())
        << all.at(0)
               .toMap()
               .value(QStringLiteral("words"))
               .toString()
               .toStdString();
    EXPECT_TRUE(registered("plugin.example-sim.base"));
}

/* A session a plugin variant decodes records the variant's files merged
 * as signals.json, with the plugin's line (ADR-0052 section 4), as
 * havne-cli stream --plugin --record does; the mock answers the variant's
 * DIDs */
TEST(BridgeTest, PluginRecordingHoldsTheVariantsFile)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    ASSERT_TRUE(bridge
                    .importPlugin(writeScratch(from.path(), "a.json",
                                               packed_plugin::example()))
                    .value(QStringLiteral("ok"))
                    .toBool());
    bridge.setRecordingsRootForTesting(root.path());
    bridge.setRecording(true);
    ASSERT_TRUE(bridge
                    .connectSession(QStringLiteral(
                        "mock://rec?profile=plugin.example-sim.base"))
                    .isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    const QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 1);
    const QString folder = root.path() + QStringLiteral("/") + folders[0];
    QFile copy(folder + QStringLiteral("/signals.json"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly)) << "no signals.json";
    const QByteArray text = copy.readAll();
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_NE(ecuio_signal_file_find(file, "oil_temp"), nullptr);
    ecuio_signal_file_free(file);
    QFile events(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray log = events.readAll();
    EXPECT_TRUE(log.contains("\nplugin  example-sim,1.0.0,base\n"))
        << log.left(600).toStdString();
    EXPECT_TRUE(log.contains(
        ("\nsignal file  signals.json," + sha256Of(text) + "\n").toUtf8()));
}

/* The bridge's own refusal of a file it can't read names no path: the
 * folder can hold the account's name */
TEST(BridgeTest, AFileThatCantBeReadIsRefusedWithoutItsPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QUrl missing = QUrl::fromLocalFile(
        from.path() + QStringLiteral("/missing-extras.json"));
    SessionBridge bridge;
    for (const QVariantMap& r :
         {bridge.importSignalFile(missing), bridge.importPlugin(missing)}) {
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), "Couldn't read the file");
        EXPECT_FALSE(words.contains(QStringLiteral("missing")));
    }
}

/* A Save out of the app writes the store's copy byte for byte, so
 * havne-cli --signals and another vehicle read the file the vehicle has;
 * a copy changed since it was kept, a sha that isn't one and a target
 * that can't be written are each refused in words naming no path, and a
 * file already at the target is replaced whole */
TEST(BridgeTest, SignalFileSavedOutIsItsCopyByteForByte)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    // Its own spacing and order, which a writer of its own would change
    const QByteArray bytes =
        signalFile(QStringLiteral("Extras")).replace(",", ",\r\n  ");
    SessionBridge bridge;
    const QVariantMap r =
        bridge.importSignalFile(writeScratch(from.path(), "e.json", bytes));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString sha = r.value(QStringLiteral("sha256")).toString();

    const QString target = out.path() + QStringLiteral("/extras.json");
    writeScratch(out.path(), "extras.json", QByteArray(5000, 'x'));
    const QVariantMap saved =
        bridge.saveSignalFile(sha, QUrl::fromLocalFile(target));
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), bytes) << "the copy, byte for byte, and only it";
    f.close();

    const auto refused = [&](const QString& which, const QUrl& to,
                             const char* expected) {
        const QVariantMap no = bridge.saveSignalFile(which, to);
        EXPECT_FALSE(no.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = no.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(out.path()) || words.contains(dir.store()));
    };
    refused(QStringLiteral("../signals/x"), QUrl::fromLocalFile(target),
            "No such signal file");
    refused(QString(64, QLatin1Char('a')), QUrl::fromLocalFile(target),
            "Its copy is missing: import the file again");
    refused(sha, QUrl::fromLocalFile(out.path()), "Couldn't write the file");
    writeScratch(dir.store(), sha + QStringLiteral(".json"),
                 signalFile(QStringLiteral("X")));
    refused(sha, QUrl::fromLocalFile(out.path() + QStringLiteral("/b.json")),
            "Its copy has changed since it was kept: import the file again");
    EXPECT_FALSE(QFile::exists(out.path() + QStringLiteral("/b.json")));
}

/* A layout file (ADR-0057's appendix on the layout file): the text goes out
 * and comes back as it was, a file already at the target is replaced whole,
 * and a write that can't be made leaves what was there. */
TEST(BridgeTest, ALayoutFileRoundTripsItsText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QString text =
        QStringLiteral("{\"format\":2,\"label\":\"caf\u00e9 \U0001F600\",\"pages\":[]}");
    const QUrl target = QUrl::fromLocalFile(dir.path() + QStringLiteral("/l.json"));
    writeScratch(dir.path(), "l.json", QByteArray(5000, 'x'));
    const QVariantMap saved = bridge.saveLayoutFile(text, target);
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), text.toUtf8()) << "the text as UTF-8, replacing the file";
    f.close();
    const QVariantMap read = bridge.layoutFileText(target);
    EXPECT_TRUE(read.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(read.value(QStringLiteral("text")).toString(), text);
}

TEST(BridgeTest, ALayoutFileIsReadWithinItsBoundAndAsUtf8)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const auto refused = [&](const QUrl& url, const char* expected) {
        const QVariantMap r = bridge.layoutFileText(url);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(dir.path()));
        EXPECT_TRUE(r.value(QStringLiteral("text")).toString().isEmpty());
    };
    refused(QUrl::fromLocalFile(dir.path() + QStringLiteral("/none.json")),
            "Couldn't read the file");
    const qint64 most = SessionBridge::layoutFileMaxBytes;
    const QUrl at = writeScratch(dir.path(), "at.json", QByteArray(most, ' '));
    EXPECT_TRUE(bridge.layoutFileText(at).value(QStringLiteral("ok")).toBool());
    const QUrl over =
        writeScratch(dir.path(), "over.json", QByteArray(most + 1, ' '));
    refused(over, "The file is over 82400 bytes, a layout file's most");
    refused(writeScratch(dir.path(), "latin1.json", QByteArray("{\"a\":\"\xE9\"}")),
            "The file isn't UTF-8 text");
    refused(writeScratch(dir.path(), "cut.json", QByteArray("{\"a\":\"\xE2\x82\"}")),
            "The file isn't UTF-8 text");
    const QVariantMap bom = bridge.layoutFileText(
        writeScratch(dir.path(), "bom.json", QByteArray("\xEF\xBB\xBF{}")));
    EXPECT_TRUE(bom.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bom.value(QStringLiteral("text")).toString().toStdString(), "{}")
        << "a leading BOM is skipped, once";
}

TEST(BridgeTest, ALayoutThatCantBeSavedLeavesTheFileAsItWas)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QByteArray before(3000, 'y');
    const QUrl target = writeScratch(dir.path(), "keep.json", before);
    const auto unchanged = [&] {
        QFile f(target.toLocalFile());
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_EQ(f.readAll(), before) << "an existing file is never touched";
    };
    // Past the bound: refused before anything is opened
    QVariantMap r = bridge.saveLayoutFile(
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x')), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "The layout is over 82400 bytes, a layout file's most");
    unchanged();
    // Nothing to save
    r = bridge.saveLayoutFile(QString(), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "There is no layout to save");
    unchanged();
    // A target that can't take a file: a folder stays a folder, nothing
    // is removed, and the words name no path
    r = bridge.saveLayoutFile(QStringLiteral("{}"),
                              QUrl::fromLocalFile(dir.path()));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    EXPECT_TRUE(QFileInfo(dir.path()).isDir());
    unchanged();
    r = bridge.saveLayoutFile(
        QStringLiteral("{}"),
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/no/such/folder/l.json")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFileInfo(dir.path() + QStringLiteral("/no")).exists());
    unchanged();
}

// A provider's document is opened by its URI as Android's Uri.parse wants it:
// encoded, so a name the picker makes for a name taken ("report (1).csv")
// keeps its space and parentheses apart from the path's own separators
TEST(BridgeTest, APickedUrlIsOpenedByItsEncodedName)
{
    EXPECT_EQ(
        pathOfUrl(
            QUrl(QStringLiteral(
                "content://com.android.externalstorage.documents/document/"
                "primary%3ADocuments%2Freport (1).csv")))
            .toStdString(),
        "content://com.android.externalstorage.documents/document/"
        "primary%3ADocuments%2Freport%20(1).csv");
    EXPECT_EQ(pathOfUrl(QUrl::fromLocalFile(QStringLiteral("/tmp/a b (1).csv")))
                  .toStdString(),
              "/tmp/a b (1).csv");
}

// Android's document picker hands back a URI's text, encoded: the URL made of
// it opens by that same text, so the content resolver is asked for the
// document picked, its escapes and parentheses as they came
TEST(BridgeTest, APickersUriOpensByTheTextItCameAs)
{
    for (const char* uri : {
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20(1).json",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20%281%29.json",
             "content://com.android.providers.downloads.documents/document/"
             "msf%3A1000000034",
             "content://com.android.providers.media.documents/document/"
             "document%3A42",
             "content://com.google.android.apps.docs.storage/document/"
             "acc%3D1%3Bdoc%3Dencoded%3DAbC-_d%2Be%3D%3D",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADocuments%2F%C3%B8l%20%26%20%23%20%25.json",
         }) {
        const QString text = QString::fromLatin1(uri);
        const QUrl url = pickedUrl(text);
        EXPECT_TRUE(url.isValid()) << uri;
        EXPECT_FALSE(url.isLocalFile()) << uri;
        EXPECT_EQ(pathOfUrl(url).toStdString(), uri);
    }
}

// A picker through a provider makes the document before the app writes. A
// QUrl that isn't a file URL stands in for a content:// one: QFile opens its
// text as a path. A save that refuses removes only a document that was empty
// when the save began
TEST(BridgeTest, ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // pathOfUrl encodes a URL: a folder whose path an encoding changes (a
    // space or a non-ASCII name in it; a drive letter's case is no change)
    // can't stand in for a provider's
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly))
            << url.toString().toStdString();
        f.write(bytes);
    };
    const auto tooBig =
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x'));
    const QByteArray theirs("the user's layout");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"over the bound",
         [&](const QUrl& u) { return bridge.saveLayoutFile(tooBig, u); }},
        {"a signal file that isn't kept",
         [&](const QUrl& u) {
             return bridge.saveSignalFile(QStringLiteral("0123456789abcdef"),
                                          u);
         }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made(1).json"));
        make(made, QByteArray());
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the picker's empty document goes";
        const QUrl chosen = provider(QStringLiteral("chosen.json"));
        make(chosen, theirs);
        EXPECT_FALSE(save.run(chosen).value(QStringLiteral("ok")).toBool());
        QFile after(chosen.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a replaced document stays";
        after.close();
        QFile::remove(chosen.toString());
    }
    // A local target is never removed by a refusal
    const QUrl local =
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/local.json"));
    make(local, QByteArray());
    EXPECT_FALSE(bridge.saveLayoutFile(QString(), local)
                     .value(QStringLiteral("ok"))
                     .toBool());
    EXPECT_TRUE(QFile::exists(local.toLocalFile()));
}

// A save pick's verdict, from its document's last_modified against the
// picker's launch and answer: the window allows 2 s of skew each way, and a
// time past the answer says nothing (a replaced document's upload ending
// meanwhile)
TEST(BridgeTest, ASavePicksVerdictIsItsDocumentsTimeAgainstTheLaunch)
{
    const qint64 launched = 1'700'000'000'000;
    const qint64 answered = launched + 60'000;
    EXPECT_EQ(kPickMadeWindowMs, 2000);
    const auto judged = [&](qint64 modified) {
        return judgePick(launched, answered, modified);
    };
    EXPECT_EQ(judged(launched), PickMade::Made);
    EXPECT_EQ(judged(answered), PickMade::Made);
    EXPECT_EQ(judged(launched - kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s behind";
    EXPECT_EQ(judged(launched - kPickMadeWindowMs - 1), PickMade::Replaced);
    EXPECT_EQ(judged(launched - 86'400'000), PickMade::Replaced);
    EXPECT_EQ(judged(answered + kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s ahead";
    EXPECT_EQ(judged(answered + kPickMadeWindowMs + 1), PickMade::Unknown)
        << "past the answer";
    EXPECT_EQ(judged(-1), PickMade::Unknown) << "no time";
    EXPECT_EQ(judged(0), PickMade::Unknown) << "a provider's none";
}

// A pick's verdict is taken once, a later pick of the same document replaces
// it, and one with no time clears it
TEST(BridgeTest, ASavePicksVerdictIsTakenOnce)
{
    const QString where = QStringLiteral("content://test/document/verdict");
    notePick(where, PickMade::Made);
    EXPECT_EQ(peekPick(where), PickMade::Made);
    notePick(where, PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Unknown) << "taken once";
    notePick(where, PickMade::Made);
    notePick(where, PickMade::Unknown);
    EXPECT_EQ(takePick(where), PickMade::Unknown);
}

// Each branch of the rule a failed save removes its document by, judged at
// the save's entry; a missing document stands in for one a provider won't
// open (Proton Drive's, still uploading)
TEST(BridgeTest, APickedDocumentIsRemovableByItsVerdictAndItsBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray none;
    const QByteArray some("the user's bytes");
    struct Case {
        const char* name;
        PickMade verdict;
        const QByteArray* bytes; // null: unreadable
        bool removable;
    };
    const Case cases[] = {
        {"made, empty", PickMade::Made, &none, true},
        {"made, unreadable", PickMade::Made, nullptr, true},
        {"made, holding bytes", PickMade::Made, &some, false},
        {"replaced, empty", PickMade::Replaced, &none, false},
        {"replaced, unreadable", PickMade::Replaced, nullptr, false},
        {"no verdict, empty", PickMade::Unknown, &none, true},
        {"no verdict, unreadable", PickMade::Unknown, nullptr, false},
        {"no verdict, holding bytes", PickMade::Unknown, &some, false},
    };
    int n = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const QString path = dir.filePath(QStringLiteral("doc-%1").arg(++n));
        if (c.bytes != nullptr) {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write(*c.bytes);
        }
        notePick(path, c.verdict);
        EXPECT_EQ(pickedDocumentRemovable(path), c.removable);
        EXPECT_EQ(peekPick(path), PickMade::Unknown) << "the save took it";
    }
}

// The verdict through the bridge's saves (a non-file QUrl stands in for a
// provider's, as in ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument):
// a refused layout save and the live report's refusal with no session
// remove the pick's own empty document and leave one the user chose to
// replace, empty or not
TEST(BridgeTest, ARefusedSaveFollowsItsSavePicksVerdict)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    const QByteArray theirs("the user's earlier save");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"a layout with nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"a report with no session",
         [&](const QUrl& u) { return bridge.saveHealthReport(u, 0); }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made.txt"));
        make(made, QByteArray());
        notePick(pathOfUrl(made), PickMade::Made);
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the pick's own empty document goes";

        const QUrl replaced = provider(QStringLiteral("replaced.txt"));
        make(replaced, QByteArray());
        notePick(pathOfUrl(replaced), PickMade::Replaced);
        EXPECT_FALSE(save.run(replaced).value(QStringLiteral("ok")).toBool());
        EXPECT_TRUE(QFile::exists(replaced.toString()))
            << "an empty document the user chose to replace stays";
        EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown)
            << "the save took the verdict";

        const QUrl full = provider(QStringLiteral("full.txt"));
        make(full, theirs);
        notePick(pathOfUrl(full), PickMade::Made);
        EXPECT_FALSE(save.run(full).value(QStringLiteral("ok")).toBool());
        QFile after(full.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a document with bytes stays";
        after.close();
        QFile::remove(replaced.toString());
        QFile::remove(full.toString());
    }
}

// A save a page refuses before it calls the bridge (a layout that can't be
// written as a file) drops the pick's own empty document by the same rule
TEST(BridgeTest, ASaveThePageRefusesDropsOnlyThePicksOwnDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto empty = [&](const QString& name) {
        const QUrl url(dir.path() + QLatin1Char('/') + name,
                       QUrl::TolerantMode);
        QFile f(url.toString());
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        return url;
    };
    const QUrl made = empty(QStringLiteral("made.json"));
    notePick(pathOfUrl(made), PickMade::Made);
    bridge.dropPicked(made);
    EXPECT_FALSE(QFile::exists(made.toString()));
    const QUrl replaced = empty(QStringLiteral("replaced.json"));
    notePick(pathOfUrl(replaced), PickMade::Replaced);
    bridge.dropPicked(replaced);
    EXPECT_TRUE(QFile::exists(replaced.toString()));
    EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown);
}
