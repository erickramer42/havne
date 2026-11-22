// File/layer: tests/old_state.h | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.OneStoreMigration*, .OneStoreRemoval* (test_bridge)
//
// An older Havne's settings folder as the move of the signal files finds
// it (ADR-0064 Q6), for the cases of the move and of the removal of the
// earlier version's data: OldState stages the folder, and the helpers
// read what a start leaves.

#pragma once


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
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#ifndef ECUIO_VEHICLEBOOK_CORPUS
#error "the test target defines ECUIO_VEHICLEBOOK_CORPUS"
#endif

namespace old_state {

using Report = StoreMigration::Report;
using Step = StoreMigration::Step;

inline const QString kSuffix = QStringLiteral(".havne-plugin.json");
inline const QString kOldBook = QStringLiteral("vehicles/document");
inline const QString kBook = QStringLiteral("vehicles/book");
inline const QString kUri = QStringLiteral("session/uri");
inline const QString kRecentUris = QStringLiteral("session/recentUris");
inline const QString kMissing =
    QStringLiteral("Its copy is missing: import the file again");
inline const QString kChanged = QStringLiteral(
    "Its copy has changed since it was kept: import the file again");
inline const QString kNoPlugin = QStringLiteral(
    "The plugin store holds no plugin made from it: import the file again");
inline const QString kNotKept = QStringLiteral(
    "Unmigrated signal files whose bytes couldn't be kept and stay in the "
    "old folder: 1");
inline const QString kAddressesStuck = QStringLiteral(
    "The saved addresses couldn't be written: the move is checked again at "
    "the next start");
inline const QString kIndexStuck = QStringLiteral(
    "The plugin store's index couldn't be written: the move is checked "
    "again at the next start");

inline QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

inline void writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    ASSERT_EQ(f.write(bytes), bytes.size());
}

// A signal file of one DID; with obd2 it adds to the built-in set
inline QByteArray signalFile(const QString& name,
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
// level, its one signal is past a set's bytes
inline QByteArray heavyFile(const QString& name)
{
    QByteArray padding = "[[[[[[[[[[[[[[[[[[[[[1";
    for (int i = 1; i < 150000; ++i) {
        padding += ",1";
    }
    padding += "]]]]]]]]]]]]]]]]]]]]]";
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
               R"({"key":"a","name":"A","x-pad":%2,)"
               R"("read":{"service":"22","number":"D100"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, QString::fromLatin1(padding))
        .toUtf8();
}

inline QJsonObject named(const QString& sha, const QString& name,
                  const QString& profile)
{
    return {{QStringLiteral("sha256"), sha},
            {QStringLiteral("name"), name},
            {QStringLiteral("profile"), profile}};
}

// A file as the old book lists it: the loader's name and profile
inline QJsonObject listed(const QByteArray& bytes)
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

inline QString profileOf(const QJsonObject& file)
{
    return file.value(QStringLiteral("profile")).toString();
}

inline QJsonObject vehicle(const QString& id, const QString& uri,
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

inline QString bookOf(const QJsonArray& vehicles)
{
    const QJsonObject doc{{QStringLiteral("salt"), QStringLiteral("5a17")},
                          {QStringLiteral("picked"), QString()},
                          {QStringLiteral("list"), vehicles}};
    return QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
}

inline QString compact(const QJsonArray& list)
{
    return QString::fromUtf8(
        QJsonDocument(list).toJson(QJsonDocument::Compact));
}

// Every file under a folder, its path from there to its content's hash
inline QMap<QString, QString> filesUnder(const QString& root)
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

inline std::string said(const QMap<QString, QString>& files)
{
    QStringList out;
    for (auto it = files.begin(); it != files.end(); ++it) {
        out << it.key() + QLatin1Char(' ') + it.value().left(12);
    }
    return out.join(QLatin1Char('\n')).toStdString();
}

inline std::string said(const Report& r)
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
            // The move's own keys: the book, and the mark that says every
            // copy no vehicle names has its record
            if (key != kBook && key != kUri && key != kRecentUris &&
                key != QStringLiteral("vehicles/unnamedRecorded")) {
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
inline QStringList copies()
{
    return QDir(PluginStore::root())
        .entryList({QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name);
}

inline QString indexPath()
{
    return PluginStore::root() + QStringLiteral("/store.json");
}

inline QJsonObject indexDoc()
{
    return QJsonDocument::fromJson(readBytes(indexPath())).object();
}

inline bool marked()
{
    return indexDoc().value(QStringLiteral("migrated")).toInt() == 1;
}

// The index's facts for a plugin id, {} for none
inline QJsonObject factsOf(const QString& id)
{
    return indexDoc()
        .value(QStringLiteral("plugins"))
        .toObject()
        .value(id)
        .toObject();
}

inline QString corpus(const QString& name)
{
    return QStringLiteral(ECUIO_VEHICLEBOOK_CORPUS "/") + name;
}

inline QString corpusText(const QString& name)
{
    return QString::fromUtf8(readBytes(corpus(name)));
}

// The corpus's signal files kept as the old store keeps them; with a
// book, only those it names, since a copy no vehicle names gets a record
// of the book's own
inline void keepCorpusSignals(const OldState& old,
                              const QString& book = QString())
{
    const QDir dir(corpus(QStringLiteral("signals")));
    const QStringList names =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    ASSERT_EQ(names.size(), 3);
    for (const QString& name : names) {
        const QByteArray bytes = readBytes(dir.filePath(name));
        if (book.isEmpty() || book.contains(sha256Of(bytes))) {
            old.keep(bytes);
        }
    }
}

inline const QString kSharedSha = QStringLiteral(
    "c2452095d51fcc8dfe3f9898f2f51f86976088141ee5c379b2ca36cd3e0717f7");
inline const QString kObdbSha = QStringLiteral(
    "1e73cff32300794f393917e8e26c87aab8920cf91736fd43f0cea0a32b76f7e3");
inline const QString kOwnSha = QStringLiteral(
    "9063fca1586e1b0f69ca7e28828fa49d6df5f689598e2961f9e9672ba948fbfc");

// The corpus's two vehicles as an older Havne left them: the files, the
// book, the saved addresses, preferences the move has no business with
// and a third-party package
inline void stageTwoVehicles(const OldState& old)
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
inline void stageWithUnmigrated(const OldState& old)
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

} // namespace old_state
