// File/layer: app/signalstore.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.SignalStore* and .SignalFile* (test_bridge)

#include "signalstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

#include "ecuio/core.h"
#include "ecuio/profile.h"

namespace {

QString shaOf(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Whether the copy at path already holds bytes: named by their hash, it
// should, but one changed since (an editor, a disk fault) holds others,
// and an import of the right bytes is what repairs it
bool holds(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(bytes.size() + 1) == bytes;
}

// The loader's or the registry's words: the detail, else the status
QString coreWords(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        return QString::fromUtf8(detail);
    }
    return QString::fromUtf8(ecuio_status_string(rc));
}

} // namespace

QString SignalStore::root()
{
    const QString dir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    const QString base =
        dir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : dir;
    return base + QStringLiteral("/signals");
}

QString SignalStore::pathOf(const QString& sha)
{
    return root() + QLatin1Char('/') + sha + QStringLiteral(".json");
}

SignalStore::~SignalStore()
{
    close();
}

QVariantMap SignalStore::entryOf(const QString& sha, const Held& h)
{
    return {{QStringLiteral("sha256"), sha},
            {QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("name"), h.name},
            {QStringLiteral("profile"), h.profile},
            {QStringLiteral("extends"), h.extends},
            {QStringLiteral("count"), h.count},
            {QStringLiteral("own"), h.own}};
}

bool SignalStore::take(const QString& sha, const QByteArray& bytes,
                       QString* words)
{
    ecuio_signal_file_t* file = nullptr;
    const ecuio_status_t rc = ecuio_signal_file_load(
        bytes.constData(), static_cast<size_t>(bytes.size()), &file);
    if (rc != ECUIO_OK) {
        *words = coreWords(rc);
        return false;
    }
    Held h;
    h.file = file;
    h.name = QString::fromUtf8(ecuio_signal_file_name(file));
    h.profile = QString::fromUtf8(ecuio_signal_file_profile(file));
    h.extends = QString::fromUtf8(ecuio_signal_file_extends(file));
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    h.count = set != nullptr ? static_cast<int>(set->count) : 0;
    // The file's own signals: the set holds the base's first
    h.own = h.count;
    const QByteArray baseName = h.extends.toUtf8();
    const ecuio_signal_set_t* base = nullptr;
    if (!baseName.isEmpty() &&
        ecuio_profile_lookup(baseName.constData(), &base) == ECUIO_OK &&
        base != nullptr) {
        h.own = std::max(0, h.count - static_cast<int>(base->count));
    }
    m_held.emplace(sha, h);
    return true;
}

QVariantMap SignalStore::import(const QByteArray& bytes)
{
    const QString sha = shaOf(bytes);
    QString words;
    if (m_held.find(sha) == m_held.end() && !take(sha, bytes, &words)) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), words}};
    }
    QVariantMap out = entryOf(sha, m_held.at(sha));
    const QString path = pathOf(sha);
    if (holds(path, bytes)) {
        out.insert(QStringLiteral("kept"), true);
        return out;
    }
    QSaveFile copy(path);
    if (!QDir().mkpath(root()) || !copy.open(QIODevice::WriteOnly) ||
        copy.write(bytes) != bytes.size() || !copy.commit()) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"),
                 QStringLiteral("Couldn't keep a copy of the file")}};
    }
    out.insert(QStringLiteral("kept"), false);
    return out;
}

const SignalStore::Held* SignalStore::load(const QString& sha, QString* words)
{
    const auto held = m_held.find(sha);
    if (held != m_held.end()) {
        return &held->second;
    }
    QFile f(pathOf(sha));
    if (!f.open(QIODevice::ReadOnly)) {
        *words = QStringLiteral("Its copy is missing: import the file again");
        return nullptr;
    }
    const QByteArray bytes = f.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1);
    if (shaOf(bytes) != sha) {
        *words = QStringLiteral("Its copy has changed: import the file again");
        return nullptr;
    }
    QString why;
    if (!take(sha, bytes, &why)) {
        *words = QStringLiteral("It no longer loads: %1").arg(why);
        return nullptr;
    }
    return &m_held.at(sha);
}

QString SignalStore::text(const QString& sha)
{
    QFile f(pathOf(sha));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(f.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1));
}

QByteArray SignalStore::bytes(const QString& sha, QString* words)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(sha).hasMatch()) {
        *words = QStringLiteral("No such signal file");
        return {};
    }
    QFile f(pathOf(sha));
    if (!f.open(QIODevice::ReadOnly)) {
        *words = QStringLiteral("Its copy is missing: import the file again");
        return {};
    }
    QByteArray b = f.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1);
    if (shaOf(b) != sha) {
        *words = QStringLiteral(
            "Its copy has changed since it was kept: import the file again");
        return {};
    }
    return b;
}

QVariantList SignalStore::use(const QStringList& shas)
{
    for (auto it = m_registered.begin(); it != m_registered.end();) {
        if (!shas.contains(*it)) {
            const QByteArray name = m_held.at(*it).profile.toUtf8();
            (void)ecuio_profile_unregister(name.constData());
            it = m_registered.erase(it);
        } else {
            ++it;
        }
    }
    QVariantList out;
    QSet<QString> seen;
    for (const QString& sha : shas) {
        if (seen.contains(sha)) {
            continue;
        }
        seen.insert(sha);
        const auto refused = [&out, &sha](const QString& words) {
            out << QVariantMap{{QStringLiteral("sha256"), sha},
                               {QStringLiteral("ok"), false},
                               {QStringLiteral("words"), words}};
        };
        QString words;
        const Held* h = load(sha, &words);
        if (h == nullptr) {
            refused(words);
            continue;
        }
        // One profile, one file: another registered file holding the
        // name would be replaced under the sessions that chose it
        QString holder;
        for (const QString& other : m_registered) {
            if (other != sha && m_held.at(other).profile == h->profile) {
                holder = m_held.at(other).name;
            }
        }
        QVariantMap entry = entryOf(sha, *h);
        if (!holder.isEmpty()) {
            entry.insert(QStringLiteral("ok"), false);
            entry.insert(
                QStringLiteral("words"),
                QStringLiteral("%1 is made by another file already (%2)")
                    .arg(h->profile, holder));
        } else if (!m_registered.contains(sha)) {
            const QByteArray name = h->profile.toUtf8();
            const ecuio_status_t rc = ecuio_profile_register(
                name.constData(), ecuio_signal_file_set(h->file), nullptr);
            if (rc == ECUIO_OK) {
                m_registered.insert(sha);
            } else {
                entry.insert(QStringLiteral("ok"), false);
                entry.insert(QStringLiteral("words"), coreWords(rc));
            }
        }
        out << entry;
    }
    return out;
}

const ecuio_signal_file_t*
SignalStore::fileFor(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    for (const auto& held : m_held) {
        if (ecuio_signal_file_set(held.second.file) == set) {
            return held.second.file;
        }
    }
    return nullptr;
}

QString SignalStore::profileOf(const QString& sha) const
{
    if (!m_registered.contains(sha)) {
        return {};
    }
    const auto held = m_held.find(sha);
    return held != m_held.end() ? held->second.profile : QString();
}

int SignalStore::deleteUnused(const QStringList& keep)
{
    QDir dir(root());
    int deleted = 0;
    const QStringList names =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& file : names) {
        const QString sha = file.chopped(5);
        if (keep.contains(sha)) {
            continue;
        }
        // Its profile goes first, so no session is created on a file whose
        // copy is gone; the loaded file itself stays until close()
        if (m_registered.contains(sha)) {
            const QByteArray name = m_held.at(sha).profile.toUtf8();
            (void)ecuio_profile_unregister(name.constData());
            m_registered.remove(sha);
        }
        if (dir.remove(file)) {
            ++deleted;
        }
    }
    if (dir.exists() && dir.isEmpty()) {
        (void)dir.rmdir(dir.absolutePath());
    }
    return deleted;
}

void SignalStore::close()
{
    for (const QString& sha : m_registered) {
        const QByteArray name = m_held.at(sha).profile.toUtf8();
        (void)ecuio_profile_unregister(name.constData());
    }
    m_registered.clear();
    for (auto& held : m_held) {
        ecuio_signal_file_free(held.second.file);
    }
    m_held.clear();
}
