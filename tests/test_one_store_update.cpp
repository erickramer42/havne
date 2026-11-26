// File/layer: tests/test_one_store_update.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_one_store_support.h"

/* A plugin exported on one device, imported and marked Mine on another,
 * updates in place from its next export, the index's facts kept and a
 * pick of a variant the new version lacks shown as missing. The same
 * bytes change nothing; the same lineage at the same or a lower version
 * and a package that only reuses the id are refused, nothing changed */
TEST(BridgeTest, OneStoreAPluginOfTheUsersOwnUpdatesOnlyFromItsLaterVersion)
{
    SettingsDir desktop;
    SettingsDir phone;
    ASSERT_TRUE(desktop.valid() && phone.valid());
    QTemporaryDir files;
    const QString id = QStringLiteral("trip-extras");
    const QUrl firstUrl = QUrl::fromLocalFile(
        files.path() + QStringLiteral("/trip-extras-1.havne-plugin.json"));
    const QUrl secondUrl = QUrl::fromLocalFile(
        files.path() + QStringLiteral("/trip-extras-2.havne-plugin.json"));
    QString desktopSha;
    {
        desktop.enter();
        SessionBridge b;
        QVariantMap r = b.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Trip extras"))),
            QString(), QString(), QStringLiteral("v1"));
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.keepSignalText(
            QString::fromUtf8(
                signalFile(QStringLiteral("Two"), QStringLiteral("D101"))),
            str(r, "sha256"), QStringLiteral("two"), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(str(r, "version").toStdString(), "1.1.0");
        desktopSha = str(r, "sha256");
        ASSERT_TRUE(ok(b.exportPlugin(desktopSha, firstUrl)));
    }
    const QByteArray v110 = readBytes(firstUrl.toLocalFile());
    ASSERT_FALSE(v110.isEmpty());
    {
        phone.enter();
        SessionBridge b;
        const QVariantMap r = b.readImport(firstUrl);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
        EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
        EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool())
            << "nothing of the first device's index travels";
        ASSERT_TRUE(ok(b.markPluginMine(str(r, "sha256"))));
    }
    {
        // A fact the phone holds of it, to be kept through the update
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        QJsonObject facts = plugins.value(id).toObject();
        ASSERT_TRUE(facts.value(QStringLiteral("mine")).toBool());
        facts.insert(QStringLiteral("importedFor"), QStringLiteral("v9"));
        plugins.insert(id, facts);
        index.insert(QStringLiteral("plugins"), plugins);
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(index).toJson());
    }
    {
        desktop.enter();
        SessionBridge b;
        QVariantMap r = b.keepSignalText(
            QString::fromUtf8(
                signalFile(QStringLiteral("Three"), QStringLiteral("D102"))),
            desktopSha, QStringLiteral("three"), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.dropPluginVariant(str(r, "sha256"), QStringLiteral("two"));
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(str(r, "version").toStdString(), "1.3.0");
        ASSERT_TRUE(ok(b.exportPlugin(str(r, "sha256"), secondUrl)));
    }
    const QByteArray v130 = readBytes(secondUrl.toLocalFile());
    ASSERT_FALSE(v130.isEmpty());

    phone.enter();
    Logged b;
    const QStringList picks = {QStringLiteral("plugin.trip-extras.two"),
                               QStringLiteral("plugin.trip-extras.base")};
    QVariantMap r = b.bridge.readImport(secondUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "1.1.0");
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v9");
    EXPECT_EQ(copies(), QStringList{sha256Of(v130) + kSuffix});
    EXPECT_EQ(factsOf(id), (QJsonObject{{QStringLiteral("mine"), true},
                                        {QStringLiteral("importedFor"),
                                         QStringLiteral("v9")}}));
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin updated: Trip extras from 1.1.0 to 1.3.0 "
                       "(plugin.trip-extras.base, plugin.trip-extras.three)")))
        << b.lines();
    const QVariantList row = b.bridge.pickRow(picks, false);
    ASSERT_EQ(row.size(), 2);
    EXPECT_TRUE(row.at(0).toMap().value(QStringLiteral("missing")).toBool())
        << "the variant the new version dropped";
    EXPECT_FALSE(row.at(1).toMap().value(QStringLiteral("missing")).toBool());

    const auto unchanged = [&](const char* what) {
        const QVariantList all = b.bridge.plugins();
        ASSERT_EQ(all.size(), 1) << what;
        EXPECT_EQ(str(all.at(0).toMap(), "version").toStdString(), "1.3.0")
            << what;
        EXPECT_EQ(copies(), QStringList{sha256Of(v130) + kSuffix}) << what;
        EXPECT_TRUE(registered(QStringLiteral("plugin.trip-extras.three")))
            << what;
    };

    r = b.bridge.readImport(secondUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "updated"), QString()) << "the same bytes update nothing";
    unchanged("the same bytes");

    r = b.bridge.readImport(firstUrl);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r),
              "This file holds Trip extras 1.1.0, older than yours at 1.3.0");
    unchanged("a lower version");

    const auto read = [&](const char* name, const QByteArray& bytes) {
        return b.bridge.readImport(
            writeScratch(files.path(), QString::fromLatin1(name), bytes));
    };
    QByteArray apart = v130;
    apart.replace("\"name\": \"Trip extras\"", "\"name\": \"Trip extras B\"");
    ASSERT_NE(apart, v130);
    r = read("apart.json", apart);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Your Trip extras is at 1.3.0 and this file holds "
                        "1.3.0 with other content: the two were changed "
                        "apart, so keep one of them");
    unchanged("the same version, other bytes");

    const std::string reused =
        "This package has the id of your own plugin Trip extras 1.3.0 "
        "(trip-extras) and isn't a later version of it: remove yours first";
    QByteArray higher = v130;
    higher.replace("\"version\": \"1.3.0\"", "\"version\": \"9.0.0\"");
    ASSERT_NE(higher, v130);
    QString text = QString::fromUtf8(higher);
    const QRegularExpression lineage(
        QStringLiteral("\\s*\"lineage\": \"[0-9a-f]{32}\","));
    ASSERT_TRUE(lineage.match(text).hasMatch());
    text.remove(lineage);
    r = read("no-lineage.json", text.toUtf8());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), reused);
    unchanged("no lineage");

    text = QString::fromUtf8(higher);
    text.replace(
        QRegularExpression(QStringLiteral("\"lineage\": \"[0-9a-f]{32}\"")),
        QStringLiteral("\"lineage\": \"%1\"")
            .arg(QString(32, QLatin1Char('0'))));
    ASSERT_NE(text.toUtf8(), higher);
    r = read("other-lineage.json", text.toUtf8());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), reused);
    unchanged("another lineage");

    // The later version it is, once more to be sure nothing above stuck
    r = read("higher.json", higher);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "1.3.0");
    EXPECT_EQ(str(r, "version").toStdString(), "9.0.0");

    // Versions compare as numbers: 10 is later than 9
    QByteArray ten = v130;
    ten.replace("\"version\": \"1.3.0\"", "\"version\": \"10.0.0\"");
    r = read("ten.json", ten);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "9.0.0");
    EXPECT_EQ(str(r, "version").toStdString(), "10.0.0");

    // Where the plugin isn't the user's own, a package of its id takes
    // its place whatever its version
    SettingsDir other;
    ASSERT_TRUE(other.valid());
    SessionBridge c;
    ASSERT_TRUE(ok(c.readImport(secondUrl)));
    r = c.readImport(firstUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.3.0");
    EXPECT_EQ(str(r, "updated"), QString());
    EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
}

/* Export plugin writes the store's copy byte for byte, for anyone
 * else's plugin and for the user's own; a plugin made for a vehicle
 * with a VIN holds no VIN, no hash of it, no match, no variant named by
 * its year and nothing of the store's index */
TEST(BridgeTest, OneStoreAnExportIsTheStoresCopyAndHoldsNothingOfTheDevice)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    const QByteArray example = packed_plugin::example();
    QVariantMap r = b.bridge.readImport(
        writeScratch(out.path(), QStringLiteral("in.json"), example));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    const QUrl theirUrl = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/example-sim.havne-plugin.json"));
    r = b.bridge.exportPlugin(theirs, theirUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(readBytes(theirUrl.toLocalFile()), example);
    EXPECT_EQ(readBytes(theirUrl.toLocalFile()), readBytes(copyPath(theirs)));
    EXPECT_TRUE(
        b.log.contains(QStringLiteral("Plugin %1 exported to a file (%2 bytes)")
                           .arg(theirs.left(12))
                           .arg(example.size())))
        << b.lines();

    // The user's own, from an OBDb set at the year a VIN decodes to
    const QString vin = QStringLiteral("ZZZFAKE11M0000001");
    const int year = b.bridge.vinModelYear(vin);
    ASSERT_EQ(year, 2021);
    r = keepObdb(b.bridge, year, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v7"), false);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString own = str(r, "sha256");
    // The VIN's matches, worked out as the vehicle's list does
    EXPECT_FALSE(
        b.bridge.offeredVariants(QStringLiteral("v7"), vin, {}).isEmpty());
    const QUrl ownUrl = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/daily-driver-obdb.havne-plugin.json"));
    r = b.bridge.exportPlugin(own, ownUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray bytes = readBytes(ownUrl.toLocalFile());
    ASSERT_FALSE(bytes.isEmpty());
    EXPECT_EQ(bytes, readBytes(copyPath(own)));
    const QString salt = b.bridge.newSalt();
    const QByteArray hash = QCryptographicHash::hash((salt + vin).toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex();
    for (const QByteArray& gone :
         {vin.toUtf8(), vin.toLower().toUtf8(), vin.left(3).toUtf8(), hash,
          QByteArray("\"match\""), QByteArray("my20"), QByteArray("mine"),
          QByteArray("importedFor"), QByteArray("wrappedFrom"),
          QByteArray("\"v7\"")}) {
        EXPECT_FALSE(bytes.contains(gone)) << gone.toStdString();
    }
    const QJsonObject manifest = QJsonDocument::fromJson(bytes).object();
    EXPECT_FALSE(manifest.contains(QStringLiteral("match")));
    EXPECT_EQ(manifest.value(QStringLiteral("variants"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("id"))
                  .toString()
                  .toStdString(),
              "base");
    EXPECT_FALSE(factsOf(QStringLiteral("daily-driver-obdb")).isEmpty())
        << "the facts are in the index, where they stay";

    // Refusals: no such plugin, a copy changed since, a file that can't
    // be written; none writes a file or names a path
    const QString nowhere = out.path() + QStringLiteral("/none.json");
    r = b.bridge.exportPlugin(QString(64, QLatin1Char('0')),
                              QUrl::fromLocalFile(nowhere));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    EXPECT_FALSE(QFile::exists(nowhere));

    r = b.bridge.exportPlugin(own, QUrl::fromLocalFile(out.path()));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't write the file");

    {
        QFile copy(copyPath(theirs));
        ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
        copy.write(QByteArray(example).replace("Oil Temp", "Oil Tamp"));
    }
    r = b.bridge.exportPlugin(theirs, QUrl::fromLocalFile(nowhere));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Its copy has changed: import the plugin again");
    EXPECT_FALSE(QFile::exists(nowhere));
    for (const QString& line : std::as_const(b.log)) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

/* A vehicle's picks are an ordered list: a pick goes last, one picked
 * again after it was taken out goes last too, 16 at most, and only a
 * variant a plugin here has. On an adapter the first pick that extends
 * obd2 stands in for Default. The vehicle's list offers every variant,
 * the VIN's matches and the plugins imported for it first */
TEST(BridgeTest, OneStoreAVehiclesPicksKeepTheirOrderAndTheFirstObd2OneStandsIn)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    ASSERT_TRUE(ok(b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("a.json"), packed_plugin::example()))));
    const auto made = [&](const char* name, const char* did, bool obd2) {
        const QVariantMap r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QString::fromLatin1(name),
                                         QString::fromLatin1(did), obd2)),
            QString(), QString(), QString());
        EXPECT_TRUE(ok(r)) << words(r);
        return list(r, "profiles").value(0);
    };
    const QString plain = made("Plain", "D100", false);
    const QString second = made("Second obd2", "D101", true);
    const QString first = made("First obd2", "D102", true);
    ASSERT_EQ(plain.toStdString(), "plugin.plain.base");

    QVariantMap p = b.bridge.pickVariant({}, plain);
    ASSERT_TRUE(ok(p)) << words(p);
    p = b.bridge.pickVariant(list(p, "picks"), second);
    p = b.bridge.pickVariant(list(p, "picks"), first);
    ASSERT_TRUE(ok(p)) << words(p);
    const QStringList picks = list(p, "picks");
    EXPECT_EQ(picks, (QStringList{plain, second, first}));
    p = b.bridge.pickVariant(picks, second);
    EXPECT_TRUE(ok(p));
    EXPECT_EQ(list(p, "picks"), picks) << "picked already: as it was";

    p = b.bridge.pickVariant(picks, QStringLiteral("plugin.gone.base"));
    EXPECT_FALSE(ok(p));
    EXPECT_EQ(words(p), "No plugin here has that variant");
    EXPECT_EQ(list(p, "picks"), picks);

    const QStringList without = b.bridge.unpickVariant(picks, second);
    EXPECT_EQ(without, (QStringList{plain, first}));
    p = b.bridge.pickVariant(without, second);
    EXPECT_EQ(list(p, "picks"), (QStringList{plain, first, second}))
        << "picked again: last";

    QStringList full;
    for (int i = 0; i < PluginStore::MaxPicks; ++i) {
        full << QStringLiteral("plugin.gone.v%1").arg(i);
    }
    p = b.bridge.pickVariant(full, plain);
    EXPECT_FALSE(ok(p));
    EXPECT_EQ(words(p),
              "A vehicle picks at most 16 plugin variants: unpick one first");
    EXPECT_EQ(list(p, "picks"), full);
    p = b.bridge.pickVariant(full.mid(1), plain);
    EXPECT_TRUE(ok(p)) << words(p);
    EXPECT_EQ(list(p, "picks").size(), PluginStore::MaxPicks);
    EXPECT_EQ(list(p, "picks").last(), plain);

    // The row: only the first obd2 pick stands in, and only on an adapter
    const QStringList shown = {plain, second, first,
                               QStringLiteral("plugin.gone.base")};
    QVariantList row = b.bridge.pickRow(shown, true);
    EXPECT_EQ(column(row, "profile"), shown);
    EXPECT_EQ(column(row, "standIn"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("false"), QStringLiteral("false")}));
    EXPECT_EQ(column(row, "missing"),
              (QStringList{QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true")}));
    EXPECT_EQ(column(row, "extends"),
              (QStringList{QString(), QStringLiteral("obd2"),
                           QStringLiteral("obd2"), QString()}));
    EXPECT_EQ(
        column(row, "name"),
        (QStringList{QStringLiteral("Plain"), QStringLiteral("Second obd2"),
                     QStringLiteral("First obd2"), QString()}));
    EXPECT_EQ(column(row, "variant"),
              (QStringList{QStringLiteral("base"), QStringLiteral("base"),
                           QStringLiteral("base"), QString()}));
    row = b.bridge.pickRow(shown, false);
    EXPECT_FALSE(column(row, "standIn").contains(QStringLiteral("true")))
        << "off an adapter Default isn't obd2";
    row = b.bridge.pickRow({first, second}, true);
    EXPECT_EQ(column(row, "standIn"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false")}))
        << "the order picked decides";

    // The vehicle's list
    const QVariantMap obdb =
        keepObdb(b.bridge, 2019, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(obdb)) << words(obdb);
    const QString imported = QStringLiteral("plugin.daily-driver-obdb.my2019");
    const QString base = QStringLiteral("plugin.example-sim.base");
    const QString my2019 = QStringLiteral("plugin.example-sim.my2019");
    const QStringList byName = {imported, base, my2019, first, plain, second};

    QVariantList offered =
        b.bridge.offeredVariants(QStringLiteral("v1"), QString(), {second});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_EQ(column(offered, "forThisCar"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false")}));
    EXPECT_EQ(column(offered, "picked"),
              (QStringList{QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true")}));
    EXPECT_EQ(column(offered, "mine"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("true")}));
    EXPECT_EQ(column(offered, "extends").value(4), QString());
    EXPECT_EQ(column(offered, "extends").value(5).toStdString(), "obd2");
    EXPECT_EQ(column(offered, "variant").value(0).toStdString(), "my2019");
    EXPECT_EQ(column(offered, "name").value(1).toStdString(),
              "Example: the simulated car");

    // The VIN's matches join the imported one, each group by name
    offered = b.bridge.offeredVariants(QStringLiteral("v2"),
                                       QStringLiteral("ZZZFAKE11M0000001"), {});
    EXPECT_EQ(column(offered, "profile"),
              (QStringList{base, my2019, imported, first, plain, second}))
        << "another vehicle: the import isn't its own";
    EXPECT_EQ(column(offered, "forThisCar").mid(0, 3),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("false")}));
    offered = b.bridge.offeredVariants(QStringLiteral("v1"),
                                       QStringLiteral("ZZZFAKE11M0000001"), {});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_EQ(column(offered, "forThisCar").mid(0, 4),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("false")}));

    // No vehicle: nothing is its own
    offered = b.bridge.offeredVariants(QString(), QString(), {});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_FALSE(
        column(offered, "forThisCar").contains(QStringLiteral("true")));
}

// A save out of the store that is refused removes only the document its
// own pick made, as every save does: one the user chose to replace stays,
// and so does one with bytes
TEST(BridgeTest, OneStoreARefusedSaveFollowsItsSavePicksVerdict)
{
    SettingsDir settings;
    ASSERT_TRUE(settings.valid());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    Logged b;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    const QString none(64, QLatin1Char('0'));
    const QByteArray theirs("the user's earlier save");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"a variant of a plugin the store lacks",
         [&](const QUrl& u) {
             return b.bridge.saveVariantFile(none, QStringLiteral("base"), u);
         }},
        {"a plugin the store lacks",
         [&](const QUrl& u) { return b.bridge.exportPlugin(none, u); }},
        {"a file that didn't move and isn't kept",
         [&](const QUrl& u) { return b.bridge.saveUnmigratedFile(none, u); }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made.json"));
        make(made, QByteArray());
        notePick(pathOfUrl(made), PickMade::Made);
        EXPECT_FALSE(ok(save.run(made)));
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the pick's own empty document goes";

        const QUrl replaced = provider(QStringLiteral("replaced.json"));
        make(replaced, QByteArray());
        notePick(pathOfUrl(replaced), PickMade::Replaced);
        EXPECT_FALSE(ok(save.run(replaced)));
        EXPECT_TRUE(QFile::exists(replaced.toString()))
            << "an empty document the user chose to replace stays";
        EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown)
            << "the save took the verdict";

        const QUrl full = provider(QStringLiteral("full.json"));
        make(full, theirs);
        notePick(pathOfUrl(full), PickMade::Made);
        EXPECT_FALSE(ok(save.run(full)));
        // Not an ASSERT: the next save is judged whatever this one did
        QFile after(full.toString());
        EXPECT_TRUE(after.open(QIODevice::ReadOnly) &&
                    after.readAll() == theirs)
            << "a document with bytes stays";
        after.close();
        QFile::remove(replaced.toString());
        QFile::remove(full.toString());
    }
}

// What a layout file names of a vehicle's picks: each one's written file
// by its hash, the file it was made from beside it, and no entry for a
// pick no plugin has
TEST(BridgeTest, OneStoreAPicksFileIsItsWrittenTextAndTheFileThatMadeIt)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    const QByteArray file = signalFile(QStringLiteral("Daily extras"));
    QVariantMap r = b.bridge.keepSignalText(QString::fromUtf8(file), QString(),
                                            QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString extrasSha = str(r, "sha256");
    const QString extras = list(r, "profiles").value(0);
    r = b.bridge.readImport(writeScratch(from.path(), QStringLiteral("a.json"),
                                         packed_plugin::example()),
                            QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString simSha = str(r, "sha256");
    const QString my2019 = QStringLiteral("plugin.example-sim.my2019");

    const QVariantList files = b.bridge.pickFiles(
        {my2019, QStringLiteral("plugin.gone.base"), extras});
    ASSERT_EQ(files.size(), 2) << "a pick no plugin has names no file";
    const QVariantMap sim = files.at(0).toMap();
    const QVariantMap own = files.at(1).toMap();
    EXPECT_EQ(str(sim, "profile"), my2019) << "in the picks' order";
    EXPECT_EQ(str(own, "profile"), extras);
    EXPECT_EQ(str(sim, "name"), "Example: the simulated car: my2019")
        << "one variant of several is named";
    EXPECT_EQ(str(own, "name"), "Daily extras");

    const auto written = [&](const QString& sha, const char* variant) {
        const QVariantMap t =
            b.bridge.pluginVariantText(sha, QString::fromLatin1(variant));
        EXPECT_TRUE(ok(t)) << words(t);
        return sha256Of(str(t, "text").toUtf8());
    };
    EXPECT_EQ(str(own, "sha256"), written(extrasSha, "base"));
    EXPECT_EQ(str(sim, "sha256"), written(simSha, "my2019"));
    EXPECT_NE(str(sim, "sha256"), written(simSha, "base"));
    EXPECT_EQ(list(own, "from"), QStringList{sha256Of(file)})
        << "the signal file the plugin was made from";
    EXPECT_TRUE(list(sim, "from").isEmpty()) << "a package came from no file";
    EXPECT_TRUE(b.bridge.pickFiles({}).isEmpty());
}
