// File/layer: tests/test_store_removal.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.OneStoreRemoval* (test_bridge)
//
// The earlier version's data removed once the move is done (ADR-0064 Q6,
// StoreMigration::removeOld): vehicles/document and the copies in
// signals/, at a start after the one that set the store's mark, sparing
// every copy whose bytes are nowhere else. Each case stages a settings
// folder (tests/old_state.h); most write the format 2 book and the mark
// by hand, since the removal reads nothing else.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include <string>

#include "old_state.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "storemigration.h"
#include <ecuio/plugin.h>

using namespace old_state;

namespace {

using Removal = StoreMigration::Removal;

const QString kListRemoved =
    QStringLiteral("The earlier version's vehicle list removed");

// A record of a file that didn't move; kept false when its bytes are
// only in signals/
QJsonObject record(const QString& sha, bool kept)
{
    QJsonObject r{{QStringLiteral("sha256"), sha},
                  {QStringLiteral("name"), QStringLiteral("File")},
                  {QStringLiteral("words"), QStringLiteral("why")}};
    if (!kept) {
        r.insert(QStringLiteral("kept"), false);
    }
    return r;
}

// A format 2 book of one vehicle with these records, and these of the
// book's own
QString book2(const QJsonArray& entryRecords, const QJsonArray& ownRecords)
{
    QJsonObject entry{{QStringLiteral("id"), QStringLiteral("v1")},
                      {QStringLiteral("name"), QStringLiteral("Car")},
                      {QStringLiteral("uri"), QString()},
                      {QStringLiteral("picks"), QJsonArray()}};
    if (!entryRecords.isEmpty()) {
        entry.insert(QStringLiteral("unmigrated"), entryRecords);
    }
    QJsonObject doc{{QStringLiteral("format"), 2},
                    {QStringLiteral("list"), QJsonArray{entry}}};
    if (!ownRecords.isEmpty()) {
        doc.insert(QStringLiteral("unmigrated"), ownRecords);
    }
    return QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
}

// The store's mark, as a start that finished the move leaves it
void markDone()
{
    PluginStore store;
    ASSERT_TRUE(store.setMigrated());
}

// One start's removal, with a plugin store of its own
Removal removal(const OldState& old)
{
    PluginStore store;
    QSettings prefs(old.prefsPath(), QSettings::IniFormat);
    return StoreMigration::removeOld(prefs, store);
}

// Where the move keeps the bytes of a file that didn't move
QString keptPath(const QString& sha)
{
    return StoreMigration::unmigratedRoot() + QLatin1Char('/') + sha +
           QStringLiteral(".json");
}

QStringList namesIn(const QString& folder)
{
    return QDir(folder).entryList(QDir::Files, QDir::Name);
}

// A copy of a signal file that moved: a plugin made from its bytes
QString keepMoved(const OldState& old, const QString& name)
{
    const QByteArray bytes = signalFile(name);
    PluginStore store;
    const QVariantMap made = store.wrap(bytes, {});
    EXPECT_TRUE(made.value(QStringLiteral("ok")).toBool())
        << made.value(QStringLiteral("words")).toString().toStdString();
    return old.keep(bytes);
}

// Copies named a to e (by their own hashes) and an old book; a's record
// is a vehicle's with kept false, b's the book's own with kept false,
// c's kept (its bytes in plugins/unmigrated too), d no one's, e moved
struct Five {
    QString a, b, c, d, e;
};

Five stageFive(const OldState& old)
{
    Five f;
    f.a = old.keep("a's bytes");
    f.b = old.keep("b's bytes");
    f.c = old.keep("c's bytes");
    f.d = old.keep("d's bytes");
    f.e = keepMoved(old, QStringLiteral("E"));
    writeBytes(keptPath(f.c), "c's bytes");
    old.set(kOldBook, QStringLiteral("{\"list\":[]}"));
    old.set(kBook, book2({record(f.a, false), record(f.c, true)},
                         {record(f.b, false)}));
    return f;
}

const QString kLeftUnnamed1 =
    QStringLiteral("Signal files no plugin was made from, left in the "
                   "earlier version's folder: 1");

// Every bridge call that imports, keeps, saves or removes in the stores,
// each once and each answered ok; what it saves goes to out
void runEveryStoreCall(SessionBridge& bridge, const QString& out,
                       const QString& big, const QString& changed)
{
    const auto url = [&out](const char* name) {
        return QUrl::fromLocalFile(out + QLatin1Char('/') +
                                   QLatin1String(name));
    };
    // The answer's sha256, once its ok is checked
    const auto ok = [](const QVariantMap& r) {
        EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        return r.value(QStringLiteral("sha256")).toString();
    };
    const QString more = QStringLiteral("more");
    writeBytes(out + QStringLiteral("/a.json"),
               signalFile(QStringLiteral("Picked")));
    ok(bridge.readImport(url("a.json")));
    QString mine =
        ok(bridge.keepSignals(QString(), QString(), QStringLiteral("v1")));
    writeBytes(out + QStringLiteral("/b.json"),
               signalFile(QStringLiteral("More"), QStringLiteral("D101")));
    ok(bridge.readImport(url("b.json")));
    mine = ok(bridge.keepSignals(mine, more, QString()));
    ok(bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Typed"))), QString(),
        QString(), QString()));
    const QString copied = ok(bridge.copyVariant(mine, more, QString(), more));
    ok(bridge.saveVariantFile(mine, more, url("more.json")));
    mine = ok(bridge.dropPluginVariant(mine, more));
    ok(bridge.exportPlugin(mine, url("mine.havne-plugin.json")));
    ok(bridge.removePlugin(mine));
    ok(bridge.markPluginMine(
        ok(bridge.importPlugin(url("mine.havne-plugin.json")))));
    ok(bridge.removePlugin(copied));
    ok(bridge.readImport(
        QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE))));
    ok(bridge.previewObdbImport(2019, QStringLiteral("Car OBDb")));
    ok(bridge.keepObdbPlugin(QStringLiteral("v1"), true));
    ok(bridge.saveUnmigratedFile(big, url("big.json")));
    ok(bridge.saveUnmigratedFile(changed, url("changed.json")));
    EXPECT_TRUE(bridge.removeUnmigratedFile(changed));
    EXPECT_TRUE(bridge.removeUnmigratedFile(big));
    (void)bridge.forgetVehicleImports(QStringLiteral("v1"));
    (void)bridge.forgetAllVehicleImports();
}

} // namespace

/* Two starts after an update from before v0.8.0: the first moves the
 * files and leaves the earlier version's data; the second removes it,
 * keeping the copy that couldn't be moved and the copy no vehicle names,
 * whose bytes are nowhere else, and Save still writes each */
TEST(BridgeTest, OneStoreRemovalComesAtTheStartAfterTheMove)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray moved = signalFile(QStringLiteral("Moved"));
    old.keep(moved);
    const QString loose = old.keep(signalFile(QStringLiteral("Loose")));
    // A copy past what the store keeps: its record says kept false, and
    // its bytes don't hash to its name, which spares it all the same
    const QString big = sha256Of("a file past the store's bound");
    writeBytes(old.copyOf(big), QByteArray());
    ASSERT_TRUE(QFile::resize(old.copyOf(big), ECUIO_PLUGIN_PACKAGE_MAX + 1));
    writeBytes(old.signalsRoot() + QStringLiteral("/notes.txt"), "mine");
    writeBytes(old.signalsRoot() + QStringLiteral("/not-a-hash.json"), "{}");
    old.set(kOldBook,
            bookOf({vehicle(QStringLiteral("v1"), QString(),
                            {listed(moved),
                             named(big, QStringLiteral("Big"),
                                   QStringLiteral("user.big"))})}));
    const auto before = filesUnder(old.signalsRoot());

    {
        SessionBridge bridge;
        QSettings prefs(old.prefsPath(), QSettings::IniFormat);
        const QStringList lines = bridge.migrateStores(prefs);
        EXPECT_FALSE(lines.filter(QStringLiteral("moved into the plugin "
                                                 "store"))
                         .isEmpty())
            << lines.join(QLatin1Char('\n')).toStdString();
        EXPECT_TRUE(lines.filter(QStringLiteral("removed")).isEmpty());
    }
    ASSERT_TRUE(marked());
    EXPECT_TRUE(old.has(kOldBook)) << "the move's own start keeps it";
    EXPECT_EQ(filesUnder(old.signalsRoot()), before);
    ASSERT_EQ(old.entry(0)
                  .value(QStringLiteral("unmigrated"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("kept")),
              QJsonValue(false));

    {
        SessionBridge bridge;
        QSettings prefs(old.prefsPath(), QSettings::IniFormat);
        EXPECT_EQ(bridge.migrateStores(prefs),
                  (QStringList{kListRemoved,
                               QStringLiteral("The earlier version's signal "
                                              "files removed: 1"),
                               QStringLiteral("Signal files that weren't "
                                              "moved, kept in the earlier "
                                              "version's folder for Save: "
                                              "2")}));
        QTemporaryDir out;
        ASSERT_TRUE(out.isValid());
        const QString to = out.path() + QStringLiteral("/big.json");
        const QVariantMap saved =
            bridge.saveUnmigratedFile(big, QUrl::fromLocalFile(to));
        EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
            << saved.value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(QFileInfo(to).size(), ECUIO_PLUGIN_PACKAGE_MAX + 1);
        const QString looseTo = out.path() + QStringLiteral("/loose.json");
        const QVariantMap looseSaved =
            bridge.saveUnmigratedFile(loose, QUrl::fromLocalFile(looseTo));
        EXPECT_TRUE(looseSaved.value(QStringLiteral("ok")).toBool())
            << looseSaved.value(QStringLiteral("words"))
                   .toString()
                   .toStdString();
        EXPECT_EQ(readBytes(looseTo), signalFile(QStringLiteral("Loose")));
    }
    EXPECT_FALSE(old.has(kOldBook));
    QStringList left{
        big + QStringLiteral(".json"), loose + QStringLiteral(".json"),
        QStringLiteral("not-a-hash.json"), QStringLiteral("notes.txt")};
    left.sort();
    EXPECT_EQ(namesIn(old.signalsRoot()), left) << "the moved copy goes";
    EXPECT_FALSE(old.book().isEmpty()) << "the new book stays";
}

/* A record with kept false spares its copy whether a vehicle lists it or
 * the book's own list does; a kept record's old copy goes (its bytes are
 * in plugins/unmigrated), and so does a moved copy (a plugin was made
 * from it); a copy no record names, with no plugin made from it, stays.
 * A copy is judged by its name alone: a's bytes no longer hash to it */
TEST(BridgeTest, OneStoreRemovalSparesTheCopiesOfAVehiclesAndTheBooksRecords)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const Five f = stageFive(old);
    writeBytes(old.copyOf(f.a), "a, edited by hand since");
    markDone();
    const QByteArray cKept = readBytes(keptPath(f.c));

    const Removal r = removal(old);
    EXPECT_TRUE(r.oldBook);
    EXPECT_EQ(r.removed, 2);
    EXPECT_EQ(r.spared, 2);
    EXPECT_EQ(r.unnamed, 1);
    EXPECT_EQ(r.stuck, 0);
    EXPECT_FALSE(old.has(kOldBook));
    QStringList left{f.a + QStringLiteral(".json"),
                     f.b + QStringLiteral(".json"),
                     f.d + QStringLiteral(".json")};
    left.sort();
    EXPECT_EQ(namesIn(old.signalsRoot()), left);
    EXPECT_EQ(readBytes(old.copyOf(f.a)), "a, edited by hand since");
    EXPECT_EQ(readBytes(old.copyOf(f.d)), "d's bytes");
    EXPECT_EQ(StoreMigration::unmigrated(), QStringList{f.c});
    EXPECT_EQ(readBytes(keptPath(f.c)), cKept);
    EXPECT_EQ(r.lines,
              (QStringList{kListRemoved,
                           QStringLiteral("The earlier version's signal files "
                                          "removed: 2"),
                           QStringLiteral("Signal files that weren't moved, "
                                          "kept in the earlier version's "
                                          "folder for Save: 2"),
                           kLeftUnnamed1}));
}

/* A book that records no file that didn't move, with the mark set (an
 * earlier version's move, which recorded none, or a book written by hand)
 * takes no copy whose bytes are nowhere else: a copy with no plugin made
 * from it stays through two starts, and only the first says so */
TEST(BridgeTest, OneStoreRemovalNeverTakesACopyOnTheBooksWordAlone)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QString stray = old.keep("stray's bytes");
    const QString moved = keepMoved(old, QStringLiteral("Moved"));
    old.set(kOldBook, QStringLiteral("{\"list\":[]}"));
    old.set(kBook, book2({}, {}));
    markDone();

    const Removal first = removal(old);
    EXPECT_EQ(first.removed, 1);
    EXPECT_EQ(first.unnamed, 1);
    EXPECT_EQ(first.lines,
              (QStringList{kListRemoved,
                           QStringLiteral("The earlier version's signal files "
                                          "removed: 1"),
                           kLeftUnnamed1}));
    EXPECT_FALSE(QFileInfo::exists(old.copyOf(moved)));
    const Removal second = removal(old);
    EXPECT_EQ(second.removed, 0);
    EXPECT_EQ(second.unnamed, 1);
    EXPECT_TRUE(second.lines.isEmpty());
    EXPECT_EQ(namesIn(old.signalsRoot()),
              QStringList{stray + QStringLiteral(".json")});
    EXPECT_EQ(readBytes(old.copyOf(stray)), "stray's bytes");
}

/* With every copy's bytes elsewhere and nothing else there, the folder
 * goes with its last copy; a second start then has nothing to do and
 * says nothing */
TEST(BridgeTest, OneStoreRemovalTakesTheFolderWithItsLastCopy)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    keepMoved(old, QStringLiteral("One"));
    const QString two = old.keep("two");
    writeBytes(keptPath(two), "two");
    old.set(kOldBook, QStringLiteral("{\"list\":[]}"));
    old.set(kBook, book2({record(two, true)}, {}));
    markDone();

    const Removal r = removal(old);
    EXPECT_EQ(r.removed, 2);
    EXPECT_FALSE(QFileInfo::exists(old.signalsRoot()));
    const Removal again = removal(old);
    EXPECT_FALSE(again.oldBook);
    EXPECT_EQ(again.removed, 0);
    EXPECT_TRUE(again.lines.isEmpty());
}

/* A second start with a spared copy left changes nothing and says
 * nothing; once its record is removed (the page's Remove), its bytes are
 * still only in the old folder, so the next start leaves it there */
TEST(BridgeTest, OneStoreRemovalKeepsACopyOnceItsRecordIsRemoved)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QString a = old.keep("a's bytes");
    old.set(kBook, book2({record(a, false)}, {}));
    markDone();

    const Removal first = removal(old);
    EXPECT_EQ(first.spared, 1);
    EXPECT_TRUE(first.lines.isEmpty()) << "nothing went";
    const auto kept = filesUnder(old.signalsRoot());
    EXPECT_TRUE(removal(old).lines.isEmpty());
    EXPECT_EQ(filesUnder(old.signalsRoot()), kept);

    old.set(kBook, book2({}, {}));
    const Removal after = removal(old);
    EXPECT_EQ(after.removed, 0);
    EXPECT_EQ(after.unnamed, 1);
    EXPECT_TRUE(after.lines.isEmpty());
    EXPECT_EQ(filesUnder(old.signalsRoot()), kept);
}

/* Nothing is touched without the mark, or without a book this version
 * judges: none, one of a later format, a format that isn't a number,
 * text that isn't JSON, a list in place of an object */
TEST(BridgeTest, OneStoreRemovalTouchesNothingWithoutAMarkOrABookItReads)
{
    const struct {
        const char* why;
        bool mark;
        const char* book; // nullptr: no key
    } cases[] = {
        {"no mark", false, "-"},
        {"no book", true, nullptr},
        {"a later format", true, R"({"format":3,"list":[]})"},
        {"a format that isn't a number", true, R"({"format":"2","list":[]})"},
        {"not JSON", true, "{\"format\":2,"},
        {"a list", true, R"([{"format":2}])"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.why);
        OldState old;
        ASSERT_TRUE(old.valid());
        old.keep("a copy");
        old.set(kOldBook, QStringLiteral("{\"list\":[]}"));
        if (c.book != nullptr) {
            old.set(kBook, std::string(c.book) == "-"
                               ? book2({}, {})
                               : QString::fromUtf8(c.book));
        }
        if (c.mark) {
            markDone();
        }
        const auto before = filesUnder(old.signalsRoot());
        const Removal r = removal(old);
        EXPECT_FALSE(r.oldBook);
        EXPECT_TRUE(r.lines.isEmpty());
        EXPECT_TRUE(old.has(kOldBook));
        EXPECT_EQ(filesUnder(old.signalsRoot()), before);
    }
}

/* Stopped after the old book or after each copy, as a killed app would
 * be, the next start finishes it: the same folder and preferences as a
 * removal that ran whole */
TEST(BridgeTest, OneStoreRemovalStoppedAfterEachStepEndsTheSame)
{
    const auto run = [](Step stop, int n, QMap<QString, QString>* files,
                        bool* bookLeft) {
        OldState old;
        ASSERT_TRUE(old.valid());
        stageFive(old);
        markDone();
        if (stop != Step::None) {
            StoreMigration::stopAfterForTesting(stop, n);
            const Removal stopped = removal(old);
            EXPECT_TRUE(stopped.stopped);
            EXPECT_FALSE(old.has(kOldBook)) << "the old book goes first";
        }
        const Removal r = removal(old);
        EXPECT_FALSE(r.stopped);
        *files = filesUnder(old.path());
        files->remove(QStringLiteral("Havne/Havne.ini"));
        *bookLeft = old.has(kOldBook);
    };
    QMap<QString, QString> whole;
    bool wholeBook = true;
    run(Step::None, 0, &whole, &wholeBook);
    ASSERT_FALSE(whole.isEmpty());
    EXPECT_FALSE(wholeBook);
    const struct {
        Step step;
        int n;
    } stops[] = {{Step::OldBook, 0}, {Step::OldCopy, 1}, {Step::OldCopy, 2}};
    for (const auto& s : stops) {
        SCOPED_TRACE(s.n);
        QMap<QString, QString> files;
        bool book = true;
        run(s.step, s.n, &files, &book);
        EXPECT_EQ(files, whole) << said(files) << "\nwhole:\n" << said(whole);
        EXPECT_FALSE(book);
    }
}

/* Preferences that can't be written stop it before anything is deleted:
 * the old book is the commit point, and a later start finishes */
TEST(BridgeTest, OneStoreRemovalWithPreferencesThatCantBeWrittenDeletesNothing)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageFive(old);
    markDone();
    const auto before = filesUnder(old.signalsRoot());
    const QFile::Permissions was = QFile::permissions(old.prefsPath());
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(),
                                      QFile::ReadOwner | QFile::ReadUser));
    if (QFileInfo(old.prefsPath()).isWritable()) {
        QFile::setPermissions(old.prefsPath(), was);
        GTEST_SKIP() << "this account writes a read-only file";
    }

    const Removal r = removal(old);
    EXPECT_FALSE(r.oldBook);
    EXPECT_EQ(r.removed, 0);
    EXPECT_EQ(r.lines, QStringList{QStringLiteral(
                           "The earlier version's vehicle list couldn't be "
                           "removed: it is tried again at the next start")});
    EXPECT_EQ(filesUnder(old.signalsRoot()), before);
    EXPECT_TRUE(readBytes(old.prefsPath()).contains("document"))
        << "the old book is still in the file";
    ASSERT_TRUE(QFile::setPermissions(old.prefsPath(), was));

    // Within one process Qt hands the next QSettings the removal the failed
    // sync never wrote, so the old book reads as gone here; a later launch
    // reads the file and removes it then. The copies go either way
    const Removal later = removal(old);
    EXPECT_EQ(later.removed, 2);
}

/* A copy stays while its bytes are nowhere else: a record with no kept
 * key whose bytes plugins/unmigrated lacks keeps it, and so does kept
 * false with a file of its hash there; a record with no kept key whose
 * bytes are there loses its old copy */
TEST(BridgeTest, OneStoreRemovalSparesACopyWhoseBytesAreNowhereElse)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QString lost = old.keep("lost's bytes");
    const QString told = old.keep("told's bytes");
    const QString held = old.keep("held's bytes");
    writeBytes(keptPath(told), "told's bytes");
    writeBytes(keptPath(held), "held's bytes");
    old.set(kBook, book2({record(lost, true), record(told, false)},
                         {record(held, true)}));
    markDone();

    const Removal r = removal(old);
    EXPECT_EQ(r.removed, 1);
    EXPECT_EQ(r.spared, 2);
    QStringList left{lost + QStringLiteral(".json"),
                     told + QStringLiteral(".json")};
    left.sort();
    EXPECT_EQ(namesIn(old.signalsRoot()), left);
}

/* A copy that is there but won't open at the move (held open elsewhere;
 * here a folder of its name) is recorded kept false, so the page asks
 * nothing before its Remove and Save writes the old folder's copy */
TEST(BridgeTest, OneStoreRemovalKnowsACopyThatWouldntOpenIsNotKept)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QString held = sha256Of("held open");
    ASSERT_TRUE(QDir().mkpath(old.copyOf(held)));
    old.set(kOldBook, bookOf({vehicle(QStringLiteral("v1"), QString(),
                                      {named(held, QStringLiteral("Held"),
                                             QStringLiteral("user.held"))})}));

    const Report moved = old.start();
    const QJsonObject left = old.entry(0)
                                 .value(QStringLiteral("unmigrated"))
                                 .toArray()
                                 .at(0)
                                 .toObject();
    EXPECT_EQ(left.value(QStringLiteral("sha256")).toString(), held)
        << said(moved);
    EXPECT_EQ(left.value(QStringLiteral("kept")), QJsonValue(false));
    EXPECT_TRUE(StoreMigration::unmigrated().isEmpty());
}

/* The old book still read after the sync stops it as a failed sync does:
 * on Windows a failed registry delete sets no status, and here an
 * organization-wide file, a fallback of the scoped QSettings main()
 * makes, keeps its key through remove() */
TEST(BridgeTest, OneStoreRemovalStopsWhileTheOldBookIsReadStill)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    stageFive(old);
    markDone();
    {
        QSettings orgWide(old.path() + QStringLiteral("/Havne.ini"),
                          QSettings::IniFormat);
        orgWide.setValue(kOldBook, QStringLiteral("{\"list\":[]}"));
    }
    const auto before = filesUnder(old.signalsRoot());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, old.path());
    PluginStore store;
    QSettings prefs(QSettings::IniFormat, QSettings::UserScope,
                    QStringLiteral("Havne"), QStringLiteral("Havne"));
    ASSERT_EQ(QFileInfo(prefs.fileName()), QFileInfo(old.prefsPath()));

    const Removal r = StoreMigration::removeOld(prefs, store);
    EXPECT_FALSE(r.oldBook);
    EXPECT_EQ(r.removed, 0);
    EXPECT_EQ(r.lines, QStringList{QStringLiteral(
                           "The earlier version's vehicle list couldn't be "
                           "removed: it is tried again at the next start")});
    EXPECT_EQ(filesUnder(old.signalsRoot()), before);
    EXPECT_FALSE(old.has(kOldBook)) << "the user's own file lost its key";
}

/* Between the move and the start after it, nothing else writes to or
 * deletes from signals/: each import, keep, save and removal through the
 * bridge (runEveryStoreCall) leaves it byte for byte, and a copy that
 * couldn't be moved still saves from there */
TEST(BridgeTest, OneStoreRemovalAloneDeletesFromTheOldFolder)
{
    OldState old;
    ASSERT_TRUE(old.valid());
    const QByteArray moved = signalFile(QStringLiteral("Moved"));
    old.keep(moved);
    // A copy past what the store keeps (kept false) and one changed since
    // it was kept, whose bytes the move keeps in plugins/unmigrated
    const QString big = sha256Of("a file past the store's bound");
    writeBytes(old.copyOf(big), QByteArray());
    ASSERT_TRUE(QFile::resize(old.copyOf(big), ECUIO_PLUGIN_PACKAGE_MAX + 1));
    const QString changed = sha256Of("as it was kept");
    writeBytes(old.copyOf(changed), "changed since");
    old.set(kOldBook,
            bookOf({vehicle(
                QStringLiteral("v1"), QString(),
                {listed(moved),
                 named(big, QStringLiteral("Big"), QStringLiteral("user.big")),
                 named(changed, QStringLiteral("Changed"),
                       QStringLiteral("user.changed"))})}));
    {
        SessionBridge bridge;
        QSettings prefs(old.prefsPath(), QSettings::IniFormat);
        (void)bridge.migrateStores(prefs);
    }
    ASSERT_TRUE(marked());
    ASSERT_TRUE(QFile::exists(keptPath(changed)));
    const auto before = filesUnder(old.signalsRoot());
    ASSERT_EQ(before.size(), 3);

    SessionBridge bridge;
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    runEveryStoreCall(bridge, out.path(), big, changed);
    EXPECT_FALSE(QFile::exists(keptPath(changed)));

    EXPECT_EQ(filesUnder(old.signalsRoot()), before) << said(before);
    EXPECT_EQ(QFileInfo(out.path() + QStringLiteral("/big.json")).size(),
              ECUIO_PLUGIN_PACKAGE_MAX + 1);
}
