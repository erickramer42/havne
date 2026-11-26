#include "signalmodules.h"

#include <QHash>
#include <QSet>

namespace signal_modules {

namespace {

QString moduleName(const ecuio_signal_set_t* set,
                   const ecuio_signal_file_t* file, uint32_t index)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    if (file != nullptr &&
        ecuio_signal_file_module(file, index, &m) == ECUIO_OK) {
        if (m.name[0] != '\0') {
            return QString::fromUtf8(m.name);
        }
        if (m.id[0] != '\0') {
            return QString::fromUtf8(m.id).toUpper();
        }
    }
    return QString::number(set->channels[index].rx_id, 16).toUpper();
}

QString partName(const ecuio_signal_file_t* file, const ecuio_signal_def_t& def)
{
    const uint32_t at = ecuio_signal_file_def_part(file, &def);
    ecuio_signal_part_t p{};
    p.struct_size = sizeof p;
    if (at != ECUIO_SIGNAL_PART_NONE &&
        ecuio_signal_file_part(file, at, &p) == ECUIO_OK) {
        return QString::fromUtf8(p.name);
    }
    // A merged set's base def belongs to no part
    const char* extends = ecuio_signal_file_extends(file);
    return ecuio_signal_file_part_count(file) > 0 && extends != nullptr &&
                   extends[0] != '\0'
               ? QString::fromUtf8(extends)
               : QString();
}

// Whether every row of the group has a distinct, non-empty value
template <typename Of>
bool tellsApart(const std::vector<Row>& rows, const std::vector<int>& group,
                Of of)
{
    QSet<QString> seen;
    for (int r : group) {
        const QString v = of(rows[static_cast<size_t>(r)]);
        if (v.isEmpty() || seen.contains(v)) {
            return false;
        }
        seen.insert(v);
    }
    return true;
}

} // namespace

std::vector<Row> of(const ecuio_signal_set_t* set,
                    const ecuio_signal_file_t* file,
                    const std::vector<ecuio_signal_key_t>& keys)
{
    if (set == nullptr || set->defs == nullptr) {
        return {};
    }
    std::vector<Row> rows(set->count);
    QHash<QString, std::vector<int>> byName;
    for (uint32_t i = 0; i < rows.size(); ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        Row& row = rows[i];
        const uint32_t m = ecuio_signal_set_def_module(set, &def);
        if (m != ECUIO_SIGNAL_MODULE_NONE && m < set->channel_count) {
            row.module = static_cast<int>(m);
            row.moduleName = moduleName(set, file, m);
        }
        row.part = file != nullptr ? partName(file, def) : QString();
        byName[QString::fromUtf8(def.name)].push_back(static_cast<int>(i));
    }
    for (auto it = byName.cbegin(); it != byName.cend(); ++it) {
        const std::vector<int>& group = it.value();
        if (group.size() < 2) {
            continue;
        }
        const bool byModule =
            tellsApart(rows, group, [](const Row& r) { return r.moduleName; });
        const bool byPart =
            !byModule &&
            tellsApart(rows, group, [](const Row& r) { return r.part; });
        for (int r : group) {
            Row& row = rows[static_cast<size_t>(r)];
            row.tag =
                byModule ? row.moduleName
                : byPart ? row.part
                         : QString::fromUtf8(keys[static_cast<size_t>(r)].text);
        }
    }
    return rows;
}

} // namespace signal_modules
