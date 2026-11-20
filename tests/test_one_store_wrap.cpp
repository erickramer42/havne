// File/layer: tests/test_one_store_wrap.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_one_store_support.h"

/* The vehicle book gives a new vehicle the highest id plus one, so the
 * id of a forgotten vehicle comes back. Forgetting a vehicle releases
 * what was imported for it: the plugins stay the user's own, for no
 * vehicle, and the next vehicle with that id neither lists them as its
 * own nor has its OBDb set take the old car's place. A set kept for no
 * vehicle is a plugin of its own too */
TEST(BridgeTest, OneStoreAForgottenVehiclesIdTakesNoImportToTheNextVehicle)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QString v2 = QStringLiteral("v2");
    const QString importedFor = QStringLiteral("importedFor");
    const QString releasedOne =
        QStringLiteral("Plugins no longer imported for a forgotten vehicle: 2");
    const QString releasedAll = QStringLiteral(
        "Plugins no longer imported for the forgotten vehicles: 2");
    const auto own = [](SessionBridge& bridge, const QString& vehicle) {
        QStringList profiles;
        for (const QVariant& v :
             bridge.offeredVariants(vehicle, QString(), {})) {
            if (v.toMap().value(QStringLiteral("forThisCar")).toBool()) {
                profiles
                    << v.toMap().value(QStringLiteral("profile")).toString();
            }
        }
        return profiles;
    };
    {
        Logged b;
        QVariantMap r = keepObdb(b.bridge, 2019, name, v2, true);
        ASSERT_TRUE(ok(r)) << words(r);
        const QString old = str(r, "sha256");
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Own extras"))),
            QString(), QString(), v2);
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Other car"),
                                         QStringLiteral("D101"))),
            QString(), QString(), v1);
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(
            own(b.bridge, v2),
            (QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019"),
                         QStringLiteral("plugin.own-extras.base")}));

        // A vehicle no plugin names: nothing to release, nothing logged,
        // the index not written (a compact text stays compact)
        const QByteArray compact =
            QJsonDocument(
                QJsonDocument::fromJson(readBytes(indexPath())).object())
                .toJson(QJsonDocument::Compact);
        {
            QFile index(indexPath());
            ASSERT_TRUE(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
            index.write(compact);
        }
        const qsizetype lines = b.log.size();
        r = b.bridge.forgetVehicleImports(QStringLiteral("v9"));
        EXPECT_TRUE(ok(r));
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 0);
        r = b.bridge.forgetVehicleImports(QString());
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 0);
        EXPECT_EQ(b.log.size(), lines);
        EXPECT_EQ(readBytes(indexPath()), compact);
        EXPECT_EQ(own(b.bridge, v2).size(), 2);

        r = b.bridge.forgetVehicleImports(v2);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "");
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 2);
        EXPECT_EQ(b.log.last(), releasedOne) << b.lines();
        EXPECT_EQ(factsOf(QStringLiteral("daily-driver-obdb")).keys(),
                  (QStringList{QStringLiteral("mine"),
                               QStringLiteral("wrappedFrom")}))
            << "still the user's own, for no vehicle";
        EXPECT_FALSE(
            factsOf(QStringLiteral("own-extras")).contains(importedFor));
        EXPECT_EQ(
            factsOf(QStringLiteral("other-car")).value(importedFor).toString(),
            v1)
            << "another vehicle's stays";
        EXPECT_EQ(b.bridge.plugins().size(), 3);

        // The next vehicle is given the id again
        EXPECT_TRUE(own(b.bridge, v2).isEmpty());
        EXPECT_EQ(own(b.bridge, v1),
                  QStringList{QStringLiteral("plugin.other-car.base")});
        r = keepObdb(b.bridge, 2021, name, v2, true);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-2");
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool());
        EXPECT_TRUE(QFile::exists(copyPath(old))) << "the old car's plugin";
        EXPECT_TRUE(
            registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
        EXPECT_EQ(own(b.bridge, v2), QStringList{QStringLiteral(
                                         "plugin.daily-driver-obdb-2.my2021")});

        // Every vehicle forgotten
        r = b.bridge.forgetAllVehicleImports();
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 2)
            << "the other car's and the new one's";
        EXPECT_EQ(b.log.last(), releasedAll) << b.lines();
        EXPECT_TRUE(own(b.bridge, v1).isEmpty());
        EXPECT_TRUE(own(b.bridge, v2).isEmpty());
        EXPECT_EQ(
            column(b.bridge.plugins(), "mine"),
            (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                         QStringLiteral("true"), QStringLiteral("true")}));

        // A set kept for no vehicle goes to no earlier conversion, one
        // that is for no vehicle included
        r = keepObdb(b.bridge, 2020, name, QString(), true);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-3");
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_EQ(str(r, "importedFor"), QString());
        EXPECT_EQ(b.bridge.plugins().size(), 5);
    }
    // Written: a later start finds no import for either id
    SessionBridge again;
    EXPECT_EQ(again.plugins().size(), 5);
    EXPECT_TRUE(own(again, v1).isEmpty());
    EXPECT_TRUE(own(again, v2).isEmpty());
}

/* A wrap whose facts never reached the index (the write failed, the app
 * stopped) leaves a plugin with no facts: the same file again finds it
 * by its bytes and writes them, and makes no second plugin */
TEST(BridgeTest, OneStoreAWrapWhoseFactsWereNeverWrittenGetsThemTheNextTime)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Cut short"));
    const QString id = QStringLiteral("cut-short");
    const PluginStore::Wrap forV4{QStringLiteral("v4"), {}};
    QString sha;
    {
        PluginStore store;
        store.failIndexWriteForTesting();
        const QVariantMap r = store.wrap(file, forV4);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "The plugin is kept, but the store's index "
                            "couldn't be written yet: if no later change "
                            "writes it before the app closes, after a "
                            "restart it isn't marked as yours");
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool())
            << "while the app runs";
        sha = str(r, "sha256");
        EXPECT_TRUE(QFile::exists(copyPath(sha)));
        EXPECT_FALSE(QFile::exists(indexPath()));
    }
    PluginStore store;
    const QVariantList all = store.all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_FALSE(all.at(0).toMap().value(QStringLiteral("mine")).toBool());
    QVariantMap r = store.wrap(file, forV4);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), sha);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v4");
    EXPECT_EQ(
        factsOf(id),
        (QJsonObject{{QStringLiteral("mine"), true},
                     {QStringLiteral("importedFor"), QStringLiteral("v4")},
                     {QStringLiteral("wrappedFrom"),
                      QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}));
    EXPECT_EQ(copies(), QStringList{sha + kSuffix});

    // The seam fails one write: the next is written, with what the
    // failed one held
    store.failIndexWriteForTesting();
    r = store.wrap(signalFile(QStringLiteral("Second")), {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(words(r).empty());
    EXPECT_TRUE(factsOf(QStringLiteral("second")).isEmpty());
    ASSERT_TRUE(ok(store.markMine(sha)));
    EXPECT_FALSE(factsOf(QStringLiteral("second")).isEmpty());

    // A forget and a Mine the index never got: the words say a later
    // change may still write them
    store.failIndexWriteForTesting();
    r = store.forgetVehicle(QStringLiteral("v4"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 1);
    EXPECT_EQ(words(r), "They are released, but the store's index couldn't "
                        "be written yet: if no later change writes it before "
                        "the app closes, after a restart each may be "
                        "imported for its vehicle again");
    EXPECT_EQ(factsOf(id).value(QStringLiteral("importedFor")).toString(),
              QStringLiteral("v4"));
    r = store.import(coreWrap(signalFile(QStringLiteral("Third"))));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    const QString third = str(r, "sha256");
    store.failIndexWriteForTesting();
    r = store.markMine(third);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(words(r), "It is marked as yours, but the store's index "
                        "couldn't be written yet: if no later change "
                        "writes it before the app closes, after a "
                        "restart it isn't marked as yours");
    EXPECT_TRUE(factsOf(QStringLiteral("third")).isEmpty());

    // The next change written carries both: a new store finds them
    ASSERT_TRUE(store.setMigrated());
    store.close();
    PluginStore later;
    QStringList seen;
    for (const QVariant& v : later.all()) {
        const QVariantMap e = v.toMap();
        seen << QStringLiteral("%1 %2 %3")
                    .arg(e.value(QStringLiteral("id")).toString(),
                         e.value(QStringLiteral("mine")).toString(),
                         e.value(QStringLiteral("importedFor")).toString());
    }
    seen.sort();
    EXPECT_EQ(seen, (QStringList{QStringLiteral("cut-short true "),
                                 QStringLiteral("second true "),
                                 QStringLiteral("third true ")}));
    EXPECT_TRUE(later.migrated());
}

namespace {

// A one-signal file whose source names a license
QByteArray licensedFile(const QString& name, const QString& license,
                        const QString& did = QStringLiteral("D200"))
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"OBDb",)"
               R"("license":"%2"},"ecus":[{"id":"pcm","rx":"7E8"}],)"
               R"("signals":[{"key":"b","name":"B","read":{"service":"22",)"
               R"("number":"%3"},"field":{"byte":0,"bits":8}}]})")
        .arg(name, license, did)
        .toUtf8();
}

// A signal file's source as core reads it
ecuio_signal_source_t sourceOf(const QByteArray& file)
{
    ecuio_signal_source_t source{};
    source.struct_size = sizeof source;
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(file.constData(),
                                     static_cast<size_t>(file.size()), &f),
              ECUIO_OK)
        << ecuio_last_error_detail();
    if (f != nullptr) {
        EXPECT_EQ(ecuio_signal_file_source(f, &source), ECUIO_OK);
        ecuio_signal_file_free(f);
    }
    return source;
}

} // namespace

/* Save as a signal file, and the form's text: a variant's signal file is
 * core's text for it, loads alone under user.<the plugin's name>, and is
 * written byte for byte */
TEST(BridgeTest, OneStoreAVariantsSignalFileIsCoresTextAndLoadsAlone)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Daily extras"))),
        QString(), QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString sha = str(r, "sha256");
    const QString version = str(r, "version");

    r = b.bridge.pluginVariantText(sha, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray text = str(r, "text").toUtf8();

    // Core's own text for the kept copy's variant
    const QByteArray copy = readBytes(copyPath(sha));
    ecuio_plugin_t* p = nullptr;
    ASSERT_EQ(ecuio_plugin_load(copy.constData(),
                                static_cast<size_t>(copy.size()), nullptr,
                                nullptr, &p),
              ECUIO_OK)
        << ecuio_last_error_detail();
    size_t length = 0;
    const char* core =
        ecuio_signal_file_text(ecuio_plugin_variant_file(p, 0), &length);
    EXPECT_EQ(text, QByteArray(core, static_cast<qsizetype>(length)));
    EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);

    ecuio_signal_file_t* f = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &f),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_STREQ(ecuio_signal_file_profile(f), "user.daily-extras");
    ecuio_signal_file_free(f);
    EXPECT_TRUE(
        text.contains(QStringLiteral("Plugin daily-extras %1, variant base")
                          .arg(version)
                          .toUtf8()))
        << text.constData();

    // Saved: the same bytes
    const QUrl url =
        QUrl::fromLocalFile(out.path() + QStringLiteral("/extras.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("base"), url);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(readBytes(url.toLocalFile()), text);
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Variant base of plugin %1 saved as a signal file "
                             "(%2 bytes)")
                  .arg(sha.left(12))
                  .arg(text.size()))
        << b.lines();

    // Refusals, in words that name no path
    r = b.bridge.pluginVariantText(sha, QStringLiteral("my2019"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Daily extras has no variant 'my2019'");
    r = b.bridge.pluginVariantText(QStringLiteral("0123"),
                                   QStringLiteral("base"));
    EXPECT_EQ(words(r), "The store has no such plugin");
    const QUrl other =
        QUrl::fromLocalFile(out.path() + QStringLiteral("/other.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("my2019"), other);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Daily extras has no variant 'my2019'");
    EXPECT_FALSE(QFile::exists(other.toLocalFile()));
    const QUrl nowhere = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/no-such-folder/extras.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("base"), nowhere);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't write the file");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Variant base of plugin %1 not saved: Couldn't "
                             "write the file")
                  .arg(sha.left(12)))
        << b.lines();
}

/* Copy to my plugin: someone else's variant goes into a new plugin of the
 * user's own or one they pick, its source kept, and says it won't follow
 * the original's updates; it never takes a variant's place, and the
 * original stays as it was */
TEST(BridgeTest, OneStoreACopiedVariantKeepsItsSourceAndFollowsNoUpdates)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    const QByteArray file = licensedFile(QStringLiteral("Their car"),
                                         QStringLiteral("CC-BY-SA-4.0"));
    QVariantMap r = b.bridge.readImport(writeScratch(
        out.path(), QStringLiteral("theirs.json"), coreWrap(file)));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    r = b.bridge.pluginVariantText(theirs, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray text = str(r, "text").toUtf8();
    const QString follows =
        QStringLiteral("The copy won't follow Their car's updates to base");

    // Into a new plugin of the user's own, as the variant named
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), QString(),
                             QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "id").toStdString(), "their-car-2");
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("imported")});
    EXPECT_EQ(str(r, "importedFor"), QString());
    EXPECT_EQ(str(r, "notes"), follows);
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral("Plugin made: ")) &&
                b.log.last().endsWith(QStringLiteral("; ") + follows))
        << b.lines();
    EXPECT_EQ(factsOf(QStringLiteral("their-car-2")),
              (QJsonObject{{QStringLiteral("mine"), true},
                           {QStringLiteral("wrappedFrom"),
                            QJsonObject{{sha256Of(text),
                                         QStringLiteral("imported")}}}}));
    const QString copy = str(r, "sha256");
    r = b.bridge.pluginVariantText(copy, QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    const ecuio_signal_source_t kept = sourceOf(str(r, "text").toUtf8());
    EXPECT_STREQ(kept.name, "OBDb");
    EXPECT_STREQ(kept.license, "CC-BY-SA-4.0");
    EXPECT_NE(kept.flags & ECUIO_SOURCE_SHARE_ALIKE, 0u);
    EXPECT_TRUE(QFile::exists(copyPath(theirs))) << "the original stays";
    EXPECT_EQ(column(b.bridge.plugins(), "mine"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true")}));

    // The same variant again, under the same name, finds that plugin
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), QString(),
                             QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), copy);
    EXPECT_TRUE(
        b.log.last().startsWith(QStringLiteral("Plugin already here: ")))
        << b.lines();
    EXPECT_EQ(b.bridge.plugins().size(), 2);

    // Into a plugin of the user's own, as a variant of its own: named,
    // then by the file's name
    r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Own extras"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    QString own = str(r, "sha256");
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own,
                             QStringLiteral("theirs"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "own-extras");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("theirs")}));
    EXPECT_TRUE(str(r, "notes").endsWith(follows))
        << str(r, "notes").toStdString();
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral("Plugin changed: ")))
        << b.lines();
    own = str(r, "sha256");
    r = b.bridge.pluginVariantText(own, QStringLiteral("theirs"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_STREQ(sourceOf(str(r, "text").toUtf8()).license, "CC-BY-SA-4.0");
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own, QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("theirs"),
                           QStringLiteral("their-car")}));
    own = str(r, "sha256");

    // Never in a variant's place, named or by the file's name, nor into
    // the plugin it is in: nothing changes
    const QStringList before = copies();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own,
                             QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Own extras already has a variant 'base': give the "
                        "copy another name");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Plugin not changed: Own extras already has a "
                             "variant 'base': give the copy another name"))
        << b.lines();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own, QString());
    EXPECT_EQ(words(r), "Own extras already has a variant 'their-car': give "
                        "the copy another name");
    r = b.bridge.copyVariant(own, QStringLiteral("base"), own,
                             QStringLiteral("again"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Copy to another plugin: this variant is already in "
                        "Own extras");
    EXPECT_EQ(copies(), before);
    r = b.bridge.pluginVariantText(own, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_STRNE(sourceOf(str(r, "text").toUtf8()).name, "OBDb")
        << "the user's own base stays";

    // Never into someone else's plugin or one not here, nor from a
    // variant not there
    r = b.bridge.copyVariant(own, QStringLiteral("base"), theirs,
                             QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Their car isn't a plugin of your own: only your own "
                        "plugins are changed");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Plugin not changed: Their car isn't a plugin of "
                             "your own: only your own plugins are changed"))
        << b.lines();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"),
                             QStringLiteral("0123"), QStringLiteral("x"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    r = b.bridge.copyVariant(theirs, QStringLiteral("x"), QString(), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Their car has no variant 'x'");
    EXPECT_EQ(b.bridge.plugins().size(), 3);
}

/* Export plugin's note: each variant whose source's license is
 * share-alike is named with it, in havne-cli plugins check's words, and a
 * plugin with none has no note */
TEST(BridgeTest, OneStoreExportsNoteNamesEachShareAlikeVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Mixed"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    QString sha = str(r, "sha256");
    r = b.bridge.pluginExportNote(sha);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"), QString());
    EXPECT_TRUE(r.value(QStringLiteral("shareAlike")).toList().isEmpty());

    // A share-alike variant and an attribution-only one beside the base
    r = b.bridge.keepSignalText(
        QString::fromUtf8(licensedFile(QStringLiteral("Shared"),
                                       QStringLiteral("CC-BY-SA-4.0"))),
        sha, QStringLiteral("obdb"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.keepSignalText(
        QString::fromUtf8(licensedFile(QStringLiteral("Credited"),
                                       QStringLiteral("CC-BY-4.0"),
                                       QStringLiteral("D201"))),
        str(r, "sha256"), QStringLiteral("credited"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    sha = str(r, "sha256");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("obdb"),
                           QStringLiteral("credited")}));
    const QString line =
        QStringLiteral("Variant 'obdb' holds data under CC-BY-SA-4.0, a "
                       "share-alike license: whoever passes the plugin on "
                       "keeps that license and the variant's source with it");
    r = b.bridge.pluginExportNote(sha);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"), line);
    const QVariantList shared = r.value(QStringLiteral("shareAlike")).toList();
    ASSERT_EQ(shared.size(), 1);
    EXPECT_EQ(shared.at(0).toMap(),
              (QVariantMap{
                  {QStringLiteral("variant"), QStringLiteral("obdb")},
                  {QStringLiteral("license"), QStringLiteral("CC-BY-SA-4.0")},
                  {QStringLiteral("words"), line}}));

    // An OBDb set kept as a plugin: its one variant, under OBDb's license
    r = keepObdb(b.bridge, 2021, QStringLiteral("Daily driver OBDb"), QString(),
                 false);
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.pluginExportNote(str(r, "sha256"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"),
              QStringLiteral("Variant 'base' holds data under CC-BY-SA-4.0, a "
                             "share-alike license: whoever passes the plugin "
                             "on keeps that license and the variant's source "
                             "with it"));

    r = b.bridge.pluginExportNote(QStringLiteral("0123"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
}
