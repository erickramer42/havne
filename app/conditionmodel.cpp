// File/layer: app/conditionmodel.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "conditionmodel.h"

#include <QStringList>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <limits>

ConditionModel::ConditionModel(QObject* parent) : QAbstractListModel(parent) {}

int ConditionModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QHash<int, QByteArray> ConditionModel::roleNames() const
{
    return {{KeyRole, "key"},
            {NameRole, "name"},
            {EnabledRole, "enabled"},
            {LevelRole, "level"},
            {UsesRole, "uses"},
            {SoundRole, "sound"},
            {BannerRole, "banner"},
            {LiveRole, "live"},
            {StateRole, "state"},
            {StaleRole, "stale"},
            {UnavailableRole, "unavailable"},
            {WhyRole, "why"},
            {FiredRole, "fired"},
            {ValueRole, "value"},
            {UnitRole, "unit"},
            {DecimalsRole, "decimals"},
            {SinceMsRole, "sinceMs"},
            {DefIdsRole, "defIds"}};
}

QString ConditionModel::whyText(uint32_t why)
{
    switch (why) {
    case ECUIO_CONDITION_WHY_NONE:
        return {};
    case ECUIO_CONDITION_WHY_DISABLED:
        return QStringLiteral("turned off");
    case ECUIO_CONDITION_WHY_NO_SIGNAL:
        return QStringLiteral("this session has none of its signals");
    case ECUIO_CONDITION_WHY_UNIT:
        return QStringLiteral("its unit isn't its signal's");
    case ECUIO_CONDITION_WHY_NOT_A_NUMBER:
        return QStringLiteral("its signal is text, not a number");
    case ECUIO_CONDITION_WHY_ENUM_ORDER:
        return QStringLiteral("above or below on a signal of named states");
    case ECUIO_CONDITION_WHY_NO_LINK:
        return QStringLiteral("this session has no link state to judge");
    case ECUIO_CONDITION_WHY_NO_CHECK:
        return QStringLiteral("this session doesn't check its frames");
    case ECUIO_CONDITION_WHY_NOT_RECEIVED:
        return QStringLiteral("the ECU doesn't send its signal");
    default:
        return QStringLiteral("unknown reason %1").arg(why);
    }
}

ecuio_display_value_t ConditionModel::shown(const Row& r, double value,
                                            uint32_t term) const
{
    ecuio_display_value_t out{};
    const Term* t = term < r.terms.size() ? &r.terms[term] : nullptr;
    if (t == nullptr || t->defId == UINT32_MAX ||
        ecuio_display_convert(t->unit.constData(), t->decimals, value, m_units,
                              &out) != ECUIO_DISPLAY_OK) {
        out = ecuio_display_value_t{};
        out.value = value;
    }
    return out;
}

QVariant ConditionModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= m_rows.size()) {
        return {};
    }
    const Row& r = m_rows[static_cast<size_t>(index.row())];
    const ecuio_condition_state_t& st = r.state;
    const bool hasValue = r.live && st.since_us > 0;
    switch (role) {
    case KeyRole:
        return r.key;
    case NameRole:
        return r.name;
    case EnabledRole:
        return (r.flags & ECUIO_CONDITION_INFO_DISABLED) == 0;
    case LevelRole:
        return r.level == ECUIO_ALERT_DANGER    ? QStringLiteral("danger")
               : r.level == ECUIO_ALERT_WARNING ? QStringLiteral("warning")
                                                : QString();
    case UsesRole: {
        QStringList uses;
        if (r.uses & ECUIO_CONDITION_USE_ALERT) {
            uses << QStringLiteral("alert");
        }
        if (r.uses & ECUIO_CONDITION_USE_RECORD) {
            uses << QStringLiteral("record");
        }
        if (r.uses & ECUIO_CONDITION_USE_MARK) {
            uses << QStringLiteral("mark");
        }
        return uses;
    }
    case SoundRole:
        return (r.uses & ECUIO_CONDITION_ALERT_SOUND) != 0;
    case BannerRole:
        return (r.uses & ECUIO_CONDITION_ALERT_BANNER) != 0;
    case LiveRole:
        return r.live;
    case StateRole:
        return r.live && st.state == ECUIO_CONDITION_ON ? QStringLiteral("on")
                                                        : QStringLiteral("off");
    case StaleRole:
        return r.live && (st.flags & ECUIO_CONDITION_STALE) != 0;
    case UnavailableRole:
        return r.live && (st.flags & ECUIO_CONDITION_UNAVAILABLE) != 0;
    case WhyRole:
        return r.live && (st.flags & ECUIO_CONDITION_UNAVAILABLE) != 0
                   ? whyText(st.why)
                   : QString();
    case FiredRole:
        return r.live ? static_cast<qint64>(st.fired) : qint64{0};
    case ValueRole:
        return hasValue ? shown(r, st.value, r.valueTerm).value
                        : std::numeric_limits<double>::quiet_NaN();
    case UnitRole:
        return QString::fromUtf8(shown(r, 0.0, r.valueTerm).unit);
    case DecimalsRole:
        return shown(r, 0.0, r.valueTerm).decimal_places;
    case SinceMsRole:
        return hasValue ? static_cast<qint64>(st.since_us / 1000) : qint64{-1};
    case DefIdsRole: {
        QVariantList ids;
        for (const Term& t : r.terms) {
            ids << (t.defId == UINT32_MAX ? -1 : static_cast<int>(t.defId));
        }
        return ids;
    }
    default:
        return {};
    }
}

namespace {

// A NaN value before the first sample equals the next NaN
bool sameState(const ecuio_condition_state_t& a,
               const ecuio_condition_state_t& b)
{
    const bool sameValue =
        a.value == b.value || (std::isnan(a.value) && std::isnan(b.value));
    return a.state == b.state && a.flags == b.flags &&
           a.since_us == b.since_us && a.fired == b.fired && sameValue &&
           a.why == b.why && a.generation == b.generation;
}

const ecuio_signal_def_t* defOfId(const ecuio_signal_set_t* set, uint32_t id)
{
    if (set == nullptr || set->defs == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        if (set->defs[i].id == id) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

} // namespace

void ConditionModel::setConditions(const ecuio_condition_set_t* set,
                                   const ecuio_signal_set_t* signal_set,
                                   const ecuio_signal_file_t* file)
{
    const int before = count();
    beginResetModel();
    m_rows.clear();
    const uint32_t n = ecuio_condition_set_count(set);
    m_rows.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ecuio_condition_info_t info{};
        if (ecuio_condition_set_get(set, i, &info) != ECUIO_OK) {
            continue;
        }
        Row r;
        r.key = QString::fromUtf8(info.key);
        r.name = QString::fromUtf8(info.name);
        r.flags = info.flags;
        r.uses = info.uses;
        r.level = info.alert_level;
        r.terms.resize(info.term_count);
        ecuio_condition_binding_t b{};
        if (signal_set != nullptr &&
            ecuio_condition_set_resolve(set, i, signal_set, file, &b) ==
                ECUIO_OK) {
            bool valued = false;
            for (uint32_t t = 0;
                 t < info.term_count && t < ECUIO_CONDITION_MAX_TERMS; ++t) {
                const ecuio_signal_def_t* def =
                    defOfId(signal_set, b.signal_id[t]);
                if (def == nullptr) {
                    continue;
                }
                r.terms[t].defId = def->id;
                r.terms[t].unit =
                    QByteArray(def->unit, static_cast<qsizetype>(qstrnlen(
                                              def->unit, sizeof def->unit)));
                r.terms[t].decimals = def->decimal_places;
                if (!valued) {
                    r.valueTerm = t;
                    valued = true;
                }
            }
        }
        m_rows.push_back(std::move(r));
    }
    endResetModel();
    if (count() != before) {
        emit countChanged();
    }
}

void ConditionModel::applyStates(const ecuio_condition_state_t* states,
                                 uint32_t n)
{
    for (size_t i = 0; i < m_rows.size() && i < n; ++i) {
        Row& r = m_rows[i];
        const ecuio_condition_state_t& s = states[i];
        if (r.live && sameState(r.state, s)) {
            continue;
        }
        r.state = s;
        r.live = true;
        const QModelIndex at = index(static_cast<int>(i));
        emit dataChanged(at, at,
                         {LiveRole, StateRole, StaleRole, UnavailableRole,
                          WhyRole, FiredRole, ValueRole, SinceMsRole});
    }
}

void ConditionModel::applyTerm(int row, uint32_t term)
{
    if (row < 0 || static_cast<size_t>(row) >= m_rows.size()) {
        return;
    }
    Row& r = m_rows[static_cast<size_t>(row)];
    if (term >= r.terms.size() || term == r.valueTerm) {
        return;
    }
    r.valueTerm = term;
    const QModelIndex at = index(row);
    emit dataChanged(at, at, {ValueRole, UnitRole, DecimalsRole});
}

void ConditionModel::clearStates()
{
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (!r.live) {
            continue;
        }
        r.live = false;
        r.state = ecuio_condition_state_t{};
        const QModelIndex at = index(static_cast<int>(i));
        emit dataChanged(at, at,
                         {LiveRole, StateRole, StaleRole, UnavailableRole,
                          WhyRole, FiredRole, ValueRole, SinceMsRole});
    }
}

void ConditionModel::setUnitSystem(uint32_t units)
{
    if (units == m_units) {
        return;
    }
    m_units = units;
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(count() - 1),
                         {ValueRole, UnitRole, DecimalsRole});
    }
}

QString ConditionModel::prefix(const Row& r) const
{
    return (r.uses & ECUIO_CONDITION_USE_ALERT) != 0
               ? QStringLiteral("Alert")
               : QStringLiteral("Condition");
}

QString ConditionModel::firingText(int row, double value, uint32_t term) const
{
    if (row < 0 || static_cast<size_t>(row) >= m_rows.size()) {
        return {};
    }
    const Row& r = m_rows[static_cast<size_t>(row)];
    const bool signal =
        term < r.terms.size() && r.terms[term].defId != UINT32_MAX;
    if (!signal || !std::isfinite(value)) {
        return QStringLiteral("%1: %2").arg(prefix(r), r.name);
    }
    const ecuio_display_value_t v = shown(r, value, term);
    const QString number = QString::number(v.value, 'f', v.decimal_places);
    return v.unit[0] == '\0'
               ? QStringLiteral("%1: %2, %3").arg(prefix(r), r.name, number)
               : QStringLiteral("%1: %2, %3 %4")
                     .arg(prefix(r), r.name, number, QString::fromUtf8(v.unit));
}

QString ConditionModel::firingsText(int row, uint64_t firings) const
{
    if (row < 0 || static_cast<size_t>(row) >= m_rows.size()) {
        return {};
    }
    const Row& r = m_rows[static_cast<size_t>(row)];
    return QStringLiteral("%1: %2 fired %3 times")
        .arg(prefix(r), r.name)
        .arg(firings);
}

std::map<uint32_t, int> ConditionModel::alertLevels() const
{
    std::map<uint32_t, int> levels;
    for (const Row& r : m_rows) {
        if ((r.uses & ECUIO_CONDITION_USE_ALERT) == 0 || !r.live ||
            r.state.state != ECUIO_CONDITION_ON) {
            continue;
        }
        for (const Term& t : r.terms) {
            if (t.defId == UINT32_MAX) {
                continue;
            }
            int& level = levels[t.defId];
            level = std::max(level, static_cast<int>(r.level));
        }
    }
    return levels;
}
