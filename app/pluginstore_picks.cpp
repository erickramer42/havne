// File/layer: app/pluginstore_picks.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge)
//
// What a vehicle is offered and picks: the variants a VIN proposes, the
// offer for a vehicle, the profile row, a pick and an unpick, the picks'
// files for a recording, and the plugins a signal file made.

#include "pluginstore.h"

#include <QCryptographicHash>
#include <QSet>

#include <algorithm>
#include <vector>

#include "ecuio/vin.h"
#include "pluginstore_parts.h"

using namespace plugin_store;

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
        if (profile == QLatin1String(Obd2Pick)) {
            out << QVariantMap{{QStringLiteral("profile"), profile},
                               {QStringLiteral("name"),
                                QStringLiteral("OBD-II standard signals")},
                               {QStringLiteral("builtIn"), true},
                               {QStringLiteral("missing"), false},
                               {QStringLiteral("standIn"), false}};
            continue;
        }
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
            {QStringLiteral("standIn"), standIn},
            {QStringLiteral("builtIn"), false}};
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
    if (profile == QLatin1String(Obd2Pick)) {
        return answer(true, QString(), picks + QStringList{profile});
    }
    int at = 0;
    if (heldWithProfile(profile, &at) == m_held.end()) {
        return answer(false, QStringLiteral("No plugin here has that variant"),
                      picks);
    }
    if (picks.size() - (picks.contains(QLatin1String(Obd2Pick)) ? 1 : 0) >=
        MaxPicks) {
        return answer(false,
                      QStringLiteral("A vehicle picks at most %1 plugin "
                                     "variants: unpick one first")
                          .arg(MaxPicks),
                      picks);
    }
    return answer(true, QString(), picks + QStringList{profile});
}

PluginStore::Parts PluginStore::partFiles(const QStringList& picks,
                                          const QString& own)
{
    (void)all();
    Parts parts;
    QStringList order = picks;
    if (!own.isEmpty() && order.removeAll(own) > 0) {
        order << own;
    }
    for (const QString& profile : order) {
        if (profile == QLatin1String(Obd2Pick)) {
            parts.obd2 = true;
            continue;
        }
        int at = 0;
        const auto it = heldWithProfile(profile, &at);
        const ecuio_signal_file_t* file =
            it == m_held.end()
                ? nullptr
                : ecuio_plugin_variant_file(it->second.plugin,
                                            static_cast<uint32_t>(at));
        if (file == nullptr) {
            parts.missing << profile;
            continue;
        }
        parts.files.push_back(file);
        parts.profiles << profile;
        parts.hashes << it->first;
    }
    return parts;
}

void PluginStore::setBusy(const ecuio_signal_file_t* merged)
{
    m_busy.clear();
    const uint32_t count = ecuio_signal_file_part_count(merged);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_signal_part_t part{};
        part.struct_size = sizeof part;
        if (ecuio_signal_file_part(merged, i, &part) == ECUIO_OK &&
            (part.flags & ECUIO_PART_PLUGIN) != 0) {
            m_busy << QString::fromUtf8(part.plugin_id);
        }
    }
}

QStringList PluginStore::unpick(const QStringList& picks,
                                const QString& profile)
{
    QStringList out = picks;
    out.removeAll(profile);
    return out;
}

QVariantList PluginStore::pickFiles(const QStringList& picks)
{
    (void)all();
    QVariantList out;
    for (const QString& profile : picks) {
        int at = 0;
        const auto it = heldWithProfile(profile, &at);
        if (it == m_held.end()) {
            continue;
        }
        const Held& h = it->second;
        const QString variant = h.variants.at(at);
        QString words;
        const QByteArray written = variantBytes(it->first, variant, &words);
        QStringList from;
        const QJsonObject wrapped = facts(h.id).value(kWrappedFrom).toObject();
        for (auto w = wrapped.begin(); w != wrapped.end(); ++w) {
            if (w.value().toString() == variant) {
                from << w.key();
            }
        }
        out << QVariantMap{
            {QStringLiteral("profile"), profile},
            {QStringLiteral("name"),
             h.variants.size() > 1
                 ? QStringLiteral("%1: %2").arg(h.name, variant)
                 : h.name},
            {QStringLiteral("sha256"),
             QString::fromLatin1(
                 QCryptographicHash::hash(written, QCryptographicHash::Sha256)
                     .toHex())},
            {QStringLiteral("from"), from}};
    }
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
