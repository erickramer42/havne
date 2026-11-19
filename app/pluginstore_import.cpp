// File/layer: app/pluginstore_import.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge)
//
// A plugin coming into the store or changed in it: a package imported,
// a signal file wrapped, a variant put, dropped or copied. Each ends in
// install, which keeps the copy first and then takes the loaded
// version's place (pluginstore.h describes the store).

#include "pluginstore.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <algorithm>

#include "ecuio/core.h"
#include "obdbimport.h"
#include "pluginstore_parts.h"

using namespace plugin_store;

namespace {

const QString kBase = QStringLiteral("base");
// The ids a wrap tries for one name: its own, then -2 to -99
constexpr int kIdTries = 99;

// Whether the copy at path already holds bytes: a copy changed since is
// written again
bool holds(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(bytes.size() + 1) == bytes;
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

} // namespace

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
