// File/layer: app/sessionbridge.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); Manual smoke

#include "sessionbridge.h"
#include "carryover.h"
#include "healthreport.h"
#include "obdbimport.h"
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */
#include "recording_signals.h" /* cli/: a replay's own signal file */
#include "signal_file_lines.h" /* cli/: the profile a URI selects */
#include "refused_signals.h" /* cli/: the signals an ECU refuses, as havne-cli says them */
#include "scan_line.h"       /* cli/: when a module's session earns a line */
#include "urlwrite.h"
#include "ecuio/version.h"
#include "ecuio/signals.h"
#include "ecuio/j2534.h"
#include "ecuio/obdb.h"
#include "ecuio/profile.h"
#include "ecuio/serial.h"
#include "ecuio/signal_sets.h"
#include "ecuio/socketcan.h"
#include "ecuio/vin.h"

#include <cmath>
#include <QMetaObject>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <iterator>
#include <limits>
#include <chrono>
#include <cstring>

namespace {

// A file or folder for display: relative to Documents when under it,
// so the account name stays out of the log and the page
QString documentsRelative(const QString& path)
{
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.isEmpty()) {
        const QString rel = QDir(docs).relativeFilePath(path);
        if (QDir::isRelativePath(rel) && !rel.startsWith(QLatin1String(".."))) {
            return QDir::toNativeSeparators(QStringLiteral("Documents/") + rel);
        }
    }
    return QDir::toNativeSeparators(path);
}

// A failed create or start, in words: the status, then core's detail
// (which URI key, which vendor call, the vendor's text) when it has one.
// Call it on the thread that made the call: the detail is thread-local.
QString failureText(ecuio_status_t rc)
{
    QString status = QString::fromUtf8(ecuio_status_string(rc));
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || detail[0] == '\0') {
        return status;
    }
    return status + QStringLiteral(": ") + QString::fromUtf8(detail);
}

// The activity log's numbers: an ID or count as 0x7E8, a byte as 0C.
QString hexId(uint32_t v)
{
    return QStringLiteral("0x") + QString::number(v, 16).toUpper();
}

QString hexByte(uint8_t b)
{
    return QStringLiteral("%1").arg(static_cast<uint>(b), 2, 16,
                                    QLatin1Char('0')).toUpper();
}

// A clear's vehicle speed in the unit system, converted by core
// (ADR-0022): "48 km/h", "30 mph"; "" if core can't convert it.
QString clearSpeedText(uint32_t kmh, uint32_t units)
{
    ecuio_display_value_t out{};
    if (ecuio_display_convert("km/h", 0, kmh, units, &out) !=
        ECUIO_DISPLAY_OK) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(out.value))
        .arg(QString::fromUtf8(out.unit));
}

// Widens [first, last] to take row; first < 0 is an empty range.
void widenRows(int row, int& first, int& last)
{
    if (first < 0 || row < first) first = row;
    if (row > last) last = row;
}

} // namespace

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

// A value, its unit and its decimals in a unit system (ADR-0022)
static ecuio_display_value_t displayed(const char* unit, uint8_t decimals,
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
static std::pair<double, double> displayedRange(const char* unit,
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
            {IsEnumRole, "isEnum"}};
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
                                   const ecuio_signal_file_t* file)
{
    // NULL/empty is valid (the raw profile): clear the model rather
    // than keep the previous session's rows.
    const bool hadHidden = m_hiddenCount != 0;
    m_hiddenCount = 0;
    m_ranked = false;
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        beginResetModel();
        m_entries.clear();
        m_rowOfId.clear();
        m_subscribedCount = 0;
        endResetModel();
        emit subscribedCountChanged();
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

    for (uint32_t i = 0; i < set->count; ++i) {
        // value-init zeroes the history ring, head, and count
        m_rowOfId.insert(set->defs[i].id, static_cast<int>(m_entries.size()));
        Entry& e = m_entries.emplace_back();
        e.id = set->defs[i].id;
        qstrncpy(e.name, set->defs[i].name, sizeof(e.name));   // always terminates
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
    endResetModel();
    // A new set plots its defaults, whatever the last one plotted
    judgePlotted();
    emit subscribedCountChanged();
    if (hadHidden) emit hiddenCountChanged();

    m_clock.restart();
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
        return e.subscribed && !e.hidden && e.histCount != 0;
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

        // Flattened [ts, val, ts, val, ...], oldest first.
        // When full, the oldest surviving sample sits at histHead (the
        // next eviction slot); while filling, samples are dense from 0.
        QVariantList samples;
        const auto put = [&](const Sample& s) {
            samples.append(static_cast<double>(s.timestampMs));
            samples.append(now.converted ? shown(e, s.value).value : s.value);
        };
        // A slice's lowest and highest, in time order, once each
        const Sample* low = nullptr;
        const Sample* high = nullptr;
        const auto flush = [&]() {
            if (low != nullptr) {
                const Sample* a = low->timestampMs <= high->timestampMs ? low : high;
                const Sample* b = a == low ? high : low;
                put(*a);
                if (b != a) put(*b);
            }
            low = high = nullptr;
        };
        const uint32_t start = (e.histCount == HistoryCapacity)
                                ? e.histHead : 0;
        qint64 slice = -1;
        const Sample* last = nullptr;
        for (uint32_t i = 0; i < e.histCount; ++i) {
            const Sample& s = e.history[(start + i) % HistoryCapacity];
            if (s.timestampMs < from) continue;
            last = &s;
            if (sliceMs <= 0.0) {
                put(s);
                continue;
            }
            // Slices counted from time 0, not from the window's start:
            // the window slides with every update, and slices measured
            // from it would put the same samples in other slices each
            // time, so a noisy trace's peaks crawl as it scrolls
            const qint64 k = static_cast<qint64>(
                std::floor(static_cast<double>(s.timestampMs) / sliceMs));
            if (k != slice) {
                flush();
                slice = k;
            }
            if (low == nullptr || s.value < low->value) low = &s;
            if (high == nullptr || s.value > high->value) high = &s;
        }
        if (sliceMs > 0.0) {
            /* the newest sample always: the trace ends, and its label
             * sits, where the value is now */
            if (last != nullptr && last != low && last != high) {
                flush();
                put(*last);
            } else {
                flush();
            }
        }
        if (samples.isEmpty()) continue;
        m["samples"] = samples;
        out.append(m);
    }
    return out;
}

void SignalListModel::toggleSubscribed(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size())) return;
    Entry& e = m_entries[static_cast<size_t>(row)];
    if (e.isText) return;   // nothing to plot
    const bool plot = !e.subscribed;
    e.choice = plot == e.plotDefault ? PlotChoice::Default
             : plot                  ? PlotChoice::Plotted
                                     : PlotChoice::Unplotted;
    judgePlotted();
}

QStringList SignalListModel::plottedNames() const
{
    QStringList names;
    for (const Entry& e : m_entries) {
        if (!e.isText && e.choice == PlotChoice::Plotted) {
            names << QString::fromUtf8(e.name);
        }
    }
    return names;
}

std::vector<uint16_t> SignalListModel::plottedIds() const
{
    std::vector<uint16_t> ids;
    for (const Entry& e : m_entries) {
        if (e.subscribed && !e.hidden) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

QStringList SignalListModel::unplottedNames() const
{
    QStringList names;
    for (const Entry& e : m_entries) {
        if (!e.isText && e.choice == PlotChoice::Unplotted) {
            names << QString::fromUtf8(e.name);
        }
    }
    return names;
}

void SignalListModel::setPlotChoices(const QStringList& plotted, const QStringList& unplotted)
{
    for (Entry& e : m_entries) {
        const QString name = QString::fromUtf8(e.name);
        e.choice = e.isText                   ? PlotChoice::Default
                 : unplotted.contains(name)   ? PlotChoice::Unplotted
                 : plotted.contains(name)     ? PlotChoice::Plotted
                                              : PlotChoice::Default;
    }
    judgePlotted();
}

void SignalListModel::judgePlotted()
{
    // The default: every numeric row, or in a ranked set the
    // DefaultPlotted shown numeric rows of the lowest rank (a tie in the
    // set's order), so a row Live hides gives its place to the next. A
    // refused row never has a value to draw, so it is no default either;
    // a tick still plots it
    std::vector<int> ranked;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (e.plotRank != 0 && !e.isText && !e.hidden && e.refused.isEmpty()) {
            ranked.push_back(static_cast<int>(row));
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [this](int a, int b) {
        return m_entries[static_cast<size_t>(a)].plotRank
             < m_entries[static_cast<size_t>(b)].plotRank;
    });
    if (ranked.size() > static_cast<size_t>(DefaultPlotted)) {
        ranked.resize(static_cast<size_t>(DefaultPlotted));
    }
    for (Entry& e : m_entries) {
        e.plotDefault = !m_ranked && !e.isText && e.refused.isEmpty();
    }
    for (int row : ranked) {
        m_entries[static_cast<size_t>(row)].plotDefault = m_ranked;
    }

    int first = -1;
    int last = -1;
    int subscribed = 0;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        const bool plot = !e.isText
                       && (e.choice == PlotChoice::Plotted
                           || (e.choice == PlotChoice::Default && e.plotDefault));
        if (plot != e.subscribed) {
            e.subscribed = plot;
            if (first < 0) first = static_cast<int>(row);
            last = static_cast<int>(row);
        }
        subscribed += (e.subscribed && !e.hidden) ? 1 : 0;
    }
    if (first >= 0) {
        emit dataChanged(index(first), index(last), {SubscribedRole});
    }
    if (subscribed != m_subscribedCount) {
        m_subscribedCount = subscribed;
        emit subscribedCountChanged();
    }
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
        if (hide != e.hidden) {
            e.hidden = hide;
            if (first < 0) first = static_cast<int>(row);
            last = static_cast<int>(row);
        }
        hidden += e.hidden ? 1 : 0;
    }
    if (first < 0) {
        return;
    }
    // Live's filtered view re-judges the rows HiddenRole changed in
    emit dataChanged(index(first), index(last), {HiddenRole});
    m_hiddenCount = hidden;
    emit hiddenCountChanged();
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
        m->data(i, SignalListModel::SubscribedRole).toBool());
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
    int shown = 0;
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r, 0);
        if (!m->data(i, SignalListModel::HiddenRole).toBool()) {
            codes.append(m->data(i, SignalListModel::GroupRole));
            ++shown;
        }
    }
    QVariantList groups;
    for (const QVariant& choice : m_filter->groupChoices(codes)) {
        groups.append(choice.toMap().value(QStringLiteral("code")));
    }
    if (groups != m_groups || shown != m_shownCount) {
        m_groups = groups;
        m_shownCount = shown;
        emit groupsChanged();
    }
}

// ---------------- SessionBridge ----------------
SessionBridge::SessionBridge(QObject* parent)
    : QObject(parent)
    , m_frameLog(new FrameListModel(this))
    , m_frameIds(new FrameIdModel(m_frameLog, this))
    , m_recordingImport(
          new RecordingImport([this] { return recordingsRoot(); }, this))
    , m_coalesceTimer(new QTimer(this))
    , m_signalModel(new SignalListModel(this))
    , m_liveSignalModel(new LiveSignalModel(m_signalModel, this))
{
    (void)ecuio_init();   // optional and always ECUIO_OK (core.h), paired below

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        notifyFrameCounts();
        pollLinkHealth();   // every tick while connected: transitions only
        pollChannelSupport();
        pollAbsentSignals();
        pollChannelIdentity();
        pollChannelDiagnostics();
        pollReplayEnd();
        pollSleep();
        // Fires even when nothing arrived this window: dimming exists for
        // rows that stopped updating. 100 ms is well inside the 3 s dim
        // threshold, and the emission is one signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        flushValues();
        pollRecording();
    });

    // The log's readout: its time and its folder's size, once a second
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(1000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() {
        countLogBytes();
        emit logProgressChanged();
    });

    // The activity lines go into the recording's events.log as notes,
    // but for those core writes in its own form (logUnnoted)
    connect(this, &SessionBridge::logMessage, this,
            [this](const QString& line) {
                if (m_recorder != nullptr && m_noting) {
                    (void)ecuio_recorder_note(m_recorder,
                                              line.toUtf8().constData());
                }
            });
}


SessionBridge::~SessionBridge()
{
    // A Read once's session closes within its wait; its queued result is
    // dropped with `this`, and core must outlive it
    m_readOnce.join();
    // The async connect worker calls core and posts a queued completion
    // against `this`: join it before tearing the session or core down.
    // With a wedged vendor that waits up to the surrogate handshake
    // timeout (~8 s): fine at exit, not on the UI thread mid-session,
    // hence the async connect.
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
    // An async disconnect still tearing down: wait for it, so core is
    // never shut down under it. Its queued completion is dropped with
    // `this`. Bounded like any teardown: about 6 s with a wedged
    // surrogate child, which the unresponsive latch caps.
    if (m_disconnectThread.joinable()) {
        m_disconnectThread.join();
    }
    // A connect whose queued completion never ran (it was dropped with
    // `this`) left its session pending. It was never started, so no
    // worker is calling onFrame; destroy it like any other.
    if (ecuio_session_t* pending = m_pendingSession.exchange(nullptr)) {
        ecuio_session_destroy(pending);
    }
    if (m_session != nullptr) {
        destroySessionLocked();
    }
    // Closed without stopRecording's signal: QML may already be gone
    if (m_recorder != nullptr) {
        (void)ecuio_recorder_close(m_recorder, "the app closed");
        m_recorder = nullptr;
    }
    // Only now: no session is left to decode with the files' sets
    m_signalStore.close();
    m_pluginStore.close();
    for (auto& held : m_replayFiles) {
        const QByteArray name = held.second.profile.toUtf8();
        (void)ecuio_profile_unregister(name.constData());
        ecuio_signal_file_free(held.second.file);
    }
    m_replayFiles.clear();
    ecuio_shutdown();
}

// "1 signal", "2 signals": the activity log's count of a file's signals
static QString signalCountText(int count)
{
    return count == 1 ? QStringLiteral("1 signal")
                      : QStringLiteral("%1 signals").arg(count);
}

QVariantMap SessionBridge::importSignalFile(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QVariantMap result;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("Couldn't read the file")}};
    } else if (file.size() > static_cast<qint64>(ECUIO_SIGNAL_FILE_MAX_BYTES)) {
        result = {
            {QStringLiteral("ok"), false},
            {QStringLiteral("words"),
             QStringLiteral("The file is over %1 KiB, a signal file's most")
                 .arg(ECUIO_SIGNAL_FILE_MAX_BYTES / 1024)}};
    } else {
        result =
            m_signalStore.import(file.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1));
    }
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file: %1 (%2, %3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()))
            : QStringLiteral("Signal file refused: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantMap SessionBridge::importSignalText(const QString& text)
{
    const QVariantMap result = m_signalStore.import(text.toUtf8());
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file saved: %1 (%2, %3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()))
            : QStringLiteral("Signal file not saved: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QString SessionBridge::signalFileText(const QString& sha) const
{
    return SignalStore::text(sha);
}

QVariantMap SessionBridge::saveSignalFile(const QString& sha, const QUrl& url)
{
    QString words;
    const QByteArray bytes = SignalStore::bytes(sha, &words);
    const bool madeEmpty = pickedDocumentEmpty(url);
    bool written = false;
    if (!bytes.isEmpty()) {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, madeEmpty);
    }
    emit logMessage(
        written ? QStringLiteral("Signal file %1 saved to a file (%2 bytes)")
                      .arg(sha.left(12))
                      .arg(bytes.size())
                : QStringLiteral("Signal file %1 not saved: %2")
                      .arg(sha.left(12), words));
    if (!written) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), words}};
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
}

QVariantMap SessionBridge::saveLayoutFile(const QString& text, const QUrl& url)
{
    const QByteArray bytes = text.toUtf8();
    const bool madeEmpty = pickedDocumentEmpty(url);
    QString words;
    bool written = false;
    if (bytes.isEmpty()) {
        words = QStringLiteral("There is no layout to save");
    } else if (bytes.size() > layoutFileMaxBytes) {
        words = QStringLiteral("The layout is over %1 bytes, a layout file's most")
                    .arg(layoutFileMaxBytes);
    } else {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, madeEmpty);
    }
    emit logMessage(written ? QStringLiteral("Layout saved to a file (%1 bytes)")
                                  .arg(bytes.size())
                            : QStringLiteral("Layout not saved: %1").arg(words));
    return {{QStringLiteral("ok"), written}, {QStringLiteral("words"), words}};
}

QVariantMap SessionBridge::layoutFileText(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QString words;
    QString text;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        words = QStringLiteral("Couldn't read the file");
    } else {
        QByteArray bytes = file.read(layoutFileMaxBytes + 1);
        if (bytes.size() > layoutFileMaxBytes) {
            words = QStringLiteral("The file is over %1 bytes, a layout file's most")
                        .arg(layoutFileMaxBytes);
        } else {
            if (bytes.startsWith("\xEF\xBB\xBF")) {
                bytes.remove(0, 3);
            }
            QStringDecoder utf8(QStringDecoder::Utf8,
                                QStringConverter::Flag::Stateless |
                                    QStringConverter::Flag::ConvertInitialBom);
            text = utf8(bytes);
            if (utf8.hasError()) {
                text.clear();
                words = QStringLiteral("The file isn't UTF-8 text");
            }
        }
    }
    emit logMessage(words.isEmpty()
                        ? QStringLiteral("Layout file read (%1 characters)")
                              .arg(text.size())
                        : QStringLiteral("Layout file not read: %1").arg(words));
    return {{QStringLiteral("ok"), words.isEmpty()},
            {QStringLiteral("words"), words},
            {QStringLiteral("text"), text}};
}

QVariantMap SessionBridge::exportFrames(const QUrl& url)
{
    const FrameListModel::ScrubbedCsv csv = m_frameLog->scrubbedCsv();
    QString words = csv.words;
    const bool madeEmpty = pickedDocumentEmpty(url);
    bool written = false;
    if (csv.ok) {
        written = writeToUrl(url, csv.bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, madeEmpty);
    }
    const qulonglong leftOut = csv.identification + csv.textValues;
    emit logMessage(
        written ? QStringLiteral("Frames exported: %1 written, %2 left out "
                                 "(identification and text signals), %3 "
                                 "segments unattributed")
                      .arg(csv.written)
                      .arg(leftOut)
                      .arg(qulonglong{csv.unattributed})
                : QStringLiteral("Frames not exported: %1").arg(words));
    return {{QStringLiteral("ok"), written},
            {QStringLiteral("words"), written ? QString() : words},
            {QStringLiteral("written"), written ? csv.written : 0},
            {QStringLiteral("leftOut"), leftOut},
            {QStringLiteral("unattributed"), qulonglong{csv.unattributed}}};
}

QVariantMap SessionBridge::saveHealthReport(const QUrl& url, int filterIndex,
                                            const QString& baselineFolder)
{
    if (m_session == nullptr || !connected()) {
        const QString words = QStringLiteral("No session");
        emit logMessage(
            QStringLiteral("Health report not saved: %1").arg(words));
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), words}};
    }
    const std::vector<ecuio_health_value_t> held = m_signalModel->heldValues();
    ecuio_health_options_t o{};
    o.format = healthFormatFor(url, filterIndex);
    o.units = m_signalModel->unitSystem();
    o.latest_model_year = healthLatestModelYear();
    o.utc_us = healthUtcNow();
    o.session = m_session;
    o.value_count = static_cast<uint32_t>(held.size());
    o.values = held.empty() ? nullptr : held.data();
    const QByteArray baseline = healthBaselineArgument(baselineFolder);
    o.baseline_folder_utf8 =
        baseline.isEmpty() ? nullptr : baseline.constData();
    const QVariantMap r = writeHealthReport(o, url);
    emit logMessage(r.value(QStringLiteral("line")).toString());
    return r;
}

QVariantList SessionBridge::useSignalFiles(const QStringList& shas)
{
    const QVariantList used = m_signalStore.use(shas);
    for (const QVariant& v : used) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("ok")).toBool()) {
            emit logMessage(
                QStringLiteral("Signal file %1 not offered: %2")
                    .arg(e.value(QStringLiteral("sha256")).toString().left(12),
                         e.value(QStringLiteral("words")).toString()));
        }
    }
    return used;
}

int SessionBridge::deleteUnusedSignalFiles(const QStringList& keep)
{
    return m_signalStore.deleteUnused(keep);
}

QVariantMap SessionBridge::previewObdb(const QUrl& url, int modelYear,
                                       const QString& name)
{
    m_obdbPending.clear();
    const uint32_t year = modelYear > 0 ? static_cast<uint32_t>(modelYear) : 0u;
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    ObdbImport::Preview p;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        p.words = QStringLiteral("Couldn't read the file");
    } else if (file.size() > static_cast<qint64>(ECUIO_OBDB_MAX_BYTES)) {
        p.words = QStringLiteral("The file is over %1 MiB, an OBDb set's most")
                      .arg(ECUIO_OBDB_MAX_BYTES / (1024 * 1024));
    } else {
        p = ObdbImport::read(file.read(ECUIO_OBDB_MAX_BYTES + 1), year, name);
    }
    if (p.ok) {
        m_obdbPending = p.json;
    }
    emit logMessage(
        p.ok ? QStringLiteral("OBDb set read for %1: %2 converted, %3 fields "
                              "differ, %4 left out (model year %5)")
                   .arg(name)
                   .arg(p.converted)
                   .arg(p.changed.size())
                   .arg(p.skipped.size())
                   .arg(year == 0 ? QStringLiteral("none")
                                  : QString::number(year))
             : QStringLiteral("OBDb set refused: %1").arg(p.words));
    return ObdbImport::report(p, name, year);
}

QVariantMap SessionBridge::keepObdb(const QString& replacing)
{
    QVariantMap result;
    if (m_obdbPending.isEmpty()) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("No OBDb set is waiting: import it again")}};
    } else if (!replacing.isEmpty() && m_session != nullptr &&
               m_signalStore.shaFor(ecuio_session_signal_set(m_session)) ==
                   replacing) {
        // The session decodes with the file this replaces: the import
        // waits, so Keep works once it has disconnected
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("The session reads the earlier import: "
                                  "disconnect, then keep it")}};
    } else {
        result = m_signalStore.import(m_obdbPending);
        m_obdbPending.clear();
    }
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file from OBDb: %1 (%2, %3)%4")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()),
                       replacing.isEmpty()
                           ? QString()
                           : QStringLiteral(", replacing %1")
                                 .arg(replacing.left(12)))
            : QStringLiteral("OBDb set not kept: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

bool SessionBridge::isObdbImport(const QString& sha) const
{
    QString words;
    return ObdbImport::isConversion(SignalStore::bytes(sha, &words));
}

int SessionBridge::vinModelYear(const QString& typedVin) const
{
    return ObdbImport::modelYear(
        typedVin.trimmed().isEmpty() ? m_vin : typedVin,
        QDate::currentDate().year() + 1);
}

QVariantList SessionBridge::plugins()
{
    return m_pluginStore.all();
}

QVariantList SessionBridge::decodeProfiles()
{
    QVariantList out;
    for (const char* builtIn : {"obd2", "mock"}) {
        out.append(QVariantMap{
            {QStringLiteral("profile"), QString::fromLatin1(builtIn)},
            {QStringLiteral("name"), QString()}});
    }
    for (const QVariant& v : m_signalStore.registered()) {
        const QVariantMap e = v.toMap();
        out.append(QVariantMap{
            {QStringLiteral("profile"), e.value(QStringLiteral("profile"))},
            {QStringLiteral("name"), e.value(QStringLiteral("name"))}});
    }
    for (const QVariant& v : m_pluginStore.all()) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("ok")).toBool()) {
            continue;
        }
        const QStringList variants =
            e.value(QStringLiteral("variants")).toStringList();
        const QStringList profiles =
            e.value(QStringLiteral("profiles")).toStringList();
        for (qsizetype i = 0; i < profiles.size(); ++i) {
            out.append(QVariantMap{
                {QStringLiteral("profile"), profiles.at(i)},
                {QStringLiteral("name"),
                 e.value(QStringLiteral("name")).toString() +
                     (i < variants.size() ? QStringLiteral(" ") + variants.at(i)
                                          : QString())}});
        }
    }
    return out;
}

QVariantMap SessionBridge::importPlugin(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QVariantMap result;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("Couldn't read the file")}};
    } else if (file.size() > static_cast<qint64>(ECUIO_PLUGIN_PACKAGE_MAX)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("The file is over %1 MiB, a plugin's most")
                       .arg(ECUIO_PLUGIN_PACKAGE_MAX / (1024 * 1024))}};
    } else {
        result = m_pluginStore.import(file.read(ECUIO_PLUGIN_PACKAGE_MAX + 1));
    }
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin: %1 %2 (%3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("version")).toString(),
                       result.value(QStringLiteral("profiles"))
                           .toStringList()
                           .join(QStringLiteral(", ")))
            : QStringLiteral("Plugin refused: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantMap SessionBridge::removePlugin(const QString& sha)
{
    const QVariantMap result = m_pluginStore.remove(sha);
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin removed")
            : QStringLiteral("Plugin not removed: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantList SessionBridge::pluginMatches(const QString& typedVin) const
{
    return m_pluginStore.matches(typedVin.trimmed().isEmpty() ? m_vin
                                                              : typedVin,
                                 QDate::currentDate().year() + 1);
}

QString SessionBridge::readOnce(const QString& adapterUri,
                                const QVariantMap& signal,
                                const QVariantMap& module)
{
    // The adapter is the session's while one runs, and a second open
    // of it may be refused: Read once waits for none
    if (m_connected.load(std::memory_order_acquire) || m_connecting ||
        m_disconnecting) {
        return QStringLiteral(
            "Read once reads only while no session is connected");
    }
    if (m_readOnce.busy()) {
        return QStringLiteral("A read is running");
    }
    ReadOnce::Request request;
    QString why;
    if (!ReadOnce::requestFrom(signal, module, &request, &why)) {
        return why;
    }
    const QString uri = ReadOnce::uriFor(adapterUri, request, &why);
    if (uri.isEmpty()) {
        return why;
    }
    // An app-fed stream's link is readied here, as a connect's is
    if (m_streamPreparer) {
        why = m_streamPreparer(uri);
        if (!why.isEmpty()) {
            return why;
        }
    }
    applyFrameLoggingEnv();
    const bool started = m_readOnce.start(
        uri, request, m_readOnceWaitMs, [this, uri](const QVariantMap& result) {
            QMetaObject::invokeMethod(
                this,
                [this, uri, result]() {
                    m_readOnce.join();
                    QVariantMap shown = result;
                    shown.insert(QStringLiteral("uri"), uri);
                    emit readingOnceChanged();
                    emit logMessage(
                        QStringLiteral("Read once: %1 %2")
                            .arg(result.value(QStringLiteral("request"))
                                     .toString(),
                                 result.value(QStringLiteral("ok")).toBool()
                                     ? QStringLiteral("answered")
                                     : result.value(QStringLiteral("words"))
                                           .toString()));
                    emit readOnceResult(shown);
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        return QStringLiteral("A read is running");
    }
    emit readingOnceChanged();
    return {};
}

void SessionBridge::destroySessionLocked()
{
    // The destructor's teardown. destroy() closes the session first if needed.
    if (m_session != nullptr) {
        ecuio_session_destroy(m_session);
        m_session = nullptr;
    }
}

QString SessionBridge::coreVersion() const
{
    return QString::fromUtf8(ecuio_version_string());
}

QVariantMap SessionBridge::gaugeSignals() const
{
    const auto ids = [](int mock, int obd2) {
        return QVariantList{QStringLiteral("id:%1").arg(mock),
                            QStringLiteral("id:%1").arg(obd2)};
    };
    return {
        {QStringLiteral("rpm"),
         ids(ECUIO_MOCK_SIG_RPM_ID, ECUIO_OBD2_SIG_RPM_ID)},
        {QStringLiteral("speed"),
         ids(ECUIO_MOCK_SIG_SPEED_ID, ECUIO_OBD2_SIG_SPEED_ID)},
        {QStringLiteral("coolant"),
         ids(ECUIO_MOCK_SIG_COOLANT_ID, ECUIO_OBD2_SIG_COOLANT_TEMP_ID)},
        {QStringLiteral("oil"),
         ids(ECUIO_MOCK_SIG_OIL_ID, ECUIO_OBD2_SIG_OIL_TEMP_ID)},
        {QStringLiteral("iat"),
         ids(ECUIO_MOCK_SIG_IAT_ID, ECUIO_OBD2_SIG_IAT_ID)},
        {QStringLiteral("maf"),
         ids(ECUIO_MOCK_SIG_MAF_ID, ECUIO_OBD2_SIG_MAF_ID)},
        {QStringLiteral("throttle"),
         ids(ECUIO_MOCK_SIG_THROTTLE_ID, ECUIO_OBD2_SIG_THROTTLE_ID)},
        {QStringLiteral("load"),
         ids(ECUIO_MOCK_SIG_LOAD_ID, ECUIO_OBD2_SIG_ENGINE_LOAD_ID)},
        {QStringLiteral("battery"),
         ids(ECUIO_MOCK_SIG_BATTERY_V_ID, ECUIO_OBD2_SIG_BATTERY_V_ID)},
        {QStringLiteral("lambda"),
         ids(ECUIO_MOCK_SIG_LAMBDA_ID, ECUIO_OBD2_PID_SIG_ID(0x44, 0))},
        {QStringLiteral("boost"),
         ids(ECUIO_MOCK_SIG_BOOST_ID, ECUIO_OBD2_SIG_BOOST_ID)},
    };
}

const ecuio_signal_file_t*
SessionBridge::fileOfSet(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    const ecuio_signal_file_t* file = m_signalStore.fileFor(set);
    if (file == nullptr) {
        file = m_pluginStore.fileFor(set);
    }
    for (auto it = m_replayFiles.begin();
         file == nullptr && it != m_replayFiles.end(); ++it) {
        if (ecuio_signal_file_set(it->second.file) == set) {
            file = it->second.file;
        }
    }
    return file;
}

QString SessionBridge::profileOfUri(const QString& uri)
{
    // Core's scheme routing and defaults (core.cpp, ecuio_session_create):
    // case-sensitive prefixes; j2534:// on Windows alone, socketcan:// on
    // Linux but not Android (internal.h's ECUIO_HAVE_SOCKETCAN); mock://
    // decodes through mock, an adapter through obd2, a replay and a bus
    // through raw. replayUriFor has its own rule for a recorded URI
    static const struct {
        const char* scheme;
        const char* profile;
    } kSchemes[] = {
        {"mock://", "mock"},   // the mock's own set
        {"replay://", "raw"},  // a recording's frames
        {"elm327://", "obd2"}, // an adapter
#ifdef _WIN32
        {"j2534://", "obd2"}, // an adapter
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        {"socketcan://", "raw"}, // a bus
#endif
    };
    const QByteArray u = uri.toUtf8();
    QString profile;
    for (const auto& s : kSchemes) {
        if (u.startsWith(s.scheme)) {
            profile = QString::fromLatin1(s.profile);
            break;
        }
    }
    if (profile.isEmpty()) {
        return {};
    }
    const qsizetype q = u.indexOf('?');
    if (q < 0) {
        return profile;
    }
    // The query's grammar (core/src/uri_query.h): key=value segments
    // joined by '&', none empty, each with an '='; every profile= value
    // 1 to ECUIO_PROFILE_NAME_MAX bytes, the last one winning. Other keys
    // and the segment count are core's to judge at the connect
    const QList<QByteArray> segments = u.mid(q + 1).split('&');
    for (const QByteArray& segment : segments) {
        const qsizetype eq = segment.indexOf('=');
        if (eq < 0) {
            return {};
        }
        if (segment.left(eq) == "profile") {
            const QByteArray value = segment.mid(eq + 1);
            if (value.isEmpty() || value.size() > ECUIO_PROFILE_NAME_MAX) {
                return {};
            }
            profile = QString::fromUtf8(value);
        }
    }
    return profile;
}

QVariantList SessionBridge::profileSignals(const QString& uri) const
{
    const QString profile = profileOfUri(uri);
    const ecuio_signal_set_t* set = nullptr;
    if (profile.isEmpty() ||
        ecuio_profile_lookup(profile.toUtf8().constData(), &set) != ECUIO_OK ||
        set == nullptr || set->defs == nullptr) {
        return {};
    }
    const ecuio_signal_file_t* file = fileOfSet(set);
    const bool builtIn = SignalListModel::isBuiltIn(set);
    const uint32_t units = m_signalModel->unitSystem();
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(set->count));
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        const SignalListModel::DefFacts f =
            SignalListModel::factsOf(file, builtIn, def);
        const ecuio_display_value_t shown =
            displayed(def.unit, def.decimal_places, 0.0, units);
        const std::pair<double, double> range = displayedRange(
            def.unit, def.decimal_places, f.rangeLow, f.rangeHigh, units);
        // The file's own gauge for this def, in its decoded unit (the unit
        // a cell stores); empty when the file gives none
        QVariantMap gauge;
        if (file != nullptr) {
            if (const ecuio_signal_gauge_t* g =
                    ecuio_signal_file_gauge(file, &def)) {
                gauge.insert(QStringLiteral("min"), g->min);
                gauge.insert(QStringLiteral("max"), g->max);
                if ((g->flags & ECUIO_GAUGE_WARN_LOW) != 0) {
                    gauge.insert(QStringLiteral("warnLow"), g->warn_low);
                }
                if ((g->flags & ECUIO_GAUGE_WARN_HIGH) != 0) {
                    gauge.insert(QStringLiteral("warnHigh"), g->warn_high);
                }
            }
        }
        rows.append(QVariantMap{
            {QStringLiteral("id"), static_cast<int>(def.id)},
            {QStringLiteral("ref"), f.ref},
            {QStringLiteral("name"), QString::fromUtf8(def.name)},
            {QStringLiteral("unit"), QString::fromUtf8(shown.unit)},
            {QStringLiteral("decimals"),
             static_cast<int>(shown.decimal_places)},
            {QStringLiteral("isText"),
             def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX},
            {QStringLiteral("isEnum"), f.isEnum},
            {QStringLiteral("group"), static_cast<int>(def.group)},
            {QStringLiteral("rangeLow"), range.first},
            {QStringLiteral("rangeHigh"), range.second},
            {QStringLiteral("decodedUnit"), QString::fromUtf8(def.unit)},
            {QStringLiteral("decodedLow"), f.rangeLow},
            {QStringLiteral("decodedHigh"), f.rangeHigh},
            {QStringLiteral("gauge"), gauge},
        });
    }
    return rows;
}

QString SessionBridge::statusString(int statusCode) const
{
    return QString::fromUtf8(ecuio_status_string(static_cast<ecuio_status_t>(statusCode)));
}

QVariantList SessionBridge::discoverJ2534Devices() const
{
    QVariantList out;

    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;
    if (ecuio_j2534_discover_devices(&devices, &count) != ECUIO_OK) {
        return out;   // includes non-Windows NOT_IMPLEMENTED -> empty list
    }
    for (uint32_t i = 0; i < count; ++i) {
        QVariantMap m;
        m["vendorName"] = QString::fromUtf8(devices[i].vendor_name);
        m["dllPath"] = QString::fromUtf8(devices[i].dll_path);
        /* reserved[0] is the loader's PE sniff: 1 = x86, 2 = x64,
         * 0 = unknown. Unknown is displayed honestly: the loader
         * sends it to the x64 surrogate, which is where it would
         * load, not a claim about the binary. */
        m["architecture"] = (devices[i].reserved[0] == 2)
                                ? QStringLiteral("x64")
                            : (devices[i].reserved[0] == 1)
                                ? QStringLiteral("x86")
                                : QStringLiteral("unknown");
        out.append(m);
    }
    ecuio_j2534_free_devices(devices);
    return out;
}

QVariantList SessionBridge::discoverSerialPorts() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_serial_ports(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* A port plugged in between the calls is left for the next refresh */
    std::vector<ecuio_serial_port_t> ports(count);
    if (ecuio_serial_ports(ports.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(ports.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(ports[i].name);
        m["device"] = QString::fromUtf8(ports[i].device);
        out.append(m);
    }
    return out;
}

namespace {
/* A CAN mode mask in core's words (ip link's), the named bits alone */
QStringList canModeWords(uint32_t modes)
{
    QStringList words;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        const char* word = (modes & bit) != 0 ? ecuio_can_mode_name(bit) : "";
        if (*word != '\0') {
            words.append(QString::fromLatin1(word));
        }
    }
    return words;
}
} // namespace

QVariantList SessionBridge::discoverCanInterfaces() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_can_interfaces(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* An interface added between the calls is left for the next refresh */
    std::vector<ecuio_can_interface_t> list(count);
    if (ecuio_can_interfaces(list.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(list.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromLatin1(list[i].name);
        m["kind"] = QString::fromLatin1(list[i].kind);
        m["up"] = (list[i].flags & ECUIO_CAN_UP) != 0;
        m["listenOnly"] = (list[i].flags & ECUIO_CAN_LISTEN_ONLY) != 0;
        m["fd"] = (list[i].flags & ECUIO_CAN_FD) != 0;
        m["bitrate"] = list[i].bitrate;
        m["state"] = QString::fromLatin1(ecuio_can_state_name(list[i].state));
        m["modes"] = canModeWords(list[i].modes);
        m["modesKnown"] = (list[i].flags & ECUIO_CAN_MODES_KNOWN) != 0;
        m["modesSupported"] = canModeWords(list[i].modes_supported);
        out.append(m);
    }
    return out;
}

QString SessionBridge::replayUriFor(const QString& folder,
                                    const QString& recordedUri)
{
    const qsizetype q = recordedUri.indexOf(QLatin1Char('?'));
    QString profile;
    QStringList dids;
    if (q >= 0) {
        // Core's grammar: key=value pairs split on &, the last profile=
        // winning
        const QStringList pairs =
            recordedUri.mid(q + 1).split(QLatin1Char('&'));
        for (const QString& pair : pairs) {
            if (pair.startsWith(QLatin1String("profile="))) {
                profile = pair.mid(8);
            } else if (pair.startsWith(QLatin1String("did="))) {
                dids << pair;
            }
        }
    }
    if (profile.isEmpty()) {
        // Core's defaults by scheme (ADR-0013); a replay's is raw, which
        // can't decode a recording wrongly, and so is an unknown source's.
        // Not profileOfUri's table: a recording made on another platform
        // names j2534:// or another case, and its frames still decode
        profile = recordedUri.startsWith(QLatin1String("mock://"),
                                         Qt::CaseInsensitive)
                      ? QStringLiteral("mock")
                  : recordedUri.startsWith(QLatin1String("j2534://"),
                                           Qt::CaseInsensitive) ||
                          recordedUri.startsWith(QLatin1String("elm327://"),
                                                 Qt::CaseInsensitive)
                      ? QStringLiteral("obd2")
                      : QStringLiteral("raw");
    }
    QString uri = QStringLiteral("replay://") +
                  QDir::fromNativeSeparators(folder) +
                  QStringLiteral("?profile=") + profile;
    // did= belongs to uds-generic alone; core refuses it under any other
    if (profile == QLatin1String("uds-generic")) {
        for (const QString& did : dids) {
            uri += QLatin1Char('&') + did;
        }
    }
    return uri;
}

QString SessionBridge::replayUriWithOwnSignals(const QString& uri)
{
    const std::string uri8 = uri.toStdString();
    std::string folder8;
    if (!cli::replay_folder(uri8, &folder8)) {
        return uri;
    }
    const QString folder = QString::fromStdString(folder8);
    cli::RecordedSignals recorded;
    QFile events(folder + QStringLiteral("/events.log"));
    if (!events.open(QIODevice::ReadOnly)) {
        return uri;
    }
    // The header alone: its lines up to the blank one (the enum labels'
    // lines make it longer than a few)
    while (!events.atEnd()) {
        QByteArray line = events.readLine(64 * 1024);
        if (line.endsWith('\n')) {
            line.chop(1);
        }
        if (!recorded.take(line.toStdString())) {
            break;
        }
    }
    if (!recorded.names_file()) {
        return uri;
    }
    // A URI with no profile= replays raw (ADR-0013), which nothing decodes
    // wrongly: the copy is for the profile the recording names
    std::string selected;
    if (!cli::uri_profile(uri8, &selected)) {
        return uri;
    }
    const QString profile = QString::fromStdString(recorded.profile);
    const QString own =
        QString::fromStdString(cli::own_profile(recorded.sha256));
    const QString chosen = QString::fromStdString(selected);
    if (chosen != profile && chosen != own) {
        return uri;
    }
    const auto as_loaded = [&](const QString& why) {
        emit logMessage(QStringLiteral("Replay: the recording's signals.json "
                                       "%1; decoding with %2 as loaded now")
                            .arg(why, chosen));
        return uri;
    };
    QFile copy(folder + QStringLiteral("/signals.json"));
    if (!copy.open(QIODevice::ReadOnly)) {
        return as_loaded(QStringLiteral("is missing"));
    }
    const QByteArray bytes = copy.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1);
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (sha.toStdString() != recorded.sha256) {
        return as_loaded(QStringLiteral("doesn't match its hash"));
    }
    const auto selecting = [&](const QString& p) {
        return chosen == p ? uri
                           : QString::fromStdString(
                                 cli::with_profile(uri8, p.toStdString()));
    };
    // The same bytes in the store: the vehicle's own file, under its name
    if (m_signalStore.profileOf(sha) == profile) {
        return selecting(profile);
    }
    auto held = m_replayFiles.find(sha);
    if (held == m_replayFiles.end()) {
        ecuio_signal_file_t* file = nullptr;
        if (ecuio_signal_file_load(bytes.constData(),
                                   static_cast<size_t>(bytes.size()),
                                   &file) != ECUIO_OK) {
            return as_loaded(
                QStringLiteral("doesn't load (%1)")
                    .arg(QString::fromUtf8(ecuio_last_error_detail())));
        }
        const QByteArray name8 = own.toUtf8();
        const ecuio_status_t rc = ecuio_profile_register(
            name8.constData(), ecuio_signal_file_set(file), nullptr);
        if (rc != ECUIO_OK) {
            ecuio_signal_file_free(file);
            return as_loaded(
                QStringLiteral("can't be registered (%1)")
                    .arg(QString::fromUtf8(ecuio_status_string(rc))));
        }
        held = m_replayFiles
                   .emplace(sha, ReplayFile{file,
                                            QString::fromUtf8(
                                                ecuio_signal_file_name(file)),
                                            own})
                   .first;
    }
    emit logMessage(QStringLiteral("Replay: decoding with the recording's own "
                                   "signal file, %1 (%2)")
                        .arg(held->second.name, own));
    return selecting(own);
}

namespace {

// A folder the viewer opens: one holding a signals.csv (values, a CSV
// import's alone) or a frames.csv (frames, decoded through a profile)
bool holdsRecording(const QString& folder)
{
    return QFileInfo::exists(folder + QStringLiteral("/signals.csv")) ||
           QFileInfo::exists(folder + QStringLiteral("/frames.csv"));
}

// A recording folder's row for the Recordings page: { title, uri, folder,
// source, profile, frames, durationMs, signals, log, marks } (recentRecordings
// says what each holds)
QVariantMap recordingRowOf(const QFileInfo& info)
{
    // "<yyyyMMdd-HHmmss>[-n][ <label>]" (core's recorder)
    static const QRegularExpression kName(
        QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})-(\\d{2})(\\d{2})(\\d{2})(?:-"
                       "\\d+)?(?: (.+))?$"));
    const QString folder = info.absoluteFilePath();

    // events.log's header, up to its "columns" line: the "uri", each "signal"
    // and the "focus" that only a log of chosen signals has
    QString recorded;
    int signalCount = 0;
    bool isLog = false;
    qint64 durationMs = -1;
    QFile events(folder + QStringLiteral("/events.log"));
    if (events.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A header holds a line a signal; a file with no "columns" line is read no further
        for (int n = 0; n < 400 && !events.atEnd(); ++n) {
            const QString line = QString::fromUtf8(events.readLine()).trimmed();
            if (line.startsWith(QLatin1String("columns"))) {
                break;
            }
            if (line.startsWith(QLatin1String("uri ")) && recorded.isEmpty()) {
                recorded = line.mid(4).trimmed();
            } else if (line.startsWith(QLatin1String("signal "))) {
                ++signalCount;
            } else if (line.startsWith(QLatin1String("focus "))) {
                isLog = true;
            }
        }
        // The last line's "+seconds" column is the recording's length
        static const QRegularExpression kAt(
            QStringLiteral("^\\S+\\s+\\+(\\d+(?:\\.\\d+)?)\\s"));
        constexpr qint64 kTail = 4096;
        events.seek(qMax<qint64>(0, events.size() - kTail));
        const QStringList tail = QString::fromUtf8(events.readAll())
                                     .split(QLatin1Char('\n'),
                                            Qt::SkipEmptyParts);
        for (auto it = tail.crbegin(); it != tail.crend(); ++it) {
            const QRegularExpressionMatch at = kAt.match(*it);
            if (at.hasMatch()) {
                durationMs = qRound64(at.captured(1).toDouble() * 1000.0);
                break;
            }
        }
    }
    // markers.csv: a header row, then one row each
    int marks = 0;
    QFile markers(folder + QStringLiteral("/markers.csv"));
    if (markers.open(QIODevice::ReadOnly)) {
        while (!markers.atEnd()) {
            marks += markers.readLine().trimmed().isEmpty() ? 0 : 1;
        }
        marks = qMax(0, marks - 1);
    }

    const QRegularExpressionMatch m = kName.match(info.fileName());
    QString title = info.fileName();
    if (m.hasMatch()) {
        title = QStringLiteral("%1-%2-%3 %4:%5:%6")
                    .arg(m.captured(1), m.captured(2), m.captured(3),
                         m.captured(4), m.captured(5), m.captured(6));
        if (m.hasCaptured(7)) {
            title += QStringLiteral(" · ") + m.captured(7);
        }
    }
    const QString uri = SessionBridge::replayUriFor(folder, recorded);
    QVariantMap row;
    row["title"] = title;
    row["uri"] = uri;
    row["folder"] = folder;
    row["source"] = SessionBridge::uriLabelFor(recorded);
    // After the query's "?profile=", the last ? in it
    row["profile"] = uri.mid(uri.lastIndexOf(QLatin1Char('?')) + 9)
                         .section(QLatin1Char('&'), 0, 0);
    // Replay plays frames.csv: a folder of values alone has none to play
    row["frames"] = QFileInfo::exists(folder + QStringLiteral("/frames.csv"));
    row["durationMs"] = durationMs;
    row["signals"] = signalCount;
    row["log"] = isLog;
    row["marks"] = marks;
    return row;
}

} // namespace

QVariantList SessionBridge::recentRecordings(int limit) const
{
    QVariantList out;
    if (limit <= 0) {
        return out;
    }
    // Both roots' folders, newest first across them (the names start with
    // the stamp, so by name, last first, is newest first)
    QFileInfoList folders;
    for (const QString& root : {recordingsRoot(), earlierRecordingsRoot()}) {
        if (!root.isEmpty()) {
            folders +=
                QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
    }
    std::sort(folders.begin(), folders.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.fileName() > b.fileName();
              });
    for (const QFileInfo& info : folders) {
        if (out.size() >= limit) {
            break;
        }
        if (holdsRecording(info.absoluteFilePath())) {
            out.append(recordingRowOf(info));
        }
    }
    return out;
}

QVariantMap SessionBridge::recordingRow(const QString& folder) const
{
    if (folder.isEmpty() || !holdsRecording(folder)) {
        return {};
    }
    QVariantMap row = recordingRowOf(QFileInfo(folder));
    row["folder"] = folder;
    return row;
}

// Pure-core half: safe on any thread. Creates and registers, the slow
// part (DLL load, PassThruOpen), but doesn't start: until the GUI thread
// has taken the session, no worker may call onFrame with `this`. On
// failure, cleans up fully (core API, mutex-safe off-thread) and returns
// the error string. On success the caller owns *out_session and must
// startSession() and applyConnectedState() on the GUI thread. Touches no
// Qt models, timers, or properties.
QString SessionBridge::connectCoreOnly(const QString& uri,
                                       ecuio_session_t** out_session)
{
    const QByteArray uri8 = uri.toUtf8();
    ecuio_session_t* session = nullptr;
    ecuio_status_t rc = ecuio_session_create(uri8.constData(), &session);
    if (rc != ECUIO_OK || session == nullptr) {
        return failureText(rc);
    }

    rc = ecuio_register_frame_callback(session, &SessionBridge::onFrame, this);
    if (rc == ECUIO_OK) {
        // ADR-0024: core decodes on the worker and hands each frame's
        // values to onValues, after onFrame
        rc = ecuio_register_values_callback(session, &SessionBridge::onValues, this);
    }
    if (rc != ECUIO_OK) {
        ecuio_session_destroy(session);
        return QString::fromUtf8(ecuio_status_string(rc));
    }

    *out_session = session;
    return {};
}

// GUI thread: start the worker of a session connectCoreOnly produced.
// onFrame and onValues accept from here (m_accepting), so a frame sent
// at once, as a replay's first is, queues and reaches
// handleFrameInternal after applyConnectedState, and its values wait in
// the staging, sized here, for the first tick. On failure the session
// is destroyed.
QString SessionBridge::startSession(ecuio_session_t* session,
                                    const QString& uri)
{
    // Zeroed before the worker starts, not at the apply: a replay at max
    // can pass the in-flight cap before then, and those drops count
    m_framesDelivered.store(0, std::memory_order_relaxed);
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_lastReportedDropped = 0;
    m_overflowFrames.store(0, std::memory_order_relaxed);
    if (m_reportedOverflow != 0) {
        m_reportedOverflow = 0;
        emit overflowFramesChanged();
    }
    m_frameLatency.clear();
    m_valueLatency.clear();
    m_valuesStaged = 0;
    prepareValueStaging(ecuio_session_signal_set(session));
    m_frameLog->noteTextDefs(ecuio_session_signal_set(session));
    // Attached before the worker starts, so the recording holds every frame
    startRecording(session, uri);
    m_accepting.store(true, std::memory_order_release);
    const ecuio_status_t rc = ecuio_session_start(session);
    if (rc != ECUIO_OK) {
        m_accepting.store(false, std::memory_order_release);
        const QString err = failureText(rc);
        // The folder keeps its header and says why there are no frames
        stopRecording(QStringLiteral("the session didn't start: %1").arg(err));
        ecuio_unregister_frame_callback(session, &SessionBridge::onFrame, this);
        ecuio_unregister_values_callback(session, &SessionBridge::onValues, this);
        ecuio_session_destroy(session);
        return err;
    }
    return {};
}

// GUI-thread half: the state a successful connect applies, ending with the
// "Connected:" and "Session:" log lines; the sync and async paths share it,
// so they apply the same state.
void SessionBridge::applyConnectedState(ecuio_session_t* session, const QString& uri)
{
    // Before any emit: each runs handlers synchronously, and a connect one
    // of them starts must be refused, not replace this session
    m_session = session;
    m_connected.store(true, std::memory_order_release);
    ecuio_session_info_t info{};
    const bool haveInfo = ecuio_session_get_info(session, &info) == ECUIO_OK;
    m_adapterVersions = haveInfo ? versionsText(info) : QString();
    m_vin.clear(); // this car's comes with its identity
    m_currentUri = uri;
    emit currentUriChanged();
    // The frame log belongs to a session, like its count: rows from two
    // connects would otherwise run together with nothing between them.
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    m_signalModel->setSignalSet(ecuio_session_signal_set(session),
                                fileOfSet(ecuio_session_signal_set(session)));
    m_stagingBuffer.clear();   // previous session's batch must never
    m_stagingTexts.clear();    // bleed into this one's first
    m_linkErrorCode = ECUIO_OK;
    m_errorEpisodes = 0;
    emit linkStateChanged();
    setLogPaused(false);       // a new session always logs
    m_coalesceTimer->start(100);
    emit connectedChanged(true);
    emit framesReset();

    emit logMessage(QStringLiteral("Connected: %1").arg(uri));
    m_channelCount = 0;
    if (haveInfo) {
        emit logMessage(sessionInfoText(info));
        m_channelCount = std::min<uint32_t>(info.channel_count,
                                            ECUIO_SESSION_INFO_MAX_CHANNELS);
    }
    m_replayFrames = info.replay_frames;
    m_sleptUs = sleptUs();
    m_replayEndLogged = info.backend != ECUIO_BACKEND_REPLAY ||
                        (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
    for (uint32_t& g : m_supportGeneration) {
        g = 0;
    }
    for (ecuio_channel_support_t& s : m_supportReports) {
        s = ecuio_channel_support_t{};
    }
    for (SessionWritten& w : m_sessionWritten) {
        w = SessionWritten{};
    }
    if (!m_refusedUnnamed.isEmpty()) {
        m_refusedUnnamed.clear();
        emit refusedUnnamedChanged();
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    m_absentRead.assign((set != nullptr && set->defs != nullptr) ? set->count : 0u, 0);
    m_unsupportedRead.assign(m_absentRead.size(), 0);
    m_absentIds.clear();
    for (uint32_t& g : m_identityGeneration) {
        g = 0;
    }
    for (uint32_t& g : m_diagnosticsGeneration) {
        g = 0;
    }
    for (bool& asked : m_clearAsked) {
        asked = false;
    }
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    m_clearSavedTo.clear();
    emit clearMovingChanged();
    m_diagnosticsShownCount = 0;
    if (!m_diagnostics.isEmpty()) {
        m_diagnostics.clear();
        emit diagnosticsChanged();
    }
}

QString SessionBridge::connectSession(const QString& typed)
{
    // Spaces around a pasted URI are the field's, not the URI's: core
    // matches the scheme at the first character
    QString uri = typed.trimmed();
    if (m_disconnecting) {
        return QStringLiteral("Disconnect in progress");
    }
    if (m_connected.load(std::memory_order_acquire) || m_connecting) {
        return {};
    }
    if (m_readOnce.busy()) {
        return QStringLiteral("Read once is using the adapter");
    }
    uri = replayUriWithOwnSignals(uri);

    QString err = m_streamPreparer ? m_streamPreparer(uri) : QString();
    if (!err.isEmpty()) {
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    applyFrameLoggingEnv();
    ecuio_session_t* session = nullptr;
    err = connectCoreOnly(uri, &session);
    if (err.isEmpty()) {
        err = startSession(session, uri);
    }
    if (!err.isEmpty()) {
        emit logMessage(QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    applyConnectedState(session, uri);
    return {};
}

void SessionBridge::connectSessionAsync(const QString& typed)
{
    QString uri = typed.trimmed(); // as connectSession does
    if (m_connected.load(std::memory_order_acquire) || m_connecting
        || m_disconnecting) {
        return;   // one attempt at a time, none during a teardown; the
                  // UI already reflects state
    }
    if (m_readOnce.busy()) {
        const QString err = QStringLiteral("Read once is using the adapter");
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        emit connectResult(uri, err);
        return;
    }
    uri = replayUriWithOwnSignals(uri);
    // An app-fed stream's link is readied here, on the GUI thread, before
    // core opens it on the connect thread
    const QString unprepared =
        m_streamPreparer ? m_streamPreparer(uri) : QString();
    if (!unprepared.isEmpty()) {
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, unprepared));
        emit connectResult(uri, unprepared);
        return;
    }
    m_connecting = true;
    emit connectingChanged();
    emit logMessage(QStringLiteral("Connecting: %1").arg(uri));
    applyFrameLoggingEnv();   // before the connect thread spawns any surrogate

    m_connectThread = std::thread([this, uri]() {
        ecuio_session_t* session = nullptr;
        const QString core_err = connectCoreOnly(uri, &session);
        // Handed over through the member, not the lambda: a completion
        // dropped with `this` would leak a lambda capture, while the
        // destructor can always find the member.
        m_pendingSession.store(session);

        QMetaObject::invokeMethod(this, [this, uri, core_err]() {
            // Join first, then clear the flag, then emit: assigning to a
            // joinable std::thread terminates the process, and a QML
            // handler can run synchronously inside connectingChanged.
            // connecting clears only once the session is applied, so a
            // handler always sees connecting or connected: a connect it
            // starts is refused, never a second session over this one.
            if (m_connectThread.joinable()) {
                m_connectThread.join();
            }
            ecuio_session_t* session = m_pendingSession.exchange(nullptr);
            QString err = core_err;
            if (err.isEmpty()) {
                err = startSession(session, uri);
            }
            if (err.isEmpty()) {
                applyConnectedState(session, uri);
            }
            m_connecting = false;
            emit connectingChanged();

            if (err.isEmpty()) {
                emit connectResult(uri, {});
            } else {
                emit logMessage(QStringLiteral("Connect failed: %1 (%2)")
                                    .arg(uri, err));
                emit connectResult(uri, err);
            }
        }, Qt::QueuedConnection);
    });
}

void SessionBridge::disconnectSession()
{
    if (!m_connected.load(std::memory_order_acquire) || m_disconnecting) return;

    const QString uri = m_currentUri;
    emit logMessage(QStringLiteral("Disconnecting: %1").arg(uri));
    // The close sends 10 01 after the worker's last report (ADR-0060)
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        const QString line = sessionReturnedText(m_supportReports[c]);
        if (!line.isEmpty()) {
            emit logMessage(line);
        }
    }

    // Every flag before any emit: a QML handler can run synchronously
    // inside one, and must already see disconnecting with connected
    // false, never a moment where Connect looks available. From here
    // onFrame and handleFrameInternal drop everything, and the GUI
    // thread never touches the session again.
    ecuio_session_t* session = std::exchange(m_session, nullptr);
    m_accepting.store(false, std::memory_order_release);
    m_connected.store(false, std::memory_order_release);
    m_disconnecting = true;
    m_coalesceTimer->stop();
    notifyFrameCounts();   // the last tick's worth
    flushValues();         // and its values
    emit logMessage(deliveryText(deliveryStats()));
    m_valueSet = nullptr;  // the drain's; onValues stops at m_accepting
    m_stagingBuffer.clear();
    m_stagingTexts.clear();
    m_currentUri.clear();
    m_adapterVersions.clear();
    m_vin.clear();
    m_linkErrorCode = ECUIO_OK;           // the properties must not
    m_errorEpisodes = 0;              // outlive the session

    emit disconnectingChanged();
    if (!uri.isEmpty()) {
        emit currentUriChanged();
    }
    emit connectedChanged(false);
    emit linkStateChanged();          // "fault"/"quiet" -> "disconnected"
    setLogPaused(false);

    // The teardown thread is the session's one caller from here: the
    // session API is single-caller by contract.
    m_disconnectThread = std::thread([this, session, uri]() {
        // Close joins the worker, whose last act publishes reply timing's
        // final rows (ADR-0042); they're read here because destroy frees
        // them
        ecuio_session_close(session);
        ecuio_session_timing_t timing{};
        std::vector<ecuio_request_timing_t> rows;
        uint32_t count = 0;
        if (ecuio_session_get_timing(session, &timing, nullptr, 0, &count) ==
                ECUIO_OK &&
            count > 0) {
            rows.resize(count);
            if (ecuio_session_get_timing(session, &timing, rows.data(), count,
                                         &count) != ECUIO_OK) {
                count = 0;
            }
            rows.resize(std::min<size_t>(count, rows.size()));
        }
        ecuio_session_destroy(session);

        QMetaObject::invokeMethod(
            this,
            [this, uri, timing, rows]() {
                // Join first, then close the recording, then clear the
                // flag and emit, as the connect completion does: a QML
                // handler inside disconnectingChanged may start the next
                // connect, which must find this session's recorder gone.
                if (m_disconnectThread.joinable()) {
                    m_disconnectThread.join();
                }
                const QString timingLine = timingText(timing, rows);
                if (!timingLine.isEmpty()) {
                    emit logMessage(timingLine);
                }
                emit logMessage(QStringLiteral("Disconnected: %1").arg(uri));
                // Kept open until here, so it brackets the whole teardown.
                // Only the user's Disconnect gets here: the Dashboard needs
                // no reason, the recording's end note keeps it
                stopRecording(QStringLiteral("disconnected"), false,
                              QStringLiteral("Log ended"));
                m_disconnecting = false;
                emit disconnectingChanged();
            },
            Qt::QueuedConnection);
    });
}

void SessionBridge::setRecording(bool on)
{
    if (m_connected.load(std::memory_order_acquire) || m_connecting || m_disconnecting) {
        emit recordingChanged();   // refused: puts the switch back
        return;
    }
    if (m_recording == on) return;
    m_recording = on;
    emit recordingChanged();
}

namespace {
// Settings' choices for a log's minimum time between sweeps, ms; 0 is off
constexpr int kMinSweepChoices[] = {0, 100, 250, 500, 1000};
} // namespace

QVariantList SessionBridge::minSweepChoices() const
{
    QVariantList choices;
    for (int ms : kMinSweepChoices) {
        choices.append(ms);
    }
    return choices;
}

void SessionBridge::setMinSweepMs(int ms)
{
    if (std::find(std::begin(kMinSweepChoices), std::end(kMinSweepChoices),
                  ms) == std::end(kMinSweepChoices) ||
        ms == m_minSweepMs) {
        return;
    }
    m_minSweepMs = ms;
    emit minSweepMsChanged();
}

void SessionBridge::setUnitSystem(int units)
{
    if (units != static_cast<int>(ECUIO_UNITS_METRIC) &&
        units != static_cast<int>(ECUIO_UNITS_IMPERIAL)) {
        return;
    }
    if (units == unitSystem()) return;
    m_signalModel->setUnitSystem(static_cast<uint32_t>(units));
    emit unitSystemChanged();
    emit clearMovingChanged();
    if (m_diagnosticsShownCount > 0) {
        rebuildDiagnostics();
    }
}

double SessionBridge::displayValue(const QString& unit, double value) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, value, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return value;
    }
    return out.value;
}

QString SessionBridge::displayUnit(const QString& unit) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, 0.0, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return unit;
    }
    return QString::fromUtf8(out.unit);
}

QString SessionBridge::recordingFolder() const
{
    if (m_recordingFolder.isEmpty()) {
        return {};
    }
    return documentsRelative(m_recordingFolder);
}

QUrl SessionBridge::recordingFolderUrl() const
{
    return m_recordingFolder.isEmpty() ? QUrl()
                                       : QUrl::fromLocalFile(m_recordingFolder);
}

// Recording turns on the surrogate's payload logging for the spawns it
// records, so the session log holds the requests and write statuses the
// recording can't see. A value set at launch is left alone; one the
// bridge set comes off again at the next connect without recording.
// Called on the GUI thread before the connect spawns anything; the child
// inherits the environment at spawn.
void SessionBridge::applyFrameLoggingEnv()
{
    static const char kVar[] = "ECUIO_SUR_LOG_FRAMES";
    if (m_recording) {
        if (!m_setFrameLogging && !qEnvironmentVariableIsSet(kVar)) {
            qputenv(kVar, "1");
            m_setFrameLogging = true;
        }
    } else if (m_setFrameLogging) {
        qunsetenv(kVar);
        m_setFrameLogging = false;
    }
}

QString SessionBridge::recordingsRoot() const
{
    if (!m_recordingsRoot.isEmpty()) {
        return m_recordingsRoot;
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs.isEmpty() ? QString()
                          : docs + QStringLiteral("/Havne/recordings");
}

QString SessionBridge::earlierRecordingsRoot() const
{
    if (!m_earlierRecordingsRoot.isEmpty()) {
        return m_earlierRecordingsRoot;
    }
    if (!m_recordingsRoot.isEmpty()) {
        return QString(); // a test's own root: Documents is never read
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString root = docs + QLatin1Char('/') + earlierProductName() +
                         QStringLiteral("/recordings");
    return docs.isEmpty() || root == recordingsRoot() ? QString() : root;
}

void SessionBridge::startRecording(ecuio_session_t* session, const QString& uri)
{
    if (!m_recording) return;
    // A replay would copy the recording it plays
    if (!m_recordReplays &&
        uri.startsWith(QLatin1String("replay://"), Qt::CaseInsensitive)) {
        emit logMessage(QStringLiteral(
            "Not recording: a replay plays a recording already"));
        return;
    }
    ecuio_recorder_t* recorder =
        openRecorder(session, uri, QStringLiteral("Recording failed"));
    if (recorder == nullptr) {
        return;
    }
    adoptRecorder(recorder);
    emit logMessage(QStringLiteral("Recording: %1").arg(recordingFolder()));
}

namespace {

// A recorder's folder, absolute; empty if core couldn't give it whole, so
// a caller never acts on a cut path
QString folderOf(ecuio_recorder_t* recorder)
{
    char folder[4096] = {};
    uint32_t length = 0;
    if (ecuio_recorder_get_folder(recorder, folder, sizeof folder, &length) !=
            ECUIO_OK ||
        length >= sizeof folder) {
        return QString();
    }
    return QString::fromUtf8(folder);
}

// Whether frames.csv holds a row past its header
bool holdsFrames(const QString& folder)
{
    QFile frames(QDir(folder).filePath(QStringLiteral("frames.csv")));
    if (!frames.open(QIODevice::ReadOnly)) {
        return false;
    }
    (void)frames.readLine();
    return !frames.readLine().trimmed().isEmpty();
}

} // namespace

ecuio_recorder_t* SessionBridge::openRecorder(ecuio_session_t* session,
                                              const QString& uri,
                                              const QString& failed,
                                              QString* reason)
{
    const auto fail = [this, &failed, reason](const QString& why) {
        if (reason != nullptr) {
            *reason = why;
        }
        emit logMessage(QStringLiteral("%1: %2").arg(failed, why));
        return nullptr;
    };
    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        return fail(QStringLiteral("no Documents folder"));
    }

    // The picked vehicle names an adapter's session only (ADR-0027 section 1):
    // a mock or a replay isn't that car. Core reduces the label to what a
    // folder's name may hold (ADR-0049 section 2).
    const bool adapter =
        uri.startsWith(QStringLiteral("j2534://"), Qt::CaseInsensitive) ||
        uri.startsWith(QStringLiteral("elm327://"), Qt::CaseInsensitive);
    const QByteArray frameLogging = qgetenv("ECUIO_SUR_LOG_FRAMES");
    // The app's own header lines, which core writes under "caller"
    QList<QByteArray> lines = {
        QStringLiteral("Havne %1").arg(coreVersion()).toUtf8(),
        (m_setFrameLogging
             ? QStringLiteral(
                   "surrogate frame logging  on, set by this recording")
         : frameLogging.isNull()
             ? QStringLiteral("surrogate frame logging  off")
             : QStringLiteral("surrogate frame logging  "
                              "ECUIO_SUR_LOG_FRAMES=%1, set at launch")
                   .arg(QString::fromUtf8(frameLogging)))
            .toUtf8(),
    };
    if (adapter && !m_vehicleLabel.isEmpty()) {
        lines << QStringLiteral("vehicle  %1").arg(m_vehicleLabel).toUtf8();
    }
    std::vector<const char*> header;
    for (const QByteArray& l : lines) {
        header.push_back(l.constData());
    }
    const QByteArray root8 = root.toUtf8();
    const QByteArray label8 = adapter ? m_vehicleLabel.toUtf8() : QByteArray();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.constData();
    o.label_utf8 = label8.constData();
    o.header_lines = header.data();
    o.header_line_count = static_cast<uint32_t>(header.size());
    ecuio_recorder_t* recorder = nullptr;
    ecuio_status_t rc = ecuio_recorder_open(&o, &recorder);
    if (rc != ECUIO_OK) {
        return fail(QString::fromUtf8(ecuio_last_error_detail()));
    }
    // The signal file decoding the session goes in as signals.json
    // (ADR-0051 section 4): a vehicle's, its hash the store copy's name,
    // or a plugin variant's files merged, with the plugin's line
    // (ADR-0052 section 4), as havne-cli stream --plugin --record does
    const ecuio_signal_file_t* file =
        fileOfSet(ecuio_session_signal_set(session));
    if (file != nullptr) {
        rc = ecuio_recorder_signal_file(recorder, file);
    }
    if (rc == ECUIO_OK) {
        rc = ecuio_session_record(session, recorder);
    }
    if (rc != ECUIO_OK) {
        // The folder is this call's own, made by the open above; a
        // session never attached wrote it no frame, so nothing is lost
        const QString folder =
            m_failRecorderFolderForTesting ? QString() : folderOf(recorder);
        (void)ecuio_recorder_close(recorder,
                                   "the session couldn't be recorded");
        if (!folder.isEmpty() && !holdsFrames(folder)) {
            (void)QDir(folder).removeRecursively();
        }
        return fail(QString::fromUtf8(ecuio_status_string(rc)));
    }
    return recorder;
}

void SessionBridge::adoptRecorder(ecuio_recorder_t* recorder)
{
    m_recorder = recorder;
    m_recordingFolder =
        m_failRecorderFolderForTesting ? QString() : folderOf(recorder);
    if (m_markCount != 0) {
        m_markCount = 0;
        emit markCountChanged();
    }
    emit recordingStateChanged();
}

bool SessionBridge::startLog()
{
    // The activity log's line, and the Dashboard header's notice
    const auto refuse = [this](const QString& why) {
        const QString line = QStringLiteral("Log not started: %1").arg(why);
        setLogNotice(line);
        emit logMessage(line);
        return false;
    };
    if (m_logActive) {
        return refuse(QStringLiteral("a log is running already"));
    }
    if (m_session == nullptr || !m_connected.load(std::memory_order_acquire)) {
        return refuse(QStringLiteral("not connected"));
    }
    // One recorder a session: Record sessions' is attached from the start
    if (m_recorder != nullptr) {
        return refuse(QStringLiteral("Recording this session already"));
    }
    if (!m_recordReplays && m_currentUri.startsWith(QLatin1String("replay://"),
                                                    Qt::CaseInsensitive)) {
        return refuse(QStringLiteral("a replay plays a recording already"));
    }
    const std::vector<uint16_t> ids = m_signalModel->plottedIds();
    if (ids.empty()) {
        return refuse(QStringLiteral("no signal is ticked on Live"));
    }
    // The focus first, so one core refuses (it would leave nothing to
    // ask) opens no folder
    const ecuio_status_t rc = ecuio_session_set_focus_with(
        m_session, ids.data(), static_cast<uint32_t>(ids.size()),
        static_cast<uint32_t>(m_minSweepMs), 0);
    if (rc != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        return refuse(detail.isEmpty() ? failureText(rc) : detail);
    }
    QString why;
    ecuio_recorder_t* recorder = openRecorder(
        m_session, m_currentUri, QStringLiteral("Log not started"), &why);
    if (recorder == nullptr) {
        (void)ecuio_session_set_focus(m_session, nullptr, 0);
        // openRecorder wrote the activity log's line
        setLogNotice(QStringLiteral("Log not started: %1").arg(why));
        return false;
    }
    setLogNotice(QString());
    m_logActive = true;
    m_logClock.restart();
    m_logElapsedMs = 0;
    m_logBytes = 0;
    adoptRecorder(recorder);
    m_logTimer->start();
    emit logStateChanged();
    emit logProgressChanged();
    /* A session that asks nothing takes the minimum and changes nothing */
    ecuio_session_info_t info{};
    const bool asks = ecuio_session_get_info(m_session, &info) == ECUIO_OK &&
                      (info.flags & ECUIO_SESSION_INFO_REQUESTS) != 0;
    const QString pace = m_minSweepMs > 0 && asks
                             ? QStringLiteral(", at least %1 ms between sweeps")
                                   .arg(m_minSweepMs)
                             : QString();
    emit logMessage(QStringLiteral("Log started: %1 (%2%3)")
                        .arg(recordingFolder(),
                             signalCountText(static_cast<int>(ids.size())),
                             pace));
    return true;
}

void SessionBridge::stopLog()
{
    // A teardown under way ends the log itself, once the session is gone
    if (!m_logActive || m_disconnecting) {
        return;
    }
    emit logMessage(QStringLiteral("Log stopped: %1").arg(recordingFolder()));
    stopRecording(QStringLiteral("Stop log"), true);
}

qint64 SessionBridge::logElapsedMs() const
{
    return m_logActive ? m_logClock.elapsed() : m_logElapsedMs;
}

void SessionBridge::countLogBytes()
{
    // QDir("") would list the working directory
    if (m_recordingFolder.isEmpty()) {
        m_logBytes = 0;
        return;
    }
    qint64 bytes = 0;
    const QFileInfoList files =
        QDir(m_recordingFolder).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo& f : files) {
        bytes += f.size();
    }
    m_logBytes = bytes;
}

void SessionBridge::stopRecording(const QString& why, bool byUser,
                                  const QString& told)
{
    if (m_recorder == nullptr) {
        return;
    }
    // The frames the Log page skipped are in the recording all the same
    const int skipped = m_droppedFrames.load(std::memory_order_relaxed);
    if (skipped > 0) {
        (void)ecuio_recorder_note(
            m_recorder,
            QStringLiteral(
                "the Log page skipped %1 frames; the recording has them")
                .arg(skipped)
                .toUtf8()
                .constData());
    }
    ecuio_recorder_t* recorder = m_recorder;
    m_recorder = nullptr;
    (void)ecuio_recorder_close(recorder, why.toUtf8().constData());
    emit recordingStateChanged();
    if (!m_logActive) {
        return;
    }
    // A session still running asks its whole set again; one torn down
    // took its focus with it
    if (m_session != nullptr) {
        (void)ecuio_session_set_focus(m_session, nullptr, 0);
    }
    m_logTimer->stop();
    m_logElapsedMs = m_logClock.elapsed();
    m_logActive = false;
    countLogBytes(); // closed: the folder's final size
    // An end the user didn't ask for is told on the Dashboard; Stop
    // log's isn't, the viewer opens on it
    if (!byUser) {
        setLogNotice(told.isEmpty() ? QStringLiteral("Log ended: %1").arg(why)
                                    : told);
    }
    emit logStateChanged();
    emit logProgressChanged();
    emit logStopped(m_recordingFolder, byUser);
}

void SessionBridge::setLogNotice(const QString& text)
{
    if (m_logNotice == text) {
        return;
    }
    m_logNotice = text;
    emit logNoticeChanged();
}

bool SessionBridge::mark(const QString& label)
{
    if (m_recorder == nullptr) {
        return false;
    }
    const QString text = label.trimmed().isEmpty()
                             ? QStringLiteral("Mark %1").arg(m_markCount + 1)
                             : label.trimmed();
    if (ecuio_recorder_mark(m_recorder, text.toUtf8().constData()) !=
        ECUIO_OK) {
        return false;
    }
    ++m_markCount;
    emit markCountChanged();
    // markers.csv holds it: the activity log only, not a note as well
    logUnnoted(QStringLiteral("Marked: %1").arg(text));
    return true;
}

void SessionBridge::pollRecording()
{
    ecuio_recorder_state_t st{};
    if (m_recorder == nullptr ||
        ecuio_recorder_get_state(m_recorder, &st) != ECUIO_OK ||
        st.state != ECUIO_RECORDING_FAILED) {
        return;
    }
    const QString reason = QString::fromUtf8(st.reason);
    const bool log = m_logActive;
    stopRecording(QStringLiteral("a write failed"));
    emit logMessage(QStringLiteral("Recording stopped: %1").arg(reason));
    // The Dashboard's notice gets the recorder's own words (the disk is
    // full), not the recording's end note
    if (log) {
        setLogNotice(QStringLiteral("Log ended: %1").arg(reason));
    }
}

void SessionBridge::logUnnoted(const QString& line)
{
    m_noting = false;
    emit logMessage(line);
    m_noting = true;
}

void SessionBridge::setLogPaused(bool paused)
{
    if (m_logPaused == paused) return;
    m_logPaused = paused;
    emit logPausedChanged();
}

void SessionBridge::clearFrames()
{
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    emit framesReset();
}

void SessionBridge::onFrame(const ecuio_frame_t* frame, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (!frame || !self) return;
    if (!self->m_accepting.load(std::memory_order_acquire)) return;
    self->m_framesDelivered.fetch_add(1, std::memory_order_relaxed);
    if (frame->flags & ECUIO_FRAME_FLAG_OVERFLOW) {
        self->m_overflowFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Backpressure: bound the QUEUED backlog, not the lifetime count.
    if (self->m_inFlightFrames.load(std::memory_order_relaxed) >= MaxInFlightFrames) {
        self->m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        // Report drops from here too: handleFrameInternal doesn't run while
        // the backlog is saturated, which is exactly when drops happen.
        // m_dropNotifyPending limits this to one queued notification.
        bool expected = false;
        if (self->m_dropNotifyPending.compare_exchange_strong(expected, true)) {
            QMetaObject::invokeMethod(self, [self]() {
                // Clear the flag first: drops arriving meanwhile may queue a
                // fresh notification that reads a newer value; reset-after-read
                // could suppress a drop report.
                self->m_dropNotifyPending.store(false, std::memory_order_release);
                self->notifyDropped();
            }, Qt::QueuedConnection);
        }
        return;
    }

    // ecuio_frame_t holds its payload inline (data[ECUIO_FRAME_DATA_MAX]),
    // so this struct copy is a deep copy: no dangling pointer is possible.
    const ecuio_frame_t copy = *frame;
    // Stamped here, as it arrives, not when the GUI thread gets to it:
    // the frame's wait for the GUI thread starts here
    const auto arrived = HostClock::now();
    self->m_inFlightFrames.fetch_add(1, std::memory_order_relaxed);

    // Lifetime contract: with `self` as the context object, the queued
    // lambda is dropped if the bridge is destroyed before it runs. Don't
    // change it to a context-free postEvent without revisiting the
    // destruction order.
    QMetaObject::invokeMethod(self, [self, copy, arrived]() {
        self->m_inFlightFrames.fetch_sub(1, std::memory_order_relaxed);
        self->handleFrameInternal(&copy, arrived);
    }, Qt::QueuedConnection);
}

void SessionBridge::handleFrameInternal(const ecuio_frame_t* frame,
                                        HostClock::time_point arrived)
{
    if (!m_connected.load(std::memory_order_acquire)) return;

    // Defensive clamp: a misbehaving transport must never make us read past
    // the inline buffer, even if data_length exceeds ECUIO_FRAME_DATA_MAX.
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                         ? frame->data_length
                         : static_cast<uint16_t>(sizeof(frame->data));

    m_frameLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                           HostClock::now() - arrived)
                           .count());

    // Paused: counted, but kept out of the frame log. Its values come
    // from core's worker through onValues, paused or not (ADR-0024).
    if (!m_logPaused) {
        m_frameLog->appendFrame(*frame, len);
    }
    ++m_currentFrameCount;

    // Counted here, reported on the tick (notifyFrameCounts): a signal
    // per frame would re-evaluate every binding on them per frame
    ++m_guiNotifyCount;
}

// The frame counters reach QML at the coalesce tick's rate, and at
// connect, disconnect and clear, never per frame.
void SessionBridge::notifyFrameCounts()
{
    if (m_currentFrameCount != m_reportedFrameCount) {
        m_reportedFrameCount = m_currentFrameCount;
        emit frameCountChanged();
    }
    if (m_guiNotifyCount != m_reportedNotifyCount) {
        m_reportedNotifyCount = m_guiNotifyCount;
        emit notifyCountChanged(m_guiNotifyCount);
    }
    const int overflow = m_overflowFrames.load(std::memory_order_relaxed);
    if (overflow != m_reportedOverflow) {
        if (m_reportedOverflow == 0) {
            emit logMessage(QStringLiteral(
                "The adapter's buffer overflowed: frames were lost"));
        }
        m_reportedOverflow = overflow;
        emit overflowFramesChanged();
    }
}

void SessionBridge::notifyDropped()
{
    const int dropped = m_droppedFrames.load(std::memory_order_relaxed);
    if (dropped != m_lastReportedDropped) {
        m_lastReportedDropped = dropped;
        emit droppedFramesChanged(dropped);
    }
}

void SessionBridge::forceFrameCountForTesting(int count)
{
    if (count < 0) {
        return;
    }
    m_currentFrameCount = count;
    notifyFrameCounts();   // keep the QML binding in sync
}

void SessionBridge::dispatchFrameForTesting(const ecuio_frame_t& frame)
{
    onFrame(&frame, this);
}

void SessionBridge::dispatchValuesForTesting(const ecuio_value_batch_t& batch)
{
    onValues(&batch, this);
}

// ---- Decoded values from core's worker (ADR-0024) ----

void SessionBridge::ValueStaging::prepare(size_t defs, size_t samplesMax,
                                          size_t textMax)
{
    samples.clear();
    samples.reserve(samplesMax);
    text.clear();
    text.reserve(textMax);
    newest.assign(defs, 0.0);
    lowest.assign(defs, 0.0);
    highest.assign(defs, 0.0);
    newestUs.assign(defs, 0);
    lowestUs.assign(defs, 0);
    highestUs.assign(defs, 0);
    newestText.assign(defs, -1);
    has.assign(defs, 0);
    sampleLimit = samplesMax;
    textLimit = textMax;
    dropped = 0;
}

// Keeps a numeric signal's window (newest, lowest, highest, each at its
// frame's time) whatever the limits; a sample past one is counted and
// goes untimed. Grows nothing: prepare reserved the limits.
void SessionBridge::ValueStaging::stage(uint32_t def,
                                        HostClock::time_point arrived,
                                        uint64_t frameUs,
                                        const ecuio_value_t& v)
{
    // By kind: an enum's value carries its label as text too, and is
    // still a number (ADR-0051)
    const bool isText = v.kind == ECUIO_VALUE_TEXT || v.kind == ECUIO_VALUE_HEX;
    if (!isText) {
        if (!has[def] || v.value < lowest[def]) {
            lowest[def] = v.value;
            lowestUs[def] = frameUs;
        }
        if (!has[def] || v.value > highest[def]) {
            highest[def] = v.value;
            highestUs[def] = frameUs;
        }
        newest[def] = v.value;
        newestUs[def] = frameUs;
        has[def] = 1;
    }
    if (samples.size() >= sampleLimit ||
        (isText && text.size() + v.text_len > textLimit)) {
        ++dropped;
        return;
    }
    Sample s;
    s.arrived = arrived;
    s.def = def;
    s.isText = isText;
    s.value = v.value;
    if (isText) {
        s.textAt = static_cast<uint32_t>(text.size());
        s.textLen = v.text_len;
        text.insert(text.end(), v.text, v.text + v.text_len);
        newestText[def] = static_cast<int>(samples.size());
        has[def] = 1;
    }
    samples.push_back(s);
}

void SessionBridge::ValueStaging::reset()
{
    samples.clear();
    text.clear();
    std::fill(newestText.begin(), newestText.end(), -1);
    std::fill(has.begin(), has.end(), 0);
    dropped = 0;
}

void SessionBridge::prepareValueStaging(const ecuio_signal_set_t* set)
{
    const bool decodes = set != nullptr && set->defs != nullptr && set->count > 0;
    const size_t defs = decodes ? set->count : 0;
    std::lock_guard<std::mutex> lock(m_valuesMutex);
    m_valueSet = decodes ? set : nullptr;
    m_defIndexOfId.clear();
    for (size_t i = 0; i < defs; ++i) {
        m_defIndexOfId.insert(set->defs[i].id, static_cast<int>(i));
    }
    for (ValueStaging& staging : m_valueStaging) {
        staging.prepare(defs, decodes ? static_cast<size_t>(m_valueSampleLimit) : 0,
                        decodes ? static_cast<size_t>(ValueTextLimit) : 0);
    }
    m_activeStaging = 0;
    m_valuesDropped = 0;
}

void SessionBridge::onValues(const ecuio_value_batch_t* batch, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (!batch || !self) return;
    if (!self->m_accepting.load(std::memory_order_acquire)) return;
    const auto arrived = HostClock::now();
    std::lock_guard<std::mutex> lock(self->m_valuesMutex);
    ValueStaging& staging = self->m_valueStaging[self->m_activeStaging];
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        const int def = self->m_defIndexOfId.value(v.signal_id, -1);
        if (def < 0 || static_cast<size_t>(def) >= staging.has.size()) {
            continue;
        }
        staging.stage(static_cast<uint32_t>(def), arrived, batch->timestamp_us, v);
    }
}

void SessionBridge::drainValues()
{
    if (m_valueSet == nullptr) return;
    ValueStaging* full = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_valuesMutex);
        full = &m_valueStaging[m_activeStaging];
        m_activeStaging ^= 1;
    }
    // The worker writes the other one now, so this one is the GUI
    // thread's until the next swap. Its values reach the model with this
    // drain, so each one's wait ends here.
    const auto drained = HostClock::now();
    m_valuesStaged += full->samples.size() + full->dropped;
    for (const ValueStaging::Sample& s : full->samples) {
        m_valueLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                               drained - s.arrived)
                               .count());
    }
    for (size_t i = 0; i < full->has.size(); ++i) {
        if (!full->has[i]) continue;
        const uint16_t id = m_valueSet->defs[i].id;
        const int at = full->newestText[i];
        if (at >= 0) {
            const ValueStaging::Sample& s = full->samples[static_cast<size_t>(at)];
            m_stagingTexts.emplace_back(
                id, QString::fromLatin1(full->text.data() + s.textAt,
                                        static_cast<qsizetype>(s.textLen)));
        } else {
            SignalListModel::WindowValue w;
            w.id = id;
            w.last = full->newest[i];
            w.lastMs = static_cast<qint64>(full->newestUs[i] / 1000);
            w.min = full->lowest[i];
            w.minMs = static_cast<qint64>(full->lowestUs[i] / 1000);
            w.max = full->highest[i];
            w.maxMs = static_cast<qint64>(full->highestUs[i] / 1000);
            m_stagingBuffer.push_back(w);
        }
    }
    // Past the limit a value goes untimed; the Delivery line counts it
    m_valuesDropped += full->dropped;
    full->reset();
}

// The tick's half: the window's values into the model, one entry per
// signal, whatever the frame rate (ADR-0024 section 7)
void SessionBridge::flushValues()
{
    drainValues();
    if (m_stagingBuffer.empty() && m_stagingTexts.empty()) return;
    if (!m_stagingBuffer.empty()) {
        m_signalModel->updateWindow(m_stagingBuffer);
        m_stagingBuffer.clear();
    }
    if (!m_stagingTexts.empty()) {
        m_signalModel->updateTexts(m_stagingTexts);
        m_stagingTexts.clear();
    }
    ++m_signalBatchCount;
    emit signalBatchChanged();
}

SessionBridge::DeliveryStats SessionBridge::deliveryStats() const
{
    DeliveryStats s;
    s.frames = m_framesDelivered.load(std::memory_order_relaxed);
    s.dropped = static_cast<quint64>(m_droppedFrames.load(std::memory_order_relaxed));
    const int queued = m_inFlightFrames.load(std::memory_order_relaxed);
    s.queuedAtEnd = queued > 0 ? static_cast<quint64>(queued) : 0;
    s.values = m_valuesStaged;
    s.valuesMissed = m_valuesDropped;
    s.frameToGui = m_frameLatency;
    s.valueToModel = m_valueLatency;
    return s;
}

namespace {

// A latency for the activity log: us below a millisecond, then ms with
// one decimal below 10 ms, then whole ms
QString latencyText(uint64_t us)
{
    if (us < 1000) return QStringLiteral("%1 µs").arg(us);
    if (us < 10000) {
        return QStringLiteral("%1 ms").arg(static_cast<double>(us) / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 ms").arg((us + 500) / 1000);
}

QString quantilesText(const LatencyStats& l)
{
    return QStringLiteral("median %1, p99 %2, max %3")
        .arg(latencyText(l.quantileUs(0.5)), latencyText(l.quantileUs(0.99)),
             latencyText(l.maxUs()));
}

} // namespace

// "Delivery: 7074 frames, none dropped; to the GUI thread in median
// 31 us, p99 412 us, max 1.2 ms; 28296 values, to the model in median
// 51 ms, p99 99 ms, max 101 ms". A quantile is the top of its bucket,
// at most 12.5 % above the true value (LatencyStats).
QString SessionBridge::deliveryText(const DeliveryStats& s)
{
    if (s.frames == 0) {
        return QStringLiteral("Delivery: no frames");
    }
    QString line = QStringLiteral("Delivery: %1 frames, ").arg(s.frames);
    if (s.dropped == 0) {
        line += QStringLiteral("none dropped");
    } else {
        line += QStringLiteral("%1 dropped at the in-flight cap (%2 %)")
                    .arg(s.dropped)
                    .arg(100.0 * static_cast<double>(s.dropped) / static_cast<double>(s.frames),
                         0, 'f', 1);
    }
    if (s.queuedAtEnd > 0) {
        line += QStringLiteral(", %1 still queued at the end").arg(s.queuedAtEnd);
    }
    if (s.frameToGui.count() > 0) {
        line += QStringLiteral("; to the GUI thread in ") + quantilesText(s.frameToGui);
    }
    if (s.values > 0) {
        line += QStringLiteral("; %1 values").arg(s.values);
        if (s.valueToModel.count() > 0) {
            line += QStringLiteral(", to the model in ") + quantilesText(s.valueToModel);
        }
        if (s.valuesMissed > 0) {
            line += QStringLiteral(" (%1 past the staging's limit, not timed)")
                        .arg(s.valuesMissed);
        }
    }
    return line;
}

// "Reply timing: 23 requests, 4512 answered, none refused, 2 timed out;
// latency P50 4.2 ms to 5.3 ms; intervals on the adapter's clock; ISO-TP:
// 640 messages, 40 segmented, none dropped, none unusable". Each
// request's figures are in the recording's latency.csv.
QString
SessionBridge::timingText(const ecuio_session_timing_t& t,
                          const std::vector<ecuio_request_timing_t>& rows)
{
    uint32_t asked = 0;
    quint64 answered = 0;
    quint64 refused = 0;
    quint64 timeouts = 0;
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (const ecuio_request_timing_t& r : rows) {
        if (r.answered + r.refused + r.timeouts == 0) {
            continue;
        }
        ++asked;
        answered += r.answered;
        refused += r.refused;
        timeouts += r.timeouts;
        if (r.answered + r.refused > 0) {
            low = std::min(low, r.latency_p50_us);
            high = std::max(high, r.latency_p50_us);
        }
    }
    if (asked == 0) {
        return {};
    }
    const auto count = [](quint64 n) {
        return n == 0 ? QStringLiteral("none") : QString::number(n);
    };
    QString line =
        QStringLiteral(
            "Reply timing: %1 requests, %2 answered, %3 refused, %4 timed out")
            .arg(asked)
            .arg(answered)
            .arg(count(refused), count(timeouts));
    if (low != UINT32_MAX) {
        line += low == high
                    ? QStringLiteral("; latency P50 %1").arg(latencyText(low))
                    : QStringLiteral("; latency P50 %1 to %2")
                          .arg(latencyText(low), latencyText(high));
    }
    switch (t.interval_clock) {
    case ECUIO_TIMING_CLOCK_ADAPTER:
        line += QStringLiteral("; intervals on the adapter's clock");
        break;
    case ECUIO_TIMING_CLOCK_KERNEL:
        line += QStringLiteral("; intervals on the kernel's receive times");
        break;
    case ECUIO_TIMING_CLOCK_HOST:
        line += QStringLiteral("; intervals on the host's clock");
        break;
    default:
        break;
    }
    if (t.flags & ECUIO_TIMING_ISOTP) {
        line +=
            QStringLiteral(
                "; ISO-TP: %1 messages, %2 segmented, %3 dropped, %4 unusable")
                .arg(t.isotp_completed)
                .arg(t.isotp_first_frames)
                .arg(count(t.isotp_dropped), count(t.isotp_malformed));
    }
    return line;
}

void SessionBridge::pollLinkHealth()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    (void)ecuio_session_get_error_state(m_session, &last_error, &episodes);
    if (last_error == m_linkErrorCode && episodes == m_errorEpisodes) {
        return;   // silent on unchanged state: the atomics only move
                  // on transitions, so this is not a poll storm
    }
    const int32_t prev = m_linkErrorCode;
    m_linkErrorCode = last_error;
    m_errorEpisodes = episodes;

    /* Transitions land in the activity log with no QML work, which turns
     * "connected with frozen values" into a named fault there. */
    if (prev != ECUIO_OK && last_error == ECUIO_OK) {
        logUnnoted(QStringLiteral("Link: recovered"));
    } else if (last_error == ECUIO_ERR_VEHICLE_TIMEOUT) {
        logUnnoted(QStringLiteral("Link: vehicle not responding"));
    } else if (last_error == ECUIO_ERR_REQUEST_FAILED) {
        logUnnoted(QStringLiteral("Link: requests failing (%1 episode(s))")
                       .arg(m_errorEpisodes));
    } else if (last_error == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
        logUnnoted(QStringLiteral("Link: adapter fault (%1 episode(s))")
                       .arg(m_errorEpisodes));
    } else if (last_error == ECUIO_ERR_RECORDING_CHANGED) {
        // Stopped, not finished: pollReplayEnd stays quiet. The worker
        // counted every frame it sent before it published the code.
        m_replayEndLogged = true;
        emit logMessage(QStringLiteral("Replay stopped after %1 frames: the recording "
                                       "changed after it was opened")
                            .arg(m_framesDelivered.load(std::memory_order_relaxed)));
    }
    emit linkStateChanged();
}

/* What each channel's ECU said it supports (ADR-0015 section 4), in the
 * activity log once per update: the probe's walk ending, and each
 * request it drops. Generation 0 is nothing learned yet. */
void SessionBridge::pollChannelSupport()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_support_t s{};
        if (ecuio_session_get_channel_support(m_session, c, &s) != ECUIO_OK) {
            continue;
        }
        if (s.generation == m_supportGeneration[c]) {
            continue;
        }
        m_supportGeneration[c] = s.generation;
        m_supportReports[c] = s;
        changed = true;
        // The session's change first (ADR-0060): it's why this report moved
        SessionWritten& w = m_sessionWritten[c];
        cli::SessionSeen seen{w.state, w.nrc, w.entries, w.lost, w.written};
        if (cli::session_due(s, seen)) {
            w = {seen.state, seen.nrc, seen.entries, seen.lost, seen.written};
            const QString line = channelSessionText(s);
            if (!line.isEmpty()) {
                emit logMessage(line);
            }
        }
        emit logMessage(channelSupportText(s));
    }
    // Live says which signals their ECU refused, and with what (ADR-0051
    // section 3): the drops a negative reply made, by the defs they read
    if (changed) {
        std::vector<SignalListModel::Refusal> refused;
        for (const cli::RefusedSignal& r :
             cli::refused_signals(ecuio_session_signal_set(m_session),
                                  m_supportReports, m_channelCount)) {
            refused.push_back({r.id, refusalText(r.request[0], r.nrc),
                               refusalCode(r.request[0], r.nrc)});
        }
        m_signalModel->applyRefused(refused);
        std::vector<std::pair<uint32_t, uint32_t>> more;
        for (const cli::RefusedMore& m :
             cli::refused_unlisted(ecuio_session_signal_set(m_session),
                                   m_supportReports, m_channelCount)) {
            more.emplace_back(m.rx_id, m.count);
        }
        const QString unnamed = refusedUnnamedText(more);
        if (unnamed != m_refusedUnnamed) {
            m_refusedUnnamed = unnamed;
            emit refusedUnnamedChanged();
        }
    }
    // Live hides the rows core judges unsupported from the reports
    // (ADR-0039 section 6)
    uint32_t count = 0;
    if (changed && ecuio_session_get_unsupported_signals(
                       m_session, m_unsupportedRead.data(),
                       static_cast<uint32_t>(m_unsupportedRead.size()),
                       &count) == ECUIO_OK) {
        count = std::min<uint32_t>(
            count, static_cast<uint32_t>(m_unsupportedRead.size()));
        m_signalModel->applyUnsupported(std::vector<uint16_t>(
            m_unsupportedRead.begin(), m_unsupportedRead.begin() + count));
    }
}

// Live hides the signals core reports its frames come without, as it
// hides an unsupported PID's (ADR-0039 section 6): the fact is core's, the
// hiding the model's
void SessionBridge::pollAbsentSignals()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_absentRead.empty()) {
        return;
    }
    uint32_t count = 0;
    if (ecuio_session_get_absent_signals(m_session, m_absentRead.data(),
                                         static_cast<uint32_t>(m_absentRead.size()),
                                         &count) != ECUIO_OK) {
        return;
    }
    count = std::min<uint32_t>(count, static_cast<uint32_t>(m_absentRead.size()));
    if (count == m_absentIds.size() &&
        std::equal(m_absentIds.begin(), m_absentIds.end(), m_absentRead.begin())) {
        return;
    }
    m_absentIds.assign(m_absentRead.begin(), m_absentRead.begin() + count);
    m_signalModel->applyAbsent(m_absentIds);
}

/* One channel's "ECU 0x…:" line: what its supported-PID bitmaps list,
 * which requests left the rotation, and why, and whether its Service 01
 * requests carry several PIDs (ADR-0039 Appendix A): packing starting or
 * ending brings a new line, which must not read like the last. Static, like
 * sessionInfoText, so test_bridge pins the wording without an adapter. */
QString SessionBridge::refusalCode(uint8_t service, uint8_t nrc)
{
    return QStringLiteral("7F %1 %2").arg(hexByte(service), hexByte(nrc));
}

QString SessionBridge::refusalText(uint8_t service, uint8_t nrc)
{
    const QString reply = refusalCode(service, nrc);
    const QString name = QString::fromLatin1(cli::nrc_name(nrc));
    return name.isEmpty() ? QStringLiteral("Refused: %1").arg(reply)
                          : QStringLiteral("Refused: %1 (%2)").arg(name, reply);
}

QString SessionBridge::refusedUnnamedText(
    const std::vector<std::pair<uint32_t, uint32_t>>& more)
{
    QStringList parts;
    for (const auto& m : more) {
        parts << QStringLiteral("ECU %1 refused %2 more request%3")
                     .arg(hexId(m.first))
                     .arg(m.second)
                     .arg(m.second == 1 ? QString() : QStringLiteral("s"));
    }
    return parts.join(QStringLiteral("; "));
}

QString SessionBridge::channelSupportText(const ecuio_channel_support_t& s)
{

    QStringList parts;
    if ((s.flags & ECUIO_SUPPORT_PROBED) != 0) {
        // core's count of the data PIDs, and the last its bitmaps cover
        if ((s.flags & ECUIO_SUPPORT_COMPLETE) != 0) {
            parts << QStringLiteral("supports %1 PIDs").arg(s.pids_supported);
        } else if (s.ranges != 0) {
            parts << QStringLiteral(
                         "supports %1 PIDs up to %2, no answer past that")
                         .arg(s.pids_supported)
                         .arg(hexId(s.pids_through));
        } else {
            parts << QStringLiteral("no answer to the supported-PID query");
        }
    }

    QString requests = QStringLiteral("%1 request%2").arg(s.requests)
                           .arg(s.requests == 1 ? QString() : QStringLiteral("s"));
    if (s.dropped == 0) {
        requests += QStringLiteral(", none dropped");
    } else {
        QStringList gone;
        const uint32_t listed = std::min<uint32_t>(s.dropped, ECUIO_SUPPORT_MAX_DROPPED);
        for (uint32_t i = 0; i < listed; ++i) {
            const ecuio_dropped_request_t& d = s.dropped_list[i];
            QStringList key;
            for (uint8_t k = 0; k < d.length && k < sizeof(d.request); ++k) {
                key << hexByte(d.request[k]);
            }
            const QString nrc = QStringLiteral("7F %1 %2").arg(hexByte(d.request[0]),
                                                               hexByte(d.nrc));
            QString why;
            switch (d.reason) {
                case ECUIO_DROP_BITMAP:
                    why = QStringLiteral("not in its supported PIDs");
                    break;
                case ECUIO_DROP_NOT_SUPPORTED:
                    why = QStringLiteral("not supported: ") + nrc;
                    break;
                case ECUIO_DROP_SERVICE:
                    why = QStringLiteral("service not supported: ") + nrc;
                    break;
                case ECUIO_DROP_ACCESS:
                    // in the extended session, "another session" is wrong
                    why = (s.session == cli::kExtendedSession
                               ? QStringLiteral("needs security access, or a "
                                                "session Havne doesn't enter: ")
                               : QStringLiteral("needs security access or "
                                                "another session: ")) +
                          nrc;
                    break;
                default:
                    why = QStringLiteral("reason %1").arg(static_cast<uint>(d.reason));
                    break;
            }
            gone << key.join(QLatin1Char(' ')) + QStringLiteral(" (") + why + QLatin1Char(')');
        }
        if (s.dropped > listed) {
            gone << QStringLiteral("and %1 more").arg(s.dropped - listed);
        }
        requests += QStringLiteral(", %1 dropped: ").arg(s.dropped) +
                    gone.join(QStringLiteral(", "));
    }
    parts << requests;

    if ((s.flags & ECUIO_SUPPORT_UNPACKED) != 0) {
        parts << QStringLiteral("one PID per request, several went unanswered");
    } else if ((s.flags & ECUIO_SUPPORT_PACKED) != 0) {
        parts << QStringLiteral("several PIDs per request");
    }

    /* The DID scan (ADR-0020): how many DIDs it covers and what each
     * answer was; found ones are polled unless the table was full. */
    if (s.scan_total > 0) {
        QString scan = QStringLiteral("scan: %1 DID%2, %3 found")
                           .arg(s.scan_total)
                           .arg(s.scan_total == 1 ? QString() : QStringLiteral("s"))
                           .arg(s.scan_found);
        if (s.scan_polled < s.scan_found) {
            scan += QStringLiteral(" (%1 polled)").arg(s.scan_polled);
        }
        scan += QStringLiteral(", %1 refused").arg(s.scan_refused);
        if (s.scan_done < s.scan_total) {
            scan += QStringLiteral(", %1 unanswered").arg(s.scan_total - s.scan_done);
        }
        parts << scan;
    }
    if (s.session_asked != 0) {
        parts << (s.session == cli::kExtendedSession
                      ? QStringLiteral("extended session")
                      : QStringLiteral("default session"));
    }
    return QStringLiteral("ECU %1: ").arg(hexId(s.rx_id)) + parts.join(QStringLiteral("; "));
}

/* A module's session change (ADR-0060), one line per state the CLI's
 * session line reports; the refusal's code by name, as Live's refusals
 * are. Static, so test_bridge pins every state's words. */
QString SessionBridge::channelSessionText(const ecuio_channel_support_t& s)
{
    const QString ecu = QStringLiteral("ECU %1: ").arg(hexId(s.rx_id));
    const QString name = QString::fromLatin1(cli::nrc_name(s.session_nrc));
    switch (s.session_asked != 0 ? s.session_state
                                 : ECUIO_SESSION_STATE_NOT_ASKED) {
    case ECUIO_SESSION_STATE_ENTERED:
        if (s.session_lost == 0) {
            return ecu +
                   QStringLiteral("reading in the extended session (50 03)");
        }
        return ecu +
               QStringLiteral("left the extended session and entered it again "
                              "(%1 %2 so far)")
                   .arg(s.session_lost)
                   .arg(s.session_lost == 1 ? QStringLiteral("time")
                                            : QStringLiteral("times"));
    case ECUIO_SESSION_STATE_REFUSED: {
        const QString code = refusalCode(0x10, s.session_nrc);
        return ecu + QStringLiteral(
                         "refused the extended session (%1): reading in the "
                         "default session")
                         .arg(name.isEmpty()
                                  ? code
                                  : name + QStringLiteral(", ") + code);
    }
    case ECUIO_SESSION_STATE_UNANSWERED:
        return ecu + QStringLiteral(
                         "no answer to 10 03: reading in the default session, "
                         "asked again when the link recovers");
    case ECUIO_SESSION_STATE_ABANDONED: {
        const QString code =
            QStringLiteral("code %1").arg(hexByte(s.session_nrc));
        return ecu +
               QStringLiteral("kept leaving the extended session (%1, %2 times "
                              "within %3 s): reading in the default session")
                   .arg(name.isEmpty() ? code
                                       : name + QStringLiteral(", ") + code)
                   .arg(ECUIO_SESSION_LOSSES_MAX)
                   .arg(ECUIO_SESSION_LOSS_WINDOW_MS / 1000);
    }
    default:
        return QString();
    }
}

QString SessionBridge::sessionReturnedText(const ecuio_channel_support_t& s)
{
    if (s.session_asked == 0 || s.session_entries == 0) {
        return QString();
    }
    return QStringLiteral("ECU %1: returning to the default session (10 01)")
        .arg(hexId(s.rx_id));
}

/* What each channel's ECU said it is (ADR-0021), in the activity log
 * once the whole list has an outcome, and again when a recovery re-asks
 * the unanswered items and they settle. One line per ECU, so the VIN
 * and the calibration IDs land in the log and in a recording's
 * events.log together. */
void SessionBridge::pollChannelIdentity()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_identity_t s{};
        if (ecuio_session_get_channel_identity(m_session, c, &s) != ECUIO_OK ||
            (s.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
            s.generation == m_identityGeneration[c]) {
            continue;
        }
        m_identityGeneration[c] = s.generation;
        emit logMessage(channelIdentityText(s));
        // The vehicle is the first channel's ECU's VIN (ADR-0027 section 3);
        // a recovery's re-asks settle on the same one and say nothing
        if (c == 0) {
            char text[ECUIO_VIN_TEXT_LEN];
            const QString vin =
                ecuio_identity_vin(&s, text, sizeof text) == ECUIO_OK
                    ? QString::fromLatin1(text)
                    : QString();
            if (!vin.isEmpty() && vin != m_vin) {
                m_vin = vin;
                // What the VIN says of the car (ADR-0043), beside the
                // identity line that holds it
                emit logMessage(
                    QStringLiteral("VIN: ") +
                    vinSummary(vin, QDate::currentDate().year() + 1));
                emit vehicleIdentified();
                // the inspection's model year (ADR-0045)
                emit diagnosticsChanged();
            }
        }
    }
}

/* Each channel's diagnostics report (ADR-0037), logged once per read
 * that completes: at connect, then once per Read again, which moves the
 * generation on. One line per ECU, so a recording's events.log keeps
 * the codes the car had when it was made. A clear (ADR-0038) moves the
 * generation with the codes unchanged, while it runs and when it ends
 * unsent or refused: that logs its own line, once per clear asked, and
 * no diagnostics line; after a 44 or no answer the read that follows
 * logs one. */
void SessionBridge::pollChannelDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = m_diagnosticsShownCount != m_channelCount;
    bool clear_ended = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) !=
            ECUIO_OK) {
            continue;
        }
        // The struct has no padding (core.h pins it); its doubles compare
        // by their bits, which is what a change test wants
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        if (memcmp(&s, &m_diagnosticsShown[c], sizeof(s)) != 0) {
            m_diagnosticsShown[c] = s;
            changed = true;
        }
        bool clear_only = (s.flags & ECUIO_DIAG_CLEARING) != 0;
        if (m_clearAsked[c] && !clear_only && s.clear != ECUIO_CLEAR_NONE) {
            m_clearAsked[c] = false;
            clear_ended = true;
            emit logMessage(channelClearText(s, m_signalModel->unitSystem()));
            if (s.clear == ECUIO_CLEAR_MOVING ||
                s.clear == ECUIO_CLEAR_NO_VEHICLE_SPEED) {
                m_clearWaiting[c] = true;
                m_clearWaitGeneration[c] = s.generation;
                m_clearWaitKmh[c] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                        ? s.clear_kmh
                                        : UINT32_MAX;
            }
            clear_only = s.clear != ECUIO_CLEAR_CLEARED &&
                         s.clear != ECUIO_CLEAR_UNANSWERED;
        }
        if ((s.flags & ECUIO_DIAG_COMPLETE) == 0 ||
            s.generation == m_diagnosticsGeneration[c]) {
            continue;
        }
        m_diagnosticsGeneration[c] = s.generation;
        if (!clear_only) {
            emit logMessage(channelDiagnosticsText(s));
        }
    }
    if (changed) {
        m_diagnosticsShownCount = m_channelCount;
        rebuildDiagnostics();
    }
    if (clear_ended) {
        emit clearMovingChanged();
    }
}

void SessionBridge::rebuildDiagnostics()
{
    m_diagnostics.clear();
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        m_diagnostics << diagnosticsEntry(m_diagnosticsShown[c],
                                          m_signalModel->unitSystem());
    }
    emit diagnosticsChanged();
}

void SessionBridge::readDiagnostics()
{
    if (m_connected.load(std::memory_order_acquire) && m_session != nullptr) {
        (void)ecuio_session_read_diagnostics(m_session);
    }
}

bool SessionBridge::clearTarget(const ecuio_channel_diagnostics_t& report)
{
    return (report.flags & ECUIO_DIAG_COMPLETE) != 0 &&
           (report.flags & ECUIO_DIAG_CLEARING) == 0;
}

bool SessionBridge::nothingStored(const ecuio_channel_diagnostics_t& r)
{
    const auto empty = [&r](uint32_t list) {
        return r.lists[list].outcome == ECUIO_DIAG_ANSWERED &&
               r.lists[list].reported == 0 && r.lists[list].held == 0;
    };
    return (r.flags & ECUIO_DIAG_READINESS) != 0 &&
           (r.flags & ECUIO_DIAG_MIL_ON) == 0 && r.dtc_count == 0 &&
           empty(ECUIO_DIAG_STORED) && empty(ECUIO_DIAG_PENDING) &&
           (r.flags & ECUIO_DIAG_FREEZE_FRAME) == 0;
}

bool SessionBridge::clearFindsNothing() const
{
    bool any = false;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (!clearTarget(m_diagnosticsShown[c])) {
            continue;
        }
        if (!nothingStored(m_diagnosticsShown[c])) {
            return false;
        }
        any = true;
    }
    return any;
}

QVariantMap SessionBridge::inspection() const
{
    uint32_t year = 0;
    ecuio_vin_info_t v{};
    const QByteArray vin = m_vin.toLatin1();
    if (!m_vin.isEmpty() &&
        ecuio_vin_decode(vin.constData(),
                         static_cast<uint32_t>(QDate::currentDate().year() + 1),
                         &v) == ECUIO_OK) {
        year = v.model_year;
    }
    return inspectionEntry(m_diagnosticsShown, m_diagnosticsShownCount, year);
}

QVariantMap
SessionBridge::inspectionEntry(const ecuio_channel_diagnostics_t* reports,
                               uint32_t count, uint32_t modelYear)
{
    QVariantMap m;
    ecuio_inspection_t i{};
    if (ecuio_diag_inspect(reports, count, modelYear, &i) != ECUIO_OK) {
        i.verdict = ECUIO_INSPECTION_UNKNOWN;
    }
    QStringList names;
    for (uint8_t id = 0; id < 16; ++id) {
        if (((i.incomplete_ids >> id) & 1u) != 0) {
            names << QString::fromLatin1(
                ecuio_monitor_short_name(id, i.diag_flags));
        }
    }
    const QString incomplete =
        (i.incomplete == 1
             ? QStringLiteral("1 monitor incomplete")
             : QStringLiteral("%1 monitors incomplete").arg(i.incomplete)) +
        QStringLiteral(" (%1)").arg(names.join(QStringLiteral(", ")));
    QString verdict;
    QString text;
    switch (i.verdict) {
    case ECUIO_INSPECTION_READY:
        verdict = QStringLiteral("ready");
        if (i.incomplete == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: every "
                               "monitor complete, the check-engine light off.");
        } else if (modelYear == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a car of any model year may have.")
                    .arg(incomplete);
        } else {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a model year %2 car may have.")
                    .arg(incomplete)
                    .arg(modelYear);
        }
        break;
    case ECUIO_INSPECTION_NOT_READY:
        verdict = QStringLiteral("not_ready");
        text =
            (modelYear == 0
                 ? QStringLiteral("Not ready for an emissions inspection: %1, "
                                  "and no model year may have more than 2.")
                       .arg(incomplete)
                 : QStringLiteral(
                       "Not ready for an emissions inspection: %1, "
                       "and a model year %2 car may have %3 at most.")
                       .arg(incomplete)
                       .arg(modelYear)
                       .arg(i.allowed)) +
            QStringLiteral(" A station would send it away to be driven until "
                           "they complete.");
        break;
    case ECUIO_INSPECTION_MIL_ON:
        verdict = QStringLiteral("mil_on");
        text = QStringLiteral("Would fail an emissions inspection: an ECU "
                              "commands the check-engine light on.");
        break;
    case ECUIO_INSPECTION_YEAR_NEEDED:
        verdict = QStringLiteral("year_needed");
        text =
            QStringLiteral("%1: ready for an emissions inspection if the car "
                           "is model year 1996-2000, not if it's 2001 or "
                           "newer, and no VIN gave its year.")
                .arg(incomplete);
        break;
    case ECUIO_INSPECTION_NOT_COVERED:
        verdict = QStringLiteral("not_covered");
        text = QStringLiteral("Model year %1: the federal inspection rule "
                              "covers 1996 and newer.")
                   .arg(modelYear);
        break;
    default:
        break;
    }
    m[QStringLiteral("verdict")] = verdict;
    m[QStringLiteral("text")] = text;
    m[QStringLiteral("rule")] = QStringLiteral(
        "By the federal rule, 40 CFR 85.2222, from the monitors and "
        "the VIN's model year. A state's program sets its own, "
        "and some are stricter.");
    return m;
}

QStringList SessionBridge::clearTargets() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (clearTarget(m_diagnosticsShown[c])) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

/* ADR-0038: what a clear erases stays on record, so the reports go to a
 * file of their own before anything is sent, whether or not the session
 * records; each ECU is then cleared at the generation that file holds,
 * so a report that moved on since is refused unsent */
QString SessionBridge::clearDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return {};
    }
    ecuio_channel_diagnostics_t reports[ECUIO_SESSION_INFO_MAX_CHANNELS];
    uint32_t channels[ECUIO_SESSION_INFO_MAX_CHANNELS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) ==
                ECUIO_OK &&
            clearTarget(s)) {
            reports[n] = s;
            channels[n] = c;
            ++n;
        }
    }
    if (n == 0) {
        emit logMessage(
            QStringLiteral("Clear not sent: no ECU has a completed read"));
        return {};
    }

    const auto refuse = [this](const QString& why) {
        emit logMessage(
            QStringLiteral("Clear not sent: the report couldn't be saved (%1)")
                .arg(why));
        return QString();
    };
    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        return refuse(QStringLiteral("no Documents folder"));
    }
    // The picked vehicle names an adapter's file only, as a recording's
    // folder (ADR-0027 section 1)
    const bool adapter = m_currentUri.startsWith(QStringLiteral("j2534://"),
                                                 Qt::CaseInsensitive) ||
                         m_currentUri.startsWith(QStringLiteral("elm327://"),
                                                 Qt::CaseInsensitive);
    const QString label = adapter ? folderLabel(m_vehicleLabel) : QString();
    const QString base =
        QStringLiteral("diagnostics-") + QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyyMMdd-HHmmss"));
    QString path;
    for (int k = 1; k <= 9 && path.isEmpty(); ++k) {
        const QString name =
            base + (k > 1 ? QStringLiteral("-%1").arg(k) : QString()) +
            (label.isEmpty() ? QString() : QLatin1Char(' ') + label) +
            QStringLiteral(".jsonl");
        if (!QFileInfo::exists(root + QLatin1Char('/') + name)) {
            path = root + QLatin1Char('/') + name;
        }
    }
    if (path.isEmpty()) {
        return refuse(
            QStringLiteral("nine files already have this second's name"));
    }
    /* No folder, no file: either way nothing is sent */
    const bool folder = QDir().mkpath(root);
    QSaveFile file(path);
    const QByteArray text = diagnosticsFileText(reports, n);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() ||
        !file.commit()) {
        return refuse(folder ? file.errorString()
                             : QStringLiteral("can't create %1")
                                   .arg(documentsRelative(root)));
    }
    const QString shown = documentsRelative(path);
    emit logMessage(
        QStringLiteral("Diagnostics saved before the clear: %1").arg(shown));
    m_clearSavedTo = shown;
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit clearMovingChanged();

    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, channels[i], reports[i].generation,
            ECUIO_CLEAR_ALLOW_RUNNING);
        if (rc == ECUIO_OK) {
            m_clearAsked[channels[i]] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(reports[i].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
        }
    }
    return shown;
}

QStringList SessionBridge::clearMoving() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearAsked[c]) {
            return {}; /* asked once all have ended */
        }
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c]) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

QString SessionBridge::clearMovingSpeed() const
{
    uint32_t kmh = UINT32_MAX;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c] && m_clearWaitKmh[c] != UINT32_MAX &&
            (kmh == UINT32_MAX || m_clearWaitKmh[c] > kmh)) {
            kmh = m_clearWaitKmh[c];
        }
    }
    if (kmh == UINT32_MAX) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(displayValue(QStringLiteral("km/h"), kmh)))
        .arg(displayUnit(QStringLiteral("km/h")));
}

void SessionBridge::clearDiagnosticsMoving()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        clearMoving().isEmpty()) {
        return;
    }
    emit logMessage(QStringLiteral("Clearing while moving; the report was "
                                   "saved before the clear: %1")
                        .arg(m_clearSavedTo));
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (!m_clearWaiting[c]) {
            continue;
        }
        m_clearWaiting[c] = false;
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, c, m_clearWaitGeneration[c],
            ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING);
        if (rc == ECUIO_OK) {
            m_clearAsked[c] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(m_diagnosticsShown[c].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
        }
    }
    emit clearMovingChanged();
}

void SessionBridge::cancelClearMoving()
{
    const QStringList ecus = clearMoving();
    if (ecus.isEmpty()) {
        return;
    }
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit logMessage(QStringLiteral("Clear while moving not sent: %1")
                        .arg(ecus.join(QStringLiteral(", "))));
    emit clearMovingChanged();
}

QByteArray
SessionBridge::diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                   uint32_t count)
{
    QByteArray out;
    char buf[8192];
    cli::Line start(buf, sizeof buf);
    start.raw("{\"type\":\"start\"");
    start.key("schema");
    start.u64(1);
    start.key("core");
    start.string(ecuio_version_string(), SIZE_MAX);
    start.ch('}');
    if (start.finish()) {
        out.append(start.data(), static_cast<qsizetype>(start.size()));
    }
    for (uint32_t i = 0; i < count; ++i) {
        cli::Line l(buf, sizeof buf);
        cli::diagnostics_line(l, reports[i]);
        if (l.finish()) {
            out.append(l.data(), static_cast<qsizetype>(l.size()));
        }
    }
    return out;
}

QVariantMap
SessionBridge::diagnosticsEntry(const ecuio_channel_diagnostics_t& s,
                                uint32_t units)
{
    const auto outcome = [](uint8_t o) {
        switch (o) {
        case ECUIO_DIAG_ANSWERED:
            return QStringLiteral("answered");
        case ECUIO_DIAG_REFUSED:
            return QStringLiteral("refused");
        case ECUIO_DIAG_UNANSWERED:
            return QStringLiteral("unanswered");
        case ECUIO_DIAG_UNSUPPORTED:
            return QStringLiteral("unsupported");
        default:
            return QStringLiteral("unasked");
        }
    };
    const auto nrc = [](uint8_t o, uint8_t service, uint8_t code) {
        return o == ECUIO_DIAG_REFUSED
                   ? QStringLiteral("7F %1 %2")
                         .arg(hexByte(service), hexByte(code))
                   : QString();
    };
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };

    QVariantMap m;
    m[QStringLiteral("ecu")] = hexId(s.rx_id);
    m[QStringLiteral("asked")] = (s.flags & ECUIO_DIAG_ASKED) != 0;
    m[QStringLiteral("complete")] = (s.flags & ECUIO_DIAG_COMPLETE) != 0;
    m[QStringLiteral("readiness")] = outcome(s.readiness);
    m[QStringLiteral("readinessNrc")] = nrc(s.readiness, 0x01, s.readiness_nrc);
    m[QStringLiteral("mil")] = (s.flags & ECUIO_DIAG_MIL_ON) != 0;
    m[QStringLiteral("dtcCount")] = static_cast<int>(s.dtc_count);
    m[QStringLiteral("compression")] = (s.flags & ECUIO_DIAG_COMPRESSION) != 0;
    m[QStringLiteral("cycle")] = outcome(s.cycle);

    static const char* const kKeys[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                        "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    QVariantList lists;
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const uint8_t held = std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
        QStringList codes;
        QStringList titles; // each code's (ADR-0044), "" with none
        for (uint8_t i = 0; i < held; ++i) {
            codes << dtc(list.codes[i]);
            titles << QString::fromLatin1(ecuio_dtc_title(list.codes[i]));
        }
        QVariantMap e;
        e[QStringLiteral("key")] = QString::fromLatin1(kKeys[l]);
        e[QStringLiteral("outcome")] = outcome(list.outcome);
        e[QStringLiteral("nrc")] = nrc(list.outcome, kServices[l], list.nrc);
        e[QStringLiteral("codes")] = codes;
        e[QStringLiteral("titles")] = titles;
        e[QStringLiteral("more")] =
            list.reported > held ? list.reported - held : 0;
        lists << e;
    }
    m[QStringLiteral("lists")] = lists;

    QVariantList monitors;
    const uint8_t n =
        std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
    for (uint8_t i = 0; i < n; ++i) {
        QVariantMap e;
        e[QStringLiteral("id")] = static_cast<int>(s.monitors[i].id);
        e[QStringLiteral("name")] =
            QString::fromLatin1(ecuio_monitor_name(s.monitors[i].id, s.flags));
        e[QStringLiteral("complete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) != 0;
        e[QStringLiteral("cycleEnabled")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_ENABLED) != 0;
        e[QStringLiteral("cycleComplete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_COMPLETE) != 0;
        monitors << e;
    }
    m[QStringLiteral("monitors")] = monitors;

    /* Each freeze-frame value by the obd2 def that decoded it (ADR-0037
     * section 4), in the user's units, as Live shows a live one */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    QVariantList values;
    const uint8_t fn = std::min<uint8_t>(s.freeze_count, ECUIO_DIAG_MAX_FREEZE);
    for (uint8_t i = 0; i < fn; ++i) {
        const ecuio_freeze_value_t& f = s.freeze_values[i];
        for (uint32_t k = 0; k < obd2->count; ++k) {
            const ecuio_signal_def_t& def = obd2->defs[k];
            if (def.id != f.def_id) {
                continue;
            }
            ecuio_display_value_t out{};
            if (ecuio_display_convert(def.unit, def.decimal_places, f.value,
                                      units, &out) != ECUIO_DISPLAY_OK) {
                out = {};
                out.value = f.value;
                qstrncpy(out.unit, def.unit, sizeof(out.unit));
                out.decimal_places = def.decimal_places;
            }
            QVariantMap e;
            e[QStringLiteral("name")] = QString::fromUtf8(def.name);
            e[QStringLiteral("value")] = out.value;
            e[QStringLiteral("unit")] = QString::fromUtf8(out.unit);
            e[QStringLiteral("decimals")] =
                static_cast<int>(out.decimal_places);
            values << e;
            break;
        }
    }
    QVariantMap freeze;
    freeze[QStringLiteral("outcome")] = outcome(s.freeze);
    freeze[QStringLiteral("nrc")] = nrc(s.freeze, 0x02, s.freeze_nrc);
    freeze[QStringLiteral("stored")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0;
    freeze[QStringLiteral("code")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                                         ? dtc(s.freeze_dtc)
                                         : QString();
    freeze[QStringLiteral("title")] =
        (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
            ? QString::fromLatin1(ecuio_dtc_title(s.freeze_dtc))
            : QString();
    freeze[QStringLiteral("values")] = values;
    m[QStringLiteral("freeze")] = freeze;

    /* The last clear (ADR-0038) and whether one runs */
    m[QStringLiteral("clearing")] = (s.flags & ECUIO_DIAG_CLEARING) != 0;
    QString clear;
    QString clearNrc;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        clear = QStringLiteral("cleared");
        break;
    case ECUIO_CLEAR_REFUSED:
        clear = QStringLiteral("refused");
        clearNrc = QStringLiteral("7F 04 %1").arg(hexByte(s.clear_nrc));
        break;
    case ECUIO_CLEAR_UNANSWERED:
        clear = QStringLiteral("unanswered");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        clear = QStringLiteral("engineRunning");
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        clear = QStringLiteral("noEngineSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        clear = QStringLiteral("reportChanged");
        break;
    case ECUIO_CLEAR_MOVING:
        clear = QStringLiteral("moving");
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        clear = QStringLiteral("noVehicleSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    default:
        clear = QStringLiteral("none");
        break;
    }
    m[QStringLiteral("clear")] = clear;
    m[QStringLiteral("clearNrc")] = clearNrc;
    m[QStringLiteral("clearRpm")] = static_cast<int>(s.clear_rpm);
    /* What the clear read: the engine turning (a 7F 04 22 then means it
     * won't clear running), and the vehicle speed in the unit system */
    m[QStringLiteral("clearRunning")] =
        (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0 && s.clear_rpm > 0;
    m[QStringLiteral("clearSpeed")] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                          ? clearSpeedText(s.clear_kmh, units)
                                          : QString();
    return m;
}

QString SessionBridge::vinKey(const QString& salt) const
{
    if (m_vin.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash((salt + m_vin).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString SessionBridge::vinSummary(const QString& vin, int latestModelYear)
{
    const QByteArray text = vin.toLatin1();
    ecuio_vin_info_t v{};
    if (vin.size() != ECUIO_VIN_LEN ||
        ecuio_vin_decode(text.constData(),
                         static_cast<uint32_t>(std::max(latestModelYear, 0)),
                         &v) != ECUIO_OK) {
        return QStringLiteral(
            "not a VIN: 49 CFR 565 takes 17 digits and capital "
            "letters, never I, O or Q");
    }
    QStringList parts;
    if (v.model_year == 0) {
        parts << QStringLiteral("no model year (position 10 is %1)")
                     .arg(vin.at(9));
    } else if ((v.flags & ECUIO_VIN_YEAR_EARLIER) != 0) {
        parts << QStringLiteral("model year %1 (not %2, which is still ahead)")
                     .arg(v.model_year)
                     .arg(v.model_year + 30);
    } else {
        parts << QStringLiteral("model year %1").arg(v.model_year);
    }
    const QString make = QString::fromLatin1(ecuio_vin_make(v.wmi));
    const QString wmi = QString::fromLatin1(v.wmi);
    parts << (make.isEmpty()
                  ? QStringLiteral("manufacturer code %1 isn't in NHTSA's list")
                        .arg(wmi)
                  : QStringLiteral("%1 (manufacturer code %2)").arg(make, wmi));
    parts << ((v.flags & ECUIO_VIN_CHECK_MATCHES) != 0
                  ? QStringLiteral("check digit matches")
                  : QStringLiteral(
                        "check digit %1 isn't the %2 it works out to (North "
                        "America requires it; other markets may not)")
                        .arg(vin.at(8))
                        .arg(QChar::fromLatin1(v.check_digit)));
    return parts.join(QStringLiteral("; "));
}

QString SessionBridge::describeVin(const QString& typed) const
{
    QString vin = typed.toUpper();
    vin.remove(QLatin1Char(' '));
    if (vin.isEmpty()) {
        return {};
    }
    return vinSummary(vin, QDate::currentDate().year() + 1);
}

QString SessionBridge::vinSource() const
{
    return QString::fromLatin1(ecuio_vin_source());
}

QString SessionBridge::describeDtc(const QString& typed) const
{
    QString text = typed.toUpper();
    text.remove(QLatin1Char(' '));
    if (text.isEmpty()) {
        return {};
    }
    const QByteArray latin = text.toLatin1();
    uint16_t code = 0;
    if (text.size() != 5 ||
        ecuio_dtc_parse(latin.constData(), &code) != ECUIO_OK) {
        return QStringLiteral("not a trouble code: a letter P, C, B or U, then "
                              "four hex digits, the first 0-3");
    }
    const QString title = QString::fromLatin1(ecuio_dtc_title(code));
    return title.isEmpty()
               ? QStringLiteral("%1: no title here (Havne names SAE's generic "
                                "powertrain and network codes; a manufacturer "
                                "defines its own)")
                     .arg(text)
               : QStringLiteral("%1: %2").arg(text, title);
}

QString SessionBridge::newSalt() const
{
    QByteArray bytes(16, '\0');
    for (char& b : bytes) {
        b = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return QString::fromLatin1(bytes.toHex());
}

void SessionBridge::setVehicleLabel(const QString& label)
{
    if (label == m_vehicleLabel) {
        return;
    }
    m_vehicleLabel = label;
    emit vehicleLabelChanged();
}

QString SessionBridge::folderLabel(const QString& label)
{
    QString out;
    for (const QChar ch : label) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') ||
            ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
            ch == QLatin1Char('.')) {
            out += ch;
        }
    }
    out = out.simplified().left(40);
    // Windows refuses a folder name ending in a dot or a space
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    return out;
}

/* One channel's "ECU 0x… identity:" line: each answered item by its
 * name and text, then the refused requests with their codes, then how
 * many went unanswered. Static, like channelSupportText, so test_bridge
 * pins the wording without an adapter. */
QString SessionBridge::channelIdentityText(const ecuio_channel_identity_t& s)
{

    QStringList parts;
    QStringList refused;
    uint32_t unanswered = 0;
    const uint32_t items = std::min<uint32_t>(s.items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = s.list[i];
        QStringList key;
        for (uint8_t k = 0; k < it.length && k < sizeof(it.request); ++k) {
            key << hexByte(it.request[k]);
        }
        const char* name = ecuio_identity_request_name(it.request, it.length);
        const QString label = (*name != '\0') ? QString::fromLatin1(name)
                                              : key.join(QLatin1Char(' '));
        switch (it.outcome) {
            case ECUIO_IDENTITY_ANSWERED: {
                const uint8_t n = std::min<uint8_t>(it.text_length,
                                                    ECUIO_IDENTITY_TEXT_LEN - 1);
                parts << label + QLatin1Char(' ') +
                             (n > 0 ? QString::fromLatin1(it.text, n)
                                    : QStringLiteral("(empty)"));
                break;
            }
            case ECUIO_IDENTITY_REFUSED:
                refused << key.join(QLatin1Char(' ')) +
                               QStringLiteral(" (7F %1 %2)").arg(hexByte(it.request[0]),
                                                                 hexByte(it.nrc));
                break;
            case ECUIO_IDENTITY_UNANSWERED:
                ++unanswered;
                break;
            default:
                break;
        }
    }
    if (!refused.isEmpty()) {
        parts << QStringLiteral("%1 refused: ").arg(refused.size()) +
                     refused.join(QStringLiteral(", "));
    }
    if (unanswered > 0) {
        parts << QStringLiteral("%1 unanswered").arg(unanswered);
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 identity: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}

/* One channel's "ECU 0x… clear:" line (ADR-0038): what became of the
 * clear its report names, a speed in the unit system the page shows */
QString SessionBridge::channelClearText(const ecuio_channel_diagnostics_t& s,
                                        uint32_t units)
{
    QString what;
    const bool rpm_read = (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0;
    const bool kmh_read = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        what = QStringLiteral("cleared");
        /* The speeds it was cleared at, when the user's word let it run */
        if (kmh_read) {
            what +=
                QStringLiteral(" (%1, %2)")
                    .arg(rpm_read ? QStringLiteral("%1 rpm").arg(s.clear_rpm)
                                  : QStringLiteral("engine speed unknown"))
                    .arg(clearSpeedText(s.clear_kmh, units));
        }
        break;
    case ECUIO_CLEAR_REFUSED:
        what = QStringLiteral("refused (7F 04 %1)").arg(hexByte(s.clear_nrc));
        /* J1979: an ECU that can't clear with the engine running answers
         * conditionsNotCorrect */
        if (s.clear_nrc == 0x22 && rpm_read && s.clear_rpm > 0) {
            what += QStringLiteral(": this ECU won't clear with the engine "
                                   "running: stop and turn it off, then try "
                                   "again");
        }
        break;
    case ECUIO_CLEAR_UNANSWERED:
        what = QStringLiteral("no answer");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        what = QStringLiteral("not sent: engine running (%1 rpm)")
                   .arg(s.clear_rpm);
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no engine speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no engine speed");
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        what = QStringLiteral("not sent: the report changed");
        break;
    case ECUIO_CLEAR_MOVING:
        what = QStringLiteral("not sent: moving (%1)")
                   .arg(clearSpeedText(s.clear_kmh, units));
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no vehicle speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no vehicle speed");
        break;
    default:
        what = QStringLiteral("not asked");
        break;
    }
    return QStringLiteral("ECU %1 clear: %2").arg(hexId(s.rx_id), what);
}

/* One channel's "ECU 0x… diagnostics:" line: the MIL, each code list,
 * readiness (the incomplete monitors by their short names) and the
 * freeze frame's code, each as answered, refused with its code or
 * unanswered; an item never asked is left out. Static, like
 * channelIdentityText, so test_bridge pins the wording without an
 * adapter. */
QString
SessionBridge::channelDiagnosticsText(const ecuio_channel_diagnostics_t& s)
{
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };
    const auto refused = [](uint8_t service, uint8_t nrc) {
        return QStringLiteral(" refused (7F %1 %2)")
            .arg(hexByte(service), hexByte(nrc));
    };
    QStringList parts;
    if (s.readiness == ECUIO_DIAG_ANSWERED) {
        parts << ((s.flags & ECUIO_DIAG_MIL_ON) != 0
                      ? QStringLiteral("MIL on")
                      : QStringLiteral("MIL off"));
    }

    static const char* const kNames[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                         "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const QString name = QString::fromLatin1(kNames[l]);
        switch (list.outcome) {
        case ECUIO_DIAG_ANSWERED: {
            QStringList codes;
            const uint8_t held =
                std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
            for (uint8_t i = 0; i < held; ++i) {
                codes << dtc(list.codes[i]);
            }
            if (codes.isEmpty()) {
                codes << QStringLiteral("none");
            }
            QString part =
                name + QLatin1Char(' ') + codes.join(QLatin1Char(' '));
            if (list.reported > held) {
                part +=
                    QStringLiteral(" and %1 more").arg(list.reported - held);
            }
            parts << part;
            break;
        }
        case ECUIO_DIAG_REFUSED:
            parts << name + refused(kServices[l], list.nrc);
            break;
        case ECUIO_DIAG_UNANSWERED:
            parts << name + QStringLiteral(" unanswered");
            break;
        default:
            break;
        }
    }

    switch (s.readiness) {
    case ECUIO_DIAG_ANSWERED: {
        QStringList incomplete;
        const uint8_t n =
            std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
        for (uint8_t i = 0; i < n; ++i) {
            if ((s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) == 0) {
                incomplete << QString::fromLatin1(
                    ecuio_monitor_short_name(s.monitors[i].id, s.flags));
            }
        }
        QString part = QStringLiteral("readiness %1 of %2 complete")
                           .arg(n - incomplete.size())
                           .arg(n);
        if (!incomplete.isEmpty()) {
            part += QStringLiteral(" (%1 incomplete)")
                        .arg(incomplete.join(QStringLiteral(", ")));
        }
        parts << part;
        break;
    }
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("readiness") + refused(0x01, s.readiness_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("readiness unanswered");
        break;
    default:
        break;
    }

    switch (s.freeze) {
    case ECUIO_DIAG_ANSWERED:
        parts << ((s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                      ? QStringLiteral("freeze frame for %1")
                            .arg(dtc(s.freeze_dtc))
                      : QStringLiteral("no freeze frame"));
        break;
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("freeze frame") + refused(0x02, s.freeze_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("freeze frame unanswered");
        break;
    default:
        break;
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 diagnostics: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}

/* The URI as the header pill and the Dashboard show it. A direct-load
 * selector (one holding a separator, as resolve_uri tells a path from a
 * vendor name) is cut to the DLL's file name, j2534://.../vendor.dll, so
 * a local folder never reaches the screen or a screenshot, and a long
 * path can't elide the DLL off the pill's end. The query stays; every
 * other URI is unchanged. Settings and the activity log keep it whole. */
QString SessionBridge::uriLabelFor(const QString& uri)
{
    // A Bluetooth adapter's stream (ADR-0028): the address is personal
    // data, and the label reaches the lock screen through the session's
    // notification, so only its last two octets stay, enough to tell two
    // adapters apart
    const QString elm = QStringLiteral("elm327://");
    if (uri.startsWith(elm)) {
        const qsizetype query = uri.indexOf(QLatin1Char('?'), elm.size());
        const qsizetype end = query < 0 ? uri.size() : query;
        const QString port = uri.mid(elm.size(), end - elm.size());
        const qsizetype colon = port.indexOf(QLatin1Char(':'));
        const QString kind = port.left(colon);
        const qsizetype kept = 6; // ":AB:CD"
        if (colon > 0 &&
            (kind == QLatin1String("bt") || kind == QLatin1String("ble")) &&
            port.size() - colon - 1 > kept) {
            return elm + kind + QLatin1Char(':') + QChar(0x2026) +
                   port.right(kept) + uri.mid(end);
        }
        return uri;
    }
    const bool replay = uri.startsWith(QStringLiteral("replay://"));
    const QString scheme =
        replay ? QStringLiteral("replay://") : QStringLiteral("j2534://");
    if (!uri.startsWith(scheme)) {
        return uri;
    }
    const qsizetype query = uri.indexOf(QLatin1Char('?'), scheme.size());
    const qsizetype end = query < 0 ? uri.size() : query;
    QString selector = uri.mid(scheme.size(), end - scheme.size());
    const auto lastSep = [](const QString& s) {
        return std::max(s.lastIndexOf(QLatin1Char('/')),
                        s.lastIndexOf(QLatin1Char('\\')));
    };
    if (replay) {
        // The recording's folder name, and its frames.csv when named
        while (selector.size() > 1 &&
               lastSep(selector) == selector.size() - 1) {
            selector.chop(1);
        }
        qsizetype sep = lastSep(selector);
        if (sep > 0 &&
            selector.mid(sep + 1).compare(QStringLiteral("frames.csv"),
                                          Qt::CaseInsensitive) == 0) {
            sep = lastSep(selector.left(sep));
        }
        if (sep < 0) {
            return uri;
        }
        return scheme + QChar(0x2026) + QLatin1Char('/') +
               selector.mid(sep + 1) + uri.mid(end);
    }
    const qsizetype sep = lastSep(selector);
    if (sep < 0) {
        return uri;
    }
    return scheme + QChar(0x2026) + QLatin1Char('/') + selector.mid(sep + 1) +
           uri.mid(end);
}

/* A replay without loop=1 stops sending at the end of its file and the
 * session stays open (ADR-0023); this says so once, when every frame the
 * file holds has been read and every one its ?faults= didn't take
 * (ADR-0041) has reached onFrame, taken or dropped */
void SessionBridge::pollReplayEnd()
{
    if (m_replayEndLogged || m_session == nullptr) {
        return;
    }
    ecuio_replay_faults_t f{};
    ecuio_session_get_replay_faults(m_session, &f);
    if (f.frames < m_replayFrames ||
        m_framesDelivered.load(std::memory_order_relaxed) + f.dropped +
                f.overflowed <
            m_replayFrames) {
        return;
    }
    m_replayEndLogged = true;
    if (f.dropped + f.overflowed + f.corrupted + f.truncated + f.stalled ==
        0) {
        emit logMessage(
            QStringLiteral("Replay finished: %1 frames; the session stays open")
                .arg(m_replayFrames));
        return;
    }
    emit logMessage(
        QStringLiteral("Replay finished: %1 frames, faults injected: %2 "
                       "dropped, %3 lost to an overflow, %4 corrupted, %5 cut "
                       "short, %6 held back; the session stays open")
            .arg(m_replayFrames)
            .arg(f.dropped)
            .arg(f.overflowed)
            .arg(f.corrupted)
            .arg(f.truncated)
            .arg(f.stalled));
}

/* A sleep while connected (ADR-0033 section 6): the machine's slept count rose
 * a second or more since the last tick. Session time counts the sleep, so
 * Live and the recording show it as a gap; this says what the gap is.
 * Smaller rises are taken in without a line */
void SessionBridge::pollSleep()
{
    const uint64_t now = sleptUs();
    const uint64_t rise = now > m_sleptUs ? now - m_sleptUs : 0;
    m_sleptUs = now;
    if (rise < 1000000u) {
        return;
    }
    logUnnoted(QStringLiteral("Asleep for %1 s: nothing was read meanwhile")
                   .arg(static_cast<double>(rise) / 1e6, 0, 'f', 1));
}

/* The published state strings, in one place; QML's Theme.linkColor and
 * Theme.linkLabel key on them. Static and session-free so test_bridge
 * can pin every mapping without a transport: linkState() itself stays
 * a connected-check + delegation. */
QString SessionBridge::linkStateForError(int32_t errorCode)
{
    switch (errorCode) {
        case ECUIO_OK:                         return QStringLiteral("ok");
        case ECUIO_ERR_VEHICLE_TIMEOUT:        return QStringLiteral("quiet");
        case ECUIO_ERR_REQUEST_FAILED:         return QStringLiteral("requests");
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return QStringLiteral("fault");
        case ECUIO_ERR_RECORDING_CHANGED:      return QStringLiteral("stopped");
        default: return QStringLiteral("degraded"); /* forward-compatible */
    }
}

/* What the session runs on, for the activity log (and so the
 * recording's events.log). Static, like linkStateForError, so
 * test_bridge pins the wording without an adapter. */
QString SessionBridge::sessionInfoText(const ecuio_session_info_t& info)
{
    const QString profile = QStringLiteral("profile ") + QString::fromUtf8(
        info.profile, static_cast<qsizetype>(
            qstrnlen(info.profile, ECUIO_SESSION_INFO_PROFILE_LEN)));
    if (info.backend == ECUIO_BACKEND_MOCK) {
        return QStringLiteral("Session: mock transport; ") + profile;
    }
    if (info.backend == ECUIO_BACKEND_REPLAY) {
        // The path is in the "Connected:" line just before
        const QString speed =
            info.replay_speed_milli == 0
                ? QStringLiteral("at full speed")
                : QStringLiteral("at %1x").arg(QString::number(
                      info.replay_speed_milli / 1000.0, 'g', 6));
        const bool loop = (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
        const bool faults =
            (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) != 0;
        // A last row cut short by a crash or a power cut (ADR-0049 section 7)
        const QString torn =
            info.replay_rows_torn != 0
                ? QStringLiteral(", its last row cut short and left out")
                : QString();
        return QStringLiteral("Session: replay of a recording, %1 frames over "
                              "%2 s %3%4%5%6; ")
                   .arg(info.replay_frames)
                   .arg(QString::number(info.replay_span_ms / 1000.0, 'f', 1),
                        speed, loop ? QStringLiteral(", looping") : QString(),
                        faults ? QStringLiteral(", with faults injected")
                               : QString(),
                        torn) +
               profile;
    }
    if (info.backend == ECUIO_BACKEND_SOCKETCAN) {
        // The interface is in the "Connected:" line just before. A real
        // controller has a bit rate (core reads it from the interface);
        // vcan has none. A profile that requests runs through ISO-TP on
        // its channels; any other only listens
        const bool listenOnly =
            (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0;
        const QString bus =
            listenOnly ? QStringLiteral(
                             "a CAN controller in listen-only mode at %1 bit/s")
                             .arg(info.baud)
            : info.baud != 0
                ? QStringLiteral("a CAN controller at %1 bit/s").arg(info.baud)
                : QStringLiteral("a virtual CAN bus");
        const QString mode =
            info.channel_count > 0
                ? (info.channel_count == 1
                       ? QStringLiteral("requesting on 1 channel")
                       : QStringLiteral("requesting on %1 channels")
                             .arg(info.channel_count))
                : QStringLiteral("listening");
        return QStringLiteral("Session: SocketCAN, %1, %2; ").arg(bus, mode) +
               profile;
    }
    const bool elm327 = info.backend == ECUIO_BACKEND_ELM327;
    if (!elm327 && info.backend != ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("Session: backend %1").arg(info.backend);
    }
    const QString versions = versionsText(info);
    const QString versionsOrNone =
        versions.isEmpty() ? QStringLiteral("no version reading") : versions;

    QStringList parts;
    if (elm327) {
        // The port is in the "Connected:" line just before
        parts << QStringLiteral("ELM327 adapter (%1)").arg(versionsOrNone);
    } else {
        const QString arch =
            (info.dll_arch == ECUIO_DLL_ARCH_X86) ? QStringLiteral("x86 DLL")
            : (info.dll_arch == ECUIO_DLL_ARCH_X64)
                ? QStringLiteral("x64 DLL")
                : QStringLiteral("DLL of unknown architecture");
        parts << arch + QStringLiteral(", through the surrogate");
    }

    QString link = (info.protocol_id == 6) ? QStringLiteral("ISO 15765")
                 : (info.protocol_id == 5) ? QStringLiteral("CAN")
                 : QStringLiteral("protocol %1").arg(info.protocol_id);
    link += QStringLiteral(" at %1 baud").arg(info.baud);
    if (info.connect_flags != 0) {
        link += QStringLiteral(", flags ") + hexId(info.connect_flags);
    }
    parts << link;

    const uint32_t channels = std::min<uint32_t>(info.channel_count,
                                                 ECUIO_SESSION_INFO_MAX_CHANNELS);
    if (channels == 0) {
        parts << profile + QStringLiteral(", no channels (passive)");
    } else {
        QStringList pairs;
        for (uint32_t i = 0; i < channels; ++i) {
            pairs << hexId(info.rx_ids[i]) + QLatin1Char('/') + hexId(info.tx_ids[i]);
        }
        parts << QStringLiteral("%1, %2 %3 (rx/tx)")
                     .arg(profile,
                          channels == 1 ? QStringLiteral("channel")
                                        : QStringLiteral("channels"),
                          pairs.join(QStringLiteral(", ")));
    }

    parts << ((info.flags & ECUIO_SESSION_INFO_VBATT)
                  ? QStringLiteral("battery %1 V").arg(info.vbatt_mv / 1000.0, 0, 'f', 1)
                  : QStringLiteral("no battery reading"));
    if (elm327) {
        // An STN with parameter 2A on and its bit 2 clear stops listening
        // at a 7F xx 78, before the answer comes (ADR-0026)
        if ((info.flags & ECUIO_SESSION_INFO_PENDING_OFF) != 0) {
            parts << QStringLiteral(
                "the adapter is set not to wait past 7F xx 78 "
                "(STN parameter 2A), so a pending reply's "
                "answer is lost");
        }
        return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
    }
    parts << versionsOrNone;
    return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
}

/* The "Session:" line's versions, which Settings also keeps for each
 * device: an ELM327's AT I answer, then STI's on an STN chip; a J2534
 * adapter's firmware, DLL and API. "" without a reading, and for the
 * backends that have no adapter. */
QString SessionBridge::versionsText(const ecuio_session_info_t& info)
{
    if ((info.flags & ECUIO_SESSION_INFO_VERSIONS) == 0) {
        return {};
    }
    const auto text = [](const char* s) {
        const QString t = QString::fromUtf8(
            s,
            static_cast<qsizetype>(qstrnlen(s, ECUIO_SESSION_INFO_TEXT_LEN)));
        return t.isEmpty() ? QStringLiteral("(none)") : t;
    };
    if (info.backend == ECUIO_BACKEND_ELM327) {
        return text(info.firmware_version);
    }
    if (info.backend == ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("firmware %1, DLL %2, API %3")
            .arg(text(info.firmware_version), text(info.dll_version),
                 text(info.api_version));
    }
    return {};
}

QString SessionBridge::linkState() const
{
    if (!m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("disconnected");
    }
    return linkStateForError(m_linkErrorCode);
}
