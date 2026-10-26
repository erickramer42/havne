// File/layer: app/qttunebridge.cpp | Qt6 Core, app layer, C++17
// Testing: test_bridge (QtCore only): worker→GUI marshaling, coalescing,
//          in-flight backpressure, the frame log's ring and order, its
//          rows' payload storage (BridgeTest.FrameLogHoldsLongPayloadsOnlyWhileShown)
//          and the counters on the tick, the signal model's roles and age
//          refresh, link-state mapping, the activity log's session and
//          ECU lines, session recording (BridgeTest.Recording*), display
//          units on the rows, the chart and not the recording
//          (BridgeTest.UnitSystem*), the URI label
//          (BridgeTest.UriLabelCutsADirectLoadPathToItsFileName), the
//          connect's trim (BridgeTest.ConnectTrimsSpacesAroundTheUri). QML
//          rendering: manual smoke.

#include "qttunebridge.h"
#include "ecuio/version.h"
#include "ecuio/signals.h"
#include "ecuio/j2534.h"
#include "ecuio/profile.h"

#include <QMetaObject>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

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
            if (e.histCount == 0) return -1;
            {
                const uint32_t last =
                    (e.histHead + static_cast<uint32_t>(HistoryCapacity) - 1)
                    % static_cast<uint32_t>(HistoryCapacity);
                return static_cast<int>(m_clock.elapsed()
                                         - e.history[last].timestampMs);
            }
        case DecimalsRole: return static_cast<int>(shown(e, e.value).decimal_places);
        case TextRole: return e.text;
        case IsTextRole: return e.isText;
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
        {IsTextRole, "isText"}
    };
}

void SignalListModel::setSignalSet(const ecuio_signal_set_t* set)
{
    // NULL/empty is valid (the raw profile): clear the model rather
    // than keep the previous session's rows.
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        beginResetModel();
        m_entries.clear();
        m_subscribedCount = 0;
        endResetModel();
        m_clock.restart();
        return;
    }

    beginResetModel();
    m_entries.clear();
    m_subscribedCount = 0;

    for (uint32_t i = 0; i < set->count; ++i) {
        Entry e = {};   // value-init zeroes the history ring, head, and count
        e.id = set->defs[i].id;
        qstrncpy(e.name, set->defs[i].name, sizeof(e.name));   // always terminates
        qstrncpy(e.unit, set->defs[i].unit, sizeof(e.unit));
        e.decimals = set->defs[i].decimal_places;
        e.isText = set->defs[i].kind == ECUIO_VALUE_TEXT ||
                   set->defs[i].kind == ECUIO_VALUE_HEX;
        // A text row has nothing to plot, so it's never subscribed
        e.subscribed = !e.isText;
        if (e.subscribed) ++m_subscribedCount;
        m_entries.push_back(e);
    }
    endResetModel();

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
    if (m_entries.empty() || values.empty()) return;

    // Batch-stamped time: the staging buffer merges latest-per-ID across the
    // coalescing window, so per-frame timestamps are already gone by here —
    // one timestamp per batch is the honest resolution. timestampMs >= 0
    // injects a deterministic value (tests today; per-sample real timestamps
    // when decode moves worker-side under J2534).
    const qint64 stamp = (timestampMs >= 0) ? timestampMs : m_clock.elapsed();

    // Every row, subscribed or not: `subscribed` filters chartData()
    // only. Skipping unsubscribed rows here would freeze the Dashboard's
    // gauges for a signal unticked on Live, and dim them as stale while
    // its frames still arrive.
    int firstRow = -1;
    int lastRow = -1;
    for (const auto& pair : values) {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            Entry& e = m_entries[i];
            if (e.id == pair.first) {
                e.value = pair.second;
                pushSample(e, stamp, pair.second);
                widenRows(static_cast<int>(i), firstRow, lastRow);
            }
        }
    }
    if (firstRow >= 0) {
        // AgeMsRole rides along with ValueRole/HasValueRole so delegates
        // bind opacity to ageMs; a QML-side clock stamped on value
        // changes would dim steady values that are still arriving.
        emit dataChanged(index(firstRow), index(lastRow),
                         {ValueRole, HasValueRole, AgeMsRole});
        // One emission per batch — the strip chart's sole repaint trigger.
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
        for (size_t i = 0; i < m_entries.size(); ++i) {
            Entry& e = m_entries[i];
            if (e.id != pair.first || !e.isText) continue;
            e.text = pair.second;
            // The ring keeps the sample's time, for hasValue and ageMs;
            // the value is never plotted
            pushSample(e, stamp, 0.0);
            widenRows(static_cast<int>(i), firstRow, lastRow);
        }
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
                     {ValueRole, UnitRole, DecimalsRole});
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

QVariantList SignalListModel::chartData() const
{
    QVariantList out;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (!e.subscribed || e.histCount == 0) continue;

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
    if (e.subscribed) ++m_subscribedCount; else --m_subscribedCount;
    emit dataChanged(index(row), index(row), {SubscribedRole});
    emit subscribedCountChanged();
}

// ---------------- QtTuneBridge ----------------
QtTuneBridge::QtTuneBridge(QObject* parent)
    : QObject(parent)
    , m_frameLog(new FrameListModel(this))
    , m_coalesceTimer(new QTimer(this))
    , m_signalModel(new SignalListModel(this))
{
    (void)ecuio_init();   // optional and always ECUIO_OK (core.h), paired below
    m_textScratch.resize(ECUIO_DECODE_TEXT_MAX);

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        notifyFrameCounts();
        pollLinkHealth();   // every tick while connected — transitions only
        pollChannelSupport();
        pollChannelIdentity();
        pollReplayEnd();
        // Must fire even when nothing arrived this window: dimming exists
        // for rows that STOPPED updating. 100 ms is well
        // inside the 3 s dim threshold and the span emission is one
        // signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        if (!m_stagingBuffer.empty() || !m_stagingTexts.empty()) {
            if (!m_stagingBuffer.empty()) {
                m_signalModel->updateValues(m_stagingBuffer);
                m_stagingBuffer.clear();
            }
            if (!m_stagingTexts.empty()) {
                m_signalModel->updateTexts(m_stagingTexts);
                m_stagingTexts.clear();
            }
            ++m_signalBatchCount;
            emit signalBatchChanged();
        }
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
    connect(this, &QtTuneBridge::logMessage, this, [this](const QString& line) {
        m_recorder.event(line);
    });
}


QtTuneBridge::~QtTuneBridge()
{
    // Async connect worker first: it calls core and posts a queued
    // completion against `this`. Join before tearing the session or
    // shutting core down. Worst case bounded by the surrogate handshake
    // timeout (~8 s) at app exit with a wedged vendor — acceptable for
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

void QtTuneBridge::destroySessionLocked()
{
    // The destructor's teardown. destroy() closes the session first if needed.
    if (m_session != nullptr) {
        ecuio_session_destroy(m_session);
        m_session = nullptr;
    }
}

QString QtTuneBridge::coreVersion() const
{
    return QString::fromUtf8(ecuio_version_string());
}

QString QtTuneBridge::statusString(int statusCode) const
{
    return QString::fromUtf8(ecuio_status_string(static_cast<ecuio_status_t>(statusCode)));
}

QVariantList QtTuneBridge::discoverJ2534Devices() const
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
         * 0 = unknown. Unknown is displayed honestly — routing
         * treats it as in-proc (x64), which is the load-time
         * fallback, not a claim about the binary. */
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

// Pure-core half: safe on ANY thread. Creates and registers, the slow
// part (DLL load, PassThruOpen), but doesn't start: until the GUI thread
// has taken the session, no worker may call onFrame with `this`. On
// failure, cleans up fully (core API, mutex-safe off-thread) and returns
// the error string. On success the caller owns *out_session and must
// startSession() and applyConnectedState() on the GUI thread. Touches no
// Qt models, timers, or properties.
QString QtTuneBridge::connectCoreOnly(const QString& uri,
                                       ecuio_session_t** out_session)
{
    const QByteArray uri8 = uri.toUtf8();
    ecuio_session_t* session = nullptr;
    ecuio_status_t rc = ecuio_session_create(uri8.constData(), &session);
    if (rc != ECUIO_OK || session == nullptr) {
        return failureText(rc);
    }

    rc = ecuio_register_frame_callback(session, &QtTuneBridge::onFrame, this);
    if (rc != ECUIO_OK) {
        ecuio_session_destroy(session);
        return QString::fromUtf8(ecuio_status_string(rc));
    }

    *out_session = session;
    return {};
}

// GUI thread: start the worker of a session connectCoreOnly produced.
// onFrame accepts from here (m_accepting), so a frame sent at once, as a
// replay's first is, queues and reaches handleFrameInternal after
// applyConnectedState. On failure the session is destroyed.
QString QtTuneBridge::startSession(ecuio_session_t* session)
{
    m_framesDelivered.store(0, std::memory_order_relaxed);
    m_accepting.store(true, std::memory_order_release);
    const ecuio_status_t rc = ecuio_session_start(session);
    if (rc != ECUIO_OK) {
        m_accepting.store(false, std::memory_order_release);
        const QString err = failureText(rc);
        ecuio_unregister_frame_callback(session, &QtTuneBridge::onFrame, this);
        ecuio_session_destroy(session);
        return err;
    }
    return {};
}

// GUI-thread half: state application after a successful connect,
// ending with the "Connected:" and "Session:" log lines. Factored out of
// connectSession so the sync and async paths apply IDENTICAL state
// (parity by construction, not by care).
void QtTuneBridge::applyConnectedState(ecuio_session_t* session, const QString& uri)
{
    // Before any emit: each runs handlers synchronously, and a connect one
    // of them starts must be refused, not replace this session
    m_session = session;
    m_connected.store(true, std::memory_order_release);
    // Before the queued frames run, so the recording holds every one
    startRecording(uri, ecuio_session_signal_set(session));
    m_currentUri = uri;
    emit currentUriChanged();
    // The frame log belongs to a session, like its count: rows from two
    // connects would otherwise run together with nothing between them.
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_lastReportedDropped = 0;
    m_signalModel->setSignalSet(ecuio_session_signal_set(session));
    m_stagingBuffer.clear();   // previous session's staged values must
    m_stagingTexts.clear();    // never bleed into this one's first batch
    m_linkErrorCode = ECUIO_OK;
    m_errorEpisodes = 0;
    emit linkStateChanged();
    setLogPaused(false);       // a new session always logs
    m_coalesceTimer->start(100);
    emit connectedChanged(true);
    emit framesReset();

    emit logMessage(QStringLiteral("Connected: %1").arg(uri));
    ecuio_session_info_t info{};
    m_channelCount = 0;
    if (ecuio_session_get_info(session, &info) == ECUIO_OK) {
        emit logMessage(sessionInfoText(info));
        m_channelCount = std::min<uint32_t>(info.channel_count,
                                            ECUIO_SESSION_INFO_MAX_CHANNELS);
    }
    m_replayFrames = info.replay_frames;
    m_replayEndLogged = info.backend != ECUIO_BACKEND_REPLAY ||
                        (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
    for (uint32_t& g : m_supportGeneration) {
        g = 0;
    }
    for (uint32_t& g : m_identityGeneration) {
        g = 0;
    }
}

QString QtTuneBridge::connectSession(const QString& typed)
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

    applyFrameLoggingEnv();
    ecuio_session_t* session = nullptr;
    QString err = connectCoreOnly(uri, &session);
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

void QtTuneBridge::connectSessionAsync(const QString& typed)
{
    const QString uri = typed.trimmed(); // as connectSession does
    if (m_connected.load(std::memory_order_acquire) || m_connecting
        || m_disconnecting) {
        return;   // one attempt at a time, none during a teardown; the
                  // UI already reflects state
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

void QtTuneBridge::disconnectSession()
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
    m_stagingBuffer.clear();
    m_stagingTexts.clear();
    m_currentUri.clear();
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

void QtTuneBridge::setRecording(bool on)
{
    if (m_connected.load(std::memory_order_acquire) || m_connecting || m_disconnecting) {
        emit recordingChanged();   // refused: puts the switch back
        return;
    }
    if (m_recording == on) return;
    m_recording = on;
    emit recordingChanged();
}

void QtTuneBridge::setUnitSystem(int units)
{
    if (units != static_cast<int>(ECUIO_UNITS_METRIC) &&
        units != static_cast<int>(ECUIO_UNITS_IMPERIAL)) {
        return;
    }
    if (units == unitSystem()) return;
    m_signalModel->setUnitSystem(static_cast<uint32_t>(units));
    emit unitSystemChanged();
}

double QtTuneBridge::displayValue(const QString& unit, double value) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, value, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return value;
    }
    return out.value;
}

QString QtTuneBridge::displayUnit(const QString& unit) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, 0.0, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return unit;
    }
    return QString::fromUtf8(out.unit);
}

QString QtTuneBridge::recordingFolder() const
{
    const QString folder = m_recorder.folder();
    if (folder.isEmpty()) return {};
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.isEmpty()) {
        const QString rel = QDir(docs).relativeFilePath(folder);
        if (QDir::isRelativePath(rel) && !rel.startsWith(QLatin1String(".."))) {
            return QDir::toNativeSeparators(QStringLiteral("Documents/") + rel);
        }
    }
    return QDir::toNativeSeparators(folder);
}

QUrl QtTuneBridge::recordingFolderUrl() const
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
void QtTuneBridge::applyFrameLoggingEnv()
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

void QtTuneBridge::startRecording(const QString& uri, const ecuio_signal_set_t* set)
{
    if (!m_recording) return;

    QString root = m_recordingsRoot;
    if (root.isEmpty()) {
        const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty()) {
            emit logMessage(QStringLiteral("Recording failed: no Documents folder"));
            return;
        }
        root = docs + QStringLiteral("/QtTune/recordings");
    }

    QStringList names;
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        names << QString::fromUtf8(set->defs[i].name,
                                   static_cast<qsizetype>(qstrnlen(set->defs[i].name,
                                                                   sizeof set->defs[i].name)));
    }
    const QByteArray frameLogging = qgetenv("ECUIO_SUR_LOG_FRAMES");
    const QStringList header = {
        QStringLiteral("QtTune session recording (ADR-0017)"),
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

    QString error;
    if (!m_recorder.start(root, header, &error)) {
        emit logMessage(QStringLiteral("Recording failed: %1").arg(error));
        return;
    }
    emit recordingStateChanged();
    emit logMessage(QStringLiteral("Recording: %1").arg(recordingFolder()));
}

QString QtTuneBridge::recordingSummary(const QString& why) const
{
    return QStringLiteral("end      %1: %2 frames, %3 values, %4 frames dropped by the bridge")
        .arg(why)
        .arg(m_recorder.frameRows())
        .arg(m_recorder.sampleRows())
        .arg(m_droppedFrames.load(std::memory_order_relaxed));
}

void QtTuneBridge::stopRecording(const QString& why)
{
    if (!m_recorder.active()) return;
    m_recorder.stop(recordingSummary(why));
    emit recordingStateChanged();
}

void QtTuneBridge::setLogPaused(bool paused)
{
    if (m_logPaused == paused) return;
    m_logPaused = paused;
    emit logPausedChanged();
}

void QtTuneBridge::clearFrames()
{
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    emit framesReset();
}

void QtTuneBridge::onFrame(const ecuio_frame_t* frame, void* user_data)
{
    QtTuneBridge* self = static_cast<QtTuneBridge*>(user_data);
    if (!frame || !self) return;
    if (!self->m_accepting.load(std::memory_order_acquire)) return;
    self->m_framesDelivered.fetch_add(1, std::memory_order_relaxed);

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

    // ecuio_frame_t holds its payload INLINE (data[ECUIO_FRAME_DATA_MAX]) —
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

namespace {

// The staging buffers keep the latest sample per signal id across the
// coalescing window
template <typename T>
void stageLatest(std::vector<std::pair<uint16_t, T>>& staging, uint16_t id,
                 const T& sample)
{
    for (auto& staged : staging) {
        if (staged.first == id) {
            staged.second = sample;   // overwrite with newest sample
            return;
        }
    }
    staging.emplace_back(id, sample);
}

} // namespace

void QtTuneBridge::handleFrameInternal(const ecuio_frame_t* frame,
                                       SessionRecorder::Clock::time_point arrived)
{
    if (!m_connected.load(std::memory_order_acquire)) return;

    // Defensive clamp: a misbehaving transport must never make us read past
    // the inline buffer, even if data_length exceeds ECUIO_FRAME_DATA_MAX.
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                         ? frame->data_length
                         : static_cast<uint16_t>(sizeof(frame->data));

    // Recorded whether or not the log is paused
    m_recorder.frame(arrived, *frame, len);

    // Paused: counted and decoded below, but kept out of the frame log
    if (!m_logPaused) {
        m_frameLog->appendFrame(*frame, len);
    }
    ++m_currentFrameCount;

    // ADR-0013: decode against the SESSION's profile set. NULL means
    // the raw profile — frames stream to the Log page, decode skipped.
    const ecuio_signal_set_t* set = ecuio_session_signal_set(m_session);
    if (set != nullptr) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& def = set->defs[i];
            // Core-side gate: arbitration ID, plus the mux key when the
            // def has one.
            if (!ecuio_def_applies_to_frame(&def, frame)) {
                continue;
            }
            if (def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX) {
                if (ecuio_decode_text(&def, frame->data, len, m_textScratch.data(),
                                      static_cast<uint32_t>(m_textScratch.size()))
                        == ECUIO_DECODE_OK) {
                    const QString text = QString::fromLatin1(m_textScratch.data());
                    m_recorder.sample(arrived, def, text);
                    stageLatest(m_stagingTexts, def.id, text);
                }
                continue;
            }
            double val = 0.0;
            if (ecuio_decode_signal(&def, frame->data, len, &val) == ECUIO_DECODE_OK) {
                m_recorder.sample(arrived, def, val);
                stageLatest(m_stagingBuffer, def.id, val);
            }
        }
    }

    // Counted here, reported on the tick (notifyFrameCounts): a signal
    // per frame would re-evaluate every binding on them per frame
    ++m_guiNotifyCount;
}

// The frame counters reach QML at the coalesce tick's rate, and at
// connect, disconnect and clear, never per frame.
void QtTuneBridge::notifyFrameCounts()
{
    if (m_currentFrameCount != m_reportedFrameCount) {
        m_reportedFrameCount = m_currentFrameCount;
        emit frameCountChanged();
    }
    if (m_guiNotifyCount != m_reportedNotifyCount) {
        m_reportedNotifyCount = m_guiNotifyCount;
        emit notifyCountChanged(m_guiNotifyCount);
    }
}

void QtTuneBridge::notifyDropped()
{
    const int dropped = m_droppedFrames.load(std::memory_order_relaxed);
    if (dropped != m_lastReportedDropped) {
        m_lastReportedDropped = dropped;
        emit droppedFramesChanged(dropped);
    }
}

void QtTuneBridge::forceFrameCountForTesting(int count)
{
    if (count < 0) {
        return;
    }
    m_currentFrameCount = count;
    notifyFrameCounts();   // keep the QML binding in sync
}

void QtTuneBridge::dispatchFrameForTesting(const ecuio_frame_t& frame)
{
    onFrame(&frame, this);
}

void QtTuneBridge::pollLinkHealth()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    (void)ecuio_session_get_error_state(m_session, &last_error, &episodes);
    if (last_error == m_linkErrorCode && episodes == m_errorEpisodes) {
        return;   // silent on unchanged state — the atomics only move
                  // on transitions, so this is not a poll storm
    }
    const int32_t prev = m_linkErrorCode;
    m_linkErrorCode = last_error;
    m_errorEpisodes = episodes;

    /* Transitions land in the activity log with ZERO QML work — this
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
    }
    emit linkStateChanged();
}

/* What each channel's ECU said it supports (ADR-0015 §4), in the
 * activity log once per update: the probe's walk ending, and each
 * request it drops. Generation 0 is nothing learned yet. */
void QtTuneBridge::pollChannelSupport()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_support_t s{};
        if (ecuio_session_get_channel_support(m_session, c, &s) != ECUIO_OK ||
            s.generation == m_supportGeneration[c]) {
            continue;
        }
        m_supportGeneration[c] = s.generation;
        emit logMessage(channelSupportText(s));
    }
}

/* One channel's "ECU 0x…:" line: what its supported-PID bitmaps list,
 * and which requests left the rotation, and why. Static, like
 * sessionInfoText, so test_bridge pins the wording without an adapter. */
QString QtTuneBridge::channelSupportText(const ecuio_channel_support_t& s)
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
void QtTuneBridge::pollChannelIdentity()
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
    }
}

/* One channel's "ECU 0x… identity:" line: each answered item by its
 * name and text, then the refused requests with their codes, then how
 * many went unanswered. Static, like channelSupportText, so test_bridge
 * pins the wording without an adapter. */
QString QtTuneBridge::channelIdentityText(const ecuio_channel_identity_t& s)
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

/* The URI as the header pill and the Dashboard show it. A direct-load
 * selector (one holding a separator, as resolve_uri tells a path from a
 * vendor name) is cut to the DLL's file name, j2534://…/vendor.dll, so
 * a local folder never reaches the screen or a screenshot, and a long
 * path can't elide the DLL off the pill's end. The query stays; every
 * other URI is unchanged. Settings and the activity log keep it whole. */
QString QtTuneBridge::uriLabelFor(const QString& uri)
{
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
void QtTuneBridge::pollReplayEnd()
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

/* The published state strings, in one place; QML's Theme.linkColor and
 * Theme.linkLabel key on them. Static and session-free so test_bridge
 * can pin every mapping without a transport — linkState() itself stays
 * a connected-check + delegation. */
QString QtTuneBridge::linkStateForError(int32_t errorCode)
{
    switch (errorCode) {
        case ECUIO_OK:                         return QStringLiteral("ok");
        case ECUIO_ERR_VEHICLE_TIMEOUT:        return QStringLiteral("quiet");
        case ECUIO_ERR_REQUEST_FAILED:         return QStringLiteral("requests");
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return QStringLiteral("fault");
        default: return QStringLiteral("degraded"); /* forward-compatible */
    }
}

/* What the session runs on, for the activity log (and so the
 * recording's events.log). Static, like linkStateForError, so
 * test_bridge pins the wording without an adapter. */
QString QtTuneBridge::sessionInfoText(const ecuio_session_info_t& info)
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
    if (info.backend != ECUIO_BACKEND_J2534_INPROC &&
        info.backend != ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("Session: backend %1").arg(info.backend);
    }
    const auto text = [](const char* s) {
        const QString t = QString::fromUtf8(s, static_cast<qsizetype>(
            qstrnlen(s, ECUIO_SESSION_INFO_TEXT_LEN)));
        return t.isEmpty() ? QStringLiteral("(none)") : t;
    };

    QStringList parts;
    const QString arch = (info.dll_arch == ECUIO_DLL_ARCH_X86) ? QStringLiteral("x86 DLL")
                       : (info.dll_arch == ECUIO_DLL_ARCH_X64) ? QStringLiteral("x64 DLL")
                       : QStringLiteral("DLL of unknown architecture");
    parts << arch + ((info.backend == ECUIO_BACKEND_J2534_SURROGATE)
                         ? QStringLiteral(", through the surrogate")
                         : QStringLiteral(", in-proc"));

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
    parts << ((info.flags & ECUIO_SESSION_INFO_VERSIONS)
                  ? QStringLiteral("firmware %1, DLL %2, API %3")
                        .arg(text(info.firmware_version), text(info.dll_version),
                             text(info.api_version))
                  : QStringLiteral("no version reading"));
    return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
}

QString QtTuneBridge::linkState() const
{
    if (!m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("disconnected");
    }
    return linkStateForError(m_linkErrorCode);
}
