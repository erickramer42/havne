// File/layer: tests/test_bridge_signalfiles_plugin.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_signalfiles_support.h"

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
 * DIDs. A session no file decodes records none (ADR-0051 section 4) */
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
    ASSERT_TRUE(
        bridge.connectSession(QStringLiteral("mock://plain")).isEmpty());
    EXPECT_TRUE(bridge.recordingActive());
    ASSERT_TRUE(disconnectAndWait(bridge));

    QStringList folders = recordingFolders(root.path());
    ASSERT_EQ(folders.size(), 2);
    folders.sort();
    const QString folder = root.path() + QStringLiteral("/") + folders[0];
    EXPECT_FALSE(QFile::exists(root.path() + QStringLiteral("/") + folders[1] +
                               QStringLiteral("/signals.json")));
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
