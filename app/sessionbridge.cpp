// File/layer: app/sessionbridge.cpp | Qt6 Core, app layer, C++17
// Testing: test_bridge (QtCore only): worker→GUI marshaling, coalescing,
//          in-flight backpressure, the frame log's ring and order, its
//          rows' payload storage (BridgeTest.FrameLogHoldsLongPayloadsOnlyWhileShown)
//          and the counters on the tick, core's values staged from the
//          worker (BridgeTest.Values*, .RecordingHoldsEveryValueCoreDecodes),
//          each window's envelope at its frames' times
//          (BridgeTest.AWindow*, .TheChartKeepsTheFixturesPeak), rows by
//          id, the signal model's roles and age, the rows an ECU's
//          bitmaps mark unsupported, hidden from Live's filtered view
//          (BridgeTest.LiveHides*),
//          refresh, link-state mapping, the activity log's session,
//          ECU, diagnostics and delivery lines (the ELM327 session line too,
//          the diagnostics line: BridgeTest.ChannelDiagnosticsText*,
//          .ConnectLogsTheMocksDiagnostics; a clear's saved report and
//          lines, ADR-0038: BridgeTest.ChannelClearText*,
//          .DiagnosticsFileTextIsTheCliLines, .AClearSavesTheReportFirst,
//          .AFailedSaveSendsNoClear, and on Windows
//          .AClearRefusedForTheEngineLogsOneLineAndNoRead and
//          .AClearAtTheFakesStoppedEngineLogsTheReadAfter;
//          a sleep's line: BridgeTest.ASleepWhileConnectedIsLoggedWithItsLength,
//          BridgeTest.SessionInfoTextNamesTheAdapter; its versions clause
//          as adapterVersions, .ConnectLogsTheAdaptersSession), the serial ports as
//          core lists them, session recording (BridgeTest.Recording*) and
//          the recent ones' replay URIs (.RecentRecordings*,
//          .ReplayUriForTakesTheRecordedProfile), display
//          units on the rows, the chart and not the recording
//          (BridgeTest.UnitSystem*), the URI label
//          (BridgeTest.UriLabelCutsADirectLoadPathToItsFileName,
//          .UriLabelKeepsABluetoothAddresssLastOctets), the
//          connect's trim (BridgeTest.ConnectTrimsSpacesAroundTheUri), the
//          vehicle's parts (ADR-0027: BridgeTest.VinFromIdentity*,
//          .NewSalt*, .FolderLabel*, .UnplottedNames*, and on Windows
//          .AConnectsVinReachesQmlOnlyAsASaltedHash and
//          .RecordingFolderNamesThePickedVehicleForAnAdapterOnly). QML
//          rendering: manual smoke.

#include "sessionbridge.h"
#include "carryover.h"
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */
#include "ecuio/version.h"
#include "ecuio/signals.h"
#include "ecuio/j2534.h"
#include "ecuio/profile.h"
#include "ecuio/serial.h"
#include "ecuio/socketcan.h"

#include <QMetaObject>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
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
    const QString status = QString::fromUtf8(ecuio_status_string(rc));
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

int FrameListModel::rowCount(const QModelIndex&) const
{
    return m_count;
}

// A view row's frame: row 0 is the newest, or the oldest
const FrameListModel::Row& FrameListModel::rowAt(int row) const
{
    const size_t age = m_newestFirst ? static_cast<size_t>(m_count - 1 - row)
                                     : static_cast<size_t>(row);   // 0 = oldest
    return m_ring[(m_oldest + age) % m_ring.size()];
}

QVariant FrameListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_count) {
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
        {ArbitrationIdRole, "arbId"}
    };
}

void FrameListModel::appendFrame(const ecuio_frame_t& frame, uint16_t len)
{
    const bool full = (m_count == MaxFrames);
    if (full) {
        // Full: the oldest row leaves first, from the end that shows it,
        // and its slot takes the new frame
        const int row = m_newestFirst ? m_count - 1 : 0;
        beginRemoveRows(QModelIndex(), row, row);
        m_oldest = (m_oldest + 1) % m_ring.size();
        --m_count;
        endRemoveRows();
    }
    const int row = m_newestFirst ? 0 : m_count;
    beginInsertRows(QModelIndex(), row, row);
    if (static_cast<size_t>(m_count) == m_ring.size()) {
        m_ring.emplace_back();   // every slot holds a row: one more, below the cap
    }
    Row& r = m_ring[(m_oldest + static_cast<size_t>(m_count)) % m_ring.size()];
    r.timestampUs = frame.timestamp_us;
    r.arbId = frame.arbitration_id;
    r.extended = (frame.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
    r.length = len;
    if (len <= InlinePayload) {
        memcpy(r.inlineData, frame.data, len);
        std::vector<uint8_t>().swap(r.spill);   // frees a long frame's block
    } else {
        r.spill.assign(frame.data, frame.data + len);
    }
    ++m_count;
    endInsertRows();
    if (!full) {
        emit countChanged(m_count);   // at the cap one row left as one came
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
    endResetModel();
    emit countChanged(0);
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
        default: return QVariant();
    }
}

QHash<int, QByteArray> SignalListModel::roleNames() const
{
    return {
        {IdRole, "id"},
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
        {HiddenRole, "hidden"}
    };
}

void SignalListModel::setSignalSet(const ecuio_signal_set_t* set)
{
    // NULL/empty is valid (the raw profile): clear the model rather
    // than keep the previous session's rows.
    const bool hadHidden = m_hiddenCount != 0;
    m_hiddenCount = 0;
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

    for (uint32_t i = 0; i < set->count; ++i) {
        Entry e = {};   // value-init zeroes the history ring, head, and count
        e.id = set->defs[i].id;
        qstrncpy(e.name, set->defs[i].name, sizeof(e.name));   // always terminates
        qstrncpy(e.unit, set->defs[i].unit, sizeof(e.unit));
        e.decimals = set->defs[i].decimal_places;
        e.isText = set->defs[i].kind == ECUIO_VALUE_TEXT ||
                   set->defs[i].kind == ECUIO_VALUE_HEX;
        e.messageId = set->defs[i].message_id;
        // A Service 01 reply's key: 41 <PID>, at the payload's start
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 && (d.mux_key >> 8) == 0x41u) {
            e.pid = static_cast<uint8_t>(d.mux_key & 0xFFu);
        }
        // A text row has nothing to plot, so it's never subscribed
        e.subscribed = !e.isText;
        if (e.subscribed) ++m_subscribedCount;
        m_rowOfId.insert(e.id, static_cast<int>(m_entries.size()));
        m_entries.push_back(e);
    }
    endResetModel();
    // A new set plots every numeric row, whatever the last one plotted
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
                         {ValueRole, WindowHighRole, WindowLowRole, HasValueRole,
                          AgeMsRole});
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

ecuio_display_value_t SignalListModel::shown(const Entry& e, double value) const
{
    ecuio_display_value_t out;
    if (ecuio_display_convert(e.unit, e.decimals, value, m_units, &out) != ECUIO_DISPLAY_OK) {
        // m_units is only ever metric or imperial, so this is unreachable;
        // show the decoded value rather than nothing
        out = {};
        out.value = value;
        qstrncpy(out.unit, e.unit, sizeof(out.unit));
        out.decimal_places = e.decimals;
    }
    return out;
}

void SignalListModel::setUnitSystem(uint32_t units)
{
    if (units == m_units) return;
    m_units = units;
    if (m_entries.empty()) return;
    emit dataChanged(index(0), index(static_cast<int>(m_entries.size()) - 1),
                     {ValueRole, WindowHighRole, WindowLowRole, UnitRole, DecimalsRole});
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

QVariantList SignalListModel::chartData() const
{
    QVariantList out;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (!e.subscribed || e.hidden || e.histCount == 0) continue;

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
        const uint32_t start = (e.histCount == HistoryCapacity)
                                ? e.histHead : 0;
        for (uint32_t i = 0; i < e.histCount; ++i) {
            const Sample& s = e.history[(start + i) % HistoryCapacity];
            samples.append(static_cast<double>(s.timestampMs));
            samples.append(now.converted ? shown(e, s.value).value : s.value);
        }
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
    e.subscribed = !e.subscribed;
    emit dataChanged(index(row), index(row), {SubscribedRole});
    if (!e.hidden) {
        m_subscribedCount += e.subscribed ? 1 : -1;
        emit subscribedCountChanged();
    }
}

QStringList SignalListModel::unplottedNames() const
{
    QStringList names;
    for (const Entry& e : m_entries) {
        if (!e.isText && !e.subscribed) {
            names << QString::fromUtf8(e.name);
        }
    }
    return names;
}

void SignalListModel::setUnplotted(const QStringList& names)
{
    bool changed = false;
    for (Entry& e : m_entries) {
        if (e.isText) {
            continue;
        }
        const bool subscribed = !names.contains(QString::fromUtf8(e.name));
        if (subscribed != e.subscribed) {
            e.subscribed = subscribed;
            if (!e.hidden) {
                m_subscribedCount += subscribed ? 1 : -1;
            }
            changed = true;
        }
    }
    if (changed && !m_entries.empty()) {
        emit dataChanged(index(0),
                         index(static_cast<int>(m_entries.size()) - 1),
                         {SubscribedRole});
        emit subscribedCountChanged();
    }
}

bool SignalListModel::pidMarkedUnsupported(const ecuio_channel_support_t& s, uint8_t pid)
{
    if (pid == 0) {
        return false;
    }
    const unsigned range = (pid - 1u) / 0x20u;
    if ((s.ranges & (1u << range)) == 0) {
        return false;
    }
    return (s.pid_bitmap[(pid - 1u) / 8u] & (0x80u >> ((pid - 1u) % 8u))) == 0;
}

void SignalListModel::applySupport(const ecuio_channel_support_t* reports, size_t count)
{
    int first = -1;
    int last = -1;
    int hidden = 0;
    int subscribed = 0;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        bool hide = false;
        for (size_t i = 0; e.pid != 0 && i < count; ++i) {
            if (reports[i].generation != 0 && reports[i].rx_id == e.messageId) {
                hide = pidMarkedUnsupported(reports[i], e.pid);
                break;
            }
        }
        if (hide != e.hidden) {
            e.hidden = hide;
            if (first < 0) first = static_cast<int>(row);
            last = static_cast<int>(row);
        }
        hidden += e.hidden ? 1 : 0;
        subscribed += (e.subscribed && !e.hidden) ? 1 : 0;
    }
    if (first < 0) {
        return;
    }
    // Live's filtered view re-judges the rows HiddenRole changed in
    emit dataChanged(index(first), index(last), {HiddenRole});
    m_hiddenCount = hidden;
    emit hiddenCountChanged();
    if (subscribed != m_subscribedCount) {
        m_subscribedCount = subscribed;
        emit subscribedCountChanged();
    }
}

// ---------------- LiveSignalModel ----------------
LiveSignalModel::LiveSignalModel(SignalListModel* source, QObject* parent)
    : QSortFilterProxyModel(parent)
{
    // A HiddenRole change re-filters its rows (dynamic filtering, the default)
    setFilterRole(SignalListModel::HiddenRole);
    setSourceModel(source);
}

bool LiveSignalModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex i = sourceModel()->index(sourceRow, 0, sourceParent);
    return !sourceModel()->data(i, SignalListModel::HiddenRole).toBool();
}

// ---------------- SessionBridge ----------------
SessionBridge::SessionBridge(QObject* parent)
    : QObject(parent)
    , m_frameLog(new FrameListModel(this))
    , m_coalesceTimer(new QTimer(this))
    , m_signalModel(new SignalListModel(this))
    , m_liveSignalModel(new LiveSignalModel(m_signalModel, this))
{
    (void)ecuio_init();   // optional and always ECUIO_OK (core.h), paired below

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        notifyFrameCounts();
        pollLinkHealth();   // every tick while connected: transitions only
        pollChannelSupport();
        pollChannelIdentity();
        pollChannelDiagnostics();
        pollReplayEnd();
        pollSleep();
        // Must fire even when nothing arrived this window: dimming exists
        // for rows that STOPPED updating. 100 ms is well
        // inside the 3 s dim threshold and the span emission is one
        // signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        flushValues();
        // On disk within a tick, so a crash loses at most 100 ms
        m_recorder.flush();
        if (m_recorder.active() && !m_recorder.ok()) {
            const QString folder = recordingFolder();
            stopRecording(QStringLiteral("a write failed"));
            emit logMessage(QStringLiteral("Recording stopped: can't write to %1").arg(folder));
        }
    });

    // The activity lines, link transitions included, go into the
    // recording's events.log too
    connect(this, &SessionBridge::logMessage, this,
            [this](const QString& line) { m_recorder.event(line); });
}


SessionBridge::~SessionBridge()
{
    // Async connect worker first: it calls core and posts a queued
    // completion against `this`. Join before tearing the session or
    // shutting core down. Worst case bounded by the surrogate handshake
    // timeout (~8 s) at app exit with a wedged vendor: acceptable for
    // shutdown, unacceptable for the UI thread mid-session (which is
    // why connect is async at all).
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
    if (m_recorder.active()) {
        m_recorder.stop(recordingSummary(QStringLiteral("the app closed")));
    }
    ecuio_shutdown();
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
        // can't decode a recording wrongly, and so is an unknown source's
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
        for (const QString& did : dids)
            uri += QLatin1Char('&') + did;
    }
    return uri;
}

QVariantList SessionBridge::recentRecordings(int limit) const
{
    QVariantList out;
    if (limit <= 0)
        return out;
    // "<yyyyMMdd-HHmmss>[-n][ <label>]" (SessionRecorder::start): by name,
    // last first, is newest first
    static const QRegularExpression kName(
        QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})-(\\d{2})(\\d{2})(\\d{2})(?:-"
                       "\\d+)?(?: (.+))?$"));
    // Both roots' folders, newest first across them (the names start with
    // the stamp)
    QFileInfoList folders;
    for (const QString& root : {recordingsRoot(), earlierRecordingsRoot()}) {
        if (!root.isEmpty())
            folders +=
                QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    }
    std::sort(folders.begin(), folders.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.fileName() > b.fileName();
              });
    for (const QFileInfo& info : folders) {
        if (out.size() >= limit)
            break;
        const QString folder = info.absoluteFilePath();
        if (!QFileInfo::exists(folder + QStringLiteral("/frames.csv")))
            continue;

        // The header's "uri" line, among events.log's first few
        QString recorded;
        QFile events(folder + QStringLiteral("/events.log"));
        if (events.open(QIODevice::ReadOnly | QIODevice::Text)) {
            for (int i = 0; i < 12 && !events.atEnd(); ++i) {
                const QString line =
                    QString::fromUtf8(events.readLine()).trimmed();
                if (line.startsWith(QLatin1String("uri "))) {
                    recorded = line.mid(4).trimmed();
                    break;
                }
            }
        }

        const QRegularExpressionMatch m = kName.match(info.fileName());
        QString title = info.fileName();
        if (m.hasMatch()) {
            title = QStringLiteral("%1-%2-%3 %4:%5:%6")
                        .arg(m.captured(1), m.captured(2), m.captured(3),
                             m.captured(4), m.captured(5), m.captured(6));
            if (m.hasCaptured(7))
                title += QStringLiteral(" · ") + m.captured(7);
        }
        const QString uri = replayUriFor(folder, recorded);
        QVariantMap row;
        row["title"] = title;
        row["uri"] = uri;
        row["source"] = uriLabelFor(recorded);
        // After the query's "?profile=", the last ? in it
        row["profile"] = uri.mid(uri.lastIndexOf(QLatin1Char('?')) + 9)
                             .section(QLatin1Char('&'), 0, 0);
        out.append(row);
    }
    return out;
}

// Pure-core half: safe on ANY thread. Creates and registers, the slow
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
QString SessionBridge::startSession(ecuio_session_t* session)
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
    m_accepting.store(true, std::memory_order_release);
    const ecuio_status_t rc = ecuio_session_start(session);
    if (rc != ECUIO_OK) {
        m_accepting.store(false, std::memory_order_release);
        const QString err = failureText(rc);
        ecuio_unregister_frame_callback(session, &SessionBridge::onFrame, this);
        ecuio_unregister_values_callback(session, &SessionBridge::onValues, this);
        ecuio_session_destroy(session);
        return err;
    }
    return {};
}

// GUI-thread half: state application after a successful connect,
// ending with the "Connected:" and "Session:" log lines. Factored out of
// connectSession so the sync and async paths apply IDENTICAL state
// (parity by construction, not by care).
void SessionBridge::applyConnectedState(ecuio_session_t* session, const QString& uri)
{
    // Before any emit: each runs handlers synchronously, and a connect one
    // of them starts must be refused, not replace this session
    m_session = session;
    m_connected.store(true, std::memory_order_release);
    // Before the queued frames run, so the recording holds every one
    startRecording(uri, ecuio_session_signal_set(session));
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
    m_signalModel->setSignalSet(ecuio_session_signal_set(session));
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
    for (uint32_t& g : m_identityGeneration) {
        g = 0;
    }
    for (uint32_t& g : m_diagnosticsGeneration) {
        g = 0;
    }
    for (bool& asked : m_clearAsked) {
        asked = false;
    }
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
    const QString uri = typed.trimmed();
    if (m_disconnecting) {
        return QStringLiteral("Disconnect in progress");
    }
    if (m_connected.load(std::memory_order_acquire) || m_connecting) {
        return {};
    }

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
        err = startSession(session);
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
    const QString uri = typed.trimmed(); // as connectSession does
    if (m_connected.load(std::memory_order_acquire) || m_connecting
        || m_disconnecting) {
        return;   // one attempt at a time, none during a teardown; the
                  // UI already reflects state
    }
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
            // JOIN FIRST, then clear the flag, then emit: assigning to a
            // joinable std::thread terminates the process, and a QML
            // handler can run SYNCHRONOUSLY inside connectingChanged.
            // connecting clears only once the session is applied, so a
            // handler always sees connecting or connected: a connect it
            // starts is refused, never a second session over this one.
            if (m_connectThread.joinable()) {
                m_connectThread.join();
            }
            ecuio_session_t* session = m_pendingSession.exchange(nullptr);
            QString err = core_err;
            if (err.isEmpty()) {
                err = startSession(session);
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
    flushValues();         // and its values, the recording's among them
    if (m_valuesDropped > 0) {
        emit logMessage(QStringLiteral("Values the recording missed: %1, past %2 "
                                       "in one 100 ms window")
                            .arg(m_valuesDropped)
                            .arg(m_valueSampleLimit));
    }
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
        ecuio_session_destroy(session);    // closes first, joining the worker

        QMetaObject::invokeMethod(this, [this, uri]() {
            // JOIN FIRST, then clear the flag, then emit, as the
            // connect completion does: a QML handler inside
            // disconnectingChanged may start the next connect.
            if (m_disconnectThread.joinable()) {
                m_disconnectThread.join();
            }
            m_disconnecting = false;
            emit disconnectingChanged();
            emit logMessage(QStringLiteral("Disconnected: %1").arg(uri));
            // Kept open until here, so it brackets the whole teardown
            stopRecording(QStringLiteral("disconnected"));
        }, Qt::QueuedConnection);
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

void SessionBridge::setUnitSystem(int units)
{
    if (units != static_cast<int>(ECUIO_UNITS_METRIC) &&
        units != static_cast<int>(ECUIO_UNITS_IMPERIAL)) {
        return;
    }
    if (units == unitSystem()) return;
    m_signalModel->setUnitSystem(static_cast<uint32_t>(units));
    emit unitSystemChanged();
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
    const QString folder = m_recorder.folder();
    if (folder.isEmpty()) return {};
    return documentsRelative(folder);
}

QUrl SessionBridge::recordingFolderUrl() const
{
    const QString folder = m_recorder.folder();
    return folder.isEmpty() ? QUrl() : QUrl::fromLocalFile(folder);
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
    if (!m_recordingsRoot.isEmpty())
        return m_recordingsRoot;
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs.isEmpty() ? QString()
                          : docs + QStringLiteral("/Havne/recordings");
}

QString SessionBridge::earlierRecordingsRoot() const
{
    if (!m_earlierRecordingsRoot.isEmpty())
        return m_earlierRecordingsRoot;
    if (!m_recordingsRoot.isEmpty())
        return QString(); // a test's own root: Documents is never read
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString root = docs + QLatin1Char('/') + earlierProductName() +
                         QStringLiteral("/recordings");
    return docs.isEmpty() || root == recordingsRoot() ? QString() : root;
}

void SessionBridge::startRecording(const QString& uri, const ecuio_signal_set_t* set)
{
    if (!m_recording) return;
    // A replay would copy the recording it plays
    if (!m_recordReplays &&
        uri.startsWith(QLatin1String("replay://"), Qt::CaseInsensitive)) {
        emit logMessage(QStringLiteral(
            "Not recording: a replay plays a recording already"));
        return;
    }

    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        emit logMessage(
            QStringLiteral("Recording failed: no Documents folder"));
        return;
    }

    QStringList names;
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        names << QString::fromUtf8(set->defs[i].name,
                                   static_cast<qsizetype>(qstrnlen(set->defs[i].name,
                                                                   sizeof set->defs[i].name)));
    }
    const QByteArray frameLogging = qgetenv("ECUIO_SUR_LOG_FRAMES");
    const QStringList header = {
        QStringLiteral("Havne session recording (ADR-0017)"),
        QStringLiteral("started  %1 (local time)")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))),
        QStringLiteral("core     %1").arg(coreVersion()),
        QStringLiteral("uri      %1").arg(uri),
        set == nullptr
            ? QStringLiteral("signals  none: the raw profile, frames only")
            : QStringLiteral("signals  %1: %2").arg(names.size()).arg(names.join(QStringLiteral(", "))),
        m_setFrameLogging
            ? QStringLiteral("surrogate frame logging  on, set by this recording")
            : frameLogging.isNull()
                ? QStringLiteral("surrogate frame logging  off")
                : QStringLiteral("surrogate frame logging  ECUIO_SUR_LOG_FRAMES=%1, set at launch")
                      .arg(QString::fromUtf8(frameLogging)),
    };

    // The picked vehicle names an adapter's session only (ADR-0027 §1):
    // a mock or a replay isn't that car
    const bool adapter =
        uri.startsWith(QStringLiteral("j2534://"), Qt::CaseInsensitive) ||
        uri.startsWith(QStringLiteral("elm327://"), Qt::CaseInsensitive);
    QString error;
    if (!m_recorder.start(root, header, &error,
                          adapter ? folderLabel(m_vehicleLabel) : QString())) {
        emit logMessage(QStringLiteral("Recording failed: %1").arg(error));
        return;
    }
    emit recordingStateChanged();
    emit logMessage(QStringLiteral("Recording: %1").arg(recordingFolder()));
}

QString SessionBridge::recordingSummary(const QString& why) const
{
    return QStringLiteral("end      %1: %2 frames, %3 values, %4 frames dropped by the bridge")
        .arg(why)
        .arg(m_recorder.frameRows())
        .arg(m_recorder.sampleRows())
        .arg(m_droppedFrames.load(std::memory_order_relaxed));
}

void SessionBridge::stopRecording(const QString& why)
{
    if (!m_recorder.active()) return;
    m_recorder.stop(recordingSummary(why));
    emit recordingStateChanged();
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
                // Clear the flag FIRST: if new drops arrive while we process,
                // a fresh notification is allowed to be queued and will read
                // a newer value. Reset-after-read could suppress a drop report.
                self->m_dropNotifyPending.store(false, std::memory_order_release);
                self->notifyDropped();
            }, Qt::QueuedConnection);
        }
        return;
    }

    // ecuio_frame_t holds its payload INLINE (data[ECUIO_FRAME_DATA_MAX]):
    // this struct copy is a deep copy of the payload; no dangling pointer
    // is possible.
    const ecuio_frame_t copy = *frame;
    // Stamped here, as it arrives, not when the GUI thread gets to it:
    // the recording's host clock
    const auto arrived = SessionRecorder::Clock::now();
    self->m_inFlightFrames.fetch_add(1, std::memory_order_relaxed);

    // Lifetime contract: invoking with `self` as context object means the
    // queued lambda is dropped if the bridge is destroyed before it runs.
    // DO NOT change to a context-free postEvent pattern without revisiting
    // destruction ordering.
    QMetaObject::invokeMethod(self, [self, copy, arrived]() {
        self->m_inFlightFrames.fetch_sub(1, std::memory_order_relaxed);
        self->handleFrameInternal(&copy, arrived);
    }, Qt::QueuedConnection);
}

void SessionBridge::handleFrameInternal(
    const ecuio_frame_t* frame, SessionRecorder::Clock::time_point arrived)
{
    if (!m_connected.load(std::memory_order_acquire)) return;

    // Defensive clamp: a misbehaving transport must never make us read past
    // the inline buffer, even if data_length exceeds ECUIO_FRAME_DATA_MAX.
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                         ? frame->data_length
                         : static_cast<uint16_t>(sizeof(frame->data));

    m_frameLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                           SessionRecorder::Clock::now() - arrived)
                           .count());

    // Recorded whether or not the log is paused
    m_recorder.frame(arrived, *frame, len);

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
// frame's time) whatever the limits; a sample past one is counted, and
// only the recording misses it. Grows nothing: prepare reserved the
// limits.
void SessionBridge::ValueStaging::stage(
    uint32_t def, SessionRecorder::Clock::time_point arrived, uint64_t frameUs,
    const ecuio_value_t& v)
{
    const bool isText = v.text != nullptr;
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
    const auto arrived = SessionRecorder::Clock::now();
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
    const auto drained = SessionRecorder::Clock::now();
    m_valuesStaged += full->samples.size() + full->dropped;
    for (const ValueStaging::Sample& s : full->samples) {
        m_valueLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                               drained - s.arrived)
                               .count());
        const ecuio_signal_def_t& def = m_valueSet->defs[s.def];
        if (s.isText) {
            m_recorder.sample(s.arrived, def,
                              QString::fromLatin1(full->text.data() + s.textAt,
                                                  static_cast<qsizetype>(s.textLen)));
        } else {
            m_recorder.sample(s.arrived, def, s.value);
        }
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
    if (full->dropped > 0) {
        if (m_valuesDropped == 0) {
            emit logMessage(QStringLiteral("Values are arriving faster than %1 in "
                                           "100 ms: the recording misses some")
                                .arg(m_valueSampleLimit));
        }
        m_valuesDropped += full->dropped;
    }
    full->reset();
}

// The tick's half: the window's values into the model, one entry per
// signal, whatever the frame rate (ADR-0024 §7)
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

// A latency for the activity log: µs below a millisecond, then ms with
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
// 31 µs, p99 412 µs, max 1.2 ms; 28296 values, to the model in median
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

    /* Transitions land in the activity log with ZERO QML work: this
     * alone turns "connected with frozen values" into a named fault
     * in the existing log surface. */
    if (prev != ECUIO_OK && last_error == ECUIO_OK) {
        emit logMessage(QStringLiteral("Link: recovered"));
    } else if (last_error == ECUIO_ERR_VEHICLE_TIMEOUT) {
        emit logMessage(QStringLiteral("Link: vehicle not responding"));
    } else if (last_error == ECUIO_ERR_REQUEST_FAILED) {
        emit logMessage(QStringLiteral("Link: requests failing (%1 episode(s))")
                            .arg(m_errorEpisodes));
    } else if (last_error == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
        emit logMessage(QStringLiteral("Link: adapter fault (%1 episode(s))")
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

/* What each channel's ECU said it supports (ADR-0015 §4), in the
 * activity log once per update: the probe's walk ending, and each
 * request it drops. Generation 0 is nothing learned yet. */
void SessionBridge::pollChannelSupport()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    ecuio_channel_support_t reports[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    bool changed = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_support_t& s = reports[c];
        if (ecuio_session_get_channel_support(m_session, c, &s) != ECUIO_OK) {
            s = ecuio_channel_support_t{};   // judges nothing
            continue;
        }
        if (s.generation == m_supportGeneration[c]) {
            continue;
        }
        m_supportGeneration[c] = s.generation;
        changed = true;
        emit logMessage(channelSupportText(s));
    }
    // Live hides the rows the reports mark unsupported (ADR-0039 §6)
    if (changed) {
        m_signalModel->applySupport(reports, m_channelCount);
    }
}

/* One channel's "ECU 0x…:" line: what its supported-PID bitmaps list,
 * and which requests left the rotation, and why. Static, like
 * sessionInfoText, so test_bridge pins the wording without an adapter. */
QString SessionBridge::channelSupportText(const ecuio_channel_support_t& s)
{

    QStringList parts;
    if ((s.flags & ECUIO_SUPPORT_PROBED) != 0) {
        /* Data PIDs: the chain's own (0x20, 0x40, ...) don't count, as
         * the first vehicle's sheet counts them */
        int pids = 0;
        for (uint32_t pid = 1; pid <= 0xFF; ++pid) {
            if (pid % 0x20 != 0 &&
                (s.pid_bitmap[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) != 0) {
                ++pids;
            }
        }
        uint32_t top = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            if ((s.ranges & (1u << i)) != 0) {
                top = 0x20u * (i + 1);
            }
        }
        if ((s.flags & ECUIO_SUPPORT_COMPLETE) != 0) {
            parts << QStringLiteral("supports %1 PIDs").arg(pids);
        } else if (s.ranges != 0) {
            parts << QStringLiteral("supports %1 PIDs up to %2, no answer past that")
                         .arg(pids).arg(hexId(top));
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
                    why = QStringLiteral("needs security access or another session: ") + nrc;
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
    return QStringLiteral("ECU %1: ").arg(hexId(s.rx_id)) + parts.join(QStringLiteral("; "));
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
        // The vehicle is the first channel's ECU's VIN (ADR-0027 §3);
        // a recovery's re-asks settle on the same one and say nothing
        if (c == 0) {
            const QString vin = vinFromIdentity(s);
            if (!vin.isEmpty() && vin != m_vin) {
                m_vin = vin;
                emit vehicleIdentified();
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
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) !=
            ECUIO_OK) {
            continue;
        }
        if (memcmp(&s, &m_diagnosticsShown[c], sizeof(s)) != 0) {
            m_diagnosticsShown[c] = s;
            changed = true;
        }
        bool clear_only = (s.flags & ECUIO_DIAG_CLEARING) != 0;
        if (m_clearAsked[c] && !clear_only && s.clear != ECUIO_CLEAR_NONE) {
            m_clearAsked[c] = false;
            emit logMessage(channelClearText(s));
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
            (s.flags & ECUIO_DIAG_COMPLETE) != 0 &&
            (s.flags & ECUIO_DIAG_CLEARING) == 0) {
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
    // folder (ADR-0027 §1)
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

    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_status_t rc = ecuio_session_clear_diagnostics(
            m_session, channels[i], reports[i].generation);
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

    static const char* const kKeys[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                        "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    QVariantList lists;
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const uint8_t held = std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
        QStringList codes;
        for (uint8_t i = 0; i < held; ++i) {
            codes << dtc(list.codes[i]);
        }
        QVariantMap e;
        e[QStringLiteral("key")] = QString::fromLatin1(kKeys[l]);
        e[QStringLiteral("outcome")] = outcome(list.outcome);
        e[QStringLiteral("nrc")] = nrc(list.outcome, kServices[l], list.nrc);
        e[QStringLiteral("codes")] = codes;
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
        monitors << e;
    }
    m[QStringLiteral("monitors")] = monitors;

    /* Each freeze-frame value by the obd2 def that decoded it (ADR-0037
     * §4), in the user's units, as Live shows a live one */
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
    default:
        clear = QStringLiteral("none");
        break;
    }
    m[QStringLiteral("clear")] = clear;
    m[QStringLiteral("clearNrc")] = clearNrc;
    m[QStringLiteral("clearRpm")] = static_cast<int>(s.clear_rpm);
    return m;
}

QString SessionBridge::vinFromIdentity(const ecuio_channel_identity_t& identity)
{
    QString fromService09;
    QString fromF190;
    const uint32_t items =
        std::min<uint32_t>(identity.items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = identity.list[i];
        if (it.outcome != ECUIO_IDENTITY_ANSWERED) {
            continue;
        }
        QString vin;
        for (uint8_t k = 0; k < it.text_length && k < ECUIO_IDENTITY_TEXT_LEN;
             ++k) {
            const QChar ch = QLatin1Char(it.text[k]);
            if (ch.isLetterOrNumber() && ch.unicode() < 0x80) {
                vin += ch.toUpper();
            }
        }
        if (vin.size() != 17) {
            continue;
        }
        if (it.length == 2 && it.request[0] == 0x09 && it.request[1] == 0x02) {
            fromService09 = vin;
        } else if (it.length == 3 && it.request[0] == 0x22 &&
                   it.request[1] == 0xF1 && it.request[2] == 0x90) {
            fromF190 = vin;
        }
    }
    return fromService09.isEmpty() ? fromF190 : fromService09;
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
 * clear its report names */
QString SessionBridge::channelClearText(const ecuio_channel_diagnostics_t& s)
{
    QString what;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        what = QStringLiteral("cleared");
        break;
    case ECUIO_CLEAR_REFUSED:
        what = QStringLiteral("refused (7F 04 %1)").arg(hexByte(s.clear_nrc));
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
 * vendor name) is cut to the DLL's file name, j2534://…/vendor.dll, so
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
 * file holds has reached onFrame, taken or dropped */
void SessionBridge::pollReplayEnd()
{
    if (m_replayEndLogged ||
        m_framesDelivered.load(std::memory_order_relaxed) < m_replayFrames) {
        return;
    }
    m_replayEndLogged = true;
    emit logMessage(
        QStringLiteral("Replay finished: %1 frames; the session stays open")
            .arg(m_replayFrames));
}

/* A sleep while connected (ADR-0033 §6): the machine's slept count rose
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
    emit logMessage(
        QStringLiteral("Asleep for %1 s: nothing was read meanwhile")
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
        return QStringLiteral(
                   "Session: replay of a recording, %1 frames over %2 s %3%4; ")
                   .arg(info.replay_frames)
                   .arg(QString::number(info.replay_span_ms / 1000.0, 'f', 1),
                        speed, loop ? QStringLiteral(", looping") : QString()) +
               profile;
    }
    if (info.backend == ECUIO_BACKEND_SOCKETCAN) {
        // The interface is in the "Connected:" line just before
        const QString bus =
            (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0
                ? QStringLiteral("a CAN controller in listen-only mode at %1 bit/s")
                      .arg(info.baud)
                : QStringLiteral("a virtual CAN bus");
        return QStringLiteral("Session: SocketCAN, %1, listening; ").arg(bus) +
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
