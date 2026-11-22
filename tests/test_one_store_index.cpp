// File/layer: tests/test_one_store_index.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_one_store_support.h"

/* The index is the store's own file: it keeps keys it doesn't know,
 * holds the migration's mark, maps a wrapped file to its plugin, is no
 * copy to an older store's listing, and goes with its last fact; a
 * plugin's facts go with the plugin, a copy that no longer loads too */
TEST(BridgeTest, OneStoreTheIndexKeepsWhatItDoesntKnowAndGoesWithItsLastFact)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Kept"));
    const QString id = QStringLiteral("kept");
    {
        PluginStore store;
        EXPECT_FALSE(store.migrated());
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        const QVariantMap r = store.wrap(file, {QStringLiteral("v1"), {}});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(
            store.wrappedFrom(sha256Of(file)),
            (QVariantList{QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("variant"), QStringLiteral("base")},
                {QStringLiteral("profile"), QStringLiteral("plugin.kept.base")},
                {QStringLiteral("wrapped"), true}}}));
        EXPECT_TRUE(store.wrappedFrom(sha256Of("another")).isEmpty());
        ASSERT_TRUE(QFile::exists(indexPath()));
        ASSERT_TRUE(ok(store.remove(str(r, "sha256"))));
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        EXPECT_FALSE(QFile::exists(indexPath()))
            << "an index with nothing to say leaves no file";
        EXPECT_FALSE(QDir(PluginStore::root()).exists())
            << "the empty folder goes too";
    }

    // The mark alone is something to say: the index stays for it
    {
        PluginStore store;
        ASSERT_TRUE(store.setMigrated());
        EXPECT_TRUE(QFile::exists(indexPath()));
    }
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated());
        EXPECT_TRUE(store.all().isEmpty());
    }

    // Keys from a later Havne stay as they are
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    {
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"format":1,"later":{"x":1},)"
                R"("plugins":{"ghost":{"mine":true,"note":"kept"}}})");
    }
    QString sha;
    {
        PluginStore store;
        const QVariantMap r = store.wrap(file, {});
        ASSERT_TRUE(ok(r)) << words(r);
        sha = str(r, "sha256");
        EXPECT_EQ(store.all().size(), 1) << "the index is no copy";
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("later"))
                      .toObject()
                      .value(QStringLiteral("x"))
                      .toInt(),
                  1);
        EXPECT_EQ(
            factsOf(QStringLiteral("ghost")),
            (QJsonObject{{QStringLiteral("mine"), true},
                         {QStringLiteral("note"), QStringLiteral("kept")}}));
        EXPECT_EQ(factsOf(id),
                  (QJsonObject{
                      {QStringLiteral("mine"), true},
                      {QStringLiteral("wrappedFrom"),
                       QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}))
            << "no vehicle given, none written";

        EXPECT_FALSE(store.migrated());
        ASSERT_TRUE(store.setMigrated());
        EXPECT_TRUE(store.migrated());
        index = QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("migrated")).toInt(), 1);
        EXPECT_FALSE(factsOf(id).isEmpty()) << "the mark changes no fact";
    }
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated()) << "read back by a later start";
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("mine")).toBool());
    }

    // A copy changed since still says whose facts go with it
    {
        QFile copy(copyPath(sha));
        ASSERT_TRUE(copy.open(QIODevice::ReadWrite));
        QByteArray bytes = copy.readAll();
        bytes.replace("\"name\": \"Kept\"", "\"name\": \"Kopt\"");
        copy.resize(0);
        copy.write(bytes);
    }
    {
        PluginStore store;
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        EXPECT_FALSE(ok(all.at(0).toMap()));
        EXPECT_EQ(words(all.at(0).toMap()),
                  "Its copy has changed: import the plugin again");
        ASSERT_TRUE(ok(store.remove(sha)));
        EXPECT_TRUE(factsOf(id).isEmpty());
        EXPECT_FALSE(factsOf(QStringLiteral("ghost")).isEmpty())
            << "another plugin's facts stay";
        EXPECT_TRUE(store.migrated());
    }

    // A fact another running app wrote since this one read the index
    // is kept by this one's next change
    PluginStore store;
    const QVariantMap r = store.wrap(file, {});
    ASSERT_TRUE(ok(r)) << words(r);
    {
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        plugins.insert(QStringLiteral("elsewhere"),
                       QJsonObject{{QStringLiteral("mine"), true}});
        plugins.remove(QStringLiteral("ghost"));
        index.insert(QStringLiteral("plugins"), plugins);
        index.remove(QStringLiteral("migrated"));
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(index).toJson());
    }
    ASSERT_TRUE(ok(store.markMine(str(r, "sha256"))));
    EXPECT_FALSE(factsOf(QStringLiteral("elsewhere")).isEmpty());
    EXPECT_TRUE(factsOf(QStringLiteral("ghost")).isEmpty())
        << "what the other removed stays removed";
    EXPECT_FALSE(factsOf(id).isEmpty());
    EXPECT_FALSE(store.migrated()) << "the file as it is now is the base";
}

/* An index the store can't trust is never written over: one that isn't
 * an index (not JSON, not an object, too large) is moved aside whole as
 * store.json.bad-N and a new one started; one that can't be moved, and a
 * later Havne's, is left as it is, and what is marked holds for the
 * running app and is said to the user */
TEST(BridgeTest, OneStoreAnIndexThatCantBeTrustedIsNeverWrittenOver)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QString aside = indexPath() + QStringLiteral(".bad-%1");
    const auto writeIndex = [](const QByteArray& bytes) {
        QDir().mkpath(PluginStore::root());
        QFile f(indexPath());
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
    };
    const QString moved =
        QStringLiteral("The plugin store's index couldn't be read: it is "
                       "kept as store.json.bad-%1 and a new one is started");
    const QString unwritten = QStringLiteral(
        "The plugin is kept, but the store's index couldn't be written yet: "
        "if no later change writes it before the app closes, after a restart "
        "it isn't marked as yours");

    // A hand edit's trailing comma
    const QByteArray comma =
        R"({"format":1,"migrated":1,"plugins":{"ghost":{"mine":true},}})";
    writeIndex(comma);
    {
        PluginStore store;
        QStringList said;
        store.setLog([&said](const QString& line) { said << line; });
        EXPECT_FALSE(store.migrated());
        EXPECT_EQ(readBytes(aside.arg(1)), comma);
        EXPECT_FALSE(QFile::exists(indexPath()));
        EXPECT_EQ(said, QStringList{moved.arg(1)});
        const QVariantMap r =
            store.wrap(signalFile(QStringLiteral("Kept")), {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "");
        EXPECT_FALSE(factsOf(QStringLiteral("kept")).isEmpty());
        EXPECT_TRUE(factsOf(QStringLiteral("ghost")).isEmpty());
        EXPECT_EQ(readBytes(aside.arg(1)), comma)
            << "whole after the new index was written";
        EXPECT_EQ(said.size(), 1);
    }

    // Each is no index: the next free name takes it, every byte kept
    const QByteArray good =
        R"({"format":1,"migrated":1,"plugins":{"ghost":{"mine":true}}})";
    const qsizetype most = 4 * 1024 * 1024;
    QByteArray large = good;
    large.insert(1, QByteArray(most + 1 - good.size(), ' '));
    ASSERT_EQ(large.size(), most + 1);
    const QByteArray bad[] = {QByteArray(), QByteArray("[1]"),
                              QByteArray(R"({"format":1,"plugins":5})"), large};
    int n = 1;
    for (const QByteArray& bytes : bad) {
        ++n;
        writeIndex(bytes);
        PluginStore store;
        EXPECT_FALSE(store.migrated()) << n;
        EXPECT_TRUE(QFile::exists(aside.arg(n))) << n;
        EXPECT_EQ(readBytes(aside.arg(n)), bytes) << n;
        EXPECT_FALSE(QFile::exists(indexPath())) << n;
    }

    // At its bound an index is read
    large.remove(1, 1);
    ASSERT_EQ(large.size(), most);
    writeIndex(large);
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated());
        EXPECT_EQ(readBytes(indexPath()).size(), most);
        EXPECT_FALSE(QFile::exists(aside.arg(n + 1)));
    }

    // No name left to move it to: the file stays as it is, the plugin is
    // the user's own while the app runs, and the user is told
    for (int i = 1; i <= 99; ++i) {
        if (!QFile::exists(aside.arg(i))) {
            QFile f(aside.arg(i));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
    }
    writeIndex(comma);
    {
        Logged b;
        const QString stuck = QStringLiteral(
            "The plugin store's index can't be read or moved aside: it is "
            "left as it is, and what is marked here holds until the app "
            "closes");
        QVariantMap r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Stuck"))), QString(),
            QString(), QStringLiteral("v1"));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "words"), unwritten);
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor").toStdString(), "v1");
        EXPECT_EQ(readBytes(indexPath()), comma) << "never written over";
        EXPECT_FALSE(QFile::exists(aside.arg(100)));
        EXPECT_TRUE(b.log.contains(
            QStringLiteral("Plugin made: Stuck 1.0.0 (plugin.stuck.base); ") +
            unwritten))
            << b.lines();
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Stuck two"))),
            QString(), QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "words"), unwritten);
        EXPECT_EQ(b.log.count(stuck), 1) << b.lines();
        EXPECT_EQ(readBytes(indexPath()), comma);
        EXPECT_TRUE(b.bridge.plugins()
                        .at(1)
                        .toMap()
                        .value(QStringLiteral("mine"))
                        .toBool())
            << "the first is still the user's own";

        // A name freed: the next change moves the file there and writes
        // what the running app held
        ASSERT_TRUE(QFile::remove(aside.arg(50)));
        ASSERT_TRUE(ok(b.bridge.markPluginMine(str(r, "sha256"))));
        EXPECT_EQ(readBytes(aside.arg(50)), comma);
        EXPECT_TRUE(b.log.contains(moved.arg(50))) << b.lines();
        EXPECT_EQ(factsOf(QStringLiteral("stuck"))
                      .value(QStringLiteral("importedFor"))
                      .toString()
                      .toStdString(),
                  "v1");
        EXPECT_FALSE(factsOf(QStringLiteral("stuck-two")).isEmpty());
    }

    // A later Havne's index is read and left as it is
    const QByteArray later = R"({"format":2,"shape":"new",)"
                             R"("plugins":{"stuck":{"mine":true,"x":1}}})";
    writeIndex(later);
    Logged b;
    const QVariantList all = b.bridge.plugins();
    ASSERT_EQ(column(all, "id"),
              (QStringList{QStringLiteral("kept"), QStringLiteral("stuck"),
                           QStringLiteral("stuck-two")}));
    EXPECT_EQ(column(all, "mine"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("false")}))
        << "read for what this version knows";
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "The plugin store's index is a later Havne's: it is read and left "
        "as it is, so what is marked here holds until the app closes")))
        << b.lines();
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Later"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "words"), unwritten);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    const QString kept =
        all.at(0).toMap().value(QStringLiteral("sha256")).toString();
    r = b.bridge.markPluginMine(kept);
    EXPECT_TRUE(ok(r));
    EXPECT_EQ(words(r), "It is marked as yours, but the store's index "
                        "couldn't be written yet: if no later change "
                        "writes it before the app closes, after a "
                        "restart it isn't marked as yours");
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin %1 marked as yours; It is marked as yours, but "
                       "the store's index couldn't be written yet: if no "
                       "later change writes it before the app closes, after "
                       "a restart it isn't marked as yours")
            .arg(kept.left(12))))
        << b.lines();
    EXPECT_EQ(column(b.bridge.plugins(), "mine"),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("false")}))
        << "each mark holds while the app runs: Kept, Later, Stuck";
    EXPECT_EQ(readBytes(indexPath()), later);
}

/* An OBDb set imported again for a vehicle whose earlier conversion the
 * user has added variants to joins them as one more: nothing of the
 * user's goes, the lineage stays and the version rises. The conversion
 * it already holds changes nothing */
TEST(BridgeTest, OneStoreAnObdbReimportKeepsTheVariantsTheUserAdded)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QString id = QStringLiteral("daily-driver-obdb");
    const QString my2019 = QStringLiteral("plugin.daily-driver-obdb.my2019");
    const QString my2021 = QStringLiteral("plugin.daily-driver-obdb.my2021");
    const QString track = QStringLiteral("plugin.daily-driver-obdb.track");
    QVariantMap r = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString lineage = str(r, "lineage");
    ASSERT_EQ(lineage.size(), 32);
    const QByteArray own =
        signalFile(QStringLiteral("Track day"), QStringLiteral("D150"));
    r = b.bridge.keepSignalText(QString::fromUtf8(own), str(r, "sha256"),
                                QStringLiteral("track"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    ASSERT_EQ(str(r, "version").toStdString(), "1.1.0");
    ASSERT_TRUE(r.value(QStringLiteral("obdb")).toBool())
        << "still a conversion to the store";

    r = keepObdb(b.bridge, 2021, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(str(r, "id"), id);
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("my2019"), QStringLiteral("track"),
                           QStringLiteral("my2021")}));
    EXPECT_EQ(str(r, "version").toStdString(), "1.2.0");
    EXPECT_EQ(str(r, "lineage"), lineage)
        << "another device's copy still updates from this one";
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.1.0");
    EXPECT_EQ(list(r, "replacedProfiles"), (QStringList{my2019, track}));
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "importedFor"), v1);
    EXPECT_EQ(b.bridge.plugins().size(), 1);
    EXPECT_EQ(copies(), QStringList{str(r, "sha256") + kSuffix});
    EXPECT_TRUE(registered(my2019));
    EXPECT_TRUE(registered(track)) << "the user's variant is still there";
    EXPECT_TRUE(registered(my2021));
    QStringList made;
    const QJsonObject from = madeFrom(id);
    for (auto it = from.begin(); it != from.end(); ++it) {
        made << it.value().toString();
    }
    made.sort();
    EXPECT_EQ(made,
              (QStringList{QStringLiteral("my2019"), QStringLiteral("my2021"),
                           QStringLiteral("track")}));
    EXPECT_EQ(from.value(sha256Of(own)).toString().toStdString(), "track");
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral(
        "Plugin from OBDb: Daily driver OBDb 1.2.0 "
        "(plugin.daily-driver-obdb.my2019, plugin.daily-driver-obdb.track, "
        "plugin.daily-driver-obdb.my2021), as a variant of the earlier "
        "import")))
        << b.lines();
    const QString merged = str(r, "sha256");

    // The conversion it holds, again: nothing changes, and it is said
    for (const int year : {2021, 2019}) {
        r = keepObdb(b.bridge, year, name, v1, true);
        ASSERT_TRUE(ok(r)) << words(r) << year;
        EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool()) << year;
        EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool()) << year;
        EXPECT_EQ(str(r, "sha256"), merged) << year;
        EXPECT_EQ(str(r, "version").toStdString(), "1.2.0") << year;
        EXPECT_EQ(words(r), "This file made your Daily driver OBDb "
                            "(daily-driver-obdb), now at 1.2.0: nothing new "
                            "was kept")
            << year;
    }
    EXPECT_EQ(b.log.last().toStdString(),
              "Plugin from OBDb: Daily driver OBDb 1.2.0 "
              "(plugin.daily-driver-obdb.my2019, "
              "plugin.daily-driver-obdb.track, "
              "plugin.daily-driver-obdb.my2021); This file made your Daily "
              "driver OBDb (daily-driver-obdb), now at 1.2.0: nothing new was "
              "kept");
    EXPECT_EQ(copies(), QStringList{merged + kSuffix});

    // The year again as another variant id joins too
    r = keepObdb(b.bridge, 2021, name, v1, false);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(list(r, "variants").last().toStdString(), "base");
    EXPECT_EQ(list(r, "variants").size(), 4);
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
}

/* A file that made a plugin of the user's own finds that plugin however
 * it has changed since, and makes no second one; a file only given to a
 * plugin as a variant still makes a plugin of its own. The index maps a
 * file to every variant made from it, the plugin it made first */
TEST(BridgeTest, OneStoreAFileFindsThePluginItMadeHoweverItHasChanged)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Extras"));
    const QByteArray hill =
        signalFile(QStringLiteral("Hill climb"), QStringLiteral("D160"));
    {
        Logged b;
        const auto keep = [&b](const QByteArray& f, const QString& into,
                               const QString& variant) {
            return b.bridge.keepSignalText(QString::fromUtf8(f), into, variant,
                                           QString());
        };
        QVariantMap r = keep(file, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        r = keep(hill, str(r, "sha256"), QStringLiteral("hill"));
        ASSERT_TRUE(ok(r)) << words(r);
        const QString changed = str(r, "sha256");

        r = keep(file, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "sha256"), changed);
        EXPECT_EQ(str(r, "id").toStdString(), "extras");
        EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
        EXPECT_EQ(words(r), "This file made your Extras (extras), now at "
                            "1.1.0: nothing new was kept");
        EXPECT_EQ(b.bridge.plugins().size(), 1) << "no extras-2";
        EXPECT_EQ(copies(), QStringList{changed + kSuffix});
        EXPECT_EQ(b.log.last().toStdString(),
                  "Plugin already here: Extras 1.1.0 (plugin.extras.base, "
                  "plugin.extras.hill); This file made your Extras (extras), "
                  "now at 1.1.0: nothing new was kept");

        // Given to a plugin as a variant, and now a plugin of its own
        r = keep(hill, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "id").toStdString(), "hill-climb");
        EXPECT_EQ(words(r), "");
        EXPECT_EQ(b.bridge.plugins().size(), 2);
        EXPECT_EQ(b.log.last().toStdString(),
                  "Plugin made: Hill climb 1.0.0 (plugin.hill-climb.base)");
    }
    {
        PluginStore store;
        const auto row = [](const char* id, const char* variant, bool wrapped) {
            return QVariantMap{
                {QStringLiteral("id"), QString::fromLatin1(id)},
                {QStringLiteral("variant"), QString::fromLatin1(variant)},
                {QStringLiteral("profile"),
                 QStringLiteral("plugin.%1.%2")
                     .arg(QString::fromLatin1(id),
                          QString::fromLatin1(variant))},
                {QStringLiteral("wrapped"), wrapped}};
        };
        EXPECT_EQ(store.wrappedFrom(sha256Of(file)),
                  QVariantList{row("extras", "base", true)});
        EXPECT_EQ(store.wrappedFrom(sha256Of(hill)),
                  (QVariantList{row("hill-climb", "base", true),
                                row("extras", "hill", false)}))
            << "the plugin the file made comes before one it was given to";
        EXPECT_TRUE(store.wrappedFrom(sha256Of("neither")).isEmpty());

        // A variant put in the place of the file's own: the plugin holds
        // the file no more, so the file makes a plugin again
        const QVariantList all = store.all();
        ASSERT_EQ(column(all, "id").value(0).toStdString(), "extras");
        QVariantMap r = store.put(
            all.at(0).toMap().value(QStringLiteral("sha256")).toString(),
            QStringLiteral("base"),
            signalFile(QStringLiteral("Other base"), QStringLiteral("D161")));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        r = store.wrap(file, {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "extras-2");
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    }
}

/* Facts left in the index by a copy that no longer loads are not a new
 * plugin's: a package of that id comes in as someone else's, and a file
 * wrapped under that id starts with facts of its own */
TEST(BridgeTest, OneStoreAPluginThatComesInNewTakesNoFactsLeftByAnother)
{
    const QByteArray file = signalFile(QStringLiteral("Kept"));
    const QString id = QStringLiteral("kept");
    const auto leaveStale = [&]() {
        QString sha;
        {
            PluginStore store;
            const QVariantMap r = store.wrap(file, {QStringLiteral("v1"), {}});
            EXPECT_TRUE(ok(r)) << words(r);
            sha = str(r, "sha256");
        }
        QFile copy(copyPath(sha));
        EXPECT_TRUE(copy.open(QIODevice::ReadWrite));
        QByteArray bytes = copy.readAll();
        bytes.replace("\"name\": \"Kept\"", "\"name\": \"Kopt\"");
        copy.resize(0);
        copy.write(bytes);
        return sha;
    };
    {
        SettingsDir dir;
        ASSERT_TRUE(dir.valid());
        const QString stale = leaveStale();
        PluginStore store;
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        ASSERT_FALSE(ok(all.at(0).toMap()));
        ASSERT_TRUE(factsOf(id).value(QStringLiteral("mine")).toBool());

        // Someone else's package with that id
        const QByteArray theirs = coreWrap(
            signalFile(QStringLiteral("Kept"), QStringLiteral("D177")));
        QVariantMap r = store.import(theirs);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor"), QString());
        EXPECT_TRUE(factsOf(id).isEmpty());
        // Not the user's own, so any package of its id takes its place
        r = store.import(coreWrap(
            signalFile(QStringLiteral("Kept"), QStringLiteral("D178"))));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");

        // The copy that didn't load goes, and takes no other's facts
        ASSERT_TRUE(ok(store.markMine(str(r, "sha256"))));
        ASSERT_TRUE(ok(store.remove(stale)));
        EXPECT_TRUE(factsOf(id).value(QStringLiteral("mine")).toBool());
    }
    {
        SettingsDir dir;
        ASSERT_TRUE(dir.valid());
        (void)leaveStale();
        PluginStore store;
        const QByteArray again =
            signalFile(QStringLiteral("Kept"), QStringLiteral("D179"));
        const QVariantMap r = store.wrap(again, {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor"), QString())
            << "the vehicle was the other plugin's";
        EXPECT_EQ(madeFrom(id),
                  (QJsonObject{{sha256Of(again), QStringLiteral("base")}}));
    }
}

/* A file is found by its bytes too: when the index has forgotten what a
 * plugin was made from, the same file again still finds its copy and
 * keeps it, for a conversion and for a plain file, and a package from
 * someone else that is the file's wrap byte for byte becomes the user's
 * own */
TEST(BridgeTest, OneStoreAFileTheIndexForgotIsFoundByItsBytes)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const ObdbImport::Preview set =
        ObdbImport::read(readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE)), 2019,
                         QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(set.ok) << set.words.toStdString();
    const QByteArray plain = signalFile(QStringLiteral("Plain"));
    const PluginStore::Wrap forV1{QStringLiteral("v1"),
                                  QStringLiteral("my2019")};
    QString conversion;
    QString own;
    {
        PluginStore store;
        QVariantMap r = store.wrap(set.json, forV1);
        ASSERT_TRUE(ok(r)) << words(r);
        conversion = str(r, "sha256");
        r = store.wrap(plain, {});
        ASSERT_TRUE(ok(r)) << words(r);
        own = str(r, "sha256");
    }
    {
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        for (const QString& id : plugins.keys()) {
            QJsonObject facts = plugins.value(id).toObject();
            ASSERT_FALSE(facts.value(QStringLiteral("wrappedFrom"))
                             .toObject()
                             .isEmpty());
            facts.remove(QStringLiteral("wrappedFrom"));
            plugins.insert(id, facts);
        }
        index.insert(QStringLiteral("plugins"), plugins);
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(index).toJson());
    }
    PluginStore store;
    QVariantMap r = store.wrap(set.json, forV1);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), conversion);
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(QFile::exists(copyPath(conversion)));
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb")),
              (QJsonObject{{sha256Of(set.json), QStringLiteral("my2019")}}))
        << "named again";

    r = store.wrap(plain, {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), own);
    EXPECT_EQ(store.all().size(), 2);
    EXPECT_EQ(copies().size(), 2);

    const QByteArray shared =
        signalFile(QStringLiteral("Shared"), QStringLiteral("D170"));
    r = store.import(coreWrap(shared));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    const QString theirs = str(r, "sha256");
    r = store.wrap(shared, {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), theirs);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(store.all().size(), 3);
}
