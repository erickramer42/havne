// File/layer: app/qttunebridge.cpp | Qt6 Core, app layer, C++17
// Testing: test_bridge (QtCore only): worker→GUI marshaling, coalescing,
//          in-flight backpressure, the signal model's roles and age
//          refresh, link-state mapping. QML rendering: manual smoke.

#include "qttunebridge.h"
#include "qttune/version.h"
#include "qttune/signals.h"
#include "qttune/j2534.h"
#include "qttune/profile.h"

#include <QMetaObject>
#include <QByteArray>

#include <algorithm>

// ---------------- FrameListModel ----------------
FrameListModel::FrameListModel(QObject* parent)
    : QAbstractListModel(parent)
{
    m_frames.reserve(MaxFrames);
}

int FrameListModel::rowCount(const QModelIndex&) const
{
    return static_cast<int>(m_frames.size());
}

QVariant FrameListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_frames.size())) {
        return QVariant();
    }
    const QVariantMap& frame = m_frames[static_cast<size_t>(index.row())];
    switch (role) {
        case TimestampRole:    return frame.value("timestampUs");
        case DlcRole:         return frame.value("dlc");
        case ExtendedRole:     return frame.value("extended");
        case PayloadRole:      return frame.value("payloadHex");
        case ArbitrationIdRole: return frame.value("arbId");
        default:              return QVariant();
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

void FrameListModel::appendFrame(const QVariantMap& frame)
{
    const int row = rowCount();
    beginInsertRows(QModelIndex(), row, row);
    m_frames.push_back(frame);
    endInsertRows();

    while (static_cast<int>(m_frames.size()) > MaxFrames) {
        beginRemoveRows(QModelIndex(), 0, 0);
        m_frames.erase(m_frames.begin());
        endRemoveRows();
    }
    emit countChanged(static_cast<int>(m_frames.size()));
}

void FrameListModel::clear()
{
    if (m_frames.empty()) return;
    beginResetModel();
    m_frames.clear();
    endResetModel();
    emit countChanged(0);
}

// ---------------- FrameSortProxy ----------------
bool FrameSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const quint64 lhs = sourceModel()->data(left, FrameListModel::TimestampRole).toULongLong();
    const quint64 rhs = sourceModel()->data(right, FrameListModel::TimestampRole).toULongLong();
    return lhs < rhs;
}

FrameSortProxy::FrameSortProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(FrameListModel::TimestampRole);
    setDynamicSortFilter(true);
    sort(0, Qt::DescendingOrder);
}

bool FrameSortProxy::newestFirst() const
{
    return m_newestFirst;
}

void FrameSortProxy::setNewestFirst(bool newestFirst)
{
    if (m_newestFirst == newestFirst) return;
    m_newestFirst = newestFirst;
    sort(0, newestFirst ? Qt::DescendingOrder : Qt::AscendingOrder);
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
        case UnitRole: return QString::fromUtf8(e.unit);
        case ValueRole: return e.value;
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
        {AgeMsRole, "ageMs"}
    };
}

void SignalListModel::setSignalSet(const QttuneSignalSet* set)
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
        strncpy(e.name, set->defs[i].name, sizeof(e.name) - 1);
        strncpy(e.unit, set->defs[i].unit, sizeof(e.unit) - 1);
        e.subscribed = true;
        ++m_subscribedCount;
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

void SignalListModel::updateValues(const std::vector<std::pair<uint16_t, float>>& values,
                                   qint64 timestampMs)
{
    if (m_entries.empty() || values.empty()) return;

    // Batch-stamped time: the staging buffer merges latest-per-ID across the
    // coalescing window, so per-frame timestamps are already gone by here —
    // one timestamp per batch is the honest resolution. timestampMs >= 0
    // injects a deterministic value (tests today; per-sample real timestamps
    // when decode moves worker-side under J2534).
    const qint64 stamp = (timestampMs >= 0) ? timestampMs : m_clock.elapsed();

    int firstRow = -1;
    int lastRow = -1;
    for (const auto& pair : values) {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            Entry& e = m_entries[i];
            if (e.id == pair.first && e.subscribed) {
                e.value = pair.second;

                // Evict-oldest ring push — bounded memory, stream never
                // freezes; mirrors the FrameListModel ring philosophy.
                e.history[e.histHead].timestampMs = stamp;
                e.history[e.histHead].value = pair.second;
                e.histHead = (e.histHead + 1) % HistoryCapacity;
                if (e.histCount < HistoryCapacity) {
                    ++e.histCount;
                }

                if (firstRow < 0 || static_cast<int>(i) < firstRow) firstRow = static_cast<int>(i);
                if (static_cast<int>(i) > lastRow) lastRow = static_cast<int>(i);
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

QVariantList SignalListModel::chartData() const
{
    QVariantList out;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (!e.subscribed || e.histCount == 0) continue;

        QVariantMap m;
        m["id"] = e.id;
        m["row"] = static_cast<int>(row);
        m["name"] = QString::fromUtf8(e.name);
        m["unit"] = QString::fromUtf8(e.unit);
        m["value"] = e.value;

        // Flattened [ts, val, ts, val, ...], oldest first.
        // When full, the oldest surviving sample sits at histHead (the
        // next eviction slot); while filling, samples are dense from 0.
        QVariantList samples;
        const uint32_t start = (e.histCount == HistoryCapacity)
                                ? e.histHead : 0;
        for (uint32_t i = 0; i < e.histCount; ++i) {
            const Sample& s = e.history[(start + i) % HistoryCapacity];
            samples.append(static_cast<double>(s.timestampMs));
            samples.append(static_cast<double>(s.value));
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
    e.subscribed = !e.subscribed;
    if (e.subscribed) ++m_subscribedCount; else --m_subscribedCount;
    emit dataChanged(index(row), index(row), {SubscribedRole});
    emit subscribedCountChanged();
}

// ---------------- QtTuneBridge ----------------
QtTuneBridge::QtTuneBridge(QObject* parent)
    : QObject(parent)
    , m_sourceModel(new FrameListModel(this))
    , m_sortProxy(new FrameSortProxy(this))
    , m_coalesceTimer(new QTimer(this))
    , m_signalModel(new SignalListModel(this))
{
    m_initialized = (qttune_init() == QT_OK);
    m_sortProxy->setSourceModel(m_sourceModel);

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        pollLinkHealth();   // every tick while connected — transitions only
        // Must fire even when nothing arrived this window: dimming exists
        // for rows that STOPPED updating. 100 ms is well
        // inside the 3 s dim threshold and the span emission is one
        // signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        if (!m_stagingBuffer.empty()) {
            m_signalModel->updateValues(m_stagingBuffer);
            m_stagingBuffer.clear();
            ++m_signalBatchCount;
            emit signalBatchChanged();
        }
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
    if (m_session != nullptr) {
        destroySessionLocked();
    }
    if (m_initialized) {
        qttune_shutdown();
    }
}

void QtTuneBridge::destroySessionLocked()
{
    // The one teardown path. destroy() closes the session first if needed.
    if (m_session != nullptr) {
        qttune_session_destroy(m_session);
        m_session = nullptr;
    }
}

QString QtTuneBridge::coreVersion() const
{
    return QString::fromUtf8(qttune_version_string());
}

bool QtTuneBridge::coreInitialized() const
{
    return m_initialized;
}

QString QtTuneBridge::statusString(int statusCode) const
{
    return QString::fromUtf8(qttune_status_string(static_cast<qttune_status_t>(statusCode)));
}

QVariantList QtTuneBridge::discoverJ2534Devices() const
{
    QVariantList out;

    qttune_j2534_device_t* devices = nullptr;
    uint32_t count = 0;
    if (qttune_j2534_discover_devices(&devices, &count) != QT_OK) {
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
    qttune_j2534_free_devices(devices);
    return out;
}

// Pure-core half: safe on ANY thread. Creates, registers, starts. On
// failure, cleans up fully (the unregister/destroy calls are core API,
// mutex-safe off-thread) and returns the error string. On success the
// caller owns *out_session and must applyConnectedState() on the GUI
// thread. Touches no Qt models, timers, or properties.
QString QtTuneBridge::connectCoreOnly(const QString& uri,
                                       qttune_session_t** out_session)
{
    const QByteArray uri8 = uri.toUtf8();
    qttune_session_t* session = nullptr;
    qttune_status_t rc = qttune_session_create(uri8.constData(), &session);
    if (rc != QT_OK || session == nullptr) {
        return QString::fromUtf8(qttune_status_string(rc));
    }

    rc = qttune_register_frame_callback(session, &QtTuneBridge::onFrame, this);
    if (rc != QT_OK) {
        qttune_session_destroy(session);
        return QString::fromUtf8(qttune_status_string(rc));
    }

    rc = qttune_session_start(session);
    if (rc != QT_OK) {
        qttune_unregister_frame_callback(session, &QtTuneBridge::onFrame, this);
        qttune_session_destroy(session);
        return QString::fromUtf8(qttune_status_string(rc));
    }

    *out_session = session;
    return {};
}

// GUI-thread half: state application after a successful connect.
// Factored out of connectSession so the sync and async paths apply
// IDENTICAL state (parity by construction, not by care).
void QtTuneBridge::applyConnectedState(qttune_session_t* session, const QString& uri)
{
    m_session = session;
    m_currentUri = uri;
    emit currentUriChanged();
    m_connected.store(true, std::memory_order_release);
    m_currentFrameCount = 0;
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_lastReportedDropped = 0;
    m_signalModel->setSignalSet(qttune_session_signal_set(session));
    m_stagingBuffer.clear();   // previous session's staged values must
                               // never bleed into this one's first batch
    m_linkErrorCode = QT_OK;
    m_errorEpisodes = 0;
    emit linkStateChanged();
    m_coalesceTimer->start(100);
    emit connectedChanged(true);
    emit framesReset();
}

QString QtTuneBridge::connectSession(const QString& uri)
{
    if (m_connected.load(std::memory_order_acquire) || m_connecting) {
        return {};
    }

    qttune_session_t* session = nullptr;
    const QString err = connectCoreOnly(uri, &session);
    if (!err.isEmpty()) {
        emit logMessage(QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    applyConnectedState(session, uri);
    emit logMessage(QStringLiteral("Connected: %1").arg(uri));
    return {};
}

void QtTuneBridge::connectSessionAsync(const QString& uri)
{
    if (m_connected.load(std::memory_order_acquire) || m_connecting) {
        return;   // one attempt at a time; UI already reflects state
    }
    m_connecting = true;
    emit connectingChanged();
    emit logMessage(QStringLiteral("Connecting: %1").arg(uri));

    m_connectThread = std::thread([this, uri]() {
        qttune_session_t* session = nullptr;
        const QString err = connectCoreOnly(uri, &session);

        QMetaObject::invokeMethod(this, [this, uri, err, session]() {
            // JOIN FIRST, then clear the flag, then emit: assigning to a
            // joinable std::thread terminates the process, and a QML
            // handler can run SYNCHRONOUSLY inside connectingChanged —
            // this ordering keeps even that nested re-entry safe.
            if (m_connectThread.joinable()) {
                m_connectThread.join();
            }
            m_connecting = false;
            emit connectingChanged();

            if (err.isEmpty()) {
                applyConnectedState(session, uri);
                emit logMessage(QStringLiteral("Connected: %1").arg(uri));
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
    if (!m_connected.load(std::memory_order_acquire)) return;

    emit logMessage(QStringLiteral("Disconnected: %1").arg(m_currentUri));

    m_connected.store(false, std::memory_order_release);
    m_coalesceTimer->stop();
    destroySessionLocked();
    m_stagingBuffer.clear();
    if (!m_currentUri.isEmpty()) {
        m_currentUri.clear();
        emit currentUriChanged();
    }
    emit connectedChanged(false);
    m_linkErrorCode = QT_OK;          // the properties must not
    m_errorEpisodes = 0;              // outlive the session
    emit linkStateChanged();          // "fault"/"quiet" -> "disconnected"
}

void QtTuneBridge::clearFrames()
{
    if (m_sourceModel) m_sourceModel->clear();
    m_currentFrameCount = 0;
    emit framesReset();
}

void QtTuneBridge::onFrame(const qttune_frame_t* frame, void* user_data)
{
    QtTuneBridge* self = static_cast<QtTuneBridge*>(user_data);
    if (!frame || !self) return;
    if (!self->m_connected.load(std::memory_order_acquire)) return;

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

    // qttune_frame_t holds its payload INLINE (uint8_t data[64]) — this struct
    // copy is a deep copy of the payload; no dangling pointer is possible.
    const qttune_frame_t copy = *frame;
    self->m_inFlightFrames.fetch_add(1, std::memory_order_relaxed);

    // Lifetime contract: invoking with `self` as context object means the
    // queued lambda is dropped if the bridge is destroyed before it runs.
    // DO NOT change to a context-free postEvent pattern without revisiting
    // destruction ordering.
    QMetaObject::invokeMethod(self, [self, copy]() {
        self->m_inFlightFrames.fetch_sub(1, std::memory_order_relaxed);
        self->handleFrameInternal(&copy);
    }, Qt::QueuedConnection);
}

void QtTuneBridge::handleFrameInternal(const qttune_frame_t* frame)
{
    if (!m_connected.load(std::memory_order_acquire)) return;

    // Defensive clamp: a misbehaving transport must never make us read past
    // the inline buffer, even if data_length exceeds the documented 0-64.
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                         ? frame->data_length
                         : static_cast<uint16_t>(sizeof(frame->data));

    QVariantMap vm;
    vm["timestampUs"] = QVariant::fromValue<quint64>(frame->timestamp_us);
    vm["arbId"] = QVariant::fromValue<quint32>(frame->arbitration_id);
    vm["dlc"] = static_cast<uint32_t>(len);
    vm["extended"] = (frame->flags & QTTUNE_FRAME_FLAG_EXTENDED) != 0;
    vm["payloadHex"] = QByteArray(
        reinterpret_cast<const char*>(frame->data),
        static_cast<qsizetype>(len)).toHex();

    m_sourceModel->appendFrame(vm);
    ++m_currentFrameCount;

    // ADR-0013: decode against the SESSION's profile set. NULL means
    // the raw profile — frames stream to the Log page, decode skipped.
    const QttuneSignalSet* set = qttune_session_signal_set(m_session);
    if (set != nullptr) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const QttuneSignalDef& def = set->defs[i];
            // Core-side gate: arbitration ID, plus the mux key when the
            // def has one.
            if (qttune_def_matches_frame(&def, frame) != 0) {
                continue;
            }
            float val = 0.0f;
            if (qttune_decode_signal(&def, frame->data, len, &val) == 0) {
                bool merged = false;
                for (auto& staged : m_stagingBuffer) {
                    if (staged.first == def.id) {
                        staged.second = val;   // overwrite with newest sample
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    m_stagingBuffer.emplace_back(def.id, val);
                }
            }
        }
    }

    ++m_guiNotifyCount;
    emit frameCountChanged();
    emit notifyCountChanged(m_guiNotifyCount);
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
    emit frameCountChanged();   // keep the QML binding in sync
}

void QtTuneBridge::dispatchFrameForTesting(const qttune_frame_t& frame)
{
    onFrame(&frame, this);
}

void QtTuneBridge::pollLinkHealth()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    int32_t last_error = QT_OK;
    uint32_t episodes = 0;
    (void)qttune_session_get_error_state(m_session, &last_error, &episodes);
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
    if (prev != QT_OK && last_error == QT_OK) {
        emit logMessage(QStringLiteral("Link: recovered"));
    } else if (last_error == QT_ERR_VEHICLE_TIMEOUT) {
        emit logMessage(QStringLiteral("Link: vehicle not responding"));
    } else if (last_error == QT_ERR_REQUEST_FAILED) {
        emit logMessage(QStringLiteral("Link: requests failing (%1 episode(s))")
                            .arg(m_errorEpisodes));
    } else if (last_error == QT_ERR_TRANSPORT_HARD_FAILURE) {
        emit logMessage(QStringLiteral("Link: adapter fault (%1 episode(s))")
                            .arg(m_errorEpisodes));
    }
    emit linkStateChanged();
}

/* The published state strings, in one place; QML's Theme.linkColor and
 * Theme.linkLabel key on them. Static and session-free so test_bridge
 * can pin every mapping without a transport — linkState() itself stays
 * a connected-check + delegation. */
QString QtTuneBridge::linkStateForError(int32_t errorCode)
{
    switch (errorCode) {
        case QT_OK:                        return QStringLiteral("ok");
        case QT_ERR_VEHICLE_TIMEOUT:       return QStringLiteral("quiet");
        case QT_ERR_REQUEST_FAILED:        return QStringLiteral("requests");
        case QT_ERR_TRANSPORT_HARD_FAILURE: return QStringLiteral("fault");
        default: return QStringLiteral("degraded"); /* forward-compatible */
    }
}

QString QtTuneBridge::linkState() const
{
    if (!m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("disconnected");
    }
    return linkStateForError(m_linkErrorCode);
}
