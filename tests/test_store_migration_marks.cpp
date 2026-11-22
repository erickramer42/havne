// File/layer: tests/test_store_migration_marks.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_store_migration_support.h"

/* Two names whose ids are cut to the same text, and a file whose id a
 * third-party plugin holds: the later ones are numbered, and a run
 * stopped half way and started again gives the same ids */
TEST(BridgeTest, OneStoreMigrationIdsThatClashAreNumberedTheSameEachRun)
{
    const QByteArray one =
        signalFile(QStringLiteral("A very long signal file name number one"));
    const QByteArray two =
        signalFile(QStringLiteral("A very long signal file name number two"),
                   QStringLiteral("D101"));
    const QByteArray sim =
        signalFile(QStringLiteral("Example sim"), QStringLiteral("D102"));
    const QString id =
        PluginStore::describe(one).value(QStringLiteral("id")).toString();
    ASSERT_EQ(id.size(), qsizetype{ECUIO_PLUGIN_ID_MAX});
    ASSERT_EQ(PluginStore::describe(two).value(QStringLiteral("id")).toString(),
              id);
    ASSERT_NE(profileOf(listed(one)), profileOf(listed(two)))
        << "the old book holds both";

    const auto stage = [&](const OldState& old) {
        old.keep(one);
        old.keep(two);
        old.keep(sim);
        old.set(kOldBook,
                bookOf({vehicle(QStringLiteral("v1"),
                                QStringLiteral("elm327://COM5?profile=") +
                                    profileOf(listed(two)),
                                {listed(one), listed(two), listed(sim)})}));
        PluginStore store;
        ASSERT_TRUE(store.import(packed_plugin::example())
                        .value(QStringLiteral("ok"))
                        .toBool());
    };
    QStringList whole;
    {
        OldState old;
        ASSERT_TRUE(old.valid());
        stage(old);
        ASSERT_TRUE(old.start().marked);
        whole = old.picks(0);
        ASSERT_EQ(whole.size(), 3);
        EXPECT_EQ(whole.at(0), QStringLiteral("plugin.%1.base").arg(id));
        EXPECT_TRUE(whole.at(1).endsWith(QStringLiteral("-2.base")))
            << whole.at(1).toStdString();
        EXPECT_NE(whole.at(1), whole.at(0));
        EXPECT_EQ(whole.at(2), QStringLiteral("plugin.example-sim-2.base"));
        EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")).toString(),
                  QStringLiteral("elm327://COM5?profile=") + whole.at(1))
            << "the address names the plugin its file became";
    }
    for (int file = 1; file <= 2; ++file) {
        OldState old;
        ASSERT_TRUE(old.valid());
        stage(old);
        StoreMigration::stopAfterForTesting(Step::File, file);
        ASSERT_TRUE(old.start().stopped);
        const Report r = old.start();
        ASSERT_TRUE(r.marked);
        EXPECT_EQ(r.kept, file);
        EXPECT_EQ(r.made, 3 - file);
        EXPECT_EQ(old.picks(0), whole) << "stopped after file " << file;
        EXPECT_EQ(copies().size(), 4);
    }
}

/* An address changes only when the profile core would read from it, the
 * last profile=, is a moved file's; its other keys stay in their order */
TEST(BridgeTest, OneStoreMigrationAddressesNameTheNewProfiles)
{
    const QByteArray moved = signalFile(QStringLiteral("P"));
    const QString gone = sha256Of("never kept");
    struct Row {
        const char* was;
        const char* is;
    };
    const Row rows[] = {
        {"elm327://COM5?profile=user.p", "elm327://COM5?profile=plugin.p.base"},
        {"elm327://COM5?profile=user.p&did=F190",
         "elm327://COM5?did=F190&profile=plugin.p.base"},
        {"elm327://COM5?baud=38400&profile=obd2&profile=user.p&identify=0",
         "elm327://COM5?baud=38400&identify=0&profile=plugin.p.base"},
        {"elm327://COM5?profile=user.p&profile=obd2",
         "elm327://COM5?profile=user.p&profile=obd2"},
        {"elm327://COM5?profile=obd2", "elm327://COM5?profile=obd2"},
        {"elm327://COM5?profile=plugin.x.y",
         "elm327://COM5?profile=plugin.x.y"},
        {"elm327://COM5?profile=user.other",
         "elm327://COM5?profile=user.other"},
        {"elm327://COM5?profile=user.gone", "elm327://COM5?profile=user.gone"},
        {"elm327://COM5?myprofile=user.p", "elm327://COM5?myprofile=user.p"},
        {"mock://demo", "mock://demo"},
        {"elm327://COM5?profile=", "elm327://COM5?profile="},
        {"", ""},
    };
    const int changed = 3;

    OldState old;
    ASSERT_TRUE(old.valid());
    old.keep(moved);
    const QByteArray nameless =
        signalFile(QStringLiteral("Q"), QStringLiteral("D101"));
    old.keep(nameless);
    ASSERT_EQ(profileOf(listed(moved)), QStringLiteral("user.p"));
    QJsonArray vehicles;
    QJsonArray recent;
    int n = 0;
    for (const Row& row : rows) {
        const QJsonArray files =
            n == 0 ? QJsonArray{listed(moved),
                                named(gone, QStringLiteral("Gone"),
                                      QStringLiteral("user.gone")),
                                named(sha256Of(nameless), QStringLiteral("Q"),
                                      QString())}
                   : QJsonArray();
        vehicles.append(vehicle(QStringLiteral("v%1").arg(++n),
                                QString::fromLatin1(row.was), files));
        recent.append(QString::fromLatin1(row.was));
    }
    old.set(kOldBook, bookOf(vehicles));
    old.set(kRecentUris, compact(recent));
    old.set(kUri, QString::fromLatin1(rows[1].was));

    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.addresses, changed + 1);
    EXPECT_EQ(old.text(kUri), QString::fromLatin1(rows[1].is));
    const QJsonArray now =
        QJsonDocument::fromJson(old.text(kRecentUris).toUtf8()).array();
    ASSERT_EQ(now.size(), n);
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(now.at(i).toString().toStdString(), rows[i].is)
            << "recent " << rows[i].was;
        EXPECT_EQ(
            old.entry(i).value(QStringLiteral("uri")).toString().toStdString(),
            rows[i].is)
            << "vehicle " << rows[i].was;
    }

    // A recent list that isn't an array of text is left byte for byte,
    // and nothing else is given up for it
    for (const QString& damaged :
         {QStringLiteral("[\"elm327://COM5?profile=user.p\",5]"),
          QStringLiteral("{\"0\":\"elm327://COM5?profile=user.p\"}"),
          QStringLiteral("[\"elm327://COM5?profile=user.p\"")}) {
        OldState other;
        ASSERT_TRUE(other.valid());
        other.keep(moved);
        other.set(kOldBook, bookOf({vehicle(QStringLiteral("v1"), QString(),
                                            {listed(moved)})}));
        other.set(kRecentUris, damaged);
        other.set(kUri, QString::fromLatin1(rows[0].was));
        const Report d = other.start();
        EXPECT_TRUE(d.marked);
        EXPECT_EQ(d.addresses, 1);
        EXPECT_EQ(other.text(kRecentUris).toStdString(), damaged.toStdString());
        EXPECT_EQ(other.text(kUri), QString::fromLatin1(rows[0].is));
    }
}

/* A book an older Havne shows as no vehicles: the old key stays, the new
 * book has no vehicle, and every stored file that loads becomes a plugin
 * of the user's own that no vehicle picks. One that doesn't load stays
 * in the old folder and the book says so, so its removal spares it. A
 * saved address naming a moved file by the profile the file itself makes
 * names its plugin */
TEST(BridgeTest, OneStoreMigrationABookThatDoesntParseLosesNothing)
{
    const QByteArray one = signalFile(QStringLiteral("One"));
    const QByteArray two =
        signalFile(QStringLiteral("Two"), QStringLiteral("D101"));
    const QByteArray bad = "{\"havne_signals\":1,\"name\":";
    const QString address = QStringLiteral("elm327://COM5?profile=user.one");
    const QString renamed =
        QStringLiteral("elm327://COM5?profile=plugin.one.base");
    ASSERT_EQ(profileOf(listed(one)), QStringLiteral("user.one"));
    for (const QString& unreadable :
         {QStringLiteral("{\"salt\":\"s\",\"picked\":\"\",\"list\":5}"),
          QStringLiteral("{\"salt\":\"s\",\"list\":["), QStringLiteral("[]")}) {
        OldState old;
        ASSERT_TRUE(old.valid());
        old.keep(one);
        old.keep(two);
        const QString badSha = old.keep(bad);
        old.set(kOldBook, unreadable);
        old.set(kUri, address);
        old.set(kRecentUris,
                compact({address, QStringLiteral("elm327://COM5?profile="
                                                 "user.three")}));
        const QMap<QString, QString> before = filesUnder(old.signalsRoot());

        const Report r = old.start();
        EXPECT_TRUE(r.marked) << said(r);
        EXPECT_EQ(r.made, 2);
        EXPECT_EQ(r.unmigrated, 0);
        EXPECT_EQ(r.unnamed, 1) << "the one that doesn't load";
        EXPECT_EQ(r.addresses, 2);
        EXPECT_EQ(old.text(kUri), renamed);
        EXPECT_EQ(old.text(kRecentUris),
                  compact({renamed, QStringLiteral("elm327://COM5?profile="
                                                   "user.three")}));
        EXPECT_TRUE(r.lines.contains(QStringLiteral(
            "The vehicle list couldn't be read: every stored signal file "
            "that loads is now a plugin of your own, picked by no vehicle")))
            << said(r);
        EXPECT_TRUE(r.lines.contains(
            QStringLiteral("Signal files left in the old folder: 1")))
            << said(r);
        EXPECT_FALSE(r.lines.contains(kNotKept)) << said(r);
        EXPECT_EQ(old.text(kOldBook).toStdString(), unreadable.toStdString());
        const QJsonObject book = old.book();
        EXPECT_EQ(book.size(), 5) << old.text(kBook).toStdString();
        EXPECT_EQ(book.value(QStringLiteral("format")).toInt(), 2);
        EXPECT_EQ(book.value(QStringLiteral("list")), QJsonValue(QJsonArray()));
        EXPECT_EQ(book.value(QStringLiteral("picked")), QJsonValue(QString()));
        EXPECT_EQ(book.value(QStringLiteral("salt")), QJsonValue(QString()));
        const QJsonArray loose =
            book.value(QStringLiteral("unmigrated")).toArray();
        ASSERT_EQ(loose.size(), 1);
        const QJsonObject left = loose.at(0).toObject();
        EXPECT_EQ(left.size(), 4);
        EXPECT_EQ(left.value(QStringLiteral("sha256")).toString(), badSha);
        EXPECT_EQ(left.value(QStringLiteral("name")), QJsonValue(QString()));
        EXPECT_TRUE(left.value(QStringLiteral("words"))
                        .toString()
                        .startsWith(QStringLiteral("line 1, column ")));
        EXPECT_EQ(left.value(QStringLiteral("kept")), QJsonValue(false));
        EXPECT_EQ(filesUnder(old.signalsRoot()), before);
        EXPECT_FALSE(QDir(StoreMigration::unmigratedRoot()).exists())
            << "its bytes stay in the old folder";
        EXPECT_EQ(copies().size(), 2);
        for (const char* id : {"one", "two"}) {
            const QJsonObject facts = factsOf(QString::fromLatin1(id));
            EXPECT_TRUE(facts.value(QStringLiteral("mine")).toBool()) << id;
            EXPECT_FALSE(facts.contains(QStringLiteral("importedFor"))) << id;
        }
        EXPECT_TRUE(QFile::exists(old.copyOf(badSha)));
    }

    // A start that only finishes the addresses reads the copies again
    OldState old;
    ASSERT_TRUE(old.valid());
    old.keep(one);
    old.set(kOldBook, QStringLiteral("[]"));
    old.set(kUri, address);
    StoreMigration::stopAfterForTesting(Step::Book);
    ASSERT_TRUE(old.start().stopped);
    ASSERT_EQ(old.text(kUri), address);
    const Report r = old.start();
    EXPECT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.addresses, 1);
    EXPECT_EQ(old.text(kUri), renamed);
}

/* A new install: an empty book and the mark, so the next start reads
 * one file. Copies no vehicle names are counted and left */
TEST(BridgeTest, OneStoreMigrationANewInstallIsMarkedAndMakesNoPlugin)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const Report r = old.start();
    EXPECT_TRUE(r.ran);
    EXPECT_TRUE(r.marked);
    EXPECT_TRUE(r.lines.isEmpty()) << said(r);
    EXPECT_EQ(r.made + r.kept + r.unmigrated + r.unnamed + r.addresses, 0);
    EXPECT_EQ(old.text(kBook),
              QStringLiteral("{\"format\":2,\"list\":[],\"picked\":\"\","
                             "\"salt\":\"\"}"));
    EXPECT_FALSE(old.has(kOldBook));
    EXPECT_FALSE(old.has(kUri));
    EXPECT_FALSE(old.has(kRecentUris));
    EXPECT_EQ(filesUnder(PluginStore::root()).keys(),
              QStringList{QStringLiteral("store.json")});
    EXPECT_FALSE(QDir(old.signalsRoot()).exists());
    EXPECT_FALSE(old.start().ran);

    // An empty book beside copies whose delete never finished
    OldState strays;
    ASSERT_TRUE(strays.valid());
    strays.keep(signalFile(QStringLiteral("Stray")));
    strays.keep(signalFile(QStringLiteral("Other"), QStringLiteral("D101")));
    const QByteArray kept = signalFile(QStringLiteral("Kept"));
    strays.keep(kept);
    writeBytes(strays.signalsRoot() + QStringLiteral("/abc123.json"), "{}");
    strays.set(kOldBook, bookOf({vehicle(QStringLiteral("v1"), QString(),
                                         {listed(kept)})}));
    const QMap<QString, QString> before = filesUnder(strays.signalsRoot());
    const Report s = strays.start();
    EXPECT_TRUE(s.marked);
    EXPECT_EQ(s.made, 1);
    EXPECT_EQ(s.unnamed, 2) << "a file not named by a hash is no copy";
    EXPECT_TRUE(s.lines.contains(
        QStringLiteral("Signal files left in the old folder: 2")))
        << said(s);
    EXPECT_EQ(copies().size(), 1) << "only the file a vehicle names moved";
    EXPECT_EQ(filesUnder(strays.signalsRoot()), before);
    /* Each copy no vehicle names has a record of the book's own, kept
     * false: its bytes are only in the old folder */
    const QJsonArray own =
        strays.book().value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(own.size(), 2) << strays.text(kBook).toStdString();
    for (const auto& v : own) {
        EXPECT_EQ(v.toObject().value(QStringLiteral("kept")),
                  QJsonValue(false));
        EXPECT_EQ(v.toObject().value(QStringLiteral("words")).toString(),
                  QStringLiteral("No vehicle lists it"));
    }
}

/* The dashboards' promise: a moved file's rows keep their references,
 * names and units in their order, so a cell bound before reads after */
TEST(BridgeTest, OneStoreMigrationEveryRowKeepsItsRef)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray file =
        readBytes(corpus(QStringLiteral("signals/daily-driver-obdb.json")));
    ASSERT_FALSE(file.isEmpty());
    ASSERT_EQ(QString::fromLatin1(
                  QCryptographicHash::hash(file, QCryptographicHash::Sha256)
                      .toHex()),
              kObdbSha);
    // As an older Havne kept and read it: its copy in signals/, loaded and
    // registered under the file's own profile
    writeBytes(old.signalsRoot() + QLatin1Char('/') + kObdbSha +
                   QStringLiteral(".json"),
               file);
    struct Older {
        ecuio_signal_file_t* file = nullptr;
        ~Older()
        {
            (void)ecuio_profile_unregister("user.daily-driver-obdb");
            ecuio_signal_file_free(file);
        }
    } older;
    ASSERT_EQ(ecuio_signal_file_load(file.constData(),
                                     static_cast<size_t>(file.size()),
                                     &older.file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_STREQ(ecuio_signal_file_profile(older.file),
                 "user.daily-driver-obdb");
    ASSERT_EQ(ecuio_profile_register("user.daily-driver-obdb",
                                     ecuio_signal_file_set(older.file), nullptr),
              ECUIO_OK);
    SessionBridge bridge;
    QVariantList before = bridge.profileSignals(
        QStringLiteral("mock://?profile=user.daily-driver-obdb"));
    ASSERT_GT(before.size(), 7) << "obd2's rows and the file's own";
    // Its refs by ADR-0057's rule, which the bridge gives only a file it
    // holds: a def of the file's own by its key, the base's by its ID
    const ecuio_signal_set_t* set = ecuio_signal_file_set(older.file);
    ASSERT_EQ(before.size(), static_cast<qsizetype>(set->count));
    for (qsizetype i = 0; i < before.size(); ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        const char* key = ecuio_signal_file_key(older.file, &def);
        QVariantMap row = before.at(i).toMap();
        row.insert(QStringLiteral("ref"),
                   key != nullptr && key[0] != '\0'
                       ? QStringLiteral("key:") + QString::fromUtf8(key)
                       : QStringLiteral("id:%1").arg(def.id));
        before[i] = row;
    }

    old.set(
        kOldBook,
        bookOf({vehicle(QStringLiteral("v1"), QString(),
                        {named(kObdbSha, QStringLiteral("Daily driver OBDb"),
                               QStringLiteral("user.daily-driver-obdb"))})}));
    QStringList lines;
    {
        QSettings prefs(old.prefsPath(), QSettings::IniFormat);
        lines = bridge.migrateStores(prefs);
    }
    EXPECT_EQ(lines,
              QStringList{QStringLiteral(
                  "Signal files moved into the plugin store: 1 made into "
                  "plugins, 0 there already")});
    ASSERT_EQ(old.picks(0),
              QStringList{QStringLiteral("plugin.daily-driver-obdb.base")});
    const QVariantList after = bridge.profileSignals(
        QStringLiteral("mock://?profile=plugin.daily-driver-obdb.base"));
    ASSERT_EQ(after.size(), before.size());
    int own = 0;
    for (qsizetype i = 0; i < before.size(); ++i) {
        const QVariantMap was = before.at(i).toMap();
        const QVariantMap is = after.at(i).toMap();
        for (const char* key : {"ref", "name", "unit", "id"}) {
            EXPECT_EQ(is.value(QString::fromLatin1(key)),
                      was.value(QString::fromLatin1(key)))
                << "row " << i << " " << key;
        }
        EXPECT_FALSE(was.value(QStringLiteral("ref")).toString().isEmpty());
        own += was.value(QStringLiteral("ref"))
                       .toString()
                       .startsWith(QStringLiteral("key:"))
                   ? 1
                   : 0;
    }
    EXPECT_EQ(own, 7) << "the file's own rows are referenced by their keys";
    {
        QSettings prefs(old.prefsPath(), QSettings::IniFormat);
        EXPECT_EQ(bridge.migrateStores(prefs),
                  (QStringList{QStringLiteral("The earlier version's vehicle "
                                              "list removed"),
                               QStringLiteral("The earlier version's signal "
                                              "files removed: 1")}))
            << "done once; the next start removes the earlier data";
    }
}

/* An index of a later Havne's is never written: the run ends without the
 * mark and says so, and the next start moves nothing again */
TEST(BridgeTest, OneStoreMigrationTheMarkThatCantBeWrittenIsTriedAgain)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray file = signalFile(QStringLiteral("One"));
    old.keep(file);
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"), QString(), {listed(file)})}));
    const QByteArray later = "{\"format\":2,\"plugins\":{}}";
    writeBytes(indexPath(), later);

    const Report r = old.start();
    EXPECT_TRUE(r.ran);
    EXPECT_FALSE(r.marked);
    EXPECT_FALSE(r.stopped);
    EXPECT_EQ(r.made, 1);
    EXPECT_EQ(r.lines.count(kIndexStuck), 1) << said(r);
    EXPECT_EQ(old.picks(0), QStringList{QStringLiteral("plugin.one.base")});
    EXPECT_EQ(readBytes(indexPath()), later);
    const QString book = old.text(kBook);
    const QMap<QString, QString> after = filesUnder(old.path());
    ASSERT_EQ(copies().size(), 1);

    const Report again = old.start();
    EXPECT_TRUE(again.ran);
    EXPECT_FALSE(again.marked);
    EXPECT_EQ(again.made + again.kept, 0);
    EXPECT_EQ(again.lines, QStringList{kIndexStuck}) << said(again);
    EXPECT_EQ(old.text(kBook).toStdString(), book.toStdString());
    EXPECT_EQ(filesUnder(old.path()), after);
}

/* Preferences that can't be written: the run says so and sets no mark,
 * so a later start finishes. Within one process Qt hands the unwritten
 * book to the next QSettings on the file, so the second start here finds
 * it; a new launch finds none and moves the files again */
TEST(BridgeTest, OneStoreMigrationABookThatCantBeSavedLeavesTheMarkUnset)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray file = signalFile(QStringLiteral("One"));
    old.keep(file);
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"), QString(), {listed(file)})}));
    const QFile::Permissions before = QFile::permissions(old.prefsPath());
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(),
                                      QFile::ReadOwner | QFile::ReadUser));
    if (QFileInfo(old.prefsPath()).isWritable()) {
        QFile::setPermissions(old.prefsPath(), before);
        GTEST_SKIP() << "this account writes a read-only file";
    }

    const Report r = old.start();
    EXPECT_FALSE(r.marked);
    EXPECT_FALSE(r.stopped);
    EXPECT_FALSE(marked());
    EXPECT_EQ(r.made, 1);
    EXPECT_TRUE(r.lines.contains(QStringLiteral(
        "The vehicle list couldn't be saved: the move runs again at the "
        "next start")))
        << said(r);
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(), before));

    const Report again = old.start();
    EXPECT_TRUE(again.marked) << said(again);
    EXPECT_EQ(again.made, 0);
    EXPECT_EQ(old.picks(0), QStringList{QStringLiteral("plugin.one.base")});
    EXPECT_EQ(copies().size(), 1);
}

/* A file is imported for the vehicle whose form wrote it, the first of
 * them, whichever vehicle lists it first */
TEST(BridgeTest, OneStoreMigrationAnOwnFileIsItsFormsVehicles)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray own = signalFile(QStringLiteral("Second own"));
    const QString sha = old.keep(own);
    const QJsonArray files{listed(own)};
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"), QString(), files),
                    vehicle(QStringLiteral("v2"), QString(), files, sha),
                    vehicle(QStringLiteral("v3"), QString(), files, sha)}));
    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 1);
    EXPECT_EQ(r.kept, 0) << "wrapped once";
    EXPECT_EQ(factsOf(QStringLiteral("second-own"))
                  .value(QStringLiteral("importedFor"))
                  .toString(),
              QStringLiteral("v2"));
    const QString profile = QStringLiteral("plugin.second-own.base");
    EXPECT_EQ(old.entry(0).value(QStringLiteral("own")).toString(), QString());
    EXPECT_EQ(old.entry(1).value(QStringLiteral("own")).toString(), profile);
    EXPECT_EQ(old.entry(2).value(QStringLiteral("own")).toString(), profile)
        << "a duplicated vehicle shares the file";
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(old.picks(i), QStringList{profile}) << i;
    }
}

/* A file the user has also given to another plugin as a variant: the
 * vehicle picks the plugin made from the file, not the one given it */
TEST(BridgeTest, OneStoreMigrationAFileGivenToAnotherPluginTooTakesItsOwn)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    // "Zed" sorts after "Able", so the plugin given the file comes first
    const QByteArray file = signalFile(QStringLiteral("Zed"));
    const QByteArray host =
        signalFile(QStringLiteral("Able"), QStringLiteral("D101"));
    old.keep(file);
    {
        PluginStore store;
        ASSERT_TRUE(store.wrap(file, {}).value(QStringLiteral("ok")).toBool());
        const QVariantMap able = store.wrap(host, {});
        ASSERT_TRUE(able.value(QStringLiteral("ok")).toBool());
        const QVariantMap put =
            store.put(able.value(QStringLiteral("sha256")).toString(),
                      QStringLiteral("extra"), file);
        ASSERT_TRUE(put.value(QStringLiteral("ok")).toBool())
            << put.value(QStringLiteral("words")).toString().toStdString();
        const QVariantList both = store.wrappedFrom(sha256Of(file));
        ASSERT_EQ(both.size(), 2);
        ASSERT_EQ(both.at(1).toMap().value(QStringLiteral("profile")),
                  QStringLiteral("plugin.able.extra"));
    }
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"),
                            QStringLiteral("elm327://COM5?profile=user.zed"),
                            {listed(file)})}));
    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 0);
    EXPECT_EQ(r.kept, 1);
    EXPECT_EQ(old.picks(0), QStringList{QStringLiteral("plugin.zed.base")});
    EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")).toString(),
              QStringLiteral("elm327://COM5?profile=plugin.zed.base"));
}

/* The book is carried, not judged, but for what no page could show: a
 * list element that isn't a vehicle is left out, an address that isn't
 * text is empty text and what names no copy is passed by. A file listed
 * twice is picked once, and a plugin pick that is a moved file's own
 * profile is not picked again */
TEST(BridgeTest, OneStoreMigrationABookIsCarriedHoweverItIsFormed)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray good = signalFile(QStringLiteral("Good"));
    old.keep(good);
    const QString gone = sha256Of("never kept");
    QJsonObject odd =
        vehicle(QStringLiteral("v1"), QString(),
                {listed(good), listed(good), 7,
                 named(QStringLiteral("nope"), QStringLiteral("Nope"),
                       QStringLiteral("user.nope")),
                 named(gone.left(63), QStringLiteral("Short"),
                       QStringLiteral("user.short")),
                 named(gone, QString(), QStringLiteral("user.gone"))},
                QString(), QStringLiteral("plugin.good.base"));
    odd.insert(QStringLiteral("uri"), 7);
    QJsonObject bare = vehicle(QStringLiteral("v2"), QString(), {});
    bare.remove(QStringLiteral("uri"));
    old.set(kOldBook, bookOf({5, odd, QStringLiteral("text"), bare}));

    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 1);
    EXPECT_EQ(r.kept, 0);
    EXPECT_EQ(r.unmigrated, 1);
    EXPECT_TRUE(r.lines.contains(QStringLiteral("Signal file not moved: ") +
                                 gone.left(12) + QStringLiteral(": ") +
                                 kMissing))
        << said(r);
    const QJsonArray list = old.book().value(QStringLiteral("list")).toArray();
    ASSERT_EQ(list.size(), 2) << "the two vehicles";
    EXPECT_EQ(old.entry(0).value(QStringLiteral("id")).toString(),
              QStringLiteral("v1"));
    EXPECT_EQ(old.picks(0), QStringList{QStringLiteral("plugin.good.base")});
    EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")), QJsonValue(QString()));
    EXPECT_EQ(old.entry(1).value(QStringLiteral("uri")), QJsonValue(QString()))
        << "an entry with no address has an empty one";
    const QJsonArray unmigrated =
        old.entry(0).value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(unmigrated.size(), 1) << "only what names a copy by its hash";
    EXPECT_EQ(unmigrated.at(0).toObject().value(QStringLiteral("sha256")),
              gone);
    EXPECT_FALSE(old.book().contains(QStringLiteral("unmigrated")))
        << "the book's own list is for a book that couldn't be read";
}

/* A file whose plugin is in the store moves without its copy: stopped
 * after the first file, its copy gone, the next start still picks it */
TEST(BridgeTest, OneStoreMigrationAFileWhosePluginIsThereMovesWithoutItsCopy)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageTwoVehicles(old);
    StoreMigration::stopAfterForTesting(Step::File, 1);
    ASSERT_TRUE(old.start().stopped);
    ASSERT_TRUE(QFile::remove(old.copyOf(kSharedSha)));

    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 2);
    EXPECT_EQ(r.kept, 1);
    EXPECT_EQ(r.unmigrated, 0);
    EXPECT_EQ(r.lines.size(), 2) << said(r);
    EXPECT_EQ(
        old.text(kBook).toStdString(),
        corpusText(QStringLiteral("two-vehicles.expect.json")).toStdString());
    EXPECT_FALSE(QDir(StoreMigration::unmigratedRoot()).exists());
}

/* Two conversions of one name for one vehicle: the later takes the
 * earlier one's place in their plugin, as a re-import does. The earlier
 * is then judged as it stands at the end: unmigrated, with words and its
 * bytes kept */
TEST(BridgeTest, OneStoreMigrationAFileALaterOneReplacedIsUnmigrated)
{
    const QByteArray set = readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE));
    const QString name = QStringLiteral("Twice OBDb");
    const ObdbImport::Preview first = ObdbImport::read(set, 2019, name);
    const ObdbImport::Preview second = ObdbImport::read(set, 0, name);
    ASSERT_TRUE(first.ok) << first.words.toStdString();
    ASSERT_TRUE(second.ok) << second.words.toStdString();
    ASSERT_NE(first.json, second.json);

    OldState old;
    ASSERT_TRUE(old.valid());
    const QString firstSha = old.keep(first.json);
    const QString secondSha = old.keep(second.json);
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"),
                            QStringLiteral("elm327://COM5?profile=user.twice-"
                                           "obdb"),
                            {listed(first.json), listed(second.json)})}));
    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 1);
    EXPECT_EQ(r.kept, 0);
    EXPECT_EQ(r.unmigrated, 1);
    EXPECT_EQ(
        r.lines,
        (QStringList{
            QStringLiteral("Signal files moved into the plugin store: 1 "
                           "made into plugins, 0 there already"),
            QStringLiteral("Signal file not moved: Twice OBDb: ") + kNoPlugin}))
        << said(r);
    EXPECT_EQ(copies().size(), 1);
    const QJsonObject from = factsOf(QStringLiteral("twice-obdb"))
                                 .value(QStringLiteral("wrappedFrom"))
                                 .toObject();
    EXPECT_EQ(from.keys(), QStringList{secondSha});
    EXPECT_EQ(old.picks(0),
              QStringList{QStringLiteral("plugin.twice-obdb.base")});
    EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")).toString(),
              QStringLiteral("elm327://COM5?profile=plugin.twice-obdb.base"));
    const QJsonArray list =
        old.entry(0).value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(list.size(), 1);
    const QJsonObject left = list.at(0).toObject();
    EXPECT_EQ(left.value(QStringLiteral("sha256")).toString(), firstSha);
    EXPECT_EQ(left.value(QStringLiteral("words")).toString(), kNoPlugin);
    EXPECT_FALSE(left.contains(QStringLiteral("kept")));
    QString words;
    EXPECT_EQ(StoreMigration::unmigratedBytes(firstSha, &words), first.json);
    EXPECT_EQ(StoreMigration::unmigrated(), QStringList{firstSha});
}

/* Saved addresses that can't be written: the run says so and sets no
 * mark, so a later start finishes them */
TEST(BridgeTest, OneStoreMigrationAddressesThatCantBeWrittenLeaveTheMarkUnset)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageTwoVehicles(old);
    StoreMigration::stopAfterForTesting(Step::Book);
    ASSERT_TRUE(old.start().stopped);
    const QFile::Permissions before = QFile::permissions(old.prefsPath());
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(),
                                      QFile::ReadOwner | QFile::ReadUser));
    if (QFileInfo(old.prefsPath()).isWritable()) {
        QFile::setPermissions(old.prefsPath(), before);
        GTEST_SKIP() << "this account writes a read-only file";
    }

    const Report r = old.start();
    EXPECT_FALSE(r.marked);
    EXPECT_FALSE(r.stopped);
    EXPECT_FALSE(marked());
    EXPECT_EQ(r.addresses, 0);
    EXPECT_EQ(r.lines, QStringList{kAddressesStuck}) << said(r);
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(), before));

    const Report again = old.start();
    EXPECT_TRUE(again.marked) << said(again);
    EXPECT_EQ(old.text(kUri),
              QStringLiteral("elm327://COM5?profile=plugin.shared-pids.base"));
}

/* The most picks a vehicle makes, moved in one start: the time it holds
 * the first frame, printed for the handover */
TEST(BridgeTest, OneStoreMigrationSixteenFilesMigrateInBoundedTime)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    QJsonArray files;
    for (int i = 0; i < PluginStore::MaxPicks + 1; ++i) {
        const QByteArray file =
            signalFile(QStringLiteral("File %1").arg(i),
                       QStringLiteral("D1%1").arg(i, 2, 16, QLatin1Char('0')));
        old.keep(file);
        files.append(listed(file));
    }
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"), QString(), files)}));
    const Report r = old.start();
    EXPECT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, PluginStore::MaxPicks + 1);
    EXPECT_EQ(old.picks(0).size(), PluginStore::MaxPicks + 1)
        << "the cap is the pick's: a book past it keeps every file";
    EXPECT_LT(r.elapsedMs, 20000);
    std::cout << "[          ] " << r.made << " files moved in " << r.elapsedMs
              << " ms\n";
}
