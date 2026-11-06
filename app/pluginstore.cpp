// File/layer: app/pluginstore.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin* (test_bridge)

#include "pluginstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/vin.h"

namespace {

const QString kSuffix = QStringLiteral(".havne-plugin.json");

QString shaOf(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Whether the copy at path already holds bytes, as the signal store
// judges its own: a copy changed since is written again
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

PluginStore::~PluginStore()
{
    close();
}

QVariantMap PluginStore::entryOf(const QString& sha, const Held& h)
{
    return {{QStringLiteral("sha256"), sha},
            {QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("id"), h.id},
            {QStringLiteral("name"), h.name},
            {QStringLiteral("version"), h.version},
            {QStringLiteral("variants"), h.variants},
            {QStringLiteral("profiles"), h.profiles}};
}

bool PluginStore::take(const QString& sha, const QByteArray& bytes,
                       QString* words)
{
    Held h;
    h.plugin = loadPackage(bytes, words);
    if (h.plugin == nullptr) {
        return false;
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    (void)ecuio_plugin_info(h.plugin, &info);
    h.id = QString::fromUtf8(info.id);
    h.name = QString::fromUtf8(info.name);
    h.version = QString::fromUtf8(info.version);
    for (const auto& held : m_held) {
        if (held.second.id == h.id) {
            *words = QStringLiteral("Another copy of %1 is loaded").arg(h.id);
            (void)ecuio_plugin_free(h.plugin);
            return false;
        }
    }
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
    }
    m_held.emplace(sha, h);
    return true;
}

QVariantList PluginStore::all()
{
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
    std::vector<const std::pair<const QString, Held>*> held;
    held.reserve(m_held.size());
    for (const auto& h : m_held) {
        held.push_back(&h);
    }
    std::sort(held.begin(), held.end(), [](const auto* a, const auto* b) {
        return a->second.name < b->second.name;
    });
    QVariantList out;
    for (const auto* h : held) {
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
    const auto same = m_held.find(sha);
    if (same != m_held.end()) {
        QVariantMap out = entryOf(sha, same->second);
        out.insert(QStringLiteral("kept"), true);
        out.insert(QStringLiteral("replaced"), QString());
        return out;
    }
    QString words;
    ecuio_plugin_t* probe = loadPackage(bytes, &words);
    if (probe == nullptr) {
        return refusal(words);
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    (void)ecuio_plugin_info(probe, &info);
    const QString id = QString::fromUtf8(info.id);
    (void)ecuio_plugin_free(probe); // read again by take(), once room is made
    // One version per id: the loaded one goes first, or the import does
    QString replaced;
    for (auto it = m_held.begin(); it != m_held.end(); ++it) {
        if (it->second.id != id) {
            continue;
        }
        if (ecuio_plugin_free(it->second.plugin) == ECUIO_ERR_IN_USE) {
            return refusal(
                QStringLiteral("A session decodes with %1 %2: disconnect, then "
                               "import it again")
                    .arg(it->second.name, it->second.version));
        }
        replaced = it->second.version;
        (void)QFile::remove(pathOf(it->first));
        m_held.erase(it);
        break;
    }
    if (!take(sha, bytes, &words)) {
        return refusal(words);
    }
    m_refused.erase(sha);
    QVariantMap out = entryOf(sha, m_held.at(sha));
    out.insert(QStringLiteral("replaced"), replaced);
    const QString path = pathOf(sha);
    if (holds(path, bytes)) {
        out.insert(QStringLiteral("kept"), true);
        return out;
    }
    QSaveFile copy(path);
    if (!QDir().mkpath(root()) || !copy.open(QIODevice::WriteOnly) ||
        copy.write(bytes) != bytes.size() || !copy.commit()) {
        (void)ecuio_plugin_free(m_held.at(sha).plugin);
        m_held.erase(sha);
        return refusal(QStringLiteral("Couldn't keep a copy of the plugin"));
    }
    out.insert(QStringLiteral("kept"), false);
    return out;
}

QVariantMap PluginStore::remove(const QString& sha)
{
    const auto it = m_held.find(sha);
    if (it != m_held.end()) {
        if (ecuio_plugin_free(it->second.plugin) == ECUIO_ERR_IN_USE) {
            return refusal(QStringLiteral(
                "A session decodes with it: disconnect, then remove it"));
        }
        m_held.erase(it);
    }
    m_refused.erase(sha);
    (void)QFile::remove(pathOf(sha));
    QDir dir(root());
    if (dir.exists() && dir.isEmpty()) {
        (void)dir.rmdir(dir.absolutePath());
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
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
    std::vector<const std::pair<const QString, Held>*> held;
    held.reserve(m_held.size());
    for (const auto& h : m_held) {
        held.push_back(&h);
    }
    std::sort(held.begin(), held.end(), [](const auto* a, const auto* b) {
        return a->second.name < b->second.name;
    });
    QVariantList out;
    for (const auto* h : held) {
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
