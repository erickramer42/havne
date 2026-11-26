// File/layer: app/signallistmodel.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "signallistmodel.h"
#include "sessionbridge.h"
#include "ecuio/profile.h"
#include "signalmodules.h"

#include <QDateTime>
#include <QVariantMap>

#include <cmath>
#include <algorithm>
#include <iterator>
#include <limits>

namespace {

// Widens [first, last] to take row; first < 0 is an empty range.
void widenRows(int row, int& first, int& last)
{
    if (first < 0 || row < first) first = row;
    if (row > last) last = row;
}

// Each def's key, with the file's own for the defs it keys itself; a
// refusal (no memory) leaves every def without one
std::vector<ecuio_signal_key_t> keysOf(const ecuio_signal_set_t* set,
                                       const ecuio_signal_file_t* file)
{
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t keyed = 0;
    if (ecuio_signal_set_keys(set, file, keys.data(), set->count, &keyed) !=
            ECUIO_OK ||
        keyed != set->count) {
        keys.assign(set->count, ecuio_signal_key_t{});
    }
    return keys;
}

} // namespace

// A value, its unit and its decimals in a unit system (ADR-0022)
ecuio_display_value_t displayed(const char* unit, uint8_t decimals,
                                double value, uint32_t units)
{
    ecuio_display_value_t out;
    if (ecuio_display_convert(unit, decimals, value, units, &out) !=
        ECUIO_DISPLAY_OK) {
        // units is only ever metric or imperial, so this is unreachable;
        // show the decoded value rather than nothing
        out = {};
        out.value = value;
        qstrncpy(out.unit, unit, sizeof(out.unit));
        out.decimal_places = decimals;
    }
    return out;
}

// A def's range as displayed in a unit system: NaN for both ends when
// either is NaN (core gives none), converts past a double (a finite end
// near DBL_MAX times 1.8 is inf, and inf isn't a range), or the converted
// ends meet (ends one ulp apart can round to one number, and lo == hi
// isn't a range, as core's refusal of a == b says)
std::pair<double, double> displayedRange(const char* unit,
                                         uint8_t decimals, double lo,
                                         double hi, uint32_t units)
{
    const double none = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(lo) || std::isnan(hi)) {
        return {none, none};
    }
    const double l = displayed(unit, decimals, lo, units).value;
    const double h = displayed(unit, decimals, hi, units).value;
    if (!std::isfinite(l) || !std::isfinite(h) || !(l < h)) {
        return {none, none};
    }
    return {l, h};
}

// ---------------- SignalListModel ----------------
SignalListModel::SignalListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int SignalListModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(m_entries.size());
}

QVariant SignalListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_entries.size())) {
        return QVariant();
    }
    const Entry& e = m_entries[static_cast<size_t>(index.row())];
    switch (role) {
        case IdRole: return e.id;
        case NameRole: return QString::fromUtf8(e.name);
        case UnitRole: return QString::fromUtf8(shown(e, e.value).unit);
        case ValueRole: return shown(e, e.value).value;
        case SubscribedRole: return e.subscribed;
        case HasValueRole: return e.histCount > 0;
        case AgeMsRole:
            if (e.arrivedMs < 0) return -1;
            return static_cast<int>(m_clock.elapsed() - e.arrivedMs);
        case DecimalsRole: return static_cast<int>(shown(e, e.value).decimal_places);
        case TextRole: return e.text;
        case IsTextRole: return e.isText;
        case WindowHighRole: return shown(e, e.windowHigh).value;
        case WindowLowRole: return shown(e, e.windowLow).value;
        case RowRole: return index.row();
        case HiddenRole: return e.hidden;
        case HiddenWhyRole: return QString::fromLatin1(whyWords(e.why));
        case GroupRole:
            return static_cast<int>(e.group);
        case LabelRole:
            return labelOf(e);
        case RefusedRole:
            return e.refused;
        case RefusedCodeRole:
            return e.refusedCode;
        case RefRole:
            return e.facts.ref;
        case RangeLowRole:
            return shownRange(e).first;
        case RangeHighRole:
            return shownRange(e).second;
        case IsEnumRole:
            return e.facts.isEnum;
        case UnaskedRole:
            return e.unasked;
        case KeyRole:
            return QString::fromUtf8(e.key);
        case ModuleRole:
            return e.module;
        case ModuleNameRole:
            return e.moduleName;
        case TagRole:
            return e.tag;
        case UnaskedWhyRole:
            return e.unasked  ? QStringLiteral("pick")
                   : e.noRoom ? QStringLiteral("room")
                              : QString();
        default: return QVariant();
    }
}

QHash<int, QByteArray> SignalListModel::roleNames() const
{
    return {{IdRole, "id"},
            {NameRole, "name"},
            {UnitRole, "unit"},
            {ValueRole, "value"},
            {SubscribedRole, "subscribed"},
            {HasValueRole, "hasValue"},
            {AgeMsRole, "ageMs"},
            {DecimalsRole, "decimals"},
            {TextRole, "text"},
            {IsTextRole, "isText"},
            {WindowHighRole, "windowHigh"},
            {WindowLowRole, "windowLow"},
            {RowRole, "row"},
            {HiddenRole, "hidden"},
            {GroupRole, "group"},
            {LabelRole, "label"},
            {RefusedRole, "refused"},
            {RefusedCodeRole, "refusedCode"},
            {RefRole, "ref"},
            {RangeLowRole, "rangeLow"},
            {RangeHighRole, "rangeHigh"},
            {IsEnumRole, "isEnum"},
            {UnaskedRole, "unasked"},
            {HiddenWhyRole, "hiddenWhy"},
            {KeyRole, "key"},
            {ModuleRole, "module"},
            {ModuleNameRole, "moduleName"},
            {TagRole, "tag"},
            {UnaskedWhyRole, "unaskedWhy"}};
}

const char* SignalListModel::whyWords(Entry::Why why)
{
    return why == Entry::Why::Unsupported ? "unsupported"
         : why == Entry::Why::Absent      ? "absent"
                                          : "";
}

QVariantList SignalListModel::hiddenSignals() const
{
    QVariantList rows;
    for (const Entry& e : m_entries) {
        if (e.hidden) {
            rows.append(QVariantMap{{QStringLiteral("name"), QString::fromUtf8(e.name)},
                                    {QStringLiteral("why"), QString::fromLatin1(whyWords(e.why))}});
        }
    }
    return rows;
}

bool SignalListModel::anyValue() const
{
    for (const Entry& e : m_entries) {
        if (e.histCount > 0) {
            return true;
        }
    }
    return false;
}

bool SignalListModel::isBuiltIn(const ecuio_signal_set_t* set)
{
    // The registry's sets, so the rule is the profile's whatever binds it
    const ecuio_signal_set_t* mock = nullptr;
    const ecuio_signal_set_t* obd2 = nullptr;
    (void)ecuio_profile_lookup("mock", &mock);
    (void)ecuio_profile_lookup("obd2", &obd2);
    return set != nullptr && (set == mock || set == obd2);
}

SignalListModel::DefFacts
SignalListModel::factsOf(const ecuio_signal_file_t* file, bool builtIn,
                         const ecuio_signal_def_t& def)
{
    DefFacts f;
    f.isEnum = def.kind == ECUIO_VALUE_ENUM;
    double lo = 0.0;
    double hi = 0.0;
    if (ecuio_def_range(&def, &lo, &hi) == ECUIO_DECODE_OK) {
        f.rangeLow = lo;
        f.rangeHigh = hi;
    } else {
        f.rangeLow = f.rangeHigh = std::numeric_limits<double>::quiet_NaN();
    }
    // ADR-0057's refs: a file's own def by its key (a def of its extends
    // base has none, and keeps the base's ID); a built-in set's by ID
    const QString byId = QStringLiteral("id:%1").arg(def.id);
    if (file != nullptr) {
        const char* key = ecuio_signal_file_key(file, &def);
        f.ref = (key != nullptr && key[0] != '\0')
                    ? QStringLiteral("key:") + QString::fromUtf8(key)
                    : byId;
    } else if (builtIn) {
        f.ref = byId;
    }
    return f;
}

void SignalListModel::setSignalSet(const ecuio_signal_set_t* set,
                                   const ecuio_signal_file_t* file,
                                   bool pickOnly)
{
    m_pickOnly = pickOnly;
    // What the last set's choices left unanswered was that set's to carry
    m_carriedPlotted.clear();
    m_carriedUnplotted.clear();
    // NULL/empty is valid (the raw profile): clear the model rather
    // than keep the previous session's rows.
    const bool hadHidden = m_hiddenCount != 0;
    m_hiddenCount = 0;
    m_ranked = false;
    // A new set's rows start asked: the bridge applies the session's list
    if (m_unaskedCount != 0) {
        m_unaskedCount = 0;
        emit unaskedCountChanged();
    }
    applyNoRoom({});
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        beginResetModel();
        m_entries.clear();
        m_rowOfId.clear();
        m_subscribedCount = 0;
        endResetModel();
        emit subscribedCountChanged();
        if (m_tickedCount != 0) {
            m_tickedCount = 0;
            emit tickedCountChanged();
        }
        if (hadHidden) emit hiddenCountChanged();
        m_clock.restart();
        return;
    }

    beginResetModel();
    m_entries.clear();
    m_rowOfId.clear();
    m_rowOfId.reserve(static_cast<qsizetype>(set->count));
    m_subscribedCount = 0;
    // Each entry holds its history ring (tens of KB): built in place,
    // never on the stack or moved by a reallocation
    m_entries.reserve(set->count);
    const bool builtIn = isBuiltIn(set);
    const std::vector<ecuio_signal_key_t> keys = keysOf(set, file);

    for (uint32_t i = 0; i < set->count; ++i) {
        // value-init zeroes the history ring, head, and count
        m_rowOfId.insert(set->defs[i].id, static_cast<int>(m_entries.size()));
        Entry& e = m_entries.emplace_back();
        e.id = set->defs[i].id;
        qstrncpy(e.name, set->defs[i].name, sizeof(e.name));   // always terminates
        qstrncpy(e.key, keys[i].text, sizeof(e.key));
        qstrncpy(e.unit, set->defs[i].unit, sizeof(e.unit));
        e.decimals = set->defs[i].decimal_places;
        e.isText = set->defs[i].kind == ECUIO_VALUE_TEXT ||
                   set->defs[i].kind == ECUIO_VALUE_HEX;
        e.facts = factsOf(file, builtIn, set->defs[i]);
        if (set->defs[i].kind == ECUIO_VALUE_ENUM && set->enums != nullptr) {
            const uint32_t first = set->defs[i].table_first;
            const uint32_t end = first + set->defs[i].table_count;
            for (uint32_t k = first; k < end && k < set->enum_count; ++k) {
                e.labels.emplace_back(set->enums[k].raw,
                                      QString::fromUtf8(set->enums[k].label));
            }
        }
        e.plotRank = set->defs[i].plot_rank;
        e.group = set->defs[i].group;
        m_ranked = m_ranked || (e.plotRank != 0 && !e.isText);
        // judgePlotted's, below; a text row has nothing to plot
        e.subscribed = false;
    }
    placeRows(set, file, keys);
    endResetModel();
    // A new set plots its defaults, whatever the last one plotted
    judgePlotted();
    emit subscribedCountChanged();
    if (hadHidden) emit hiddenCountChanged();

    m_clock.restart();
}

void SignalListModel::placeRows(const ecuio_signal_set_t* set,
                                const ecuio_signal_file_t* file,
                                const std::vector<ecuio_signal_key_t>& keys)
{
    const std::vector<signal_modules::Row> rows =
        signal_modules::of(set, file, keys);
    for (size_t i = 0; i < m_entries.size() && i < rows.size(); ++i) {
        m_entries[i].module = rows[i].module;
        m_entries[i].moduleName = rows[i].moduleName;
        m_entries[i].tag = rows[i].tag;
    }
}

void SignalListModel::refreshAges()
{
    if (m_entries.empty()) return;
    emit dataChanged(index(0),
                     index(static_cast<int>(m_entries.size()) - 1),
                     {AgeMsRole});
}

void SignalListModel::updateValues(const std::vector<std::pair<uint16_t, double>>& values,
                                   qint64 timestampMs)
{
    const qint64 stamp = (timestampMs >= 0) ? timestampMs : m_clock.elapsed();
    std::vector<WindowValue> window;
    window.reserve(values.size());
    for (const auto& pair : values) {
        WindowValue w;
        w.id = pair.first;
        w.last = w.min = w.max = pair.second;
        w.lastMs = w.minMs = w.maxMs = stamp;
        window.push_back(w);
    }
    updateWindow(window);
}

void SignalListModel::updateWindow(const std::vector<WindowValue>& window)
{
    if (m_entries.empty() || window.empty()) return;

    // Every row, subscribed or not: `subscribed` filters chartData()
    // only. Skipping unsubscribed rows here would freeze the Dashboard's
    // gauges for a signal unticked on Live, and dim them as stale while
    // its frames still arrive.
    const qint64 arrived = m_clock.elapsed();
    int firstRow = -1;
    int lastRow = -1;
    for (const WindowValue& w : window) {
        const int row = m_rowOfId.value(w.id, -1);
        if (row < 0) continue;
        Entry& e = m_entries[static_cast<size_t>(row)];
        e.value = w.last;
        e.windowHigh = w.max;
        e.windowLow = w.min;
        e.arrivedMs = arrived;
        pushWindow(e, w);
        widenRows(row, firstRow, lastRow);
    }
    if (firstRow >= 0) {
        // AgeMsRole rides along with ValueRole/HasValueRole so delegates
        // bind opacity to ageMs; a QML-side clock stamped on value
        // changes would dim steady values that are still arriving.
        emit dataChanged(index(firstRow), index(lastRow),
                         {ValueRole, WindowHighRole, WindowLowRole,
                          HasValueRole, AgeMsRole, LabelRole});
        // One emission per batch: the strip chart's sole repaint trigger.
        emit valuesUpdated();
    }
}

void SignalListModel::updateTexts(const std::vector<std::pair<uint16_t, QString>>& texts,
                                  qint64 timestampMs)
{
    if (m_entries.empty() || texts.empty()) return;
    const qint64 stamp = (timestampMs >= 0) ? timestampMs : m_clock.elapsed();
    int firstRow = -1;
    int lastRow = -1;
    for (const auto& pair : texts) {
        const int row = m_rowOfId.value(pair.first, -1);
        if (row < 0) continue;
        Entry& e = m_entries[static_cast<size_t>(row)];
        if (!e.isText) continue;
        e.text = pair.second;
        e.arrivedMs = m_clock.elapsed();
        // The ring keeps the sample's time, for hasValue; the value is
        // never plotted
        pushSample(e, stamp, 0.0);
        widenRows(row, firstRow, lastRow);
    }
    if (firstRow >= 0) {
        emit dataChanged(index(firstRow), index(lastRow),
                         {TextRole, HasValueRole, AgeMsRole});
    }
}

std::vector<ecuio_health_value_t> SignalListModel::heldValues() const
{
    std::vector<ecuio_health_value_t> held;
    const qint64 now = m_clock.elapsed();
    for (const Entry& e : m_entries) {
        if (e.isText || e.arrivedMs < 0 || e.histCount == 0) {
            continue;
        }
        ecuio_health_value_t v{};
        v.def_id = e.id;
        v.value = e.value;
        v.age_us =
            static_cast<uint64_t>((std::max)(qint64{0}, now - e.arrivedMs)) *
            1000u;
        held.push_back(v);
    }
    return held;
}

qint64 SignalListModel::readAtMs(uint32_t id) const
{
    if (id > UINT16_MAX) {
        return -1;
    }
    const int row = m_rowOfId.value(static_cast<uint16_t>(id), -1);
    if (row < 0) {
        return -1;
    }
    return m_entries[static_cast<size_t>(row)].arrivedMs;
}

qint64 SignalListModel::wallOffsetMs() const
{
    return QDateTime::currentMSecsSinceEpoch() - m_clock.elapsed();
}

qint64 SignalListModel::lastReadMs(uint32_t id) const
{
    const qint64 at = readAtMs(id);
    return at < 0 ? -1 : at + wallOffsetMs();
}

QString SignalListModel::labelOf(const Entry& e)
{
    // An enum's value is its raw number: a whole number from 0 up
    if (e.labels.empty() || e.histCount == 0 || !(e.value >= 0.0) ||
        e.value != std::floor(e.value) || e.value > 4294967295.0) {
        return QString();
    }
    const auto raw = static_cast<uint32_t>(e.value);
    for (const auto& l : e.labels) {
        if (l.first == raw) {
            return l.second;
        }
    }
    return QString();
}

ecuio_display_value_t SignalListModel::shown(const Entry& e, double value) const
{
    return displayed(e.unit, e.decimals, value, m_units);
}

std::pair<double, double> SignalListModel::shownRange(const Entry& e) const
{
    return displayedRange(e.unit, e.decimals, e.facts.rangeLow,
                          e.facts.rangeHigh, m_units);
}

void SignalListModel::setUnitSystem(uint32_t units)
{
    if (units == m_units) return;
    m_units = units;
    if (m_entries.empty()) return;
    emit dataChanged(index(0), index(static_cast<int>(m_entries.size()) - 1),
                     {ValueRole, WindowHighRole, WindowLowRole, UnitRole,
                      DecimalsRole, RangeLowRole, RangeHighRole});
    emit valuesUpdated();   // the chart redraws its history in the new units
}

// Evict-oldest ring push: bounded memory, and the stream never freezes.
void SignalListModel::pushSample(Entry& e, qint64 stamp, double value)
{
    e.history[e.histHead].timestampMs = stamp;
    e.history[e.histHead].value = value;
    e.histHead = (e.histHead + 1) % HistoryCapacity;
    if (e.histCount < HistoryCapacity) {
        ++e.histCount;
    }
}

// A window's lowest, highest and newest value, in time order, each once:
// a point repeating the one before it (same time and value) is dropped
void SignalListModel::pushWindow(Entry& e, const WindowValue& w)
{
    Sample points[3] = {{w.minMs, w.min}, {w.maxMs, w.max}, {w.lastMs, w.last}};
    std::stable_sort(std::begin(points), std::end(points),
                     [](const Sample& a, const Sample& b) {
                         return a.timestampMs < b.timestampMs;
                     });
    for (int i = 0; i < 3; ++i) {
        bool repeat = false;
        for (int j = 0; j < i; ++j) {
            repeat = repeat || (points[j].timestampMs == points[i].timestampMs &&
                                points[j].value == points[i].value);
        }
        if (!repeat) {
            pushSample(e, points[i].timestampMs, points[i].value);
        }
    }
}

QVariantList SignalListModel::chartData(qint64 windowMs, int columns) const
{
    const auto plotted = [](const Entry& e) {
        return e.subscribed && !e.isText && !e.hidden && e.histCount != 0;
    };
    // The window ends at the newest plotted sample, the chart's "now"
    qint64 newest = std::numeric_limits<qint64>::min();
    for (const Entry& e : m_entries) {
        if (plotted(e)) {
            const uint32_t last = (e.histHead + HistoryCapacity - 1) % HistoryCapacity;
            newest = std::max(newest, e.history[last].timestampMs);
        }
    }
    const qint64 from = windowMs > 0 ? newest - windowMs
                                     : std::numeric_limits<qint64>::min();
    const double sliceMs = (windowMs > 0 && columns > 0)
                               ? static_cast<double>(windowMs) / columns : 0.0;

    QVariantList out;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (!plotted(e)) continue;

        const ecuio_display_value_t now = shown(e, e.value);
        QVariantMap m;
        m["id"] = e.id;
        m["row"] = static_cast<int>(row);
        m["name"] = QString::fromUtf8(e.name);
        m["unit"] = QString::fromUtf8(now.unit);
        m["decimals"] = static_cast<int>(now.decimal_places);
        m["value"] = now.value;

        // Flattened [ts, val, ts, val, ...], oldest first
        QVariantList samples;
        for (const Sample* s : sliced(e, from, sliceMs)) {
            samples.append(static_cast<double>(s->timestampMs));
            samples.append(now.converted ? shown(e, s->value).value : s->value);
        }
        if (samples.isEmpty()) {
            continue;
        }
        m["samples"] = samples;
        out.append(m);
    }
    return out;
}

std::vector<const SignalListModel::Sample*>
SignalListModel::sliced(const Entry& e, qint64 from, double sliceMs,
                        qint64 breakMs)
{
    std::vector<const Sample*> out;
    // A slice's lowest and highest, in time order, once each
    const Sample* low = nullptr;
    const Sample* high = nullptr;
    const auto flush = [&]() {
        if (low != nullptr) {
            const Sample* a =
                low->timestampMs <= high->timestampMs ? low : high;
            const Sample* b = a == low ? high : low;
            out.push_back(a);
            if (b != a) {
                out.push_back(b);
            }
        }
        low = high = nullptr;
    };
    // When full, the oldest surviving sample sits at histHead (the next
    // eviction slot); while filling, samples are dense from 0
    const uint32_t start = (e.histCount == HistoryCapacity) ? e.histHead : 0;
    qint64 slice = -1;
    const Sample* last = nullptr;
    bool lastOut = false;
    for (uint32_t i = 0; i < e.histCount; ++i) {
        const Sample& s = e.history[(start + i) % HistoryCapacity];
        if (s.timestampMs < from) {
            continue;
        }
        if (breakMs > 0) {
            // A stall in the ring is a break; a value that isn't a number is
            // its own point, so it hides no slice's extremes
            const bool stalled =
                last != nullptr && s.timestampMs - last->timestampMs > breakMs;
            const bool number = std::isfinite(s.value);
            if (stalled || !number) {
                flush();
                slice = -1;
            }
            if (stalled) {
                out.push_back(nullptr);
            }
            last = &s;
            lastOut = !number;
            if (!number) {
                out.push_back(&s);
                continue;
            }
        }
        last = &s;
        if (sliceMs <= 0.0) {
            out.push_back(&s);
            continue;
        }
        // Slices counted from time 0, not from the window's start: slices
        // measured from a sliding window would move a noisy trace's peaks
        const qint64 k = static_cast<qint64>(
            std::floor(static_cast<double>(s.timestampMs) / sliceMs));
        if (k != slice) {
            flush();
            slice = k;
        }
        if (low == nullptr || s.value < low->value) {
            low = &s;
        }
        if (high == nullptr || s.value > high->value) {
            high = &s;
        }
    }
    if (sliceMs > 0.0) {
        // The newest sample always: the trace ends where the value is now
        if (last != nullptr && !lastOut && last != low && last != high) {
            flush();
            out.push_back(last);
        } else {
            flush();
        }
    }
    return out;
}

QList<double> SignalListModel::trace(int id, qint64 windowMs, int columns) const
{
    QList<double> out;
    if (id < 0 || id > 0xFFFF) {
        return out;
    }
    const int row = m_rowOfId.value(static_cast<uint16_t>(id), -1);
    if (row < 0) {
        return out;
    }
    const Entry& e = m_entries[static_cast<size_t>(row)];
    if (e.isText || e.histCount == 0) {
        return out;
    }
    const uint32_t newest =
        (e.histHead + HistoryCapacity - 1) % HistoryCapacity;
    const qint64 from = windowMs > 0 ? e.history[newest].timestampMs - windowMs
                                     : std::numeric_limits<qint64>::min();
    const double sliceMs = (windowMs > 0 && columns > 0)
                               ? static_cast<double>(windowMs) / columns
                               : 0.0;
    const bool converted = shown(e, e.value).converted;
    const std::vector<const Sample*> picked =
        sliced(e, from, sliceMs, SessionBridge::valueStaleMs());
    out.reserve(static_cast<qsizetype>(picked.size() * 2));
    double lastTs = 0.0;
    for (const Sample* s : picked) {
        // A break: a point that isn't a number, at the stall's start
        if (s == nullptr) {
            out.append(lastTs);
            out.append(std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        lastTs = static_cast<double>(s->timestampMs);
        out.append(lastTs);
        out.append(converted ? shown(e, s->value).value : s->value);
    }
    return out;
}

void SignalListModel::applyUnsupported(const std::vector<uint16_t>& ids)
{
    for (Entry& e : m_entries) {
        e.unsupported = false;
    }
    for (uint16_t id : ids) {
        const int row = m_rowOfId.value(id, -1);
        if (row >= 0) {
            m_entries[static_cast<size_t>(row)].unsupported = true;
        }
    }
    rejudgeHidden();
}

void SignalListModel::applyAbsent(const std::vector<uint16_t>& ids)
{
    for (Entry& e : m_entries) {
        e.absent = false;
    }
    for (uint16_t id : ids) {
        const int row = m_rowOfId.value(id, -1);
        if (row >= 0) {
            m_entries[static_cast<size_t>(row)].absent = true;
        }
    }
    rejudgeHidden();
}

void SignalListModel::applyUnasked(const std::vector<uint16_t>& ids)
{
    std::vector<uint8_t> unasked(m_entries.size(), 0);
    for (uint16_t id : ids) {
        const int row = m_rowOfId.value(id, -1);
        if (row >= 0) {
            unasked[static_cast<size_t>(row)] = 1;
        }
    }
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        if (e.unasked != (unasked[row] != 0)) {
            e.unasked = unasked[row] != 0;
            const QModelIndex i = index(static_cast<int>(row));
            emit dataChanged(i, i, {UnaskedRole, UnaskedWhyRole});
        }
    }
    recountUnasked();
    rejudgeNoRoom();
}

void SignalListModel::applyNoRoom(const std::vector<int>& modules)
{
    m_noRoomModules = modules;
    rejudgeNoRoom();
}

void SignalListModel::rejudgeNoRoom()
{
    int count = 0;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        const bool noRoom =
            !e.unasked && e.module >= 0 &&
            std::find(m_noRoomModules.begin(), m_noRoomModules.end(),
                      e.module) != m_noRoomModules.end();
        if (e.noRoom != noRoom) {
            e.noRoom = noRoom;
            const QModelIndex i = index(static_cast<int>(row));
            emit dataChanged(i, i, {UnaskedWhyRole});
        }
        count += (e.noRoom && !e.hidden) ? 1 : 0;
    }
    if (count != m_noRoomCount) {
        m_noRoomCount = count;
        emit noRoomCountChanged();
    }
}

QStringList SignalListModel::noRoomNames() const
{
    QStringList names;
    for (const Entry& e : m_entries) {
        if (e.noRoom && !e.hidden) {
            names << QStringLiteral("%1's %2").arg(e.moduleName,
                                                   QString::fromUtf8(e.name));
        }
    }
    return names;
}

void SignalListModel::recountUnasked()
{
    int count = 0;
    for (const Entry& e : m_entries) {
        count += (e.unasked && !e.hidden) ? 1 : 0;
    }
    if (count != m_unaskedCount) {
        m_unaskedCount = count;
        emit unaskedCountChanged();
    }
}

void SignalListModel::applyRefused(const std::vector<Refusal>& refused)
{
    std::vector<Refusal> byRow(m_entries.size());
    for (const Refusal& r : refused) {
        const int row = m_rowOfId.value(r.id, -1);
        if (row >= 0) {
            byRow[static_cast<size_t>(row)] = r;
        }
    }
    bool changed = false;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        if (e.refused != byRow[row].words || e.refusedCode != byRow[row].code) {
            e.refused = byRow[row].words;
            e.refusedCode = byRow[row].code;
            const QModelIndex i = index(static_cast<int>(row));
            emit dataChanged(i, i, {RefusedRole, RefusedCodeRole});
            changed = true;
        }
    }
    // A refused row gives up its place in the default plot
    if (changed) {
        judgePlotted();
    }
}

void SignalListModel::rejudgeHidden()
{
    int first = -1;
    int last = -1;
    int hidden = 0;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        const bool hide = e.unsupported || e.absent;
        const Entry::Why why = e.unsupported ? Entry::Why::Unsupported
                               : e.absent ? Entry::Why::Absent : Entry::Why::None;
        if (hide != e.hidden || why != e.why) {
            e.hidden = hide;
            e.why = why;
            if (first < 0) first = static_cast<int>(row);
            last = static_cast<int>(row);
        }
        hidden += e.hidden ? 1 : 0;
    }
    if (first < 0) {
        return;
    }
    // Live's filtered view re-judges the rows HiddenRole changed in
    emit dataChanged(index(first), index(last), {HiddenRole, HiddenWhyRole});
    m_hiddenCount = hidden;
    emit hiddenCountChanged();
    recountUnasked();
    rejudgeNoRoom();
    // A hidden row's default place goes to the next ranked row
    judgePlotted();
}

// ---------------- LiveSignalModel ----------------
LiveSignalModel::LiveSignalModel(SignalListModel* source, QObject* parent)
    : QSortFilterProxyModel(parent), m_filter(new SignalFilter(this))
{
    // A HiddenRole change re-filters its rows (dynamic filtering, the default)
    setFilterRole(SignalListModel::HiddenRole);
    setSourceModel(source);
    // Dynamic filtering re-judges a row on any change to it, so a tick
    // under Plotted only, or a unit changed under typed text, re-filters
    // it; the filter's own changes re-judge every row
    connect(m_filter, &SignalFilter::changed, this, &LiveSignalModel::refilter);
    connect(source, &QAbstractItemModel::modelReset, this,
            &LiveSignalModel::judgeGroups);
    connect(source, &SignalListModel::hiddenCountChanged, this,
            &LiveSignalModel::judgeGroups);
    judgeGroups();
}

bool LiveSignalModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QAbstractItemModel* m = sourceModel();
    const QModelIndex i = m->index(sourceRow, 0, sourceParent);
    if (m->data(i, SignalListModel::HiddenRole).toBool()) {
        return false;
    }
    return m_filter->matches(
        m->data(i, SignalListModel::NameRole).toString(),
        m->data(i, SignalListModel::UnitRole).toString(),
        m->data(i, SignalListModel::GroupRole).toInt(),
        m->data(i, SignalListModel::SubscribedRole).toBool() &&
            !m->data(i, SignalListModel::IsTextRole).toBool(),
        !m->data(i, SignalListModel::UnaskedRole).toBool(),
        m->data(i, SignalListModel::ModuleRole).toInt());
}

void LiveSignalModel::refilter()
{
    // The filter changed already: begin and end at once re-judge every row
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void LiveSignalModel::judgeGroups()
{
    const QAbstractItemModel* m = sourceModel();
    QVariantList codes;
    QVariantList rows;
    int shown = 0;
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r, 0);
        if (!m->data(i, SignalListModel::HiddenRole).toBool()) {
            codes.append(m->data(i, SignalListModel::GroupRole));
            rows.append(
                QVariantMap{{QStringLiteral("module"),
                             m->data(i, SignalListModel::ModuleRole)},
                            {QStringLiteral("moduleName"),
                             m->data(i, SignalListModel::ModuleNameRole)}});
            ++shown;
        }
    }
    const QVariantList modules = m_filter->moduleChoices(rows);
    m_filter->keepModules(modules);
    QVariantList groups;
    for (const QVariant& choice : m_filter->groupChoices(codes)) {
        groups.append(choice.toMap().value(QStringLiteral("code")));
    }
    if (groups != m_groups || modules != m_modules || shown != m_shownCount) {
        m_groups = groups;
        m_modules = modules;
        m_shownCount = shown;
        emit groupsChanged();
    }
}
