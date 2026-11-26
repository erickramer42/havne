// File/layer: app/bridge_connect.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); Manual smoke

// Connect and disconnect: the core-only half, the GUI-thread start and apply,
// the sync and async connects, and the async teardown.

#include "sessionbridge.h"
#include "bridgetext.h"
#include "ecuio/obdb.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <QMetaObject>
#include <QStringList>

#include <algorithm>
#include <utility>
#include <vector>

using namespace bridgetext;

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

    // Here, off the GUI thread: on Linux a take can wait for the buses
    holdAwake(session);
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
    m_signalModel->setSignalSet(
        ecuio_session_signal_set(session),
        fileOfSet(ecuio_session_signal_set(session)),
        haveInfo && (info.flags & ECUIO_SESSION_INFO_PICK_ONLY) != 0);
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
        if (info.backend == ECUIO_BACKEND_REPLAY) {
            logReplayMadeUnder(uri);
        }
        m_channelCount =
            std::min<uint32_t>(info.module_count, ECUIO_MAX_CHANNELS);
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
    m_replaySettling = false;
    if (std::exchange(m_replayFinished, false)) {
        emit replayFinishedChanged();
    }
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
    const ConsentNeed need = m_consent->needFor(uri, m_vehicleLabel);
    holdUriSet(uri);
    ecuio_session_t* session = nullptr;
    err = connectCoreOnly(uri, &session);
    if (err.isEmpty()) {
        m_heldSet = ecuio_session_signal_set(session);
        // Nothing waits for an answer here: with no record it reads only
        for (const QString& line : ConsentFlow::applyFound(session, need).lines) {
            emit logMessage(line);
        }
        err = startSession(session, uri);
    }
    if (!err.isEmpty()) {
        m_heldSet = nullptr;
        releaseTogether(false);
        emit logMessage(QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    // Before the apply's emits, so a handler's disconnect ends it
    m_consent->begin(session, need);
    applyConnectedState(session, uri);
    applyAwake(session);
    releaseTogether(false);
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
    holdUriSet(uri);
    emit connectingChanged();
    emit logMessage(QStringLiteral("Connecting: %1").arg(uri));
    applyFrameLoggingEnv();   // before the connect thread spawns any surrogate
    const ConsentNeed need = m_consent->needFor(uri, m_vehicleLabel);

    m_connectThread = std::thread([this, uri, need]() {
        ecuio_session_t* session = nullptr;
        const QString core_err = connectCoreOnly(uri, &session);
        const ConsentFound found = core_err.isEmpty()
                                       ? ConsentFlow::applyFound(session, need)
                                       : ConsentFound{};
        // Handed over through the member, not the lambda: a completion
        // dropped with `this` would leak a lambda capture, while the
        // destructor can always find the member.
        m_pendingSession.store(session);

        QMetaObject::invokeMethod(this, [this, uri, core_err, need, found]() {
            // Join first: assigning to a joinable std::thread terminates
            if (m_connectThread.joinable()) {
                m_connectThread.join();
            }
            for (const QString& line : found.lines) {
                emit logMessage(line);
            }
            if (core_err.isEmpty() && found.waits) {
                // Unstarted until the answer, still connecting
                m_consent->wait(m_pendingSession.load(), need, found,
                                [this, uri, need]() {
                                    finishConnect(uri, {}, need);
                                });
                return;
            }
            finishConnect(uri, core_err, need);
        }, Qt::QueuedConnection);
    });
}

void SessionBridge::finishConnect(const QString& uri, QString err,
                                  const ConsentNeed& need)
{
    // Clear the flag, then emit: a QML handler can run synchronously
    // inside connectingChanged. connecting clears only once the session
    // is applied, so a handler always sees connecting or connected: a
    // connect it starts is refused, never a second session over this one.
    ecuio_session_t* session = m_pendingSession.exchange(nullptr);
    if (err.isEmpty()) {
        m_heldSet = ecuio_session_signal_set(session);
        err = startSession(session, uri);
    }
    if (err.isEmpty()) {
        // Before the apply's emits, so a handler's disconnect ends it
        m_consent->begin(session, need);
        applyConnectedState(session, uri);
        applyAwake(session);
    } else {
        m_heldSet = nullptr;
    }
    m_connecting = false;
    releaseTogether(false);
    emit connectingChanged();

    if (err.isEmpty()) {
        emit connectResult(uri, {});
    } else {
        emit logMessage(QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        emit connectResult(uri, err);
    }
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
    m_consent->end();
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
    m_replaySettling = false;
    const bool replayWasFinished = std::exchange(m_replayFinished, false);

    emit disconnectingChanged();
    if (!uri.isEmpty()) {
        emit currentUriChanged();
    }
    emit connectedChanged(false);
    if (replayWasFinished) {
        emit replayFinishedChanged();
    }
    emit linkStateChanged();          // "fault"/"quiet" -> "disconnected"
    setLogPaused(false);

    // From here the teardown thread is the session's one caller (contract)
    m_disconnectThread = std::thread([this, session, uri]() {
        m_consent->drain(); // an unlock posted before the end uses session
        // Close joins the worker, whose last act publishes reply timing's
        // final rows (ADR-0042), read here since destroy frees them
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
                endTeardown();
                emit disconnectingChanged();
            },
            Qt::QueuedConnection);
    });
}
