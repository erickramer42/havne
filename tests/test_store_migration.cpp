// File/layer: tests/test_store_migration.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.OneStoreMigration* (test_bridge)
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

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <iostream>
#include <utility>

#include "obdbimport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "storemigration.h"
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>

#ifndef ECUIO_VEHICLEBOOK_CORPUS
#error "the test target defines ECUIO_VEHICLEBOOK_CORPUS"
#endif

namespace {

using Report = StoreMigration::Report;
using Step = StoreMigration::Step;

const QString kSuffix = QStringLiteral(".havne-plugin.json");
const QString kOldBook = QStringLiteral("vehicles/document");
const QString kBook = QStringLiteral("vehicles/book");
const QString kUri = QStringLiteral("session/uri");
const QString kRecentUris = QStringLiteral("session/recentUris");
const QString kMissing =
    QStringLiteral("Its copy is missing: import the file again");
const QString kChanged = QStringLiteral(
    "Its copy has changed since it was kept: import the file again");
const QString kNoPlugin = QStringLiteral(
    "The plugin store holds no plugin made from it: import the file again");
const QString kNotKept = QStringLiteral(
    "Unmigrated signal files whose bytes couldn't be kept and stay in the "
    "old folder: 1");
const QString kAddressesStuck = QStringLiteral(
    "The saved addresses couldn't be written: the move is checked again at "
    "the next start");
const QString kIndexStuck = QStringLiteral(
    "The plugin store's index couldn't be written: the move is checked "
    "again at the next start");

QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    ASSERT_EQ(f.write(bytes), bytes.size());
}

// A signal file of one DID; with obd2 it adds to the built-in set
QByteArray signalFile(const QString& name,
                      const QString& did = QStringLiteral("D100"),
                      bool obd2 = true)
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},%3)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"a","name":"A","read":{"service":"22","number":"%2"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, did,
             obd2 ? QStringLiteral(R"("extends":"obd2",)") : QString())
        .toUtf8();
}

// A file the loader takes and a wrap refuses: written out two spaces a
// level, its one signal is past a signal file's bound
QByteArray heavyFile(const QString& name)
{
    QByteArray padding = "[1";
    for (int i = 1; i < 150000; ++i) {
        padding += ",1";
    }
    padding += "]";
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"a","name":"A","x-pad":%2,)"
               R"("read":{"service":"22","number":"D100"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, QString::fromLatin1(padding))
        .toUtf8();
}

QJsonObject named(const QString& sha, const QString& name,
                  const QString& profile)
{
    return {{QStringLiteral("sha256"), sha},
            {QStringLiteral("name"), name},
            {QStringLiteral("profile"), profile}};
}

// A file as the old book lists it: the loader's name and profile
QJsonObject listed(const QByteArray& bytes)
{
    ecuio_signal_file_t* file = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(bytes.constData(),
                                     static_cast<size_t>(bytes.size()), &file),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const QJsonObject out =
        named(sha256Of(bytes), QString::fromUtf8(ecuio_signal_file_name(file)),
              QString::fromUtf8(ecuio_signal_file_profile(file)));
    ecuio_signal_file_free(file);
    return out;
}

QString profileOf(const QJsonObject& file)
{
    return file.value(QStringLiteral("profile")).toString();
}

QJsonObject vehicle(const QString& id, const QString& uri,
                    const QJsonArray& files, const QString& own = QString(),
                    const QString& plugin = QString())
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("label"), QStringLiteral("Vehicle ") + id},
            {QStringLiteral("vinKey"), QString()},
            {QStringLiteral("uri"), uri},
            {QStringLiteral("plotted"), QJsonArray()},
            {QStringLiteral("unplotted"), QJsonArray()},
            {QStringLiteral("signals"), files},
            {QStringLiteral("own"), own},
            {QStringLiteral("plugin"), plugin},
            {QStringLiteral("baseline"), QString()}};
}

QString bookOf(const QJsonArray& vehicles)
{
    const QJsonObject doc{{QStringLiteral("salt"), QStringLiteral("5a17")},
                          {QStringLiteral("picked"), QString()},
                          {QStringLiteral("list"), vehicles}};
    return QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonArray& list)
{
    return QString::fromUtf8(
        QJsonDocument(list).toJson(QJsonDocument::Compact));
}

// Every file under a folder, its path from there to its content's hash
QMap<QString, QString> filesUnder(const QString& root)
{
    QMap<QString, QString> out;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        const QString path = it.next();
        out.insert(base.relativeFilePath(path), sha256Of(readBytes(path)));
    }
    return out;
}

std::string said(const QMap<QString, QString>& files)
{
    QStringList out;
    for (auto it = files.begin(); it != files.end(); ++it) {
        out << it.key() + QLatin1Char(' ') + it.value().left(12);
    }
    return out.join(QLatin1Char('\n')).toStdString();
}

std::string said(const Report& r)
{
    return r.lines.join(QLatin1Char('\n')).toStdString();
}

// A settings folder as an older Havne leaves it, entered while it lives
class OldState
{
public:
    OldState() : m_before(qgetenv("HAVNE_SETTINGS_DIR"))
    {
        qputenv("HAVNE_SETTINGS_DIR", m_dir.path().toUtf8());
    }
    ~OldState()
    {
        StoreMigration::stopAfterForTesting(Step::None);
        qputenv("HAVNE_SETTINGS_DIR", m_before);
    }
    OldState(const OldState&) = delete;
    OldState& operator=(const OldState&) = delete;

    bool valid() const { return m_dir.isValid(); }
    QString path() const { return m_dir.path(); }
    QString prefsPath() const
    {
        return m_dir.path() + QStringLiteral("/Havne/Havne.ini");
    }
    QString signalsRoot() const
    {
        return m_dir.path() + QStringLiteral("/signals");
    }
    QString copyOf(const QString& sha) const
    {
        return signalsRoot() + QLatin1Char('/') + sha + QStringLiteral(".json");
    }

    // A file's bytes kept as the old store keeps them: its hash
    QString keep(const QByteArray& bytes) const
    {
        const QString sha = sha256Of(bytes);
        writeBytes(copyOf(sha), bytes);
        return sha;
    }
    void set(const QString& key, const QVariant& value) const
    {
        QSettings prefs(prefsPath(), QSettings::IniFormat);
        prefs.setValue(key, value);
    }
    QString text(const QString& key) const
    {
        QSettings prefs(prefsPath(), QSettings::IniFormat);
        return prefs.value(key).toString();
    }
    bool has(const QString& key) const
    {
        QSettings prefs(prefsPath(), QSettings::IniFormat);
        return prefs.contains(key);
    }
    // Every preference but the ones the move may write
    QMap<QString, QVariant> otherPrefs() const
    {
        QSettings prefs(prefsPath(), QSettings::IniFormat);
        QMap<QString, QVariant> out;
        for (const QString& key : prefs.allKeys()) {
            if (key != kBook && key != kUri && key != kRecentUris) {
                out.insert(key, prefs.value(key));
            }
        }
        return out;
    }
    QJsonObject book() const
    {
        return QJsonDocument::fromJson(text(kBook).toUtf8()).object();
    }
    QJsonObject entry(int i) const
    {
        return book().value(QStringLiteral("list")).toArray().at(i).toObject();
    }
    QStringList picks(int i) const
    {
        QStringList out;
        for (const QJsonValue& v :
             entry(i).value(QStringLiteral("picks")).toArray()) {
            out << v.toString();
        }
        return out;
    }

    // One start of the app: the move with a plugin store of its own
    Report start() const
    {
        PluginStore store;
        QSettings prefs(prefsPath(), QSettings::IniFormat);
        return StoreMigration::run(prefs, store);
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_before;
};

// The store's kept copies, by name
QStringList copies()
{
    return QDir(PluginStore::root())
        .entryList({QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name);
}

QString indexPath()
{
    return PluginStore::root() + QStringLiteral("/store.json");
}

QJsonObject indexDoc()
{
    return QJsonDocument::fromJson(readBytes(indexPath())).object();
}

bool marked()
{
    return indexDoc().value(QStringLiteral("migrated")).toInt() == 1;
}

// The index's facts for a plugin id, {} for none
QJsonObject factsOf(const QString& id)
{
    return indexDoc()
        .value(QStringLiteral("plugins"))
        .toObject()
        .value(id)
        .toObject();
}

QString corpus(const QString& name)
{
    return QStringLiteral(ECUIO_VEHICLEBOOK_CORPUS "/") + name;
}

QString corpusText(const QString& name)
{
    return QString::fromUtf8(readBytes(corpus(name)));
}

// The corpus's signal files kept as the old store keeps them
void keepCorpusSignals(const OldState& old)
{
    const QDir dir(corpus(QStringLiteral("signals")));
    const QStringList names =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    ASSERT_EQ(names.size(), 3);
    for (const QString& name : names) {
        old.keep(readBytes(dir.filePath(name)));
    }
}

const QString kSharedSha = QStringLiteral(
    "c2452095d51fcc8dfe3f9898f2f51f86976088141ee5c379b2ca36cd3e0717f7");
const QString kObdbSha = QStringLiteral(
    "1e73cff32300794f393917e8e26c87aab8920cf91736fd43f0cea0a32b76f7e3");
const QString kOwnSha = QStringLiteral(
    "9063fca1586e1b0f69ca7e28828fa49d6df5f689598e2961f9e9672ba948fbfc");

// The corpus's two vehicles as an older Havne left them: the files, the
// book, the saved addresses, preferences the move has no business with
// and a third-party package
void stageTwoVehicles(const OldState& old)
{
    keepCorpusSignals(old);
    old.set(kOldBook, corpusText(QStringLiteral("two-vehicles.json")));
    old.set(kUri, QStringLiteral("elm327://COM5?profile=user.shared-pids"));
    old.set(kRecentUris,
            compact({QStringLiteral("elm327://COM5?profile=user.shared-pids"),
                     QStringLiteral("elm327://192.168.0.10:35000?profile=obd2"),
                     QStringLiteral("elm327://COM5?profile=user.daily-driver-"
                                    "own")}));
    old.set(QStringLiteral("session/recording"), true);
    old.set(QStringLiteral("session/adapterVersions"),
            QStringLiteral("{\"elm327://COM5\":\"ELM327 v1.4b\"}"));
    old.set(QStringLiteral("ui/zoom"), 1.25);
    old.set(QStringLiteral("live/pauseHintSeen"), true);
    old.set(QStringLiteral("recordings/paneShares"), QStringLiteral("{}"));
    old.set(QStringLiteral("dashboards/phone"),
            QStringLiteral("{\"format\":2,\"pages\":[{\"name\":\"user."
                           "shared-pids\",\"cells\":[]}]}"));
    PluginStore store;
    const QVariantMap r = store.import(packed_plugin::example());
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
}

// stageTwoVehicles with a third vehicle whose two files don't move: one
// with no copy, one whose copy changed
void stageWithUnmigrated(const OldState& old)
{
    stageTwoVehicles(old);
    QJsonObject doc =
        QJsonDocument::fromJson(old.text(kOldBook).toUtf8()).object();
    QJsonArray list = doc.value(QStringLiteral("list")).toArray();
    const QString gone = sha256Of("never kept");
    const QString changed = sha256Of("as it was kept");
    writeBytes(old.copyOf(changed), "changed since");
    list.append(vehicle(
        QStringLiteral("v3"), QStringLiteral("elm327://COM7?profile=user.gone"),
        {named(gone, QStringLiteral("Gone"), QStringLiteral("user.gone")),
         named(changed, QStringLiteral("Changed"),
               QStringLiteral("user.changed"))},
        gone));
    doc.insert(QStringLiteral("list"), list);
    old.set(kOldBook, QString::fromUtf8(
                          QJsonDocument(doc).toJson(QJsonDocument::Compact)));
}

} // namespace

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
    SessionBridge bridge;
    const QVariantMap kept = bridge.importSignalText(QString::fromUtf8(file));
    ASSERT_TRUE(kept.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(kept.value(QStringLiteral("sha256")).toString(), kObdbSha);
    (void)bridge.useSignalFiles({kObdbSha});
    const QVariantList before = bridge.profileSignals(
        QStringLiteral("mock://?profile=user.daily-driver-obdb"));
    ASSERT_GT(before.size(), 7) << "obd2's rows and the file's own";

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
        EXPECT_TRUE(bridge.migrateStores(prefs).isEmpty()) << "done once";
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
