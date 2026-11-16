// File/layer: app/framelistmodel.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "framelistmodel.h"
#include "ecuio/recording.h"

#include <QStringList>

#include <algorithm>
#include <limits>
#include <cstring>
#include <map>
#include <utility>

// ---------------- FrameListModel ----------------
FrameListModel::FrameListModel(QObject* parent)
    : QAbstractListModel(parent)
{
    m_ring.reserve(MaxFrames);
}

int FrameListModel::count() const
{
    return m_filter.active() ? static_cast<int>(m_shown.size()) : m_count;
}

int FrameListModel::rowCount(const QModelIndex&) const
{
    return count();
}

// A view row's frame: row 0 is the newest, or the oldest
const FrameListModel::Row& FrameListModel::rowAt(int row) const
{
    const int n = count();
    const size_t age = m_newestFirst ? static_cast<size_t>(n - 1 - row)
                                     : static_cast<size_t>(row);   // 0 = oldest
    if (m_filter.active()) {
        return rowOfArrival(m_shown[age]);
    }
    return m_ring[(m_oldest + age) % m_ring.size()];
}

QString FrameListModel::flagWords(uint32_t flags)
{
    QStringList words;
    if ((flags & ECUIO_FRAME_FLAG_FD) != 0) {
        words << ((flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) != 0
                      ? QStringLiteral("FD BRS")
                      : QStringLiteral("FD"));
    }
    if ((flags & ECUIO_FRAME_FLAG_REMOTE) != 0) {
        words << QStringLiteral("remote");
    }
    if ((flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0) {
        words << QStringLiteral("reassembled");
    }
    if ((flags & ECUIO_FRAME_FLAG_TRUNCATED) != 0) {
        words << QStringLiteral("truncated");
    }
    if ((flags & ECUIO_FRAME_FLAG_OVERFLOW) != 0) {
        words << QStringLiteral("after a loss");
    }
    if ((flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
        words << QStringLiteral("error");
    }
    if ((flags & ECUIO_FRAME_FLAG_TX_ECHO) != 0) {
        words << QStringLiteral("sent");
    }
    return words.join(QLatin1Char(' '));
}

QVariant FrameListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return QVariant();
    }
    const Row& r = rowAt(index.row());
    switch (role) {
        case TimestampRole:     return QVariant::fromValue<quint64>(r.timestampUs);
        case DlcRole:           return static_cast<uint>(r.length);
        case ExtendedRole:      return r.extended;
        case PayloadRole:
            // Spaced uppercase bytes ("41 0C 1F 40"), matching the ID column
            return QString::fromLatin1(
                QByteArray::fromRawData(reinterpret_cast<const char*>(r.data()),
                                        static_cast<qsizetype>(r.length))
                    .toHex(' ').toUpper());
        case ArbitrationIdRole: return QVariant::fromValue<quint32>(r.arbId);
        case SelectedRole:      return m_selected.count(r.seq) != 0;
        case FlagsRole:         return QVariant::fromValue<quint32>(r.flags);
        case FlagsTextRole:     return flagWords(r.flags);
        default:                return QVariant();
    }
}

QHash<int, QByteArray> FrameListModel::roleNames() const
{
    return {
        {TimestampRole,     "timestampUs"},
        {DlcRole,           "dlc"},
        {ExtendedRole,      "extended"},
        {PayloadRole,       "payloadHex"},
        {ArbitrationIdRole, "arbId"},
        {SelectedRole,      "selected"},
        {FlagsRole,         "flags"},
        {FlagsTextRole,     "flagsText"}
    };
}

void FrameListModel::appendFrame(const ecuio_frame_t& frame, uint16_t len)
{
    const bool filtered = m_filter.active();
    const int before = count();
    const bool full = (m_count == MaxFrames);
    if (full) {
        // Full: the oldest row leaves first, from the end that shows it,
        // and its slot takes the new frame. Filtered, it leaves the view
        // only when the view shows it
        const quint64 leaving = m_ring[m_oldest].seq;
        const bool shown = !filtered || (!m_shown.empty() && m_shown.front() == leaving);
        if (shown) {
            const int n = count();
            const int row = m_newestFirst ? n - 1 : 0;
            beginRemoveRows(QModelIndex(), row, row);
        }
        m_oldest = (m_oldest + 1) % m_ring.size();
        --m_count;
        if (filtered && shown) {
            m_shown.pop_front();
        }
        if (shown) {
            endRemoveRows();
        }
        if (!m_selected.empty() && *m_selected.begin() == leaving) {
            m_selected.erase(m_selected.begin());
            emit selectionChanged();
        }
    }
    if (static_cast<size_t>(m_count) == m_ring.size()) {
        m_ring.emplace_back();   // every slot holds a row: one more, below the cap
    }
    Row& r = m_ring[(m_oldest + static_cast<size_t>(m_count)) % m_ring.size()];
    r.seq = m_nextSeq++;
    r.timestampUs = frame.timestamp_us;
    r.arbId = frame.arbitration_id;
    r.flags = frame.flags;
    r.extended = (frame.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
    r.length = len;
    if (len <= InlinePayload) {
        memcpy(r.inlineData, frame.data, len);
        std::vector<uint8_t>().swap(r.spill);   // frees a long frame's block
    } else {
        r.spill.assign(frame.data, frame.data + len);
    }
    const bool shown = !filtered || shows(r);
    if (shown) {
        const int row = m_newestFirst ? 0 : count();
        beginInsertRows(QModelIndex(), row, row);
        ++m_count;
        if (filtered) {
            m_shown.push_back(r.seq);
        }
        endInsertRows();
    } else {
        ++m_count;
    }
    if (count() != before || !full) {
        emit countChanged(count());   // at the cap one row left as one came
    }
}

void FrameListModel::clear()
{
    if (m_count == 0) return;
    beginResetModel();
    for (Row& r : m_ring) {
        std::vector<uint8_t>().swap(r.spill);   // the slots stay; their blocks go
    }
    m_oldest = 0;
    m_count = 0;
    m_shown.clear();
    const bool hadSelection = !m_selected.empty();
    m_selected.clear();
    m_hasAnchor = false;
    endResetModel();
    emit countChanged(0);
    if (hadSelection) {
        emit selectionChanged();
    }
}

QVariantMap FrameListModel::setFilter(const QString& ids, const QString& data,
                                      int direction, int flag,
                                      const QString& lengths,
                                      const QString& times)
{
    FrameFilter next;
    const QString idWords = FrameFilter::parseIds(ids, &next.ids);
    const QString dataWords = FrameFilter::parseData(data, &next.data, &next.anchored);
    const QString lengthWords =
        FrameFilter::parseLengths(lengths, &next.lengths);
    const QString timeWords = FrameFilter::parseTimes(times, &next.time);
    next.direction = direction >= FrameFilter::AnyDirection &&
                             direction <= FrameFilter::Sent
                         ? direction
                         : FrameFilter::AnyDirection;
    next.flag = flag >= FrameFilter::AnyFlag && flag <= FrameFilter::Error
                    ? flag
                    : FrameFilter::AnyFlag;
    // A field that doesn't parse keeps its part of the filter as it was
    if (!idWords.isEmpty()) {
        next.ids = m_filter.ids;
    }
    if (!dataWords.isEmpty()) {
        next.data = m_filter.data;
        next.anchored = m_filter.anchored;
    }
    if (!lengthWords.isEmpty()) {
        next.lengths = m_filter.lengths;
    }
    if (!timeWords.isEmpty()) {
        next.time = m_filter.time;
    }
    const bool same =
        next.ids == m_filter.ids && next.data == m_filter.data &&
        next.anchored == m_filter.anchored &&
        next.direction == m_filter.direction && next.flag == m_filter.flag &&
        next.lengths == m_filter.lengths && next.time == m_filter.time;
    if (!same) {
        beginResetModel();
        m_filter = std::move(next);
        m_shown.clear();
        if (m_filter.active()) {
            for (int age = 0; age < m_count; ++age) {
                const Row& r = m_ring[(m_oldest + static_cast<size_t>(age)) % m_ring.size()];
                if (shows(r)) {
                    m_shown.push_back(r.seq);
                }
            }
        }
        const bool hadSelection = !m_selected.empty();
        m_selected.clear();
        m_hasAnchor = false;
        endResetModel();
        emit filterChanged();
        emit countChanged(count());
        if (hadSelection) {
            emit selectionChanged();
        }
    }
    return {{QStringLiteral("ids"), idWords},
            {QStringLiteral("data"), dataWords},
            {QStringLiteral("lengths"), lengthWords},
            {QStringLiteral("times"), timeWords}};
}

quint64 FrameListModel::newestTimestampUs() const
{
    if (m_count == 0) {
        return 0;
    }
    return m_ring[(m_oldest + static_cast<size_t>(m_count - 1)) % m_ring.size()].timestampUs;
}

size_t FrameListModel::spilledBytesForTesting() const
{
    size_t bytes = 0;
    for (const Row& r : m_ring) {
        bytes += r.spill.capacity();
    }
    return bytes;
}

void FrameListModel::setNewestFirst(bool newestFirst)
{
    if (m_newestFirst == newestFirst) return;
    // Every row moves, so the views start over
    beginResetModel();
    m_newestFirst = newestFirst;
    endResetModel();
    emit newestFirstChanged();
}

// The ring holds the newest m_count arrivals, numbered consecutively;
// filtered, the shown ones are m_shown, in order
int FrameListModel::rowOfSeq(quint64 seq) const
{
    const quint64 oldestSeq = m_nextSeq - static_cast<quint64>(m_count);
    if (m_count == 0 || seq < oldestSeq || seq >= m_nextSeq) {
        return -1;
    }
    int age = 0;   // 0 = oldest
    if (m_filter.active()) {
        const auto it = std::lower_bound(m_shown.begin(), m_shown.end(), seq);
        if (it == m_shown.end() || *it != seq) {
            return -1;
        }
        age = static_cast<int>(it - m_shown.begin());
    } else {
        age = static_cast<int>(seq - oldestSeq);
    }
    return m_newestFirst ? count() - 1 - age : age;
}

double FrameListModel::seqAt(int row) const
{
    if (row < 0 || row >= count()) {
        return -1;
    }
    return static_cast<double>(rowAt(row).seq);
}

int FrameListModel::rowAtSeq(double seq) const
{
    const int n = count();
    if (n == 0) {
        return -1;
    }
    const quint64 oldestSeq = m_nextSeq - static_cast<quint64>(m_count);
    const quint64 want = seq > 0 ? static_cast<quint64>(seq) : 0;
    // The oldest shown row at or after the arrival, by age (0 = oldest);
    // n when every shown row arrived before it
    int age = 0;
    bool exact = false;
    if (m_filter.active()) {
        const auto it = std::lower_bound(m_shown.begin(), m_shown.end(), want);
        age = static_cast<int>(it - m_shown.begin());
        exact = it != m_shown.end() && *it == want;
    } else if (want >= oldestSeq) {
        age = static_cast<int>(
            std::min<quint64>(want - oldestSeq, static_cast<quint64>(n)));
        exact = age < n;
    }
    if (!m_newestFirst) {
        return std::min(age, n - 1);
    }
    // Newest first the next row is the one that arrived before it
    const int next = exact ? age : age - 1;
    return n - 1 - std::max(next, 0);
}

void FrameListModel::notifySelected(int first, int last)
{
    emit dataChanged(index(first, 0), index(last, 0), {SelectedRole});
    emit selectionChanged();
}

void FrameListModel::toggleSelected(int row)
{
    if (row < 0 || row >= count()) return;
    const quint64 seq = rowAt(row).seq;
    if (m_selected.erase(seq) == 0) {
        m_selected.insert(seq);
    }
    m_anchorSeq = seq;
    m_hasAnchor = true;
    notifySelected(row, row);
}

void FrameListModel::selectThrough(int row)
{
    if (row < 0 || row >= count()) return;
    const int anchor = m_hasAnchor ? rowOfSeq(m_anchorSeq) : -1;
    if (anchor < 0) {
        toggleSelected(row);
        return;
    }
    const int first = std::min(anchor, row);
    const int last = std::max(anchor, row);
    for (int r = first; r <= last; ++r) {
        m_selected.insert(rowAt(r).seq);
    }
    notifySelected(first, last);
}

void FrameListModel::clearSelection()
{
    m_hasAnchor = false;
    if (m_selected.empty()) return;
    m_selected.clear();
    if (count() > 0) {
        notifySelected(0, count() - 1);
    } else {
        emit selectionChanged();
    }
}

QString FrameListModel::selectedText() const
{
    if (m_selected.empty()) return {};
    QString text = QStringLiteral("time_s\tdlc\text\tid\tdata\n");
    for (int row = 0; row < count(); ++row) {
        const Row& r = rowAt(row);
        if (m_selected.count(r.seq) == 0) continue;
        text += QString::number(static_cast<double>(r.timestampUs) / 1e6, 'f', 6)
              + QLatin1Char('\t') + QString::number(r.length)
              + QLatin1Char('\t') + (r.extended ? QLatin1Char('E') : QLatin1Char('-'))
              + QStringLiteral("\t0x") + QString::number(r.arbId, 16).toUpper()
              + QLatin1Char('\t')
              + QString::fromLatin1(
                    QByteArray::fromRawData(reinterpret_cast<const char*>(r.data()),
                                            static_cast<qsizetype>(r.length))
                        .toHex(' ').toUpper())
              + QLatin1Char('\n');
    }
    return text;
}

void FrameListModel::noteTextDefs(const ecuio_signal_set_t* set)
{
    if (set == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
            (d.kind != ECUIO_VALUE_TEXT && d.kind != ECUIO_VALUE_HEX)) {
            continue;
        }
        // What the scrub reads of a def: the frames it applies to
        const bool held =
            std::any_of(m_textDefs.begin(), m_textDefs.end(),
                        [&](const ecuio_signal_def_t& k) {
                            return k.message_id == d.message_id &&
                                   k.mux_offset == d.mux_offset &&
                                   k.mux_length == d.mux_length &&
                                   k.mux_key == d.mux_key;
                        });
        if (!held) {
            m_textDefs.push_back(d);
        }
    }
}

FrameListModel::ScrubbedCsv FrameListModel::scrubbedCsv() const
{
    ScrubbedCsv out;
    // The rows as they are now: both of core's passes read this copy
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(m_count));
    for (int age = 0; age < m_count; ++age) {
        rows.push_back(
            m_ring[(m_oldest + static_cast<size_t>(age)) % m_ring.size()]);
    }
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(m_textDefs.size());
    set.defs = m_textDefs.empty() ? nullptr : m_textDefs.data();
    std::vector<uint8_t> keep(rows.size());
    ecuio_export_report_t report{};
    const ecuio_scrub_frame_callback_t get = [](void* user, size_t index,
                                                ecuio_frame_t* f) -> int {
        const Row& r = (*static_cast<const std::vector<Row>*>(user))[index];
        f->timestamp_us = r.timestampUs;
        f->arbitration_id = r.arbId;
        f->flags = r.flags;
        f->data_length = r.length;
        memcpy(f->data, r.data(), r.length);
        return 0;
    };
    const ecuio_status_t rc = ecuio_scrub_frames(rows.size(), get, &rows, &set,
                                                 0, keep.data(), &report);
    if (rc != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        out.words = detail.isEmpty()
                        ? QStringLiteral("The frames couldn't be checked")
                        : detail.left(1).toUpper() + detail.mid(1);
        return out;
    }
    out.identification = report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION];
    out.textValues = report.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES];
    out.unattributed = report.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE];
    QByteArray body;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (keep[i] == 0 || (m_filter.active() && !shows(r))) {
            continue;
        }
        ++out.written;
        body += QByteArray::number(static_cast<double>(r.timestampUs) / 1e6,
                                   'f', 6);
        body += ",0x" + QByteArray::number(r.arbId, 16).toUpper();
        body += r.extended ? ",1" : ",0";
        body += (r.flags & ECUIO_FRAME_FLAG_TX_ECHO) != 0 ? ",tx," : ",rx,";
        body += QByteArray::number(r.length);
        body += ',' + flagWords(r.flags).toLatin1() + ',';
        body += QByteArray::fromRawData(reinterpret_cast<const char*>(r.data()),
                                        static_cast<qsizetype>(r.length))
                    .toHex(' ')
                    .toUpper();
        body += '\n';
    }
    out.bytes =
        QStringLiteral(
            "# Havne %1, the Log page's frames, scrubbed (ADR-0054): %2 "
            "written%3; left out of the %4 held: %5 frames of "
            "identification replies, %6 of text signals' messages, %7 "
            "segments of messages that couldn't be reassembled\n")
            .arg(QString::fromLatin1(ecuio_version_string()))
            .arg(out.written)
            .arg(m_filter.active() ? QStringLiteral(" (those the filter shows)")
                                   : QString())
            .arg(rows.size())
            .arg(out.identification)
            .arg(out.textValues)
            .arg(out.unattributed)
            .toUtf8() +
        "time_s,id,ext,dir,dlc,flags,data\n" + body;
    out.ok = true;
    return out;
}

// ---------------- FrameIdModel ----------------
FrameIdModel::FrameIdModel(const FrameListModel* frames, QObject* parent)
    : QAbstractListModel(parent)
    , m_frames(frames)
{
}

int FrameIdModel::rowCount(const QModelIndex&) const
{
    return count();
}

QVariant FrameIdModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return QVariant();
    }
    const IdRow& r = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
        case ArbIdRole:    return QVariant::fromValue<quint32>(static_cast<quint32>(r.key));
        case ExtendedRole: return (r.key >> 32) != 0;
        case CountRole:    return r.count;
        case RateRole:     return r.rate;
        case PayloadRole:  return QString::fromLatin1(r.payload.toHex(' ').toUpper());
        case ChangedRole:  return r.changed;
        default:           return QVariant();
    }
}

QHash<int, QByteArray> FrameIdModel::roleNames() const
{
    return {
        {ArbIdRole,    "arbId"},
        {ExtendedRole, "extended"},
        {CountRole,    "frames"},
        {RateRole,     "rate"},
        {PayloadRole,  "payloadHex"},
        {ChangedRole,  "changedBytes"}
    };
}

void FrameIdModel::refresh()
{
    // The window ends at the newest frame held and is half-open,
    // (newest - window, newest], so N frames a second count N. A log
    // shorter than the window rates each ID over its own first to last
    // frame: N frames there are N - 1 intervals (a lone frame, none)
    const quint64 newest = m_frames->newestTimestampUs();
    const quint64 from = newest > RateWindowUs ? newest - RateWindowUs : 0;
    quint64 oldest = newest;
    struct Seen {
        quint64 first = std::numeric_limits<quint64>::max();
        quint64 last = 0;
        int recent = 0;
        QByteArray before; // the payload of the frame before the newest
    };
    std::map<quint64, IdRow> byKey;
    std::map<quint64, Seen> seen;
    m_frames->forEachShown([&](const FrameListModel::Shown& f) {
        const quint64 key =
            ((f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0 ? (quint64{1} << 32) : 0) | f.id;
        IdRow& row = byKey[key];
        row.key = key;
        Seen& s = seen[key];
        s.before = std::move(row.payload);
        ++row.count;
        row.payload = QByteArray(reinterpret_cast<const char*>(f.data), f.length);
        oldest = std::min(oldest, f.timestampUs);
        s.first = std::min(s.first, f.timestampUs);
        s.last = std::max(s.last, f.timestampUs);
        if (f.timestampUs > from) {
            ++s.recent;
        }
    });
    const bool shortLog = newest - std::min(oldest, newest) < RateWindowUs;
    std::vector<IdRow> next;
    next.reserve(byKey.size());
    for (auto& [key, row] : byKey) {
        const Seen& s = seen[key];
        if (row.count > 1) {
            const qsizetype common =
                std::min(s.before.size(), row.payload.size());
            for (qsizetype b = 0; b < row.payload.size(); ++b) {
                if (b >= common || s.before[b] != row.payload[b]) {
                    row.changed.append(static_cast<int>(b));
                }
            }
        }
        if (!shortLog) {
            row.rate = static_cast<double>(s.recent) * 1e6 /
                       static_cast<double>(RateWindowUs);
        } else if (row.count > 1 && s.last > s.first) {
            row.rate = static_cast<double>(row.count - 1) * 1e6 /
                       static_cast<double>(s.last - s.first);
        } else {
            row.rate = 0.0;
        }
        next.push_back(std::move(row));
    }
    // Both lists are sorted by key: walk them together, so a row that
    // stays keeps its place and only one that comes or goes moves others
    const int before = count();
    size_t i = 0;
    size_t j = 0;
    while (i < m_rows.size() || j < next.size()) {
        if (j == next.size() || (i < m_rows.size() && m_rows[i].key < next[j].key)) {
            beginRemoveRows(QModelIndex(), static_cast<int>(i), static_cast<int>(i));
            m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(i));
            endRemoveRows();
        } else if (i == m_rows.size() || next[j].key < m_rows[i].key) {
            beginInsertRows(QModelIndex(), static_cast<int>(i), static_cast<int>(i));
            m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(i), next[j]);
            endInsertRows();
            ++i;
            ++j;
        } else {
            IdRow& old = m_rows[i];
            if (old.count != next[j].count || old.rate != next[j].rate ||
                old.payload != next[j].payload ||
                old.changed != next[j].changed) {
                old = next[j];
                emit dataChanged(
                    index(static_cast<int>(i), 0),
                    index(static_cast<int>(i), 0),
                    {CountRole, RateRole, PayloadRole, ChangedRole});
            }
            ++i;
            ++j;
        }
    }
    if (count() != before) {
        emit countChanged();
    }
}
