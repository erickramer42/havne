#include "sessionbridge.h"

#include <QSet>

#include <utility>

#include "bridgeformats.h"
#include "bridgetext.h"

namespace {

const char* const kObd2 = PluginStore::Obd2Pick;

QString countText(int n, const char* one)
{
    return QStringLiteral("%1 %2%3")
        .arg(n)
        .arg(QLatin1String(one))
        .arg(n == 1 ? QString() : QStringLiteral("s"));
}

// Every key present, so QML reads one shape
QVariantMap answerOf(bool ok, const QString& reads, const QString& words)
{
    return {{QStringLiteral("ok"), ok},
            {QStringLiteral("words"), words},
            {QStringLiteral("reads"), reads},
            {QStringLiteral("profile"), QString()},
            {QStringLiteral("name"), QString()},
            {QStringLiteral("base"), QString()},
            {QStringLiteral("parts"), QVariantList()},
            {QStringLiteral("totals"), QVariantMap()},
            {QStringLiteral("folded"), QVariantList()},
            {QStringLiteral("notes"), QVariantList()},
            {QStringLiteral("beside"), QVariantList()},
            {QStringLiteral("shareAlike"), QStringList()},
            {QStringLiteral("missing"), QStringList()}};
}

QString pickWords(const QString& profile)
{
    const QString prefix = QStringLiteral("plugin.");
    const qsizetype dot = profile.indexOf(QLatin1Char('.'), prefix.size());
    if (!profile.startsWith(prefix) || dot < 0) {
        return profile;
    }
    return profile.mid(prefix.size(), dot - prefix.size()) + QLatin1Char(' ') +
           profile.mid(dot + 1);
}

QVariantMap missingAnswer(const QStringList& missing, const QString& label)
{
    QStringList named;
    for (const QString& m : missing) {
        named << pickWords(m);
    }
    const bool one = missing.size() == 1;
    QVariantMap answer = answerOf(
        false, QStringLiteral("refused"),
        QStringLiteral("%1 picks %2, which %3 installed here: install %4 "
                       "or unpick %4 on Settings, Signals")
            .arg(label.isEmpty() ? QStringLiteral("This vehicle") : label,
                 named.join(QStringLiteral(", ")),
                 one ? QStringLiteral("isn't") : QStringLiteral("aren't"),
                 one ? QStringLiteral("it") : QStringLiteral("them")));
    answer.insert(QStringLiteral("missing"), missing);
    return answer;
}

QVariantList partsOf(const ecuio_signal_file_t* file, const QString& own,
                     QStringList* shareAlike, int* plugins)
{
    QVariantList out;
    QSet<QString> ids;
    const uint32_t count = ecuio_signal_file_part_count(file);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_signal_part_t p{};
        p.struct_size = sizeof p;
        if (ecuio_signal_file_part(file, i, &p) != ECUIO_OK) {
            continue;
        }
        const QString profile = QString::fromUtf8(p.profile);
        const QString name = QString::fromUtf8(p.name);
        ecuio_signal_source_t src{};
        src.struct_size = sizeof src;
        if (ecuio_signal_file_part_source(file, i, &src) == ECUIO_OK &&
            (src.flags & ECUIO_SOURCE_SHARE_ALIKE) != 0) {
            *shareAlike << name;
        }
        if ((p.flags & ECUIO_PART_PLUGIN) != 0) {
            ids << QString::fromUtf8(p.plugin_id);
        }
        out << QVariantMap{
            {QStringLiteral("profile"), profile},
            {QStringLiteral("name"), name},
            {QStringLiteral("plugin"), QString::fromUtf8(p.plugin_id)},
            {QStringLiteral("version"), QString::fromUtf8(p.plugin_version)},
            {QStringLiteral("variant"), QString::fromUtf8(p.plugin_variant)},
            {QStringLiteral("format"), static_cast<int>(p.format)},
            {QStringLiteral("signals"), static_cast<int>(p.signal_count)},
            {QStringLiteral("modules"), static_cast<int>(p.module_count)},
            {QStringLiteral("own"), !own.isEmpty() && profile == own}};
    }
    *plugins = static_cast<int>(ids.size());
    return out;
}

QString kindName(uint32_t kind)
{
    switch (kind) {
    case ECUIO_NOTE_REPLACED_BY_ADDRESS:
        return QStringLiteral("replacedByAddress");
    case ECUIO_NOTE_REPLACED_BY_KEY:
        return QStringLiteral("replacedByKey");
    case ECUIO_NOTE_KEY_TAKEN:
        return QStringLiteral("keyTaken");
    case ECUIO_NOTE_BESIDE_BASE:
        return QStringLiteral("besideBase");
    case ECUIO_NOTE_MODULE_RENAMED:
        return QStringLiteral("moduleRenamed");
    case ECUIO_NOTE_MODULE_LEFT_OUT:
        return QStringLiteral("moduleLeftOut");
    case ECUIO_NOTE_PART_LEFT_OUT:
        return QStringLiteral("partLeftOut");
    case ECUIO_NOTE_SIGNAL_LEFT_OUT:
        return QStringLiteral("signalLeftOut");
    default:
        return QStringLiteral("other");
    }
}

QVariantMap besideOf(const ecuio_signal_file_t* file,
                     const std::vector<ecuio_signal_key_t>& keys,
                     const ecuio_signal_note_t& n, const QVariantList& parts)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    const ecuio_signal_def_t& base = set->defs[n.base_def];
    const ecuio_signal_def_t& standing = set->defs[n.def];
    const char* own = ecuio_signal_file_key(file, &standing);
    return {
        {QStringLiteral("baseDef"), static_cast<int>(n.base_def)},
        {QStringLiteral("baseRef"), QStringLiteral("id:%1").arg(base.id)},
        {QStringLiteral("baseTick"), QString::fromUtf8(keys[n.base_def].text)},
        {QStringLiteral("standing"), static_cast<int>(n.def)},
        {QStringLiteral("standingKey"),
         own != nullptr && own[0] != '\0'
             ? QString::fromUtf8(own)
             : QString::fromUtf8(keys[n.def].text)},
        {QStringLiteral("name"), QString::fromUtf8(standing.name)},
        {QStringLiteral("part"), n.part < static_cast<uint32_t>(parts.size())
                                     ? parts.at(static_cast<qsizetype>(n.part))
                                           .toMap()
                                           .value(QStringLiteral("name"))
                                           .toString()
                                     : QString()}};
}

std::vector<ecuio_signal_key_t> keysOf(const ecuio_signal_file_t* file)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t count = 0;
    if (ecuio_signal_set_keys(set, file, keys.data(), set->count, &count) !=
        ECUIO_OK) {
        keys.assign(set->count, ecuio_signal_key_t{});
    }
    return keys;
}

void notesInto(const ecuio_signal_file_t* file, QVariantMap& answer)
{
    const QVariantList parts = answer.value(QStringLiteral("parts")).toList();
    const uint32_t defs = ecuio_signal_file_set(file)->count;
    const std::vector<ecuio_signal_key_t> keys = keysOf(file);
    QVariantList notes;
    QVariantList beside;
    std::map<uint32_t, int> byKind;
    const uint32_t count = ecuio_signal_file_note_count(file);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_signal_note_t n{};
        n.struct_size = sizeof n;
        if (ecuio_signal_file_note(file, i, &n) != ECUIO_OK) {
            continue;
        }
        ++byKind[n.kind];
        notes << QVariantMap{
            {QStringLiteral("kind"), kindName(n.kind)},
            {QStringLiteral("text"), QString::fromUtf8(n.text)}};
        if (n.kind == ECUIO_NOTE_BESIDE_BASE && n.base_def < defs &&
            n.def < defs) {
            beside << besideOf(file, keys, n, parts);
        }
    }
    QVariantList folded;
    for (const auto& k : byKind) {
        folded << QVariantMap{{QStringLiteral("kind"), kindName(k.first)},
                              {QStringLiteral("count"), k.second}};
    }
    answer.insert(QStringLiteral("notes"), notes);
    answer.insert(QStringLiteral("folded"), folded);
    answer.insert(QStringLiteral("beside"), beside);
}

QVariantMap mergedAnswer(const ecuio_signal_file_t* file, const QString& own)
{
    QVariantMap answer = answerOf(true, QStringLiteral("merged"), QString());
    QStringList shareAlike;
    int plugins = 0;
    answer.insert(QStringLiteral("parts"),
                  partsOf(file, own, &shareAlike, &plugins));
    answer.insert(QStringLiteral("shareAlike"), shareAlike);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    answer.insert(QStringLiteral("profile"),
                  QString::fromUtf8(ecuio_signal_file_profile(file)));
    answer.insert(QStringLiteral("name"),
                  QString::fromUtf8(ecuio_signal_file_name(file)));
    const char* extends = ecuio_signal_file_extends(file);
    answer.insert(QStringLiteral("base"),
                  extends != nullptr && extends[0] != '\0'
                      ? QStringLiteral("obd2")
                      : QStringLiteral("modules"));
    answer.insert(
        QStringLiteral("totals"),
        QVariantMap{
            {QStringLiteral("plugins"), plugins},
            {QStringLiteral("modules"), static_cast<int>(set->channel_count)},
            {QStringLiteral("signals"), static_cast<int>(set->count)}});
    notesInto(file, answer);
    return answer;
}

// A plugin's new version is a new copy hash, so it merges again
QString keyOf(const PluginStore::Parts& parts, const QString& own,
              const QString& label)
{
    QStringList key{parts.obd2 ? QStringLiteral("obd2") : QStringLiteral("-"),
                    parts.profiles.contains(own) ? own : QString()};
    for (qsizetype i = 0; i < parts.profiles.size(); ++i) {
        key << parts.profiles.at(i) + QLatin1Char('@') + parts.hashes.at(i);
    }
    if (!parts.missing.isEmpty()) {
        key << QStringLiteral("missing") << parts.missing << label;
    }
    return key.join(QLatin1Char('\n'));
}

// A later merge may hold the name now (last wins): leave it registered
void dropMerged(ecuio_signal_file_t* file)
{
    const char* name = ecuio_signal_file_profile(file);
    const ecuio_signal_set_t* now = nullptr;
    if (ecuio_profile_lookup(name, &now) == ECUIO_OK &&
        now == ecuio_signal_file_set(file)) {
        (void)ecuio_profile_unregister(name);
    }
    ecuio_signal_file_free(file);
}

} // namespace

QVariantMap SessionBridge::vehicleSet(const QStringList& picks,
                                      const QString& own, const QString& label)
{
    const PluginStore::Parts parts = m_pluginStore.partFiles(picks, own);
    const QString key = keyOf(parts, own, label);
    if (!m_together.key.isEmpty() && key == m_together.key) {
        return m_together.answer;
    }
    QVariantMap answer;
    ecuio_signal_file_t* made = nullptr;
    if (!parts.missing.isEmpty()) {
        answer = missingAnswer(parts.missing, label);
    } else if (parts.files.empty() && parts.obd2) {
        answer = answerOf(true, QStringLiteral("obd2"), QString());
        answer.insert(QStringLiteral("profile"), QLatin1String(kObd2));
        answer.insert(QStringLiteral("base"), QLatin1String(kObd2));
    } else {
        made = mergeTogether(parts, own, &answer);
    }
    if (m_together.file != nullptr) {
        m_togetherRetired.push_back(m_together.file);
    }
    m_together = {made, key, answer};
    releaseTogether(false);
    return answer;
}

ecuio_signal_file_t*
SessionBridge::mergeTogether(const PluginStore::Parts& parts,
                             const QString& own, QVariantMap* answer)
{
    ecuio_merge_options_t options{};
    options.struct_size = sizeof options;
    options.flags =
        parts.obd2 ? ECUIO_MERGE_BASE_OBD2 : ECUIO_MERGE_BASE_MODULES;
    ecuio_signal_file_t* made = nullptr;
    ++m_togetherMerges;
    ecuio_status_t rc = ecuio_signal_file_merge(
        parts.files.data(), static_cast<uint32_t>(parts.files.size()), &options,
        &made);
    if (rc == ECUIO_OK) {
        rc = ecuio_profile_register(ecuio_signal_file_profile(made),
                                    ecuio_signal_file_set(made), nullptr);
    }
    if (rc != ECUIO_OK) {
        const QString words = bridgetext::detailOr(rc);
        ecuio_signal_file_free(made);
        *answer = answerOf(false, QStringLiteral("refused"), words);
        emit logMessage(QStringLiteral("Read together refused: %1").arg(words));
        return nullptr;
    }
    *answer = mergedAnswer(made, own);
    const QVariantMap totals = answer->value(QStringLiteral("totals")).toMap();
    emit logMessage(
        QStringLiteral("Read together: %1, %2, %3, %4 (%5)")
            .arg(
                countText(totals.value(QStringLiteral("plugins")).toInt(),
                          "plugin"),
                countText(totals.value(QStringLiteral("modules")).toInt(),
                          "module"),
                bridgetext::signalCountText(
                    totals.value(QStringLiteral("signals")).toInt()),
                countText(
                    static_cast<int>(
                        answer->value(QStringLiteral("notes")).toList().size()),
                    "note"),
                answer->value(QStringLiteral("profile")).toString()));
    return made;
}

void SessionBridge::endTeardown()
{
    m_disconnecting = false;
    m_heldSet = nullptr;
    releaseTogether(false);
}

const ecuio_signal_file_t*
SessionBridge::togetherFileOf(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    if (m_together.file != nullptr &&
        ecuio_signal_file_set(m_together.file) == set) {
        return m_together.file;
    }
    for (const ecuio_signal_file_t* f : m_togetherRetired) {
        if (ecuio_signal_file_set(f) == set) {
            return f;
        }
    }
    return nullptr;
}

void SessionBridge::releaseTogether(bool all)
{
    const ecuio_signal_set_t* used = all ? nullptr : m_heldSet;
    m_pluginStore.setBusy(togetherFileOf(used));
    // The connect thread looks its profile up by name, maybe after a merge
    if (!all && m_connecting) {
        return;
    }
    std::vector<ecuio_signal_file_t*> kept;
    for (ecuio_signal_file_t* f : std::exchange(m_togetherRetired, {})) {
        if (used != nullptr && ecuio_signal_file_set(f) == used) {
            kept.push_back(f);
        } else {
            dropMerged(f);
        }
    }
    m_togetherRetired = std::move(kept);
    if (all && m_together.file != nullptr) {
        dropMerged(m_together.file);
        m_together = {};
    }
}

void SessionBridge::holdUriSet(const QString& uri)
{
    const QByteArray name = BridgeFormats::profileOfUri(uri).toUtf8();
    const ecuio_signal_set_t* set = nullptr;
    m_heldSet = ecuio_profile_lookup(name.constData(), &set) == ECUIO_OK
                    ? set
                    : nullptr;
    releaseTogether(false);
}

QString SessionBridge::vehicleSetText(const QString& profile) const
{
    const ecuio_signal_set_t* set = nullptr;
    const QByteArray name = profile.toUtf8();
    if (ecuio_profile_lookup(name.constData(), &set) != ECUIO_OK) {
        return {};
    }
    size_t length = 0;
    const char* text = ecuio_signal_file_text(togetherFileOf(set), &length);
    return QString::fromUtf8(text, static_cast<qsizetype>(length));
}
