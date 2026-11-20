// File/layer: tests/test_one_store.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// The one store (ADR-0064 step 3, app/pluginstore.{h,cpp} and the
// bridge's calls over it): a signal file and an OBDb set wrapped into
// plugins of the user's own, a package kept as it is, the store's index
// (mine, importedFor, wrappedFrom) and what it does with a file it can't
// trust, the update of a plugin of the user's own by its lineage and
// version, Export plugin's bytes and its share-alike note, a variant's
// signal file, Copy to my plugin, and a vehicle's ordered picks. Every
// case has a settings folder of its own.

#include "test_one_store_support.h"

namespace {

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool disconnectAndWait(SessionBridge& bridge)
{
    bridge.disconnectSession();
    QElapsedTimer clock;
    clock.start();
    while (bridge.disconnecting()) {
        if (clock.elapsed() > 5000) {
            return false;
        }
        spin(10);
    }
    return true;
}

} // namespace

/* A signal file is read, held and kept as a plugin of the user's own:
 * core's wrap of it byte for byte, one variant, registered, with the
 * index saying whose it is, which vehicle it came for and which file it
 * was made from; a later bridge loads it as the user's own */
TEST(BridgeTest, OneStoreASignalFileBecomesAPluginOfTheUsersOwn)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray file = signalFile(QStringLiteral("Daily driver extras"),
                                       QStringLiteral("D100"), true);
    const QString id = QStringLiteral("daily-driver-extras");
    const QString profile = QStringLiteral("plugin.daily-driver-extras.base");
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    {
        Logged b;
        QVariantMap r = b.bridge.readImport(
            writeScratch(from.path(), QStringLiteral("extras.json"), file));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "kind").toStdString(), "signals");
        EXPECT_EQ(str(r, "name").toStdString(), "Daily driver extras");
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_EQ(str(r, "variant"), id) << "the name's slug, within 24";
        EXPECT_TRUE(copies().isEmpty()) << "nothing kept by a read";
        EXPECT_TRUE(b.bridge.plugins().isEmpty());
        EXPECT_FALSE(registered(profile));
        EXPECT_TRUE(b.log.contains(QStringLiteral(
            "Signal file read: Daily driver extras (as a plugin, "
            "daily-driver-extras)")))
            << b.lines();

        r = b.bridge.keepSignals(QString(), QString(), QStringLiteral("v3"));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_EQ(str(r, "name").toStdString(), "Daily driver extras");
        EXPECT_EQ(str(r, "version").toStdString(), "1.0.0");
        EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")});
        EXPECT_EQ(list(r, "profiles"), QStringList{profile});
        EXPECT_EQ(list(r, "extends"), QStringList{QStringLiteral("obd2")});
        EXPECT_EQ(r.value(QStringLiteral("counts")).toList(),
                  QVariantList{static_cast<int>(obd2->count) + 1});
        EXPECT_EQ(r.value(QStringLiteral("owns")).toList(), QVariantList{1});
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor").toStdString(), "v3");
        EXPECT_EQ(str(r, "lineage"), sha256Of(file).left(32));
        EXPECT_FALSE(r.value(QStringLiteral("obdb")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_TRUE(list(r, "replacedProfiles").isEmpty());
        EXPECT_EQ(words(r), "");
        EXPECT_TRUE(registered(profile));

        const QByteArray wrap = coreWrap(file);
        ASSERT_FALSE(wrap.isEmpty());
        EXPECT_EQ(str(r, "sha256"), sha256Of(wrap));
        EXPECT_EQ(copies(), QStringList{sha256Of(wrap) + kSuffix});
        EXPECT_EQ(readBytes(copyPath(sha256Of(wrap))), wrap);

        const QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("format")).toInt(), 1);
        EXPECT_EQ(
            factsOf(id),
            (QJsonObject{
                {QStringLiteral("mine"), true},
                {QStringLiteral("importedFor"), QStringLiteral("v3")},
                {QStringLiteral("wrappedFrom"),
                 QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}));
        EXPECT_TRUE(b.log.contains(
            QStringLiteral("Plugin made: Daily driver extras 1.0.0 (%1)")
                .arg(profile)))
            << b.lines();

        r = b.bridge.keepSignals(QString(), QString(), QString());
        EXPECT_FALSE(ok(r)) << "the import was taken";
        EXPECT_EQ(words(r), "No signal file is waiting: import it again");
    }
    EXPECT_FALSE(registered(profile)) << "freed at close";

    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_TRUE(ok(e));
    EXPECT_TRUE(e.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(e, "importedFor").toStdString(), "v3");
    EXPECT_TRUE(registered(profile));
}

/* The same file again finds its copy; another file whose name gives an
 * id the store has is numbered -2, -3, within the id's 31 bytes and
 * with no '-' left before the number */
TEST(BridgeTest, OneStoreTheSameFileIsKeptOnceAndATakenIdIsNumbered)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const auto keep = [&bridge](const QByteArray& file) {
        return bridge.keepSignalText(QString::fromUtf8(file), QString(),
                                     QString(), QString());
    };
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    QVariantMap r = keep(a);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString first = str(r, "sha256");
    EXPECT_EQ(str(r, "id").toStdString(), "extras");
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());

    r = keep(a);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), first);
    EXPECT_EQ(copies().size(), 1);
    EXPECT_EQ(bridge.plugins().size(), 1);

    const QByteArray b =
        signalFile(QStringLiteral("Extras"), QStringLiteral("D101"));
    r = keep(b);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "extras-2");
    EXPECT_EQ(str(r, "name").toStdString(), "Extras");
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(registered(QStringLiteral("plugin.extras-2.base")));
    const QString second = str(r, "sha256");

    r = keep(b);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool())
        << "found under its number";
    EXPECT_EQ(str(r, "sha256"), second);
    EXPECT_EQ(words(r), "") << "unchanged since: nothing to say";
    EXPECT_EQ(madeFrom(QStringLiteral("extras-2")),
              (QJsonObject{{sha256Of(b), QStringLiteral("base")}}));

    r = keep(signalFile(QStringLiteral("Extras"), QStringLiteral("D102")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "extras-3");
    EXPECT_EQ(column(bridge.plugins(), "id"),
              (QStringList{QStringLiteral("extras"), QStringLiteral("extras-2"),
                           QStringLiteral("extras-3")}))
        << "plugins of one name are listed by id";

    // An id of 31 bytes gives up two for its number
    const QString full = QStringLiteral("abcdefghij abcdefghij abcdefghi");
    r = keep(signalFile(full));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdefghi");
    r = keep(signalFile(full, QStringLiteral("D101")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdefg-2");

    // A cut that ends on a '-' drops it before the number
    const QString dashed = QStringLiteral("abcdefghij abcdefghij abcdef hij");
    r = keep(signalFile(dashed));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdef-hi");
    r = keep(signalFile(dashed, QStringLiteral("D101")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdef-2");
}

/* A read that asks for a signal file (Replace's) keeps and holds nothing
 * of another kind: a package doesn't go in, an OBDb set isn't held */
TEST(BridgeTest, OneStoreAReadForASignalFileTakesNothingElse)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    const QString only = QStringLiteral("signals");
    const auto read = [&](const char* name, const QByteArray& bytes) {
        return b.bridge.readImport(
            writeScratch(from.path(), QString::fromLatin1(name), bytes), only);
    };

    QVariantMap r = read("a.json", packed_plugin::example());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Not a signal file: only one takes a variant's place");
    EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
    EXPECT_TRUE(copies().isEmpty()) << "the package didn't go in";
    EXPECT_EQ(b.log.constLast().toStdString(),
              "Import refused: Not a signal file: only one takes a variant's "
              "place");

    r = read("set.json", readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE)));
    EXPECT_FALSE(ok(r));
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Held"));
    EXPECT_FALSE(ok(r)) << "the set isn't held";

    r = read("file.json", signalFile(QStringLiteral("Extras")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_TRUE(ok(r)) << words(r) << " (a signal file waits as ever)";
}

/* One Import tells the three kinds apart by the file's first key: a
 * package goes in, a signal file waits, an OBDb set is left to its
 * report; anything else, and a file a loader refuses, is refused in
 * words that never name the path */
TEST(BridgeTest, OneStoreAnImportTellsItsKindByItsFirstKey)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    const auto read = [&](const char* name, const QByteArray& bytes) {
        return b.bridge.readImport(
            writeScratch(from.path(), QString::fromLatin1(name), bytes));
    };

    QVariantMap r = read("a.json", packed_plugin::example());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
    EXPECT_EQ(str(r, "id").toStdString(), "example-sim");
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool())
        << "a package is someone else's until it is marked";
    EXPECT_EQ(str(r, "updated"), QString());
    EXPECT_EQ(copies().size(), 1);
    EXPECT_FALSE(QFile::exists(indexPath())) << "a package leaves no fact";

    // An OBDb set is held, read once: its report converts the held
    // bytes, as often as it is asked, with the file gone
    const QByteArray set = readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE));
    ASSERT_FALSE(set.isEmpty());
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Early"));
    EXPECT_FALSE(ok(r)) << "nothing read yet";
    EXPECT_EQ(words(r), "No OBDb set is waiting: import it again");
    const QUrl setUrl =
        writeScratch(from.path(), QStringLiteral("set.json"), set);
    r = b.bridge.readImport(setUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    ASSERT_TRUE(QFile::remove(setUrl.toLocalFile()));
    EXPECT_EQ(copies().size(), 1) << "an OBDb set shows its report first";
    r = b.bridge.keepObdbPlugin(QStringLiteral("v1"), false);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "No OBDb set is waiting: import it again");
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "the file held is no signal file";
    EXPECT_EQ(words(r), "No signal file is waiting: import it again");
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Held OBDb"));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.previewObdbImport(2020, QStringLiteral("Held OBDb"));
    ASSERT_TRUE(ok(r)) << words(r) << " (another year, the same bytes)";
    r = b.bridge.keepObdbPlugin(QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2020")});
    const QString held = str(r, "sha256");
    r = b.bridge.previewObdbImport(2020, QStringLiteral("Held OBDb"));
    EXPECT_FALSE(ok(r)) << "kept: the import is done";
    ASSERT_TRUE(ok(b.bridge.removePlugin(held)));
    ASSERT_EQ(copies().size(), 1);

    // A new read forgets what the last one held
    r = b.bridge.readImport(
        writeScratch(from.path(), QStringLiteral("set.json"), set));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("none.json"), QByteArray("{}")));
    EXPECT_FALSE(ok(r));
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Held OBDb"));
    EXPECT_FALSE(ok(r));

    // An editor's byte-order mark is passed by, for each kind
    const QByteArray mark("\xEF\xBB\xBF");
    const QByteArray marked = signalFile(QStringLiteral("Marked"));
    r = read("bom.json", mark + marked);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    r = b.bridge.keepSignals(QString(), QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "sha256"), sha256Of(coreWrap(marked)))
        << "the text without its mark is what was wrapped";
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    r = read("bom-plugin.json",
             mark + coreWrap(signalFile(QStringLiteral("Marked pack"))));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    r = read("bom-set.json", mark + set);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    b.bridge.dropImport();
    ASSERT_EQ(copies().size(), 1);

    // Leading space is passed by; the first key decides
    r = read("b.json", " \r\n\t" + signalFile(QStringLiteral("Spaced")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    b.bridge.dropImport();
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "a dropped import is gone";

    const QByteArray neither[] = {
        R"({"a":1})",
        "not json",
        "",
        "[1]",
        R"({"commands_":[]})",
        // The format key must come first
        R"({"name":"x","havne_signals":1,"source":{"name":"t"},"signals":[]})",
        R"({"name":"x","havne_plugin":1})",
    };
    for (const QByteArray& bytes : neither) {
        r = read("c.json", bytes);
        EXPECT_FALSE(ok(r)) << bytes.toStdString();
        EXPECT_EQ(str(r, "kind"), QString()) << bytes.toStdString();
        EXPECT_EQ(words(r), "The file is neither a plugin, a signal file "
                            "nor an OBDb signal set")
            << bytes.toStdString();
    }
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "Import refused: The file is neither a plugin, a signal file nor "
        "an OBDb signal set")))
        << b.lines();

    // A signal file the loader refuses: its words, and nothing waiting
    r = read("broken-name.json",
             signalFile(QStringLiteral("Broken"))
                 .replace("\"source\"", "\"scael\":1,\"source\""));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    const QString why = str(r, "words");
    EXPECT_TRUE(why.contains(QStringLiteral("line"))) << why.toStdString();
    EXPECT_TRUE(why.contains(QStringLiteral("scael"))) << why.toStdString();
    EXPECT_FALSE(why.contains(QStringLiteral("broken-name")))
        << why.toStdString();
    EXPECT_FALSE(why.contains(from.path())) << why.toStdString();
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "a refused file isn't held";

    // Past a signal file's bound, and past a plugin's
    QByteArray large = signalFile(QStringLiteral("Large"));
    large.insert(1, QByteArray(ECUIO_SIGNAL_FILE_MAX_BYTES, ' '));
    r = read("large.json", large);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 1024 KiB, a signal file's most");
    r = read("huge.json", QByteArray(ECUIO_PLUGIN_PACKAGE_MAX + 1, ' '));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 16 MiB, a plugin's most");

    // An OBDb set at its bound is read; one byte past it is refused as
    // what it is, by the import and by the report's own read
    QByteArray most = set;
    most.insert(1, QByteArray(ECUIO_OBDB_MAX_BYTES - set.size(), ' '));
    ASSERT_EQ(most.size(), static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES));
    r = read("most.json", most);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Most OBDb"));
    EXPECT_TRUE(ok(r)) << words(r);
    most.insert(1, ' ');
    r = read("over.json", most);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    EXPECT_EQ(words(r), "The file is over 1 MiB, an OBDb set's most");
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "OBDb set refused: The file is over 1 MiB, an OBDb set's most")))
        << b.lines();
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Most OBDb"));
    EXPECT_FALSE(ok(r)) << "a refused file isn't held";
    r = b.bridge.previewObdb(
        writeScratch(from.path(), QStringLiteral("over.json"), most), 2019,
        QStringLiteral("Most OBDb"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 1 MiB, an OBDb set's most");

    r = b.bridge.readImport(
        QUrl::fromLocalFile(from.path() + QStringLiteral("/missing.json")));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't read the file");
    EXPECT_EQ(str(r, "kind"), QString());

    EXPECT_EQ(copies().size(), 1) << "only the package was kept";
    for (const QString& line : std::as_const(b.log)) {
        EXPECT_FALSE(line.contains(from.path())) << line.toStdString();
    }
}

/* The import's one read opens a picked file as every other does, by the
 * URL's encoded text (pathOfUrl): a provider's document named with an
 * escape in it (Android's "signals (1).json" arrives as one) is read by
 * that name, not by the decoded one ("a b.json") toString() gives. A URL with no
 * scheme stands in for a provider's, which a desktop can't have */
TEST(BridgeTest, OneStoreAPickedFileIsOpenedByItsEncodedName)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    const QUrl made = writeScratch(from.path(), QStringLiteral("a%20b.json"),
                                   packed_plugin::example());
    ASSERT_FALSE(made.isEmpty());
    const QUrl picked =
        QUrl::fromEncoded(made.toLocalFile().toUtf8(), QUrl::TolerantMode);
    ASSERT_FALSE(picked.isLocalFile());
    if (picked.toString(QUrl::FullyEncoded)
            .compare(made.toLocalFile(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "a temporary path a URL spells differently (a space "
                        "or a non-ASCII character in it)";
    }

    const QVariantMap r = b.bridge.readImport(picked);
    EXPECT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(str(r, "name").isEmpty()) << "the package was read";
}

/* An OBDb set converted and kept is a plugin of the user's own, imported
 * for its vehicle; its variant is my<year> only for a year the user
 * typed, base for one decoded from a VIN or for none */
TEST(BridgeTest, OneStoreAnObdbSetIsWrappedAndOnlyATypedYearNamesItsVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    QVariantMap r =
        keepObdb(b.bridge, 2019, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "version").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2019")});
    EXPECT_EQ(list(r, "profiles"),
              QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019")});
    EXPECT_EQ(list(r, "extends"), QStringList{QStringLiteral("obd2")});
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("obdb")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v1");
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin from OBDb: Daily driver OBDb 1.0.0 "
                       "(plugin.daily-driver-obdb.my2019)")))
        << b.lines();
    EXPECT_FALSE(ok(b.bridge.keepObdbPlugin(QStringLiteral("v1"), true)))
        << "the set was taken";

    r = keepObdb(b.bridge, 2019, QStringLiteral("Weekend car OBDb"),
                 QStringLiteral("v2"), false);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")})
        << "a year decoded from a VIN names nothing";
    EXPECT_FALSE(readBytes(copyPath(str(r, "sha256"))).contains("my2019"));

    r = keepObdb(b.bridge, 0, QStringLiteral("Third OBDb"),
                 QStringLiteral("v3"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")})
        << "no year, no name";
}

/* An OBDb set imported again for its vehicle takes the place of that
 * vehicle's earlier conversion of the same name, and of nothing else:
 * not another vehicle's, not a file the user wrote; while a session
 * decodes with the earlier one it waits */
TEST(BridgeTest, OneStoreAnObdbReimportTakesItsVehiclesEarlierImportsPlace)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QVariantMap first = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(first)) << words(first);

    QVariantMap r = keepObdb(b.bridge, 2021, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString second = str(r, "sha256");
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "replacedProfiles"),
              QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019")})
        << "what a pick of the earlier year is moved by";
    EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2021")});
    EXPECT_EQ(copies(), QStringList{second + kSuffix});
    EXPECT_EQ(b.bridge.plugins().size(), 1);
    EXPECT_FALSE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2021")));
    const QJsonObject facts = factsOf(QStringLiteral("daily-driver-obdb"));
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb")).size(), 1)
        << "the conversion it holds, not the one that went";
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb"))
                  .begin()
                  .value()
                  .toString()
                  .toStdString(),
              "my2021");
    EXPECT_EQ(facts.value(QStringLiteral("importedFor")).toString(), v1);
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "Plugin from OBDb: Daily driver OBDb 1.0.0 "
        "(plugin.daily-driver-obdb.my2021), in the earlier import's place")))
        << b.lines();

    // The same bytes for another vehicle: the copy is here, and stays
    // the first vehicle's
    r = keepObdb(b.bridge, 2021, name, QStringLiteral("v3"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), second);
    EXPECT_EQ(str(r, "importedFor"), v1);

    // Another vehicle's set of that name is a plugin of its own
    r = keepObdb(b.bridge, 2020, name, QStringLiteral("v2"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-2");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v2");
    EXPECT_EQ(b.bridge.plugins().size(), 2);

    // The first vehicle's again: its own goes, the other stays
    r = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(b.bridge.plugins().size(), 2);
    EXPECT_TRUE(
        registered(QStringLiteral("plugin.daily-driver-obdb-2.my2020")));

    // A file the user wrote under that name replaces nothing
    r = b.bridge.keepSignalText(QString::fromUtf8(signalFile(name)), QString(),
                                QString(), v1);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-3");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_FALSE(r.value(QStringLiteral("obdb")).toBool());
    EXPECT_EQ(b.bridge.plugins().size(), 3);

    // The vehicle's set under another name is another plugin
    r = keepObdb(b.bridge, 2019, QStringLiteral("Second set OBDb"), v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "second-set-obdb");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(b.bridge.plugins().size(), 4);
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));

    // A file the user wrote and kept for a vehicle is never taken for
    // that vehicle's earlier conversion
    const QString solo = QStringLiteral("Solo OBDb");
    r = b.bridge.keepSignalText(QString::fromUtf8(signalFile(solo)), QString(),
                                QString(), QStringLiteral("v5"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString written = str(r, "sha256");
    r = keepObdb(b.bridge, 2019, solo, QStringLiteral("v5"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "solo-obdb-2");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_TRUE(QFile::exists(copyPath(written)));
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    ASSERT_TRUE(ok(b.bridge.removePlugin(written)));
    EXPECT_EQ(b.bridge.plugins().size(), 3);

    // A session on the earlier import: the new one waits, still pending
    ASSERT_TRUE(b.bridge
                    .connectSession(QStringLiteral(
                        "mock://obdb?profile=plugin.daily-driver-obdb.my2019"))
                    .isEmpty());
    r = keepObdb(b.bridge, 2021, name, v1, true);
    EXPECT_FALSE(ok(r));
    EXPECT_TRUE(r.value(QStringLiteral("inUse")).toBool());
    EXPECT_EQ(words(r), "A session decodes with Daily driver OBDb 1.0.0: "
                        "disconnect, then import it again");
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_EQ(b.bridge.plugins().size(), 3);
    ASSERT_TRUE(disconnectAndWait(b.bridge));
    r = b.bridge.keepObdbPlugin(v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2021")});
    EXPECT_EQ(b.bridge.plugins().size(), 3);
}

/* Only a plugin of the user's own takes a new, changed or removed
 * variant. A package becomes one by Mine; each change raises the minor,
 * replaces the kept copy, and says when an ECU of the file's took
 * another id; the last variant never goes */
TEST(BridgeTest, OneStoreOnlyAPluginOfTheUsersOwnTakesAVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    QVariantMap r = b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("a.json"), packed_plugin::example()));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    const QString extra = QStringLiteral("plugin.example-sim.extra");

    // The file's ECU is named as the plugin's pcm, at another address
    const QByteArray file =
        signalFile(QStringLiteral("Extra file"), QStringLiteral("D150"), false,
                   QStringLiteral("7EA"));
    r = b.bridge.readImport(
        writeScratch(from.path(), QStringLiteral("extra.json"), file));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.keepSignals(theirs, QStringLiteral("extra"), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Example: the simulated car isn't a plugin of your "
                        "own: only your own plugins are changed");
    r = b.bridge.dropPluginVariant(theirs, QStringLiteral("my2019"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Example: the simulated car isn't a plugin of your "
                        "own: only your own plugins are changed");
    EXPECT_EQ(copies(), QStringList{theirs + kSuffix}) << "nothing changed";
    EXPECT_TRUE(registered(QStringLiteral("plugin.example-sim.my2019")));

    r = b.bridge.markPluginMine(QString(64, QLatin1Char('0')));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    r = b.bridge.markPluginMine(theirs);
    ASSERT_TRUE(ok(r));
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(b.bridge.plugins()
                    .at(0)
                    .toMap()
                    .value(QStringLiteral("mine"))
                    .toBool());
    EXPECT_EQ(factsOf(QStringLiteral("example-sim")),
              (QJsonObject{{QStringLiteral("mine"), true}}));

    // A variant id core refuses leaves the file waiting
    r = b.bridge.keepSignals(theirs, QStringLiteral("Bad Id"), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());

    r = b.bridge.keepSignals(theirs, QStringLiteral("extra"), QString());
    ASSERT_TRUE(ok(r)) << words(r) << " (the refusals left it pending)";
    const QString second = str(r, "sha256");
    EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("my2019"),
                           QStringLiteral("extra")}));
    EXPECT_TRUE(str(r, "notes").contains(QStringLiteral("pcm_2")))
        << str(r, "notes").toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(registered(extra));
    EXPECT_EQ(copies(), QStringList{second + kSuffix});
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(file), QStringLiteral("extra")}}));
    EXPECT_TRUE(b.log.last().startsWith(
        QStringLiteral("Plugin changed: Example: the simulated car 1.1.0 "
                       "(plugin.example-sim.base, plugin.example-sim.my2019, "
                       "plugin.example-sim.extra); ")))
        << b.lines();

    // The same id again: put in its place, no note for a shared ECU,
    // and the file that variant was made from before is named no more
    const QByteArray other =
        signalFile(QStringLiteral("Other"), QStringLiteral("D151"));
    r = b.bridge.keepSignalText(QString::fromUtf8(other), second,
                                QStringLiteral("extra"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.2.0");
    EXPECT_EQ(list(r, "variants").size(), 3);
    EXPECT_EQ(str(r, "notes"), QString());
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(other), QStringLiteral("extra")}}));

    // No id given: the file's name as core cuts it
    const QByteArray track =
        signalFile(QStringLiteral("Track day"), QStringLiteral("D152"));
    r = b.bridge.keepSignalText(QString::fromUtf8(track), str(r, "sha256"),
                                QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
    EXPECT_EQ(list(r, "variants").last().toStdString(), "track-day");
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(other), QStringLiteral("extra")},
                           {sha256Of(track), QStringLiteral("track-day")}}))
        << "the index names the id core gave the variant";
    const QString fourth = str(r, "sha256");

    r = b.bridge.dropPluginVariant(fourth, QStringLiteral("nothing"));
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());
    EXPECT_EQ(copies(), QStringList{fourth + kSuffix});

    r = b.bridge.dropPluginVariant(fourth, QStringLiteral("extra"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.4.0");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("my2019"),
                           QStringLiteral("track-day")}));
    EXPECT_FALSE(registered(extra));
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(copies(), QStringList{str(r, "sha256") + kSuffix});
    EXPECT_EQ(list(r, "replacedProfiles").size(), 4);
    EXPECT_TRUE(list(r, "replacedProfiles").contains(extra));
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(track), QStringLiteral("track-day")}}))
        << "a variant that went takes its file's name with it";

    // A plugin's last variant stays
    r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Only one"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString one = str(r, "sha256");
    r = b.bridge.dropPluginVariant(one, QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());
    EXPECT_TRUE(registered(QStringLiteral("plugin.only-one.base")));
    EXPECT_TRUE(QFile::exists(copyPath(one)));
}
