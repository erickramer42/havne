// File/layer: app/pluginstore.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge)

#include "pluginstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/vin.h"
#include "obdbimport.h"

namespace {

const QString kSuffix = QStringLiteral(".havne-plugin.json");
const QString kPlugins = QStringLiteral("plugins");
const QString kMine = QStringLiteral("mine");
const QString kImportedFor = QStringLiteral("importedFor");
const QString kWrappedFrom = QStringLiteral("wrappedFrom");
const QString kMigrated = QStringLiteral("migrated");
const QString kFormat = QStringLiteral("format");
const QString kReplacedProfiles = QStringLiteral("replacedProfiles");
const QString kBase = QStringLiteral("base");
constexpr int kIndexFormat = 1;
constexpr qint64 kIndexMaxBytes = 4 * 1024 * 1024;
// The ids a wrap tries for one name: its own, then -2 to -99
constexpr int kIdTries = 99;
// The names an index moved aside tries: store.json.bad-1 to -99
constexpr int kAsideTries = 99;

QString shaOf(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Whether the copy at path already holds bytes: a copy changed since is
// written again
bool holds(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(bytes.size() + 1) == bytes;
}

// Core's words: the detail, else the status
QString coreWords(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        return QString::fromUtf8(detail);
    }
    return QString::fromUtf8(ecuio_status_string(rc));
}

QVariantMap refusal(const QString& words)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), words}};
}

// The package read: its plugin loaded, nothing registered; nullptr with
// *words saying why not
ecuio_plugin_t* loadPackage(const QByteArray& bytes, QString* words)
{
    ecuio_plugin_t* plugin = nullptr;
    const ecuio_status_t rc =
        ecuio_plugin_load(bytes.constData(), static_cast<size_t>(bytes.size()),
                          nullptr, nullptr, &plugin);
    if (rc != ECUIO_OK) {
        *words = coreWords(rc);
        return nullptr;
    }
    return plugin;
}

// What a package says of itself
struct Named {
    QString id;
    QString name;
    QString version;
    QString lineage;
};

Named namedOf(const ecuio_plugin_t* plugin)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    (void)ecuio_plugin_info(plugin, &info);
    std::array<char, ECUIO_PLUGIN_LINEAGE_LEN + 1> lineage{};
    (void)ecuio_plugin_lineage(plugin, lineage.data(), lineage.size());
    return {QString::fromUtf8(info.id), QString::fromUtf8(info.name),
            QString::fromUtf8(info.version),
            QString::fromLatin1(lineage.data())};
}

// A package's own words for itself, read and freed; false with *words
bool nameOf(const QByteArray& bytes, Named* out, QString* words)
{
    ecuio_plugin_t* probe = loadPackage(bytes, words);
    if (probe == nullptr) {
        return false;
    }
    *out = namedOf(probe);
    (void)ecuio_plugin_free(probe);
    return true;
}

// X.Y.Z as three numbers (the loader has checked the form): below zero
// when a is the lower version, above when the higher
int versionOrder(const QString& a, const QString& b)
{
    const QStringList x = a.split(QLatin1Char('.'));
    const QStringList y = b.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < 3; ++i) {
        const uint p = i < x.size() ? x.at(i).toUInt() : 0u;
        const uint q = i < y.size() ? y.at(i).toUInt() : 0u;
        if (p != q) {
            return p < q ? -1 : 1;
        }
    }
    return 0;
}

// An id cut to max bytes with no '-' left at its end, as core cuts a slug
QString cutId(const QString& id, qsizetype max)
{
    QString cut = id.left(max);
    while (cut.endsWith(QLatin1Char('-'))) {
        cut.chop(1);
    }
    return cut;
}

// base numbered n ("-2" and on), cut so the id stays within its bound
QString numbered(const QString& base, int n)
{
    const QString tail = QStringLiteral("-%1").arg(n);
    return cutId(base,
                 static_cast<qsizetype>(ECUIO_PLUGIN_ID_MAX) - tail.size()) +
           tail;
}

// A text core gave, released at the scope's end
struct CoreText {
    char* text = nullptr;
    size_t length = 0;
    CoreText() = default;
    ~CoreText() { ecuio_plugin_text_free(text); }
    CoreText(const CoreText&) = delete;
    CoreText& operator=(const CoreText&) = delete;
    QByteArray bytes() const
    {
        return text != nullptr
                   ? QByteArray(text, static_cast<qsizetype>(length))
                   : QByteArray();
    }
};

// A signal file as a packed plugin of one variant, by core: empty with
// *words when core refuses it. id and variant "" are core's own
QByteArray wrapped(const QByteArray& file, const QString& id,
                   const QString& variant, QString* words)
{
    const QByteArray idText = id.toUtf8();
    const QByteArray variantText = variant.toUtf8();
    ecuio_plugin_wrap_options_t options{};
    options.struct_size = sizeof options;
    options.id = id.isEmpty() ? nullptr : idText.constData();
    options.variant = variant.isEmpty() ? nullptr : variantText.constData();
    CoreText out;
    const ecuio_status_t rc =
        ecuio_plugin_wrap(file.constData(), static_cast<size_t>(file.size()),
                          &options, &out.text, &out.length);
    if (rc != ECUIO_OK) {
        *words = coreWords(rc);
        return {};
    }
    return out.bytes();
}

enum class IndexFile { None, Read, Bad };

// The index as its file gives it. Bad: a file that is there and isn't
// an index this store can trust (unreadable, too large, not an object)
IndexFile readIndexFile(const QString& path, QJsonObject* out)
{
    QFile f(path);
    if (!f.exists()) {
        return IndexFile::None;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        return IndexFile::Bad;
    }
    const QByteArray bytes = f.read(kIndexMaxBytes + 1);
    if (bytes.size() > kIndexMaxBytes) {
        return IndexFile::Bad;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    const QJsonObject index = doc.object();
    if (!doc.isObject() ||
        (index.contains(kPlugins) && !index.value(kPlugins).isObject())) {
        return IndexFile::Bad;
    }
    *out = index;
    return IndexFile::Read;
}

// A later Havne's index: read for what this version knows, never written
bool laterFormat(const QJsonObject& index)
{
    return index.value(kFormat).toInt() > kIndexFormat;
}

// The index written whole. One that says nothing leaves no file, so an
// emptied store's folder can go
bool writeIndexFile(const QString& folder, const QString& path, QJsonObject doc)
{
    doc.insert(kFormat, kIndexFormat);
    QJsonObject said = doc;
    said.remove(kFormat);
    said.remove(kPlugins);
    if (said.isEmpty() && doc.value(kPlugins).toObject().isEmpty()) {
        return !QFile::exists(path) || QFile::remove(path);
    }
    QSaveFile out(path);
    const QByteArray text = QJsonDocument(doc).toJson(QJsonDocument::Indented);
    return QDir().mkpath(folder) && out.open(QIODevice::WriteOnly) &&
           out.write(text) == text.size() && out.commit();
}

} // namespace

QString PluginStore::root()
{
    const QString dir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    const QString base =
        dir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : dir;
    return base + QStringLiteral("/plugins");
}

QString PluginStore::pathOf(const QString& sha)
{
    return root() + QLatin1Char('/') + sha + kSuffix;
}

// Not a *.havne-plugin.json name, so an older Havne's store passes it by
QString PluginStore::indexPath()
{
    return root() + QStringLiteral("/store.json");
}

PluginStore::~PluginStore()
{
    close();
}

void PluginStore::say(const QString& line) const
{
    if (m_log) {
        m_log(line);
    }
}

void PluginStore::loadIndex()
{
    if (m_indexLoaded) {
        return;
    }
    m_indexLoaded = true;
    QJsonObject read;
    switch (readIndexFile(indexPath(), &read)) {
    case IndexFile::Read:
        m_index = read;
        if (laterFormat(m_index)) {
            say(QStringLiteral(
                "The plugin store's index is a later Havne's: it is read "
                "and left as it is, so what is marked here holds until the "
                "app closes"));
        }
        break;
    case IndexFile::Bad:
        (void)indexAside();
        break;
    case IndexFile::None:
        break;
    }
}

bool PluginStore::indexAside()
{
    for (int n = 1; n <= kAsideTries; ++n) {
        const QString aside = indexPath() + QStringLiteral(".bad-%1").arg(n);
        if (QFile::exists(aside)) {
            continue;
        }
        if (!QFile::rename(indexPath(), aside)) {
            break;
        }
        m_indexStuckSaid = false;
        say(QStringLiteral("The plugin store's index couldn't be read: it is "
                           "kept as store.json.bad-%1 and a new one is "
                           "started")
                .arg(n));
        return true;
    }
    if (!m_indexStuckSaid) {
        m_indexStuckSaid = true;
        say(QStringLiteral(
            "The plugin store's index can't be read or moved aside: it is "
            "left as it is, and what is marked here holds until the app "
            "closes"));
    }
    return false;
}

bool PluginStore::changeIndex(const std::function<bool(QJsonObject&)>& change)
{
    loadIndex();
    QJsonObject file;
    const IndexFile read = readIndexFile(indexPath(), &file);
    // What another running app wrote since is the base, unless memory
    // holds a change the file never got
    if (!m_indexUnsaved && read != IndexFile::Bad) {
        m_index = file;
    }
    if (!change(m_index)) {
        return true;
    }
    // A file that can't be trusted is never written over
    const bool stuck = read == IndexFile::Bad && !indexAside();
    const bool fail = m_failIndexWriteForTesting;
    m_failIndexWriteForTesting = false;
    const bool written = !stuck && !fail && !laterFormat(m_index) &&
                         writeIndexFile(root(), indexPath(), m_index);
    m_indexUnsaved = !written;
    return written;
}

bool PluginStore::changeFacts(const QString& id,
                              const std::function<void(QJsonObject&)>& change)
{
    return changeIndex([&](QJsonObject& index) {
        QJsonObject plugins = index.value(kPlugins).toObject();
        QJsonObject f = plugins.value(id).toObject();
        change(f);
        if (f.isEmpty()) {
            plugins.remove(id);
        } else {
            plugins.insert(id, f);
        }
        index.insert(kPlugins, plugins);
        return true;
    });
}

void PluginStore::settleFacts(const QString& id, bool cameNew,
                              const QStringList& variants)
{
    const auto gone = [&variants](const QJsonObject& from) {
        for (auto it = from.begin(); it != from.end(); ++it) {
            if (!variants.contains(it.value().toString())) {
                return true;
            }
        }
        return false;
    };
    const QJsonObject before = facts(id);
    if (before.isEmpty() ||
        (!cameNew && !gone(before.value(kWrappedFrom).toObject()))) {
        return;
    }
    (void)changeFacts(id, [&](QJsonObject& f) {
        if (cameNew) {
            // Facts with no plugin loaded were of a copy that is gone or
            // no longer loads: this plugin is not that one
            f = QJsonObject();
            return;
        }
        QJsonObject from = f.value(kWrappedFrom).toObject();
        for (auto it = from.begin(); it != from.end();) {
            if (variants.contains(it.value().toString())) {
                ++it;
            } else {
                it = from.erase(it);
            }
        }
        if (from.isEmpty()) {
            f.remove(kWrappedFrom);
        } else {
            f.insert(kWrappedFrom, from);
        }
    });
}

QJsonObject PluginStore::facts(const QString& id) const
{
    return m_index.value(kPlugins).toObject().value(id).toObject();
}

QVariantMap PluginStore::keptEntry(const QString& sha) const
{
    QVariantMap out = entryOf(sha, m_held.at(sha));
    out.insert(QStringLiteral("kept"), true);
    out.insert(QStringLiteral("replaced"), QString());
    out.insert(kReplacedProfiles, QStringList());
    out.insert(QStringLiteral("updated"), QString());
    return out;
}

bool PluginStore::isMine(const QString& id) const
{
    return facts(id).value(kMine).toBool();
}

QVariantMap PluginStore::entryOf(const QString& sha, const Held& h) const
{
    const QJsonObject f = facts(h.id);
    return {{QStringLiteral("sha256"), sha},
            {QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("id"), h.id},
            {QStringLiteral("name"), h.name},
            {QStringLiteral("version"), h.version},
            {QStringLiteral("lineage"), h.lineage},
            {QStringLiteral("variants"), h.variants},
            {QStringLiteral("profiles"), h.profiles},
            {QStringLiteral("extends"), h.extends},
            {QStringLiteral("counts"), h.counts},
            {QStringLiteral("owns"), h.owns},
            {QStringLiteral("mine"), f.value(kMine).toBool()},
            {QStringLiteral("importedFor"), f.value(kImportedFor).toString()},
            {QStringLiteral("obdb"), h.obdb}};
}

bool PluginStore::take(const QString& sha, const QByteArray& bytes,
                       QString* words)
{
    if (!m_failLoadForTesting.isEmpty() && sha == m_failLoadForTesting) {
        m_failLoadForTesting.clear();
        *words = QStringLiteral("a load failed for the test");
        return false;
    }
    Held h;
    h.plugin = loadPackage(bytes, words);
    if (h.plugin == nullptr) {
        return false;
    }
    const Named named = namedOf(h.plugin);
    h.id = named.id;
    h.name = named.name;
    h.version = named.version;
    h.lineage = named.lineage;
    h.obdb = ObdbImport::isConversion(bytes);
    for (const auto& held : m_held) {
        if (held.second.id == h.id) {
            *words = QStringLiteral("Another copy of %1 is loaded").arg(h.id);
            (void)ecuio_plugin_free(h.plugin);
            return false;
        }
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    (void)ecuio_plugin_info(h.plugin, &info);
    for (uint32_t i = 0; i < info.variant_count; ++i) {
        char variant[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
        char profile[64] = {};
        (void)ecuio_plugin_variant(h.plugin, i, variant, sizeof variant);
        const ecuio_status_t rc =
            ecuio_plugin_register(h.plugin, i, profile, sizeof profile);
        if (rc != ECUIO_OK) {
            *words = coreWords(rc);
            (void)ecuio_plugin_free(h.plugin); // its profiles go with it
            return false;
        }
        h.variants << QString::fromUtf8(variant);
        h.profiles << QString::fromUtf8(profile);
        // What the variant adds to, and how many signals are its own: the
        // set holds the base's first
        const QByteArray base =
            ecuio_signal_file_extends(ecuio_plugin_variant_file(h.plugin, i));
        const ecuio_signal_set_t* set = ecuio_plugin_variant_set(h.plugin, i);
        const int count = set != nullptr ? static_cast<int>(set->count) : 0;
        int own = count;
        const ecuio_signal_set_t* baseSet = nullptr;
        if (!base.isEmpty() &&
            ecuio_profile_lookup(base.constData(), &baseSet) == ECUIO_OK &&
            baseSet != nullptr) {
            own = std::max(0, count - static_cast<int>(baseSet->count));
        }
        h.extends << QString::fromUtf8(base);
        h.counts << count;
        h.owns << own;
    }
    m_held.emplace(sha, h);
    return true;
}

std::vector<const PluginStore::HeldMap::value_type*> PluginStore::byName() const
{
    std::vector<const HeldMap::value_type*> held;
    held.reserve(m_held.size());
    for (const auto& h : m_held) {
        held.push_back(&h);
    }
    // Two plugins may share a name (a wrap numbers the id, not the name)
    std::sort(held.begin(), held.end(), [](const auto* a, const auto* b) {
        return a->second.name != b->second.name
                   ? a->second.name < b->second.name
                   : a->second.id < b->second.id;
    });
    return held;
}

PluginStore::HeldMap::const_iterator
PluginStore::heldWithId(const QString& id) const
{
    return std::find_if(m_held.begin(), m_held.end(),
                        [&id](const auto& h) { return h.second.id == id; });
}

PluginStore::HeldMap::const_iterator
PluginStore::heldWithProfile(const QString& profile, int* variant) const
{
    for (auto it = m_held.begin(); it != m_held.end(); ++it) {
        const auto at = it->second.profiles.indexOf(profile);
        if (at >= 0) {
            *variant = static_cast<int>(at);
            return it;
        }
    }
    return m_held.end();
}

QVariantList PluginStore::all()
{
    loadIndex();
    QVariantList refused;
    if (!m_loaded) {
        m_loaded = true;
        const QStringList names = QDir(root()).entryList(
            {QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name);
        for (const QString& file : names) {
            const QString sha = file.chopped(kSuffix.size());
            QFile f(root() + QLatin1Char('/') + file);
            QString words;
            if (!f.open(QIODevice::ReadOnly)) {
                words = QStringLiteral(
                    "Its copy can't be read: import the plugin again");
            } else {
                const QByteArray bytes = f.read(ECUIO_PLUGIN_PACKAGE_MAX + 1);
                if (shaOf(bytes) != sha) {
                    words = QStringLiteral(
                        "Its copy has changed: import the plugin again");
                } else if (!take(sha, bytes, &words)) {
                    words = QStringLiteral("It no longer loads: %1").arg(words);
                }
            }
            if (!words.isEmpty()) {
                m_refused[sha] = words;
            }
        }
    }
    QVariantList out;
    for (const auto* h : byName()) {
        out << entryOf(h->first, h->second);
    }
    for (const auto& r : m_refused) {
        refused << QVariantMap{{QStringLiteral("sha256"), r.first},
                               {QStringLiteral("ok"), false},
                               {QStringLiteral("words"), r.second}};
    }
    return out + refused;
}

QVariantMap PluginStore::import(const QByteArray& bytes)
{
    (void)all(); // the kept copies first: an id may replace one
    const QString sha = shaOf(bytes);
    if (m_held.find(sha) != m_held.end()) {
        return keptEntry(sha);
    }
    QString words;
    Named named;
    if (!nameOf(bytes, &named, &words)) {
        return refusal(words);
    }
    // A plugin of the user's own gives way only to a later version of
    // itself: the same lineage, a higher version (ADR-0064 Q4)
    QString updated;
    const auto own = heldWithId(named.id);
    if (own != m_held.end() && isMine(named.id)) {
        const Held& mine = own->second;
        if (named.lineage.isEmpty() || named.lineage != mine.lineage) {
            return refusal(
                QStringLiteral("This package has the id of your own plugin "
                               "%1 %2 (%3) and isn't a later version of it: "
                               "remove yours first")
                    .arg(mine.name, mine.version, mine.id));
        }
        const int order = versionOrder(named.version, mine.version);
        if (order < 0) {
            return refusal(
                QStringLiteral("This file holds %1 %2, older than yours at %3")
                    .arg(named.name, named.version, mine.version));
        }
        if (order == 0) {
            return refusal(
                QStringLiteral("Your %1 is at %2 and this file holds %3 with "
                               "other content: the two were changed apart, so "
                               "keep one of them")
                    .arg(mine.name, mine.version, named.version));
        }
        updated = mine.version;
    }
    QVariantMap out =
        install(bytes, sha, named.id, QStringLiteral("import it again"));
    if (out.value(QStringLiteral("ok")).toBool()) {
        out.insert(QStringLiteral("updated"), updated);
    }
    return out;
}

QVariantMap PluginStore::install(const QByteArray& bytes, const QString& sha,
                                 const QString& id, const QString& retry)
{
    QString words;
    const bool cameNew = heldWithId(id) == m_held.end();
    // The copy goes first, so a copy that can't be kept changes nothing
    const QString path = pathOf(sha);
    const bool kept = holds(path, bytes);
    if (!kept) {
        QSaveFile copy(path);
        if (!QDir().mkpath(root()) || !copy.open(QIODevice::WriteOnly) ||
            copy.write(bytes) != bytes.size() || !copy.commit()) {
            return refusal(
                QStringLiteral("Couldn't keep a copy of the plugin"));
        }
    }
    const auto dropCopy = [&]() {
        if (!kept) {
            (void)QFile::remove(path);
        }
    };
    // One version per id: the loaded one goes first (their profiles share
    // names), its copy only once the import has taken its place
    QString replaced;
    QStringList replacedProfiles;
    QString oldSha;
    QByteArray oldBytes;
    for (auto it = m_held.begin(); it != m_held.end(); ++it) {
        if (it->second.id != id) {
            continue;
        }
        QFile old(pathOf(it->first));
        if (old.open(QIODevice::ReadOnly)) {
            oldBytes = old.readAll();
        }
        if (ecuio_plugin_free(it->second.plugin) == ECUIO_ERR_IN_USE) {
            dropCopy();
            QVariantMap busy = refusal(
                QStringLiteral("A session decodes with %1 %2: disconnect, "
                               "then %3")
                    .arg(it->second.name, it->second.version, retry));
            busy.insert(QStringLiteral("inUse"), true);
            return busy;
        }
        replaced = it->second.version;
        replacedProfiles = it->second.profiles;
        oldSha = it->first;
        m_held.erase(it);
        break;
    }
    if (!take(sha, bytes, &words)) {
        dropCopy();
        // The version it would have replaced is loaded again from its copy
        QString again;
        if (!oldSha.isEmpty() && !take(oldSha, oldBytes, &again)) {
            m_refused[oldSha] = again;
        }
        return refusal(words);
    }
    if (!oldSha.isEmpty()) {
        (void)QFile::remove(pathOf(oldSha));
    }
    m_refused.erase(sha);
    settleFacts(id, cameNew, m_held.at(sha).variants);
    QVariantMap out = entryOf(sha, m_held.at(sha));
    out.insert(QStringLiteral("replaced"), replaced);
    out.insert(kReplacedProfiles, replacedProfiles);
    out.insert(QStringLiteral("kept"), kept);
    out.insert(QStringLiteral("updated"), QString());
    return out;
}

QVariantMap PluginStore::describe(const QByteArray& file)
{
    QString words;
    const QByteArray text = wrapped(file, QString(), QString(), &words);
    Named named;
    if (text.isEmpty() || !nameOf(text, &named, &words)) {
        return refusal(words);
    }
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("words"), QString()},
        {QStringLiteral("name"), named.name},
        {QStringLiteral("id"), named.id},
        {QStringLiteral("variant"),
         cutId(named.id, static_cast<qsizetype>(ECUIO_PLUGIN_VARIANT_ID_MAX))}};
}

QVariantMap PluginStore::noted(QVariantMap installed, const QString& id,
                               const Note& note)
{
    if (!installed.value(QStringLiteral("ok")).toBool()) {
        return installed;
    }
    const bool saved = changeFacts(id, [&note](QJsonObject& f) {
        f.insert(kMine, true);
        // The first vehicle it was imported for stays: the same file kept
        // again for another moves nothing
        if (!note.vehicle.isEmpty() && !f.contains(kImportedFor)) {
            f.insert(kImportedFor, note.vehicle);
        }
        if (note.fileSha.isEmpty() || note.variant.isEmpty()) {
            return;
        }
        // One file for each variant: the file it was made from before is
        // no longer in the plugin
        QJsonObject from = f.value(kWrappedFrom).toObject();
        for (auto it = from.begin(); it != from.end();) {
            if (it.value().toString() == note.variant) {
                it = from.erase(it);
            } else {
                ++it;
            }
        }
        from.insert(note.fileSha, note.variant);
        f.insert(kWrappedFrom, from);
    });
    // The entry again, with what the index now says of it
    const QString sha = installed.value(QStringLiteral("sha256")).toString();
    QVariantMap out = entryOf(sha, m_held.at(sha));
    for (const QString& key :
         {QStringLiteral("kept"), QStringLiteral("replaced"), kReplacedProfiles,
          QStringLiteral("updated")}) {
        out.insert(key, installed.value(key));
    }
    if (!saved) {
        out.insert(QStringLiteral("words"),
                   QStringLiteral("The plugin is kept, but the store's index "
                                  "couldn't be written yet: if no later "
                                  "change writes it before the app closes, "
                                  "after a restart it isn't marked as yours"));
    }
    return out;
}

QVariantMap PluginStore::wrap(const QByteArray& file, const Wrap& how)
{
    (void)all();
    QString words;
    const QString fileSha = shaOf(file);
    const QByteArray first = wrapped(file, QString(), how.variant, &words);
    Named named;
    if (first.isEmpty() || !nameOf(first, &named, &words)) {
        return refusal(words);
    }
    const QString retry = QStringLiteral("import it again");
    const Note note{how.vehicle, fileSha,
                    how.variant.isEmpty() ? kBase : how.variant};
    // Whether the index says this file made that variant of the plugin
    const auto madeFrom = [&](const Held& h) {
        const QJsonObject from = facts(h.id).value(kWrappedFrom).toObject();
        return from.value(fileSha).toString() == note.variant;
    };
    // The plugin the file made is the answer, with words when it has
    // changed since and so is no longer the file's wrap
    const auto already = [&](const QString& sha) {
        const Held h = m_held.at(sha);
        QString unused;
        const QByteArray text = h.id == named.id
                                    ? first
                                    : wrapped(file, h.id, how.variant, &unused);
        QVariantMap out = noted(keptEntry(sha), h.id, note);
        if (shaOf(text) != sha &&
            out.value(QStringLiteral("words")).toString().isEmpty()) {
            out.insert(QStringLiteral("words"),
                       QStringLiteral("This file made your %1 (%2), now at "
                                      "%3: nothing new was kept")
                           .arg(h.name, h.id, h.version));
        }
        return out;
    };
    // An OBDb set converted again for its vehicle goes to the earlier
    // conversion of that name, under the id that one has
    if (!how.vehicle.isEmpty() && ObdbImport::isConversion(file)) {
        const auto earlier =
            std::find_if(m_held.begin(), m_held.end(), [&](const auto& h) {
                const QJsonObject f = facts(h.second.id);
                return h.second.obdb && h.second.name == named.name &&
                       f.value(kMine).toBool() &&
                       f.value(kImportedFor).toString() == how.vehicle;
            });
        if (earlier != m_held.end()) {
            const QString sha = earlier->first;
            const QString id = earlier->second.id;
            if (madeFrom(earlier->second)) {
                return already(sha);
            }
            // Variants the user added stay: the set joins them as one
            if (earlier->second.variants.size() > 1) {
                QVariantMap out = putInto(sha, note.variant, file, how.vehicle);
                if (out.value(QStringLiteral("ok")).toBool()) {
                    out.insert(QStringLiteral("merged"), true);
                }
                return out;
            }
            const QByteArray text =
                id == named.id ? first : wrapped(file, id, how.variant, &words);
            if (text.isEmpty()) {
                return refusal(words);
            }
            const QString now = shaOf(text);
            if (now == sha) {
                return noted(keptEntry(sha), id, note);
            }
            return noted(install(text, now, id, retry), id, note);
        }
    }
    for (const auto* h : byName()) {
        if (madeFrom(h->second)) {
            return already(h->first);
        }
    }
    for (int n = 1; n <= kIdTries; ++n) {
        const QString id = n == 1 ? named.id : numbered(named.id, n);
        const QByteArray text =
            n == 1 ? first : wrapped(file, id, how.variant, &words);
        if (text.isEmpty()) {
            return refusal(words);
        }
        const QString sha = shaOf(text);
        if (m_held.find(sha) != m_held.end()) {
            // The same file again: its copy is here
            return noted(keptEntry(sha), id, note);
        }
        if (heldWithId(id) == m_held.end()) {
            return noted(install(text, sha, id, retry), id, note);
        }
    }
    return refusal(QStringLiteral("%1 plugins here have ids made from the "
                                  "name %2: give the file another name")
                       .arg(kIdTries)
                       .arg(named.name));
}

QVariantMap PluginStore::put(const QString& sha, const QString& variant,
                             const QByteArray& file)
{
    (void)all();
    const auto it = m_held.find(sha);
    if (it == m_held.end()) {
        return refusal(QStringLiteral("The store has no such plugin"));
    }
    if (!isMine(it->second.id)) {
        return refusal(QStringLiteral("%1 isn't a plugin of your own: only "
                                      "your own plugins are changed")
                           .arg(it->second.name));
    }
    return putInto(sha, variant, file, QString());
}

QVariantMap PluginStore::putInto(const QString& sha, const QString& variant,
                                 const QByteArray& file, const QString& vehicle)
{
    const QString id = m_held.at(sha).id;
    QString words;
    const QByteArray before = packed(sha, &words);
    if (before.isEmpty()) {
        return refusal(words);
    }
    // The id the variant takes: the one given, else the file's name as
    // core cuts it
    const QString made =
        variant.isEmpty()
            ? describe(file).value(QStringLiteral("variant")).toString()
            : variant;
    const QByteArray variantText = variant.toUtf8();
    CoreText out;
    const ecuio_status_t rc = ecuio_plugin_put_variant(
        before.constData(), static_cast<size_t>(before.size()),
        variant.isEmpty() ? nullptr : variantText.constData(), file.constData(),
        static_cast<size_t>(file.size()), &out.text, &out.length);
    if (rc != ECUIO_OK) {
        return refusal(coreWords(rc));
    }
    // On success the detail is core's note for the user: read before
    // another core call
    const QString notes = QString::fromUtf8(ecuio_last_error_detail());
    const QByteArray text = out.bytes();
    QVariantMap result =
        noted(install(text, shaOf(text), id, QStringLiteral("try again")), id,
              {vehicle, shaOf(file), made});
    if (result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("notes"), notes);
    }
    return result;
}

QVariantMap PluginStore::drop(const QString& sha, const QString& variant)
{
    (void)all();
    const auto it = m_held.find(sha);
    if (it == m_held.end()) {
        return refusal(QStringLiteral("The store has no such plugin"));
    }
    const QString id = it->second.id;
    if (!isMine(id)) {
        return refusal(QStringLiteral("%1 isn't a plugin of your own: only "
                                      "your own plugins are changed")
                           .arg(it->second.name));
    }
    QString words;
    const QByteArray before = packed(sha, &words);
    if (before.isEmpty()) {
        return refusal(words);
    }
    const QByteArray variantText = variant.toUtf8();
    CoreText out;
    const ecuio_status_t rc = ecuio_plugin_drop_variant(
        before.constData(), static_cast<size_t>(before.size()),
        variantText.constData(), &out.text, &out.length);
    if (rc != ECUIO_OK) {
        return refusal(coreWords(rc));
    }
    const QByteArray text = out.bytes();
    return noted(install(text, shaOf(text), id, QStringLiteral("try again")),
                 id, {});
}

QVariantMap PluginStore::forgetVehicle(const QString& vehicle)
{
    return forgetImports(&vehicle);
}

QVariantMap PluginStore::forgetVehicles()
{
    return forgetImports(nullptr);
}

QVariantMap PluginStore::forgetImports(const QString* vehicle)
{
    int count = 0;
    const bool saved = changeIndex([&](QJsonObject& index) {
        QJsonObject plugins = index.value(kPlugins).toObject();
        for (auto it = plugins.begin(); it != plugins.end(); ++it) {
            QJsonObject f = it.value().toObject();
            if (!f.contains(kImportedFor) ||
                (vehicle != nullptr &&
                 f.value(kImportedFor).toString() != *vehicle)) {
                continue;
            }
            f.remove(kImportedFor);
            it.value() = f;
            ++count;
        }
        if (count == 0) {
            return false;
        }
        index.insert(kPlugins, plugins);
        return true;
    });
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"),
             saved || count == 0
                 ? QString()
                 : QStringLiteral("They are released, but the store's index "
                                  "couldn't be written yet: if no later "
                                  "change writes it before the app closes, "
                                  "after a restart each may be imported for "
                                  "its vehicle again")},
            {QStringLiteral("count"), count}};
}

QVariantMap PluginStore::markMine(const QString& sha)
{
    (void)all();
    const auto it = m_held.find(sha);
    if (it == m_held.end()) {
        return refusal(QStringLiteral("The store has no such plugin"));
    }
    const bool saved = changeFacts(
        it->second.id, [](QJsonObject& f) { f.insert(kMine, true); });
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"),
             saved ? QString()
                   : QStringLiteral("It is marked as yours, but the store's "
                                    "index couldn't be written yet: if no "
                                    "later change writes it before the app "
                                    "closes, after a restart it isn't marked "
                                    "as yours")}};
}

QVariantMap PluginStore::remove(const QString& sha)
{
    loadIndex();
    QString id;
    const auto it = m_held.find(sha);
    if (it != m_held.end()) {
        if (ecuio_plugin_free(it->second.plugin) == ECUIO_ERR_IN_USE) {
            return refusal(QStringLiteral(
                "A session decodes with it: disconnect, then remove it"));
        }
        id = it->second.id;
        m_held.erase(it);
    } else {
        // A copy that didn't load may still say whose facts go with it
        QFile copy(pathOf(sha));
        if (copy.open(QIODevice::ReadOnly)) {
            id = QJsonDocument::fromJson(copy.read(ECUIO_PLUGIN_PACKAGE_MAX))
                     .object()
                     .value(QStringLiteral("id"))
                     .toString();
        }
    }
    m_refused.erase(sha);
    (void)QFile::remove(pathOf(sha));
    if (!id.isEmpty() && heldWithId(id) == m_held.end() &&
        !facts(id).isEmpty()) {
        (void)changeFacts(id, [](QJsonObject& f) { f = QJsonObject(); });
    }
    QDir dir(root());
    if (dir.exists() && dir.isEmpty()) {
        (void)dir.rmdir(dir.absolutePath());
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
}

QByteArray PluginStore::packed(const QString& sha, QString* words) const
{
    if (m_held.find(sha) == m_held.end()) {
        *words = QStringLiteral("The store has no such plugin");
        return {};
    }
    QFile f(pathOf(sha));
    if (!f.open(QIODevice::ReadOnly)) {
        *words =
            QStringLiteral("Its copy can't be read: import the plugin again");
        return {};
    }
    QByteArray bytes = f.read(ECUIO_PLUGIN_PACKAGE_MAX + 1);
    if (shaOf(bytes) != sha) {
        *words =
            QStringLiteral("Its copy has changed: import the plugin again");
        return {};
    }
    return bytes;
}

QByteArray PluginStore::variantBytes(const QString& sha, const QString& variant,
                                     QString* words)
{
    (void)all();
    const auto it = m_held.find(sha);
    if (it == m_held.end()) {
        *words = QStringLiteral("The store has no such plugin");
        return {};
    }
    const qsizetype i = it->second.variants.indexOf(variant);
    if (i < 0) {
        *words = QStringLiteral("%1 has no variant '%2'")
                     .arg(it->second.name, variant);
        return {};
    }
    // Borrowed until the plugin is freed: copied before anything frees it
    size_t length = 0;
    const char* text = ecuio_signal_file_text(
        ecuio_plugin_variant_file(it->second.plugin, static_cast<uint32_t>(i)),
        &length);
    return {text, static_cast<qsizetype>(length)};
}

QVariantMap PluginStore::variantText(const QString& sha, const QString& variant)
{
    QString words;
    const QByteArray bytes = variantBytes(sha, variant, &words);
    if (bytes.isEmpty()) {
        return refusal(words);
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("text"), QString::fromUtf8(bytes)}};
}

QVariantMap PluginStore::exportNote(const QString& sha)
{
    (void)all();
    const auto it = m_held.find(sha);
    if (it == m_held.end()) {
        return refusal(QStringLiteral("The store has no such plugin"));
    }
    const Held& h = it->second;
    QVariantList shared;
    QStringList lines;
    for (qsizetype i = 0; i < h.variants.size(); ++i) {
        ecuio_signal_source_t source{};
        source.struct_size = sizeof source;
        if (ecuio_signal_file_source(
                ecuio_plugin_variant_file(h.plugin, static_cast<uint32_t>(i)),
                &source) != ECUIO_OK ||
            (source.flags & ECUIO_SOURCE_SHARE_ALIKE) == 0) {
            continue;
        }
        const QString license = QString::fromUtf8(source.license);
        // havne-cli plugins check's words (cli/plugin_lines.h)
        const QString line =
            QStringLiteral("Variant '%1' holds data under %2, a share-alike "
                           "license: whoever passes the plugin on keeps that "
                           "license and the variant's source with it")
                .arg(h.variants.at(i), license);
        shared << QVariantMap{{QStringLiteral("variant"), h.variants.at(i)},
                              {QStringLiteral("license"), license},
                              {QStringLiteral("words"), line}};
        lines << line;
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("note"), lines.join(QLatin1Char('\n'))},
            {QStringLiteral("shareAlike"), shared}};
}

QVariantMap PluginStore::copyVariant(const QString& sha, const QString& variant,
                                     const QString& into,
                                     const QString& newVariant)
{
    QString words;
    const QByteArray file = variantBytes(sha, variant, &words);
    if (file.isEmpty()) {
        return refusal(words);
    }
    const QString from = m_held.at(sha).name;
    // A copy never takes a variant's place: the user names it. A plugin
    // not held or not the user's own is put's to refuse
    if (into == sha) {
        return refusal(QStringLiteral("Copy to another plugin: this variant "
                                      "is already in %1")
                           .arg(from));
    }
    const auto target = m_held.find(into);
    if (target != m_held.end() && isMine(target->second.id)) {
        const QString made =
            newVariant.isEmpty()
                ? describe(file).value(QStringLiteral("variant")).toString()
                : newVariant;
        if (target->second.variants.contains(made)) {
            return refusal(QStringLiteral("%1 already has a variant '%2': "
                                          "give the copy another name")
                               .arg(target->second.name, made));
        }
    }
    QVariantMap result = into.isEmpty() ? wrap(file, {QString(), newVariant})
                                        : put(into, newVariant, file);
    if (!result.value(QStringLiteral("ok")).toBool()) {
        return result;
    }
    const QString follows =
        QStringLiteral("The copy won't follow %1's updates to %2")
            .arg(from, variant);
    const QString notes = result.value(QStringLiteral("notes")).toString();
    result.insert(QStringLiteral("notes"),
                  notes.isEmpty() ? follows
                                  : notes + QStringLiteral("; ") + follows);
    return result;
}

QVariantList PluginStore::matches(const QString& vin, int latestModelYear) const
{
    QString text = vin.toUpper();
    text.remove(QLatin1Char(' '));
    const QByteArray latin = text.toLatin1();
    ecuio_vin_info_t info{};
    if (text.size() != ECUIO_VIN_LEN ||
        ecuio_vin_decode(latin.constData(),
                         static_cast<uint32_t>(std::max(latestModelYear, 0)),
                         &info) != ECUIO_OK) {
        return {};
    }
    QVariantList out;
    for (const auto* h : byName()) {
        uint32_t indices[ECUIO_PLUGIN_MAX_VARIANTS] = {};
        uint32_t count = 0;
        if (ecuio_plugin_match(h->second.plugin, &info, indices,
                               ECUIO_PLUGIN_MAX_VARIANTS, &count) != ECUIO_OK) {
            continue;
        }
        for (uint32_t i = 0; i < std::min(count, ECUIO_PLUGIN_MAX_VARIANTS);
             ++i) {
            const int at = static_cast<int>(indices[i]);
            if (at >= h->second.variants.size()) {
                continue;
            }
            out << QVariantMap{
                {QStringLiteral("sha256"), h->first},
                {QStringLiteral("id"), h->second.id},
                {QStringLiteral("name"), h->second.name},
                {QStringLiteral("variant"), h->second.variants.at(at)},
                {QStringLiteral("profile"), h->second.profiles.at(at)}};
        }
    }
    return out;
}

QVariantList PluginStore::offered(const QString& vehicle, const QString& vin,
                                  int latestModelYear, const QStringList& picks)
{
    (void)all();
    QSet<QString> proposed;
    for (const QVariant& m : matches(vin, latestModelYear)) {
        proposed << m.toMap().value(QStringLiteral("profile")).toString();
    }
    QVariantList first;
    QVariantList rest;
    for (const auto* h : byName()) {
        const QJsonObject f = facts(h->second.id);
        const bool imported =
            !vehicle.isEmpty() && f.value(kImportedFor).toString() == vehicle;
        for (qsizetype i = 0; i < h->second.profiles.size(); ++i) {
            const QString profile = h->second.profiles.at(i);
            const bool forThisCar = imported || proposed.contains(profile);
            (forThisCar ? first : rest) << QVariantMap{
                {QStringLiteral("sha256"), h->first},
                {QStringLiteral("id"), h->second.id},
                {QStringLiteral("name"), h->second.name},
                {QStringLiteral("version"), h->second.version},
                {QStringLiteral("variant"), h->second.variants.at(i)},
                {QStringLiteral("profile"), profile},
                {QStringLiteral("extends"), h->second.extends.at(i)},
                {QStringLiteral("mine"), f.value(kMine).toBool()},
                {QStringLiteral("forThisCar"), forThisCar},
                {QStringLiteral("picked"), picks.contains(profile)}};
        }
    }
    return first + rest;
}

QVariantList PluginStore::pickRow(const QStringList& picks, bool adapter)
{
    (void)all();
    QVariantList out;
    bool stood = false;
    for (const QString& profile : picks) {
        int at = 0;
        const auto it = heldWithProfile(profile, &at);
        const bool missing = it == m_held.end();
        const QString extends = missing ? QString() : it->second.extends.at(at);
        // The first pick that reads obd2's signals stands in for Default
        const bool standIn =
            adapter && !stood && extends == QLatin1String("obd2");
        stood = stood || standIn;
        out << QVariantMap{
            {QStringLiteral("profile"), profile},
            {QStringLiteral("sha256"), missing ? QString() : it->first},
            {QStringLiteral("id"), missing ? QString() : it->second.id},
            {QStringLiteral("name"), missing ? QString() : it->second.name},
            {QStringLiteral("version"),
             missing ? QString() : it->second.version},
            {QStringLiteral("variant"),
             missing ? QString() : it->second.variants.at(at)},
            {QStringLiteral("extends"), extends},
            {QStringLiteral("missing"), missing},
            {QStringLiteral("standIn"), standIn}};
    }
    return out;
}

QVariantMap PluginStore::pick(const QStringList& picks, const QString& profile)
{
    (void)all();
    const auto answer = [](bool ok, const QString& words,
                           const QStringList& list) {
        return QVariantMap{{QStringLiteral("ok"), ok},
                           {QStringLiteral("words"), words},
                           {QStringLiteral("picks"), list}};
    };
    if (picks.contains(profile)) {
        return answer(true, QString(), picks);
    }
    int at = 0;
    if (heldWithProfile(profile, &at) == m_held.end()) {
        return answer(false, QStringLiteral("No plugin here has that variant"),
                      picks);
    }
    if (picks.size() >= MaxPicks) {
        return answer(false,
                      QStringLiteral("A vehicle picks at most %1 variants: "
                                     "unpick one first")
                          .arg(MaxPicks),
                      picks);
    }
    return answer(true, QString(), picks + QStringList{profile});
}

QStringList PluginStore::unpick(const QStringList& picks,
                                const QString& profile)
{
    QStringList out = picks;
    out.removeAll(profile);
    return out;
}

QVariantList PluginStore::wrappedFrom(const QString& fileSha)
{
    (void)all();
    std::vector<const Held*> held;
    held.reserve(m_held.size());
    for (const auto& h : m_held) {
        held.push_back(&h.second);
    }
    std::sort(held.begin(), held.end(),
              [](const Held* a, const Held* b) { return a->id < b->id; });
    QVariantList made;
    QVariantList given;
    for (const Held* h : held) {
        const QString variant = facts(h->id)
                                    .value(kWrappedFrom)
                                    .toObject()
                                    .value(fileSha)
                                    .toString();
        const auto at = h->variants.indexOf(variant);
        if (variant.isEmpty() || at < 0) {
            continue;
        }
        // A wrap's lineage is the start of its file's hash
        const bool wrapped =
            !h->lineage.isEmpty() && fileSha.startsWith(h->lineage);
        (wrapped ? made : given)
            << QVariantMap{{QStringLiteral("id"), h->id},
                           {QStringLiteral("variant"), variant},
                           {QStringLiteral("profile"), h->profiles.at(at)},
                           {QStringLiteral("wrapped"), wrapped}};
    }
    return made + given;
}

bool PluginStore::migrated()
{
    loadIndex();
    return m_index.value(kMigrated).toInt() == 1;
}

bool PluginStore::setMigrated()
{
    return changeIndex([](QJsonObject& index) {
        index.insert(kMigrated, 1);
        return true;
    });
}

const ecuio_signal_file_t*
PluginStore::fileFor(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    for (const auto& held : m_held) {
        const auto count = static_cast<uint32_t>(held.second.variants.size());
        for (uint32_t i = 0; i < count; ++i) {
            if (ecuio_plugin_variant_set(held.second.plugin, i) == set) {
                return ecuio_plugin_variant_file(held.second.plugin, i);
            }
        }
    }
    return nullptr;
}

void PluginStore::close()
{
    for (auto& held : m_held) {
        (void)ecuio_plugin_free(held.second.plugin);
    }
    m_held.clear();
}
