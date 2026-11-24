// File/layer: app/pluginstore.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge)

#include "pluginstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "obdbimport.h"
#include "pluginstore_parts.h"
#include "variantshape.h"

using namespace plugin_store;

namespace {

const QString kSuffix = QStringLiteral(".havne-plugin.json");
const QString kPlugins = QStringLiteral("plugins");
const QString kMigrated = QStringLiteral("migrated");
const QString kFormat = QStringLiteral("format");
constexpr int kIndexFormat = 1;
constexpr qint64 kIndexMaxBytes = 4 * 1024 * 1024;
// The names an index moved aside tries: store.json.bad-1 to -99
constexpr int kAsideTries = 99;

} // namespace

namespace plugin_store {

QString shaOf(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
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

} // namespace plugin_store

namespace {

// What a consent asks of a plugin: conversation (its code talks to
// modules), publisher ("" for none) and fingerprint (64 hex digits)
QVariantMap consentFactsOf(const ecuio_plugin_t* plugin)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ecuio_plugin_publisher_t publisher{};
    publisher.struct_size = sizeof publisher;
    ecuio_plugin_fingerprint_t print{};
    print.struct_size = sizeof print;
    const bool talks = ecuio_plugin_info(plugin, &info) == ECUIO_OK &&
                       (info.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) != 0;
    const bool named = ecuio_plugin_publisher(plugin, &publisher) == ECUIO_OK;
    const bool printed = ecuio_plugin_fingerprint(plugin, &print) == ECUIO_OK;
    const QByteArray bytes(reinterpret_cast<const char*>(print.fingerprint),
                           sizeof print.fingerprint);
    return {{QStringLiteral("conversation"), talks},
            {QStringLiteral("publisher"),
             named ? QString::fromUtf8(publisher.name) : QString()},
            {QStringLiteral("fingerprint"),
             printed ? QString::fromLatin1(bytes.toHex()) : QString()}};
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

} // namespace

namespace plugin_store {

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

} // namespace plugin_store

namespace {

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
    QVariantMap e{{QStringLiteral("sha256"), sha},
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
            {QStringLiteral("replacedNames"), h.replacedNames},
            {QStringLiteral("pages"), pagesOf(h.plugin)},
            {QStringLiteral("mine"), f.value(kMine).toBool()},
            {QStringLiteral("importedFor"), f.value(kImportedFor).toString()},
            {QStringLiteral("obdb"), h.obdb}};
    e.insert(consentFactsOf(h.plugin));
    return e;
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
        // What the variant adds to, and what it adds and replaces there
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(h.plugin, i);
        const QByteArray base = ecuio_signal_file_extends(file);
        const ecuio_signal_set_t* baseSet = nullptr;
        if (base.isEmpty() ||
            ecuio_profile_lookup(base.constData(), &baseSet) != ECUIO_OK) {
            baseSet = nullptr;
        }
        const variant_shape::Shape shape = variant_shape::of(file, baseSet);
        h.extends << QString::fromUtf8(base);
        h.counts << shape.count;
        h.owns << shape.added;
        h.replacedNames << shape.replaced;
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

const ecuio_plugin_t* PluginStore::pluginOfProfile(const QString& profile,
                                                   QString* sha)
{
    (void)all();
    int at = 0;
    const auto it = heldWithProfile(profile, &at);
    if (it == m_held.end()) {
        return nullptr;
    }
    *sha = it->first;
    return it->second.plugin;
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
