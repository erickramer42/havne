// File/layer: app/sessionbridge.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); Manual smoke

#include "sessionbridge.h"
#include "bridgetext.h"
#include "carryover.h"
#include "healthreport.h"
#include "obdbimport.h"
#include "storemigration.h"
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */
#include "recording_signals.h" /* cli/: a replay's own signal file */
#include "signal_file_lines.h" /* cli/: the profile a URI selects */
#include "refused_signals.h" /* cli/: the signals an ECU refuses, as havne-cli says them */
#include "default_pick.h" /* cli/: the default pick, as havne-cli's --pick ranked */
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
#include <QJsonDocument>
#include <QJsonObject>
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

using namespace bridgetext;

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
    m_pluginStore.setLog(
        [this](const QString& line) { emit logMessage(line); });
    m_conditionModel = new ConditionModel(this);
    loadSavedConditions();

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        notifyFrameCounts();
        pollLinkHealth();   // every tick while connected: transitions only
        pollChannelSupport();
        pollAbsentSignals();
        pollDefaultPick(); // after the reports: their drops judge it
        pollUnaskedSignals();
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
        flushConditions(
            m_session); // after the values: a firing's value is shown
        pollRecording();
    });
    // A tick asks its row: the default pick follows on the next tick
    connect(m_signalModel, &SignalListModel::ticksChanged, this,
            [this]() { m_pickDirty = true; });

    // The log's readout: its time and its folder's size, once a second
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(1000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() {
        countLogBytes();
        emit logProgressChanged();
    });

    // What Armed says about starting follows everything that decides it
    for (auto changed :
         {&SessionBridge::recordingStateChanged,
          &SessionBridge::logStateChanged, &SessionBridge::conditionsChanged,
          &SessionBridge::currentUriChanged,
          &SessionBridge::disconnectingChanged}) {
        connect(this, changed, this, [this]() { judgeArmedWhy(); });
    }
    connect(this, &SessionBridge::connectedChanged, this,
            [this]() { judgeArmedWhy(); });
    connect(m_conditionModel, &QAbstractItemModel::modelReset, this,
            [this]() { judgeArmedWhy(); });

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
    if (rc == ECUIO_OK) {
        // ADR-0063: a frame's condition events, after its values
        rc = ecuio_register_conditions_callback(
            session, &SessionBridge::onConditions, this);
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
namespace {

// Whether a URI's query names poll=: such a session asks the polled
// signals alone, whatever its pick
bool namesPoll(const QString& uri)
{
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    if (q < 0) {
        return false;
    }
    const QStringList segments = uri.mid(q + 1).split(QLatin1Char('&'));
    return std::any_of(segments.begin(), segments.end(), [](const QString& s) {
        return s.startsWith(QLatin1String("poll="));
    });
}

} // namespace

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
    // Before the start (no whole sweep first) and the recorder (its header
    // names the pick); the vehicle's ticks come after the connect
    m_pickSet = false;
    m_pickIds.clear();
    m_pickPolled = namesPoll(uri);
    m_pickLine.clear();
    m_pickLinePending = false;
    applyDefaultPick(session, false, true);
    // Before the recorder, which holds the set the session judges then
    // (ECUIO_ERR_IN_USE after), and before the start, so the first frame
    // is judged
    attachSessionConditions(session, uri);
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
        ecuio_unregister_conditions_callback(
            session, &SessionBridge::onConditions, this);
        ecuio_session_destroy(session);
        endSessionConditions();
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
    // Precise: a coarse timer fires up to 5 % early, and the ring's 120 s
    // counts on ten windows a second
    m_coalesceTimer->setTimerType(Qt::PreciseTimer);
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
    if (m_pickLinePending) {
        m_pickLinePending = false;
        writePickLine();
    }
    for (const QString& line : std::exchange(m_conditionsLines, {})) {
        emit logMessage(line);
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
    m_unaskedRead.assign(m_absentRead.size(), 0);
    m_unaskedIds.clear();
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
    flushConditions(session); // and its condition events
    endSessionConditions();
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
    m_pickDirty = true; // the default pick takes it too
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
    m_conditionModel->setUnitSystem(static_cast<uint32_t>(units));
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

double SessionBridge::decodedValue(const QString& unit, double shown) const
{
    ecuio_display_value_t at0;
    ecuio_display_value_t at1;
    const QByteArray u = unit.toUtf8();
    const uint32_t units = m_signalModel->unitSystem();
    if (ecuio_display_convert(u.constData(), 0, 0.0, units, &at0) != ECUIO_DISPLAY_OK ||
        ecuio_display_convert(u.constData(), 0, 1.0, units, &at1) != ECUIO_DISPLAY_OK ||
        at0.converted == 0 || at1.value == at0.value) {
        return shown;
    }
    return (shown - at0.value) / (at1.value - at0.value);
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
                                              QString* reason, uint32_t flags)
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
    o.flags = flags;
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
        // Core's words first: the close below can clear them
        const QString detail = detailOr(rc);
        // The folder is this call's own, made by the open above; a
        // session never attached wrote it no frame, so nothing is lost
        const QString folder =
            m_failRecorderFolderForTesting ? QString() : folderOf(recorder);
        (void)ecuio_recorder_close(recorder,
                                   "the session couldn't be recorded");
        if (!folder.isEmpty() && !holdsFrames(folder)) {
            (void)QDir(folder).removeRecursively();
        }
        return fail(detail);
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
    if (m_armedActive) {
        return refuse(QStringLiteral("Armed is recording this session"));
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
        if (!applyDefaultPick(m_session, true, true)) {
            (void)ecuio_session_set_focus(m_session, nullptr, 0);
        }
        // openRecorder wrote the activity log's line
        setLogNotice(QStringLiteral("Log not started: %1").arg(why));
        return false;
    }
    setLogNotice(QString());
    m_logActive = true;
    // The log owns the focus; its end sets the pick again, with its line
    m_pickLine.clear();
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

QString SessionBridge::armedRefusal() const
{
    if (m_session == nullptr || !m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("not connected");
    }
    if (m_disconnecting) {
        return QStringLiteral("disconnecting");
    }
    if (m_armedActive) {
        return QString();
    }
    if (m_logActive) {
        return QStringLiteral("a log is running");
    }
    if (m_recorder != nullptr) {
        return QStringLiteral("Recording this session already");
    }
    // A replay is judged by its recording's own conditions, which an
    // armed copy of it would only duplicate: refused as a log of one is
    if (!m_recordReplays && m_currentUri.startsWith(QLatin1String("replay://"),
                                                    Qt::CaseInsensitive)) {
        return QStringLiteral("a replay plays a recording already");
    }
    // The words core's attach refuses with (ADR-0063 decision D4)
    if (!m_skipArmedJudgmentForTesting &&
        (m_conditionsGeneration == 0 || !m_conditionModel->anyRecords())) {
        return QStringLiteral("no condition records");
    }
    return QString();
}

void SessionBridge::judgeArmedWhy()
{
    const QString why = armedRefusal();
    if (why != m_armedWhy) {
        m_armedWhy = why;
        emit armedWhyChanged();
    }
}

bool SessionBridge::startArmed()
{
    const auto refuse = [this](const QString& why) {
        const QString line = QStringLiteral("Armed not started: %1").arg(why);
        setLogNotice(line);
        emit logMessage(line);
        return false;
    };
    if (m_armedActive) {
        return refuse(QStringLiteral("armed already"));
    }
    const QString why = armedRefusal();
    if (!why.isEmpty()) {
        return refuse(why);
    }
    QString failed;
    ecuio_recorder_t* recorder = openRecorder(
        m_session, m_currentUri, QStringLiteral("Armed not started"), &failed,
        ECUIO_RECORD_ARMED);
    if (recorder == nullptr) {
        // openRecorder wrote the activity log's line
        setLogNotice(QStringLiteral("Armed not started: %1").arg(failed));
        return false;
    }
    setLogNotice(QString());
    m_armedActive = true;
    m_armedInWindow = false;
    m_armedWindows = 0;
    m_armedFolder.clear();
    adoptRecorder(recorder);
    emit armedStateChanged();
    emit logMessage(QStringLiteral(
        "Armed: nothing is written until a condition that records fires"));
    judgeArmedWhy();
    return true;
}

void SessionBridge::stopArmed()
{
    // A teardown under way ends it itself, once the session is gone
    if (!m_armedActive || m_disconnecting) {
        return;
    }
    stopRecording(QStringLiteral("Stop armed"), true);
}

void SessionBridge::pollArmed(const ecuio_recorder_state_t& st)
{
    const bool inWindow = st.state == ECUIO_RECORDING_ACTIVE;
    const bool rose = static_cast<int>(st.windows) > m_armedWindows;
    const bool wasIn = m_armedInWindow;
    if (!rose && inWindow == wasIn) {
        return;
    }
    if (wasIn && (rose || !inWindow)) {
        emit logMessage(
            QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
        m_armedInWindow = false;
    }
    if (rose) {
        // Windows that opened and closed between two polls show as one
        const int opened = static_cast<int>(st.windows) - m_armedWindows;
        m_armedWindows = static_cast<int>(st.windows);
        m_recordingFolder = folderOf(m_recorder);
        m_armedFolder = QFileInfo(m_recordingFolder).fileName();
        emit logMessage(
            opened > 1
                ? QStringLiteral("Armed windows opened (%1), newest: %2")
                      .arg(opened)
                      .arg(m_armedFolder)
                : QStringLiteral("Armed window opened: %1").arg(m_armedFolder));
        m_armedInWindow = true;
        if (!inWindow) {
            emit logMessage(
                QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
            m_armedInWindow = false;
        }
    }
    emit armedStateChanged();
    emit recordingStateChanged();
}

void SessionBridge::endArmed()
{
    if (m_armedInWindow) {
        emit logMessage(
            QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
    }
    // A window opened by the close's last drain isn't seen here: the
    // line says what was seen
    if (!m_armedFailure.isEmpty()) {
        emit logMessage(QStringLiteral("Armed ended: %1").arg(m_armedFailure));
        m_armedFailure.clear();
    } else {
        emit logMessage(m_armedWindows == 0
                            ? QStringLiteral("Armed ended: no window opened")
                            : QStringLiteral("Armed ended: %1 window%2")
                                  .arg(m_armedWindows)
                                  .arg(m_armedWindows == 1
                                           ? QString()
                                           : QStringLiteral("s")));
    }
    m_armedActive = false;
    m_armedInWindow = false;
    emit armedStateChanged();
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
    if (m_armedActive) {
        ecuio_recorder_state_t last{};
        if (ecuio_recorder_get_state(m_recorder, &last) == ECUIO_OK) {
            pollArmed(last);
        }
    }
    ecuio_recorder_t* recorder = m_recorder;
    m_recorder = nullptr;
    (void)ecuio_recorder_close(recorder, why.toUtf8().constData());
    emit recordingStateChanged();
    if (m_armedActive) {
        endArmed();
    }
    // The recording held its set: an edit saved meanwhile is judged now
    if (!m_conditionsPending.isEmpty() && conditionsWait().isEmpty()) {
        m_conditionsPending.clear();
        applySavedConditions();
        emit conditionsChanged();
    }
    if (!m_logActive) {
        return;
    }
    m_logTimer->stop();
    m_logElapsedMs = m_logClock.elapsed();
    m_logActive = false;
    // A session still running asks its default pick again (the whole set
    // on one that ranks none); one torn down took its focus with it
    if (m_session != nullptr && !applyDefaultPick(m_session, true, true)) {
        (void)ecuio_session_set_focus(m_session, nullptr, 0);
    }
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
    // Between windows a mark would go nowhere (recording.h)
    if (m_armedActive) {
        ecuio_recorder_state_t st{};
        if (ecuio_recorder_get_state(m_recorder, &st) != ECUIO_OK ||
            st.state != ECUIO_RECORDING_ACTIVE) {
            return false;
        }
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
        ecuio_recorder_get_state(m_recorder, &st) != ECUIO_OK) {
        return;
    }
    if (m_armedActive) {
        pollArmed(st);
    }
    if (st.state != ECUIO_RECORDING_FAILED) {
        return;
    }
    const QString reason = QString::fromUtf8(st.reason);
    const bool log = m_logActive;
    const bool armed = m_armedActive;
    if (armed) {
        m_armedFailure = reason;
    }
    stopRecording(QStringLiteral("a write failed"));
    emit logMessage(QStringLiteral("Recording stopped: %1").arg(reason));
    // The Dashboard's notice gets the recorder's own words (the disk is
    // full), not the recording's end note
    if (log) {
        setLogNotice(QStringLiteral("Log ended: %1").arg(reason));
    }
    if (armed) {
        setLogNotice(QStringLiteral("Armed ended: %1").arg(reason));
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

void SessionBridge::dispatchConditionsForTesting(
    const ecuio_condition_event_t* events, uint32_t count)
{
    onConditions(events, count, this);
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

// ---- Conditions (ADR-0063 sections 6 and 9) ----

namespace {

// "1 condition", "2 conditions"
QString conditionCountText(uint32_t count)
{
    return count == 1 ? QStringLiteral("1 condition")
                      : QStringLiteral("%1 conditions").arg(count);
}

} // namespace

void SessionBridge::onConditions(const ecuio_condition_event_t* events,
                                 uint32_t count, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (events == nullptr || self == nullptr) {
        return;
    }
    if (!self->m_accepting.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->m_conditionsMutex);
    std::vector<ConditionSlot>& window =
        self->m_conditionStaging[self->m_activeConditions];
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_condition_event_t& e = events[i];
        // A set since replaced: its index means another condition now
        if (e.generation != self->m_stagingGeneration ||
            e.condition >= window.size()) {
            continue;
        }
        ConditionSlot& slot = window[e.condition];
        if ((e.flags & ECUIO_CONDITION_FIRED) != 0 && slot.firings++ == 0) {
            slot.first = e;
        }
        slot.newest = e;
        slot.has = true;
    }
}

void SessionBridge::prepareConditionStaging(uint32_t count, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(m_conditionsMutex);
    for (std::vector<ConditionSlot>& window : m_conditionStaging) {
        window.assign(count, ConditionSlot{});
    }
    m_activeConditions = 0;
    m_stagingGeneration = generation;
}

bool SessionBridge::attachConditions(ecuio_session_t* session,
                                     const ecuio_condition_set_t* set,
                                     QString* why)
{
    if (m_conditionsGeneration != 0) {
        flushConditions(session);
    }
    size_t staged = 0;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        staged = m_conditionStaging[0].size();
    }
    // The slots first, under the generation core is about to begin: a
    // first judgment can come before the call returns
    const uint32_t count = ecuio_condition_set_count(set);
    const uint32_t next = m_conditionsGeneration + 1;
    prepareConditionStaging(count, next);
    const ecuio_signal_set_t* signalSet = ecuio_session_signal_set(session);
    const ecuio_signal_file_t* file = fileOfSet(signalSet);
    const ecuio_status_t rc =
        m_failAttaches > 0 ? (--m_failAttaches, ECUIO_ERR_OUT_OF_MEMORY)
                           : ecuio_session_set_conditions(session, set, file);
    if (rc != ECUIO_OK) {
        *why = detailOr(rc);
        prepareConditionStaging(static_cast<uint32_t>(staged),
                                m_conditionsGeneration);
        return false;
    }
    m_conditionsGeneration = next;
    m_conditionStates.assign(count, ecuio_condition_state_t{});
    // A condition whose key the new set keeps keeps its last tone: an edit
    // of another doesn't let it sound again within its 5 s
    std::vector<std::pair<QString, uint64_t>> toned;
    for (size_t i = 0; i < m_toneAtUs.size(); ++i) {
        if (m_toneAtUs[i] != NoTone) {
            toned.emplace_back(m_conditionModel->key(static_cast<int>(i)),
                               m_toneAtUs[i]);
        }
    }
    m_conditionModel->setConditions(set, signalSet, file);
    m_toneAtUs.assign(count, NoTone);
    for (const auto& [key, at] : toned) {
        const int row = m_conditionModel->rowOfKey(key);
        if (row >= 0 && static_cast<size_t>(row) < m_toneAtUs.size()) {
            m_toneAtUs[static_cast<size_t>(row)] = at;
        }
    }
    if (m_conditionModel->sounds()) {
        m_tone.prepare();
    }
    judgeAlertLevels();
    return true;
}

void SessionBridge::attachSessionConditions(ecuio_session_t* session,
                                            const QString& uri)
{
    m_conditionsGeneration = 0;
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    prepareConditionStaging(0, 0);
    // ADR-0063 decision 12: a replay is judged by its recording's own copy
    QString unused;
    const ConditionSetPtr own = recordingConditions(uri, &unused);
    QString why;
    if (own != nullptr) {
        if (attachConditions(session, own.get(), &why)) {
            m_conditionsFromRecording = true;
            m_conditionsLines
                << QStringLiteral("Alerts: judging the recording's own %1")
                       .arg(conditionCountText(
                           ecuio_condition_set_count(own.get())));
            emit conditionsChanged();
            return;
        }
        unused = QStringLiteral("isn't judged (%1)").arg(why);
    }
    const ecuio_condition_set_t* saved = m_conditionStore.set();
    if (!unused.isEmpty()) {
        const QString judged = saved != nullptr
                                   ? QStringLiteral("the saved conditions")
                                   : QStringLiteral("none");
        m_conditionsLines << QStringLiteral(
                                 "Replay: the recording's conditions.json %1; "
                                 "judging %2")
                                 .arg(unused, judged);
    }
    if (saved == nullptr) {
        emit conditionsChanged();
        return;
    }
    if (attachConditions(session, saved, &why)) {
        m_conditionsLines << QStringLiteral("Alerts: judging %1")
                                 .arg(conditionCountText(
                                     ecuio_condition_set_count(saved)));
    } else {
        m_conditionsLines << QStringLiteral("Alerts not judged: %1").arg(why);
        showSavedConditions();
    }
    emit conditionsChanged();
}

QString SessionBridge::conditionsWait() const
{
    if (m_session == nullptr || !connected()) {
        return {};
    }
    if (m_conditionsFromRecording) {
        return QStringLiteral("applies when this replay is disconnected");
    }
    if (m_recorder != nullptr) {
        return QStringLiteral("applies when this recording ends");
    }
    return {};
}

void SessionBridge::loadSavedConditions()
{
    m_conditionsProblem.clear();
    if (!m_conditionStore.load(&m_conditionsProblem)) {
        m_conditionsProblem =
            QStringLiteral("The saved conditions weren't read: %1")
                .arg(m_conditionsProblem);
    }
    showSavedConditions();
}

void SessionBridge::reloadSavedConditionsForTesting()
{
    loadSavedConditions();
    emit conditionsChanged();
}

void SessionBridge::showSavedConditions()
{
    m_conditionModel->setConditions(m_conditionStore.set(), nullptr, nullptr);
    judgeAlertLevels();
}

void SessionBridge::applySavedConditions()
{
    if (m_session == nullptr || !connected()) {
        showSavedConditions();
        return;
    }
    QString why;
    const ecuio_condition_set_t* set = m_conditionStore.set();
    const uint32_t count = ecuio_condition_set_count(set);
    if (attachConditions(m_session, set, &why)) {
        emit logMessage(QStringLiteral("Alerts: judging %1")
                            .arg(conditionCountText(count)));
    } else {
        emit logMessage(QStringLiteral("Alerts not judged: %1").arg(why));
    }
}

void SessionBridge::endSessionConditions()
{
    m_conditionsGeneration = 0;
    prepareConditionStaging(0, 0);
    m_conditionStates.clear();
    m_toneAtUs.clear();
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    showSavedConditions();
    emit conditionsChanged();
}

QString SessionBridge::conditionsText() const
{
    return QString::fromUtf8(m_conditionStore.bytes().isEmpty()
                                 ? m_conditionStore.refused()
                                 : m_conditionStore.bytes());
}

QVariantMap SessionBridge::saveConditions(const QString& text)
{
    QVariantMap result = m_conditionStore.save(text.toUtf8());
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("pending"), QString());
        return result;
    }
    m_conditionsProblem.clear();
    const QString wait = conditionsWait();
    const QString saved =
        QStringLiteral("Alerts saved: %1")
            .arg(conditionCountText(static_cast<uint32_t>(
                result.value(QStringLiteral("conditions")).toInt())));
    emit logMessage(wait.isEmpty()
                        ? saved
                        : QStringLiteral("%1; the change %2").arg(saved, wait));
    m_conditionsPending = wait;
    if (wait.isEmpty()) {
        applySavedConditions();
    }
    result.insert(QStringLiteral("pending"), wait);
    emit conditionsChanged();
    return result;
}

QVariantMap SessionBridge::importConditions(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QString words;
    QString text;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        words = QStringLiteral("Couldn't read the file");
    } else {
        const QByteArray bytes = file.read(static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES) + 1);
        if (bytes.size() > static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES)) {
            words = QStringLiteral("The file is over %1 KiB, a condition file's most")
                        .arg(ECUIO_CONDITION_FILE_MAX_BYTES / 1024);
        } else {
            // Without ConvertInitialBom the decoder skips a leading BOM
            QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            text = utf8(bytes);
            if (utf8.hasError()) {
                words = QStringLiteral("The file isn't UTF-8 text");
            }
        }
    }
    if (words.isEmpty()) {
        QVariantMap result = saveConditions(text);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            emit logMessage(QStringLiteral("Alerts not imported: %1")
                                .arg(result.value(QStringLiteral("words")).toString()));
        }
        return result;
    }
    emit logMessage(QStringLiteral("Alerts not imported: %1").arg(words));
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("words"), words},
            {QStringLiteral("pending"), QString()}};
}

QVariantMap SessionBridge::exportConditions(const QUrl& url)
{
    // A file core refused is exported as it is, to be mended elsewhere
    const QByteArray bytes = m_conditionStore.bytes().isEmpty() ? m_conditionStore.refused()
                                                                : m_conditionStore.bytes();
    const bool removable = pickedDocumentRemovable(url);
    QString words;
    bool written = false;
    if (bytes.isEmpty()) {
        words = QStringLiteral("There are no saved alerts to export");
    } else {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(written ? QStringLiteral("Alerts exported to a file (%1 bytes)")
                                  .arg(bytes.size())
                            : QStringLiteral("Alerts not exported: %1").arg(words));
    return {{QStringLiteral("ok"), written}, {QStringLiteral("words"), words}};
}

int SessionBridge::alertLevelOf(int defId) const
{
    return m_alertLevels.value(QString::number(defId)).toInt();
}

void SessionBridge::judgeAlertLevels()
{
    QVariantMap levels;
    for (const auto& [id, level] : m_conditionModel->alertLevels()) {
        levels.insert(QString::number(id), level);
    }
    if (levels != m_alertLevels) {
        m_alertLevels = levels;
        emit alertLevelsChanged();
    }
}

void SessionBridge::flushConditions(ecuio_session_t* session)
{
    if (session == nullptr || m_conditionsGeneration == 0) {
        return;
    }
    std::vector<ConditionSlot>* full = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        full = &m_conditionStaging[m_activeConditions];
        m_activeConditions ^= 1;
    }
    // The worker writes the other one now. Each firing is a line, in the
    // order they fired; several of one condition's in a window, one line
    struct Fired {
        uint64_t at;
        int row;
    };
    std::vector<Fired> fired;
    // One tone a tick, the loudest that may sound. A 5 s starts at the ask,
    // heard or not: one the player drops (stale, a failed generator) too
    const uint64_t now = ecuio_clock_us() + m_toneTimeAddedForTesting;
    uint32_t tone = ECUIO_ALERT_NONE;
    for (size_t i = 0; i < full->size(); ++i) {
        ConditionSlot& slot = (*full)[i];
        if (!slot.has) {
            continue;
        }
        m_conditionModel->applyTerm(static_cast<int>(i), slot.newest.term);
        if (slot.firings == 0) {
            continue;
        }
        fired.push_back({slot.first.timestamp_us, static_cast<int>(i)});
        const uint32_t level = m_conditionModel->toneLevel(static_cast<int>(i));
        if (level != ECUIO_ALERT_NONE && i < m_toneAtUs.size() &&
            (m_toneAtUs[i] == NoTone || now - m_toneAtUs[i] >= ToneGapUs)) {
            m_toneAtUs[i] = now;
            tone = std::max(tone, level);
        }
    }
    if (tone != ECUIO_ALERT_NONE) {
        m_tone.play(tone);
    }
    std::stable_sort(
        fired.begin(), fired.end(),
        [](const Fired& a, const Fired& b) { return a.at < b.at; });
    // A handler of the lines may attach again: they go out once the slots
    // and the model are done with
    QStringList lines;
    for (const Fired& f : fired) {
        const ConditionSlot& slot = (*full)[static_cast<size_t>(f.row)];
        lines << (slot.firings == 1
                      ? m_conditionModel->firingText(f.row, slot.first.value,
                                                     slot.first.term)
                      : m_conditionModel->firingsText(f.row, slot.firings));
    }
    for (ConditionSlot& slot : *full) {
        slot = ConditionSlot{};
    }
    // The states core keeps: UNAVAILABLE and NOT_RECEIVED are the getter's
    // alone, and can change after the attach
    uint32_t n = 0;
    const auto size = static_cast<uint32_t>(m_conditionStates.size());
    if (size > 0 &&
        ecuio_session_get_condition_states(session, m_conditionStates.data(),
                                           size, &n) == ECUIO_OK &&
        n == size &&
        m_conditionStates[0].generation == m_conditionsGeneration) {
        m_conditionModel->applyStates(m_conditionStates.data(), n);
    }
    m_conditionModel->judgeStaleSince(
        [this](uint32_t defId) { return m_signalModel->readAtMs(defId); },
        m_signalModel->wallOffsetMs());
    judgeAlertLevels();
    for (const QString& line : lines) {
        emit logMessage(line);
    }
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

QString SessionBridge::linkState() const
{
    if (!m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("disconnected");
    }
    return linkStateForError(m_linkErrorCode);
}
