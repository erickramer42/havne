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
    m_consent = new ConsentFlow(&m_pluginStore, this);
    m_consent->setLog([this](const QString& line) { emit logMessage(line); });
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
        m_consent->poll(m_channelCount);
        pollSleep();
        // Fires even when nothing arrived this window: dimming exists for
        // rows that stopped updating. 100 ms is well inside the 3 s dim
        // threshold, and the emission is one signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        flushValues();
        pollReplayEnd();  // after the flush: the settle reads the model
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
    // A give or an unlock under way uses the session the lines below free
    m_consent->join();
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
    m_pluginStore.close();
    releaseTogether(true);
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

void BridgeState::ValueStaging::prepare(size_t defs, size_t samplesMax,
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
void BridgeState::ValueStaging::stage(uint32_t def,
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

void BridgeState::ValueStaging::reset()
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
    if (m_replaySettling && m_session != nullptr) {
        m_replaySettling = false;
        m_replayFinished = true;
        emit replayFinishedChanged();
        if (m_signalModel->rowCount() > 0 && !m_signalModel->anyValue()) {
            emit logMessage(QStringLiteral(
                "Replay: none of the profile's signals is in this recording"));
        }
        return;
    }
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
    m_replaySettling = true;
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
QString BridgeFormats::linkStateForError(int32_t errorCode)
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
