// File/layer: app/storemigration.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.OneStoreMigration* (test_bridge)

#include "storemigration.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantList>
#include <QVariantMap>

#include <string>

#include "ecuio/plugin.h"
#include "ecuio/signals.h"
#include "obdbimport.h"
#include "pluginstore.h"
#include "recording_signals.h" /* cli/: an address given another profile */
#include "signal_file_lines.h" /* cli/: the profile an address selects */

namespace {

using Report = StoreMigration::Report;
using Step = StoreMigration::Step;

const QString kOldBook = QStringLiteral("vehicles/document");
const QString kBook = QStringLiteral("vehicles/book");
const QString kUri = QStringLiteral("session/uri");
const QString kRecentUris = QStringLiteral("session/recentUris");
const QString kList = QStringLiteral("list");
const QString kSignals = QStringLiteral("signals");
const QString kOwn = QStringLiteral("own");
const QString kPlugin = QStringLiteral("plugin");
const QString kSha = QStringLiteral("sha256");
const QString kName = QStringLiteral("name");
const QString kProfile = QStringLiteral("profile");
const QString kWords = QStringLiteral("words");
const QString kUriKey = QStringLiteral("uri");
const QString kUnmigrated = QStringLiteral("unmigrated");
const QString kJson = QStringLiteral(".json");
constexpr int kBookFormat = 2;
// The most of one unmigrated copy kept: core reads nothing larger
constexpr qint64 kKeptMaxBytes = ECUIO_PLUGIN_PACKAGE_MAX;

Step g_stopStep = Step::None;
int g_stopFile = 0;

// Whether the seam ends the run here; it ends one run
bool stopsAfter(Step step, int file = 0)
{
    if (g_stopStep != step || (step == Step::File && g_stopFile != file)) {
        return false;
    }
    g_stopStep = Step::None;
    return true;
}

QString shaOf(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// A SHA-256 as both stores name a file: 64 lower-case hex digits
bool isSha(const QString& s)
{
    if (s.size() != 64) {
        return false;
    }
    for (const QChar c : s) {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool letter = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

QString oldPath(const QString& sha)
{
    return StoreMigration::oldSignalsRoot() + QLatin1Char('/') + sha + kJson;
}

QString unmigratedPath(const QString& sha)
{
    return StoreMigration::unmigratedRoot() + QLatin1Char('/') + sha + kJson;
}

// The hashes a folder's <sha256>.json files are named by, in name order
QStringList shasIn(const QString& folder)
{
    QStringList out;
    const QStringList names = QDir(folder).entryList(
        {QStringLiteral("*") + kJson}, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        const QString sha = name.chopped(kJson.size());
        if (isSha(sha)) {
            out << sha;
        }
    }
    return out;
}

// A preference's text: "" for none and for a list, which has no text
QString textOf(const QSettings& prefs, const QString& key)
{
    return prefs.value(key).toString();
}

// Whether a preference holds anything but empty text
bool holds(const QSettings& prefs, const QString& key)
{
    const QVariant v = prefs.value(key);
    return v.isValid() &&
           (v.typeId() != QMetaType::QString || !v.toString().isEmpty());
}

// The old book: no key is an empty book; text that isn't an object with a
// list is one that can't be read, which an older Havne shows as empty
struct OldBook {
    bool readable = true;
    QJsonObject doc;
    QJsonArray list() const { return doc.value(kList).toArray(); }
};

QJsonObject emptyBook()
{
    return {{QStringLiteral("salt"), QString()},
            {QStringLiteral("picked"), QString()},
            {kList, QJsonArray()}};
}

OldBook readOldBook(const QSettings& prefs)
{
    OldBook old;
    old.doc = emptyBook();
    if (!holds(prefs, kOldBook)) {
        return old;
    }
    const QJsonDocument d =
        QJsonDocument::fromJson(textOf(prefs, kOldBook).toUtf8());
    old.readable = d.isObject() && d.object().value(kList).isArray();
    if (old.readable) {
        old.doc = d.object();
    }
    return old;
}

// The old copy's bytes as far as the loader reads, and whether they are
// the file the book names
enum class Copy { Missing, Changed, Whole };

Copy readCopy(const QString& sha, QByteArray* bytes)
{
    QFile f(oldPath(sha));
    if (!f.open(QIODevice::ReadOnly)) {
        return Copy::Missing;
    }
    *bytes = f.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1);
    return shaOf(*bytes) == sha ? Copy::Whole : Copy::Changed;
}

struct OldFile {
    QString sha;
    QString name;
    QString profile;
    // The vehicle a wrap is told of: the first whose form wrote the file,
    // else the first that has it
    QString vehicle;
    bool own = false;
};

// An entry's files: each element of signals that names a copy
QList<OldFile> filesOf(const QJsonObject& entry)
{
    QList<OldFile> out;
    const QJsonArray files = entry.value(kSignals).toArray();
    for (const auto& v : files) {
        const QJsonObject f = v.toObject();
        OldFile file;
        file.sha = f.value(kSha).toString();
        if (!isSha(file.sha)) {
            continue;
        }
        file.name = f.value(kName).toString();
        file.profile = f.value(kProfile).toString();
        out << file;
    }
    return out;
}

// Every file the book names, once, in the vehicles' order then each
// vehicle's own
QList<OldFile> referenced(const QJsonArray& list)
{
    QList<OldFile> out;
    QHash<QString, qsizetype> placeOf;
    for (const auto& v : list) {
        const QJsonObject entry = v.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const QString own = entry.value(kOwn).toString();
        for (OldFile f : filesOf(entry)) {
            if (!placeOf.contains(f.sha)) {
                placeOf.insert(f.sha, out.size());
                f.vehicle = id;
                out << f;
            }
            OldFile& first = out[placeOf.value(f.sha)];
            if (f.sha == own && !first.own) {
                first.own = true;
                first.vehicle = id;
            }
        }
    }
    return out;
}

// The profile a copy made in an older Havne (user.<slug>), "" for one
// that doesn't load: what a saved address names it by when no book does
QString profileInCopy(const QString& sha)
{
    QByteArray bytes;
    ecuio_signal_file_t* file = nullptr;
    (void)readCopy(sha, &bytes);
    if (ecuio_signal_file_load(bytes.constData(),
                               static_cast<size_t>(bytes.size()),
                               &file) != ECUIO_OK) {
        return QString();
    }
    const QString profile = QString::fromUtf8(ecuio_signal_file_profile(file));
    ecuio_signal_file_free(file);
    return profile;
}

// The files to move: the ones the book names; with no book that reads,
// every copy, for no vehicle
QList<OldFile> oldFiles(const OldBook& old)
{
    if (old.readable) {
        return referenced(old.list());
    }
    QList<OldFile> out;
    const QStringList copies = shasIn(StoreMigration::oldSignalsRoot());
    for (const QString& sha : copies) {
        OldFile f;
        f.sha = sha;
        f.profile = profileInCopy(sha);
        out << f;
    }
    return out;
}

// The profile a file became, "" for a file that isn't moved: the variant
// of the plugin made from it, else of the first plugin given it
QString profileOf(PluginStore& store, const QString& sha)
{
    const QVariantList made = store.wrappedFrom(sha);
    return made.isEmpty()
               ? QString()
               : made.constFirst().toMap().value(kProfile).toString();
}

// Each moved file's old profile to its new one; the old book lets a
// profile name one file
QHash<QString, QString> renamesOf(const QList<OldFile>& files,
                                  PluginStore& store)
{
    QHash<QString, QString> out;
    for (const OldFile& f : files) {
        const QString now = profileOf(store, f.sha);
        if (!now.isEmpty() && !f.profile.isEmpty()) {
            out.insert(f.profile, now);
        }
    }
    return out;
}

// An address whose profile (the last profile=, as core reads it) is a
// moved file's, with the plugin's instead; any other as it is
QString renamed(const QString& uri, const QHash<QString, QString>& renames)
{
    const std::string text = uri.toStdString();
    std::string selected;
    if (!cli::uri_profile(text, &selected)) {
        return uri;
    }
    const auto found = renames.constFind(QString::fromStdString(selected));
    return found == renames.constEnd()
               ? uri
               : QString::fromStdString(
                     cli::with_profile(text, found->toStdString()));
}

// One file through the store's wrap: why it didn't go, "" when the wrap
// took it (*fresh: this run made its plugin)
QString wrapFile(PluginStore& store, const OldFile& f, bool* fresh)
{
    QByteArray bytes;
    switch (readCopy(f.sha, &bytes)) {
    case Copy::Missing:
        return QStringLiteral("Its copy is missing: import the file again");
    case Copy::Changed:
        return QStringLiteral("Its copy has changed since it was kept: "
                              "import the file again");
    case Copy::Whole:
        break;
    }
    PluginStore::Wrap how;
    if (f.own || ObdbImport::isConversion(bytes)) {
        how.vehicle = f.vehicle;
    }
    const QVariantMap r = store.wrap(bytes, how);
    const bool ok = r.value(QStringLiteral("ok")).toBool();
    *fresh = ok && !r.value(QStringLiteral("kept")).toBool();
    return ok ? QString() : r.value(kWords).toString();
}

// An unmigrated file's bytes into the plugin store's folder, whole
enum class Bytes { None, Kept, NotKept };

Bytes keepBytes(const QString& sha)
{
    QFile from(oldPath(sha));
    if (!from.open(QIODevice::ReadOnly)) {
        return Bytes::None;
    }
    if (from.size() > kKeptMaxBytes) {
        return Bytes::NotKept;
    }
    const QByteArray bytes = from.readAll();
    QSaveFile to(unmigratedPath(sha));
    const bool kept = QDir().mkpath(StoreMigration::unmigratedRoot()) &&
                      to.open(QIODevice::WriteOnly) &&
                      to.write(bytes) == bytes.size() && to.commit();
    return kept ? Bytes::Kept : Bytes::NotKept;
}

// The files that didn't move: each one's words, the ones whose bytes are
// still only in the old folder, and the ones no vehicle has
struct NotMoved {
    QHash<QString, QString> why;
    QStringList unkept;
    QStringList loose;
};

// A file that didn't move as the book records it. kept false: its bytes
// are only in the old folder, which that folder's removal must spare
QJsonObject record(const QString& sha, const QString& name,
                   const NotMoved& left)
{
    QJsonObject r{{kSha, sha}, {kName, name}, {kWords, left.why.value(sha)}};
    if (left.unkept.contains(sha)) {
        r.insert(QStringLiteral("kept"), false);
    }
    return r;
}

// Format 2 of the book: each entry as it is but for its files, which
// become picks, own and unmigrated, and its address, which is text. What
// isn't an entry is left out: it holds nothing a page shows
QJsonObject newBook(const OldBook& old, PluginStore& store,
                    const NotMoved& left,
                    const QHash<QString, QString>& renames)
{
    QJsonArray list;
    const QJsonArray entries = old.list();
    for (const auto& v : entries) {
        if (!v.isObject()) {
            continue;
        }
        QJsonObject entry = v.toObject();
        const QString own = entry.value(kOwn).toString();
        QJsonArray picks;
        QJsonArray unmigrated;
        QString ownProfile;
        for (const OldFile& f : filesOf(entry)) {
            const QString profile = profileOf(store, f.sha);
            if (profile.isEmpty()) {
                unmigrated.append(record(f.sha, f.name, left));
                continue;
            }
            if (!picks.contains(profile)) {
                picks.append(profile);
            }
            if (f.sha == own) {
                ownProfile = profile;
            }
        }
        const QString plugin = entry.value(kPlugin).toString();
        if (!plugin.isEmpty() && !picks.contains(plugin)) {
            picks.append(plugin);
        }
        entry.remove(kSignals);
        entry.remove(kPlugin);
        entry.insert(QStringLiteral("picks"), picks);
        entry.insert(kOwn, ownProfile);
        if (!unmigrated.isEmpty()) {
            entry.insert(kUnmigrated, unmigrated);
        }
        entry.insert(kUriKey,
                     renamed(entry.value(kUriKey).toString(), renames));
        list.append(entry);
    }
    QJsonObject book = old.doc;
    book.insert(QStringLiteral("format"), kBookFormat);
    book.insert(kList, list);
    QJsonArray loose;
    for (const QString& sha : left.loose) {
        loose.append(record(sha, QString(), left));
    }
    if (!loose.isEmpty()) {
        book.insert(kUnmigrated, loose);
    }
    return book;
}

// The files into the store, the bytes of those that didn't move kept, and
// the new book written: false when the run ends here (the seam, or a
// book that can't be saved)
bool moveFiles(QSettings& prefs, PluginStore& store, const OldBook& old,
               Report* report)
{
    const QList<OldFile> files = oldFiles(old);
    if (old.readable) {
        QStringList named;
        for (const OldFile& f : files) {
            named << f.sha;
        }
        const QStringList copies = shasIn(StoreMigration::oldSignalsRoot());
        for (const QString& sha : copies) {
            report->unnamed += named.contains(sha) ? 0 : 1;
        }
    }

    QHash<QString, QString> refused;
    QStringList fresh;
    int n = 0;
    for (const OldFile& f : files) {
        bool made = false;
        const QString words = wrapFile(store, f, &made);
        if (!words.isEmpty()) {
            refused.insert(f.sha, words);
        }
        if (made) {
            fresh << f.sha;
        }
        if (stopsAfter(Step::File, ++n)) {
            return false;
        }
    }
    if (stopsAfter(Step::Wrapped)) {
        return false;
    }

    // Judged once every file is in: a later file can take an earlier
    // one's place in a plugin, and that one then has none
    NotMoved left;
    for (const OldFile& f : files) {
        if (!profileOf(store, f.sha).isEmpty()) {
            // A copy an earlier run kept, while its wrap failed, is no one's now
            (void)StoreMigration::removeUnmigrated(f.sha);
            report->made += fresh.contains(f.sha) ? 1 : 0;
            report->kept += fresh.contains(f.sha) ? 0 : 1;
            continue;
        }
        const QString words = refused.value(
            f.sha, QStringLiteral("The plugin store holds no plugin made "
                                  "from it: import the file again"));
        left.why.insert(f.sha, words);
        if (!old.readable) {
            // No vehicle lists it, so its bytes stay where they are
            ++report->unnamed;
            left.loose << f.sha;
            left.unkept << f.sha;
            continue;
        }
        ++report->unmigrated;
        report->lines << QStringLiteral("Signal file not moved: %1: %2")
                             .arg(f.name.isEmpty() ? f.sha.left(12) : f.name,
                                  words);
        if (keepBytes(f.sha) == Bytes::NotKept) {
            left.unkept << f.sha;
        }
    }
    if (old.readable && !left.unkept.isEmpty()) {
        report->lines << QStringLiteral("Unmigrated signal files whose bytes "
                                        "couldn't be kept and stay in the "
                                        "old folder: %1")
                             .arg(left.unkept.size());
    }
    if (stopsAfter(Step::Copied)) {
        return false;
    }

    const QJsonObject book = newBook(old, store, left, renamesOf(files, store));
    prefs.setValue(kBook, QString::fromUtf8(QJsonDocument(book).toJson(
                              QJsonDocument::Compact)));
    prefs.sync();
    if (prefs.status() != QSettings::NoError) {
        report->lines << QStringLiteral("The vehicle list couldn't be saved: "
                                        "the move runs again at the next "
                                        "start");
        return false;
    }
    return !stopsAfter(Step::Book);
}

// session/uri and each address of session/recentUris; a list that isn't
// an array of text is left as it is. false: they couldn't be written
bool renameAddresses(QSettings& prefs, const QHash<QString, QString>& renames,
                     Report* report)
{
    const QString uri = textOf(prefs, kUri);
    const QString now = renamed(uri, renames);
    if (now != uri) {
        prefs.setValue(kUri, now);
        ++report->addresses;
    }
    const QJsonDocument recent =
        QJsonDocument::fromJson(textOf(prefs, kRecentUris).toUtf8());
    QJsonArray list = recent.array();
    int changed = 0;
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (!list.at(i).isString()) {
            changed = 0;
            break;
        }
        const QString was = list.at(i).toString();
        const QString is = renamed(was, renames);
        if (is != was) {
            list.replace(i, is);
            ++changed;
        }
    }
    if (changed > 0) {
        prefs.setValue(kRecentUris,
                       QString::fromUtf8(
                           QJsonDocument(list).toJson(QJsonDocument::Compact)));
        report->addresses += changed;
    }
    prefs.sync();
    return prefs.status() == QSettings::NoError;
}

} // namespace

QString StoreMigration::oldSignalsRoot()
{
    const QString dir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    const QString base =
        dir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : dir;
    return base + QStringLiteral("/signals");
}

QString StoreMigration::unmigratedRoot()
{
    return PluginStore::root() + QStringLiteral("/unmigrated");
}

QStringList StoreMigration::unmigrated()
{
    return shasIn(unmigratedRoot());
}

QByteArray StoreMigration::unmigratedBytes(const QString& sha, QString* words)
{
    QFile f(unmigratedPath(sha));
    if (!isSha(sha) || !f.open(QIODevice::ReadOnly)) {
        *words = QStringLiteral("Its bytes weren't kept");
        return {};
    }
    return f.read(kKeptMaxBytes);
}

bool StoreMigration::removeUnmigrated(const QString& sha)
{
    if (!isSha(sha)) {
        return false;
    }
    const QString path = unmigratedPath(sha);
    const bool gone = !QFile::exists(path) || QFile::remove(path);
    // The folder goes with its last file; rmdir leaves one that holds any
    (void)QDir().rmdir(unmigratedRoot());
    return gone;
}

QByteArray StoreMigration::oldCopy(const QString& sha, QString* words)
{
    QFile f(oldPath(sha));
    if (!isSha(sha) || !f.open(QIODevice::ReadOnly)) {
        *words = QStringLiteral("Its copy is gone from the earlier version's "
                                "folder");
        return {};
    }
    QByteArray bytes = f.read(OldCopyMaxBytes + 1);
    if (bytes.size() > OldCopyMaxBytes) {
        *words = QStringLiteral("Its copy is over %1 MiB")
                     .arg(OldCopyMaxBytes / (1024 * 1024));
        return {};
    }
    if (bytes.isEmpty()) {
        *words = QStringLiteral("Its copy is empty");
    }
    return bytes;
}

void StoreMigration::stopAfterForTesting(Step step, int file)
{
    g_stopStep = step;
    g_stopFile = file;
}

StoreMigration::Report StoreMigration::run(QSettings& prefs, PluginStore& store)
{
    Report report;
    if (store.migrated()) {
        return report;
    }
    QElapsedTimer clock;
    clock.start();
    report.ran = true;
    const auto ended = [&](bool stopped) {
        report.stopped = stopped;
        report.elapsedMs = clock.elapsed();
        return report;
    };

    const OldBook old = readOldBook(prefs);
    // A book that is there is the user's: the files it was made from are
    // moved already, and are not moved again
    if (!holds(prefs, kBook)) {
        if (!moveFiles(prefs, store, old, &report)) {
            return ended(prefs.status() == QSettings::NoError);
        }
        if (report.made + report.kept > 0) {
            report.lines.prepend(
                QStringLiteral("Signal files moved into the plugin store: %1 "
                               "made into plugins, %2 there already")
                    .arg(report.made)
                    .arg(report.kept));
        }
        if (!old.readable) {
            report.lines << QStringLiteral(
                "The vehicle list couldn't be read: every stored signal file "
                "that loads is now a plugin of your own, picked by no "
                "vehicle");
        }
        if (report.unnamed > 0) {
            report.lines << QStringLiteral("Signal files left in the old "
                                           "folder: %1")
                                .arg(report.unnamed);
        }
    }

    if (!renameAddresses(prefs, renamesOf(oldFiles(old), store), &report)) {
        report.addresses = 0;
        report.lines << QStringLiteral("The saved addresses couldn't be "
                                       "written: the move is checked again "
                                       "at the next start");
        return ended(false);
    }
    if (report.addresses > 0) {
        report.lines << QStringLiteral("Saved addresses that now name a "
                                       "plugin's profile: %1")
                            .arg(report.addresses);
    }
    if (stopsAfter(Step::Addresses)) {
        return ended(true);
    }

    report.marked = store.setMigrated();
    if (!report.marked) {
        report.lines << QStringLiteral("The plugin store's index couldn't be "
                                       "written: the move is checked again "
                                       "at the next start");
    }
    return ended(false);
}
