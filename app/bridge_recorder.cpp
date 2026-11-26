// File/layer: app/bridge_recorder.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "carryover.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>

using namespace bridgetext;

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

namespace {

QString noRoomLine(const QStringList& names)
{
    return names.isEmpty()
               ? QString()
               : QStringLiteral("Logging %1 the adapter has no room for: %2: "
                                "their columns stay empty")
                     .arg(signalCountText(static_cast<int>(names.size())),
                          names.join(QStringLiteral(", ")));
}

} // namespace

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
    const QString noRoom = noRoomLine(m_signalModel->noRoomNames());
    if (!noRoom.isEmpty()) {
        emit logMessage(noRoom);
    }
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
    // on one that ranks none, nothing on a pick-only one); one torn down
    // took its focus with it
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
