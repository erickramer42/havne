// File/layer: tests/test_store_migration.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// The move of the vehicles' signal files into the plugin store (ADR-0064
// Q6, app/storemigration.{h,cpp}). Each case stages a settings folder as
// an older Havne leaves it: a scratch ini with the vehicle book and the
// saved addresses, the copies in signals/, a third-party package in
// plugins/. A start is StoreMigration::run with a plugin store of its
// own, so two starts are two launches of the app.
//
// tests/data/vehiclebook holds the book in both formats, <case>.json the
// text an older Havne stored and <case>.expect.json the text the move
// writes for it, with the signal files they name in signals/. The move
// must write the second exactly; VehicleBook.qml must read each second.

#include "test_store_migration_support.h"

/* The contract with the page's reader: each book of the corpus becomes
 * exactly its format 2 text */
TEST(BridgeTest, OneStoreMigrationWritesEachCorpusBookAsItsFormat2)
{
    const QDir dir(corpus(QString()));
    QStringList cases;
    for (const QString& name :
         dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        if (!name.endsWith(QStringLiteral(".expect.json"))) {
            cases << name.chopped(5);
        }
    }
    ASSERT_GE(cases.size(), 6);
    for (const QString& name : cases) {
        OldState old;
        ASSERT_TRUE(old.valid());
        keepCorpusSignals(old);
        old.set(kOldBook, corpusText(name + QStringLiteral(".json")));
        const Report r = old.start();
        EXPECT_TRUE(r.marked) << name.toStdString();
        const QString expect =
            corpusText(name + QStringLiteral(".expect.json"));
        ASSERT_FALSE(expect.isEmpty()) << name.toStdString();
        EXPECT_EQ(old.text(kBook).toStdString(), expect.toStdString())
            << name.toStdString();
    }
}

/* Two vehicles: a plain file both have is one plugin both pick, an OBDb
 * conversion and a form's own file are imported for their vehicle, the
 * plugin pick comes last, and the saved addresses name the new profiles */
TEST(BridgeTest, OneStoreMigrationMovesEveryFileAndVehicle)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageTwoVehicles(old);

    const Report r = old.start();
    EXPECT_TRUE(r.ran);
    EXPECT_FALSE(r.stopped);
    EXPECT_TRUE(r.marked);
    EXPECT_EQ(r.made, 3);
    EXPECT_EQ(r.kept, 0);
    EXPECT_EQ(r.unmigrated, 0);
    EXPECT_EQ(r.unnamed, 0);
    EXPECT_EQ(r.addresses, 3);
    EXPECT_EQ(r.lines,
              (QStringList{
                  QStringLiteral("Signal files moved into the plugin store: 3 "
                                 "made into plugins, 0 there already"),
                  QStringLiteral("Saved addresses that now name a plugin's "
                                 "profile: 3")}))
        << said(r);

    EXPECT_EQ(
        old.text(kBook).toStdString(),
        corpusText(QStringLiteral("two-vehicles.expect.json")).toStdString());
    EXPECT_EQ(old.picks(0),
              (QStringList{QStringLiteral("plugin.shared-pids.base"),
                           QStringLiteral("plugin.daily-driver-obdb.base"),
                           QStringLiteral("plugin.daily-driver-own.base"),
                           QStringLiteral("plugin.example-sim.my2019")}))
        << "the files in the book's order, then the plugin pick";
    EXPECT_EQ(old.picks(1),
              QStringList{QStringLiteral("plugin.shared-pids.base")});
    EXPECT_EQ(old.entry(0).value(QStringLiteral("own")).toString(),
              QStringLiteral("plugin.daily-driver-own.base"));
    EXPECT_EQ(old.entry(1).value(QStringLiteral("own")).toString(), QString());
    EXPECT_FALSE(old.entry(0).contains(QStringLiteral("signals")));
    EXPECT_FALSE(old.entry(0).contains(QStringLiteral("plugin")));
    EXPECT_FALSE(old.entry(0).contains(QStringLiteral("unmigrated")));
    EXPECT_EQ(old.entry(0).value(QStringLiteral("later")).toObject(),
              (QJsonObject{{QStringLiteral("kept"), true}}))
        << "a key this version doesn't know is carried";
    EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")).toString(),
              QStringLiteral("elm327://COM5?profile=plugin.shared-pids.base"));
    EXPECT_EQ(old.book().value(QStringLiteral("format")).toInt(), 2);
    EXPECT_EQ(old.book().value(QStringLiteral("picked")).toString(),
              QStringLiteral("v2"));
    EXPECT_EQ(old.book().value(QStringLiteral("salt")).toString(),
              QStringLiteral("00112233445566778899aabbccddeeff"));

    EXPECT_EQ(old.text(kUri),
              QStringLiteral("elm327://COM5?profile=plugin.shared-pids.base"));
    EXPECT_EQ(old.text(kRecentUris),
              compact({QStringLiteral("elm327://COM5?profile=plugin.shared-"
                                      "pids.base"),
                       QStringLiteral("elm327://192.168.0.10:35000?profile="
                                      "obd2"),
                       QStringLiteral("elm327://COM5?profile=plugin.daily-"
                                      "driver-own.base")}));

    // Three plugins of the user's own, each made from its file
    EXPECT_EQ(copies().size(), 4) << "and the third-party package";
    EXPECT_TRUE(marked());
    const auto facts = [](const char* id) {
        return factsOf(QString::fromLatin1(id));
    };
    const auto from = [&](const char* id, const QString& sha) {
        return facts(id)
            .value(QStringLiteral("wrappedFrom"))
            .toObject()
            .value(sha)
            .toString();
    };
    EXPECT_TRUE(facts("shared-pids").value(QStringLiteral("mine")).toBool());
    EXPECT_FALSE(facts("shared-pids").contains(QStringLiteral("importedFor")))
        << "a plain file is no one vehicle's";
    EXPECT_EQ(from("shared-pids", kSharedSha), QStringLiteral("base"));
    EXPECT_TRUE(
        facts("daily-driver-obdb").value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(facts("daily-driver-obdb")
                  .value(QStringLiteral("importedFor"))
                  .toString(),
              QStringLiteral("v1"));
    EXPECT_EQ(from("daily-driver-obdb", kObdbSha), QStringLiteral("base"));
    EXPECT_TRUE(
        facts("daily-driver-own").value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(facts("daily-driver-own")
                  .value(QStringLiteral("importedFor"))
                  .toString(),
              QStringLiteral("v1"))
        << "the form's file is its vehicle's";
    EXPECT_EQ(from("daily-driver-own", kOwnSha), QStringLiteral("base"));
    EXPECT_TRUE(facts("example-sim").isEmpty())
        << "the third-party package is not the user's";

    PluginStore store;
    EXPECT_TRUE(store.migrated());
    EXPECT_EQ(store.all().size(), 4);
}

/* What an older Havne reads is as it was: the old book, every copy in
 * signals/, every other preference and the third-party package */
TEST(BridgeTest, OneStoreMigrationLeavesTheOldStateAsItWas)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageWithUnmigrated(old);
    writeBytes(old.signalsRoot() + QStringLiteral("/notes.txt"), "not a copy");
    const QString bookBefore = old.text(kOldBook);
    const QMap<QString, QString> signalsBefore = filesUnder(old.signalsRoot());
    const QMap<QString, QVariant> prefsBefore = old.otherPrefs();
    const QStringList packages = copies();
    ASSERT_EQ(packages.size(), 1);
    const QString package =
        PluginStore::root() + QLatin1Char('/') + packages.constFirst();
    const QByteArray packageBefore = readBytes(package);
    ASSERT_EQ(signalsBefore.size(), 5);
    ASSERT_GE(prefsBefore.size(), 7);

    const Report r = old.start();
    ASSERT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 3);
    EXPECT_EQ(r.unmigrated, 2);

    EXPECT_EQ(old.text(kOldBook).toStdString(), bookBefore.toStdString());
    EXPECT_EQ(filesUnder(old.signalsRoot()), signalsBefore)
        << said(filesUnder(old.signalsRoot()));
    EXPECT_EQ(old.otherPrefs(), prefsBefore);
    EXPECT_EQ(readBytes(package), packageBefore);
    EXPECT_FALSE(old.text(kBook).isEmpty());
}

/* A kill after any file or step, then a start: the folder and the
 * preferences end as one whole run leaves them */
TEST(BridgeTest, OneStoreMigrationStoppedAfterEachStepEndsTheSame)
{
    QMap<QString, QString> whole;
    QString wholeBook;
    {
        OldState old;
        ASSERT_TRUE(old.valid());
        stageWithUnmigrated(old);
        const Report r = old.start();
        ASSERT_TRUE(r.marked) << said(r);
        ASSERT_FALSE(r.stopped);
        whole = filesUnder(old.path());
        wholeBook = old.text(kBook);
        ASSERT_TRUE(whole.contains(QStringLiteral("plugins/unmigrated/") +
                                   sha256Of("as it was kept") +
                                   QStringLiteral(".json")))
            << said(whole);
    }

    struct Stop {
        Step step;
        int file;
        bool bookThere;
    };
    // Five files: three that move and two that don't
    const Stop stops[] = {{Step::File, 1, false},    {Step::File, 2, false},
                          {Step::File, 3, false},    {Step::File, 4, false},
                          {Step::File, 5, false},    {Step::Wrapped, 0, false},
                          {Step::Copied, 0, false},  {Step::Book, 0, true},
                          {Step::Addresses, 0, true}};
    for (const Stop& stop : stops) {
        const std::string at = "stopped after step " +
                               std::to_string(static_cast<int>(stop.step)) +
                               ", file " + std::to_string(stop.file);
        OldState old;
        ASSERT_TRUE(old.valid());
        stageWithUnmigrated(old);
        const QString uriBefore = old.text(kUri);
        StoreMigration::stopAfterForTesting(stop.step, stop.file);
        const Report first = old.start();
        EXPECT_TRUE(first.stopped) << at;
        EXPECT_FALSE(first.marked) << at;
        EXPECT_FALSE(marked()) << at;
        EXPECT_EQ(!old.text(kBook).isEmpty(), stop.bookThere) << at;
        EXPECT_EQ(QDir(StoreMigration::unmigratedRoot()).exists(),
                  stop.step != Step::File && stop.step != Step::Wrapped)
            << at << ": the bytes are kept only once every file is wrapped";
        EXPECT_EQ(old.text(kUri) != uriBefore, stop.step == Step::Addresses)
            << at << ": the addresses change only after the book is written";

        const Report second = old.start();
        EXPECT_FALSE(second.stopped) << at;
        EXPECT_TRUE(second.marked) << at << "\n" << said(second);
        EXPECT_EQ(second.made + second.kept, stop.bookThere ? 0 : 3) << at;
        EXPECT_EQ(old.text(kBook).toStdString(), wholeBook.toStdString()) << at;
        const QMap<QString, QString> now = filesUnder(old.path());
        EXPECT_EQ(now, whole) << at << "\n"
                              << said(now) << "\nwhole:\n"
                              << said(whole);
    }
}

/* A package written and the app killed before its facts were: the next
 * start finds the package by its bytes and writes the facts then */
TEST(BridgeTest, OneStoreMigrationAPackageWithNoFactsGetsThem)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageTwoVehicles(old);
    StoreMigration::stopAfterForTesting(Step::File, 1);
    ASSERT_TRUE(old.start().stopped);
    ASSERT_EQ(copies().size(), 2);
    ASSERT_TRUE(factsOf(QStringLiteral("shared-pids"))
                    .value(QStringLiteral("mine"))
                    .toBool());
    ASSERT_TRUE(QFile::remove(indexPath()));

    const Report r = old.start();
    EXPECT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 2);
    EXPECT_EQ(r.kept, 1) << "the package was there";
    EXPECT_EQ(copies().size(), 4) << "no second plugin for the first file";
    const QJsonObject facts = factsOf(QStringLiteral("shared-pids"));
    EXPECT_TRUE(facts.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(facts.value(QStringLiteral("wrappedFrom"))
                  .toObject()
                  .value(kSharedSha)
                  .toString(),
              QStringLiteral("base"));
    EXPECT_EQ(
        old.text(kBook).toStdString(),
        corpusText(QStringLiteral("two-vehicles.expect.json")).toStdString());
}

/* With the mark, a start reads the index and nothing else. With the mark
 * lost and the book there, the book is the user's: it stays as edited,
 * no file is moved again and the mark comes back */
TEST(BridgeTest, OneStoreMigrationASecondRunChangesNothing)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageTwoVehicles(old);
    ASSERT_TRUE(old.start().marked);
    const QMap<QString, QString> after = filesUnder(old.path());

    const Report again = old.start();
    EXPECT_FALSE(again.ran);
    EXPECT_FALSE(again.marked) << "nothing was written";
    EXPECT_TRUE(again.lines.isEmpty()) << said(again);
    EXPECT_EQ(filesUnder(old.path()), after);

    // The mark goes; the user has dropped a pick, removed that plugin and
    // added a vehicle since
    QJsonObject index = indexDoc();
    index.remove(QStringLiteral("migrated"));
    writeBytes(indexPath(), QJsonDocument(index).toJson());
    {
        PluginStore store;
        QString sha;
        for (const QVariant& v : store.all()) {
            const QVariantMap p = v.toMap();
            if (p.value(QStringLiteral("id")).toString() ==
                QStringLiteral("daily-driver-obdb")) {
                sha = p.value(QStringLiteral("sha256")).toString();
            }
        }
        ASSERT_TRUE(store.remove(sha).value(QStringLiteral("ok")).toBool());
    }
    QJsonObject book = old.book();
    QJsonArray list = book.value(QStringLiteral("list")).toArray();
    QJsonObject first = list.at(0).toObject();
    first.insert(QStringLiteral("picks"),
                 QJsonArray{QStringLiteral("plugin.shared-pids.base")});
    list.replace(0, first);
    list.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("v9")}});
    book.insert(QStringLiteral("list"), list);
    const QString edited =
        QString::fromUtf8(QJsonDocument(book).toJson(QJsonDocument::Compact));
    old.set(kBook, edited);
    ASSERT_EQ(copies().size(), 3);

    const Report lost = old.start();
    EXPECT_TRUE(lost.ran);
    EXPECT_TRUE(lost.marked);
    EXPECT_EQ(lost.made, 0);
    EXPECT_EQ(lost.kept, 0);
    EXPECT_EQ(lost.addresses, 0) << "an address that names a plugin stays";
    EXPECT_TRUE(lost.lines.isEmpty()) << said(lost);
    EXPECT_EQ(old.text(kBook).toStdString(), edited.toStdString());
    EXPECT_EQ(copies().size(), 3) << "the removed plugin doesn't come back";
    EXPECT_TRUE(marked());
}

/* A book under the new key is never written over, and its presence
 * alone keeps the files from being moved */
TEST(BridgeTest, OneStoreMigrationNeverWritesOverABookThatIsThere)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    keepCorpusSignals(old);
    old.set(kOldBook, corpusText(QStringLiteral("two-vehicles.json")));
    const QString planted = QStringLiteral(
        "{\"format\":2,\"list\":[],\"picked\":\"\",\"salt\":\"mine\"}");
    old.set(kBook, planted);

    const Report r = old.start();
    EXPECT_TRUE(r.marked);
    EXPECT_EQ(r.made + r.kept + r.unmigrated, 0);
    EXPECT_EQ(old.text(kBook).toStdString(), planted.toStdString());
    EXPECT_TRUE(copies().isEmpty()) << "no file was wrapped";
    EXPECT_FALSE(QDir(StoreMigration::unmigratedRoot()).exists());

    // A value there that isn't text counts as a book too
    OldState other;
    ASSERT_TRUE(other.valid());
    keepCorpusSignals(other);
    other.set(kOldBook, corpusText(QStringLiteral("two-vehicles.json")));
    other.set(kBook, QStringList{QStringLiteral("a"), QStringLiteral("b")});
    EXPECT_TRUE(other.start().marked);
    EXPECT_TRUE(copies().isEmpty());
}

/* A file with no copy, a copy changed since, one the loader refuses and
 * one the wrap refuses: each stays with its vehicle as unmigrated, with
 * words, out of the picks; the bytes there are, are kept; an address
 * naming one is left as typed; the vehicle's other file still moves */
TEST(BridgeTest, OneStoreMigrationAFileThatDoesntWrapIsKeptAsUnmigrated)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray good = signalFile(QStringLiteral("Good"));
    const QByteArray bad = "{\"havne_signals\":1,\"name\":";
    const QByteArray heavy = heavyFile(QStringLiteral("Heavy"));
    ASSERT_LT(heavy.size(), qsizetype{ECUIO_SIGNAL_FILE_MAX_BYTES});
    const QString gone = sha256Of("never kept");
    const QString changed = sha256Of("as it was kept");
    writeBytes(old.copyOf(changed), "changed since");
    const QString badSha = old.keep(bad);
    const QString heavySha = old.keep(heavy);
    old.keep(good);
    const QString address = QStringLiteral("elm327://COM5?profile=user.gone");
    old.set(
        kOldBook,
        bookOf({vehicle(
            QStringLiteral("v1"), address,
            {named(gone, QStringLiteral("Gone"), QStringLiteral("user.gone")),
             named(changed, QStringLiteral("Changed"),
                   QStringLiteral("user.changed")),
             listed(good),
             named(badSha, QStringLiteral("Bad"), QStringLiteral("user.bad")),
             listed(heavy)},
            badSha)}));
    old.set(kUri, address);

    const Report r = old.start();
    EXPECT_TRUE(r.marked) << said(r);
    EXPECT_EQ(r.made, 1);
    EXPECT_EQ(r.unmigrated, 4);
    EXPECT_EQ(r.addresses, 0);
    EXPECT_EQ(r.lines.size(), 5) << said(r);
    EXPECT_EQ(old.picks(0), QStringList{QStringLiteral("plugin.good.base")});
    EXPECT_EQ(old.entry(0).value(QStringLiteral("own")).toString(), QString())
        << "the own file didn't move";
    EXPECT_EQ(old.entry(0).value(QStringLiteral("uri")).toString(), address);
    EXPECT_EQ(old.text(kUri), address);

    const QJsonArray list =
        old.entry(0).value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(list.size(), 4);
    const auto at = [&](int i, const char* key) {
        return list.at(i).toObject().value(QString::fromLatin1(key)).toString();
    };
    EXPECT_EQ(at(0, "sha256"), gone);
    EXPECT_EQ(at(0, "name"), QStringLiteral("Gone"));
    EXPECT_EQ(at(0, "words"), kMissing);
    EXPECT_EQ(at(1, "sha256"), changed);
    EXPECT_EQ(at(1, "words"), kChanged);
    EXPECT_EQ(at(2, "sha256"), badSha);
    EXPECT_TRUE(at(2, "words").startsWith(QStringLiteral("line 1, column ")))
        << at(2, "words").toStdString();
    EXPECT_EQ(at(3, "sha256"), heavySha);
    EXPECT_TRUE(
        at(3, "words")
            .contains(QStringLiteral("files don't make one signal file")))
        << at(3, "words").toStdString();
    for (const QJsonValue& v : list) {
        EXPECT_EQ(v.toObject().size(), 3)
            << "sha256, name and words: its bytes are kept, or it has none";
    }
    EXPECT_TRUE(r.lines.contains(
        QStringLiteral("Signal file not moved: Gone: ") + kMissing))
        << said(r);
    EXPECT_TRUE(r.lines.contains(
        QStringLiteral("Signal file not moved: Changed: ") + kChanged))
        << said(r);

    // The three with bytes, named by the book's hash, whole
    QStringList kept{changed, badSha, heavySha};
    kept.sort();
    EXPECT_EQ(StoreMigration::unmigrated(), kept);
    QString words;
    EXPECT_EQ(StoreMigration::unmigratedBytes(changed, &words),
              QByteArray("changed since"));
    EXPECT_EQ(StoreMigration::unmigratedBytes(badSha, &words), bad);
    EXPECT_EQ(StoreMigration::unmigratedBytes(heavySha, &words), heavy);
    EXPECT_TRUE(words.isEmpty());
    EXPECT_TRUE(StoreMigration::unmigratedBytes(gone, &words).isEmpty());
    EXPECT_EQ(words, QStringLiteral("Its bytes weren't kept"));
    words.clear();
    EXPECT_TRUE(
        StoreMigration::unmigratedBytes(QStringLiteral("../store"), &words)
            .isEmpty());
    EXPECT_EQ(words, QStringLiteral("Its bytes weren't kept"));
    EXPECT_EQ(copies().size(), 1) << "an older Havne's store lists one plugin";

    // Removed one at a time; the folder goes with the last
    EXPECT_FALSE(StoreMigration::removeUnmigrated(QStringLiteral("../store")));
    EXPECT_TRUE(QFile::exists(indexPath()));
    EXPECT_TRUE(StoreMigration::removeUnmigrated(gone)) << "none to remove";
    EXPECT_TRUE(StoreMigration::removeUnmigrated(changed));
    EXPECT_EQ(StoreMigration::unmigrated().size(), 2);
    EXPECT_TRUE(QDir(StoreMigration::unmigratedRoot()).exists());
    EXPECT_TRUE(StoreMigration::removeUnmigrated(badSha));
    EXPECT_TRUE(StoreMigration::removeUnmigrated(heavySha));
    EXPECT_FALSE(QDir(StoreMigration::unmigratedRoot()).exists());
    EXPECT_TRUE(QFile::exists(old.copyOf(badSha))) << "the old copy stays";
}

/* A run that kept a file's bytes as unmigrated and stopped before the
 * book was written, and a next run whose wrap of that file succeeds (its
 * old copy is whole now): no book lists the kept bytes, so the run that
 * moves the file removes them */
TEST(BridgeTest, OneStoreMigrationAKeptCopyOfAFileThatMovesLaterIsRemoved)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray good = signalFile(QStringLiteral("Good"));
    const QString sha = sha256Of(good);
    writeBytes(old.copyOf(sha), "changed since");
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"),
                            QStringLiteral("elm327://COM5?profile=user.good"),
                            {named(sha, QStringLiteral("Good"),
                                   QStringLiteral("user.good"))},
                            sha)}));

    StoreMigration::stopAfterForTesting(Step::Copied);
    const Report first = old.start();
    EXPECT_TRUE(first.stopped) << said(first);
    EXPECT_EQ(StoreMigration::unmigrated(), QStringList{sha})
        << "the bytes as they were are kept";

    writeBytes(old.copyOf(sha), good);
    const Report second = old.start();
    EXPECT_TRUE(second.marked) << said(second);
    EXPECT_EQ(second.made, 1) << said(second);
    EXPECT_TRUE(StoreMigration::unmigrated().isEmpty())
        << "the file moved, so no copy of it is left over";
}

/* A copy past what core reads of anything, and one the folder can't
 * take: the book records that its bytes are only in the old folder (kept
 * false), which that folder's removal must spare, and the log says so.
 * The move isn't held up for one file */
TEST(BridgeTest, OneStoreMigrationAnUnmigratedCopyThatCantBeKeptIsSaid)
{
    const auto kept = [](const QJsonValue& entry) {
        return entry.toObject().value(QStringLiteral("kept"));
    };
    OldState old;
    ASSERT_TRUE(old.valid());
    const QString big = sha256Of("big");
    const QString atBound = sha256Of("at the bound");
    writeBytes(old.copyOf(big), "x");
    writeBytes(old.copyOf(atBound), "x");
    ASSERT_TRUE(QFile::resize(old.copyOf(big), ECUIO_PLUGIN_PACKAGE_MAX + 1));
    ASSERT_TRUE(QFile::resize(old.copyOf(atBound), ECUIO_PLUGIN_PACKAGE_MAX));
    old.set(kOldBook,
            bookOf({vehicle(
                QStringLiteral("v1"), QString(),
                {named(big, QStringLiteral("Big"), QStringLiteral("user.big")),
                 named(atBound, QStringLiteral("At the bound"),
                       QStringLiteral("user.at-the-bound"))})}));
    const Report r = old.start();
    EXPECT_TRUE(r.marked);
    EXPECT_EQ(r.unmigrated, 2);
    EXPECT_EQ(StoreMigration::unmigrated(), QStringList{atBound});
    EXPECT_EQ(QFileInfo(StoreMigration::unmigratedRoot() + QLatin1Char('/') +
                        atBound + QStringLiteral(".json"))
                  .size(),
              qint64{ECUIO_PLUGIN_PACKAGE_MAX});
    EXPECT_EQ(r.lines.count(kNotKept), 1) << said(r);
    const QJsonArray list =
        old.entry(0).value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(kept(list.at(0)), QJsonValue(false)) << "past the bound";
    EXPECT_TRUE(kept(list.at(1)).isUndefined()) << "its bytes are kept";
    EXPECT_TRUE(QFile::exists(old.copyOf(big)));

    // The folder's name taken by a file: nothing can be written there
    OldState blocked;
    ASSERT_TRUE(blocked.valid());
    const QString changed = sha256Of("as it was kept");
    writeBytes(blocked.copyOf(changed), "changed since");
    writeBytes(StoreMigration::unmigratedRoot(), "in the way");
    blocked.set(kOldBook,
                bookOf({vehicle(QStringLiteral("v1"), QString(),
                                {named(changed, QStringLiteral("Changed"),
                                       QStringLiteral("user.changed"))})}));
    const Report b = blocked.start();
    EXPECT_TRUE(b.marked) << "the old copy is still where it was";
    EXPECT_EQ(b.lines.count(kNotKept), 1) << said(b);
    const QJsonArray held =
        blocked.entry(0).value(QStringLiteral("unmigrated")).toArray();
    ASSERT_EQ(held.size(), 1);
    EXPECT_EQ(kept(held.at(0)), QJsonValue(false));
    EXPECT_EQ(held.at(0).toObject().value(QStringLiteral("words")).toString(),
              kChanged);
}

/* The page's Save and Remove of a file that didn't move, through the
 * bridge: the bytes the move kept, else the earlier folder's copy as it
 * is, written where the user chose; Remove takes the kept bytes alone.
 * The log names the file by its hash's start, never a path */
TEST(BridgeTest, OneStoreMigrationAFileThatDidntMoveIsSavedAndRemoved)
{
    const auto told = [](const QVariantMap& r) {
        return r.value(QStringLiteral("words")).toString();
    };
    const auto ok = [](const QVariantMap& r) {
        return r.value(QStringLiteral("ok")).toBool();
    };
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const auto target = [&](const char* name) {
        return out.path() + QLatin1Char('/') + QString::fromLatin1(name);
    };
    const QString gone = sha256Of("never kept");
    const QString changed = sha256Of("as it was kept");
    {
        OldState old;
        ASSERT_TRUE(old.valid());
        writeBytes(old.copyOf(changed), "changed since");
        old.set(kOldBook,
                bookOf({vehicle(QStringLiteral("v1"), QString(),
                                {named(gone, QStringLiteral("Gone"),
                                       QStringLiteral("user.gone")),
                                 named(changed, QStringLiteral("Changed"),
                                       QStringLiteral("user.changed"))})}));
        SessionBridge bridge;
        QStringList log;
        QObject::connect(&bridge, &SessionBridge::logMessage, &bridge,
                         [&log](const QString& line) { log << line; });
        {
            QSettings prefs(old.prefsPath(), QSettings::IniFormat);
            (void)bridge.migrateStores(prefs);
        }
        ASSERT_EQ(StoreMigration::unmigrated(), QStringList{changed});
        // The kept bytes win over the earlier folder's copy
        writeBytes(old.copyOf(changed), "changed again");
        log.clear();

        QVariantMap r = bridge.saveUnmigratedFile(
            changed, QUrl::fromLocalFile(target("changed.json")));
        EXPECT_TRUE(ok(r)) << told(r).toStdString();
        EXPECT_EQ(told(r), QString());
        EXPECT_EQ(readBytes(target("changed.json")),
                  QByteArray("changed since"));
        ASSERT_EQ(log.size(), 1);
        EXPECT_EQ(log.constFirst(),
                  QStringLiteral("Signal file %1 that wasn't moved saved to "
                                 "a file (13 bytes)")
                      .arg(changed.left(12)));

        r = bridge.saveUnmigratedFile(gone,
                                      QUrl::fromLocalFile(target("gone.json")));
        EXPECT_FALSE(ok(r));
        EXPECT_EQ(told(r), QStringLiteral("Its copy is gone from the earlier "
                                          "version's folder"));
        EXPECT_FALSE(QFile::exists(target("gone.json")));
        ASSERT_EQ(log.size(), 2);
        EXPECT_EQ(log.constLast(),
                  QStringLiteral("Signal file %1 that wasn't moved not "
                                 "saved: %2")
                      .arg(gone.left(12), told(r)));
        for (const QString& line : std::as_const(log)) {
            EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
            EXPECT_FALSE(line.contains(QStringLiteral("signals")))
                << line.toStdString();
        }

        r = bridge.saveUnmigratedFile(
            QStringLiteral("../store"),
            QUrl::fromLocalFile(target("escape.json")));
        EXPECT_FALSE(ok(r)) << "only a hash names a file";
        EXPECT_FALSE(QFile::exists(target("escape.json")));

        r = bridge.saveUnmigratedFile(
            changed, QUrl::fromLocalFile(out.path() +
                                         QStringLiteral("/none/such.json")));
        EXPECT_FALSE(ok(r));
        EXPECT_EQ(told(r), QStringLiteral("Couldn't write the file"));

        EXPECT_FALSE(bridge.removeUnmigratedFile(QStringLiteral("../store")));
        EXPECT_TRUE(bridge.removeUnmigratedFile(changed));
        EXPECT_TRUE(StoreMigration::unmigrated().isEmpty());
        EXPECT_EQ(readBytes(old.copyOf(changed)), QByteArray("changed again"))
            << "the earlier folder's copy is never removed";
    }

    // Bytes the move couldn't keep (kept false): the earlier folder's copy
    OldState blocked;
    ASSERT_TRUE(blocked.valid());
    const QString big = sha256Of("big");
    writeBytes(blocked.copyOf(changed), "changed since");
    writeBytes(blocked.copyOf(big), "x");
    ASSERT_TRUE(QFile::resize(blocked.copyOf(big),
                              StoreMigration::OldCopyMaxBytes + 1));
    writeBytes(StoreMigration::unmigratedRoot(), "in the way");
    blocked.set(kOldBook,
                bookOf({vehicle(QStringLiteral("v1"), QString(),
                                {named(changed, QStringLiteral("Changed"),
                                       QStringLiteral("user.changed")),
                                 named(big, QStringLiteral("Big"),
                                       QStringLiteral("user.big"))})}));
    SessionBridge bridge;
    {
        QSettings prefs(blocked.prefsPath(), QSettings::IniFormat);
        (void)bridge.migrateStores(prefs);
    }
    ASSERT_EQ(blocked.entry(0)
                  .value(QStringLiteral("unmigrated"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("kept")),
              QJsonValue(false));
    QVariantMap r = bridge.saveUnmigratedFile(
        changed, QUrl::fromLocalFile(target("unkept.json")));
    EXPECT_TRUE(ok(r)) << told(r).toStdString();
    EXPECT_EQ(readBytes(target("unkept.json")), QByteArray("changed since"));
    r = bridge.saveUnmigratedFile(big, QUrl::fromLocalFile(target("big.json")));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(told(r), QStringLiteral("Its copy is over 64 MiB"));
    EXPECT_FALSE(QFile::exists(target("big.json")));
    ASSERT_TRUE(
        QFile::resize(blocked.copyOf(big), StoreMigration::OldCopyMaxBytes));
    r = bridge.saveUnmigratedFile(big, QUrl::fromLocalFile(target("big.json")));
    EXPECT_TRUE(ok(r)) << "a copy at the bound is saved";
    EXPECT_EQ(QFileInfo(target("big.json")).size(),
              StoreMigration::OldCopyMaxBytes);
    EXPECT_TRUE(bridge.removeUnmigratedFile(changed)) << "none kept to remove";
    EXPECT_TRUE(QFile::exists(blocked.copyOf(changed)));
}
